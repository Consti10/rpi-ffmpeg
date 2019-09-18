#include "config.h"

#include "libavcodec/avcodec.h"
#include "rpi_mem.h"
#include "rpi_mailbox.h"
#include "rpi_zc.h"
#include "libavutil/avassert.h"
#include <pthread.h>

#include "libavutil/buffer_internal.h"

#pragma GCC diagnostic push
// Many many redundant decls in the header files
#pragma GCC diagnostic ignored "-Wredundant-decls"
#include <interface/vctypes/vc_image_types.h>
#include <interface/vcsm/user-vcsm.h>
#pragma GCC diagnostic pop

#define TRACE_ALLOC 0
#define DEBUG_ALWAYS_KEEP_LOCKED 0

struct ZcPoolEnt;

typedef struct ZcPool
{
    size_t numbytes;
    int keep_locked;
    unsigned int n;
    struct ZcPoolEnt * head;
    pthread_mutex_t lock;
} ZcPool;

typedef struct ZcPoolEnt
{
    size_t numbytes;

    unsigned int vcsm_handle;
    unsigned int vc_handle;
    void * map_arm;
    unsigned int map_vc;

    unsigned int n;
    struct ZcPoolEnt * next;
    struct ZcPool * pool;
} ZcPoolEnt;

typedef struct ZcOldCtxVals
{
    int thread_safe_callbacks;
    int (*get_buffer2)(struct AVCodecContext *s, AVFrame *frame, int flags);
    void * get_buffer_context;
} ZcOldCtxVals;

typedef struct AVZcEnv
{
    unsigned int refcount;
    ZcOldCtxVals old;

    void * pool_env;
    av_rpi_zc_alloc_buf_fn_t * alloc_buf;
    av_rpi_zc_free_pool_fn_t * free_pool;

} ZcEnv;

typedef struct ZcBufEnv {
    GPU_MEM_PTR_T gmem;
    void * v;
    const av_rpi_zc_buf_fn_tab_t * fn;
    AVZcEnvPtr zc;
} ZcBufEnv;


static void rpi_free_zc_buf(void * opaque, uint8_t * data);




#define ALLOC_PAD       0
#define ALLOC_ROUND     0x1000
#define STRIDE_ROUND    64
#define STRIDE_OR       0

#define DEBUG_ZAP0_BUFFERS 0

static inline int av_rpi_is_sand_format(const int format)
{
    return (format >= AV_PIX_FMT_SAND128 && format <= AV_PIX_FMT_SAND64_16) ||
        (format == AV_PIX_FMT_RPI4_8 || format == AV_PIX_FMT_RPI4_10);
}

static inline int av_rpi_is_sand_frame(const AVFrame * const frame)
{
    return av_rpi_is_sand_format(frame->format);
}

static inline ZcBufEnv * pic_zbe_ptr(AVBufferRef *const buf)
{
    // Kludge where we check the free fn to check this is really
    // one of our buffers - can't think of a better way
    return buf == NULL || buf->buffer->free != rpi_free_zc_buf ? NULL :
        av_buffer_get_opaque(buf);
}

static inline GPU_MEM_PTR_T * pic_gm_ptr(AVBufferRef * const buf)
{
    // As gmem is the first el NULL should be preserved
    return &pic_zbe_ptr(buf)->gmem;
}

//----------------------------------------------------------------------------
//
// Internal pool stuff

