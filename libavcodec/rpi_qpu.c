// Use the vcsm device for shared memory
// This works better than the mmap in that the memory can be cached, but requires a kernel modification to enable the device.
#define RPI_USE_VCSM
#define RPI_TIME_TOTAL_QPU
#define RPI_TIME_TOTAL_VPU

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <assert.h>

#include <pthread.h>
#include <time.h>

#include "rpi_mailbox.h"
#include "rpi_qpu.h"
#include "rpi_shader.h"
#include "rpi_hevc_transform.h"

#ifdef RPI_USE_VCSM
#include "rpi_user_vcsm.h"
#endif

// On Pi2 there is no way to access the VPU L2 cache
// GPU_MEM_FLG should be 4 for uncached memory.
// However, if using VCSM allocated buffers, need to use C at the moment because VCSM does not allocate uncached memory correctly
// The QPU crashes if we mix L2 cached and L2 uncached accesses due to a HW bug.
#define GPU_MEM_FLG 0xC
#define GPU_MEM_MAP 0x0

#define vcos_verify(x) ((x)>=0)

typedef unsigned char uint8_t;
typedef signed char int8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef int int32_t;

/*static const unsigned code[] =
{
  #include "rpi_shader.hex"
};*/

// Size in 32bit words
#define QPU_CODE_SIZE 2048
#define VPU_CODE_SIZE 2048

const short rpi_transMatrix2even[32][16] = { // Even rows first
{64,  64,  64,  64,  64,  64,  64,  64,  64,  64,  64,  64,  64,  64,  64,  64},
{90,  87,  80,  70,  57,  43,  25,   9,  -9, -25, -43, -57, -70, -80, -87, -90},
{89,  75,  50,  18, -18, -50, -75, -89, -89, -75, -50, -18,  18,  50,  75,  89},
{87,  57,   9, -43, -80, -90, -70, -25,  25,  70,  90,  80,  43,  -9, -57, -87},
{83,  36, -36, -83, -83, -36,  36,  83,  83,  36, -36, -83, -83, -36,  36,  83},
{80,   9, -70, -87, -25,  57,  90,  43, -43, -90, -57,  25,  87,  70,  -9, -80},
{75, -18, -89, -50,  50,  89,  18, -75, -75,  18,  89,  50, -50, -89, -18,  75},
{70, -43, -87,   9,  90,  25, -80, -57,  57,  80, -25, -90,  -9,  87,  43, -70},
{64, -64, -64,  64,  64, -64, -64,  64,  64, -64, -64,  64,  64, -64, -64,  64},
{57, -80, -25,  90,  -9, -87,  43,  70, -70, -43,  87,   9, -90,  25,  80, -57},
{50, -89,  18,  75, -75, -18,  89, -50, -50,  89, -18, -75,  75,  18, -89,  50},
{43, -90,  57,  25, -87,  70,   9, -80,  80,  -9, -70,  87, -25, -57,  90, -43},
{36, -83,  83, -36, -36,  83, -83,  36,  36, -83,  83, -36, -36,  83, -83,  36},
{25, -70,  90, -80,  43,   9, -57,  87, -87,  57,  -9, -43,  80, -90,  70, -25},
{18, -50,  75, -89,  89, -75,  50, -18, -18,  50, -75,  89, -89,  75, -50,  18},
{ 9, -25,  43, -57,  70, -80,  87, -90,  90, -87,  80, -70,  57, -43,  25,  -9},
// Odd rows
{90,  90,  88,  85,  82,  78,  73,  67,  61,  54,  46,  38,  31,  22,  13,   4},
{90,  82,  67,  46,  22,  -4, -31, -54, -73, -85, -90, -88, -78, -61, -38, -13},
{88,  67,  31, -13, -54, -82, -90, -78, -46,  -4,  38,  73,  90,  85,  61,  22},
{85,  46, -13, -67, -90, -73, -22,  38,  82,  88,  54,  -4, -61, -90, -78, -31},
{82,  22, -54, -90, -61,  13,  78,  85,  31, -46, -90, -67,   4,  73,  88,  38},
{78,  -4, -82, -73,  13,  85,  67, -22, -88, -61,  31,  90,  54, -38, -90, -46},
{73, -31, -90, -22,  78,  67, -38, -90, -13,  82,  61, -46, -88,  -4,  85,  54},
{67, -54, -78,  38,  85, -22, -90,   4,  90,  13, -88, -31,  82,  46, -73, -61},
{61, -73, -46,  82,  31, -88, -13,  90,  -4, -90,  22,  85, -38, -78,  54,  67},
{54, -85,  -4,  88, -46, -61,  82,  13, -90,  38,  67, -78, -22,  90, -31, -73},
{46, -90,  38,  54, -90,  31,  61, -88,  22,  67, -85,  13,  73, -82,   4,  78},
{38, -88,  73,  -4, -67,  90, -46, -31,  85, -78,  13,  61, -90,  54,  22, -82},
{31, -78,  90, -61,   4,  54, -88,  82, -38, -22,  73, -90,  67, -13, -46,  85},
{22, -61,  85, -90,  73, -38,  -4,  46, -78,  90, -82,  54, -13, -31,  67, -88},
{13, -38,  61, -78,  88, -90,  85, -73,  54, -31,   4,  22, -46,  67, -82,  90},
{ 4, -13,  22, -31,  38, -46,  54, -61,  67, -73,  78, -82,  85, -88,  90, -90}
};

