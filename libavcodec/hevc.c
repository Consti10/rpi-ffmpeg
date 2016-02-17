/*
 * HEVC video Decoder
 *
 * Copyright (C) 2012 - 2013 Guillaume Martres
 * Copyright (C) 2012 - 2013 Mickael Raulet
 * Copyright (C) 2012 - 2013 Gildas Cocherel
 * Copyright (C) 2012 - 2013 Wassim Hamidouche
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/atomic.h"
#include "libavutil/attributes.h"
#include "libavutil/common.h"
#include "libavutil/display.h"
#include "libavutil/internal.h"
#include "libavutil/md5.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/stereo3d.h"

#include "bswapdsp.h"
#include "bytestream.h"
#include "cabac_functions.h"
#include "golomb.h"
#include "hevc.h"

#ifdef RPI
  #include "rpi_qpu.h"
  #include "rpi_auxframe.h"
  #include "rpi_user_vcsm.h"
  #include "rpi_mailbox.h"

  // Move Inter prediction into separate pass
  #define RPI_INTER

  #ifdef RPI_INTER_QPU
    // Define RPI_MULTI_MAILBOX to use the updated mailbox that can launch both QPU and VPU
    #define RPI_MULTI_MAILBOX
  #endif

  // Define RPI_CACHE_UNIF_MVS to write motion vector uniform stream to cached memory
  // RPI_CACHE_UNIF_MVS doesn't seem to make much difference, so left undefined.

  // Define RPI_SIMULATE_QPUS for debugging to run QPU code on the ARMs
  //#define RPI_SIMULATE_QPUS
  #ifdef RPI_WORKER
    #include "pthread.h"
  #endif

  static void rpi_execute_dblk_cmds(HEVCContext *s);
  static void rpi_execute_transform(HEVCContext *s);
  static void rpi_launch_vpu_qpu(HEVCContext *s);
  static void rpi_execute_pred_cmds(HEVCContext *s);
  static void rpi_execute_inter_cmds(HEVCContext *s);
  static void rpi_begin(HEVCContext *s);
  static void flush_frame(HEVCContext *s,AVFrame *frame);
  static void flush_frame3(HEVCContext *s,AVFrame *frame,GPU_MEM_PTR_T *p0,GPU_MEM_PTR_T *p1,GPU_MEM_PTR_T *p2, int job);

#endif

// #define DISABLE_MC

#ifndef av_mod_uintp2
static av_always_inline av_const unsigned av_mod_uintp2_c(unsigned a, unsigned p)
{
    return a & ((1 << p) - 1);
}
#   define av_mod_uintp2   av_mod_uintp2_c
#endif

const uint8_t ff_hevc_pel_weight[65] = { [2] = 0, [4] = 1, [6] = 2, [8] = 3, [12] = 4, [16] = 5, [24] = 6, [32] = 7, [48] = 8, [64] = 9 };


#ifdef RPI_INTER_QPU

// Each luma QPU processes 2*RPI_NUM_CHUNKS 64x64 blocks
// Each chroma QPU processes 3*RPI_NUM_CHUNKS 64x64 blocks, but requires two commands for B blocks
// For each block of 64*64 the smallest block size is 8x4
// We also need an extra command for the setup information

#define RPI_CHROMA_COMMAND_WORDS 12
#define UV_COMMANDS_PER_QPU ((1 + 3*RPI_NUM_CHUNKS*(64*64)*2/(8*4)) * RPI_CHROMA_COMMAND_WORDS)
// The QPU code for UV blocks only works up to a block width of 8
#define RPI_CHROMA_BLOCK_WIDTH 8

#define RPI_LUMA_COMMAND_WORDS 9
#define Y_COMMANDS_PER_QPU ((1+2*RPI_NUM_CHUNKS*(64*64)/(8*4)) * RPI_LUMA_COMMAND_WORDS)

#define ENCODE_COEFFS(c0, c1, c2, c3) (((c0) & 0xff) | ((c1) & 0xff) << 8 | ((c2) & 0xff) << 16 | ((c3) & 0xff) << 24)

// TODO Chroma only needs 4 taps
static uint32_t rpi_filter_coefs[8][1] = {
        { ENCODE_COEFFS(   0,  64,   0,   0) },
        { ENCODE_COEFFS(  -2,  58,  10,  -2) },
        { ENCODE_COEFFS(  -4,  54,  16,  -2) },
        { ENCODE_COEFFS(  -6,  46,  28,  -4) },
        { ENCODE_COEFFS(  -4,  36,  36,  -4) },
        { ENCODE_COEFFS(  -4,  28,  46,  -6) },
        { ENCODE_COEFFS(  -2,  16,  54,  -4) },
        { ENCODE_COEFFS(  -2,  10,  58,  -2) }
};

#endif


#ifdef RPI_WORKER

//#define LOG_ENTER printf("Enter %s: p0=%d p1=%d (%d jobs) %p\n", __func__,s->pass0_job,s->pass1_job,s->worker_tail-s->worker_head,s);
//#define LOG_EXIT printf("Exit %s: p0=%d p1=%d (%d jobs) %p\n", __func__,s->pass0_job,s->pass1_job,s->worker_tail-s->worker_head,s);

#define LOG_ENTER
#define LOG_EXIT

// Call this when we have completed pass0 and wish to trigger pass1 for the current job
static void worker_submit_job(HEVCContext *s)
{
  LOG_ENTER
  pthread_mutex_lock(&s->worker_mutex);
  s->worker_tail++;
  s->pass0_job = (s->pass0_job + 1) % RPI_MAX_JOBS; // Move onto the next slot
  pthread_cond_broadcast(&s->worker_cond_tail); // Let people know that the tail has moved
  pthread_mutex_unlock(&s->worker_mutex);
  LOG_EXIT
}

// Call this to say we have completed pass1
static void worker_complete_job(HEVCContext *s)
{
  LOG_ENTER
  pthread_mutex_lock(&s->worker_mutex);
  s->worker_head++;
  s->pass1_job = (s->pass1_job + 1) % RPI_MAX_JOBS; // Move onto the next slot
  pthread_cond_broadcast(&s->worker_cond_head); // Let people know that the head has moved
  pthread_mutex_unlock(&s->worker_mutex);
  LOG_EXIT
}

// Call this to wait for all jobs to have completed at the end of a frame
static void worker_wait(HEVCContext *s)
{
  LOG_ENTER
  pthread_mutex_lock(&s->worker_mutex);
  while( s->worker_head !=s->worker_tail)
  {
    pthread_cond_wait(&s->worker_cond_head, &s->worker_mutex);
  }
  pthread_mutex_unlock(&s->worker_mutex);
  LOG_EXIT
}

// Call worker_pass0_ready to wait until the s->pass0_job slot becomes
// available to receive the next job.
static void worker_pass0_ready(HEVCContext *s)
{
  LOG_ENTER
    pthread_mutex_lock(&s->worker_mutex);
    // tail is number of submitted jobs
    // head is number of completed jobs
    // tail-head is number of outstanding jobs in the queue
    // we need to ensure there is at least 1 space left for us to use
    while( s->worker_tail - s->worker_head >= RPI_MAX_JOBS)
    {
      // Wait until another job is completed
      pthread_cond_wait(&s->worker_cond_head, &s->worker_mutex);
    }
    pthread_mutex_unlock(&s->worker_mutex);
  LOG_EXIT
}

static void *worker_start(void *arg)
{
  HEVCContext *s = (HEVCContext *)arg;
  while(1) {
    pthread_mutex_lock(&s->worker_mutex);

    while( !s->kill_worker && s->worker_tail - s->worker_head <= 0)
    {
      pthread_cond_wait(&s->worker_cond_tail, &s->worker_mutex);
    }
    pthread_mutex_unlock(&s->worker_mutex);

    if (s->kill_worker) {
      break;
    }
    LOG_ENTER
    // printf("%d %d %d : %d %d %d %d\n",s->poc, x_ctb, y_ctb, s->num_pred_cmds,s->num_mv_cmds,s->num_coeffs[2] >> 8,s->num_coeffs[3] >> 10);
    rpi_launch_vpu_qpu(s);
    // Perform inter prediction
    rpi_execute_inter_cmds(s);
    // Wait for transform completion
    vpu_wait(s->vpu_id);

    // Perform intra prediction and residual reconstruction
    rpi_execute_pred_cmds(s);
    // Perform deblocking for CTBs in this row
    rpi_execute_dblk_cmds(s);

    worker_complete_job(s);
    LOG_EXIT
  }
  return NULL;
}

#endif

/**
 * NOTE: Each function hls_foo correspond to the function foo in the
 * specification (HLS stands for High Level Syntax).
 */

/**
 * Section 5.7
 */

/* free everything allocated  by pic_arrays_init() */
static void pic_arrays_free(HEVCContext *s)
{
#ifdef RPI
    int job;
    for(job=0;job<RPI_MAX_JOBS;job++) {
      if (s->coeffs_buf_arm[job][0]) {
        gpu_free(&s->coeffs_buf_default[job]);
        s->coeffs_buf_arm[job][0] = 0;
      }
      if (s->coeffs_buf_arm[job][2]) {
        gpu_free(&s->coeffs_buf_accelerated[job]);
        s->coeffs_buf_arm[job][2] = 0;
      }
    }
#endif
#ifdef RPI_DEBLOCK_VPU
    if (s->y_setup_arm) {
      gpu_free(&s->y_setup_ptr);
      s->y_setup_arm = 0;
    }
    if (s->uv_setup_arm) {
      gpu_free(&s->uv_setup_ptr);
      s->uv_setup_arm = 0;
    }
    if (s->vpu_cmds_arm) {
      gpu_free(&s->vpu_cmds_ptr);
      s->vpu_cmds_arm = 0;
    }
#endif
    av_freep(&s->sao);
    av_freep(&s->deblock);

    av_freep(&s->skip_flag);
    av_freep(&s->tab_ct_depth);

    av_freep(&s->tab_ipm);
    av_freep(&s->cbf_luma);
    av_freep(&s->is_pcm);

    av_freep(&s->qp_y_tab);
    av_freep(&s->tab_slice_address);
    av_freep(&s->filter_slice_edges);

    av_freep(&s->horizontal_bs);
    av_freep(&s->vertical_bs);

    av_freep(&s->sh.entry_point_offset);
    av_freep(&s->sh.size);
    av_freep(&s->sh.offset);

    av_buffer_pool_uninit(&s->tab_mvf_pool);
    av_buffer_pool_uninit(&s->rpl_tab_pool);
}

/* allocate arrays that depend on frame dimensions */
static int pic_arrays_init(HEVCContext *s, const HEVCSPS *sps)
{
    int log2_min_cb_size = sps->log2_min_cb_size;
    int width            = sps->width;
    int height           = sps->height;
    int pic_size_in_ctb  = ((width  >> log2_min_cb_size) + 1) *
                           ((height >> log2_min_cb_size) + 1);
    int ctb_count        = sps->ctb_width * sps->ctb_height;
    int min_pu_size      = sps->min_pu_width * sps->min_pu_height;

#ifdef RPI
    int coefs_in_ctb = (1 << sps->log2_ctb_size) * (1 << sps->log2_ctb_size);
    int coefs_per_luma = 64*64*24*RPI_NUM_CHUNKS;
    int coefs_per_chroma = (coefs_per_luma * 2) >> sps->vshift[1] >> sps->hshift[1];
    int coefs_per_row = coefs_per_luma + coefs_per_chroma;
    int job;
    av_assert0(sps);
    s->max_ctu_count = coefs_per_luma / coefs_in_ctb;
    s->ctu_per_y_chan = s->max_ctu_count / 12;
    s->ctu_per_uv_chan = s->max_ctu_count / 8;
    for(job=0;job<RPI_MAX_JOBS;job++) {
      printf("Allocated %d\n",coefs_per_row);
      for(job=0;job<RPI_MAX_JOBS;job++) {
        gpu_malloc_cached(sizeof(int16_t) * coefs_per_row, &s->coeffs_buf_default[job]);
        s->coeffs_buf_arm[job][0] = (int16_t*) s->coeffs_buf_default[job].arm;
        if (!s->coeffs_buf_arm[job][0])
            goto fail;
        gpu_malloc_cached(sizeof(int16_t) * (coefs_per_row + 32*32), &s->coeffs_buf_accelerated[job]);  // We prefetch past the end so provide an extra blocks worth of data
        s->coeffs_buf_arm[job][2] = (int16_t*) s->coeffs_buf_accelerated[job].arm;
        s->coeffs_buf_vc[job][2] = s->coeffs_buf_accelerated[job].vc;
        if (!s->coeffs_buf_arm[job][2])
            goto fail;
        s->coeffs_buf_arm[job][3] = coefs_per_row + s->coeffs_buf_arm[job][2];  // This points to just beyond the end of the buffer.  Coefficients fill in backwards.
        s->coeffs_buf_vc[job][3] = sizeof(int16_t) * coefs_per_row + s->coeffs_buf_vc[job][2];
      }
    }
#endif
#ifdef RPI_DEBLOCK_VPU
    s->enable_rpi_deblock = !sps->sao_enabled;
    s->setup_width = (sps->width+15) / 16;
    s->setup_height = (sps->height+15) / 16;
    gpu_malloc_uncached(sizeof(*s->y_setup_arm) * s->setup_width * s->setup_height, &s->y_setup_ptr); // TODO make this cached
    s->y_setup_arm = (void*)s->y_setup_ptr.arm;
    s->y_setup_vc = (void*)s->y_setup_ptr.vc;
    memset(s->y_setup_arm, 0, s->y_setup_ptr.numbytes);
    printf("Setup %d by %d by %d\n",s->setup_width,s->setup_height,sizeof(*s->y_setup_arm));

    s->uv_setup_width = ( (sps->width >> sps->hshift[1]) + 15) / 16;
    s->uv_setup_height = ( (sps->height >> sps->vshift[1]) + 15) / 16;
    gpu_malloc_uncached(sizeof(*s->uv_setup_arm) * s->uv_setup_width * s->uv_setup_height, &s->uv_setup_ptr); // TODO make this cached
    s->uv_setup_arm = (void*)s->uv_setup_ptr.arm;
    s->uv_setup_vc = (void*)s->uv_setup_ptr.vc;
    memset(s->uv_setup_arm, 0, s->uv_setup_ptr.numbytes);
    printf("Setup uv %d by %d by %d\n",s->uv_setup_width,s->uv_setup_height,sizeof(*s->uv_setup_arm));

    gpu_malloc_uncached(sizeof(*s->vpu_cmds_arm) * 3,&s->vpu_cmds_ptr);
    s->vpu_cmds_arm = (void*) s->vpu_cmds_ptr.arm;
    s->vpu_cmds_vc = s->vpu_cmds_ptr.vc;
#endif

    s->bs_width  = (width  >> 2) + 1;
    s->bs_height = (height >> 2) + 1;

    s->sao           = av_mallocz_array(ctb_count, sizeof(*s->sao));
    s->deblock       = av_mallocz_array(ctb_count, sizeof(*s->deblock));
    if (!s->sao || !s->deblock)
        goto fail;

    s->skip_flag    = av_malloc_array(sps->min_cb_height, sps->min_cb_width);
    s->tab_ct_depth = av_malloc_array(sps->min_cb_height, sps->min_cb_width);
    if (!s->skip_flag || !s->tab_ct_depth)
        goto fail;

    s->cbf_luma = av_malloc_array(sps->min_tb_width, sps->min_tb_height);
    s->tab_ipm  = av_mallocz(min_pu_size);
    s->is_pcm   = av_malloc_array(sps->min_pu_width + 1, sps->min_pu_height + 1);
    if (!s->tab_ipm || !s->cbf_luma || !s->is_pcm)
        goto fail;

    s->filter_slice_edges = av_mallocz(ctb_count);
    s->tab_slice_address  = av_malloc_array(pic_size_in_ctb,
                                      sizeof(*s->tab_slice_address));
    s->qp_y_tab           = av_malloc_array(pic_size_in_ctb,
                                      sizeof(*s->qp_y_tab));
    if (!s->qp_y_tab || !s->filter_slice_edges || !s->tab_slice_address)
        goto fail;

    s->horizontal_bs = av_mallocz_array(s->bs_width, s->bs_height);
    s->vertical_bs   = av_mallocz_array(s->bs_width, s->bs_height);
    if (!s->horizontal_bs || !s->vertical_bs)
        goto fail;

    s->tab_mvf_pool = av_buffer_pool_init(min_pu_size * sizeof(MvField),
                                          av_buffer_allocz);
    s->rpl_tab_pool = av_buffer_pool_init(ctb_count * sizeof(RefPicListTab),
                                          av_buffer_allocz);
    if (!s->tab_mvf_pool || !s->rpl_tab_pool)
        goto fail;

    return 0;

fail:
    pic_arrays_free(s);
    return AVERROR(ENOMEM);
}

static void pred_weight_table(HEVCContext *s, GetBitContext *gb)
{
    int i = 0;
    int j = 0;
    uint8_t luma_weight_l0_flag[16];
    uint8_t chroma_weight_l0_flag[16];
    uint8_t luma_weight_l1_flag[16];
    uint8_t chroma_weight_l1_flag[16];
    int luma_log2_weight_denom;

    luma_log2_weight_denom = get_ue_golomb_long(gb);
    if (luma_log2_weight_denom < 0 || luma_log2_weight_denom > 7)
        av_log(s->avctx, AV_LOG_ERROR, "luma_log2_weight_denom %d is invalid\n", luma_log2_weight_denom);
    s->sh.luma_log2_weight_denom = av_clip_uintp2(luma_log2_weight_denom, 3);
    if (s->ps.sps->chroma_format_idc != 0) {
        int delta = get_se_golomb(gb);
        s->sh.chroma_log2_weight_denom = av_clip_uintp2(s->sh.luma_log2_weight_denom + delta, 3);
    }

    for (i = 0; i < s->sh.nb_refs[L0]; i++) {
        luma_weight_l0_flag[i] = get_bits1(gb);
        if (!luma_weight_l0_flag[i]) {
            s->sh.luma_weight_l0[i] = 1 << s->sh.luma_log2_weight_denom;
            s->sh.luma_offset_l0[i] = 0;
        }
    }
    if (s->ps.sps->chroma_format_idc != 0) {
        for (i = 0; i < s->sh.nb_refs[L0]; i++)
            chroma_weight_l0_flag[i] = get_bits1(gb);
    } else {
        for (i = 0; i < s->sh.nb_refs[L0]; i++)
            chroma_weight_l0_flag[i] = 0;
    }
    for (i = 0; i < s->sh.nb_refs[L0]; i++) {
        if (luma_weight_l0_flag[i]) {
            int delta_luma_weight_l0 = get_se_golomb(gb);
            s->sh.luma_weight_l0[i] = (1 << s->sh.luma_log2_weight_denom) + delta_luma_weight_l0;
            s->sh.luma_offset_l0[i] = get_se_golomb(gb);
        }
        if (chroma_weight_l0_flag[i]) {
            for (j = 0; j < 2; j++) {
                int delta_chroma_weight_l0 = get_se_golomb(gb);
                int delta_chroma_offset_l0 = get_se_golomb(gb);
                s->sh.chroma_weight_l0[i][j] = (1 << s->sh.chroma_log2_weight_denom) + delta_chroma_weight_l0;
                s->sh.chroma_offset_l0[i][j] = av_clip((delta_chroma_offset_l0 - ((128 * s->sh.chroma_weight_l0[i][j])
                                                                                    >> s->sh.chroma_log2_weight_denom) + 128), -128, 127);
            }
        } else {
            s->sh.chroma_weight_l0[i][0] = 1 << s->sh.chroma_log2_weight_denom;
            s->sh.chroma_offset_l0[i][0] = 0;
            s->sh.chroma_weight_l0[i][1] = 1 << s->sh.chroma_log2_weight_denom;
            s->sh.chroma_offset_l0[i][1] = 0;
        }
    }
    if (s->sh.slice_type == B_SLICE) {
        for (i = 0; i < s->sh.nb_refs[L1]; i++) {
            luma_weight_l1_flag[i] = get_bits1(gb);
            if (!luma_weight_l1_flag[i]) {
                s->sh.luma_weight_l1[i] = 1 << s->sh.luma_log2_weight_denom;
                s->sh.luma_offset_l1[i] = 0;
            }
        }
        if (s->ps.sps->chroma_format_idc != 0) {
            for (i = 0; i < s->sh.nb_refs[L1]; i++)
                chroma_weight_l1_flag[i] = get_bits1(gb);
        } else {
            for (i = 0; i < s->sh.nb_refs[L1]; i++)
                chroma_weight_l1_flag[i] = 0;
        }
        for (i = 0; i < s->sh.nb_refs[L1]; i++) {
            if (luma_weight_l1_flag[i]) {
                int delta_luma_weight_l1 = get_se_golomb(gb);
                s->sh.luma_weight_l1[i] = (1 << s->sh.luma_log2_weight_denom) + delta_luma_weight_l1;
                s->sh.luma_offset_l1[i] = get_se_golomb(gb);
            }
            if (chroma_weight_l1_flag[i]) {
                for (j = 0; j < 2; j++) {
                    int delta_chroma_weight_l1 = get_se_golomb(gb);
                    int delta_chroma_offset_l1 = get_se_golomb(gb);
                    s->sh.chroma_weight_l1[i][j] = (1 << s->sh.chroma_log2_weight_denom) + delta_chroma_weight_l1;
                    s->sh.chroma_offset_l1[i][j] = av_clip((delta_chroma_offset_l1 - ((128 * s->sh.chroma_weight_l1[i][j])
                                                                                        >> s->sh.chroma_log2_weight_denom) + 128), -128, 127);
                }
            } else {
                s->sh.chroma_weight_l1[i][0] = 1 << s->sh.chroma_log2_weight_denom;
                s->sh.chroma_offset_l1[i][0] = 0;
                s->sh.chroma_weight_l1[i][1] = 1 << s->sh.chroma_log2_weight_denom;
                s->sh.chroma_offset_l1[i][1] = 0;
            }
        }
    }
}

static int decode_lt_rps(HEVCContext *s, LongTermRPS *rps, GetBitContext *gb)
{
    const HEVCSPS *sps = s->ps.sps;
    int max_poc_lsb    = 1 << sps->log2_max_poc_lsb;
    int prev_delta_msb = 0;
    unsigned int nb_sps = 0, nb_sh;
    int i;

    rps->nb_refs = 0;
    if (!sps->long_term_ref_pics_present_flag)
        return 0;

    if (sps->num_long_term_ref_pics_sps > 0)
        nb_sps = get_ue_golomb_long(gb);
    nb_sh = get_ue_golomb_long(gb);

    if (nb_sh + (uint64_t)nb_sps > FF_ARRAY_ELEMS(rps->poc))
        return AVERROR_INVALIDDATA;

    rps->nb_refs = nb_sh + nb_sps;

    for (i = 0; i < rps->nb_refs; i++) {
        uint8_t delta_poc_msb_present;

        if (i < nb_sps) {
            uint8_t lt_idx_sps = 0;

            if (sps->num_long_term_ref_pics_sps > 1)
                lt_idx_sps = get_bits(gb, av_ceil_log2(sps->num_long_term_ref_pics_sps));

            rps->poc[i]  = sps->lt_ref_pic_poc_lsb_sps[lt_idx_sps];
            rps->used[i] = sps->used_by_curr_pic_lt_sps_flag[lt_idx_sps];
        } else {
            rps->poc[i]  = get_bits(gb, sps->log2_max_poc_lsb);
            rps->used[i] = get_bits1(gb);
        }

        delta_poc_msb_present = get_bits1(gb);
        if (delta_poc_msb_present) {
            int delta = get_ue_golomb_long(gb);

            if (i && i != nb_sps)
                delta += prev_delta_msb;

            rps->poc[i] += s->poc - delta * max_poc_lsb - s->sh.pic_order_cnt_lsb;
            prev_delta_msb = delta;
        }
    }

    return 0;
}

static void export_stream_params(AVCodecContext *avctx, const HEVCParamSets *ps,
                                 const HEVCSPS *sps)
{
    const HEVCVPS *vps = (const HEVCVPS*)ps->vps_list[sps->vps_id]->data;
    unsigned int num = 0, den = 0;

    avctx->pix_fmt             = sps->pix_fmt;
    avctx->coded_width         = sps->width;
    avctx->coded_height        = sps->height;
    avctx->width               = sps->output_width;
    avctx->height              = sps->output_height;
    avctx->has_b_frames        = sps->temporal_layer[sps->max_sub_layers - 1].num_reorder_pics;
    avctx->profile             = sps->ptl.general_ptl.profile_idc;
    avctx->level               = sps->ptl.general_ptl.level_idc;

    ff_set_sar(avctx, sps->vui.sar);

    if (sps->vui.video_signal_type_present_flag)
        avctx->color_range = sps->vui.video_full_range_flag ? AVCOL_RANGE_JPEG
                                                            : AVCOL_RANGE_MPEG;
    else
        avctx->color_range = AVCOL_RANGE_MPEG;

    if (sps->vui.colour_description_present_flag) {
        avctx->color_primaries = sps->vui.colour_primaries;
        avctx->color_trc       = sps->vui.transfer_characteristic;
        avctx->colorspace      = sps->vui.matrix_coeffs;
    } else {
        avctx->color_primaries = AVCOL_PRI_UNSPECIFIED;
        avctx->color_trc       = AVCOL_TRC_UNSPECIFIED;
        avctx->colorspace      = AVCOL_SPC_UNSPECIFIED;
    }

    if (vps->vps_timing_info_present_flag) {
        num = vps->vps_num_units_in_tick;
        den = vps->vps_time_scale;
    } else if (sps->vui.vui_timing_info_present_flag) {
        num = sps->vui.vui_num_units_in_tick;
        den = sps->vui.vui_time_scale;
    }

    if (num != 0 && den != 0)
        av_reduce(&avctx->framerate.den, &avctx->framerate.num,
                  num, den, 1 << 30);
}

static int set_sps(HEVCContext *s, const HEVCSPS *sps, enum AVPixelFormat pix_fmt)
{
    #define HWACCEL_MAX (CONFIG_HEVC_DXVA2_HWACCEL + CONFIG_HEVC_D3D11VA_HWACCEL + CONFIG_HEVC_VAAPI_HWACCEL + CONFIG_HEVC_VDPAU_HWACCEL)
    enum AVPixelFormat pix_fmts[HWACCEL_MAX + 2], *fmt = pix_fmts;
    int ret, i;

    pic_arrays_free(s);
    s->ps.sps = NULL;
    s->ps.vps = NULL;

    if (!sps)
        return 0;

    ret = pic_arrays_init(s, sps);
    if (ret < 0)
        goto fail;

    export_stream_params(s->avctx, &s->ps, sps);

    if (sps->pix_fmt == AV_PIX_FMT_YUV420P || sps->pix_fmt == AV_PIX_FMT_YUVJ420P) {
#if CONFIG_HEVC_DXVA2_HWACCEL
        *fmt++ = AV_PIX_FMT_DXVA2_VLD;
#endif
#if CONFIG_HEVC_D3D11VA_HWACCEL
        *fmt++ = AV_PIX_FMT_D3D11VA_VLD;
#endif
#if CONFIG_HEVC_VAAPI_HWACCEL
        *fmt++ = AV_PIX_FMT_VAAPI;
#endif
#if CONFIG_HEVC_VDPAU_HWACCEL
        *fmt++ = AV_PIX_FMT_VDPAU;
#endif
    }

    if (pix_fmt == AV_PIX_FMT_NONE) {
        *fmt++ = sps->pix_fmt;
        *fmt = AV_PIX_FMT_NONE;

        ret = ff_thread_get_format(s->avctx, pix_fmts);
        if (ret < 0)
            goto fail;
        s->avctx->pix_fmt = ret;
    }
    else {
        s->avctx->pix_fmt = pix_fmt;
    }

    ff_hevc_pred_init(&s->hpc,     sps->bit_depth);
    ff_hevc_dsp_init (&s->hevcdsp, sps->bit_depth);
    ff_videodsp_init (&s->vdsp,    sps->bit_depth);

    for (i = 0; i < 3; i++) {
        av_freep(&s->sao_pixel_buffer_h[i]);
        av_freep(&s->sao_pixel_buffer_v[i]);
    }

    if (sps->sao_enabled && !s->avctx->hwaccel) {
        int c_count = (sps->chroma_format_idc != 0) ? 3 : 1;
        int c_idx;

        for(c_idx = 0; c_idx < c_count; c_idx++) {
            int w = sps->width >> sps->hshift[c_idx];
            int h = sps->height >> sps->vshift[c_idx];
            s->sao_pixel_buffer_h[c_idx] =
                av_malloc((w * 2 * sps->ctb_height) <<
                          sps->pixel_shift);
            s->sao_pixel_buffer_v[c_idx] =
                av_malloc((h * 2 * sps->ctb_width) <<
                          sps->pixel_shift);
        }
    }

    s->ps.sps = sps;
    s->ps.vps = (HEVCVPS*) s->ps.vps_list[s->ps.sps->vps_id]->data;

    return 0;

fail:
    pic_arrays_free(s);
    s->ps.sps = NULL;
    return ret;
}