static ZcPoolEnt * zc_pool_ent_alloc(ZcPool * const pool, const unsigned int req_size)
{
    ZcPoolEnt * const zp = av_mallocz(sizeof(ZcPoolEnt));

    // Round up to 4k & add 4k
    const unsigned int alloc_size = (req_size + ALLOC_PAD + ALLOC_ROUND - 1) & ~(ALLOC_ROUND - 1);

    if (zp == NULL) {
        av_log(NULL, AV_LOG_ERROR, "av_malloc(ZcPoolEnt) failed\n");
        goto fail0;
    }

    // The 0x80 here maps all pages here rather than waiting for lazy mapping
    // BEWARE that in GPU land a later unlock/lock pair will put us back into
    // lazy mode - which will also break cache invalidate calls.
    if ((zp->vcsm_handle = vcsm_malloc_cache(alloc_size, VCSM_CACHE_TYPE_HOST | 0x80, "ffmpeg_rpi_zc")) == 0)
    {
        av_log(NULL, AV_LOG_ERROR, "av_gpu_malloc_cached(%d) failed\n", alloc_size);
        goto fail1;
    }
#if 0
    // If GPU alloced then lock here and now to prevent slow mapping
    // ops when we assign to a buf
    if (pool->keep_locked)
    {
        if (vcsm_lock(zp->vcsm_handle) == NULL)
        {
            av_log(NULL, AV_LOG_ERROR, "vcsm_lock failed\n");
            vcsm_free(zp->vcsm_handle);
            goto fail1;
        }
    }
#endif

#if TRACE_ALLOC
    printf("%s: Alloc %#x bytes @ h=%d\n", __func__, alloc_size, zp->vcsm_handle);
#endif

    pool->numbytes = alloc_size;
    zp->numbytes = alloc_size;
    zp->pool = pool;
    zp->n = pool->n++;
    return zp;

fail1:
    av_free(zp);
fail0:
    return NULL;
}

static void zc_pool_ent_free(ZcPoolEnt * const zp)
{
#if TRACE_ALLOC
    printf("%s: Free %#x bytes @ h=%d\n", __func__, zp->numbytes, zp->vcsm_handle);
#endif

    if (zp->vcsm_handle != 0)
    {
        // VC addr & handle need no dealloc
        if (zp->map_arm != NULL)
            vcsm_unlock_hdl(zp->vcsm_handle);
        vcsm_free(zp->vcsm_handle);
    }
    av_free(zp);
}

static void zc_pool_flush(ZcPool * const pool)
{
    ZcPoolEnt * p = pool->head;
    pool->head = NULL;
    pool->numbytes = ~0U;

    while (p != NULL)
    {
        ZcPoolEnt * const zp = p;
        p = p->next;
        zc_pool_ent_free(zp);
    }
}

static ZcPoolEnt * zc_pool_alloc(ZcPool * const pool, const int req_bytes)
{
    ZcPoolEnt * zp;
    int numbytes;

    pthread_mutex_lock(&pool->lock);

    numbytes = pool->numbytes;

    // If size isn't close then dump the pool
    // Close in this context means within 128k
    if (req_bytes > numbytes || req_bytes + 0x20000 < numbytes)
    {
        zc_pool_flush(pool);
        numbytes = req_bytes;
    }

    if (pool->head != NULL)
    {
        zp = pool->head;
        pool->head = zp->next;
    }
    else
    {
        zp = zc_pool_ent_alloc(pool, numbytes);
    }

    pthread_mutex_unlock(&pool->lock);

    // Start with our buffer empty of preconceptions
//    rpi_cache_flush_one_gm_ptr(&zp->gmem, RPI_CACHE_FLUSH_MODE_INVALIDATE);

    return zp;
}

static void zc_pool_free(ZcPoolEnt * const zp)
{
    ZcPool * const pool = zp == NULL ? NULL : zp->pool;
    if (zp != NULL)
    {
        pthread_mutex_lock(&pool->lock);
#if TRACE_ALLOC
        printf("%s: Recycle %#x, %#x\n", __func__, pool->numbytes, zp->numbytes);
#endif

        if (pool->numbytes == zp->numbytes)
        {
            zp->next = pool->head;
            pool->head = zp;
            pthread_mutex_unlock(&pool->lock);
        }
        else
        {
            pthread_mutex_unlock(&pool->lock);
            zc_pool_ent_free(zp);
        }
    }
}

static ZcPool *
zc_pool_new(const int keep_locked)
{
    ZcPool * const pool = av_mallocz(sizeof(*pool));
    if (pool == NULL)
        return NULL;

    pool->numbytes = -1;
    pool->keep_locked = keep_locked;
    pool->head = NULL;
    pthread_mutex_init(&pool->lock, NULL);
    return pool;
}

static void
zc_pool_delete(ZcPool * const pool)
{
    if (pool != NULL)
    {
        pool->numbytes = -1;
        zc_pool_flush(pool);
        pthread_mutex_destroy(&pool->lock);
        av_free(pool);
    }
}