struct GPU
{
  unsigned int qpu_code[QPU_CODE_SIZE];
  unsigned int vpu_code[VPU_CODE_SIZE];
  short transMatrix2even[16*16];
  int open_count; // Number of allocated video buffers
  unsigned int vc_handle; // Handle of this memory
  int      mb; // Mailbox handle
  int      vc; // Address in GPU memory
  int mail[12]; // These are used to pass pairs of code/unifs to the QPUs
};

// Stop more than one thread trying to allocate memory or use the processing resources at once
static pthread_mutex_t gpu_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile struct GPU* gpu = NULL;

#ifdef RPI_TIME_TOTAL_QPU
static unsigned int Microseconds(void) {
    struct timespec ts;
    unsigned int x;
    static unsigned int base = 0;
    clock_gettime(CLOCK_REALTIME, &ts);
    x = ts.tv_sec*1000000 + ts.tv_nsec/1000;
    if (base==0) base=x;
    return x-base;
}
#endif

// Connect to QPU, returns 0 on success.
static int gpu_init(volatile struct GPU **gpu) {
  int mb = mbox_open();
  int vc;
  int handle;
  volatile struct GPU* ptr;
	if (mb < 0)
		return -1;

	if (qpu_enable(mb, 1)) return -2;

#ifdef RPI_USE_VCSM
  vcsm_init();
#endif

  handle = mem_alloc(mb, sizeof(struct GPU), 4096, GPU_MEM_FLG);
  if (!handle)
  {
    qpu_enable(mb, 0);
    return -3;
  }
	vc = mem_lock(mb, handle);
	ptr = mapmem_shared((vc+GPU_MEM_MAP)&~0xc0000000, sizeof(struct GPU));
	if (ptr == NULL)
	{	mem_free(mb, handle);
		mem_unlock(mb, handle);
		qpu_enable(mb, 0);
		return -4;
	}

	ptr->mb = mb;
	ptr->vc_handle = handle;
	ptr->vc = vc;

  *gpu = ptr;

  // Now copy over the QPU code into GPU memory
  {
    int num_bytes = qpu_get_fn(QPU_MC_END) - qpu_get_fn(QPU_MC_SETUP);
    assert(num_bytes<=QPU_CODE_SIZE*sizeof(unsigned int));
    memcpy((void*)ptr->qpu_code, rpi_shader, num_bytes);
  }
  // And the VPU code
  {
    int num_bytes = sizeof(rpi_hevc_transform);
    assert(num_bytes<=VPU_CODE_SIZE*sizeof(unsigned int));
    memcpy((void*)ptr->vpu_code, rpi_hevc_transform, num_bytes);
  }
  // And the transform coefficients
  memcpy((void*)ptr->transMatrix2even, rpi_transMatrix2even, 16*16*sizeof(short));

  return 0;
}

// Make sure we have exclusive access to the mailbox, and enable qpu if necessary.
static void gpu_lock(void) {
  pthread_mutex_lock(&gpu_mutex);
  if (gpu==NULL) {
    gpu_init(&gpu);
  }
}