static int hls_slice_header(HEVCContext *s)
{
    GetBitContext *gb = &s->HEVClc->gb;
    SliceHeader *sh   = &s->sh;
    int i, ret;

    // Coded parameters
    sh->first_slice_in_pic_flag = get_bits1(gb);
    if ((IS_IDR(s) || IS_BLA(s)) && sh->first_slice_in_pic_flag) {
        s->seq_decode = (s->seq_decode + 1) & 0xff;
        s->max_ra     = INT_MAX;
        if (IS_IDR(s))
            ff_hevc_clear_refs(s);
    }
    sh->no_output_of_prior_pics_flag = 0;
    if (IS_IRAP(s))
        sh->no_output_of_prior_pics_flag = get_bits1(gb);

    sh->pps_id = get_ue_golomb_long(gb);
    if (sh->pps_id >= MAX_PPS_COUNT || !s->ps.pps_list[sh->pps_id]) {
        av_log(s->avctx, AV_LOG_ERROR, "PPS id out of range: %d\n", sh->pps_id);
        return AVERROR_INVALIDDATA;
    }
    if (!sh->first_slice_in_pic_flag &&
        s->ps.pps != (HEVCPPS*)s->ps.pps_list[sh->pps_id]->data) {
        av_log(s->avctx, AV_LOG_ERROR, "PPS changed between slices.\n");
        return AVERROR_INVALIDDATA;
    }
    s->ps.pps = (HEVCPPS*)s->ps.pps_list[sh->pps_id]->data;
    if (s->nal_unit_type == NAL_CRA_NUT && s->last_eos == 1)
        sh->no_output_of_prior_pics_flag = 1;

    if (s->ps.sps != (HEVCSPS*)s->ps.sps_list[s->ps.pps->sps_id]->data) {
        const HEVCSPS* last_sps = s->ps.sps;
        s->ps.sps = (HEVCSPS*)s->ps.sps_list[s->ps.pps->sps_id]->data;
        if (last_sps && IS_IRAP(s) && s->nal_unit_type != NAL_CRA_NUT) {
            if (s->ps.sps->width !=  last_sps->width || s->ps.sps->height != last_sps->height ||
                s->ps.sps->temporal_layer[s->ps.sps->max_sub_layers - 1].max_dec_pic_buffering !=
                last_sps->temporal_layer[last_sps->max_sub_layers - 1].max_dec_pic_buffering)
                sh->no_output_of_prior_pics_flag = 0;
        }
        ff_hevc_clear_refs(s);
        ret = set_sps(s, s->ps.sps, AV_PIX_FMT_NONE);
        if (ret < 0)
            return ret;

        s->seq_decode = (s->seq_decode + 1) & 0xff;
        s->max_ra     = INT_MAX;
    }

    sh->dependent_slice_segment_flag = 0;
    if (!sh->first_slice_in_pic_flag) {
        int slice_address_length;

        if (s->ps.pps->dependent_slice_segments_enabled_flag)
            sh->dependent_slice_segment_flag = get_bits1(gb);

        slice_address_length = av_ceil_log2(s->ps.sps->ctb_width *
                                            s->ps.sps->ctb_height);
        sh->slice_segment_addr = slice_address_length ? get_bits(gb, slice_address_length) : 0;
        if (sh->slice_segment_addr >= s->ps.sps->ctb_width * s->ps.sps->ctb_height) {
            av_log(s->avctx, AV_LOG_ERROR,
                   "Invalid slice segment address: %u.\n",
                   sh->slice_segment_addr);
            return AVERROR_INVALIDDATA;
        }

        if (!sh->dependent_slice_segment_flag) {
            sh->slice_addr = sh->slice_segment_addr;
            s->slice_idx++;
        }
    } else {
        sh->slice_segment_addr = sh->slice_addr = 0;
        s->slice_idx           = 0;
        s->slice_initialized   = 0;
    }

    if (!sh->dependent_slice_segment_flag) {
        s->slice_initialized = 0;

        for (i = 0; i < s->ps.pps->num_extra_slice_header_bits; i++)
            skip_bits(gb, 1);  // slice_reserved_undetermined_flag[]

        sh->slice_type = get_ue_golomb_long(gb);
        if (!(sh->slice_type == I_SLICE ||
              sh->slice_type == P_SLICE ||
              sh->slice_type == B_SLICE)) {
            av_log(s->avctx, AV_LOG_ERROR, "Unknown slice type: %d.\n",
                   sh->slice_type);
            return AVERROR_INVALIDDATA;
        }
        if (IS_IRAP(s) && sh->slice_type != I_SLICE) {
            av_log(s->avctx, AV_LOG_ERROR, "Inter slices in an IRAP frame.\n");
            return AVERROR_INVALIDDATA;
        }

        // when flag is not present, picture is inferred to be output
        sh->pic_output_flag = 1;
        if (s->ps.pps->output_flag_present_flag)
            sh->pic_output_flag = get_bits1(gb);

        if (s->ps.sps->separate_colour_plane_flag)
            sh->colour_plane_id = get_bits(gb, 2);

        if (!IS_IDR(s)) {
            int poc, pos;

            sh->pic_order_cnt_lsb = get_bits(gb, s->ps.sps->log2_max_poc_lsb);
            poc = ff_hevc_compute_poc(s, sh->pic_order_cnt_lsb);
            if (!sh->first_slice_in_pic_flag && poc != s->poc) {
                av_log(s->avctx, AV_LOG_WARNING,
                       "Ignoring POC change between slices: %d -> %d\n", s->poc, poc);
                if (s->avctx->err_recognition & AV_EF_EXPLODE)
                    return AVERROR_INVALIDDATA;
                poc = s->poc;
            }
            s->poc = poc;

            sh->short_term_ref_pic_set_sps_flag = get_bits1(gb);
            pos = get_bits_left(gb);
            if (!sh->short_term_ref_pic_set_sps_flag) {
                ret = ff_hevc_decode_short_term_rps(gb, s->avctx, &sh->slice_rps, s->ps.sps, 1);
                if (ret < 0)
                    return ret;

                sh->short_term_rps = &sh->slice_rps;
            } else {
                int numbits, rps_idx;

                if (!s->ps.sps->nb_st_rps) {
                    av_log(s->avctx, AV_LOG_ERROR, "No ref lists in the SPS.\n");
                    return AVERROR_INVALIDDATA;
                }

                numbits = av_ceil_log2(s->ps.sps->nb_st_rps);
                rps_idx = numbits > 0 ? get_bits(gb, numbits) : 0;
                sh->short_term_rps = &s->ps.sps->st_rps[rps_idx];
            }
            sh->short_term_ref_pic_set_size = pos - get_bits_left(gb);

            pos = get_bits_left(gb);
            ret = decode_lt_rps(s, &sh->long_term_rps, gb);
            if (ret < 0) {
                av_log(s->avctx, AV_LOG_WARNING, "Invalid long term RPS.\n");
                if (s->avctx->err_recognition & AV_EF_EXPLODE)
                    return AVERROR_INVALIDDATA;
            }
            sh->long_term_ref_pic_set_size = pos - get_bits_left(gb);

            if (s->ps.sps->sps_temporal_mvp_enabled_flag)
                sh->slice_temporal_mvp_enabled_flag = get_bits1(gb);
            else
                sh->slice_temporal_mvp_enabled_flag = 0;
        } else {
            s->sh.short_term_rps = NULL;
            s->poc               = 0;
        }

        /* 8.3.1 */
        if (s->temporal_id == 0 &&
            s->nal_unit_type != NAL_TRAIL_N &&
            s->nal_unit_type != NAL_TSA_N   &&
            s->nal_unit_type != NAL_STSA_N  &&
            s->nal_unit_type != NAL_RADL_N  &&
            s->nal_unit_type != NAL_RADL_R  &&
            s->nal_unit_type != NAL_RASL_N  &&
            s->nal_unit_type != NAL_RASL_R)
            s->pocTid0 = s->poc;

        if (s->ps.sps->sao_enabled) {
            sh->slice_sample_adaptive_offset_flag[0] = get_bits1(gb);
            if (s->ps.sps->chroma_format_idc) {
                sh->slice_sample_adaptive_offset_flag[1] =
                sh->slice_sample_adaptive_offset_flag[2] = get_bits1(gb);
            }
        } else {
            sh->slice_sample_adaptive_offset_flag[0] = 0;
            sh->slice_sample_adaptive_offset_flag[1] = 0;
            sh->slice_sample_adaptive_offset_flag[2] = 0;
        }

        sh->nb_refs[L0] = sh->nb_refs[L1] = 0;
        if (sh->slice_type == P_SLICE || sh->slice_type == B_SLICE) {
            int nb_refs;

            sh->nb_refs[L0] = s->ps.pps->num_ref_idx_l0_default_active;
            if (sh->slice_type == B_SLICE)
                sh->nb_refs[L1] = s->ps.pps->num_ref_idx_l1_default_active;

            if (get_bits1(gb)) { // num_ref_idx_active_override_flag
                sh->nb_refs[L0] = get_ue_golomb_long(gb) + 1;
                if (sh->slice_type == B_SLICE)
                    sh->nb_refs[L1] = get_ue_golomb_long(gb) + 1;
            }
            if (sh->nb_refs[L0] > MAX_REFS || sh->nb_refs[L1] > MAX_REFS) {
                av_log(s->avctx, AV_LOG_ERROR, "Too many refs: %d/%d.\n",
                       sh->nb_refs[L0], sh->nb_refs[L1]);
                return AVERROR_INVALIDDATA;
            }

            sh->rpl_modification_flag[0] = 0;
            sh->rpl_modification_flag[1] = 0;
            nb_refs = ff_hevc_frame_nb_refs(s);
            if (!nb_refs) {
                av_log(s->avctx, AV_LOG_ERROR, "Zero refs for a frame with P or B slices.\n");
                return AVERROR_INVALIDDATA;
            }

            if (s->ps.pps->lists_modification_present_flag && nb_refs > 1) {
                sh->rpl_modification_flag[0] = get_bits1(gb);
                if (sh->rpl_modification_flag[0]) {
                    for (i = 0; i < sh->nb_refs[L0]; i++)
                        sh->list_entry_lx[0][i] = get_bits(gb, av_ceil_log2(nb_refs));
                }

                if (sh->slice_type == B_SLICE) {
                    sh->rpl_modification_flag[1] = get_bits1(gb);
                    if (sh->rpl_modification_flag[1] == 1)
                        for (i = 0; i < sh->nb_refs[L1]; i++)
                            sh->list_entry_lx[1][i] = get_bits(gb, av_ceil_log2(nb_refs));
                }
            }

            if (sh->slice_type == B_SLICE)
                sh->mvd_l1_zero_flag = get_bits1(gb);

            if (s->ps.pps->cabac_init_present_flag)
                sh->cabac_init_flag = get_bits1(gb);
            else
                sh->cabac_init_flag = 0;

            sh->collocated_ref_idx = 0;
            if (sh->slice_temporal_mvp_enabled_flag) {
                sh->collocated_list = L0;
                if (sh->slice_type == B_SLICE)
                    sh->collocated_list = !get_bits1(gb);

                if (sh->nb_refs[sh->collocated_list] > 1) {
                    sh->collocated_ref_idx = get_ue_golomb_long(gb);
                    if (sh->collocated_ref_idx >= sh->nb_refs[sh->collocated_list]) {
                        av_log(s->avctx, AV_LOG_ERROR,
                               "Invalid collocated_ref_idx: %d.\n",
                               sh->collocated_ref_idx);
                        return AVERROR_INVALIDDATA;
                    }
                }
            }

            if ((s->ps.pps->weighted_pred_flag   && sh->slice_type == P_SLICE) ||
                (s->ps.pps->weighted_bipred_flag && sh->slice_type == B_SLICE)) {
                pred_weight_table(s, gb);
            }

            sh->max_num_merge_cand = 5 - get_ue_golomb_long(gb);
            if (sh->max_num_merge_cand < 1 || sh->max_num_merge_cand > 5) {
                av_log(s->avctx, AV_LOG_ERROR,
                       "Invalid number of merging MVP candidates: %d.\n",
                       sh->max_num_merge_cand);
                return AVERROR_INVALIDDATA;
            }
        }

        sh->slice_qp_delta = get_se_golomb(gb);

        if (s->ps.pps->pic_slice_level_chroma_qp_offsets_present_flag) {
            sh->slice_cb_qp_offset = get_se_golomb(gb);
            sh->slice_cr_qp_offset = get_se_golomb(gb);
        } else {
            sh->slice_cb_qp_offset = 0;
            sh->slice_cr_qp_offset = 0;
        }

        if (s->ps.pps->chroma_qp_offset_list_enabled_flag)
            sh->cu_chroma_qp_offset_enabled_flag = get_bits1(gb);
        else
            sh->cu_chroma_qp_offset_enabled_flag = 0;

        if (s->ps.pps->deblocking_filter_control_present_flag) {
            int deblocking_filter_override_flag = 0;

            if (s->ps.pps->deblocking_filter_override_enabled_flag)
                deblocking_filter_override_flag = get_bits1(gb);

            if (deblocking_filter_override_flag) {
                sh->disable_deblocking_filter_flag = get_bits1(gb);
                if (!sh->disable_deblocking_filter_flag) {
                    sh->beta_offset = get_se_golomb(gb) * 2;
                    sh->tc_offset   = get_se_golomb(gb) * 2;
                }
            } else {
                sh->disable_deblocking_filter_flag = s->ps.pps->disable_dbf;
                sh->beta_offset                    = s->ps.pps->beta_offset;
                sh->tc_offset                      = s->ps.pps->tc_offset;
            }
        } else {
            sh->disable_deblocking_filter_flag = 0;
            sh->beta_offset                    = 0;
            sh->tc_offset                      = 0;
        }

        if (s->ps.pps->seq_loop_filter_across_slices_enabled_flag &&
            (sh->slice_sample_adaptive_offset_flag[0] ||
             sh->slice_sample_adaptive_offset_flag[1] ||
             !sh->disable_deblocking_filter_flag)) {
            sh->slice_loop_filter_across_slices_enabled_flag = get_bits1(gb);
        } else {
            sh->slice_loop_filter_across_slices_enabled_flag = s->ps.pps->seq_loop_filter_across_slices_enabled_flag;
        }
    } else if (!s->slice_initialized) {
        av_log(s->avctx, AV_LOG_ERROR, "Independent slice segment missing.\n");
        return AVERROR_INVALIDDATA;
    }

    sh->num_entry_point_offsets = 0;
    if (s->ps.pps->tiles_enabled_flag || s->ps.pps->entropy_coding_sync_enabled_flag) {
        unsigned num_entry_point_offsets = get_ue_golomb_long(gb);
        // It would be possible to bound this tighter but this here is simpler
        if (num_entry_point_offsets > get_bits_left(gb)) {
            av_log(s->avctx, AV_LOG_ERROR, "num_entry_point_offsets %d is invalid\n", num_entry_point_offsets);
            return AVERROR_INVALIDDATA;
        }

        sh->num_entry_point_offsets = num_entry_point_offsets;
        if (sh->num_entry_point_offsets > 0) {
            int offset_len = get_ue_golomb_long(gb) + 1;

            if (offset_len < 1 || offset_len > 32) {
                sh->num_entry_point_offsets = 0;
                av_log(s->avctx, AV_LOG_ERROR, "offset_len %d is invalid\n", offset_len);
                return AVERROR_INVALIDDATA;
            }

            av_freep(&sh->entry_point_offset);
            av_freep(&sh->offset);
            av_freep(&sh->size);
            sh->entry_point_offset = av_malloc_array(sh->num_entry_point_offsets, sizeof(unsigned));
            sh->offset = av_malloc_array(sh->num_entry_point_offsets, sizeof(int));
            sh->size = av_malloc_array(sh->num_entry_point_offsets, sizeof(int));
            if (!sh->entry_point_offset || !sh->offset || !sh->size) {
                sh->num_entry_point_offsets = 0;
                av_log(s->avctx, AV_LOG_ERROR, "Failed to allocate memory\n");
                return AVERROR(ENOMEM);
            }
            for (i = 0; i < sh->num_entry_point_offsets; i++) {
                unsigned val = get_bits_long(gb, offset_len);
                sh->entry_point_offset[i] = val + 1; // +1; // +1 to get the size
            }
            if (s->threads_number > 1 && (s->ps.pps->num_tile_rows > 1 || s->ps.pps->num_tile_columns > 1)) {
                s->enable_parallel_tiles = 0; // TODO: you can enable tiles in parallel here
                s->threads_number = 1;
            } else
                s->enable_parallel_tiles = 0;
        } else
            s->enable_parallel_tiles = 0;
    }

    if (s->ps.pps->slice_header_extension_present_flag) {
        unsigned int length = get_ue_golomb_long(gb);
        if (length*8LL > get_bits_left(gb)) {
            av_log(s->avctx, AV_LOG_ERROR, "too many slice_header_extension_data_bytes\n");
            return AVERROR_INVALIDDATA;
        }
        for (i = 0; i < length; i++)
            skip_bits(gb, 8);  // slice_header_extension_data_byte
    }

    // Inferred parameters
    sh->slice_qp = 26U + s->ps.pps->pic_init_qp_minus26 + sh->slice_qp_delta;
    if (sh->slice_qp > 51 ||
        sh->slice_qp < -s->ps.sps->qp_bd_offset) {
        av_log(s->avctx, AV_LOG_ERROR,
               "The slice_qp %d is outside the valid range "
               "[%d, 51].\n",
               sh->slice_qp,
               -s->ps.sps->qp_bd_offset);
        return AVERROR_INVALIDDATA;
    }

    sh->slice_ctb_addr_rs = sh->slice_segment_addr;

    if (!s->sh.slice_ctb_addr_rs && s->sh.dependent_slice_segment_flag) {
        av_log(s->avctx, AV_LOG_ERROR, "Impossible slice segment.\n");
        return AVERROR_INVALIDDATA;
    }

    if (get_bits_left(gb) < 0) {
        av_log(s->avctx, AV_LOG_ERROR,
               "Overread slice header by %d bits\n", -get_bits_left(gb));
        return AVERROR_INVALIDDATA;
    }

    s->HEVClc->first_qp_group = !s->sh.dependent_slice_segment_flag;

    if (!s->ps.pps->cu_qp_delta_enabled_flag)
        s->HEVClc->qp_y = s->sh.slice_qp;

    s->slice_initialized = 1;
    s->HEVClc->tu.cu_qp_offset_cb = 0;
    s->HEVClc->tu.cu_qp_offset_cr = 0;

    s->no_rasl_output_flag = IS_IDR(s) || IS_BLA(s) || (s->nal_unit_type == NAL_CRA_NUT && s->last_eos);

    return 0;
}

#define CTB(tab, x, y) ((tab)[(y) * s->ps.sps->ctb_width + (x)])

#define SET_SAO(elem, value)                            \
do {                                                    \
    if (!sao_merge_up_flag && !sao_merge_left_flag)     \
        sao->elem = value;                              \
    else if (sao_merge_left_flag)                       \
        sao->elem = CTB(s->sao, rx-1, ry).elem;         \
    else if (sao_merge_up_flag)                         \
        sao->elem = CTB(s->sao, rx, ry-1).elem;         \
    else                                                \
        sao->elem = 0;                                  \
} while (0)

static void hls_sao_param(HEVCContext *s, int rx, int ry)
{
    HEVCLocalContext *lc    = s->HEVClc;
    int sao_merge_left_flag = 0;
    int sao_merge_up_flag   = 0;
    SAOParams *sao          = &CTB(s->sao, rx, ry);
    int c_idx, i;

    if (s->sh.slice_sample_adaptive_offset_flag[0] ||
        s->sh.slice_sample_adaptive_offset_flag[1]) {
        if (rx > 0) {
            if (lc->ctb_left_flag)
                sao_merge_left_flag = ff_hevc_sao_merge_flag_decode(s);
        }
        if (ry > 0 && !sao_merge_left_flag) {
            if (lc->ctb_up_flag)
                sao_merge_up_flag = ff_hevc_sao_merge_flag_decode(s);
        }
    }

    for (c_idx = 0; c_idx < (s->ps.sps->chroma_format_idc ? 3 : 1); c_idx++) {
        int log2_sao_offset_scale = c_idx == 0 ? s->ps.pps->log2_sao_offset_scale_luma :
                                                 s->ps.pps->log2_sao_offset_scale_chroma;

        if (!s->sh.slice_sample_adaptive_offset_flag[c_idx]) {
            sao->type_idx[c_idx] = SAO_NOT_APPLIED;
            continue;
        }

        if (c_idx == 2) {
            sao->type_idx[2] = sao->type_idx[1];
            sao->eo_class[2] = sao->eo_class[1];
        } else {
            SET_SAO(type_idx[c_idx], ff_hevc_sao_type_idx_decode(s));
        }

        if (sao->type_idx[c_idx] == SAO_NOT_APPLIED)
            continue;

        for (i = 0; i < 4; i++)
            SET_SAO(offset_abs[c_idx][i], ff_hevc_sao_offset_abs_decode(s));

        if (sao->type_idx[c_idx] == SAO_BAND) {
            for (i = 0; i < 4; i++) {
                if (sao->offset_abs[c_idx][i]) {
                    SET_SAO(offset_sign[c_idx][i],
                            ff_hevc_sao_offset_sign_decode(s));
                } else {
                    sao->offset_sign[c_idx][i] = 0;
                }
            }
            SET_SAO(band_position[c_idx], ff_hevc_sao_band_position_decode(s));
        } else if (c_idx != 2) {
            SET_SAO(eo_class[c_idx], ff_hevc_sao_eo_class_decode(s));
        }

        // Inferred parameters
        sao->offset_val[c_idx][0] = 0;
        for (i = 0; i < 4; i++) {
            sao->offset_val[c_idx][i + 1] = sao->offset_abs[c_idx][i];
            if (sao->type_idx[c_idx] == SAO_EDGE) {
                if (i > 1)
                    sao->offset_val[c_idx][i + 1] = -sao->offset_val[c_idx][i + 1];
            } else if (sao->offset_sign[c_idx][i]) {
                sao->offset_val[c_idx][i + 1] = -sao->offset_val[c_idx][i + 1];
            }
            sao->offset_val[c_idx][i + 1] *= 1 << log2_sao_offset_scale;
        }
    }
}

#undef SET_SAO
#undef CTB

static int hls_cross_component_pred(HEVCContext *s, int idx) {
    HEVCLocalContext *lc    = s->HEVClc;
    int log2_res_scale_abs_plus1 = ff_hevc_log2_res_scale_abs(s, idx);

    if (log2_res_scale_abs_plus1 !=  0) {
        int res_scale_sign_flag = ff_hevc_res_scale_sign_flag(s, idx);
        lc->tu.res_scale_val = (1 << (log2_res_scale_abs_plus1 - 1)) *
                               (1 - 2 * res_scale_sign_flag);
    } else {
        lc->tu.res_scale_val = 0;
    }


    return 0;
}

#ifdef RPI
static void rpi_intra_pred(HEVCContext *s, int log2_trafo_size, int x0, int y0, int c_idx)
{
    if (s->enable_rpi) {
        HEVCLocalContext *lc = s->HEVClc;
        HEVCPredCmd *cmd = s->univ_pred_cmds[s->pass0_job] + s->num_pred_cmds[s->pass0_job]++;
        cmd->type = RPI_PRED_INTRA;
        cmd->size = log2_trafo_size;
        cmd->c_idx = c_idx;
        cmd->x = x0;
        cmd->y = y0;
        cmd->na = (lc->na.cand_bottom_left<<4) + (lc->na.cand_left<<3) + (lc->na.cand_up_left<<2) + (lc->na.cand_up<<1) + lc->na.cand_up_right;
        cmd->mode = c_idx ? lc->tu.intra_pred_mode_c :  lc->tu.intra_pred_mode;
    } else {
        s->hpc.intra_pred[log2_trafo_size - 2](s, x0, y0, c_idx);
    }
}
#endif