//============================================================================
//
// ZC implementation using above pool implementation
//
// Fn table fns...

static void zc_pool_free_v(void * v)
{
    zc_pool_free(v);
}

static unsigned int zc_pool_ent_vcsm_handle_v(void * v)
{
    ZcPoolEnt * zp = v;
    return zp->vcsm_handle;
}

static unsigned int zc_pool_ent_vc_handle_v(void * v)
{
    ZcPoolEnt * zp = v;
    if (zp->vc_handle == 0)
        zp->vc_handle = vcsm_vc_hdl_from_hdl(zp->vcsm_handle);
    return zp->vc_handle;
}

static void * zc_pool_ent_map_arm_v(void * v)
{
    ZcPoolEnt * zp = v;
    if (zp->map_arm == NULL)
        zp->map_arm = vcsm_lock(zp->vcsm_handle);
    return zp->map_arm;
}

static unsigned int zc_pool_ent_map_vc_v(void * v)
{
    ZcPoolEnt * zp = v;
    if (zp->map_vc == 0)
        zp->map_vc = vcsm_vc_addr_from_hdl(zp->vcsm_handle);
    return zp->map_vc;
}

static const av_rpi_zc_buf_fn_tab_t zc_pool_buf_fns = {
    .free        = zc_pool_free_v,
    .vcsm_handle = zc_pool_ent_vcsm_handle_v,
    .vc_handle   = zc_pool_ent_vc_handle_v,
    .map_arm     = zc_pool_ent_map_arm_v,
    .map_vc      = zc_pool_ent_map_vc_v,
};

// ZC Env fns

// Delete pool
// All buffers guaranteed freed by now
static void
zc_pool_delete_v(void * v)
{
    zc_pool_delete((ZcPool *)v);
    rpi_mem_gpu_uninit();
}

// Allocate a new ZC buffer
static AVBufferRef *
zc_pool_buf_alloc(void * v, size_t size, const AVRpiZcFrameGeometry * geo)
{
    ZcPool * const pool = v;
    ZcPoolEnt *const zp = zc_pool_alloc(pool, size);
    AVBufferRef * buf;

    (void)geo;  // geo ignored here

    if (zp == NULL) {
        av_log(NULL, AV_LOG_ERROR, "zc_pool_alloc(%d) failed\n", size);
        goto fail0;
    }

    if ((buf = av_rpi_zc_buf(size, 0, zp, &zc_pool_buf_fns)) == NULL)
    {
        av_log(NULL, AV_LOG_ERROR, "av_rpi_zc_buf() failed\n");
        goto fail2;
    }

    return buf;

fail2:
    zc_pool_free(zp);
fail0:
    return NULL;
}

// Init wrappers - the public fns

AVZcEnvPtr
av_rpi_zc_int_env_alloc(void * logctx)
{
    ZcEnv * zc;
    ZcPool * pool_env;
    int gpu_type;

    if ((gpu_type = rpi_mem_gpu_init(0)) < 0)
        return NULL;

    if ((pool_env = zc_pool_new(gpu_type != GPU_INIT_CMA || DEBUG_ALWAYS_KEEP_LOCKED)) == NULL)
        goto fail1;

    if ((zc = av_rpi_zc_env_alloc(logctx, pool_env, zc_pool_buf_alloc, zc_pool_delete_v)) == NULL)
        goto fail2;

    return zc;

fail2:
    zc_pool_delete(pool_env);
fail1:
    rpi_mem_gpu_uninit();
    return NULL;
}

void
av_rpi_zc_int_env_free(AVZcEnvPtr zc)
{
    av_rpi_zc_env_release(zc);
}

//============================================================================
//
// Geometry
//
// This is a separate chunck to the rest

// Get mailbox fd - should be in a lock when called
// Rely on process close to close it
static int mbox_fd(void)
{
    static int fd = -1;
    if (fd != -1)
        return fd;
    return (fd = mbox_open());
}