static void gpu_unlock(void) {
  pthread_mutex_unlock(&gpu_mutex);
}

// Allocate memory on GPU
// Fills in structure <p> containing ARM pointer, videocore handle, videocore memory address, numbytes
// Returns 0 on success.
// This allocates memory that will not be cached in ARM's data cache.
// Therefore safe to use without data cache flushing.
int gpu_malloc_uncached(int numbytes, GPU_MEM_PTR_T *p) {
  gpu_lock();
  p->vc_handle = mem_alloc(gpu->mb, numbytes, 4096, GPU_MEM_FLG);
  p->vcsm_handle = 0;
  if (!p->vc_handle)
  {
    qpu_enable(gpu->mb, 0);
    return -3;
  }
  p->vc = mem_lock(gpu->mb, p->vc_handle);
  p->arm = mapmem_shared((p->vc+GPU_MEM_MAP)&~0xc0000000,numbytes);
  p->numbytes = numbytes;
  if (p->arm == NULL)
  {
    mem_free(gpu->mb, p->vc_handle);
    mem_unlock(gpu->mb, p->vc_handle);
    gpu_unlock();
    qpu_enable(gpu->mb, 0);
    return -4;
  }
  gpu->open_count++;
  gpu_unlock();
  return 0;
}

void gpu_cache_flush(GPU_MEM_PTR_T *p)
{
  // This only works when using RPI_USE_VCSM
  void *tmp = vcsm_lock(p->vcsm_handle);
  vcsm_unlock_ptr(tmp);
}

// This allocates data that will be
//    Cached in ARM L2
//    Uncached in VPU L2
int gpu_malloc_cached(int numbytes, GPU_MEM_PTR_T *p) {
  gpu_lock();
#ifdef RPI_USE_VCSM
  {
      p->vcsm_handle = vcsm_malloc_cache(numbytes, VCSM_CACHE_TYPE_HOST, (char *)"Video Frame" ); // f....... locks up for VP9 - retest this?
      //p->vcsm_handle = vcsm_malloc_cache(numbytes, VCSM_CACHE_TYPE_VC, (char *)"Video Frame" ); // 3b...... works
      //p->vcsm_handle = vcsm_malloc_cache(numbytes, VCSM_CACHE_TYPE_NONE, (char *)"Video Frame" ); //fb...... locks up
      //p->vcsm_handle = vcsm_malloc_cache(numbytes, VCSM_CACHE_TYPE_HOST_AND_VC, (char *)"Video Frame" ); // 3b works (but corrupted due to caching)
      p->vc_handle = vcsm_vc_hdl_from_hdl(p->vcsm_handle);
      p->arm = vcsm_lock(p->vcsm_handle);
      p->vc = mem_lock(gpu->mb, p->vc_handle);
  }
#else
  p->vc_handle = mem_alloc(gpu->mb, numbytes, 4096, GPU_MEM_FLG);
  p->vcsm_handle = 0;
  if (!p->handle)
  {
    qpu_enable(gpu->mb, 0);
    return -3;
  }
  p->vc = mem_lock(gpu->mb, p->vc_handle);
  printf("This mapmem_private does not seem to work\n");
  exit(-1);
  p->arm = mapmem_private((p->vc+GPU_MEM_MAP)&~0xc0000000,numbytes);
  p->numbytes = numbytes;
  if (p->arm == NULL)
  {
    mem_free(gpu->mb, p->handle);
    mem_unlock(gpu->mb, p->handle);
    gpu_unlock();
    qpu_enable(gpu->mb, 0);
    return -4;
  }
#endif
  gpu->open_count++;
  gpu_unlock();
  return 0;
}

static void gpu_term(void)
{
	int mb = gpu->mb;
	unsigned handle = gpu->vc_handle;
  if (gpu==NULL)
    return;
	unmapmem((void*)gpu, sizeof(struct GPU));
	mem_unlock(mb, handle);
	mem_free(mb, handle);
	qpu_enable(mb, 0);
#ifdef RPI_USE_VCSM
  vcsm_exit();
#endif
	mbox_close(mb);
  gpu = NULL;
}