static int hls_transform_unit(HEVCContext *s, int x0, int y0,
                              int xBase, int yBase, int cb_xBase, int cb_yBase,
                              int log2_cb_size, int log2_trafo_size,
                              int blk_idx, int cbf_luma, int *cbf_cb, int *cbf_cr)
{
    HEVCLocalContext *lc = s->HEVClc;
    const int log2_trafo_size_c = log2_trafo_size - s->ps.sps->hshift[1];
    int i;

    if (lc->cu.pred_mode == MODE_INTRA) {
        int trafo_size = 1 << log2_trafo_size;
        ff_hevc_set_neighbour_available(s, x0, y0, trafo_size, trafo_size);
#ifdef RPI
        rpi_intra_pred(s, log2_trafo_size, x0, y0, 0);
#else
        s->hpc.intra_pred[log2_trafo_size - 2](s, x0, y0, 0);
#endif
    }

    if (cbf_luma || cbf_cb[0] || cbf_cr[0] ||
        (s->ps.sps->chroma_format_idc == 2 && (cbf_cb[1] || cbf_cr[1]))) {
        int scan_idx   = SCAN_DIAG;
        int scan_idx_c = SCAN_DIAG;
        int cbf_chroma = cbf_cb[0] || cbf_cr[0] ||
                         (s->ps.sps->chroma_format_idc == 2 &&
                         (cbf_cb[1] || cbf_cr[1]));

        if (s->ps.pps->cu_qp_delta_enabled_flag && !lc->tu.is_cu_qp_delta_coded) {
            lc->tu.cu_qp_delta = ff_hevc_cu_qp_delta_abs(s);
            if (lc->tu.cu_qp_delta != 0)
                if (ff_hevc_cu_qp_delta_sign_flag(s) == 1)
                    lc->tu.cu_qp_delta = -lc->tu.cu_qp_delta;
            lc->tu.is_cu_qp_delta_coded = 1;

            if (lc->tu.cu_qp_delta < -(26 + s->ps.sps->qp_bd_offset / 2) ||
                lc->tu.cu_qp_delta >  (25 + s->ps.sps->qp_bd_offset / 2)) {
                av_log(s->avctx, AV_LOG_ERROR,
                       "The cu_qp_delta %d is outside the valid range "
                       "[%d, %d].\n",
                       lc->tu.cu_qp_delta,
                       -(26 + s->ps.sps->qp_bd_offset / 2),
                        (25 + s->ps.sps->qp_bd_offset / 2));
                return AVERROR_INVALIDDATA;
            }

            ff_hevc_set_qPy(s, cb_xBase, cb_yBase, log2_cb_size);
        }

        if (s->sh.cu_chroma_qp_offset_enabled_flag && cbf_chroma &&
            !lc->cu.cu_transquant_bypass_flag  &&  !lc->tu.is_cu_chroma_qp_offset_coded) {
            int cu_chroma_qp_offset_flag = ff_hevc_cu_chroma_qp_offset_flag(s);
            if (cu_chroma_qp_offset_flag) {
                int cu_chroma_qp_offset_idx  = 0;
                if (s->ps.pps->chroma_qp_offset_list_len_minus1 > 0) {
                    cu_chroma_qp_offset_idx = ff_hevc_cu_chroma_qp_offset_idx(s);
                    av_log(s->avctx, AV_LOG_ERROR,
                        "cu_chroma_qp_offset_idx not yet tested.\n");
                }
                lc->tu.cu_qp_offset_cb = s->ps.pps->cb_qp_offset_list[cu_chroma_qp_offset_idx];
                lc->tu.cu_qp_offset_cr = s->ps.pps->cr_qp_offset_list[cu_chroma_qp_offset_idx];
            } else {
                lc->tu.cu_qp_offset_cb = 0;
                lc->tu.cu_qp_offset_cr = 0;
            }
            lc->tu.is_cu_chroma_qp_offset_coded = 1;
        }

        if (lc->cu.pred_mode == MODE_INTRA && log2_trafo_size < 4) {
            if (lc->tu.intra_pred_mode >= 6 &&
                lc->tu.intra_pred_mode <= 14) {
                scan_idx = SCAN_VERT;
            } else if (lc->tu.intra_pred_mode >= 22 &&
                       lc->tu.intra_pred_mode <= 30) {
                scan_idx = SCAN_HORIZ;
            }

            if (lc->tu.intra_pred_mode_c >=  6 &&
                lc->tu.intra_pred_mode_c <= 14) {
                scan_idx_c = SCAN_VERT;
            } else if (lc->tu.intra_pred_mode_c >= 22 &&
                       lc->tu.intra_pred_mode_c <= 30) {
                scan_idx_c = SCAN_HORIZ;
            }
        }

        lc->tu.cross_pf = 0;

        if (cbf_luma)
            ff_hevc_hls_residual_coding(s, x0, y0, log2_trafo_size, scan_idx, 0);
        if (s->ps.sps->chroma_format_idc && (log2_trafo_size > 2 || s->ps.sps->chroma_format_idc == 3)) {
            int trafo_size_h = 1 << (log2_trafo_size_c + s->ps.sps->hshift[1]);
            int trafo_size_v = 1 << (log2_trafo_size_c + s->ps.sps->vshift[1]);
            lc->tu.cross_pf  = (s->ps.pps->cross_component_prediction_enabled_flag && cbf_luma &&
                                (lc->cu.pred_mode == MODE_INTER ||
                                 (lc->tu.chroma_mode_c ==  4)));

            if (lc->tu.cross_pf) {
                hls_cross_component_pred(s, 0);
            }
            for (i = 0; i < (s->ps.sps->chroma_format_idc == 2 ? 2 : 1); i++) {
                if (lc->cu.pred_mode == MODE_INTRA) {
                    ff_hevc_set_neighbour_available(s, x0, y0 + (i << log2_trafo_size_c), trafo_size_h, trafo_size_v);
#ifdef RPI
                    rpi_intra_pred(s, log2_trafo_size_c, x0, y0 + (i << log2_trafo_size_c), 1);
#else
                    s->hpc.intra_pred[log2_trafo_size_c - 2](s, x0, y0 + (i << log2_trafo_size_c), 1);
#endif
                }
                if (cbf_cb[i])
                    ff_hevc_hls_residual_coding(s, x0, y0 + (i << log2_trafo_size_c),
                                                log2_trafo_size_c, scan_idx_c, 1);
                else
                    if (lc->tu.cross_pf) {
                        ptrdiff_t stride = s->frame->linesize[1];
                        int hshift = s->ps.sps->hshift[1];
                        int vshift = s->ps.sps->vshift[1];
                        int16_t *coeffs_y = (int16_t*)lc->edge_emu_buffer;
                        int16_t *coeffs   = (int16_t*)lc->edge_emu_buffer2;
                        int size = 1 << log2_trafo_size_c;

                        uint8_t *dst = &s->frame->data[1][(y0 >> vshift) * stride +
                                                              ((x0 >> hshift) << s->ps.sps->pixel_shift)];
                        for (i = 0; i < (size * size); i++) {
                            coeffs[i] = ((lc->tu.res_scale_val * coeffs_y[i]) >> 3);
                        }
                        s->hevcdsp.transform_add[log2_trafo_size_c-2](dst, coeffs, stride);
                    }
            }

            if (lc->tu.cross_pf) {
                hls_cross_component_pred(s, 1);
            }
            for (i = 0; i < (s->ps.sps->chroma_format_idc == 2 ? 2 : 1); i++) {
                if (lc->cu.pred_mode == MODE_INTRA) {
                    ff_hevc_set_neighbour_available(s, x0, y0 + (i << log2_trafo_size_c), trafo_size_h, trafo_size_v);
#ifdef RPI
                    rpi_intra_pred(s, log2_trafo_size_c, x0, y0 + (i << log2_trafo_size_c), 2);
#else
                    s->hpc.intra_pred[log2_trafo_size_c - 2](s, x0, y0 + (i << log2_trafo_size_c), 2);
#endif
                }
                if (cbf_cr[i])
                    ff_hevc_hls_residual_coding(s, x0, y0 + (i << log2_trafo_size_c),
                                                log2_trafo_size_c, scan_idx_c, 2);
                else
                    if (lc->tu.cross_pf) {
                        ptrdiff_t stride = s->frame->linesize[2];
                        int hshift = s->ps.sps->hshift[2];
                        int vshift = s->ps.sps->vshift[2];
                        int16_t *coeffs_y = (int16_t*)lc->edge_emu_buffer;
                        int16_t *coeffs   = (int16_t*)lc->edge_emu_buffer2;
                        int size = 1 << log2_trafo_size_c;

                        uint8_t *dst = &s->frame->data[2][(y0 >> vshift) * stride +
                                                          ((x0 >> hshift) << s->ps.sps->pixel_shift)];
                        for (i = 0; i < (size * size); i++) {
                            coeffs[i] = ((lc->tu.res_scale_val * coeffs_y[i]) >> 3);
                        }
                        s->hevcdsp.transform_add[log2_trafo_size_c-2](dst, coeffs, stride);
                    }
            }
        } else if (s->ps.sps->chroma_format_idc && blk_idx == 3) {
            int trafo_size_h = 1 << (log2_trafo_size + 1);
            int trafo_size_v = 1 << (log2_trafo_size + s->ps.sps->vshift[1]);
            for (i = 0; i < (s->ps.sps->chroma_format_idc == 2 ? 2 : 1); i++) {
                if (lc->cu.pred_mode == MODE_INTRA) {
                    ff_hevc_set_neighbour_available(s, xBase, yBase + (i << log2_trafo_size),
                                                    trafo_size_h, trafo_size_v);
#ifdef RPI
                    rpi_intra_pred(s, log2_trafo_size, xBase, yBase + (i << log2_trafo_size), 1);
#else
                    s->hpc.intra_pred[log2_trafo_size - 2](s, xBase, yBase + (i << log2_trafo_size), 1);
#endif
                }
                if (cbf_cb[i])
                    ff_hevc_hls_residual_coding(s, xBase, yBase + (i << log2_trafo_size),
                                                log2_trafo_size, scan_idx_c, 1);
            }
            for (i = 0; i < (s->ps.sps->chroma_format_idc == 2 ? 2 : 1); i++) {
                if (lc->cu.pred_mode == MODE_INTRA) {
                    ff_hevc_set_neighbour_available(s, xBase, yBase + (i << log2_trafo_size),
                                                trafo_size_h, trafo_size_v);
#ifdef RPI
                    rpi_intra_pred(s, log2_trafo_size, xBase, yBase + (i << log2_trafo_size), 2);
#else
                    s->hpc.intra_pred[log2_trafo_size - 2](s, xBase, yBase + (i << log2_trafo_size), 2);
#endif
                }
                if (cbf_cr[i])
                    ff_hevc_hls_residual_coding(s, xBase, yBase + (i << log2_trafo_size),
                                                log2_trafo_size, scan_idx_c, 2);
            }
        }
    } else if (s->ps.sps->chroma_format_idc && lc->cu.pred_mode == MODE_INTRA) {
        if (log2_trafo_size > 2 || s->ps.sps->chroma_format_idc == 3) {
            int trafo_size_h = 1 << (log2_trafo_size_c + s->ps.sps->hshift[1]);
            int trafo_size_v = 1 << (log2_trafo_size_c + s->ps.sps->vshift[1]);
            ff_hevc_set_neighbour_available(s, x0, y0, trafo_size_h, trafo_size_v);
#ifdef RPI
            rpi_intra_pred(s, log2_trafo_size_c, x0, y0, 1);
            rpi_intra_pred(s, log2_trafo_size_c, x0, y0, 2);
#else
            s->hpc.intra_pred[log2_trafo_size_c - 2](s, x0, y0, 1);
            s->hpc.intra_pred[log2_trafo_size_c - 2](s, x0, y0, 2);
#endif
            if (s->ps.sps->chroma_format_idc == 2) {
                ff_hevc_set_neighbour_available(s, x0, y0 + (1 << log2_trafo_size_c),
                                                trafo_size_h, trafo_size_v);
#ifdef RPI
                rpi_intra_pred(s, log2_trafo_size_c, x0, y0 + (1 << log2_trafo_size_c), 1);
                rpi_intra_pred(s, log2_trafo_size_c, x0, y0 + (1 << log2_trafo_size_c), 2);
#else
                s->hpc.intra_pred[log2_trafo_size_c - 2](s, x0, y0 + (1 << log2_trafo_size_c), 1);
                s->hpc.intra_pred[log2_trafo_size_c - 2](s, x0, y0 + (1 << log2_trafo_size_c), 2);
#endif
            }
        } else if (blk_idx == 3) {
            int trafo_size_h = 1 << (log2_trafo_size + 1);
            int trafo_size_v = 1 << (log2_trafo_size + s->ps.sps->vshift[1]);
            ff_hevc_set_neighbour_available(s, xBase, yBase,
                                            trafo_size_h, trafo_size_v);
#ifdef RPI
            rpi_intra_pred(s, log2_trafo_size, xBase, yBase, 1);
            rpi_intra_pred(s, log2_trafo_size, xBase, yBase, 2);
#else
            s->hpc.intra_pred[log2_trafo_size - 2](s, xBase, yBase, 1);
            s->hpc.intra_pred[log2_trafo_size - 2](s, xBase, yBase, 2);
#endif
            if (s->ps.sps->chroma_format_idc == 2) {
                ff_hevc_set_neighbour_available(s, xBase, yBase + (1 << (log2_trafo_size)),
                                                trafo_size_h, trafo_size_v);
#ifdef RPI
                rpi_intra_pred(s, log2_trafo_size, xBase, yBase + (1 << (log2_trafo_size)), 1);
                rpi_intra_pred(s, log2_trafo_size, xBase, yBase + (1 << (log2_trafo_size)), 2);
#else
                s->hpc.intra_pred[log2_trafo_size - 2](s, xBase, yBase + (1 << (log2_trafo_size)), 1);
                s->hpc.intra_pred[log2_trafo_size - 2](s, xBase, yBase + (1 << (log2_trafo_size)), 2);
#endif
            }
        }
    }

    return 0;
}

static void set_deblocking_bypass(HEVCContext *s, int x0, int y0, int log2_cb_size)
{
    int cb_size          = 1 << log2_cb_size;
    int log2_min_pu_size = s->ps.sps->log2_min_pu_size;

    int min_pu_width     = s->ps.sps->min_pu_width;
    int x_end = FFMIN(x0 + cb_size, s->ps.sps->width);
    int y_end = FFMIN(y0 + cb_size, s->ps.sps->height);
    int i, j;

    for (j = (y0 >> log2_min_pu_size); j < (y_end >> log2_min_pu_size); j++)
        for (i = (x0 >> log2_min_pu_size); i < (x_end >> log2_min_pu_size); i++)
            s->is_pcm[i + j * min_pu_width] = 2;
}

static int hls_transform_tree(HEVCContext *s, int x0, int y0,
                              int xBase, int yBase, int cb_xBase, int cb_yBase,
                              int log2_cb_size, int log2_trafo_size,
                              int trafo_depth, int blk_idx,
                              const int *base_cbf_cb, const int *base_cbf_cr)
{
    HEVCLocalContext *lc = s->HEVClc;
    uint8_t split_transform_flag;
    int cbf_cb[2];
    int cbf_cr[2];
    int ret;

    cbf_cb[0] = base_cbf_cb[0];
    cbf_cb[1] = base_cbf_cb[1];
    cbf_cr[0] = base_cbf_cr[0];
    cbf_cr[1] = base_cbf_cr[1];

    if (lc->cu.intra_split_flag) {
        if (trafo_depth == 1) {
            lc->tu.intra_pred_mode   = lc->pu.intra_pred_mode[blk_idx];
            if (s->ps.sps->chroma_format_idc == 3) {
                lc->tu.intra_pred_mode_c = lc->pu.intra_pred_mode_c[blk_idx];
                lc->tu.chroma_mode_c     = lc->pu.chroma_mode_c[blk_idx];
            } else {
                lc->tu.intra_pred_mode_c = lc->pu.intra_pred_mode_c[0];
                lc->tu.chroma_mode_c     = lc->pu.chroma_mode_c[0];
            }
        }
    } else {
        lc->tu.intra_pred_mode   = lc->pu.intra_pred_mode[0];
        lc->tu.intra_pred_mode_c = lc->pu.intra_pred_mode_c[0];
        lc->tu.chroma_mode_c     = lc->pu.chroma_mode_c[0];
    }

    if (log2_trafo_size <= s->ps.sps->log2_max_trafo_size &&
        log2_trafo_size >  s->ps.sps->log2_min_tb_size    &&
        trafo_depth     < lc->cu.max_trafo_depth       &&
        !(lc->cu.intra_split_flag && trafo_depth == 0)) {
        split_transform_flag = ff_hevc_split_transform_flag_decode(s, log2_trafo_size);
    } else {
        int inter_split = s->ps.sps->max_transform_hierarchy_depth_inter == 0 &&
                          lc->cu.pred_mode == MODE_INTER &&
                          lc->cu.part_mode != PART_2Nx2N &&
                          trafo_depth == 0;

        split_transform_flag = log2_trafo_size > s->ps.sps->log2_max_trafo_size ||
                               (lc->cu.intra_split_flag && trafo_depth == 0) ||
                               inter_split;
    }

    if (s->ps.sps->chroma_format_idc && (log2_trafo_size > 2 || s->ps.sps->chroma_format_idc == 3)) {
        if (trafo_depth == 0 || cbf_cb[0]) {
            cbf_cb[0] = ff_hevc_cbf_cb_cr_decode(s, trafo_depth);
            if (s->ps.sps->chroma_format_idc == 2 && (!split_transform_flag || log2_trafo_size == 3)) {
                cbf_cb[1] = ff_hevc_cbf_cb_cr_decode(s, trafo_depth);
            }
        }

        if (trafo_depth == 0 || cbf_cr[0]) {
            cbf_cr[0] = ff_hevc_cbf_cb_cr_decode(s, trafo_depth);
            if (s->ps.sps->chroma_format_idc == 2 && (!split_transform_flag || log2_trafo_size == 3)) {
                cbf_cr[1] = ff_hevc_cbf_cb_cr_decode(s, trafo_depth);
            }
        }
    }

    if (split_transform_flag) {
        const int trafo_size_split = 1 << (log2_trafo_size - 1);
        const int x1 = x0 + trafo_size_split;
        const int y1 = y0 + trafo_size_split;

#define SUBDIVIDE(x, y, idx)                                                    \
do {                                                                            \
    ret = hls_transform_tree(s, x, y, x0, y0, cb_xBase, cb_yBase, log2_cb_size, \
                             log2_trafo_size - 1, trafo_depth + 1, idx,         \
                             cbf_cb, cbf_cr);                                   \
    if (ret < 0)                                                                \
        return ret;                                                             \
} while (0)

        SUBDIVIDE(x0, y0, 0);
        SUBDIVIDE(x1, y0, 1);
        SUBDIVIDE(x0, y1, 2);
        SUBDIVIDE(x1, y1, 3);

#undef SUBDIVIDE
    } else {
        int min_tu_size      = 1 << s->ps.sps->log2_min_tb_size;
        int log2_min_tu_size = s->ps.sps->log2_min_tb_size;
        int min_tu_width     = s->ps.sps->min_tb_width;
        int cbf_luma         = 1;

        if (lc->cu.pred_mode == MODE_INTRA || trafo_depth != 0 ||
            cbf_cb[0] || cbf_cr[0] ||
            (s->ps.sps->chroma_format_idc == 2 && (cbf_cb[1] || cbf_cr[1]))) {
            cbf_luma = ff_hevc_cbf_luma_decode(s, trafo_depth);
        }

        ret = hls_transform_unit(s, x0, y0, xBase, yBase, cb_xBase, cb_yBase,
                                 log2_cb_size, log2_trafo_size,
                                 blk_idx, cbf_luma, cbf_cb, cbf_cr);
        if (ret < 0)
            return ret;
        // TODO: store cbf_luma somewhere else
        if (cbf_luma) {
            int i, j;
            for (i = 0; i < (1 << log2_trafo_size); i += min_tu_size)
                for (j = 0; j < (1 << log2_trafo_size); j += min_tu_size) {
                    int x_tu = (x0 + j) >> log2_min_tu_size;
                    int y_tu = (y0 + i) >> log2_min_tu_size;
                    s->cbf_luma[y_tu * min_tu_width + x_tu] = 1;
                }
        }
        if (!s->sh.disable_deblocking_filter_flag) {
            ff_hevc_deblocking_boundary_strengths(s, x0, y0, log2_trafo_size);
            if (s->ps.pps->transquant_bypass_enable_flag &&
                lc->cu.cu_transquant_bypass_flag)
                set_deblocking_bypass(s, x0, y0, log2_trafo_size);
        }
    }
    return 0;
}

static int hls_pcm_sample(HEVCContext *s, int x0, int y0, int log2_cb_size)
{
    HEVCLocalContext *lc = s->HEVClc;
    GetBitContext gb;
    int cb_size   = 1 << log2_cb_size;
    int stride0   = s->frame->linesize[0];
    uint8_t *dst0 = &s->frame->data[0][y0 * stride0 + (x0 << s->ps.sps->pixel_shift)];
    int   stride1 = s->frame->linesize[1];
    uint8_t *dst1 = &s->frame->data[1][(y0 >> s->ps.sps->vshift[1]) * stride1 + ((x0 >> s->ps.sps->hshift[1]) << s->ps.sps->pixel_shift)];
    int   stride2 = s->frame->linesize[2];
    uint8_t *dst2 = &s->frame->data[2][(y0 >> s->ps.sps->vshift[2]) * stride2 + ((x0 >> s->ps.sps->hshift[2]) << s->ps.sps->pixel_shift)];

    int length         = cb_size * cb_size * s->ps.sps->pcm.bit_depth +
                         (((cb_size >> s->ps.sps->hshift[1]) * (cb_size >> s->ps.sps->vshift[1])) +
                          ((cb_size >> s->ps.sps->hshift[2]) * (cb_size >> s->ps.sps->vshift[2]))) *
                          s->ps.sps->pcm.bit_depth_chroma;
    const uint8_t *pcm = skip_bytes(&lc->cc, (length + 7) >> 3);
    int ret;

    if (!s->sh.disable_deblocking_filter_flag)
        ff_hevc_deblocking_boundary_strengths(s, x0, y0, log2_cb_size);

    ret = init_get_bits(&gb, pcm, length);
    if (ret < 0)
        return ret;

    s->hevcdsp.put_pcm(dst0, stride0, cb_size, cb_size,     &gb, s->ps.sps->pcm.bit_depth);
    if (s->ps.sps->chroma_format_idc) {
        s->hevcdsp.put_pcm(dst1, stride1,
                           cb_size >> s->ps.sps->hshift[1],
                           cb_size >> s->ps.sps->vshift[1],
                           &gb, s->ps.sps->pcm.bit_depth_chroma);
        s->hevcdsp.put_pcm(dst2, stride2,
                           cb_size >> s->ps.sps->hshift[2],
                           cb_size >> s->ps.sps->vshift[2],
                           &gb, s->ps.sps->pcm.bit_depth_chroma);
    }

    return 0;
}

/**
 * 8.5.3.2.2.1 Luma sample unidirectional interpolation process
 *
 * @param s HEVC decoding context
 * @param dst target buffer for block data at block position
 * @param dststride stride of the dst buffer
 * @param ref reference picture buffer at origin (0, 0)
 * @param mv motion vector (relative to block position) to get pixel data from
 * @param x_off horizontal position of block from origin (0, 0)
 * @param y_off vertical position of block from origin (0, 0)
 * @param block_w width of block
 * @param block_h height of block
 * @param luma_weight weighting factor applied to the luma prediction
 * @param luma_offset additive offset applied to the luma prediction value
 */

#ifdef RPI_INTER
#define RPI_REDIRECT(fn) (s->enable_rpi ? rpi_ ## fn : fn)
static void rpi_luma_mc_uni(HEVCContext *s, uint8_t *dst, ptrdiff_t dststride,
                        AVFrame *ref, const Mv *mv, int x_off, int y_off,
                        int block_w, int block_h, int luma_weight, int luma_offset)
{
    HEVCMvCmd *cmd = s->unif_mv_cmds[s->pass0_job] + s->num_mv_cmds[s->pass0_job]++;
    cmd->cmd = RPI_CMD_LUMA_UNI;
    cmd->dst = dst;
    cmd->dststride = dststride;
    cmd->src = ref->data[0];
    cmd->srcstride = ref->linesize[0];
    cmd->mv = *mv;
    cmd->x_off = x_off;
    cmd->y_off = y_off;
    cmd->block_w = block_w;
    cmd->block_h = block_h;
    cmd->weight = luma_weight;
    cmd->offset = luma_offset;
}

static void rpi_luma_mc_bi(HEVCContext *s, uint8_t *dst, ptrdiff_t dststride,
                       AVFrame *ref0, const Mv *mv0, int x_off, int y_off,
                       int block_w, int block_h, AVFrame *ref1, const Mv *mv1, struct MvField *current_mv)
{
    HEVCMvCmd *cmd = s->unif_mv_cmds[s->pass0_job] + s->num_mv_cmds[s->pass0_job]++;
    cmd->cmd = RPI_CMD_LUMA_BI;
    cmd->dst = dst;
    cmd->dststride = dststride;
    cmd->src = ref0->data[0];
    cmd->srcstride = ref0->linesize[0];
    cmd->mv = *mv0;
    cmd->x_off = x_off;
    cmd->y_off = y_off;
    cmd->block_w = block_w;
    cmd->block_h = block_h;
    cmd->src1 = ref1->data[0];
    cmd->srcstride1 = ref1->linesize[0];
    cmd->mv1 = *mv1;
    cmd->ref_idx[0] = current_mv->ref_idx[0];
    cmd->ref_idx[1] = current_mv->ref_idx[1];
}

static void rpi_chroma_mc_uni(HEVCContext *s, uint8_t *dst0,
                          ptrdiff_t dststride, uint8_t *src0, ptrdiff_t srcstride, int reflist,
                          int x_off, int y_off, int block_w, int block_h, struct MvField *current_mv, int chroma_weight, int chroma_offset)
{
    HEVCMvCmd *cmd = s->unif_mv_cmds[s->pass0_job] + s->num_mv_cmds[s->pass0_job]++;
    cmd->cmd = RPI_CMD_CHROMA_UNI;
    cmd->dst = dst0;
    cmd->dststride = dststride;
    cmd->src = src0;
    cmd->srcstride = srcstride;
    cmd->mv = current_mv->mv[reflist];
    cmd->x_off = x_off;
    cmd->y_off = y_off;
    cmd->block_w = block_w;
    cmd->block_h = block_h;
    cmd->weight = chroma_weight;
    cmd->offset = chroma_offset;
}

static void rpi_chroma_mc_bi(HEVCContext *s, uint8_t *dst0, ptrdiff_t dststride, AVFrame *ref0, AVFrame *ref1,
                         int x_off, int y_off, int block_w, int block_h, struct MvField *current_mv, int cidx)
{
    HEVCMvCmd *cmd = s->unif_mv_cmds[s->pass0_job] + s->num_mv_cmds[s->pass0_job]++;
    cmd->cmd = RPI_CMD_CHROMA_BI+cidx;
    cmd->dst = dst0;
    cmd->dststride = dststride;
    cmd->src = ref0->data[cidx+1];
    cmd->srcstride = ref0->linesize[cidx+1];
    cmd->mv = current_mv->mv[0];
    cmd->mv1 = current_mv->mv[1];
    cmd->x_off = x_off;
    cmd->y_off = y_off;
    cmd->block_w = block_w;
    cmd->block_h = block_h;
    cmd->src1 = ref1->data[cidx+1];
    cmd->srcstride1 = ref1->linesize[cidx+1];
    cmd->ref_idx[0] = current_mv->ref_idx[0];
    cmd->ref_idx[1] = current_mv->ref_idx[1];
}

#else
#define RPI_REDIRECT(fn) fn
#endif

static void luma_mc_uni(HEVCContext *s, uint8_t *dst, ptrdiff_t dststride,
                        AVFrame *ref, const Mv *mv, int x_off, int y_off,
                        int block_w, int block_h, int luma_weight, int luma_offset)
{
    HEVCLocalContext *lc = s->HEVClc;
    uint8_t *src         = ref->data[0];
    ptrdiff_t srcstride  = ref->linesize[0];
    int pic_width        = s->ps.sps->width;
    int pic_height       = s->ps.sps->height;
    int mx               = mv->x & 3;
    int my               = mv->y & 3;
    int weight_flag      = (s->sh.slice_type == P_SLICE && s->ps.pps->weighted_pred_flag) ||
                           (s->sh.slice_type == B_SLICE && s->ps.pps->weighted_bipred_flag);
    int idx              = ff_hevc_pel_weight[block_w];

#ifdef DISABLE_MC
    return;
#endif

    x_off += mv->x >> 2;
    y_off += mv->y >> 2;
    src   += y_off * srcstride + (x_off * (1 << s->ps.sps->pixel_shift));

    if (x_off < QPEL_EXTRA_BEFORE || y_off < QPEL_EXTRA_AFTER ||
        x_off >= pic_width - block_w - QPEL_EXTRA_AFTER ||
        y_off >= pic_height - block_h - QPEL_EXTRA_AFTER) {
        const int edge_emu_stride = EDGE_EMU_BUFFER_STRIDE << s->ps.sps->pixel_shift;
        int offset     = QPEL_EXTRA_BEFORE * srcstride       + (QPEL_EXTRA_BEFORE << s->ps.sps->pixel_shift);
        int buf_offset = QPEL_EXTRA_BEFORE * edge_emu_stride + (QPEL_EXTRA_BEFORE << s->ps.sps->pixel_shift);

        s->vdsp.emulated_edge_mc(lc->edge_emu_buffer, src - offset,
                                 edge_emu_stride, srcstride,
                                 block_w + QPEL_EXTRA,
                                 block_h + QPEL_EXTRA,
                                 x_off - QPEL_EXTRA_BEFORE, y_off - QPEL_EXTRA_BEFORE,
                                 pic_width, pic_height);
        src = lc->edge_emu_buffer + buf_offset;
        srcstride = edge_emu_stride;
    }

    if (!weight_flag)
        s->hevcdsp.put_hevc_qpel_uni[idx][!!my][!!mx](dst, dststride, src, srcstride,
                                                      block_h, mx, my, block_w);
    else
        s->hevcdsp.put_hevc_qpel_uni_w[idx][!!my][!!mx](dst, dststride, src, srcstride,
                                                        block_h, s->sh.luma_log2_weight_denom,
                                                        luma_weight, luma_offset, mx, my, block_w);
}

/**
 * 8.5.3.2.2.1 Luma sample bidirectional interpolation process
 *
 * @param s HEVC decoding context
 * @param dst target buffer for block data at block position
 * @param dststride stride of the dst buffer
 * @param ref0 reference picture0 buffer at origin (0, 0)
 * @param mv0 motion vector0 (relative to block position) to get pixel data from
 * @param x_off horizontal position of block from origin (0, 0)
 * @param y_off vertical position of block from origin (0, 0)
 * @param block_w width of block
 * @param block_h height of block
 * @param ref1 reference picture1 buffer at origin (0, 0)
 * @param mv1 motion vector1 (relative to block position) to get pixel data from
 * @param current_mv current motion vector structure
 */
static void luma_mc_bi(HEVCContext *s, uint8_t *dst, ptrdiff_t dststride,
                       AVFrame *ref0, const Mv *mv0, int x_off, int y_off,
                       int block_w, int block_h, AVFrame *ref1, const Mv *mv1, struct MvField *current_mv)
{
    HEVCLocalContext *lc = s->HEVClc;
    ptrdiff_t src0stride  = ref0->linesize[0];
    ptrdiff_t src1stride  = ref1->linesize[0];
    int pic_width        = s->ps.sps->width;
    int pic_height       = s->ps.sps->height;
    int mx0              = mv0->x & 3;
    int my0              = mv0->y & 3;
    int mx1              = mv1->x & 3;
    int my1              = mv1->y & 3;
    int weight_flag      = (s->sh.slice_type == P_SLICE && s->ps.pps->weighted_pred_flag) ||
                           (s->sh.slice_type == B_SLICE && s->ps.pps->weighted_bipred_flag);
    int x_off0           = x_off + (mv0->x >> 2);
    int y_off0           = y_off + (mv0->y >> 2);
    int x_off1           = x_off + (mv1->x >> 2);
    int y_off1           = y_off + (mv1->y >> 2);
    int idx              = ff_hevc_pel_weight[block_w];

    uint8_t *src0  = ref0->data[0] + y_off0 * src0stride + (int)((unsigned)x_off0 << s->ps.sps->pixel_shift);
    uint8_t *src1  = ref1->data[0] + y_off1 * src1stride + (int)((unsigned)x_off1 << s->ps.sps->pixel_shift);

#ifdef DISABLE_MC
    return;
#endif

    if (x_off0 < QPEL_EXTRA_BEFORE || y_off0 < QPEL_EXTRA_AFTER ||
        x_off0 >= pic_width - block_w - QPEL_EXTRA_AFTER ||
        y_off0 >= pic_height - block_h - QPEL_EXTRA_AFTER) {
        const int edge_emu_stride = EDGE_EMU_BUFFER_STRIDE << s->ps.sps->pixel_shift;
        int offset     = QPEL_EXTRA_BEFORE * src0stride       + (QPEL_EXTRA_BEFORE << s->ps.sps->pixel_shift);
        int buf_offset = QPEL_EXTRA_BEFORE * edge_emu_stride + (QPEL_EXTRA_BEFORE << s->ps.sps->pixel_shift);

        s->vdsp.emulated_edge_mc(lc->edge_emu_buffer, src0 - offset,
                                 edge_emu_stride, src0stride,
                                 block_w + QPEL_EXTRA,
                                 block_h + QPEL_EXTRA,
                                 x_off0 - QPEL_EXTRA_BEFORE, y_off0 - QPEL_EXTRA_BEFORE,
                                 pic_width, pic_height);
        src0 = lc->edge_emu_buffer + buf_offset;
        src0stride = edge_emu_stride;
    }

    if (x_off1 < QPEL_EXTRA_BEFORE || y_off1 < QPEL_EXTRA_AFTER ||
        x_off1 >= pic_width - block_w - QPEL_EXTRA_AFTER ||
        y_off1 >= pic_height - block_h - QPEL_EXTRA_AFTER) {
        const int edge_emu_stride = EDGE_EMU_BUFFER_STRIDE << s->ps.sps->pixel_shift;
        int offset     = QPEL_EXTRA_BEFORE * src1stride       + (QPEL_EXTRA_BEFORE << s->ps.sps->pixel_shift);
        int buf_offset = QPEL_EXTRA_BEFORE * edge_emu_stride + (QPEL_EXTRA_BEFORE << s->ps.sps->pixel_shift);

        s->vdsp.emulated_edge_mc(lc->edge_emu_buffer2, src1 - offset,
                                 edge_emu_stride, src1stride,
                                 block_w + QPEL_EXTRA,
                                 block_h + QPEL_EXTRA,
                                 x_off1 - QPEL_EXTRA_BEFORE, y_off1 - QPEL_EXTRA_BEFORE,
                                 pic_width, pic_height);
        src1 = lc->edge_emu_buffer2 + buf_offset;
        src1stride = edge_emu_stride;
    }

    s->hevcdsp.put_hevc_qpel[idx][!!my0][!!mx0](lc->tmp, src0, src0stride,
                                                block_h, mx0, my0, block_w);
    if (!weight_flag)
        s->hevcdsp.put_hevc_qpel_bi[idx][!!my1][!!mx1](dst, dststride, src1, src1stride, lc->tmp,
                                                       block_h, mx1, my1, block_w);
    else
        s->hevcdsp.put_hevc_qpel_bi_w[idx][!!my1][!!mx1](dst, dststride, src1, src1stride, lc->tmp,
                                                         block_h, s->sh.luma_log2_weight_denom,
                                                         s->sh.luma_weight_l0[current_mv->ref_idx[0]],
                                                         s->sh.luma_weight_l1[current_mv->ref_idx[1]],
                                                         s->sh.luma_offset_l0[current_mv->ref_idx[0]],
                                                         s->sh.luma_offset_l1[current_mv->ref_idx[1]],
                                                         mx1, my1, block_w);

}