AVRpiZcFrameGeometry av_rpi_zc_frame_geometry(
    const int format, const unsigned int video_width, const unsigned int video_height)
{
    static pthread_mutex_t sand_lock = PTHREAD_MUTEX_INITIALIZER;

    AVRpiZcFrameGeometry geo = {
        .format       = format,
        .video_width  = video_width,
        .video_height = video_height
    };

    switch (format)
    {
        case AV_PIX_FMT_YUV420P:
            geo.stride_y = ((video_width + 32 + STRIDE_ROUND - 1) & ~(STRIDE_ROUND - 1)) | STRIDE_OR;
            geo.stride_c = geo.stride_y / 2;
            geo.height_y = (video_height + 32 + 31) & ~31;
            geo.height_c = geo.height_y / 2;
            geo.planes_c = 2;
            geo.stripes = 1;
            geo.bytes_per_pel = 1;
            geo.stripe_is_yc = 1;
            break;

        case AV_PIX_FMT_YUV420P10:
            geo.stride_y = ((video_width * 2 + 64 + STRIDE_ROUND - 1) & ~(STRIDE_ROUND - 1)) | STRIDE_OR;
            geo.stride_c = geo.stride_y / 2;
            geo.height_y = (video_height + 32 + 31) & ~31;
            geo.height_c = geo.height_y / 2;
            geo.planes_c = 2;
            geo.stripes = 1;
            geo.bytes_per_pel = 2;
            geo.stripe_is_yc = 1;
            break;

        case AV_PIX_FMT_SAND128:
        case AV_PIX_FMT_RPI4_8:
        {
            const unsigned int stripe_w = 128;

            static VC_IMAGE_T img = {0};

            // Given the overhead of calling the mailbox keep a stashed
            // copy as we will almost certainly just want the same numbers again
            // but that means we need a lock
            pthread_mutex_lock(&sand_lock);

            if (img.width != video_width || img.height != video_height)
            {
                VC_IMAGE_T new_img = {
                    .type = VC_IMAGE_YUV_UV,
                    .width = video_width,
                    .height = video_height
                };

                mbox_get_image_params(mbox_fd(), &new_img);
                img = new_img;
            }

            geo.stride_y = stripe_w;
            geo.stride_c = stripe_w;
            geo.height_y = ((intptr_t)img.extra.uv.u - (intptr_t)img.image_data) / stripe_w;
            geo.height_c = img.pitch / stripe_w - geo.height_y;
            geo.stripe_is_yc = 1;
            if (geo.height_y * stripe_w > img.pitch)
            {
                // "tall" sand - all C blocks now follow Y
                geo.height_y = img.pitch / stripe_w;
                geo.height_c = geo.height_y;
                geo.stripe_is_yc = 0;
            }
            geo.planes_c = 1;
            geo.stripes = (video_width + stripe_w - 1) / stripe_w;
            geo.bytes_per_pel = 1;

            pthread_mutex_unlock(&sand_lock);
#if 0
            printf("Req: %dx%d: stride=%d/%d, height=%d/%d, stripes=%d, img.pitch=%d\n",
                   video_width, video_height,
                   geo.stride_y, geo.stride_c,
                   geo.height_y, geo.height_c,
                   geo.stripes, img.pitch);
#endif
            av_assert0((int)geo.height_y > 0 && (int)geo.height_c > 0);
            av_assert0(geo.height_y >= video_height && geo.height_c >= video_height / 2);
            break;
        }

        case AV_PIX_FMT_RPI4_10:
        {
            const unsigned int stripe_w = 128;  // bytes

            static pthread_mutex_t sand_lock = PTHREAD_MUTEX_INITIALIZER;
            static VC_IMAGE_T img = {0};

            // Given the overhead of calling the mailbox keep a stashed
            // copy as we will almost certainly just want the same numbers again
            // but that means we need a lock
            pthread_mutex_lock(&sand_lock);

            if (img.width != video_width || img.height != video_height)
            {
                VC_IMAGE_T new_img = {
                    .type = VC_IMAGE_YUV10COL,
                    .width = video_width,
                    .height = video_height
                };

                mbox_get_image_params(mbox_fd(), &new_img);
                img = new_img;
            }

            geo.stride_y = stripe_w;
            geo.stride_c = stripe_w;
            geo.height_y = ((intptr_t)img.extra.uv.u - (intptr_t)img.image_data) / stripe_w;
            geo.height_c = img.pitch / stripe_w - geo.height_y;
            geo.planes_c = 1;
            geo.stripes = ((video_width * 4 + 2) / 3 + stripe_w - 1) / stripe_w;
            geo.bytes_per_pel = 1;
            geo.stripe_is_yc = 1;

            pthread_mutex_unlock(&sand_lock);

            av_assert0((int)geo.height_y > 0 && (int)geo.height_c > 0);
            av_assert0(geo.height_y >= video_height && geo.height_c >= video_height / 2);
            break;
        }

        case AV_PIX_FMT_SAND64_16:
        case AV_PIX_FMT_SAND64_10:
        {
            const unsigned int stripe_w = 128;  // bytes

            static pthread_mutex_t sand_lock = PTHREAD_MUTEX_INITIALIZER;
            static VC_IMAGE_T img = {0};

            // Given the overhead of calling the mailbox keep a stashed
            // copy as we will almost certainly just want the same numbers again
            // but that means we need a lock
            pthread_mutex_lock(&sand_lock);

             if (img.width != video_width || img.height != video_height)
            {
                VC_IMAGE_T new_img = {
                    .type = VC_IMAGE_YUV_UV_16,
                    .width = video_width,
                    .height = video_height
                };

                mbox_get_image_params(mbox_fd(), &new_img);
                img = new_img;
            }

            geo.stride_y = stripe_w;
            geo.stride_c = stripe_w;
            geo.height_y = ((intptr_t)img.extra.uv.u - (intptr_t)img.image_data) / stripe_w;
            geo.height_c = img.pitch / stripe_w - geo.height_y;
            geo.planes_c = 1;
            geo.stripes = (video_width * 2 + stripe_w - 1) / stripe_w;
            geo.bytes_per_pel = 2;
            geo.stripe_is_yc = 1;

            pthread_mutex_unlock(&sand_lock);
            break;
        }

        default:
            break;
    }
    return geo;
}