void gpu_free(GPU_MEM_PTR_T *p) {
  int mb = gpu->mb;
	unsigned handle = p->vc_handle;
  gpu_lock();
#ifdef RPI_USE_VCSM
  if (p->vcsm_handle) {
      mem_unlock(mb,p->vc_handle);
      vcsm_unlock_ptr(p->arm);
      vcsm_free(p->vcsm_handle);
  } else {
	unmapmem((void*)p->arm, sizeof(struct GPU));
      mem_unlock(mb, handle);
      mem_free(mb, handle);
  }
#else
	unmapmem((void*)p->arm, sizeof(struct GPU));
	mem_unlock(mb, handle);
	mem_free(mb, handle);
#endif

  gpu->open_count--;
  if (gpu->open_count==0) {
      printf("Closing GPU\n");
      gpu_term();
      gpu = NULL;
  }
  gpu_unlock();
}

unsigned int vpu_get_fn(void) {
  // Make sure that the gpu is initialized
  if (gpu==NULL) {
    printf("Preparing gpu\n");
    gpu_lock();
    gpu_unlock();
  }
  return gpu->vc + offsetof(struct GPU,vpu_code);
}

unsigned int vpu_get_constants(void) {
  if (gpu==NULL) {
    gpu_lock();
    gpu_unlock();
  }
  return gpu->vc + offsetof(struct GPU,transMatrix2even);
}

unsigned vpu_execute_code( unsigned code, unsigned r0, unsigned r1, unsigned r2, unsigned r3, unsigned r4, unsigned r5)
{
  unsigned r;
#ifdef RPI_TIME_TOTAL_VPU
  static int last_time=0;
  static long long on_time=0;
  static long long off_time=0;
  int start_time;
  int end_time;
  static int count=0;
  static long long countr2=0;
#endif
  gpu_lock();
#ifdef RPI_TIME_TOTAL_VPU
  start_time = Microseconds();
  if (last_time==0)
    last_time = start_time;
  off_time += start_time-last_time;
#endif
  r = execute_code(gpu->mb, code, r0, r1, r2, r3, r4, r5);
#ifdef RPI_TIME_TOTAL_VPU
  end_time = Microseconds();
  last_time = end_time;
  on_time += end_time - start_time;
  count++;
  countr2 += r2;
  if ((count&0x7f)==0)
    printf("VPU %d %lld On=%dms, Off=%dms\n",count,countr2,(int)(on_time/1000),(int)(off_time/1000));
#endif
  gpu_unlock();
  return r;
}

// Run a program on a QPU with the given code and uniform stream (given in GPU addresses)
// The first num QPUs will start at code, the next num2 QPUs will start at code2
void qpu_run_shader12(int code, int num, int code2, int num2, int unifs1, int unifs2, int unifs3, int unifs4, int unifs5, int unifs6, int unifs7, int unifs8, int unifs9, int unifs10, int unifs11, int unifs12)
{
  int i;
#ifdef RPI_TIME_TOTAL_QPU
  static int last_time=0;
  static long long on_time=0;
  static long long off_time=0;
  int start_time;
  int end_time;
  static int count=0;
#endif

  gpu_lock();
#ifdef RPI_TIME_TOTAL_QPU
  start_time = Microseconds();
  if (last_time==0)
    last_time = start_time;
  off_time += start_time-last_time;
#endif
  for(i=0;i<num;i++) {
    gpu->mail[i*2 + 1] = code;
  }
  for(;i<num+num2;i++) {
    gpu->mail[i*2 + 1] = code2;
  }
  gpu->mail[0 ] = unifs1;
  gpu->mail[2 ] = unifs2;
  gpu->mail[4 ] = unifs3;
  gpu->mail[6 ] = unifs4;
  gpu->mail[8 ] = unifs5;
  gpu->mail[10] = unifs6;
	gpu->mail[12] = unifs7;
	gpu->mail[14] = unifs8;
	gpu->mail[16] = unifs9;
	gpu->mail[18] = unifs10;
	gpu->mail[20] = unifs11;
	gpu->mail[22] = unifs12;
	execute_qpu(
		gpu->mb,
		12 /* Number of QPUs */,
		gpu->vc + offsetof(struct GPU, mail),
		1 /* no flush */,  // Don't flush VPU L1 cache
		5000 /* timeout ms */);
#ifdef RPI_TIME_TOTAL_QPU
  end_time = Microseconds();
  last_time = end_time;
  on_time += end_time - start_time;
  count++;
  if ((count&0x7f)==0)
    printf("On=%dms, Off=%dms\n",(int)(on_time/1000),(int)(off_time/1000));
#endif
  gpu_unlock();
}