/**
 * 8.5.3.2.2.2 Chroma sample uniprediction interpolation process
 *
 * @param s HEVC decoding context
 * @param dst1 target buffer for block data at block position (U plane)
 * @param dst2 target buffer for block data at block position (V plane)
 * @param dststride stride of the dst1 and dst2 buffers
 * @param ref reference picture buffer at origin (0, 0)
 * @param mv motion vector (relative to block position) to get pixel data from
 * @param x_off horizontal position of block from origin (0, 0)
 * @param y_off vertical position of block from origin (0, 0)
 * @param block_w width of block
 * @param block_h height of block
 * @param chroma_weight weighting factor applied to the chroma prediction
 * @param chroma_offset additive offset applied to the chroma prediction value
 */

static void chroma_mc_uni(HEVCContext *s, uint8_t *dst0,
                          ptrdiff_t dststride, uint8_t *src0, ptrdiff_t srcstride, int reflist,
                          int x_off, int y_off, int block_w, int block_h, struct MvField *current_mv, int chroma_weight, int chroma_offset)
{
    HEVCLocalContext *lc = s->HEVClc;
    int pic_width        = s->ps.sps->width >> s->ps.sps->hshift[1];
    int pic_height       = s->ps.sps->height >> s->ps.sps->vshift[1];
    const Mv *mv         = &current_mv->mv[reflist];
    int weight_flag      = (s->sh.slice_type == P_SLICE && s->ps.pps->weighted_pred_flag) ||
                           (s->sh.slice_type == B_SLICE && s->ps.pps->weighted_bipred_flag);
    int idx              = ff_hevc_pel_weight[block_w];
    int hshift           = s->ps.sps->hshift[1];
    int vshift           = s->ps.sps->vshift[1];
    intptr_t mx          = av_mod_uintp2(mv->x, 2 + hshift);
    intptr_t my          = av_mod_uintp2(mv->y, 2 + vshift);
    intptr_t _mx         = mx << (1 - hshift);
    intptr_t _my         = my << (1 - vshift);

#ifdef DISABLE_MC
    return;
#endif

    x_off += mv->x >> (2 + hshift);
    y_off += mv->y >> (2 + vshift);
    src0  += y_off * srcstride + (x_off * (1 << s->ps.sps->pixel_shift));

    if (x_off < EPEL_EXTRA_BEFORE || y_off < EPEL_EXTRA_AFTER ||
        x_off >= pic_width - block_w - EPEL_EXTRA_AFTER ||
        y_off >= pic_height - block_h - EPEL_EXTRA_AFTER) {
        const int edge_emu_stride = EDGE_EMU_BUFFER_STRIDE << s->ps.sps->pixel_shift;
        int offset0 = EPEL_EXTRA_BEFORE * (srcstride + (1 << s->ps.sps->pixel_shift));
        int buf_offset0 = EPEL_EXTRA_BEFORE *
                          (edge_emu_stride + (1 << s->ps.sps->pixel_shift));
        s->vdsp.emulated_edge_mc(lc->edge_emu_buffer, src0 - offset0,
                                 edge_emu_stride, srcstride,
                                 block_w + EPEL_EXTRA, block_h + EPEL_EXTRA,
                                 x_off - EPEL_EXTRA_BEFORE,
                                 y_off - EPEL_EXTRA_BEFORE,
                                 pic_width, pic_height);

        src0 = lc->edge_emu_buffer + buf_offset0;
        srcstride = edge_emu_stride;
    }
    if (!weight_flag)
        s->hevcdsp.put_hevc_epel_uni[idx][!!my][!!mx](dst0, dststride, src0, srcstride,
                                                  block_h, _mx, _my, block_w);
    else
        s->hevcdsp.put_hevc_epel_uni_w[idx][!!my][!!mx](dst0, dststride, src0, srcstride,
                                                        block_h, s->sh.chroma_log2_weight_denom,
                                                        chroma_weight, chroma_offset, _mx, _my, block_w);
}

/**
 * 8.5.3.2.2.2 Chroma sample bidirectional interpolation process
 *
 * @param s HEVC decoding context
 * @param dst target buffer for block data at block position
 * @param dststride stride of the dst buffer
 * @param ref0 reference picture0 buffer at origin (0, 0)
 * @param mv0 motion vector0 (relative to block position) to get pixel data from
 * @param x_off horizontal position of block from origin (0, 0)
 * @param y_off vertical position of block from origin (0, 0)
 * @param block_w width of block
 * @param block_h height of block
 * @param ref1 reference picture1 buffer at origin (0, 0)
 * @param mv1 motion vector1 (relative to block position) to get pixel data from
 * @param current_mv current motion vector structure
 * @param cidx chroma component(cb, cr)
 */
static void chroma_mc_bi(HEVCContext *s, uint8_t *dst0, ptrdiff_t dststride, AVFrame *ref0, AVFrame *ref1,
                         int x_off, int y_off, int block_w, int block_h, struct MvField *current_mv, int cidx)
{
    HEVCLocalContext *lc = s->HEVClc;
    uint8_t *src1        = ref0->data[cidx+1];
    uint8_t *src2        = ref1->data[cidx+1];
    ptrdiff_t src1stride = ref0->linesize[cidx+1];
    ptrdiff_t src2stride = ref1->linesize[cidx+1];
    int weight_flag      = (s->sh.slice_type == P_SLICE && s->ps.pps->weighted_pred_flag) ||
                           (s->sh.slice_type == B_SLICE && s->ps.pps->weighted_bipred_flag);
    int pic_width        = s->ps.sps->width >> s->ps.sps->hshift[1];
    int pic_height       = s->ps.sps->height >> s->ps.sps->vshift[1];
    Mv *mv0              = &current_mv->mv[0];
    Mv *mv1              = &current_mv->mv[1];
    int hshift = s->ps.sps->hshift[1];
    int vshift = s->ps.sps->vshift[1];

#ifdef DISABLE_MC
    return;
#endif

    intptr_t mx0 = av_mod_uintp2(mv0->x, 2 + hshift);
    intptr_t my0 = av_mod_uintp2(mv0->y, 2 + vshift);
    intptr_t mx1 = av_mod_uintp2(mv1->x, 2 + hshift);
    intptr_t my1 = av_mod_uintp2(mv1->y, 2 + vshift);
    intptr_t _mx0 = mx0 << (1 - hshift);
    intptr_t _my0 = my0 << (1 - vshift);
    intptr_t _mx1 = mx1 << (1 - hshift);
    intptr_t _my1 = my1 << (1 - vshift);

    int x_off0 = x_off + (mv0->x >> (2 + hshift));
    int y_off0 = y_off + (mv0->y >> (2 + vshift));
    int x_off1 = x_off + (mv1->x >> (2 + hshift));
    int y_off1 = y_off + (mv1->y >> (2 + vshift));
    int idx = ff_hevc_pel_weight[block_w];
    src1  += y_off0 * src1stride + (int)((unsigned)x_off0 << s->ps.sps->pixel_shift);
    src2  += y_off1 * src2stride + (int)((unsigned)x_off1 << s->ps.sps->pixel_shift);

    if (x_off0 < EPEL_EXTRA_BEFORE || y_off0 < EPEL_EXTRA_AFTER ||
        x_off0 >= pic_width - block_w - EPEL_EXTRA_AFTER ||
        y_off0 >= pic_height - block_h - EPEL_EXTRA_AFTER) {
        const int edge_emu_stride = EDGE_EMU_BUFFER_STRIDE << s->ps.sps->pixel_shift;
        int offset1 = EPEL_EXTRA_BEFORE * (src1stride + (1 << s->ps.sps->pixel_shift));
        int buf_offset1 = EPEL_EXTRA_BEFORE *
                          (edge_emu_stride + (1 << s->ps.sps->pixel_shift));

        s->vdsp.emulated_edge_mc(lc->edge_emu_buffer, src1 - offset1,
                                 edge_emu_stride, src1stride,
                                 block_w + EPEL_EXTRA, block_h + EPEL_EXTRA,
                                 x_off0 - EPEL_EXTRA_BEFORE,
                                 y_off0 - EPEL_EXTRA_BEFORE,
                                 pic_width, pic_height);

        src1 = lc->edge_emu_buffer + buf_offset1;
        src1stride = edge_emu_stride;
    }

    if (x_off1 < EPEL_EXTRA_BEFORE || y_off1 < EPEL_EXTRA_AFTER ||
        x_off1 >= pic_width - block_w - EPEL_EXTRA_AFTER ||
        y_off1 >= pic_height - block_h - EPEL_EXTRA_AFTER) {
        const int edge_emu_stride = EDGE_EMU_BUFFER_STRIDE << s->ps.sps->pixel_shift;
        int offset1 = EPEL_EXTRA_BEFORE * (src2stride + (1 << s->ps.sps->pixel_shift));
        int buf_offset1 = EPEL_EXTRA_BEFORE *
                          (edge_emu_stride + (1 << s->ps.sps->pixel_shift));

        s->vdsp.emulated_edge_mc(lc->edge_emu_buffer2, src2 - offset1,
                                 edge_emu_stride, src2stride,
                                 block_w + EPEL_EXTRA, block_h + EPEL_EXTRA,
                                 x_off1 - EPEL_EXTRA_BEFORE,
                                 y_off1 - EPEL_EXTRA_BEFORE,
                                 pic_width, pic_height);

        src2 = lc->edge_emu_buffer2 + buf_offset1;
        src2stride = edge_emu_stride;
    }

    s->hevcdsp.put_hevc_epel[idx][!!my0][!!mx0](lc->tmp, src1, src1stride,
                                                block_h, _mx0, _my0, block_w);
    if (!weight_flag)
        s->hevcdsp.put_hevc_epel_bi[idx][!!my1][!!mx1](dst0, s->frame->linesize[cidx+1],
                                                       src2, src2stride, lc->tmp,
                                                       block_h, _mx1, _my1, block_w);
    else
        s->hevcdsp.put_hevc_epel_bi_w[idx][!!my1][!!mx1](dst0, s->frame->linesize[cidx+1],
                                                         src2, src2stride, lc->tmp,
                                                         block_h,
                                                         s->sh.chroma_log2_weight_denom,
                                                         s->sh.chroma_weight_l0[current_mv->ref_idx[0]][cidx],
                                                         s->sh.chroma_weight_l1[current_mv->ref_idx[1]][cidx],
                                                         s->sh.chroma_offset_l0[current_mv->ref_idx[0]][cidx],
                                                         s->sh.chroma_offset_l1[current_mv->ref_idx[1]][cidx],
                                                         _mx1, _my1, block_w);
}

static void hevc_await_progress(HEVCContext *s, HEVCFrame *ref,
                                const Mv *mv, int y0, int height)
{
    int y = FFMAX(0, (mv->y >> 2) + y0 + height + 9);

    if (s->threads_type == FF_THREAD_FRAME )
        ff_thread_await_progress(&ref->tf, y, 0);
}

static void hevc_luma_mv_mvp_mode(HEVCContext *s, int x0, int y0, int nPbW,
                                  int nPbH, int log2_cb_size, int part_idx,
                                  int merge_idx, MvField *mv)
{
    HEVCLocalContext *lc = s->HEVClc;
    enum InterPredIdc inter_pred_idc = PRED_L0;
    int mvp_flag;

    ff_hevc_set_neighbour_available(s, x0, y0, nPbW, nPbH);
    mv->pred_flag = 0;
    if (s->sh.slice_type == B_SLICE)
        inter_pred_idc = ff_hevc_inter_pred_idc_decode(s, nPbW, nPbH);

    if (inter_pred_idc != PRED_L1) {
        if (s->sh.nb_refs[L0])
            mv->ref_idx[0]= ff_hevc_ref_idx_lx_decode(s, s->sh.nb_refs[L0]);

        mv->pred_flag = PF_L0;
        ff_hevc_hls_mvd_coding(s, x0, y0, 0);
        mvp_flag = ff_hevc_mvp_lx_flag_decode(s);
        ff_hevc_luma_mv_mvp_mode(s, x0, y0, nPbW, nPbH, log2_cb_size,
                                 part_idx, merge_idx, mv, mvp_flag, 0);
        mv->mv[0].x += lc->pu.mvd.x;
        mv->mv[0].y += lc->pu.mvd.y;
    }

    if (inter_pred_idc != PRED_L0) {
        if (s->sh.nb_refs[L1])
            mv->ref_idx[1]= ff_hevc_ref_idx_lx_decode(s, s->sh.nb_refs[L1]);

        if (s->sh.mvd_l1_zero_flag == 1 && inter_pred_idc == PRED_BI) {
            AV_ZERO32(&lc->pu.mvd);
        } else {
            ff_hevc_hls_mvd_coding(s, x0, y0, 1);
        }

        mv->pred_flag += PF_L1;
        mvp_flag = ff_hevc_mvp_lx_flag_decode(s);
        ff_hevc_luma_mv_mvp_mode(s, x0, y0, nPbW, nPbH, log2_cb_size,
                                 part_idx, merge_idx, mv, mvp_flag, 1);
        mv->mv[1].x += lc->pu.mvd.x;
        mv->mv[1].y += lc->pu.mvd.y;
    }
}


#if RPI_AUX_FRAME_USE
#define get_vc_address_ref_y(fr) rpi_auxframe_vc_y(fr)
#define get_vc_address_ref_u(fr) rpi_auxframe_vc_u(fr)
#define get_vc_address_ref_v(fr) rpi_auxframe_vc_v(fr)
#else
#define get_vc_address_ref_y(fr) get_vc_address_y(fr)
#define get_vc_address_ref_u(fr) get_vc_address_u(fr)
#define get_vc_address_ref_v(fr) get_vc_address_v(fr)
#endif

static void hls_prediction_unit(HEVCContext *s, int x0, int y0,
                                int nPbW, int nPbH,
                                int log2_cb_size, int partIdx, int idx)
{
#define POS(c_idx, x, y)                                                              \
    &s->frame->data[c_idx][((y) >> s->ps.sps->vshift[c_idx]) * s->frame->linesize[c_idx] + \
                           (((x) >> s->ps.sps->hshift[c_idx]) << s->ps.sps->pixel_shift)]
    HEVCLocalContext *lc = s->HEVClc;
    int merge_idx = 0;
    struct MvField current_mv = {{{ 0 }}};

    int min_pu_width = s->ps.sps->min_pu_width;

    MvField *tab_mvf = s->ref->tab_mvf;
    RefPicList  *refPicList = s->ref->refPicList;
    HEVCFrame *ref0 = NULL, *ref1 = NULL;
    uint8_t *dst0 = POS(0, x0, y0);
    uint8_t *dst1 = POS(1, x0, y0);
    uint8_t *dst2 = POS(2, x0, y0);
    int log2_min_cb_size = s->ps.sps->log2_min_cb_size;
    int min_cb_width     = s->ps.sps->min_cb_width;
    int x_cb             = x0 >> log2_min_cb_size;
    int y_cb             = y0 >> log2_min_cb_size;
    int x_pu, y_pu;
    int i, j;

    int skip_flag = SAMPLE_CTB(s->skip_flag, x_cb, y_cb);

    if (!skip_flag)
        lc->pu.merge_flag = ff_hevc_merge_flag_decode(s);

    if (skip_flag || lc->pu.merge_flag) {
        if (s->sh.max_num_merge_cand > 1)
            merge_idx = ff_hevc_merge_idx_decode(s);
        else
            merge_idx = 0;

        ff_hevc_luma_mv_merge_mode(s, x0, y0, nPbW, nPbH, log2_cb_size,
                                   partIdx, merge_idx, &current_mv);
    } else {
        hevc_luma_mv_mvp_mode(s, x0, y0, nPbW, nPbH, log2_cb_size,
                              partIdx, merge_idx, &current_mv);
    }

    x_pu = x0 >> s->ps.sps->log2_min_pu_size;
    y_pu = y0 >> s->ps.sps->log2_min_pu_size;

    for (j = 0; j < nPbH >> s->ps.sps->log2_min_pu_size; j++)
        for (i = 0; i < nPbW >> s->ps.sps->log2_min_pu_size; i++)
            tab_mvf[(y_pu + j) * min_pu_width + x_pu + i] = current_mv;

    if (current_mv.pred_flag & PF_L0) {
        ref0 = refPicList[0].ref[current_mv.ref_idx[0]];
        if (!ref0)
            return;
        hevc_await_progress(s, ref0, &current_mv.mv[0], y0, nPbH);
    }
    if (current_mv.pred_flag & PF_L1) {
        ref1 = refPicList[1].ref[current_mv.ref_idx[1]];
        if (!ref1)
            return;
        hevc_await_progress(s, ref1, &current_mv.mv[1], y0, nPbH);
    }

    if (current_mv.pred_flag == PF_L0) {
        int x0_c = x0 >> s->ps.sps->hshift[1];
        int y0_c = y0 >> s->ps.sps->vshift[1];
        int nPbW_c = nPbW >> s->ps.sps->hshift[1];
        int nPbH_c = nPbH >> s->ps.sps->vshift[1];

#ifdef RPI_LUMA_QPU
        if (s->enable_rpi) {
            int reflist = 0;
            const Mv *mv         = &current_mv.mv[reflist];
            int mx          = mv->x & 3;
            int my          = mv->y & 3;
            int my_mx = (my<<8) + mx;
            int my2_mx2_my_mx = (my_mx << 16) + my_mx;
            int x1 = x0 + (mv->x >> 2);
            int y1 = y0 + (mv->y >> 2);
            int weight_flag = (s->sh.slice_type == P_SLICE && s->ps.pps->weighted_pred_flag) ||
                              (s->sh.slice_type == B_SLICE && s->ps.pps->weighted_bipred_flag);
            uint32_t *y = s->curr_y_mvs;
            for(int start_y=0;start_y < nPbH;start_y+=16) {  // Potentially we could change the assembly code to support taller sizes in one go
              for(int start_x=0;start_x < nPbW;start_x+=16) {
                  int bw = nPbW-start_x;
                  int bh = nPbH-start_y;
                  y++[-RPI_LUMA_COMMAND_WORDS] = ((y1 - 3 + start_y) << 16) + ( (x1 - 3 + start_x) & 0xffff);
                  y++[-RPI_LUMA_COMMAND_WORDS] = get_vc_address_ref_y(ref0->frame);
                  y++[-RPI_LUMA_COMMAND_WORDS] = ((y1 - 3 + start_y) << 16) + ( (x1 - 3 + 8 + start_x) & 0xffff);
                  y++[-RPI_LUMA_COMMAND_WORDS] = get_vc_address_ref_y(ref0->frame);
                  *y++ = ( (bw<16 ? bw : 16) << 16 ) + (bh<16 ? bh : 16);
                  *y++ = my2_mx2_my_mx;
                  if (weight_flag) {
                      *y++ = (s->sh.luma_offset_l0[current_mv.ref_idx[reflist]] << 16) + (s->sh.luma_weight_l0[current_mv.ref_idx[reflist]] & 0xffff);
                  } else {
                      *y++ = 1; // Weight of 1 and offset of 0
                  }
                  *y++ = (get_vc_address_y(s->frame) + x0 + start_x + (start_y + y0) * s->frame->linesize[0]);
                  y++[-RPI_LUMA_COMMAND_WORDS] = s->mc_filter;
                }
            }
            s->curr_y_mvs = y;
        } else
#endif
        {
            RPI_REDIRECT(luma_mc_uni)(s, dst0, s->frame->linesize[0], ref0->frame,
                    &current_mv.mv[0], x0, y0, nPbW, nPbH,
                    s->sh.luma_weight_l0[current_mv.ref_idx[0]],
                    s->sh.luma_offset_l0[current_mv.ref_idx[0]]);
        }

        if (s->ps.sps->chroma_format_idc) {
#ifdef RPI_INTER_QPU
            if (s->enable_rpi) {
                int reflist = 0;
                int hshift           = s->ps.sps->hshift[1];
                int vshift           = s->ps.sps->vshift[1];
                const Mv *mv         = &current_mv.mv[reflist];
                intptr_t mx          = av_mod_uintp2(mv->x, 2 + hshift);
                intptr_t my          = av_mod_uintp2(mv->y, 2 + vshift);
                intptr_t _mx         = mx << (1 - hshift);
                intptr_t _my         = my << (1 - vshift); // Fractional part of motion vector

                int x1_c = x0_c + (mv->x >> (2 + hshift));
                int y1_c = y0_c + (mv->y >> (2 + hshift));
                int weight_flag      = (s->sh.slice_type == P_SLICE && s->ps.pps->weighted_pred_flag) ||
                                       (s->sh.slice_type == B_SLICE && s->ps.pps->weighted_bipred_flag);

                uint32_t *u = s->curr_u_mvs;
                for(int start_y=0;start_y < nPbH_c;start_y+=16) {
                  for(int start_x=0;start_x < nPbW_c;start_x+=RPI_CHROMA_BLOCK_WIDTH) {
                      int bw = nPbW_c-start_x;
                      int bh = nPbH_c-start_y;
                      u++[-RPI_CHROMA_COMMAND_WORDS] = s->mc_filter_uv;
                      u++[-RPI_CHROMA_COMMAND_WORDS] = x1_c - 1 + start_x;
                      u++[-RPI_CHROMA_COMMAND_WORDS] = y1_c - 1 + start_y;
                      u++[-RPI_CHROMA_COMMAND_WORDS] = get_vc_address_ref_u(ref0->frame);
                      u++[-RPI_CHROMA_COMMAND_WORDS] = get_vc_address_ref_v(ref0->frame);
                      *u++ = ( (bw<RPI_CHROMA_BLOCK_WIDTH ? bw : RPI_CHROMA_BLOCK_WIDTH) << 16 ) + (bh<16 ? bh : 16);
                      *u++ = rpi_filter_coefs[_mx][0];
                      *u++ = rpi_filter_coefs[_my][0];
                      if (weight_flag) {
                          *u++ = (s->sh.chroma_offset_l0[current_mv.ref_idx[0]][0] << 16) + (s->sh.chroma_weight_l0[current_mv.ref_idx[0]][0] & 0xffff);
                          *u++ = (s->sh.chroma_offset_l0[current_mv.ref_idx[0]][1] << 16) + (s->sh.chroma_weight_l0[current_mv.ref_idx[0]][1] & 0xffff);
                      } else {
                          *u++ = 1; // Weight of 1 and offset of 0
                          *u++ = 1;
                      }
                      *u++ = (get_vc_address_u(s->frame) + x0_c + start_x + (start_y + y0_c) * s->frame->linesize[1]);
                      *u++ = (get_vc_address_v(s->frame) + x0_c + start_x + (start_y + y0_c) * s->frame->linesize[2]);
                    }
                }
                s->curr_u_mvs = u;
                return;
            }
#endif
            RPI_REDIRECT(chroma_mc_uni)(s, dst1, s->frame->linesize[1], ref0->frame->data[1], ref0->frame->linesize[1],
                          0, x0_c, y0_c, nPbW_c, nPbH_c, &current_mv,
                          s->sh.chroma_weight_l0[current_mv.ref_idx[0]][0], s->sh.chroma_offset_l0[current_mv.ref_idx[0]][0]);
            RPI_REDIRECT(chroma_mc_uni)(s, dst2, s->frame->linesize[2], ref0->frame->data[2], ref0->frame->linesize[2],
                          0, x0_c, y0_c, nPbW_c, nPbH_c, &current_mv,
                          s->sh.chroma_weight_l0[current_mv.ref_idx[0]][1], s->sh.chroma_offset_l0[current_mv.ref_idx[0]][1]);
        }
    } else if (current_mv.pred_flag == PF_L1) {
        int x0_c = x0 >> s->ps.sps->hshift[1];
        int y0_c = y0 >> s->ps.sps->vshift[1];
        int nPbW_c = nPbW >> s->ps.sps->hshift[1];
        int nPbH_c = nPbH >> s->ps.sps->vshift[1];

#ifdef RPI_LUMA_QPU
        if (s->enable_rpi) {
            int reflist = 1;
            const Mv *mv    = &current_mv.mv[reflist];
            int mx          = mv->x & 3;
            int my          = mv->y & 3;
            int my_mx = (my<<8) + mx;
            int my2_mx2_my_mx = (my_mx << 16) + my_mx;
            int x1 = x0 + (mv->x >> 2);
            int y1 = y0 + (mv->y >> 2);
            int weight_flag = (s->sh.slice_type == P_SLICE && s->ps.pps->weighted_pred_flag) ||
                              (s->sh.slice_type == B_SLICE && s->ps.pps->weighted_bipred_flag);
            uint32_t *y = s->curr_y_mvs;
            for(int start_y=0;start_y < nPbH;start_y+=16) {  // Potentially we could change the assembly code to support taller sizes in one go
              for(int start_x=0;start_x < nPbW;start_x+=16) {
                  int bw = nPbW-start_x;
                  int bh = nPbH-start_y;
                  y++[-RPI_LUMA_COMMAND_WORDS] = ((y1 - 3 + start_y) << 16) + ( (x1 - 3 + start_x) & 0xffff);
                  y++[-RPI_LUMA_COMMAND_WORDS] = get_vc_address_ref_y(ref1->frame);
                  y++[-RPI_LUMA_COMMAND_WORDS] = ((y1 - 3 + start_y) << 16) + ( (x1 - 3 + 8 + start_x) & 0xffff);
                  y++[-RPI_LUMA_COMMAND_WORDS] = get_vc_address_ref_y(ref1->frame);
                  *y++ = ( (bw<16 ? bw : 16) << 16 ) + (bh<16 ? bh : 16);
                  *y++ = my2_mx2_my_mx;
                  if (weight_flag) {
                      *y++ = (s->sh.luma_offset_l0[current_mv.ref_idx[reflist]] << 16) + (s->sh.luma_weight_l0[current_mv.ref_idx[reflist]] & 0xffff);
                  } else {
                      *y++ = 1; // Weight of 1 and offset of 0
                  }
                  *y++ = (get_vc_address_y(s->frame) + x0 + start_x + (start_y + y0) * s->frame->linesize[0]);
                  y++[-RPI_LUMA_COMMAND_WORDS] = s->mc_filter;
                }
            }
            s->curr_y_mvs = y;
        } else
#endif

        {
            RPI_REDIRECT(luma_mc_uni)(s, dst0, s->frame->linesize[0], ref1->frame,
                    &current_mv.mv[1], x0, y0, nPbW, nPbH,
                    s->sh.luma_weight_l1[current_mv.ref_idx[1]],
                    s->sh.luma_offset_l1[current_mv.ref_idx[1]]);
        }

        if (s->ps.sps->chroma_format_idc) {
#ifdef RPI_INTER_QPU
            if (s->enable_rpi) {
                int reflist = 1;
                int hshift           = s->ps.sps->hshift[1];
                int vshift           = s->ps.sps->vshift[1];
                const Mv *mv         = &current_mv.mv[reflist];
                intptr_t mx          = av_mod_uintp2(mv->x, 2 + hshift);
                intptr_t my          = av_mod_uintp2(mv->y, 2 + vshift);
                intptr_t _mx         = mx << (1 - hshift);
                intptr_t _my         = my << (1 - vshift); // Fractional part of motion vector

                int x1_c = x0_c + (mv->x >> (2 + hshift));
                int y1_c = y0_c + (mv->y >> (2 + hshift));
                int weight_flag      = (s->sh.slice_type == P_SLICE && s->ps.pps->weighted_pred_flag) ||
                                       (s->sh.slice_type == B_SLICE && s->ps.pps->weighted_bipred_flag);

                uint32_t *u = s->curr_u_mvs;
                for(int start_y=0;start_y < nPbH_c;start_y+=16) {
                  for(int start_x=0;start_x < nPbW_c;start_x+=RPI_CHROMA_BLOCK_WIDTH) {
                      int bw = nPbW_c-start_x;
                      int bh = nPbH_c-start_y;
                      u++[-RPI_CHROMA_COMMAND_WORDS] = s->mc_filter_uv;
                      u++[-RPI_CHROMA_COMMAND_WORDS] = x1_c - 1 + start_x;
                      u++[-RPI_CHROMA_COMMAND_WORDS] = y1_c - 1 + start_y;
                      u++[-RPI_CHROMA_COMMAND_WORDS] = get_vc_address_ref_u(ref1->frame);
                      u++[-RPI_CHROMA_COMMAND_WORDS] = get_vc_address_ref_v(ref1->frame);
                      *u++ = ( (bw<RPI_CHROMA_BLOCK_WIDTH ? bw : RPI_CHROMA_BLOCK_WIDTH) << 16 ) + (bh<16 ? bh : 16);
                      // TODO chroma weight and offset... s->sh.chroma_weight_l0[current_mv.ref_idx[0]][0], s->sh.chroma_offset_l0[current_mv.ref_idx[0]][0]
                      *u++ = rpi_filter_coefs[_mx][0];
                      *u++ = rpi_filter_coefs[_my][0];
                      if (weight_flag) {
                          *u++ = (s->sh.chroma_offset_l0[current_mv.ref_idx[reflist]][0] << 16) + (s->sh.chroma_weight_l0[current_mv.ref_idx[reflist]][0] & 0xffff);
                          *u++ = (s->sh.chroma_offset_l0[current_mv.ref_idx[reflist]][1] << 16) + (s->sh.chroma_weight_l0[current_mv.ref_idx[reflist]][1] & 0xffff);
                      } else {
                          *u++ = 1; // Weight of 1 and offset of 0
                          *u++ = 1;
                      }
                      *u++ = (get_vc_address_u(s->frame) + x0_c + start_x + (start_y + y0_c) * s->frame->linesize[1]);
                      *u++ = (get_vc_address_v(s->frame) + x0_c + start_x + (start_y + y0_c) * s->frame->linesize[2]);
                    }
                }
                s->curr_u_mvs = u;
                return;
            }
#endif
            RPI_REDIRECT(chroma_mc_uni)(s, dst1, s->frame->linesize[1], ref1->frame->data[1], ref1->frame->linesize[1],
                          1, x0_c, y0_c, nPbW_c, nPbH_c, &current_mv,
                          s->sh.chroma_weight_l1[current_mv.ref_idx[1]][0], s->sh.chroma_offset_l1[current_mv.ref_idx[1]][0]);

            RPI_REDIRECT(chroma_mc_uni)(s, dst2, s->frame->linesize[2], ref1->frame->data[2], ref1->frame->linesize[2],
                          1, x0_c, y0_c, nPbW_c, nPbH_c, &current_mv,
                          s->sh.chroma_weight_l1[current_mv.ref_idx[1]][1], s->sh.chroma_offset_l1[current_mv.ref_idx[1]][1]);
        }
    } else if (current_mv.pred_flag == PF_BI) {
        int x0_c = x0 >> s->ps.sps->hshift[1];
        int y0_c = y0 >> s->ps.sps->vshift[1];
        int nPbW_c = nPbW >> s->ps.sps->hshift[1];
        int nPbH_c = nPbH >> s->ps.sps->vshift[1];

#ifdef RPI_LUMA_QPU
        if (s->enable_rpi) {
            const Mv *mv    = &current_mv.mv[0];
            int mx          = mv->x & 3;
            int my          = mv->y & 3;
            int my_mx = (my<<8) + mx;
            const Mv *mv2    = &current_mv.mv[1];
            int mx2          = mv2->x & 3;
            int my2          = mv2->y & 3;
            int my2_mx2 = (my2<<8) + mx2;
            int my2_mx2_my_mx = (my2_mx2 << 16) + my_mx;
            int x1 = x0 + (mv->x >> 2);
            int y1 = y0 + (mv->y >> 2);
            int x2 = x0 + (mv2->x >> 2);
            int y2 = y0 + (mv2->y >> 2);
            uint32_t *y = s->curr_y_mvs;
            for(int start_y=0;start_y < nPbH;start_y+=16) {  // Potentially we could change the assembly code to support taller sizes in one go
              for(int start_x=0;start_x < nPbW;start_x+=8) { // B blocks work 8 at a time
                  int bw = nPbW-start_x;
                  int bh = nPbH-start_y;
                  y++[-RPI_LUMA_COMMAND_WORDS] = ((y1 - 3 + start_y) << 16) + ( (x1 - 3 + start_x) & 0xffff);
                  y++[-RPI_LUMA_COMMAND_WORDS] = get_vc_address_ref_y(ref0->frame);
                  y++[-RPI_LUMA_COMMAND_WORDS] = ((y2 - 3 + start_y) << 16) + ( (x2 - 3 + start_x) & 0xffff); // Second fetch is for ref1
                  y++[-RPI_LUMA_COMMAND_WORDS] = get_vc_address_ref_y(ref1->frame);
                  *y++ = ( (bw<8 ? bw : 8) << 16 ) + (bh<16 ? bh : 16);
                  *y++ = my2_mx2_my_mx;
                  *y++ = 1; // B frame weighted prediction not supported
                  *y++ = (get_vc_address_y(s->frame) + x0 + start_x + (start_y + y0) * s->frame->linesize[0]);
                  y++[-RPI_LUMA_COMMAND_WORDS] = s->mc_filter_b;
                }
            }
            s->curr_y_mvs = y;
        } else
#endif
        {
            RPI_REDIRECT(luma_mc_bi)(s, dst0, s->frame->linesize[0], ref0->frame,
                   &current_mv.mv[0], x0, y0, nPbW, nPbH,
                   ref1->frame, &current_mv.mv[1], &current_mv);
        }

        if (s->ps.sps->chroma_format_idc) {
#ifdef RPI_INTER_QPU
            if (s->enable_rpi) {
                int hshift           = s->ps.sps->hshift[1];
                int vshift           = s->ps.sps->vshift[1];
                const Mv *mv         = &current_mv.mv[0];
                intptr_t mx          = av_mod_uintp2(mv->x, 2 + hshift);
                intptr_t my          = av_mod_uintp2(mv->y, 2 + vshift);
                intptr_t _mx         = mx << (1 - hshift);
                intptr_t _my         = my << (1 - vshift); // Fractional part of motion vector
                int x1_c = x0_c + (mv->x >> (2 + hshift));
                int y1_c = y0_c + (mv->y >> (2 + hshift));

                const Mv *mv2         = &current_mv.mv[1];
                intptr_t mx2          = av_mod_uintp2(mv2->x, 2 + hshift);
                intptr_t my2          = av_mod_uintp2(mv2->y, 2 + vshift);
                intptr_t _mx2         = mx2 << (1 - hshift);
                intptr_t _my2         = my2 << (1 - vshift); // Fractional part of motion vector

                int x2_c = x0_c + (mv2->x >> (2 + hshift));
                int y2_c = y0_c + (mv2->y >> (2 + hshift));


                uint32_t *u = s->curr_u_mvs;
                for(int start_y=0;start_y < nPbH_c;start_y+=16) {
                  for(int start_x=0;start_x < nPbW_c;start_x+=RPI_CHROMA_BLOCK_WIDTH) {
                      int bw = nPbW_c-start_x;
                      int bh = nPbH_c-start_y;
                      u++[-RPI_CHROMA_COMMAND_WORDS] = s->mc_filter_uv_b0;
                      u++[-RPI_CHROMA_COMMAND_WORDS] = x1_c - 1 + start_x;
                      u++[-RPI_CHROMA_COMMAND_WORDS] = y1_c - 1 + start_y;
                      u++[-RPI_CHROMA_COMMAND_WORDS] = get_vc_address_ref_u(ref0->frame);
                      u++[-RPI_CHROMA_COMMAND_WORDS] = get_vc_address_ref_v(ref0->frame);
                      *u++ = ( (bw<RPI_CHROMA_BLOCK_WIDTH ? bw : RPI_CHROMA_BLOCK_WIDTH) << 16 ) + (bh<16 ? bh : 16);
                      *u++ = rpi_filter_coefs[_mx][0];
                      *u++ = rpi_filter_coefs[_my][0];
                      u+=2; // Weights not supported in B slices
                      u+=2; // Intermediate results are not written back in first pass of B filtering

                      u++[-RPI_CHROMA_COMMAND_WORDS] = s->mc_filter_uv_b;
                      u++[-RPI_CHROMA_COMMAND_WORDS] = x2_c - 1 + start_x;
                      u++[-RPI_CHROMA_COMMAND_WORDS] = y2_c - 1 + start_y;
                      u++[-RPI_CHROMA_COMMAND_WORDS] = get_vc_address_ref_u(ref1->frame);
                      u++[-RPI_CHROMA_COMMAND_WORDS] = get_vc_address_ref_v(ref1->frame);
                      *u++ = ( (bw<RPI_CHROMA_BLOCK_WIDTH ? bw : RPI_CHROMA_BLOCK_WIDTH) << 16 ) + (bh<16 ? bh : 16);
                      *u++ = rpi_filter_coefs[_mx2][0];
                      *u++ = rpi_filter_coefs[_my2][0];
                      u+=2; // Weights not supported in B slices
                      *u++ = (get_vc_address_u(s->frame) + x0_c + start_x + (start_y + y0_c) * s->frame->linesize[1]);
                      *u++ = (get_vc_address_v(s->frame) + x0_c + start_x + (start_y + y0_c) * s->frame->linesize[2]);
                    }
                }
                s->curr_u_mvs = u;
                return;
            }
#endif
            RPI_REDIRECT(chroma_mc_bi)(s, dst1, s->frame->linesize[1], ref0->frame, ref1->frame,
                         x0_c, y0_c, nPbW_c, nPbH_c, &current_mv, 0);

            RPI_REDIRECT(chroma_mc_bi)(s, dst2, s->frame->linesize[2], ref0->frame, ref1->frame,
                         x0_c, y0_c, nPbW_c, nPbH_c, &current_mv, 1);
        }
    }
}