//============================================================================
//
// ZC Env fns
//
// Frame copy fns

static AVBufferRef * zc_copy(const AVZcEnvPtr zc,
    const AVFrame * const src)
{
    AVFrame dest_frame;
    AVFrame * const dest = &dest_frame;
    unsigned int i;
    uint8_t * psrc, * pdest;

    dest->format = src->format;
    dest->width = src->width;
    dest->height = src->height;

    if (av_rpi_zc_get_buffer(zc, dest) != 0)
    {
        return NULL;
    }

    for (i = 0, psrc = src->data[0], pdest = dest->data[0];
         i != dest->height;
         ++i, psrc += src->linesize[0], pdest += dest->linesize[0])
    {
        memcpy(pdest, psrc, dest->width);
    }
    for (i = 0, psrc = src->data[1], pdest = dest->data[1];
         i != dest->height / 2;
         ++i, psrc += src->linesize[1], pdest += dest->linesize[1])
    {
        memcpy(pdest, psrc, dest->width / 2);
    }
    for (i = 0, psrc = src->data[2], pdest = dest->data[2];
         i != dest->height / 2;
         ++i, psrc += src->linesize[2], pdest += dest->linesize[2])
    {
        memcpy(pdest, psrc, dest->width / 2);
    }

    return dest->buf[0];
}


static AVBufferRef * zc_420p10_to_sand128(const AVZcEnvPtr zc,
    const AVFrame * const src)
{
    assert(0);
    return NULL;
}


static AVBufferRef * zc_sand64_16_to_sand128(const AVZcEnvPtr zc,
    const AVFrame * const src, const unsigned int src_bits)
{
    assert(0);
    return NULL;
}

//----------------------------------------------------------------------------
//
// Public info extraction calls

int av_rpi_zc_vc_handle(const AVRpiZcRefPtr fr_ref)
{
    const GPU_MEM_PTR_T * const p = pic_gm_ptr(fr_ref);
    return p == NULL ? -1 : p->vc_handle;
}

int av_rpi_zc_offset(const AVRpiZcRefPtr fr_ref)
{
    const GPU_MEM_PTR_T * const p = pic_gm_ptr(fr_ref);
    return p == NULL ? 0 : fr_ref->data - p->arm;
}

int av_rpi_zc_length(const AVRpiZcRefPtr fr_ref)
{
    return fr_ref == NULL ? 0 : fr_ref->size;
}