unsigned int qpu_get_fn(int num) {
    // Make sure that the gpu is initialized
    unsigned int *fn;
    if (gpu==NULL) {
      printf("Preparing gpu\n");
      gpu_lock();
      gpu_unlock();
    }
    switch(num) {
    case QPU_MC_SETUP:
      fn = mc_setup;
      break;
    case QPU_MC_FILTER:
      fn = mc_filter;
      break;
    case QPU_MC_EXIT:
      fn = mc_exit;
      break;
    case QPU_MC_INTERRUPT_EXIT:
      fn = mc_interrupt_exit;
      break;
    case QPU_MC_FILTER_B:
      fn = mc_filter_b;
      break;
    case QPU_MC_FILTER_HONLY:
      fn = mc_filter_honly;
      break;
    case QPU_MC_SETUP_UV:
      fn = mc_setup_uv;
      break;
    case QPU_MC_FILTER_UV:
      fn = mc_filter_uv;
      break;
    case QPU_MC_FILTER_UV_B:
      fn = mc_filter_uv_b;
      break;
    case QPU_MC_END:
      fn = mc_end;
      break;
    default:
      printf("Unknown function\n");
      exit(-1);
    }
    return gpu->vc + 4*(int)(fn-rpi_shader);
    //return code[num] + gpu->vc;
}

#if 0

int32_t hcoeffs[] = {-4, 10, -21, 70, 90, -24, 11, -4};
//int32_t hcoeffs[] = {1, 1, 1, 1, 1, 1, 1, 1};
int32_t vcoeffs[] = {-2, 6, -13, 37, 115, -20, 9, -4};
//int32_t vcoeffs[] = {1, 1, 1, 1, 1, 1, 1, 1};

#define ENCODE_COEFFS(c0, c1, c2, c3) (((c0-1) & 0xff) | ((c1-1) & 0xff) << 8 | ((c2-1) & 0xff) << 16 | ((c3-1) & 0xff) << 24);

static uint8_t av_clip_uint8(int32_t a)
{
    if (a&(~255)) return (-a)>>31;
    else          return a;
}

static int32_t filter8(const uint8_t *data, int pitch)
{
   int32_t vsum = 0;
   int x, y;

   for (y = 0; y < 8; y++) {
      int32_t hsum = 0;

      for (x = 0; x < 8; x++)
         hsum += hcoeffs[x]*data[x + y * pitch];

      vsum += vcoeffs[y]*av_clip_uint8( (hsum + 64) >> 7); // Added brackets to stop compiler warning
   }

   return av_clip_uint8( (vsum + 64) >> 7);
}

// Note regression changes coefficients so is not thread safe
//#define REGRESSION
#ifdef REGRESSION
#define CMAX 100
#else
#define CMAX 2
#endif
#define YMAX 16