/**
 * 8.4.1
 */
static int luma_intra_pred_mode(HEVCContext *s, int x0, int y0, int pu_size,
                                int prev_intra_luma_pred_flag)
{
    HEVCLocalContext *lc = s->HEVClc;
    int x_pu             = x0 >> s->ps.sps->log2_min_pu_size;
    int y_pu             = y0 >> s->ps.sps->log2_min_pu_size;
    int min_pu_width     = s->ps.sps->min_pu_width;
    int size_in_pus      = pu_size >> s->ps.sps->log2_min_pu_size;
    int x0b              = av_mod_uintp2(x0, s->ps.sps->log2_ctb_size);
    int y0b              = av_mod_uintp2(y0, s->ps.sps->log2_ctb_size);

    int cand_up   = (lc->ctb_up_flag || y0b) ?
                    s->tab_ipm[(y_pu - 1) * min_pu_width + x_pu] : INTRA_DC;
    int cand_left = (lc->ctb_left_flag || x0b) ?
                    s->tab_ipm[y_pu * min_pu_width + x_pu - 1]   : INTRA_DC;

    int y_ctb = (y0 >> (s->ps.sps->log2_ctb_size)) << (s->ps.sps->log2_ctb_size);

    MvField *tab_mvf = s->ref->tab_mvf;
    int intra_pred_mode;
    int candidate[3];
    int i, j;

    // intra_pred_mode prediction does not cross vertical CTB boundaries
    if ((y0 - 1) < y_ctb)
        cand_up = INTRA_DC;

    if (cand_left == cand_up) {
        if (cand_left < 2) {
            candidate[0] = INTRA_PLANAR;
            candidate[1] = INTRA_DC;
            candidate[2] = INTRA_ANGULAR_26;
        } else {
            candidate[0] = cand_left;
            candidate[1] = 2 + ((cand_left - 2 - 1 + 32) & 31);
            candidate[2] = 2 + ((cand_left - 2 + 1) & 31);
        }
    } else {
        candidate[0] = cand_left;
        candidate[1] = cand_up;
        if (candidate[0] != INTRA_PLANAR && candidate[1] != INTRA_PLANAR) {
            candidate[2] = INTRA_PLANAR;
        } else if (candidate[0] != INTRA_DC && candidate[1] != INTRA_DC) {
            candidate[2] = INTRA_DC;
        } else {
            candidate[2] = INTRA_ANGULAR_26;
        }
    }

    if (prev_intra_luma_pred_flag) {
        intra_pred_mode = candidate[lc->pu.mpm_idx];
    } else {
        if (candidate[0] > candidate[1])
            FFSWAP(uint8_t, candidate[0], candidate[1]);
        if (candidate[0] > candidate[2])
            FFSWAP(uint8_t, candidate[0], candidate[2]);
        if (candidate[1] > candidate[2])
            FFSWAP(uint8_t, candidate[1], candidate[2]);

        intra_pred_mode = lc->pu.rem_intra_luma_pred_mode;
        for (i = 0; i < 3; i++)
            if (intra_pred_mode >= candidate[i])
                intra_pred_mode++;
    }

    /* write the intra prediction units into the mv array */
    if (!size_in_pus)
        size_in_pus = 1;
    for (i = 0; i < size_in_pus; i++) {
        memset(&s->tab_ipm[(y_pu + i) * min_pu_width + x_pu],
               intra_pred_mode, size_in_pus);

        for (j = 0; j < size_in_pus; j++) {
            tab_mvf[(y_pu + j) * min_pu_width + x_pu + i].pred_flag = PF_INTRA;
        }
    }

    return intra_pred_mode;
}

static av_always_inline void set_ct_depth(HEVCContext *s, int x0, int y0,
                                          int log2_cb_size, int ct_depth)
{
    int length = (1 << log2_cb_size) >> s->ps.sps->log2_min_cb_size;
    int x_cb   = x0 >> s->ps.sps->log2_min_cb_size;
    int y_cb   = y0 >> s->ps.sps->log2_min_cb_size;
    int y;

    for (y = 0; y < length; y++)
        memset(&s->tab_ct_depth[(y_cb + y) * s->ps.sps->min_cb_width + x_cb],
               ct_depth, length);
}

static const uint8_t tab_mode_idx[] = {
     0,  1,  2,  2,  2,  2,  3,  5,  7,  8, 10, 12, 13, 15, 17, 18, 19, 20,
    21, 22, 23, 23, 24, 24, 25, 25, 26, 27, 27, 28, 28, 29, 29, 30, 31};

static void intra_prediction_unit(HEVCContext *s, int x0, int y0,
                                  int log2_cb_size)
{
    HEVCLocalContext *lc = s->HEVClc;
    static const uint8_t intra_chroma_table[4] = { 0, 26, 10, 1 };
    uint8_t prev_intra_luma_pred_flag[4];
    int split   = lc->cu.part_mode == PART_NxN;
    int pb_size = (1 << log2_cb_size) >> split;
    int side    = split + 1;
    int chroma_mode;
    int i, j;

    for (i = 0; i < side; i++)
        for (j = 0; j < side; j++)
            prev_intra_luma_pred_flag[2 * i + j] = ff_hevc_prev_intra_luma_pred_flag_decode(s);

    for (i = 0; i < side; i++) {
        for (j = 0; j < side; j++) {
            if (prev_intra_luma_pred_flag[2 * i + j])
                lc->pu.mpm_idx = ff_hevc_mpm_idx_decode(s);
            else
                lc->pu.rem_intra_luma_pred_mode = ff_hevc_rem_intra_luma_pred_mode_decode(s);

            lc->pu.intra_pred_mode[2 * i + j] =
                luma_intra_pred_mode(s, x0 + pb_size * j, y0 + pb_size * i, pb_size,
                                     prev_intra_luma_pred_flag[2 * i + j]);
        }
    }

    if (s->ps.sps->chroma_format_idc == 3) {
        for (i = 0; i < side; i++) {
            for (j = 0; j < side; j++) {
                lc->pu.chroma_mode_c[2 * i + j] = chroma_mode = ff_hevc_intra_chroma_pred_mode_decode(s);
                if (chroma_mode != 4) {
                    if (lc->pu.intra_pred_mode[2 * i + j] == intra_chroma_table[chroma_mode])
                        lc->pu.intra_pred_mode_c[2 * i + j] = 34;
                    else
                        lc->pu.intra_pred_mode_c[2 * i + j] = intra_chroma_table[chroma_mode];
                } else {
                    lc->pu.intra_pred_mode_c[2 * i + j] = lc->pu.intra_pred_mode[2 * i + j];
                }
            }
        }
    } else if (s->ps.sps->chroma_format_idc == 2) {
        int mode_idx;
        lc->pu.chroma_mode_c[0] = chroma_mode = ff_hevc_intra_chroma_pred_mode_decode(s);
        if (chroma_mode != 4) {
            if (lc->pu.intra_pred_mode[0] == intra_chroma_table[chroma_mode])
                mode_idx = 34;
            else
                mode_idx = intra_chroma_table[chroma_mode];
        } else {
            mode_idx = lc->pu.intra_pred_mode[0];
        }
        lc->pu.intra_pred_mode_c[0] = tab_mode_idx[mode_idx];
    } else if (s->ps.sps->chroma_format_idc != 0) {
        chroma_mode = ff_hevc_intra_chroma_pred_mode_decode(s);
        if (chroma_mode != 4) {
            if (lc->pu.intra_pred_mode[0] == intra_chroma_table[chroma_mode])
                lc->pu.intra_pred_mode_c[0] = 34;
            else
                lc->pu.intra_pred_mode_c[0] = intra_chroma_table[chroma_mode];
        } else {
            lc->pu.intra_pred_mode_c[0] = lc->pu.intra_pred_mode[0];
        }
    }
}

static void intra_prediction_unit_default_value(HEVCContext *s,
                                                int x0, int y0,
                                                int log2_cb_size)
{
    HEVCLocalContext *lc = s->HEVClc;
    int pb_size          = 1 << log2_cb_size;
    int size_in_pus      = pb_size >> s->ps.sps->log2_min_pu_size;
    int min_pu_width     = s->ps.sps->min_pu_width;
    MvField *tab_mvf     = s->ref->tab_mvf;
    int x_pu             = x0 >> s->ps.sps->log2_min_pu_size;
    int y_pu             = y0 >> s->ps.sps->log2_min_pu_size;
    int j, k;

    if (size_in_pus == 0)
        size_in_pus = 1;
    for (j = 0; j < size_in_pus; j++)
        memset(&s->tab_ipm[(y_pu + j) * min_pu_width + x_pu], INTRA_DC, size_in_pus);
    if (lc->cu.pred_mode == MODE_INTRA)
        for (j = 0; j < size_in_pus; j++)
            for (k = 0; k < size_in_pus; k++)
                tab_mvf[(y_pu + j) * min_pu_width + x_pu + k].pred_flag = PF_INTRA;
}

static int hls_coding_unit(HEVCContext *s, int x0, int y0, int log2_cb_size)
{
    int cb_size          = 1 << log2_cb_size;
    HEVCLocalContext *lc = s->HEVClc;
    int log2_min_cb_size = s->ps.sps->log2_min_cb_size;
    int length           = cb_size >> log2_min_cb_size;
    int min_cb_width     = s->ps.sps->min_cb_width;
    int x_cb             = x0 >> log2_min_cb_size;
    int y_cb             = y0 >> log2_min_cb_size;
    int idx              = log2_cb_size - 2;
    int qp_block_mask    = (1<<(s->ps.sps->log2_ctb_size - s->ps.pps->diff_cu_qp_delta_depth)) - 1;
    int x, y, ret;

    lc->cu.x                = x0;
    lc->cu.y                = y0;
    lc->cu.pred_mode        = MODE_INTRA;
    lc->cu.part_mode        = PART_2Nx2N;
    lc->cu.intra_split_flag = 0;

    SAMPLE_CTB(s->skip_flag, x_cb, y_cb) = 0;
    for (x = 0; x < 4; x++)
        lc->pu.intra_pred_mode[x] = 1;
    if (s->ps.pps->transquant_bypass_enable_flag) {
        lc->cu.cu_transquant_bypass_flag = ff_hevc_cu_transquant_bypass_flag_decode(s);
        if (lc->cu.cu_transquant_bypass_flag)
            set_deblocking_bypass(s, x0, y0, log2_cb_size);
    } else
        lc->cu.cu_transquant_bypass_flag = 0;

    if (s->sh.slice_type != I_SLICE) {
        uint8_t skip_flag = ff_hevc_skip_flag_decode(s, x0, y0, x_cb, y_cb);

        x = y_cb * min_cb_width + x_cb;
        for (y = 0; y < length; y++) {
            memset(&s->skip_flag[x], skip_flag, length);
            x += min_cb_width;
        }
        lc->cu.pred_mode = skip_flag ? MODE_SKIP : MODE_INTER;
    } else {
        x = y_cb * min_cb_width + x_cb;
        for (y = 0; y < length; y++) {
            memset(&s->skip_flag[x], 0, length);
            x += min_cb_width;
        }
    }

    if (SAMPLE_CTB(s->skip_flag, x_cb, y_cb)) {
        hls_prediction_unit(s, x0, y0, cb_size, cb_size, log2_cb_size, 0, idx);
        intra_prediction_unit_default_value(s, x0, y0, log2_cb_size);

        if (!s->sh.disable_deblocking_filter_flag)
            ff_hevc_deblocking_boundary_strengths(s, x0, y0, log2_cb_size);
    } else {
        int pcm_flag = 0;

        if (s->sh.slice_type != I_SLICE)
            lc->cu.pred_mode = ff_hevc_pred_mode_decode(s);
        if (lc->cu.pred_mode != MODE_INTRA ||
            log2_cb_size == s->ps.sps->log2_min_cb_size) {
            lc->cu.part_mode        = ff_hevc_part_mode_decode(s, log2_cb_size);
            lc->cu.intra_split_flag = lc->cu.part_mode == PART_NxN &&
                                      lc->cu.pred_mode == MODE_INTRA;
        }

        if (lc->cu.pred_mode == MODE_INTRA) {
            if (lc->cu.part_mode == PART_2Nx2N && s->ps.sps->pcm_enabled_flag &&
                log2_cb_size >= s->ps.sps->pcm.log2_min_pcm_cb_size &&
                log2_cb_size <= s->ps.sps->pcm.log2_max_pcm_cb_size) {
                pcm_flag = ff_hevc_pcm_flag_decode(s);
            }
            if (pcm_flag) {
                intra_prediction_unit_default_value(s, x0, y0, log2_cb_size);
                ret = hls_pcm_sample(s, x0, y0, log2_cb_size);
                if (s->ps.sps->pcm.loop_filter_disable_flag)
                    set_deblocking_bypass(s, x0, y0, log2_cb_size);

                if (ret < 0)
                    return ret;
            } else {
                intra_prediction_unit(s, x0, y0, log2_cb_size);
            }
        } else {
            intra_prediction_unit_default_value(s, x0, y0, log2_cb_size);
            switch (lc->cu.part_mode) {
            case PART_2Nx2N:
                hls_prediction_unit(s, x0, y0, cb_size, cb_size, log2_cb_size, 0, idx);
                break;
            case PART_2NxN:
                hls_prediction_unit(s, x0, y0,               cb_size, cb_size / 2, log2_cb_size, 0, idx);
                hls_prediction_unit(s, x0, y0 + cb_size / 2, cb_size, cb_size / 2, log2_cb_size, 1, idx);
                break;
            case PART_Nx2N:
                hls_prediction_unit(s, x0,               y0, cb_size / 2, cb_size, log2_cb_size, 0, idx - 1);
                hls_prediction_unit(s, x0 + cb_size / 2, y0, cb_size / 2, cb_size, log2_cb_size, 1, idx - 1);
                break;
            case PART_2NxnU:
                hls_prediction_unit(s, x0, y0,               cb_size, cb_size     / 4, log2_cb_size, 0, idx);
                hls_prediction_unit(s, x0, y0 + cb_size / 4, cb_size, cb_size * 3 / 4, log2_cb_size, 1, idx);
                break;
            case PART_2NxnD:
                hls_prediction_unit(s, x0, y0,                   cb_size, cb_size * 3 / 4, log2_cb_size, 0, idx);
                hls_prediction_unit(s, x0, y0 + cb_size * 3 / 4, cb_size, cb_size     / 4, log2_cb_size, 1, idx);
                break;
            case PART_nLx2N:
                hls_prediction_unit(s, x0,               y0, cb_size     / 4, cb_size, log2_cb_size, 0, idx - 2);
                hls_prediction_unit(s, x0 + cb_size / 4, y0, cb_size * 3 / 4, cb_size, log2_cb_size, 1, idx - 2);
                break;
            case PART_nRx2N:
                hls_prediction_unit(s, x0,                   y0, cb_size * 3 / 4, cb_size, log2_cb_size, 0, idx - 2);
                hls_prediction_unit(s, x0 + cb_size * 3 / 4, y0, cb_size     / 4, cb_size, log2_cb_size, 1, idx - 2);
                break;
            case PART_NxN:
                hls_prediction_unit(s, x0,               y0,               cb_size / 2, cb_size / 2, log2_cb_size, 0, idx - 1);
                hls_prediction_unit(s, x0 + cb_size / 2, y0,               cb_size / 2, cb_size / 2, log2_cb_size, 1, idx - 1);
                hls_prediction_unit(s, x0,               y0 + cb_size / 2, cb_size / 2, cb_size / 2, log2_cb_size, 2, idx - 1);
                hls_prediction_unit(s, x0 + cb_size / 2, y0 + cb_size / 2, cb_size / 2, cb_size / 2, log2_cb_size, 3, idx - 1);
                break;
            }
        }

        if (!pcm_flag) {
            int rqt_root_cbf = 1;

            if (lc->cu.pred_mode != MODE_INTRA &&
                !(lc->cu.part_mode == PART_2Nx2N && lc->pu.merge_flag)) {
                rqt_root_cbf = ff_hevc_no_residual_syntax_flag_decode(s);
            }
            if (rqt_root_cbf) {
                const static int cbf[2] = { 0 };
                lc->cu.max_trafo_depth = lc->cu.pred_mode == MODE_INTRA ?
                                         s->ps.sps->max_transform_hierarchy_depth_intra + lc->cu.intra_split_flag :
                                         s->ps.sps->max_transform_hierarchy_depth_inter;
                ret = hls_transform_tree(s, x0, y0, x0, y0, x0, y0,
                                         log2_cb_size,
                                         log2_cb_size, 0, 0, cbf, cbf);
                if (ret < 0)
                    return ret;
            } else {
                if (!s->sh.disable_deblocking_filter_flag)
                    ff_hevc_deblocking_boundary_strengths(s, x0, y0, log2_cb_size);
            }
        }
    }

    if (s->ps.pps->cu_qp_delta_enabled_flag && lc->tu.is_cu_qp_delta_coded == 0)
        ff_hevc_set_qPy(s, x0, y0, log2_cb_size);

    x = y_cb * min_cb_width + x_cb;
    for (y = 0; y < length; y++) {
        memset(&s->qp_y_tab[x], lc->qp_y, length);
        x += min_cb_width;
    }

    if(((x0 + (1<<log2_cb_size)) & qp_block_mask) == 0 &&
       ((y0 + (1<<log2_cb_size)) & qp_block_mask) == 0) {
        lc->qPy_pred = lc->qp_y;
    }

    set_ct_depth(s, x0, y0, log2_cb_size, lc->ct_depth);

    return 0;
}

static int hls_coding_quadtree(HEVCContext *s, int x0, int y0,
                               int log2_cb_size, int cb_depth)
{
    HEVCLocalContext *lc = s->HEVClc;
    const int cb_size    = 1 << log2_cb_size;
    int ret;
    int split_cu;

    lc->ct_depth = cb_depth;
    if (x0 + cb_size <= s->ps.sps->width  &&
        y0 + cb_size <= s->ps.sps->height &&
        log2_cb_size > s->ps.sps->log2_min_cb_size) {
        split_cu = ff_hevc_split_coding_unit_flag_decode(s, cb_depth, x0, y0);
    } else {
        split_cu = (log2_cb_size > s->ps.sps->log2_min_cb_size);
    }
    if (s->ps.pps->cu_qp_delta_enabled_flag &&
        log2_cb_size >= s->ps.sps->log2_ctb_size - s->ps.pps->diff_cu_qp_delta_depth) {
        lc->tu.is_cu_qp_delta_coded = 0;
        lc->tu.cu_qp_delta          = 0;
    }

    if (s->sh.cu_chroma_qp_offset_enabled_flag &&
        log2_cb_size >= s->ps.sps->log2_ctb_size - s->ps.pps->diff_cu_chroma_qp_offset_depth) {
        lc->tu.is_cu_chroma_qp_offset_coded = 0;
    }

    if (split_cu) {
        int qp_block_mask = (1<<(s->ps.sps->log2_ctb_size - s->ps.pps->diff_cu_qp_delta_depth)) - 1;
        const int cb_size_split = cb_size >> 1;
        const int x1 = x0 + cb_size_split;
        const int y1 = y0 + cb_size_split;

        int more_data = 0;

        more_data = hls_coding_quadtree(s, x0, y0, log2_cb_size - 1, cb_depth + 1);
        if (more_data < 0)
            return more_data;

        if (more_data && x1 < s->ps.sps->width) {
            more_data = hls_coding_quadtree(s, x1, y0, log2_cb_size - 1, cb_depth + 1);
            if (more_data < 0)
                return more_data;
        }
        if (more_data && y1 < s->ps.sps->height) {
            more_data = hls_coding_quadtree(s, x0, y1, log2_cb_size - 1, cb_depth + 1);
            if (more_data < 0)
                return more_data;
        }
        if (more_data && x1 < s->ps.sps->width &&
            y1 < s->ps.sps->height) {
            more_data = hls_coding_quadtree(s, x1, y1, log2_cb_size - 1, cb_depth + 1);
            if (more_data < 0)
                return more_data;
        }

        if(((x0 + (1<<log2_cb_size)) & qp_block_mask) == 0 &&
            ((y0 + (1<<log2_cb_size)) & qp_block_mask) == 0)
            lc->qPy_pred = lc->qp_y;

        if (more_data)
            return ((x1 + cb_size_split) < s->ps.sps->width ||
                    (y1 + cb_size_split) < s->ps.sps->height);
        else
            return 0;
    } else {
        ret = hls_coding_unit(s, x0, y0, log2_cb_size);
        if (ret < 0)
            return ret;
        if ((!((x0 + cb_size) %
               (1 << (s->ps.sps->log2_ctb_size))) ||
             (x0 + cb_size >= s->ps.sps->width)) &&
            (!((y0 + cb_size) %
               (1 << (s->ps.sps->log2_ctb_size))) ||
             (y0 + cb_size >= s->ps.sps->height))) {
            int end_of_slice_flag = ff_hevc_end_of_slice_flag_decode(s);
            return !end_of_slice_flag;
        } else {
            return 1;
        }
    }

    return 0;
}