int av_rpi_zc_numbytes(const AVRpiZcRefPtr fr_ref)
{
    const GPU_MEM_PTR_T * const p = pic_gm_ptr(fr_ref);
    return p == NULL ? 0 : p->numbytes;
}

AVRpiZcRefPtr av_rpi_zc_ref(void * const logctx, const AVZcEnvPtr zc,
    const AVFrame * const frame, const enum AVPixelFormat expected_format, const int maycopy)
{
    av_assert0(!maycopy || zc != NULL);

    if (frame->format != AV_PIX_FMT_YUV420P &&
        frame->format != AV_PIX_FMT_YUV420P10 &&
        !av_rpi_is_sand_frame(frame))
    {
        av_log(logctx, AV_LOG_WARNING, "%s: *** Format not SAND/YUV420P: %d\n", __func__, frame->format);
        return NULL;
    }

    if (frame->buf[1] != NULL || frame->format != expected_format)
    {
#if RPI_ZC_SAND_8_IN_10_BUF
        if (frame->format == AV_PIX_FMT_SAND64_10 && expected_format == AV_PIX_FMT_SAND128 && frame->buf[RPI_ZC_SAND_8_IN_10_BUF] != NULL)
        {
//            av_log(s, AV_LOG_INFO, "%s: --- found buf[4]\n", __func__);
            return av_buffer_ref(frame->buf[RPI_ZC_SAND_8_IN_10_BUF]);
        }
#endif

        if (maycopy)
        {
            if (frame->buf[1] != NULL)
                av_log(logctx, AV_LOG_INFO, "%s: *** Not a single buf frame: copying\n", __func__);
            else
                av_log(logctx, AV_LOG_INFO, "%s: *** Unexpected frame format %d: copying to %d\n", __func__, frame->format, expected_format);

            switch (frame->format)
            {
                case AV_PIX_FMT_YUV420P10:
                    return zc_420p10_to_sand128(zc, frame);

                case AV_PIX_FMT_SAND64_10:
                    return zc_sand64_16_to_sand128(zc, frame, 10);

                default:
                    return zc_copy(zc, frame);
            }
        }
        else
        {
            if (frame->buf[1] != NULL)
                av_log(logctx, AV_LOG_WARNING, "%s: *** Not a single buf frame: buf[1] != NULL\n", __func__);
            else
                av_log(logctx, AV_LOG_INFO, "%s: *** Unexpected frame format: %d != %d\n", __func__, frame->format, expected_format);
            return NULL;
        }
    }

    if (pic_gm_ptr(frame->buf[0]) == NULL)
    {
        if (maycopy)
        {
            av_log(logctx, AV_LOG_INFO, "%s: *** Not one of our buffers: copying\n", __func__);
            return zc_copy(zc, frame);
        }
        else
        {
            av_log(logctx, AV_LOG_WARNING, "%s: *** Not one of our buffers: NULL\n", __func__);
            return NULL;
        }
    }

    return av_buffer_ref(frame->buf[0]);
}

void av_rpi_zc_unref(AVRpiZcRefPtr fr_ref)
{
    if (fr_ref != NULL)
    {
        av_buffer_unref(&fr_ref);
    }
}

//----------------------------------------------------------------------------

// Extract user environment from an AVBufferRef
void * av_rpi_zc_buf_v(AVBufferRef * const buf)
{
    ZcBufEnv * const zbe = pic_zbe_ptr(buf);
    return zbe == NULL ? NULL : zbe->v;
}

// AV buffer pre-free callback
static void rpi_free_zc_buf(void * opaque, uint8_t * data)
{
    if (opaque != NULL)
    {
        ZcBufEnv * const zbe = opaque;

        if (zbe->fn->free)
            zbe->fn->free(zbe->v);

        if (zbe->zc != NULL)
            av_rpi_zc_env_release(zbe->zc);

        av_free(zbe);
    }
}