int rpi_test_shader(void)
{
   int i, c;

   uint32_t *unifs;

   uint8_t *in_buffer;
   uint8_t *out_buffer[2];

   GPU_MEM_PTR_T unifs_ptr;
   GPU_MEM_PTR_T in_buffer_ptr;
   GPU_MEM_PTR_T out_buffer_ptr[2];

   // Addresses in GPU memory of filter programs
   uint32_t mc_setup = 0;
   uint32_t mc_filter = 0;
   uint32_t mc_exit = 0;

   int pitch = 0x500;

   if (gpu==NULL) {
      gpu_lock();
      gpu_unlock();
   }

   printf("This needs to change to reflect new assembler\n");
   // Use table to compute locations of program start points
   mc_setup = code[0] + gpu->vc;
   mc_filter = code[1] + gpu->vc;
   mc_exit = code[2] + gpu->vc;

   if (!vcos_verify(gpu_malloc_uncached(4*64,&unifs_ptr))) {
      return -2;
   }
   unifs = (uint32_t*)unifs_ptr.arm;

   if (!vcos_verify(gpu_malloc_uncached(64*23,&in_buffer_ptr))) {
      return -3;
   }
   in_buffer = (uint8_t*)in_buffer_ptr.arm;

   if (!vcos_verify(gpu_malloc_uncached(16*pitch,&out_buffer_ptr[0])) || !vcos_verify(gpu_malloc_uncached(16*pitch,&out_buffer_ptr[1]))) {
      return -4;
   }
   out_buffer[0] = (uint8_t*)out_buffer_ptr[0].arm;
   out_buffer[1] = (uint8_t*)out_buffer_ptr[1].arm;

   for (c = 0; c < CMAX; c++) {
      int xo[] = {rand()&31, rand()&31};

#ifdef REGRESSION
      for (i = 0; i < 8; i++) {
         hcoeffs[i] = (int8_t)rand();
         vcoeffs[i] = (int8_t)rand();
         if (hcoeffs[i]==-128)
           hcoeffs[i]++;
         if (vcoeffs[i]==-128)
           vcoeffs[i]++;
      }
#endif

      for (i = 0; i < 64*23; i++) {
         //printf("%d %d %p\n",i,gpu->mb,&in_buffer[i]);
         in_buffer[i] = rand();
      }

      // Clear output array
      {
        int b;
        for(b=0;b<2;b++) {
          for(i=0;i<16*16;i++) {
            out_buffer[b][i] = 3;
          }
        }
      }

      unifs[0] = mc_filter;
      unifs[1] = in_buffer_ptr.vc+xo[0]+16;
      unifs[2] = 64; // src pitch
      unifs[3] = pitch; // dst pitch
      unifs[4] = 0; // Padding
      unifs[5] = 0;
      unifs[6] = 0;
      unifs[7 ] = mc_filter;
      unifs[8 ] = in_buffer_ptr.vc+xo[1]+16;
      unifs[9 ] = ENCODE_COEFFS(hcoeffs[0], hcoeffs[1], hcoeffs[2], hcoeffs[3]);
      unifs[10] = ENCODE_COEFFS(hcoeffs[4], hcoeffs[5], hcoeffs[6], hcoeffs[7]);
      unifs[11] = ENCODE_COEFFS(vcoeffs[0], vcoeffs[1], vcoeffs[2], vcoeffs[3]);
      unifs[12] = ENCODE_COEFFS(vcoeffs[4], vcoeffs[5], vcoeffs[6], vcoeffs[7]);
      unifs[13] = out_buffer_ptr[0].vc;
      unifs[14] = mc_exit;
      unifs[15] = in_buffer_ptr.vc+xo[1]+16;        // dummy
      unifs[16] = ENCODE_COEFFS(hcoeffs[0], hcoeffs[1], hcoeffs[2], hcoeffs[3]);
      unifs[17] = ENCODE_COEFFS(hcoeffs[4], hcoeffs[5], hcoeffs[6], hcoeffs[7]);
      unifs[18] = ENCODE_COEFFS(vcoeffs[0], vcoeffs[1], vcoeffs[2], vcoeffs[3]);
      unifs[19] = ENCODE_COEFFS(vcoeffs[4], vcoeffs[5], vcoeffs[6], vcoeffs[7]);
      unifs[20] = out_buffer_ptr[1].vc;

      printf("Gpu->vc=%x Code=%x dst=%x\n",gpu->vc, mc_filter,out_buffer_ptr[1].vc);

      // flush_dcache(); TODO is this needed on ARM side? - tried to use the direct alias to avoid this problem

      //qpu_run_shader(mc_setup, unifs_ptr.vc);
      //qpu_run_shader(gpu, gpu->vc, unifs_ptr.vc);
      rpi_do_block(in_buffer_ptr.vc+xo[0]+16, 64, out_buffer_ptr[0].vc, pitch,out_buffer[0]);
      rpi_do_block(in_buffer_ptr.vc+xo[1]+16, 64, out_buffer_ptr[1].vc, pitch,out_buffer[1]);

      if (1)
      {
         int x, y, b;
         int bad = 0;

         for (b=0; b<2; ++b)
            for (y=0; y<YMAX; ++y)
               for (x=0; x<16; ++x) {
                  int32_t ref = filter8(in_buffer+x+y*64+xo[b], 64);

                  if (out_buffer[b][x+y*pitch] != ref) {
                      bad = 1;
//                     printf("%d, %d, %d, %d\n", c, b, x, y);
                  }
#ifndef REGRESSION
                  //printf("%08x %08x\n", out_buffer[b][x+y*pitch], ref);
#endif
               }
          if (bad)
            printf("Failed dst=%x test=%d\n",out_buffer_ptr[1].vc,c);
          else
            printf("Passed dst=%x test=%d\n",out_buffer_ptr[1].vc,c);
      }
      //printf("%d\n", simpenrose_get_qpu_tick_count());
   }

   gpu_free(&out_buffer_ptr[0]);
   gpu_free(&out_buffer_ptr[1]);
   gpu_free(&in_buffer_ptr);
   gpu_free(&unifs_ptr);

   return 0;
}