static void hls_decode_neighbour(HEVCContext *s, int x_ctb, int y_ctb,
                                 int ctb_addr_ts)
{
    HEVCLocalContext *lc  = s->HEVClc;
    int ctb_size          = 1 << s->ps.sps->log2_ctb_size;
    int ctb_addr_rs       = s->ps.pps->ctb_addr_ts_to_rs[ctb_addr_ts];
    int ctb_addr_in_slice = ctb_addr_rs - s->sh.slice_addr;

    s->tab_slice_address[ctb_addr_rs] = s->sh.slice_addr;

    if (s->ps.pps->entropy_coding_sync_enabled_flag) {
        if (x_ctb == 0 && (y_ctb & (ctb_size - 1)) == 0)
            lc->first_qp_group = 1;
        lc->end_of_tiles_x = s->ps.sps->width;
    } else if (s->ps.pps->tiles_enabled_flag) {
        if (ctb_addr_ts && s->ps.pps->tile_id[ctb_addr_ts] != s->ps.pps->tile_id[ctb_addr_ts - 1]) {
            int idxX = s->ps.pps->col_idxX[x_ctb >> s->ps.sps->log2_ctb_size];
            lc->end_of_tiles_x   = x_ctb + (s->ps.pps->column_width[idxX] << s->ps.sps->log2_ctb_size);
            lc->first_qp_group   = 1;
        }
    } else {
        lc->end_of_tiles_x = s->ps.sps->width;
    }

    lc->end_of_tiles_y = FFMIN(y_ctb + ctb_size, s->ps.sps->height);

    lc->boundary_flags = 0;
    if (s->ps.pps->tiles_enabled_flag) {
        if (x_ctb > 0 && s->ps.pps->tile_id[ctb_addr_ts] != s->ps.pps->tile_id[s->ps.pps->ctb_addr_rs_to_ts[ctb_addr_rs - 1]])
            lc->boundary_flags |= BOUNDARY_LEFT_TILE;
        if (x_ctb > 0 && s->tab_slice_address[ctb_addr_rs] != s->tab_slice_address[ctb_addr_rs - 1])
            lc->boundary_flags |= BOUNDARY_LEFT_SLICE;
        if (y_ctb > 0 && s->ps.pps->tile_id[ctb_addr_ts] != s->ps.pps->tile_id[s->ps.pps->ctb_addr_rs_to_ts[ctb_addr_rs - s->ps.sps->ctb_width]])
            lc->boundary_flags |= BOUNDARY_UPPER_TILE;
        if (y_ctb > 0 && s->tab_slice_address[ctb_addr_rs] != s->tab_slice_address[ctb_addr_rs - s->ps.sps->ctb_width])
            lc->boundary_flags |= BOUNDARY_UPPER_SLICE;
    } else {
        if (ctb_addr_in_slice <= 0)
            lc->boundary_flags |= BOUNDARY_LEFT_SLICE;
        if (ctb_addr_in_slice < s->ps.sps->ctb_width)
            lc->boundary_flags |= BOUNDARY_UPPER_SLICE;
    }

    lc->ctb_left_flag = ((x_ctb > 0) && (ctb_addr_in_slice > 0) && !(lc->boundary_flags & BOUNDARY_LEFT_TILE));
    lc->ctb_up_flag   = ((y_ctb > 0) && (ctb_addr_in_slice >= s->ps.sps->ctb_width) && !(lc->boundary_flags & BOUNDARY_UPPER_TILE));
    lc->ctb_up_right_flag = ((y_ctb > 0)  && (ctb_addr_in_slice+1 >= s->ps.sps->ctb_width) && (s->ps.pps->tile_id[ctb_addr_ts] == s->ps.pps->tile_id[s->ps.pps->ctb_addr_rs_to_ts[ctb_addr_rs+1 - s->ps.sps->ctb_width]]));
    lc->ctb_up_left_flag = ((x_ctb > 0) && (y_ctb > 0)  && (ctb_addr_in_slice-1 >= s->ps.sps->ctb_width) && (s->ps.pps->tile_id[ctb_addr_ts] == s->ps.pps->tile_id[s->ps.pps->ctb_addr_rs_to_ts[ctb_addr_rs-1 - s->ps.sps->ctb_width]]));
}

#ifdef RPI
static void rpi_execute_dblk_cmds(HEVCContext *s)
{
    int n;
    int job = s->pass1_job;
    int ctb_size    = 1 << s->ps.sps->log2_ctb_size;
    int (*p)[2] = s->dblk_cmds[job];
    for(n = s->num_dblk_cmds[job]; n>0 ;n--,p++) {
        ff_hevc_hls_filters(s, (*p)[0], (*p)[1], ctb_size);
    }
    s->num_dblk_cmds[job] = 0;
}

static void rpi_execute_transform(HEVCContext *s)
{
    int i=2;
    int job = s->pass1_job;
    /*int j;
    int16_t *coeffs = s->coeffs_buf_arm[job][i];
    for(j=s->num_coeffs[job][i]; j > 0; j-= 16*16, coeffs+=16*16) {
        s->hevcdsp.idct[4-2](coeffs, 16);
    }
    i=3;
    coeffs = s->coeffs_buf_arm[job][i] - s->num_coeffs[job][i];
    for(j=s->num_coeffs[job][i]; j > 0; j-= 32*32, coeffs+=32*32) {
        s->hevcdsp.idct[5-2](coeffs, 32);
    }*/

    gpu_cache_flush(&s->coeffs_buf_accelerated[job]);
    s->vpu_id = vpu_post_code( vpu_get_fn(), vpu_get_constants(), s->coeffs_buf_vc[job][2],
                               s->num_coeffs[job][2] >> 8, s->coeffs_buf_vc[job][3] - sizeof(int16_t) * s->num_coeffs[job][3],
                               s->num_coeffs[job][3] >> 10, 0, &s->coeffs_buf_accelerated[job]);
    //vpu_execute_code( vpu_get_fn(), vpu_get_constants(), s->coeffs_buf_vc[2], s->num_coeffs[2] >> 8, s->coeffs_buf_vc[3], s->num_coeffs[3] >> 10, 0);
    //gpu_cache_flush(&s->coeffs_buf_accelerated);
    //vpu_wait(s->vpu_id);

    for(i=0;i<4;i++)
        s->num_coeffs[job][i] = 0;
}

static void rpi_execute_pred_cmds(HEVCContext *s)
{
  int i;
  int job = s->pass1_job;
  HEVCPredCmd *cmd = s->univ_pred_cmds[job];
#ifdef RPI_WORKER
  HEVCLocalContextIntra *lc = &s->HEVClcIntra;
#else
  HEVCLocalContext *lc = s->HEVClc;
#endif

  for(i = s->num_pred_cmds[job]; i > 0; i--, cmd++) {
      //printf("i=%d cmd=%p job1=%d job0=%d\n",i,cmd,s->pass1_job,s->pass0_job);
      if (cmd->type == RPI_PRED_INTRA) {
          lc->tu.intra_pred_mode_c = lc->tu.intra_pred_mode = cmd->mode;
          lc->na.cand_bottom_left  = (cmd->na >> 4) & 1;
          lc->na.cand_left         = (cmd->na >> 3) & 1;
          lc->na.cand_up_left      = (cmd->na >> 2) & 1;
          lc->na.cand_up           = (cmd->na >> 1) & 1;
          lc->na.cand_up_right     = (cmd->na >> 0) & 1;
          s->hpc.intra_pred[cmd->size - 2](s, cmd->x, cmd->y, cmd->c_idx);
      } else {
#ifdef RPI_PRECLEAR
          int trafo_size = 1 << cmd->size;
#endif
          s->hevcdsp.transform_add[cmd->size-2](cmd->dst, cmd->buf, cmd->stride);
#ifdef RPI_PRECLEAR
          memset(cmd->buf, 0, trafo_size * trafo_size * sizeof(int16_t)); // Clear coefficients here while they are in the cache
#endif
      }
  }
  s->num_pred_cmds[job] = 0;
}

static void rpi_execute_inter_cmds(HEVCContext *s)
{
    int job = s->pass1_job;
    HEVCMvCmd *cmd = s->unif_mv_cmds[job];
    int n,cidx;
    AVFrame myref;
    AVFrame myref1;
    struct MvField mymv;
    if (s->num_mv_cmds[job] > RPI_MAX_MV_CMDS) {
        printf("Overflow inter_cmds\n");
        exit(-1);
    }
    for(n = s->num_mv_cmds[job]; n>0 ; n--, cmd++) {
        switch(cmd->cmd) {
        case RPI_CMD_LUMA_UNI:
            myref.data[0] = cmd->src;
            myref.linesize[0] = cmd->srcstride;
            luma_mc_uni(s, cmd->dst, cmd->dststride, &myref, &cmd->mv, cmd->x_off, cmd->y_off, cmd->block_w, cmd->block_h, cmd->weight, cmd->offset);
            break;
        case RPI_CMD_LUMA_BI:
            myref.data[0] = cmd->src;
            myref.linesize[0] = cmd->srcstride;
            myref1.data[0] = cmd->src1;
            myref1.linesize[0] = cmd->srcstride1;
            mymv.ref_idx[0] = cmd->ref_idx[0];
            mymv.ref_idx[1] = cmd->ref_idx[1];
            luma_mc_bi(s, cmd->dst, cmd->dststride,
                       &myref, &cmd->mv, cmd->x_off, cmd->y_off, cmd->block_w, cmd->block_h,
                       &myref1, &cmd->mv1, &mymv);
            break;
        case RPI_CMD_CHROMA_UNI:
            mymv.mv[0] = cmd->mv;
            chroma_mc_uni(s, cmd->dst,
                          cmd->dststride, cmd->src, cmd->srcstride, 0,
                          cmd->x_off, cmd->y_off, cmd->block_w, cmd->block_h, &mymv, cmd->weight, cmd->offset);
            break;
        case RPI_CMD_CHROMA_BI:
        case RPI_CMD_CHROMA_BI+1:
            cidx = cmd->cmd - RPI_CMD_CHROMA_BI;
            myref.data[cidx+1] = cmd->src;
            myref.linesize[cidx+1] = cmd->srcstride;
            myref1.data[cidx+1] = cmd->src1;
            myref1.linesize[cidx+1] = cmd->srcstride1;
            mymv.ref_idx[0] = cmd->ref_idx[0];
            mymv.ref_idx[1] = cmd->ref_idx[1];
            mymv.mv[0] = cmd->mv;
            mymv.mv[1] = cmd->mv1;
            chroma_mc_bi(s, cmd->dst, cmd->dststride, &myref, &myref1,
                         cmd->x_off, cmd->y_off, cmd->block_w, cmd->block_h, &mymv, cidx);
            break;
        }
    }
    s->num_mv_cmds[job] = 0;
}

static void rpi_do_all_passes(HEVCContext *s)
{
    // Kick off QPUs and VPUs
    rpi_launch_vpu_qpu(s);
    // Perform luma inter prediction
    rpi_execute_inter_cmds(s);
    // Wait for transform completion
    vpu_wait(s->vpu_id);
    // Perform intra prediction and residual reconstruction
    rpi_execute_pred_cmds(s);
    // Perform deblocking for CTBs in this row
    rpi_execute_dblk_cmds(s);
    // Prepare next batch
    rpi_begin(s);
}

#endif

#ifdef RPI
static void rpi_begin(HEVCContext *s)
{
    int job = s->pass0_job;
    int i;
#ifdef RPI_INTER_QPU
    int pic_width        = s->ps.sps->width >> s->ps.sps->hshift[1];
    int pic_height       = s->ps.sps->height >> s->ps.sps->vshift[1];
    int weight_flag      = (s->sh.slice_type == P_SLICE && s->ps.pps->weighted_pred_flag) ||
                           (s->sh.slice_type == B_SLICE && s->ps.pps->weighted_bipred_flag);

    for(i=0;i<8;i++) {
        s->u_mvs[job][i] = s->mvs_base[job][i];
        *s->u_mvs[job][i]++ = 0;  // next_kernel
        *s->u_mvs[job][i]++ = 0;  // x
        *s->u_mvs[job][i]++ = 0;  // y
        *s->u_mvs[job][i]++ = 0;  // ref_u_base
        *s->u_mvs[job][i]++ = 0;  // ref_y_base
        *s->u_mvs[job][i]++ = pic_width;   // frame_width
        *s->u_mvs[job][i]++ = pic_height;  // frame_height
#if RPI_AUX_FRAME_USE
//        *s->u_mvs[job][i]++ = 0;
        *s->u_mvs[job][i]++ = rpi_auxframe_stride_c(s->frame) >> (RPI_AUX_FRAME_XBLK_SHIFT - 1); // pitch
#else
        *s->u_mvs[job][i]++ = s->frame->linesize[1];  // src_pitch
#endif
        *s->u_mvs[job][i]++ = s->frame->linesize[1];  // dest_pitch
        if (weight_flag) {
            *s->u_mvs[job][i]++ = 1 << (s->sh.chroma_log2_weight_denom + 6 - 1);
            *s->u_mvs[job][i]++ = s->sh.chroma_log2_weight_denom + 6;
        } else {
            *s->u_mvs[job][i]++ = 1 << 5;
            *s->u_mvs[job][i]++ = 6;
        }
        *s->u_mvs[job][i]++ = i;  // Select section of VPM (avoid collisions with 3d unit)
    }
    s->curr_u_mvs = s->u_mvs[job][0];
#endif

#ifdef RPI_LUMA_QPU
    for(i=0;i<12;i++) {
        s->y_mvs[job][i] = s->y_mvs_base[job][i];
        *s->y_mvs[job][i]++ = 0; // y_x
        *s->y_mvs[job][i]++ = 0; // ref_y_base
        *s->y_mvs[job][i]++ = 0; // y2_x2
        *s->y_mvs[job][i]++ = 0; // ref_y2_base
        *s->y_mvs[job][i]++ = (s->ps.sps->width << 16) + s->ps.sps->height;
#if RPI_AUX_FRAME_USE
//        *s->y_mvs[job][i]++ = 0;
        *s->y_mvs[job][i]++ = rpi_auxframe_stride_y(s->frame) >> RPI_AUX_FRAME_XBLK_SHIFT; // pitch
#else
        *s->y_mvs[job][i]++ = s->frame->linesize[0]; // pitch
#endif
        *s->y_mvs[job][i]++ = s->frame->linesize[0]; // dst_pitch
        if (weight_flag) {
            int offset = 1 << (s->sh.luma_log2_weight_denom + 6 - 1);
            int shift = s->sh.luma_log2_weight_denom + 6;
            *s->y_mvs[job][i]++ = (offset << 16) + shift;
        } else {
            int offset = 1 << 5;
            int shift = 6;
            *s->y_mvs[job][i]++ = (offset << 16) + shift;
        }
        *s->y_mvs[job][i]++ = 0; // Next kernel
    }
    s->curr_y_mvs = s->y_mvs[job][0];
#endif
    s->ctu_count = 0;
}
#endif

#ifdef RPI_SIMULATE_QPUS

static int32_t clipx(int x,int FRAME_WIDTH)
{
	if (x<=0) return 0;
	if (x>=FRAME_WIDTH) return FRAME_WIDTH-1;
	return x;
}

static int32_t clipy(int y,int FRAME_HEIGHT)
{
	if (y<=0) return 0;
	if (y>=FRAME_HEIGHT) return FRAME_HEIGHT-1;
	return y;
}

/*static int32_t filter8(uint8_t *data, int x0, int y0, int pitch, int mx, int my,int round,int denom,int weight,int offset)
{
   int32_t vsum = 0;
   int x, y;

   for (y = 0; y < 8; y++) {
      int32_t hsum = 0;

      for (x = 0; x < 8; x++)
         hsum += lumaFilter[mx][x]*data[clipx(x + x0) + clipy(y + y0) * pitch];

      vsum += lumaFilter[my][y]*hsum;
   }
   vsum >>= 6;
   vsum = (((vsum*weight)+round)>>denom)+offset;

   return av_clip_uint8( vsum );
}*/

static int32_t filter8_chroma(uint8_t *data, int x0, int y0, int pitch, int hcoeffs, int vcoeffs,int offset_weight,int offset_before,int denom,int pic_width, int pic_height)
{
  int32_t vsum = 0;
  int x, y;
  int chromaFilterH[4];
  int chromaFilterV[4];
  int i;
  int offset_after = offset_weight>>16;
  int weight = (offset_weight<<16)>>16;
  for(i=0;i<4;i++) {
    chromaFilterH[i] = ((hcoeffs>>(8*i))<<24)>>24;
    chromaFilterV[i] = ((vcoeffs>>(8*i))<<24)>>24;
  }

   for (y = 0; y < 4; y++) {
      int32_t hsum = 0;

      for (x = 0; x < 4; x++)
         hsum += chromaFilterH[x]*data[clipx(x + x0,pic_width) + clipy(y + y0,pic_height) * pitch];

      vsum += chromaFilterV[y]*hsum;
   }
   vsum >>= 6;
   vsum = (((vsum*weight)+offset_before)>>denom)+offset_after;

   return vsum;
}

int lumaFilter[4][8]={ {0,0,0,64,0,0,0,0},{-1,4,-10,58,17,-5,1,0},{-1,4,-11,40,40,-11,4,-1},{0,1,-5,17,58,-10,4,-1} };

static int32_t filter8_luma(uint8_t *data, int x0, int y0, int pitch, int my_mx,int offset_weight,int offset_before,int denom,int pic_width, int pic_height)
{
  int32_t vsum = 0;
  int x, y;
  int i;
  int offset_after = offset_weight>>16;
  int weight = (offset_weight<<16)>>16;

   for (y = 0; y < 8; y++) {
      int32_t hsum = 0;

      for (x = 0; x < 8; x++)
         hsum += lumaFilter[my_mx&3][x]*data[clipx(x + x0,pic_width) + clipy(y + y0,pic_height) * pitch];

      vsum += lumaFilter[(my_mx>>8)&3][y]*hsum;
   }
   vsum >>= 6;
   vsum = (((vsum*weight)+offset_before)>>denom)+offset_after;

   return vsum;
}

static uint8_t *test_frame(HEVCContext *s,uint32_t p, AVFrame *frame, const int cIdx)
{
  //int pic_width        = s->ps.sps->width >> s->ps.sps->hshift[cIdx];
  int pic_height       = s->ps.sps->height >> s->ps.sps->vshift[cIdx];
  int pitch = frame->linesize[cIdx];
  uint32_t base = c_idx == 0 ? get_vc_address_y(frame);
    c_idx == 1 ? get_vc_address_u(frame) : get_vc_address_v(frame);
  if (p>=base && p<base+pitch*pic_height) {
    return frame->data[cIdx] + (p-base);
  }
  return NULL;
}

static uint8_t *compute_arm_addr(HEVCContext *s,uint32_t p, int cIdx)
{
  SliceHeader *sh   = &s->sh;
  uint8_t *arm = test_frame(s,p,s->frame,cIdx);
  int i;
  if (arm) return arm;
  if (sh->slice_type == P_SLICE || sh->slice_type == B_SLICE)
  {
    for(i=0;i<sh->nb_refs[L0];i++) {
      arm = test_frame(s,p,s->ref->refPicList[0].ref[i]->frame,cIdx);
      if (arm) return arm;
    }
  }
  if (sh->slice_type == B_SLICE) {
    for(i=0;i<sh->nb_refs[L1];i++) {
      arm = test_frame(s,p,s->ref->refPicList[1].ref[i]->frame,cIdx);
      if (arm) return arm;
    }
  }
  printf("Frame 0x%x not found! Exit=%x\n",p,qpu_get_fn(QPU_MC_EXIT));
  exit(-1);
  return NULL;
}

static void rpi_simulate_inter_chroma(HEVCContext *s,uint32_t *p)
{
  uint32_t next_kernel;
  uint32_t x0;
  uint32_t y0;
  uint8_t *ref_u_base;
  uint8_t *ref_v_base;
  uint32_t frame_width = p[5];
  uint32_t frame_height = p[6];
  uint32_t pitch = p[7];
  uint32_t dst_pitch = p[8];
  int32_t offset_before = p[9];
  int32_t denom = p[10];
  uint32_t vpm_id = p[11];
  uint32_t tmp_u_dst[256];
  uint32_t tmp_v_dst[256];
  while(1) {
    p += 12;
    next_kernel = p[0-12];
    x0 = p[1-12];
    y0 = p[2-12];
    if (next_kernel==s->mc_filter_uv || next_kernel==s->mc_filter_uv_b0 || next_kernel==s->mc_filter_uv_b) {
      int x,y;
      uint32_t width_height = p[5];
      uint32_t hcoeffs = p[6];
      uint32_t vcoeffs = p[7];
      uint32_t offset_weight_u = p[8];
      uint32_t offset_weight_v = p[9];
      uint8_t *this_u_dst;
      uint8_t *this_v_dst;
      uint32_t width = width_height >> 16;
      uint32_t height = (width_height << 16) >> 16;
      ref_u_base = compute_arm_addr(s,p[3-12],1);
      ref_v_base = compute_arm_addr(s,p[4-12],2);
      if (next_kernel!=s->mc_filter_uv_b0)
      {
        this_u_dst = compute_arm_addr(s,p[10],1);
        this_v_dst = compute_arm_addr(s,p[11],2);
      }
      for (y=0; y<height; ++y) {
        for (x=0; x<width; ++x) {
          if (next_kernel==s->mc_filter_uv) {
            int32_t refa = filter8_chroma(ref_u_base,x+x0, y+y0, pitch, hcoeffs, vcoeffs, offset_weight_u,offset_before,denom,frame_width,frame_height);
            int32_t refb = filter8_chroma(ref_v_base,x+x0, y+y0, pitch, hcoeffs, vcoeffs, offset_weight_v,offset_before,denom,frame_width,frame_height);
            this_u_dst[x+y*dst_pitch] = av_clip_uint8(refa);
            this_v_dst[x+y*dst_pitch] = av_clip_uint8(refb);
          } else if (next_kernel==s->mc_filter_uv_b0) {
            int32_t refa = filter8_chroma(ref_u_base, x+x0, y+y0, pitch, hcoeffs, vcoeffs, 1,0,0,frame_width,frame_height);
            int32_t refb = filter8_chroma(ref_v_base, x+x0, y+y0, pitch, hcoeffs, vcoeffs, 1,0,0,frame_width,frame_height);
            tmp_u_dst[x+y*16] = refa;
            tmp_v_dst[x+y*16] = refb;
          } else {
            int32_t refa = filter8_chroma(ref_u_base, x+x0, y+y0, pitch, hcoeffs, vcoeffs, 1, 64 + tmp_u_dst[x+y*16], 7, frame_width, frame_height);
            int32_t refb = filter8_chroma(ref_v_base, x+x0, y+y0, pitch, hcoeffs, vcoeffs, 1, 64 + tmp_v_dst[x+y*16], 7, frame_width, frame_height);
            this_u_dst[x+y*dst_pitch] = av_clip_uint8(refa);
            this_v_dst[x+y*dst_pitch] = av_clip_uint8(refb);
          }
        }
      }
    } else {
      av_assert0(next_kernel==qpu_get_fn(QPU_MC_INTERRUPT_EXIT8) || next_kernel==qpu_get_fn(QPU_MC_EXIT) );
      break;
    }
  }
}

// mc_setup(y_x, ref_y_base, y2_x2, ref_y2_base, frame_width_height, pitch, dst_pitch, offset_shift, next_kernel)
static void rpi_simulate_inter_luma(HEVCContext *s,uint32_t *p,int chan)
{
  uint32_t next_kernel;
  int y_x,y2_x2;
  int x0;
  int y0;
  int x2;
  int y2;
  uint32_t *p0 = p;
  uint8_t *ref_y_base;
  uint8_t *ref_y2_base;
  uint32_t frame_width_height = p[4];
  uint32_t frame_width = frame_width_height>>16;
  uint32_t frame_height = (frame_width_height<<16)>>16;
  uint32_t pitch = p[5];
  uint32_t dst_pitch = p[6];
  int offset_shift = p[7];
  int32_t offset_before = offset_shift>>16;
  int32_t denom = (offset_shift<<16)>>16;
  while(1) {
    p += 9;
    next_kernel = p[8-9];
    y_x = p[0-9];
    x0 = (y_x<<16)>>16;
    y0 = y_x>>16;
    y2_x2 = p[2-9];
    x2 = (y2_x2<<16)>>16;
    y2 = y2_x2>>16;

    if (next_kernel==s->mc_filter || next_kernel==s->mc_filter_b) {
      // y_x, frame_base, y2_x2, frame_base2, width_height, my2_mx2_my_mx, offsetweight0, this_dst, next_kernel)
      int x,y;
      uint32_t width_height = p[4];
      uint32_t my2_mx2_my_mx = p[5];
      uint32_t offset_weight = p[6];
      uint8_t *this_dst = compute_arm_addr(s,p[7],0);
      uint32_t width = width_height >> 16;
      uint32_t height = (width_height << 16) >> 16;
      uint8_t *dst_base = s->frame->data[0];
      ref_y_base = compute_arm_addr(s,p[1-9],0);
      ref_y2_base = compute_arm_addr(s,p[3-9],0);
      for (y=0; y<height; ++y) {
        for (x=0; x<width; ++x) {
          if (next_kernel==s->mc_filter) {
            int32_t refa = filter8_luma(ref_y_base,x+x0, y+y0, pitch, my2_mx2_my_mx, offset_weight,offset_before,denom,frame_width,frame_height);
            refa = av_clip_uint8(refa);
            this_dst[x+y*dst_pitch] = refa;
          }
          else {
            int32_t refa = filter8_luma(ref_y_base, x+x0, y+y0, pitch, my2_mx2_my_mx, 1, 0, 0, frame_width, frame_height);
            int32_t refb = filter8_luma(ref_y2_base, x+x2, y+y2, pitch, my2_mx2_my_mx>>16, 1, 64 + refa, 7, frame_width, frame_height);
            this_dst[x+y*dst_pitch] = av_clip_uint8(refb);
          }
        }
      }
    } else {
      av_assert0(next_kernel==qpu_get_fn(QPU_MC_INTERRUPT_EXIT12) || next_kernel==qpu_get_fn(QPU_MC_EXIT) );
      break;
    }
  }
}

static void rpi_simulate_inter_qpu(HEVCContext *s)
{
  // First run the transform as normal
  int i;
  rpi_execute_transform(s);
  for(i=0;i<8;i++)
  {
    rpi_simulate_inter_chroma(s,s->mvs_base[i]);
  }
  for(i=0;i<12;i++)
  {
    rpi_simulate_inter_luma(s,s->y_mvs_base[i],i);
  }
}

#endif

#ifdef RPI_INTER_QPU