// Wrap the various ZC bits in an AV Buffer and resolve those things we want
// resolved now.
// Currently we resolve everything, but in future we might not
AVBufferRef * av_rpi_zc_buf(size_t numbytes, int addr_offset, void * v, const av_rpi_zc_buf_fn_tab_t * fn_tab)
{
    AVBufferRef *buf;
    ZcBufEnv * zbe;
    uint8_t * arm_ptr;

    if ((zbe = calloc(1, sizeof(ZcBufEnv))) == NULL)
        return NULL;

    zbe->fn = fn_tab;
    zbe->v = v;
    zbe->gmem.numbytes = numbytes;

    if ((zbe->gmem.vcsm_handle = zbe->fn->vcsm_handle(zbe->v)) == 0)
    {
        av_log(NULL, AV_LOG_ERROR, "ZC: Failed to get vcsm_handle\n");
        goto fail;
    }

    // For now init all this here
    // Later we may avoid some parts (in particular arm addr lock) unless
    // actually wanted
    if ((arm_ptr = zbe->fn->map_arm(zbe->v)) == NULL)
    {
        av_log(NULL, AV_LOG_ERROR, "ZC: Failed to lock vcsm_handle %u\n", zbe->gmem.vcsm_handle);
        goto fail;
    }

    zbe->gmem.arm = arm_ptr + addr_offset;
    if ((zbe->gmem.vc_handle = zbe->fn->vc_handle(zbe->v)) == 0)
    {
        av_log(NULL, AV_LOG_ERROR, "ZC: Failed to get vc handle from vcsm_handle %u\n", zbe->gmem.vcsm_handle);
        goto fail;
    }
    if ((zbe->gmem.vc = zbe->fn->map_vc(zbe->v)) == 0)
    {
        av_log(NULL, AV_LOG_ERROR, "ZC: Failed to get vc addr from vcsm_handle %u\n", zbe->gmem.vcsm_handle);
        goto fail;
    }

    if ((buf = av_buffer_create(zbe->gmem.arm, zbe->gmem.numbytes, rpi_free_zc_buf, zbe, 0)) == NULL)
    {
        av_log(NULL, AV_LOG_ERROR, "ZC: Failed av_buffer_create\n");
        goto fail;
    }

    return buf;

fail:
    rpi_free_zc_buf(zbe, NULL);
    return NULL;
}

int av_rpi_zc_get_buffer(ZcEnv *const zc, AVFrame * const frame)
{
    const AVRpiZcFrameGeometry geo = av_rpi_zc_frame_geometry(frame->format, frame->width, frame->height);
    const unsigned int size_y = geo.stride_y * geo.height_y;
    const unsigned int size_c = geo.stride_c * geo.height_c;
    const unsigned int size_pic = (size_y + size_c * geo.planes_c) * geo.stripes;
    AVBufferRef * buf;
    unsigned int i;

//    printf("Do local alloc: format=%#x, %dx%d: %u\n", frame->format, frame->width, frame->height, size_pic);

    if ((buf = zc->alloc_buf(zc->pool_env, size_pic, &geo)) == NULL)
    {
        av_log(NULL, AV_LOG_ERROR, "rpi_get_display_buffer: Failed to get buffer from pool\n");
        return AVERROR(ENOMEM);
    }

    // Track
    atomic_fetch_add(&zc->refcount, 1);
    pic_zbe_ptr(buf)->zc = zc;

    for (i = 0; i < AV_NUM_DATA_POINTERS; i++) {
        frame->buf[i] = NULL;
        frame->data[i] = NULL;
        frame->linesize[i] = 0;
    }

    frame->buf[0] = buf;

    frame->linesize[0] = geo.stride_y;
    frame->linesize[1] = geo.stride_c;
    frame->linesize[2] = geo.stride_c;
    // abuse: linesize[3] = "stripe stride"
    // stripe_stride is NOT the stride between slices it is (that / geo.stride_y).
    // In a general case this makes the calculation an xor and multiply rather
    // than a divide and multiply
    if (geo.stripes > 1)
        frame->linesize[3] = geo.stripe_is_yc ? geo.height_y + geo.height_c : geo.height_y;

    frame->data[0] = buf->data;
    frame->data[1] = frame->data[0] + (geo.stripe_is_yc ? size_y : size_y * geo.stripes);
    if (geo.planes_c > 1)
        frame->data[2] = frame->data[1] + size_c;

    frame->extended_data = frame->data;
    // Leave extended buf alone

#if RPI_ZC_SAND_8_IN_10_BUF != 0
    // *** If we intend to use this for real we will want a 2nd buffer pool
    frame->buf[RPI_ZC_SAND_8_IN_10_BUF] = zc_pool_buf_alloc(&zc->pool, size_pic);  // *** 2 * wanted size - kludge
#endif

    return 0;
}