void rpi_do_block_arm(const uint8_t *in_buffer, int src_pitch, uint8_t *dst, int dst_pitch)
{
  int x,y;
  for (y=0; y<16; ++y) {
    for (x=0; x<16; ++x) {
       dst[x+y*dst_pitch] = filter8(in_buffer+x+y*src_pitch, src_pitch);
    }
  }
}

void rpi_do_block(const uint8_t *in_buffer_vc, int src_pitch, uint8_t *dst_vc, int dst_pitch, uint8_t *dst)
{
   uint32_t *unifs;

   GPU_MEM_PTR_T unifs_ptr;
   //uint8_t *out_buffer;
   //GPU_MEM_PTR_T out_buffer_ptr;

   // Addresses in GPU memory of filter programs
   uint32_t mc_setup = 0;
   uint32_t mc_filter = 0;
   uint32_t mc_exit = 0;
   //int x,y;

   if (gpu==NULL) {
      gpu_lock();
      gpu_unlock();
   }

   // Use table to compute locations of program start points
   mc_setup = code[0] + gpu->vc;
   mc_filter = code[1] + gpu->vc;
   mc_exit = code[2] + gpu->vc;

   if (!vcos_verify(gpu_malloc_uncached(4*64,&unifs_ptr))) {
      return;
   }
   //gpu_malloc_uncached(16*dst_pitch,&out_buffer_ptr);
   //out_buffer = (uint8_t*)out_buffer_ptr.arm;

   /*for (y=0; y<16; ++y) {
      for (x=0; x<16; ++x) {
         out_buffer[x+y*dst_pitch] = 7;
      }
    }*/

   unifs = (uint32_t*)unifs_ptr.arm;

    unifs[0] = mc_filter;
    unifs[1] = (int)in_buffer_vc;
    unifs[2] = src_pitch; // src pitch
    unifs[3] = dst_pitch; // dst pitch
    unifs[4] = 0; // Padding
    unifs[5] = 0;
    unifs[6] = 0;
    unifs[7 ] = mc_exit;
    unifs[8 ] = (int)in_buffer_vc;
    unifs[9 ] = ENCODE_COEFFS(hcoeffs[0], hcoeffs[1], hcoeffs[2], hcoeffs[3]);
    unifs[10] = ENCODE_COEFFS(hcoeffs[4], hcoeffs[5], hcoeffs[6], hcoeffs[7]);
    unifs[11] = ENCODE_COEFFS(vcoeffs[0], vcoeffs[1], vcoeffs[2], vcoeffs[3]);
    unifs[12] = ENCODE_COEFFS(vcoeffs[4], vcoeffs[5], vcoeffs[6], vcoeffs[7]);
    unifs[13] = (int)dst_vc;
    //unifs[13] = (int)out_buffer_ptr.vc;

    //printf("Gpu->vc=%x Code=%x dst=%x\n",gpu->vc, mc_filter,out_buffer_ptr[1].vc);

    qpu_run_shader(mc_setup, unifs_ptr.vc);

    /*for (y=0; y<16; ++y) {
      for (x=0; x<16; ++x) {
         dst[x+y*dst_pitch] = out_buffer[x+y*dst_pitch];
      }
    }*/

    gpu_free(&unifs_ptr);
    //gpu_free(&out_buffer_ptr);
}


#endif