static void rpi_launch_vpu_qpu(HEVCContext *s)
{
    int k;
    int job = s->pass1_job;
    int i;
    uint32_t *unif_vc = (uint32_t *)s->unif_mvs_ptr[job].vc;
#ifdef RPI_LUMA_QPU
    uint32_t *y_unif_vc = (uint32_t *)s->y_unif_mvs_ptr[job].vc;
#endif
    if (s->sh.slice_type == I_SLICE) {
#ifdef RPI_MULTI_MAILBOX
      rpi_execute_transform(s);
      return;
#endif
    }
    for(k=0;k<8;k++) {
        s->u_mvs[job][k][-RPI_CHROMA_COMMAND_WORDS] = qpu_get_fn(QPU_MC_EXIT); // Add exit command
        s->u_mvs[job][k][-RPI_CHROMA_COMMAND_WORDS+3] = qpu_get_fn(QPU_MC_SETUP_UV); // A dummy texture location (maps to our code) - this is needed as the texture requests are pipelined
        s->u_mvs[job][k][-RPI_CHROMA_COMMAND_WORDS+4] = qpu_get_fn(QPU_MC_SETUP_UV); // Also need a dummy for V
        av_assert0(s->u_mvs[job][k] - s->mvs_base[job][k] < UV_COMMANDS_PER_QPU);
    }

    s->u_mvs[job][8-1][-RPI_CHROMA_COMMAND_WORDS] = qpu_get_fn(QPU_MC_INTERRUPT_EXIT8); // This QPU will signal interrupt when all others are done and have acquired a semaphore

#ifdef RPI_LUMA_QPU
    for(k=0;k<12;k++) {
        s->y_mvs[job][k][-RPI_LUMA_COMMAND_WORDS+1] = qpu_get_fn(QPU_MC_SETUP_UV); // A dummy texture location (maps to our code) - this is needed as the texture requests are pipelined
        s->y_mvs[job][k][-RPI_LUMA_COMMAND_WORDS+3] = qpu_get_fn(QPU_MC_SETUP_UV); // Also need a dummy for second request
        s->y_mvs[job][k][-RPI_LUMA_COMMAND_WORDS+8] = qpu_get_fn(QPU_MC_EXIT); // Add exit command
        av_assert0(s->y_mvs[job][k] - s->y_mvs_base[job][k] < Y_COMMANDS_PER_QPU);
    }
    s->y_mvs[job][12-1][-RPI_LUMA_COMMAND_WORDS+8] = qpu_get_fn(QPU_MC_INTERRUPT_EXIT12); // This QPU will signal interrupt when all others are done and have acquired a semaphore
#endif

#ifdef RPI_SIMULATE_QPUS
    rpi_simulate_inter_qpu(s);
    return;
#endif

#ifdef RPI_MULTI_MAILBOX
#ifdef RPI_CACHE_UNIF_MVS
    flush_frame3(s, s->frame,&s->coeffs_buf_accelerated[job],&s->y_unif_mvs_ptr[job], &s->unif_mvs_ptr[job], job);
#else
    flush_frame3(s, s->frame,&s->coeffs_buf_accelerated[job],NULL,NULL, job);
#endif
    s->vpu_id = vpu_qpu_post_code( vpu_get_fn(), vpu_get_constants(), s->coeffs_buf_vc[job][2], s->num_coeffs[job][2] >> 8,
                                                                      s->coeffs_buf_vc[job][3] - sizeof(int16_t) * s->num_coeffs[job][3], s->num_coeffs[job][3] >> 10, 0,
                                   qpu_get_fn(QPU_MC_SETUP_UV),
                                   (uint32_t)(unif_vc+(s->mvs_base[job][0 ] - (uint32_t*)s->unif_mvs_ptr[job].arm)),
                                   (uint32_t)(unif_vc+(s->mvs_base[job][1 ] - (uint32_t*)s->unif_mvs_ptr[job].arm)),
                                   (uint32_t)(unif_vc+(s->mvs_base[job][2 ] - (uint32_t*)s->unif_mvs_ptr[job].arm)),
                                   (uint32_t)(unif_vc+(s->mvs_base[job][3 ] - (uint32_t*)s->unif_mvs_ptr[job].arm)),
                                   (uint32_t)(unif_vc+(s->mvs_base[job][4 ] - (uint32_t*)s->unif_mvs_ptr[job].arm)),
                                   (uint32_t)(unif_vc+(s->mvs_base[job][5 ] - (uint32_t*)s->unif_mvs_ptr[job].arm)),
                                   (uint32_t)(unif_vc+(s->mvs_base[job][6 ] - (uint32_t*)s->unif_mvs_ptr[job].arm)),
                                   (uint32_t)(unif_vc+(s->mvs_base[job][7 ] - (uint32_t*)s->unif_mvs_ptr[job].arm)),
#ifdef RPI_LUMA_QPU
                                   qpu_get_fn(QPU_MC_SETUP),
                                   (uint32_t)(y_unif_vc+(s->y_mvs_base[job][0 ] - (uint32_t*)s->y_unif_mvs_ptr[job].arm)),
                                   (uint32_t)(y_unif_vc+(s->y_mvs_base[job][1 ] - (uint32_t*)s->y_unif_mvs_ptr[job].arm)),
                                   (uint32_t)(y_unif_vc+(s->y_mvs_base[job][2 ] - (uint32_t*)s->y_unif_mvs_ptr[job].arm)),
                                   (uint32_t)(y_unif_vc+(s->y_mvs_base[job][3 ] - (uint32_t*)s->y_unif_mvs_ptr[job].arm)),
                                   (uint32_t)(y_unif_vc+(s->y_mvs_base[job][4 ] - (uint32_t*)s->y_unif_mvs_ptr[job].arm)),
                                   (uint32_t)(y_unif_vc+(s->y_mvs_base[job][5 ] - (uint32_t*)s->y_unif_mvs_ptr[job].arm)),
                                   (uint32_t)(y_unif_vc+(s->y_mvs_base[job][6 ] - (uint32_t*)s->y_unif_mvs_ptr[job].arm)),
                                   (uint32_t)(y_unif_vc+(s->y_mvs_base[job][7 ] - (uint32_t*)s->y_unif_mvs_ptr[job].arm)),
                                   (uint32_t)(y_unif_vc+(s->y_mvs_base[job][8 ] - (uint32_t*)s->y_unif_mvs_ptr[job].arm)),
                                   (uint32_t)(y_unif_vc+(s->y_mvs_base[job][9 ] - (uint32_t*)s->y_unif_mvs_ptr[job].arm)),
                                   (uint32_t)(y_unif_vc+(s->y_mvs_base[job][10 ] - (uint32_t*)s->y_unif_mvs_ptr[job].arm)),
                                   (uint32_t)(y_unif_vc+(s->y_mvs_base[job][11 ] - (uint32_t*)s->y_unif_mvs_ptr[job].arm))
#else
                                   0,
                                   0,0,0,0,
                                   0,0,0,0,
                                   0,0,0,0
#endif
                                 );
    for(i=0;i<4;i++)
        s->num_coeffs[job][i] = 0;
#else
    qpu_run_shader8(qpu_get_fn(QPU_MC_SETUP_UV),
      (uint32_t)(unif_vc+(s->mvs_base[job][0 ] - (uint32_t*)s->unif_mvs_ptr[job].arm)),
      (uint32_t)(unif_vc+(s->mvs_base[job][1 ] - (uint32_t*)s->unif_mvs_ptr[job].arm)),
      (uint32_t)(unif_vc+(s->mvs_base[job][2 ] - (uint32_t*)s->unif_mvs_ptr[job].arm)),
      (uint32_t)(unif_vc+(s->mvs_base[job][3 ] - (uint32_t*)s->unif_mvs_ptr[job].arm)),
      (uint32_t)(unif_vc+(s->mvs_base[job][4 ] - (uint32_t*)s->unif_mvs_ptr[job].arm)),
      (uint32_t)(unif_vc+(s->mvs_base[job][5 ] - (uint32_t*)s->unif_mvs_ptr[job].arm)),
      (uint32_t)(unif_vc+(s->mvs_base[job][6 ] - (uint32_t*)s->unif_mvs_ptr[job].arm)),
      (uint32_t)(unif_vc+(s->mvs_base[job][7 ] - (uint32_t*)s->unif_mvs_ptr[job].arm))
      );
#endif


}
#else

#ifdef RPI
static void rpi_launch_vpu_qpu(HEVCContext *s)
{
  rpi_execute_transform(s);
}
#endif

#endif

#ifdef RPI

#ifndef RPI_FAST_CACHEFLUSH
#error RPI_FAST_CACHEFLUSH is broken
static void flush_buffer(AVBufferRef *bref) {
    GPU_MEM_PTR_T *p = av_buffer_pool_opaque(bref);
    gpu_cache_flush(p);
}
#endif

static void flush_frame(HEVCContext *s,AVFrame *frame)
{
#ifdef RPI_FAST_CACHEFLUSH
    struct vcsm_user_clean_invalid_s iocache = {};
    GPU_MEM_PTR_T p = get_gpu_mem_ptr_u(s->frame);
    int n = s->ps.sps->height;
    int curr_y = 0;
    int curr_uv = 0;
    int n_uv = n >> s->ps.sps->vshift[1];
    int sz,base;
    sz = s->frame->linesize[1] * (n_uv-curr_uv);
    base = s->frame->linesize[1] * curr_uv;
    iocache.s[0].handle = p.vcsm_handle;
    iocache.s[0].cmd = 3; // clean+invalidate
    iocache.s[0].addr = (int)(p.arm) + base;
    iocache.s[0].size  = sz;
    p = get_gpu_mem_ptr_v(s->frame);
    iocache.s[1].handle = p.vcsm_handle;
    iocache.s[1].cmd = 3; // clean+invalidate
    iocache.s[1].addr = (int)(p.arm) + base;
    iocache.s[1].size  = sz;
    p = get_gpu_mem_ptr_y(s->frame);
    sz = s->frame->linesize[0] * (n-curr_y);
    base = s->frame->linesize[0] * curr_y;
    iocache.s[2].handle = p.vcsm_handle;
    iocache.s[2].cmd = 3; // clean+invalidate
    iocache.s[2].addr = (int)(p.arm) + base;
    iocache.s[2].size  = sz;
    vcsm_clean_invalid( &iocache );
#else
    flush_buffer(frame->buf[0]);
    flush_buffer(frame->buf[1]);
    flush_buffer(frame->buf[2]);
#endif
}

static void flush_frame3(HEVCContext *s,AVFrame *frame,GPU_MEM_PTR_T *p0,GPU_MEM_PTR_T *p1,GPU_MEM_PTR_T *p2, int job)
{
#ifdef RPI_FAST_CACHEFLUSH
    struct vcsm_user_clean_invalid_s iocache = {};
    int n;
    int curr_y;
    int curr_uv;
    int n_uv;
    GPU_MEM_PTR_T p = get_gpu_mem_ptr_u(s->frame);
    int sz,base;
    int (*d)[2] = s->dblk_cmds[job];
    int low=(*d)[1];
    int high=(*d)[1];
    for(n = s->num_dblk_cmds[job]; n>0 ;n--,d++) {
        int y = (*d)[1];
        low=FFMIN(low,y);
        high=FFMAX(high,y);
    }
    curr_y = low;
    n = high+(1 << s->ps.sps->log2_ctb_size);
    curr_uv = curr_y >> s->ps.sps->vshift[1];
    n_uv = n >> s->ps.sps->vshift[1];

    sz = s->frame->linesize[1] * (n_uv-curr_uv);
    base = s->frame->linesize[1] * curr_uv;
    iocache.s[0].handle = p.vcsm_handle;
    iocache.s[0].cmd = 3; // clean+invalidate
    iocache.s[0].addr = (int)(p.arm) + base;
    iocache.s[0].size  = sz;
    p = get_gpu_mem_ptr_v(s->frame);
    iocache.s[1].handle = p.vcsm_handle;
    iocache.s[1].cmd = 3; // clean+invalidate
    iocache.s[1].addr = (int)(p.arm) + base;
    iocache.s[1].size  = sz;
    p = get_gpu_mem_ptr_y(s->frame);
    sz = s->frame->linesize[0] * (n-curr_y);
    base = s->frame->linesize[0] * curr_y;
    iocache.s[2].handle = p.vcsm_handle;
    iocache.s[2].cmd = 3; // clean+invalidate
    iocache.s[2].addr = (int)(p.arm) + base;
    iocache.s[2].size  = sz;

    iocache.s[3].handle = p0->vcsm_handle;
    iocache.s[3].cmd = 3; // clean+invalidate
    iocache.s[3].addr = (int) p0->arm;
    iocache.s[3].size  = p0->numbytes;
    if (p1) {
      iocache.s[4].handle = p1->vcsm_handle;
      iocache.s[4].cmd = 3; // clean+invalidate
      iocache.s[4].addr = (int) p1->arm;
      iocache.s[4].size  = p1->numbytes;
    }
    if (p2) {
      iocache.s[5].handle = p2->vcsm_handle;
      iocache.s[5].cmd = 3; // clean+invalidate
      iocache.s[5].addr = (int) p2->arm;
      iocache.s[5].size  = p2->numbytes;
    }
    vcsm_clean_invalid( &iocache );
#else
    flush_buffer(frame->buf[0]);
    flush_buffer(frame->buf[1]);
    flush_buffer(frame->buf[2]);
    gpu_cache_flush3(p0, p1, p2);
#endif
}

#endif

static int hls_decode_entry(AVCodecContext *avctxt, void *isFilterThread)
{
    HEVCContext *s  = avctxt->priv_data;
    int ctb_size    = 1 << s->ps.sps->log2_ctb_size;
    int more_data   = 1;
    int x_ctb       = 0;
    int y_ctb       = 0;
    int ctb_addr_ts = s->ps.pps->ctb_addr_rs_to_ts[s->sh.slice_ctb_addr_rs];

#ifdef RPI
#ifdef RPI_INTER_QPU
    s->enable_rpi = s->ps.sps->bit_depth == 8
                    && !s->ps.pps->cross_component_prediction_enabled_flag
                    && !(s->ps.pps->weighted_bipred_flag && s->sh.slice_type == B_SLICE);
#else
    s->enable_rpi = s->ps.sps->bit_depth == 8
                    && !s->ps.pps->cross_component_prediction_enabled_flag;
#endif

    if (!s->enable_rpi) {
      if (s->ps.pps->cross_component_prediction_enabled_flag)
        printf("Cross component\n");
      if (s->ps.pps->weighted_bipred_flag && s->sh.slice_type == B_SLICE)
        printf("Weighted B slice\n");
    }
#endif
    //printf("L0=%d L1=%d\n",s->sh.nb_refs[L1],s->sh.nb_refs[L1]);

    if (!ctb_addr_ts && s->sh.dependent_slice_segment_flag) {
        av_log(s->avctx, AV_LOG_ERROR, "Impossible initial tile.\n");
        return AVERROR_INVALIDDATA;
    }

    if (s->sh.dependent_slice_segment_flag) {
        int prev_rs = s->ps.pps->ctb_addr_ts_to_rs[ctb_addr_ts - 1];
        if (s->tab_slice_address[prev_rs] != s->sh.slice_addr) {
            av_log(s->avctx, AV_LOG_ERROR, "Previous slice segment missing\n");
            return AVERROR_INVALIDDATA;
        }
    }

#ifdef RPI_WORKER
    s->pass0_job = 0;
    s->pass1_job = 0;
#endif
#ifdef RPI
    rpi_begin(s);
#endif

    while (more_data && ctb_addr_ts < s->ps.sps->ctb_size) {
        int ctb_addr_rs = s->ps.pps->ctb_addr_ts_to_rs[ctb_addr_ts];

        x_ctb = (ctb_addr_rs % ((s->ps.sps->width + ctb_size - 1) >> s->ps.sps->log2_ctb_size)) << s->ps.sps->log2_ctb_size;
        y_ctb = (ctb_addr_rs / ((s->ps.sps->width + ctb_size - 1) >> s->ps.sps->log2_ctb_size)) << s->ps.sps->log2_ctb_size;
        hls_decode_neighbour(s, x_ctb, y_ctb, ctb_addr_ts);

        ff_hevc_cabac_init(s, ctb_addr_ts);

        hls_sao_param(s, x_ctb >> s->ps.sps->log2_ctb_size, y_ctb >> s->ps.sps->log2_ctb_size);

        s->deblock[ctb_addr_rs].beta_offset = s->sh.beta_offset;
        s->deblock[ctb_addr_rs].tc_offset   = s->sh.tc_offset;
        s->filter_slice_edges[ctb_addr_rs]  = s->sh.slice_loop_filter_across_slices_enabled_flag;

#ifdef RPI_INTER_QPU
        s->curr_u_mvs = s->u_mvs[s->pass0_job][s->ctu_count % 8];
#endif
#ifdef RPI_LUMA_QPU
        s->curr_y_mvs = s->y_mvs[s->pass0_job][s->ctu_count % 12];
#endif

        more_data = hls_coding_quadtree(s, x_ctb, y_ctb, s->ps.sps->log2_ctb_size, 0);

#ifdef RPI_INTER_QPU
        s->u_mvs[s->pass0_job][s->ctu_count % 8]= s->curr_u_mvs;
#endif
#ifdef RPI_LUMA_QPU
        s->y_mvs[s->pass0_job][s->ctu_count % 12] = s->curr_y_mvs;
#endif

#ifdef RPI
        if (s->enable_rpi) {
          //av_assert0(s->num_dblk_cmds[s->pass0_job]>=0);
          //av_assert0(s->num_dblk_cmds[s->pass0_job]<RPI_MAX_DEBLOCK_CMDS);
          //av_assert0(s->pass0_job<RPI_MAX_JOBS);
          //av_assert0(s->pass0_job>=0);
          s->dblk_cmds[s->pass0_job][s->num_dblk_cmds[s->pass0_job]][0] = x_ctb;
          s->dblk_cmds[s->pass0_job][s->num_dblk_cmds[s->pass0_job]++][1] = y_ctb;
          s->ctu_count++;
          //printf("%d %d/%d job=%d\n",s->ctu_count,s->num_dblk_cmds[s->pass0_job],RPI_MAX_DEBLOCK_CMDS,s->pass0_job);

          if ( s->ctu_count >= s->max_ctu_count ) {
#ifdef RPI_WORKER
            if (s->used_for_ref) {
              // Split work load onto separate threads so we make as rapid progress as possible with this frame
              // Pass on this job to worker thread
              worker_submit_job(s);
              // Make sure we have space to prepare the next job
              worker_pass0_ready(s);

              // Prepare the next batch of commands
              rpi_begin(s);
            } else {
              // Non-ref frame so do it all on this thread
              rpi_do_all_passes(s);
            }
#else
            rpi_do_all_passes(s);
#endif
          }

        }
#endif


        if (more_data < 0) {
            s->tab_slice_address[ctb_addr_rs] = -1;
            return more_data;
        }


        ctb_addr_ts++;
        ff_hevc_save_states(s, ctb_addr_ts);
#ifdef RPI
        if (s->enable_rpi)
            continue;
#endif
        ff_hevc_hls_filters(s, x_ctb, y_ctb, ctb_size);
    }

#ifdef RPI

#ifdef RPI_WORKER
    // Wait for the worker to finish all its jobs
    if (s->enable_rpi) {
        worker_wait(s);
    }
#endif

    // Finish off any half-completed rows
    if (s->enable_rpi && s->ctu_count) {
        rpi_do_all_passes(s);
    }

#endif

    if (x_ctb + ctb_size >= s->ps.sps->width &&
        y_ctb + ctb_size >= s->ps.sps->height)
        ff_hevc_hls_filter(s, x_ctb, y_ctb, ctb_size);

    return ctb_addr_ts;
}

static int hls_slice_data(HEVCContext *s)
{
    int arg[2];
    int ret[2];

    arg[0] = 0;
    arg[1] = 1;

    s->avctx->execute(s->avctx, hls_decode_entry, arg, ret , 1, sizeof(int));
    return ret[0];
}
static int hls_decode_entry_wpp(AVCodecContext *avctxt, void *input_ctb_row, int job, int self_id)
{
    HEVCContext *s1  = avctxt->priv_data, *s;
    HEVCLocalContext *lc;
    int ctb_size    = 1<< s1->ps.sps->log2_ctb_size;
    int more_data   = 1;
    int *ctb_row_p    = input_ctb_row;
    int ctb_row = ctb_row_p[job];
    int ctb_addr_rs = s1->sh.slice_ctb_addr_rs + ctb_row * ((s1->ps.sps->width + ctb_size - 1) >> s1->ps.sps->log2_ctb_size);
    int ctb_addr_ts = s1->ps.pps->ctb_addr_rs_to_ts[ctb_addr_rs];
    int thread = ctb_row % s1->threads_number;
    int ret;

    s = s1->sList[self_id];
    lc = s->HEVClc;

#ifdef RPI
    s->enable_rpi = 0;
    //printf("Wavefront\n");
#endif

    if(ctb_row) {
        ret = init_get_bits8(&lc->gb, s->data + s->sh.offset[ctb_row - 1], s->sh.size[ctb_row - 1]);

        if (ret < 0)
            return ret;
        ff_init_cabac_decoder(&lc->cc, s->data + s->sh.offset[(ctb_row)-1], s->sh.size[ctb_row - 1]);
    }

    while(more_data && ctb_addr_ts < s->ps.sps->ctb_size) {
        int x_ctb = (ctb_addr_rs % s->ps.sps->ctb_width) << s->ps.sps->log2_ctb_size;
        int y_ctb = (ctb_addr_rs / s->ps.sps->ctb_width) << s->ps.sps->log2_ctb_size;

        hls_decode_neighbour(s, x_ctb, y_ctb, ctb_addr_ts);

        ff_thread_await_progress2(s->avctx, ctb_row, thread, SHIFT_CTB_WPP);

        if (avpriv_atomic_int_get(&s1->wpp_err)){
            ff_thread_report_progress2(s->avctx, ctb_row , thread, SHIFT_CTB_WPP);
            return 0;
        }

        ff_hevc_cabac_init(s, ctb_addr_ts);
        hls_sao_param(s, x_ctb >> s->ps.sps->log2_ctb_size, y_ctb >> s->ps.sps->log2_ctb_size);
        more_data = hls_coding_quadtree(s, x_ctb, y_ctb, s->ps.sps->log2_ctb_size, 0);

        if (more_data < 0) {
            s->tab_slice_address[ctb_addr_rs] = -1;
            return more_data;
        }

        ctb_addr_ts++;

        ff_hevc_save_states(s, ctb_addr_ts);
        ff_thread_report_progress2(s->avctx, ctb_row, thread, 1);
        ff_hevc_hls_filters(s, x_ctb, y_ctb, ctb_size);

        if (!more_data && (x_ctb+ctb_size) < s->ps.sps->width && ctb_row != s->sh.num_entry_point_offsets) {
            avpriv_atomic_int_set(&s1->wpp_err,  1);
            ff_thread_report_progress2(s->avctx, ctb_row ,thread, SHIFT_CTB_WPP);
            return 0;
        }

        if ((x_ctb+ctb_size) >= s->ps.sps->width && (y_ctb+ctb_size) >= s->ps.sps->height ) {
            ff_hevc_hls_filter(s, x_ctb, y_ctb, ctb_size);
            ff_thread_report_progress2(s->avctx, ctb_row , thread, SHIFT_CTB_WPP);
            return ctb_addr_ts;
        }
        ctb_addr_rs       = s->ps.pps->ctb_addr_ts_to_rs[ctb_addr_ts];
        x_ctb+=ctb_size;

        if(x_ctb >= s->ps.sps->width) {
            break;
        }
    }
    ff_thread_report_progress2(s->avctx, ctb_row ,thread, SHIFT_CTB_WPP);

    return 0;
}

static int hls_slice_data_wpp(HEVCContext *s, const HEVCNAL *nal)
{
    const uint8_t *data = nal->data;
    int length          = nal->size;
    HEVCLocalContext *lc = s->HEVClc;
    int *ret = av_malloc_array(s->sh.num_entry_point_offsets + 1, sizeof(int));
    int *arg = av_malloc_array(s->sh.num_entry_point_offsets + 1, sizeof(int));
    int64_t offset;
    int64_t startheader, cmpt = 0;
    int i, j, res = 0;

    if (!ret || !arg) {
        av_free(ret);
        av_free(arg);
        return AVERROR(ENOMEM);
    }

    if (s->sh.slice_ctb_addr_rs + s->sh.num_entry_point_offsets * s->ps.sps->ctb_width >= s->ps.sps->ctb_width * s->ps.sps->ctb_height) {
        av_log(s->avctx, AV_LOG_ERROR, "WPP ctb addresses are wrong (%d %d %d %d)\n",
            s->sh.slice_ctb_addr_rs, s->sh.num_entry_point_offsets,
            s->ps.sps->ctb_width, s->ps.sps->ctb_height
        );
        res = AVERROR_INVALIDDATA;
        goto error;
    }

    ff_alloc_entries(s->avctx, s->sh.num_entry_point_offsets + 1);

    if (!s->sList[1]) {
        for (i = 1; i < s->threads_number; i++) {
            s->sList[i] = av_malloc(sizeof(HEVCContext));
            memcpy(s->sList[i], s, sizeof(HEVCContext));
            s->HEVClcList[i] = av_mallocz(sizeof(HEVCLocalContext));
            s->sList[i]->HEVClc = s->HEVClcList[i];
        }
    }

    offset = (lc->gb.index >> 3);

    for (j = 0, cmpt = 0, startheader = offset + s->sh.entry_point_offset[0]; j < nal->skipped_bytes; j++) {
        if (nal->skipped_bytes_pos[j] >= offset && nal->skipped_bytes_pos[j] < startheader) {
            startheader--;
            cmpt++;
        }
    }

    for (i = 1; i < s->sh.num_entry_point_offsets; i++) {
        offset += (s->sh.entry_point_offset[i - 1] - cmpt);
        for (j = 0, cmpt = 0, startheader = offset
             + s->sh.entry_point_offset[i]; j < nal->skipped_bytes; j++) {
            if (nal->skipped_bytes_pos[j] >= offset && nal->skipped_bytes_pos[j] < startheader) {
                startheader--;
                cmpt++;
            }
        }
        s->sh.size[i - 1] = s->sh.entry_point_offset[i] - cmpt;
        s->sh.offset[i - 1] = offset;

    }
    if (s->sh.num_entry_point_offsets != 0) {
        offset += s->sh.entry_point_offset[s->sh.num_entry_point_offsets - 1] - cmpt;
        if (length < offset) {
            av_log(s->avctx, AV_LOG_ERROR, "entry_point_offset table is corrupted\n");
            res = AVERROR_INVALIDDATA;
            goto error;
        }
        s->sh.size[s->sh.num_entry_point_offsets - 1] = length - offset;
        s->sh.offset[s->sh.num_entry_point_offsets - 1] = offset;

    }
    s->data = data;

    for (i = 1; i < s->threads_number; i++) {
        s->sList[i]->HEVClc->first_qp_group = 1;
        s->sList[i]->HEVClc->qp_y = s->sList[0]->HEVClc->qp_y;
        memcpy(s->sList[i], s, sizeof(HEVCContext));
        s->sList[i]->HEVClc = s->HEVClcList[i];
    }

    avpriv_atomic_int_set(&s->wpp_err, 0);
    ff_reset_entries(s->avctx);

    for (i = 0; i <= s->sh.num_entry_point_offsets; i++) {
        arg[i] = i;
        ret[i] = 0;
    }

    if (s->ps.pps->entropy_coding_sync_enabled_flag)
        s->avctx->execute2(s->avctx, (void *) hls_decode_entry_wpp, arg, ret, s->sh.num_entry_point_offsets + 1);

    for (i = 0; i <= s->sh.num_entry_point_offsets; i++)
        res += ret[i];
error:
    av_free(ret);
    av_free(arg);
    return res;
}

static int set_side_data(HEVCContext *s)
{
    AVFrame *out = s->ref->frame;

    if (s->sei_frame_packing_present &&
        s->frame_packing_arrangement_type >= 3 &&
        s->frame_packing_arrangement_type <= 5 &&
        s->content_interpretation_type > 0 &&
        s->content_interpretation_type < 3) {
        AVStereo3D *stereo = av_stereo3d_create_side_data(out);
        if (!stereo)
            return AVERROR(ENOMEM);

        switch (s->frame_packing_arrangement_type) {
        case 3:
            if (s->quincunx_subsampling)
                stereo->type = AV_STEREO3D_SIDEBYSIDE_QUINCUNX;
            else
                stereo->type = AV_STEREO3D_SIDEBYSIDE;
            break;
        case 4:
            stereo->type = AV_STEREO3D_TOPBOTTOM;
            break;
        case 5:
            stereo->type = AV_STEREO3D_FRAMESEQUENCE;
            break;
        }

        if (s->content_interpretation_type == 2)
            stereo->flags = AV_STEREO3D_FLAG_INVERT;
    }

    if (s->sei_display_orientation_present &&
        (s->sei_anticlockwise_rotation || s->sei_hflip || s->sei_vflip)) {
        double angle = s->sei_anticlockwise_rotation * 360 / (double) (1 << 16);
        AVFrameSideData *rotation = av_frame_new_side_data(out,
                                                           AV_FRAME_DATA_DISPLAYMATRIX,
                                                           sizeof(int32_t) * 9);
        if (!rotation)
            return AVERROR(ENOMEM);

        av_display_rotation_set((int32_t *)rotation->data, angle);
        av_display_matrix_flip((int32_t *)rotation->data,
                               s->sei_hflip, s->sei_vflip);
    }

    return 0;
}

static int hevc_frame_start(HEVCContext *s)
{
    HEVCLocalContext *lc = s->HEVClc;
    int pic_size_in_ctb  = ((s->ps.sps->width  >> s->ps.sps->log2_min_cb_size) + 1) *
                           ((s->ps.sps->height >> s->ps.sps->log2_min_cb_size) + 1);
    int ret;

    memset(s->horizontal_bs, 0, s->bs_width * s->bs_height);
    memset(s->vertical_bs,   0, s->bs_width * s->bs_height);
    memset(s->cbf_luma,      0, s->ps.sps->min_tb_width * s->ps.sps->min_tb_height);
    memset(s->is_pcm,        0, (s->ps.sps->min_pu_width + 1) * (s->ps.sps->min_pu_height + 1));
    memset(s->tab_slice_address, -1, pic_size_in_ctb * sizeof(*s->tab_slice_address));

    s->is_decoded        = 0;
    s->first_nal_type    = s->nal_unit_type;

    if (s->ps.pps->tiles_enabled_flag)
        lc->end_of_tiles_x = s->ps.pps->column_width[0] << s->ps.sps->log2_ctb_size;

    qpu_stat_poke();

    ret = ff_hevc_set_new_ref(s, &s->frame, s->poc);
    if (ret < 0)
        goto fail;

    ret = ff_hevc_frame_rps(s);
    if (ret < 0) {
        av_log(s->avctx, AV_LOG_ERROR, "Error constructing the frame RPS.\n");
        goto fail;
    }

    s->ref->frame->key_frame = IS_IRAP(s);

    ret = set_side_data(s);
    if (ret < 0)
        goto fail;

    s->frame->pict_type = 3 - s->sh.slice_type;

    if (!IS_IRAP(s))
        ff_hevc_bump_frame(s);

    av_frame_unref(s->output_frame);
    ret = ff_hevc_output_frame(s, s->output_frame, 0);
    if (ret < 0)
        goto fail;

    if (!s->avctx->hwaccel)
        ff_thread_finish_setup(s->avctx);

#ifdef RPI
//    if (s->frame->key_frame)
//        rpi_auxframe_attach(s->frame);
#endif

    return 0;

fail:
    if (s->ref)
        ff_hevc_unref_frame(s, s->ref, ~0);
    s->ref = NULL;
    return ret;
}