void av_rpi_zc_env_release(const AVZcEnvPtr zc)
{
    const int n = atomic_fetch_add(&zc->refcount, -1);
    if (n == 1)  // was 1, now 0
    {
        zc->free_pool(zc->pool_env);
        av_free(zc);
    }
}

AVZcEnvPtr av_rpi_zc_env_alloc(void * logctx,
                    void * pool_env,
                    av_rpi_zc_alloc_buf_fn_t * alloc_buf_fn,
                    av_rpi_zc_free_pool_fn_t * free_pool_fn)
{
    ZcEnv * zc;

	if ((zc = av_mallocz(sizeof(ZcEnv))) == NULL)
    {
        av_log(logctx, AV_LOG_ERROR, "av_rpi_zc_env_alloc: Context allocation failed\n");
        return NULL;
    }

    *zc = (ZcEnv){
        .refcount = ATOMIC_VAR_INIT(1),
        .pool_env = pool_env,
        .alloc_buf = alloc_buf_fn,
        .free_pool = free_pool_fn
    };

    return zc;
}

//============================================================================
//
// External ZC initialisation

#define RPI_GET_BUFFER2 1


static int zc_get_buffer2(struct AVCodecContext *s, AVFrame *frame, int flags)
{
#if !RPI_GET_BUFFER2
    return avcodec_default_get_buffer2(s, frame, flags);
#else
    int rv;

    if ((s->codec->capabilities & AV_CODEC_CAP_DR1) == 0)
    {
//        printf("Do default alloc: format=%#x\n", frame->format);
        rv = avcodec_default_get_buffer2(s, frame, flags);
    }
    else if (frame->format == AV_PIX_FMT_YUV420P ||
             av_rpi_is_sand_frame(frame))
    {
        rv = av_rpi_zc_get_buffer(s->get_buffer_context, frame);
    }
    else
    {
        rv = avcodec_default_get_buffer2(s, frame, flags);
    }

#if 0
    printf("%s: fmt:%d, %dx%d lsize=%d/%d/%d/%d data=%p/%p/%p bref=%p/%p/%p opaque[0]=%p\n", __func__,
        frame->format, frame->width, frame->height,
        frame->linesize[0], frame->linesize[1], frame->linesize[2], frame->linesize[3],
        frame->data[0], frame->data[1], frame->data[2],
        frame->buf[0], frame->buf[1], frame->buf[2],
        av_buffer_get_opaque(frame->buf[0]));
#endif
    return rv;
#endif
}

int av_rpi_zc_in_use(const struct AVCodecContext * const s)
{
    return s->get_buffer2 == zc_get_buffer2;
}

int av_rpi_zc_init2(struct AVCodecContext * const s,
                    void * pool_env,
                    av_rpi_zc_alloc_buf_fn_t * alloc_buf_fn,
                    av_rpi_zc_free_pool_fn_t * free_pool_fn)
{
    ZcEnv * zc;

    av_assert0(!av_rpi_zc_in_use(s));

    if ((zc = av_rpi_zc_env_alloc(s, pool_env, alloc_buf_fn, free_pool_fn)) == NULL)
        return AVERROR(ENOMEM);

    zc->old = (ZcOldCtxVals){
        .get_buffer_context = s->get_buffer_context,
        .get_buffer2 = s->get_buffer2,
        .thread_safe_callbacks = s->thread_safe_callbacks
    };

    s->get_buffer_context = zc;
    s->get_buffer2 = zc_get_buffer2;
    s->thread_safe_callbacks = 1;
    return 0;
}

void av_rpi_zc_uninit2(struct AVCodecContext * const s)
{
    ZcEnv * const zc = s->get_buffer_context;

    av_assert0(av_rpi_zc_in_use(s));

    s->get_buffer2 = zc->old.get_buffer2;
    s->get_buffer_context = zc->old.get_buffer_context;
    s->thread_safe_callbacks = zc->old.thread_safe_callbacks;

    av_rpi_zc_env_release(zc);
}