static int decode_nal_unit(HEVCContext *s, const HEVCNAL *nal)
{
    HEVCLocalContext *lc = s->HEVClc;
    GetBitContext *gb    = &lc->gb;
    int ctb_addr_ts, ret;

    *gb              = nal->gb;
    s->nal_unit_type = nal->type;
    s->temporal_id   = nal->temporal_id;

    switch (s->nal_unit_type) {
    case NAL_VPS:
        ret = ff_hevc_decode_nal_vps(gb, s->avctx, &s->ps);
        if (ret < 0)
            goto fail;
        break;
    case NAL_SPS:
        ret = ff_hevc_decode_nal_sps(gb, s->avctx, &s->ps,
                                     s->apply_defdispwin);
        if (ret < 0)
            goto fail;
        break;
    case NAL_PPS:
        ret = ff_hevc_decode_nal_pps(gb, s->avctx, &s->ps);
        if (ret < 0)
            goto fail;
        break;
    case NAL_SEI_PREFIX:
    case NAL_SEI_SUFFIX:
        ret = ff_hevc_decode_nal_sei(s);
        if (ret < 0)
            goto fail;
        break;
    case NAL_TRAIL_R:
    case NAL_TRAIL_N:
    case NAL_TSA_N:
    case NAL_TSA_R:
    case NAL_STSA_N:
    case NAL_STSA_R:
    case NAL_BLA_W_LP:
    case NAL_BLA_W_RADL:
    case NAL_BLA_N_LP:
    case NAL_IDR_W_RADL:
    case NAL_IDR_N_LP:
    case NAL_CRA_NUT:
    case NAL_RADL_N:
    case NAL_RADL_R:
    case NAL_RASL_N:
    case NAL_RASL_R:
        ret = hls_slice_header(s);
        if (ret < 0)
            return ret;

        s->used_for_ref = !(s->nal_unit_type == NAL_TRAIL_N ||
                        s->nal_unit_type == NAL_TSA_N   ||
                        s->nal_unit_type == NAL_STSA_N  ||
                        s->nal_unit_type == NAL_RADL_N  ||
                        s->nal_unit_type == NAL_RASL_N);

        if (!s->used_for_ref && s->avctx->skip_frame >= AVDISCARD_NONREF) {
            s->is_decoded = 0;
            break;
        }
        if (s->max_ra == INT_MAX) {
            if (s->nal_unit_type == NAL_CRA_NUT || IS_BLA(s)) {
                s->max_ra = s->poc;
            } else {
                if (IS_IDR(s))
                    s->max_ra = INT_MIN;
            }
        }

        if ((s->nal_unit_type == NAL_RASL_R || s->nal_unit_type == NAL_RASL_N) &&
            s->poc <= s->max_ra) {
            s->is_decoded = 0;
            break;
        } else {
            if (s->nal_unit_type == NAL_RASL_R && s->poc > s->max_ra)
                s->max_ra = INT_MIN;
        }

        if (s->sh.first_slice_in_pic_flag) {
            ret = hevc_frame_start(s);
            if (ret < 0)
                return ret;
        } else if (!s->ref) {
            av_log(s->avctx, AV_LOG_ERROR, "First slice in a frame missing.\n");
            goto fail;
        }

        if (s->nal_unit_type != s->first_nal_type) {
            av_log(s->avctx, AV_LOG_ERROR,
                   "Non-matching NAL types of the VCL NALUs: %d %d\n",
                   s->first_nal_type, s->nal_unit_type);
            return AVERROR_INVALIDDATA;
        }

        if (!s->sh.dependent_slice_segment_flag &&
            s->sh.slice_type != I_SLICE) {
            ret = ff_hevc_slice_rpl(s);
            if (ret < 0) {
                av_log(s->avctx, AV_LOG_WARNING,
                       "Error constructing the reference lists for the current slice.\n");
                goto fail;
            }
        }

        if (s->sh.first_slice_in_pic_flag && s->avctx->hwaccel) {
            ret = s->avctx->hwaccel->start_frame(s->avctx, NULL, 0);
            if (ret < 0)
                goto fail;
        }

        if (s->avctx->hwaccel) {
            ret = s->avctx->hwaccel->decode_slice(s->avctx, nal->raw_data, nal->raw_size);
            if (ret < 0)
                goto fail;
        } else {
            if (s->threads_number > 1 && s->sh.num_entry_point_offsets > 0)
                ctb_addr_ts = hls_slice_data_wpp(s, nal);
            else
                ctb_addr_ts = hls_slice_data(s);
            if (ctb_addr_ts >= (s->ps.sps->ctb_width * s->ps.sps->ctb_height)) {
                s->is_decoded = 1;
            }

            if (ctb_addr_ts < 0) {
                ret = ctb_addr_ts;
                goto fail;
            }
        }
        break;
    case NAL_EOS_NUT:
    case NAL_EOB_NUT:
        s->seq_decode = (s->seq_decode + 1) & 0xff;
        s->max_ra     = INT_MAX;
        break;
    case NAL_AUD:
    case NAL_FD_NUT:
        break;
    default:
        av_log(s->avctx, AV_LOG_INFO,
               "Skipping NAL unit %d\n", s->nal_unit_type);
    }

    return 0;
fail:
    if (s->avctx->err_recognition & AV_EF_EXPLODE)
        return ret;
    return 0;
}

static int decode_nal_units(HEVCContext *s, const uint8_t *buf, int length)
{
    int i, ret = 0;

    s->ref = NULL;
    s->last_eos = s->eos;
    s->eos = 0;

    /* split the input packet into NAL units, so we know the upper bound on the
     * number of slices in the frame */
    ret = ff_hevc_split_packet(s, &s->pkt, buf, length, s->avctx, s->is_nalff,
                               s->nal_length_size);
    if (ret < 0) {
        av_log(s->avctx, AV_LOG_ERROR,
               "Error splitting the input into NAL units.\n");
        return ret;
    }

    for (i = 0; i < s->pkt.nb_nals; i++) {
        if (s->pkt.nals[i].type == NAL_EOB_NUT ||
            s->pkt.nals[i].type == NAL_EOS_NUT)
            s->eos = 1;
    }

    /* decode the NAL units */
    for (i = 0; i < s->pkt.nb_nals; i++) {
        ret = decode_nal_unit(s, &s->pkt.nals[i]);
        if (ret < 0) {
            av_log(s->avctx, AV_LOG_WARNING,
                   "Error parsing NAL unit #%d.\n", i);
            goto fail;
        }
    }

fail:
    if (s->ref && s->threads_type == FF_THREAD_FRAME) {
#ifdef RPI_INTER_QPU
        ff_hevc_flush_buffer(s, &s->ref->tf, s->ps.sps->height);
#endif
        ff_thread_report_progress(&s->ref->tf, INT_MAX, 0);
    } else if (s->ref) {
#ifdef RPI_INTER_QPU
      // When running single threaded we need to flush the whole frame
      flush_frame(s,s->frame);
#endif
    }
    return ret;
}

static void print_md5(void *log_ctx, int level, uint8_t md5[16])
{
    int i;
    for (i = 0; i < 16; i++)
        av_log(log_ctx, level, "%02"PRIx8, md5[i]);
}

static int verify_md5(HEVCContext *s, AVFrame *frame)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(frame->format);
    int pixel_shift;
    int i, j;

    if (!desc)
        return AVERROR(EINVAL);

    pixel_shift = desc->comp[0].depth_minus1 > 7;

    av_log(s->avctx, AV_LOG_DEBUG, "Verifying checksum for frame with POC %d: ",
           s->poc);

    /* the checksums are LE, so we have to byteswap for >8bpp formats
     * on BE arches */
#if HAVE_BIGENDIAN
    if (pixel_shift && !s->checksum_buf) {
        av_fast_malloc(&s->checksum_buf, &s->checksum_buf_size,
                       FFMAX3(frame->linesize[0], frame->linesize[1],
                              frame->linesize[2]));
        if (!s->checksum_buf)
            return AVERROR(ENOMEM);
    }
#endif

    for (i = 0; frame->data[i]; i++) {
        int width  = s->avctx->coded_width;
        int height = s->avctx->coded_height;
        int w = (i == 1 || i == 2) ? (width  >> desc->log2_chroma_w) : width;
        int h = (i == 1 || i == 2) ? (height >> desc->log2_chroma_h) : height;
        uint8_t md5[16];

        av_md5_init(s->md5_ctx);
        for (j = 0; j < h; j++) {
            const uint8_t *src = frame->data[i] + j * frame->linesize[i];
#if HAVE_BIGENDIAN
            if (pixel_shift) {
                s->bdsp.bswap16_buf((uint16_t *) s->checksum_buf,
                                    (const uint16_t *) src, w);
                src = s->checksum_buf;
            }
#endif
            av_md5_update(s->md5_ctx, src, w << pixel_shift);
        }
        av_md5_final(s->md5_ctx, md5);

        if (!memcmp(md5, s->md5[i], 16)) {
            av_log   (s->avctx, AV_LOG_DEBUG, "plane %d - correct ", i);
            print_md5(s->avctx, AV_LOG_DEBUG, md5);
            av_log   (s->avctx, AV_LOG_DEBUG, "; ");
        } else {
            av_log   (s->avctx, AV_LOG_ERROR, "mismatching checksum of plane %d - ", i);
            print_md5(s->avctx, AV_LOG_ERROR, md5);
            av_log   (s->avctx, AV_LOG_ERROR, " != ");
            print_md5(s->avctx, AV_LOG_ERROR, s->md5[i]);
            av_log   (s->avctx, AV_LOG_ERROR, "\n");
            return AVERROR_INVALIDDATA;
        }
    }

    av_log(s->avctx, AV_LOG_DEBUG, "\n");

    return 0;
}

static int hevc_decode_frame(AVCodecContext *avctx, void *data, int *got_output,
                             AVPacket *avpkt)
{
    int ret;
    HEVCContext *s = avctx->priv_data;

    if (!avpkt->size) {
        ret = ff_hevc_output_frame(s, data, 1);
        if (ret < 0)
            return ret;

        *got_output = ret;
        return 0;
    }

    s->ref = NULL;
    ret    = decode_nal_units(s, avpkt->data, avpkt->size);
    if (ret < 0)
        return ret;

    if (avctx->hwaccel) {
        if (s->ref && (ret = avctx->hwaccel->end_frame(avctx)) < 0) {
            av_log(avctx, AV_LOG_ERROR,
                   "hardware accelerator failed to decode picture\n");
            ff_hevc_unref_frame(s, s->ref, ~0);
            return ret;
        }
    } else {
        /* verify the SEI checksum */
        if (avctx->err_recognition & AV_EF_CRCCHECK && s->is_decoded &&
            s->is_md5) {
            ret = verify_md5(s, s->ref->frame);
            if (ret < 0 && avctx->err_recognition & AV_EF_EXPLODE) {
                ff_hevc_unref_frame(s, s->ref, ~0);
                return ret;
            }
        }
    }
    s->is_md5 = 0;

    if (s->is_decoded) {
        av_log(avctx, AV_LOG_DEBUG, "Decoded frame with POC %d.\n", s->poc);
        s->is_decoded = 0;
    }

    if (s->output_frame->buf[0]) {
        av_frame_move_ref(data, s->output_frame);
        *got_output = 1;
    }

    return avpkt->size;
}

static int hevc_ref_frame(HEVCContext *s, HEVCFrame *dst, HEVCFrame *src)
{
    int ret;

    ret = ff_thread_ref_frame(&dst->tf, &src->tf);
    if (ret < 0)
        return ret;

    dst->tab_mvf_buf = av_buffer_ref(src->tab_mvf_buf);
    if (!dst->tab_mvf_buf)
        goto fail;
    dst->tab_mvf = src->tab_mvf;

    dst->rpl_tab_buf = av_buffer_ref(src->rpl_tab_buf);
    if (!dst->rpl_tab_buf)
        goto fail;
    dst->rpl_tab = src->rpl_tab;

    dst->rpl_buf = av_buffer_ref(src->rpl_buf);
    if (!dst->rpl_buf)
        goto fail;

    dst->poc        = src->poc;
    dst->ctb_count  = src->ctb_count;
    dst->window     = src->window;
    dst->flags      = src->flags;
    dst->sequence   = src->sequence;

    if (src->hwaccel_picture_private) {
        dst->hwaccel_priv_buf = av_buffer_ref(src->hwaccel_priv_buf);
        if (!dst->hwaccel_priv_buf)
            goto fail;
        dst->hwaccel_picture_private = dst->hwaccel_priv_buf->data;
    }

    return 0;
fail:
    ff_hevc_unref_frame(s, dst, ~0);
    return AVERROR(ENOMEM);
}

#ifdef RPI_WORKER
static av_cold void hevc_init_worker(HEVCContext *s)
{
    int err;
    pthread_cond_init(&s->worker_cond_head, NULL);
    pthread_cond_init(&s->worker_cond_tail, NULL);
    pthread_mutex_init(&s->worker_mutex, NULL);

    s->worker_tail=0;
    s->worker_head=0;
    s->kill_worker=0;
    err = pthread_create(&s->worker_thread, NULL, worker_start, s);
    if (err) {
        printf("Failed to create worker thread\n");
        exit(-1);
    }
}

static av_cold void hevc_exit_worker(HEVCContext *s)
{
    void *res;
    s->kill_worker=1;
    pthread_cond_broadcast(&s->worker_cond_tail);
    pthread_join(s->worker_thread, &res);

    pthread_cond_destroy(&s->worker_cond_head);
    pthread_cond_destroy(&s->worker_cond_tail);
    pthread_mutex_destroy(&s->worker_mutex);

    s->worker_tail=0;
    s->worker_head=0;
    s->kill_worker=0;
}
#endif

static av_cold int hevc_decode_free(AVCodecContext *avctx)
{
    HEVCContext       *s = avctx->priv_data;
    int i;

    pic_arrays_free(s);

    av_freep(&s->md5_ctx);

    av_freep(&s->cabac_state);

#ifdef RPI

#ifdef RPI_WORKER
    hevc_exit_worker(s);
#endif

    for(i=0;i<RPI_MAX_JOBS;i++) {
      av_freep(&s->unif_mv_cmds[i]);
      av_freep(&s->univ_pred_cmds[i]);

#ifdef RPI_INTER_QPU
      if (s->unif_mvs[i]) {
        gpu_free( &s->unif_mvs_ptr[i] );
        s->unif_mvs[i] = 0;
      }
#endif
#ifdef RPI_LUMA_QPU
      if (s->y_unif_mvs[i]) {
        gpu_free( &s->y_unif_mvs_ptr[i] );
        s->y_unif_mvs[i] = 0;
      }
#endif
    }

#endif

    for (i = 0; i < 3; i++) {
        av_freep(&s->sao_pixel_buffer_h[i]);
        av_freep(&s->sao_pixel_buffer_v[i]);
    }
    av_frame_free(&s->output_frame);

    for (i = 0; i < FF_ARRAY_ELEMS(s->DPB); i++) {
        ff_hevc_unref_frame(s, &s->DPB[i], ~0);
        av_frame_free(&s->DPB[i].frame);
    }

    for (i = 0; i < FF_ARRAY_ELEMS(s->ps.vps_list); i++)
        av_buffer_unref(&s->ps.vps_list[i]);
    for (i = 0; i < FF_ARRAY_ELEMS(s->ps.sps_list); i++)
        av_buffer_unref(&s->ps.sps_list[i]);
    for (i = 0; i < FF_ARRAY_ELEMS(s->ps.pps_list); i++)
        av_buffer_unref(&s->ps.pps_list[i]);
    s->ps.sps = NULL;
    s->ps.pps = NULL;
    s->ps.vps = NULL;

    av_freep(&s->sh.entry_point_offset);
    av_freep(&s->sh.offset);
    av_freep(&s->sh.size);

    for (i = 1; i < s->threads_number; i++) {
        HEVCLocalContext *lc = s->HEVClcList[i];
        if (lc) {
            av_freep(&s->HEVClcList[i]);
            av_freep(&s->sList[i]);
        }
    }
    if (s->HEVClc == s->HEVClcList[0])
        s->HEVClc = NULL;
    av_freep(&s->HEVClcList[0]);

    for (i = 0; i < s->pkt.nals_allocated; i++) {
        av_freep(&s->pkt.nals[i].rbsp_buffer);
        av_freep(&s->pkt.nals[i].skipped_bytes_pos);
    }
    av_freep(&s->pkt.nals);
    s->pkt.nals_allocated = 0;

    return 0;
}

#ifdef RPI
#ifdef RPI_PRECLEAR
static av_cold void memclear16(int16_t *p, int n)
{
  vpu_execute_code( vpu_get_fn(), p, n, 0, 0, 0, 1);
  //int i;
  //for(i=0;i<n;i++)
  //  p[i] = 0;
}
#endif
#endif

static av_cold int hevc_init_context(AVCodecContext *avctx)
{
    HEVCContext *s = avctx->priv_data;
    int i;
    int job;

    s->avctx = avctx;

    s->HEVClc = av_mallocz(sizeof(HEVCLocalContext));
    if (!s->HEVClc)
        goto fail;
    s->HEVClcList[0] = s->HEVClc;
    s->sList[0] = s;

#ifdef RPI
    for(job=0;job<RPI_MAX_JOBS;job++) {
        s->unif_mv_cmds[job] = av_mallocz(sizeof(HEVCMvCmd)*RPI_MAX_MV_CMDS);
        if (!s->unif_mv_cmds[job])
            goto fail;
        s->univ_pred_cmds[job] = av_mallocz(sizeof(HEVCPredCmd)*RPI_MAX_PRED_CMDS);
        if (!s->univ_pred_cmds[job])
            goto fail;
    }

#ifdef RPI_INTER_QPU
    // We divide the image into blocks 256 wide and 64 high
    // We support up to 2048 widths
    // We compute the number of chroma motion vector commands for 4:4:4 format and 4x4 chroma blocks - assuming all blocks are B predicted
    // Also add space for the startup command for each stream.

    {
        int uv_commands_per_qpu = UV_COMMANDS_PER_QPU;
        uint32_t *p;
		for(job=0;job<RPI_MAX_JOBS;job++) {
#ifdef RPI_CACHE_UNIF_MVS
          gpu_malloc_cached( 8 * uv_commands_per_qpu * sizeof(uint32_t), &s->unif_mvs_ptr[job] );
#else
          gpu_malloc_uncached( 8 * uv_commands_per_qpu * sizeof(uint32_t), &s->unif_mvs_ptr[job] );
#endif
          s->unif_mvs[job] = (uint32_t *) s->unif_mvs_ptr[job].arm;

          // Set up initial locations for uniform streams
          p = s->unif_mvs[job];
          for(i = 0; i < 8; i++) {
            s->mvs_base[job][i] = p;
            p += uv_commands_per_qpu;
          }
        }
        s->mc_filter_uv = qpu_get_fn(QPU_MC_FILTER_UV);
        s->mc_filter_uv_b0 = qpu_get_fn(QPU_MC_FILTER_UV_B0);
        s->mc_filter_uv_b = qpu_get_fn(QPU_MC_FILTER_UV_B);

    }
#endif
#ifdef RPI_LUMA_QPU
    for(job=0;job<RPI_MAX_JOBS;job++)
    {
        int y_commands_per_qpu = Y_COMMANDS_PER_QPU;
        uint32_t *p;
#ifdef RPI_CACHE_UNIF_MVS
        gpu_malloc_cached( 12 * y_commands_per_qpu * sizeof(uint32_t), &s->y_unif_mvs_ptr[job] );
#else
        gpu_malloc_uncached( 12 * y_commands_per_qpu * sizeof(uint32_t), &s->y_unif_mvs_ptr[job] );
#endif
        s->y_unif_mvs[job] = (uint32_t *) s->y_unif_mvs_ptr[job].arm;

        // Set up initial locations for uniform streams
        p = s->y_unif_mvs[job];
        for(i = 0; i < 12; i++) {
            s->y_mvs_base[job][i] = p;
            p += y_commands_per_qpu;
        }
    }
    s->mc_filter = qpu_get_fn(QPU_MC_FILTER);
    s->mc_filter_b = qpu_get_fn(QPU_MC_FILTER_B);
#endif
    //gpu_malloc_uncached(2048*64,&s->dummy);

    s->enable_rpi = 0;

#ifdef RPI_WORKER
    hevc_init_worker(s);
#endif

#endif

    s->cabac_state = av_malloc(HEVC_CONTEXTS);
    if (!s->cabac_state)
        goto fail;

    s->output_frame = av_frame_alloc();
    if (!s->output_frame)
        goto fail;

    for (i = 0; i < FF_ARRAY_ELEMS(s->DPB); i++) {
        s->DPB[i].frame = av_frame_alloc();
        if (!s->DPB[i].frame)
            goto fail;
        s->DPB[i].tf.f = s->DPB[i].frame;
    }

    s->max_ra = INT_MAX;

    s->md5_ctx = av_md5_alloc();
    if (!s->md5_ctx)
        goto fail;

    ff_bswapdsp_init(&s->bdsp);

    s->context_initialized = 1;
    s->eos = 0;

    return 0;

fail:
    hevc_decode_free(avctx);
    return AVERROR(ENOMEM);
}

static int hevc_update_thread_context(AVCodecContext *dst,
                                      const AVCodecContext *src)
{
    HEVCContext *s  = dst->priv_data;
    HEVCContext *s0 = src->priv_data;
    int i, ret;

    if (!s->context_initialized) {
        ret = hevc_init_context(dst);
        if (ret < 0)
            return ret;
    }

    for (i = 0; i < FF_ARRAY_ELEMS(s->DPB); i++) {
        ff_hevc_unref_frame(s, &s->DPB[i], ~0);
        if (s0->DPB[i].frame->buf[0]) {
            ret = hevc_ref_frame(s, &s->DPB[i], &s0->DPB[i]);
            if (ret < 0)
                return ret;
        }
    }

    if (s->ps.sps != s0->ps.sps)
        s->ps.sps = NULL;
    for (i = 0; i < FF_ARRAY_ELEMS(s->ps.vps_list); i++) {
        av_buffer_unref(&s->ps.vps_list[i]);
        if (s0->ps.vps_list[i]) {
            s->ps.vps_list[i] = av_buffer_ref(s0->ps.vps_list[i]);
            if (!s->ps.vps_list[i])
                return AVERROR(ENOMEM);
        }
    }

    for (i = 0; i < FF_ARRAY_ELEMS(s->ps.sps_list); i++) {
        av_buffer_unref(&s->ps.sps_list[i]);
        if (s0->ps.sps_list[i]) {
            s->ps.sps_list[i] = av_buffer_ref(s0->ps.sps_list[i]);
            if (!s->ps.sps_list[i])
                return AVERROR(ENOMEM);
        }
    }

    for (i = 0; i < FF_ARRAY_ELEMS(s->ps.pps_list); i++) {
        av_buffer_unref(&s->ps.pps_list[i]);
        if (s0->ps.pps_list[i]) {
            s->ps.pps_list[i] = av_buffer_ref(s0->ps.pps_list[i]);
            if (!s->ps.pps_list[i])
                return AVERROR(ENOMEM);
        }
    }

    if (s->ps.sps != s0->ps.sps)
        if ((ret = set_sps(s, s0->ps.sps, src->pix_fmt)) < 0)
            return ret;

    s->seq_decode = s0->seq_decode;
    s->seq_output = s0->seq_output;
    s->pocTid0    = s0->pocTid0;
    s->max_ra     = s0->max_ra;
    s->eos        = s0->eos;
    s->no_rasl_output_flag = s0->no_rasl_output_flag;

    s->is_nalff        = s0->is_nalff;
    s->nal_length_size = s0->nal_length_size;

    s->threads_number      = s0->threads_number;
    s->threads_type        = s0->threads_type;

    if (s0->eos) {
        s->seq_decode = (s->seq_decode + 1) & 0xff;
        s->max_ra = INT_MAX;
    }

    return 0;
}

static int hevc_decode_extradata(HEVCContext *s)
{
    AVCodecContext *avctx = s->avctx;
    GetByteContext gb;
    int ret, i;

    bytestream2_init(&gb, avctx->extradata, avctx->extradata_size);

    if (avctx->extradata_size > 3 &&
        (avctx->extradata[0] || avctx->extradata[1] ||
         avctx->extradata[2] > 1)) {
        /* It seems the extradata is encoded as hvcC format.
         * Temporarily, we support configurationVersion==0 until 14496-15 3rd
         * is finalized. When finalized, configurationVersion will be 1 and we
         * can recognize hvcC by checking if avctx->extradata[0]==1 or not. */
        int i, j, num_arrays, nal_len_size;

        s->is_nalff = 1;

        bytestream2_skip(&gb, 21);
        nal_len_size = (bytestream2_get_byte(&gb) & 3) + 1;
        num_arrays   = bytestream2_get_byte(&gb);

        /* nal units in the hvcC always have length coded with 2 bytes,
         * so put a fake nal_length_size = 2 while parsing them */
        s->nal_length_size = 2;

        /* Decode nal units from hvcC. */
        for (i = 0; i < num_arrays; i++) {
            int type = bytestream2_get_byte(&gb) & 0x3f;
            int cnt  = bytestream2_get_be16(&gb);

            for (j = 0; j < cnt; j++) {
                // +2 for the nal size field
                int nalsize = bytestream2_peek_be16(&gb) + 2;
                if (bytestream2_get_bytes_left(&gb) < nalsize) {
                    av_log(s->avctx, AV_LOG_ERROR,
                           "Invalid NAL unit size in extradata.\n");
                    return AVERROR_INVALIDDATA;
                }

                ret = decode_nal_units(s, gb.buffer, nalsize);
                if (ret < 0) {
                    av_log(avctx, AV_LOG_ERROR,
                           "Decoding nal unit %d %d from hvcC failed\n",
                           type, i);
                    return ret;
                }
                bytestream2_skip(&gb, nalsize);
            }
        }

        /* Now store right nal length size, that will be used to parse
         * all other nals */
        s->nal_length_size = nal_len_size;
    } else {
        s->is_nalff = 0;
        ret = decode_nal_units(s, avctx->extradata, avctx->extradata_size);
        if (ret < 0)
            return ret;
    }

    /* export stream parameters from the first SPS */
    for (i = 0; i < FF_ARRAY_ELEMS(s->ps.sps_list); i++) {
        if (s->ps.sps_list[i]) {
            const HEVCSPS *sps = (const HEVCSPS*)s->ps.sps_list[i]->data;
            export_stream_params(s->avctx, &s->ps, sps);
            break;
        }
    }

    return 0;
}

static av_cold int hevc_decode_init(AVCodecContext *avctx)
{
    HEVCContext *s = avctx->priv_data;
    int ret;

    ff_init_cabac_states();

    avctx->internal->allocate_progress = 1;

    ret = hevc_init_context(avctx);
    if (ret < 0)
        return ret;

    s->enable_parallel_tiles = 0;
    s->picture_struct = 0;
    s->eos = 1;

    if(avctx->active_thread_type & FF_THREAD_SLICE)
        s->threads_number = avctx->thread_count;
    else
        s->threads_number = 1;

    if (avctx->extradata_size > 0 && avctx->extradata) {
        ret = hevc_decode_extradata(s);
        if (ret < 0) {
            hevc_decode_free(avctx);
            return ret;
        }
    }

    if((avctx->active_thread_type & FF_THREAD_FRAME) && avctx->thread_count > 1)
            s->threads_type = FF_THREAD_FRAME;
        else
            s->threads_type = FF_THREAD_SLICE;

    return 0;
}

static av_cold int hevc_init_thread_copy(AVCodecContext *avctx)
{
    HEVCContext *s = avctx->priv_data;
    int ret;

    memset(s, 0, sizeof(*s));

    ret = hevc_init_context(avctx);
    if (ret < 0)
        return ret;

    return 0;
}

static void hevc_decode_flush(AVCodecContext *avctx)
{
    HEVCContext *s = avctx->priv_data;
    ff_hevc_flush_dpb(s);
    s->max_ra = INT_MAX;
    s->eos = 1;
}

#define OFFSET(x) offsetof(HEVCContext, x)
#define PAR (AV_OPT_FLAG_DECODING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)

static const AVProfile profiles[] = {
    { FF_PROFILE_HEVC_MAIN,                 "Main"                },
    { FF_PROFILE_HEVC_MAIN_10,              "Main 10"             },
    { FF_PROFILE_HEVC_MAIN_STILL_PICTURE,   "Main Still Picture"  },
    { FF_PROFILE_HEVC_REXT,                 "Rext"  },
    { FF_PROFILE_UNKNOWN },
};

static const AVOption options[] = {
    { "apply_defdispwin", "Apply default display window from VUI", OFFSET(apply_defdispwin),
        AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, PAR },
    { "strict-displaywin", "stricly apply default display window size", OFFSET(apply_defdispwin),
        AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, PAR },
    { NULL },
};

static const AVClass hevc_decoder_class = {
    .class_name = "HEVC decoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_hevc_decoder = {
    .name                  = "hevc",
    .long_name             = NULL_IF_CONFIG_SMALL("HEVC (High Efficiency Video Coding)"),
    .type                  = AVMEDIA_TYPE_VIDEO,
    .id                    = AV_CODEC_ID_HEVC,
    .priv_data_size        = sizeof(HEVCContext),
    .priv_class            = &hevc_decoder_class,
    .init                  = hevc_decode_init,
    .close                 = hevc_decode_free,
    .decode                = hevc_decode_frame,
    .flush                 = hevc_decode_flush,
    .update_thread_context = hevc_update_thread_context,
    .init_thread_copy      = hevc_init_thread_copy,
    .capabilities          = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_DELAY |
                             AV_CODEC_CAP_SLICE_THREADS | AV_CODEC_CAP_FRAME_THREADS,
    .profiles              = NULL_IF_CONFIG_SMALL(profiles),
};
