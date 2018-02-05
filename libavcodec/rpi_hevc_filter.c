/*
 * HEVC video decoder
 *
 * Copyright (C) 2012 - 2013 Guillaume Martres
 * Copyright (C) 2013 Seppo Tomperi
 * Copyright (C) 2013 Wassim Hamidouche
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

//#define DISABLE_SAO
//#define DISABLE_DEBLOCK
//#define DISABLE_STRENGTHS
// define DISABLE_DEBLOCK_NONREF for a 6% speed boost (by skipping deblocking on unimportant frames)
//#define DISABLE_DEBLOCK_NONREF

#include "libavutil/common.h"
#include "libavutil/internal.h"

#include "cabac_functions.h"
#include "rpi_hevcdec.h"

#include "bit_depth_template.c"

#include "rpi_qpu.h"
#include "rpi_zc.h"
#include "libavutil/rpi_sand_fns.h"

#define LUMA 0
#define CB 1
#define CR 2

static const uint8_t tctable[54] = {
    0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 0, 0, 1, // QP  0...18
    1, 1, 1, 1, 1, 1, 1,  1,  2,  2,  2,  2,  3,  3,  3,  3, 4, 4, 4, // QP 19...37
    5, 5, 6, 6, 7, 8, 9, 10, 11, 13, 14, 16, 18, 20, 22, 24           // QP 38...53
};

static const uint8_t betatable[52] = {
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  6,  7,  8, // QP 0...18
     9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 20, 22, 24, 26, 28, 30, 32, 34, 36, // QP 19...37
    38, 40, 42, 44, 46, 48, 50, 52, 54, 56, 58, 60, 62, 64                      // QP 38...51
};

static int chroma_tc(HEVCRpiContext *s, int qp_y, int c_idx, int tc_offset)
{
    static const int qp_c[] = {
        29, 30, 31, 32, 33, 33, 34, 34, 35, 35, 36, 36, 37, 37
    };
    int qp, qp_i, offset, idxt;

    // slice qp offset is not used for deblocking
    if (c_idx == 1)
        offset = s->ps.pps->cb_qp_offset;
    else
        offset = s->ps.pps->cr_qp_offset;

    qp_i = av_clip(qp_y + offset, 0, 57);
    if (ctx_cfmt(s) == 1) {
        if (qp_i < 30)
            qp = qp_i;
        else if (qp_i > 43)
            qp = qp_i - 6;
        else
            qp = qp_c[qp_i - 30];
    } else {
        qp = av_clip(qp_i, 0, 51);
    }

    idxt = av_clip(qp + DEFAULT_INTRA_TC_OFFSET + tc_offset, 0, 53);
    return tctable[idxt];
}

static inline int get_qPy_pred(const HEVCRpiContext * const s, HEVCRpiLocalContext * const lc, int xBase, int yBase, int log2_cb_size)
{
    int ctb_size_mask        = (1 << s->ps.sps->log2_ctb_size) - 1;
    int MinCuQpDeltaSizeMask = ~((1 << (s->ps.sps->log2_ctb_size -
                                      s->ps.pps->diff_cu_qp_delta_depth)) - 1);
    int xQgBase              = xBase & MinCuQpDeltaSizeMask;
    int yQgBase              = yBase & MinCuQpDeltaSizeMask;
    int min_cb_width         = s->ps.sps->min_cb_width;
    int x_cb                 = xQgBase >> s->ps.sps->log2_min_cb_size;
    int y_cb                 = yQgBase >> s->ps.sps->log2_min_cb_size;
    int availableA           = (xBase   & ctb_size_mask) &&
                               (xQgBase & ctb_size_mask);
    int availableB           = (yBase   & ctb_size_mask) &&
                               (yQgBase & ctb_size_mask);
    const int qPy_pred = lc->qPy_pred;

    return ((!availableA ? qPy_pred : s->qp_y_tab[(x_cb - 1) + y_cb * min_cb_width]) +
            (!availableB ? qPy_pred : s->qp_y_tab[x_cb + (y_cb - 1) * min_cb_width]) + 1) >> 1;
}

// * Only called from bitstream decode in foreground
//   so should be safe
void ff_hevc_rpi_set_qPy(const HEVCRpiContext * const s, HEVCRpiLocalContext * const lc, int xBase, int yBase, int log2_cb_size)
{
    const int qp_y = get_qPy_pred(s, lc, xBase, yBase, log2_cb_size);

    if (lc->tu.cu_qp_delta != 0) {
        int off = s->ps.sps->qp_bd_offset;
        lc->qp_y = FFUMOD(qp_y + lc->tu.cu_qp_delta + 52 + 2 * off,
                                 52 + off) - off;
    } else
        lc->qp_y = qp_y;
}

static int get_qPy(const HEVCRpiContext * const s, const int xC, const int yC)
{
    const int log2_min_cb_size  = s->ps.sps->log2_min_cb_size;
    const int x                 = xC >> log2_min_cb_size;
    const int y                 = yC >> log2_min_cb_size;
    return s->qp_y_tab[x + y * s->ps.sps->min_cb_width];
}

static inline unsigned int pixel_shift(const HEVCRpiContext * const s, const unsigned int c_idx)
{
    return c_idx != 0 ? 1 + s->ps.sps->pixel_shift : s->ps.sps->pixel_shift;
}

static void copy_CTB(uint8_t *dst, const uint8_t *src, int width, int height,
                     ptrdiff_t stride_dst, ptrdiff_t stride_src)
{
int i, j;

    if (((intptr_t)dst | (intptr_t)src | stride_dst | stride_src) & 15) {
        for (i = 0; i < height; i++) {
            for (j = 0; j < width; j+=8)
                AV_COPY64U(dst+j, src+j);
            dst += stride_dst;
            src += stride_src;
        }
    } else {
        for (i = 0; i < height; i++) {
            for (j = 0; j < width; j+=16)
                AV_COPY128(dst+j, src+j);
            dst += stride_dst;
            src += stride_src;
        }
    }
}

// "DSP" these?
static void copy_pixel(uint8_t *dst, const uint8_t *src, int pixel_shift)
{
    switch (pixel_shift)
    {
        case 2:
            *(uint32_t *)dst = *(uint32_t *)src;
            break;
        case 1:
            *(uint16_t *)dst = *(uint16_t *)src;
            break;
        default:
            *dst = *src;
            break;
    }
}

static void copy_vert(uint8_t *dst, const uint8_t *src,
                      int pixel_shift, int height,
                      ptrdiff_t stride_dst, ptrdiff_t stride_src)
{
    int i;
    switch (pixel_shift)
    {
        case 2:
            for (i = 0; i < height; i++) {
                *(uint32_t *)dst = *(uint32_t *)src;
                dst += stride_dst;
                src += stride_src;
            }
            break;
        case 1:
            for (i = 0; i < height; i++) {
                *(uint16_t *)dst = *(uint16_t *)src;
                dst += stride_dst;
                src += stride_src;
            }
            break;
        default:
            for (i = 0; i < height; i++) {
                *dst = *src;
                dst += stride_dst;
                src += stride_src;
            }
            break;
    }
}

static void copy_CTB_to_hv(const HEVCRpiContext * const s, const uint8_t * const src,
                           ptrdiff_t stride_src, int x, int y, int width, int height,
                           int c_idx, int x_ctb, int y_ctb)
{
    const unsigned int sh = pixel_shift(s, c_idx);
    const unsigned int w = s->ps.sps->width >> ctx_hshift(s, c_idx);
    const unsigned int h = s->ps.sps->height >> ctx_vshift(s, c_idx);

    /* copy horizontal edges */
    memcpy(s->sao_pixel_buffer_h[c_idx] + (((2 * y_ctb) * w + x) << sh),
        src, width << sh);
    memcpy(s->sao_pixel_buffer_h[c_idx] + (((2 * y_ctb + 1) * w + x) << sh),
        src + stride_src * (height - 1), width << sh);

    /* copy vertical edges */
    copy_vert(s->sao_pixel_buffer_v[c_idx] + (((2 * x_ctb) * h + y) << sh), src, sh, height, 1 << sh, stride_src);

    copy_vert(s->sao_pixel_buffer_v[c_idx] + (((2 * x_ctb + 1) * h + y) << sh), src + ((width - 1) << sh), sh, height, 1 << sh, stride_src);
}

// N.B. Src & dst are swapped as this is a restore!
static void restore_tqb_pixels(const HEVCRpiContext * const s,
                               uint8_t *src1, const uint8_t *dst1,
                               ptrdiff_t stride_src, ptrdiff_t stride_dst,
                               int x0, int y0, int width, int height, int c_idx)
{
    if ( s->ps.pps->transquant_bypass_enable_flag ||
            (s->ps.sps->pcm.loop_filter_disable_flag && s->ps.sps->pcm_enabled_flag)) {
        int x, y;
        int min_pu_size  = 1 << s->ps.sps->log2_min_pu_size;
        const unsigned int hshift = ctx_hshift(s, c_idx);
        const unsigned int vshift = ctx_vshift(s, c_idx);
        int x_min        = ((x0         ) >> s->ps.sps->log2_min_pu_size);
        int y_min        = ((y0         ) >> s->ps.sps->log2_min_pu_size);
        int x_max        = ((x0 + width ) >> s->ps.sps->log2_min_pu_size);
        int y_max        = ((y0 + height) >> s->ps.sps->log2_min_pu_size);
        const unsigned int sh = pixel_shift(s, c_idx);
        int len          = (min_pu_size >> hshift) << sh;
        for (y = y_min; y < y_max; y++) {
            for (x = x_min; x < x_max; x++) {
                if (s->is_pcm[y * s->ps.sps->min_pu_width + x]) {
                    int n;
                    uint8_t *src = src1 + (((y << s->ps.sps->log2_min_pu_size) - y0) >> vshift) * stride_src + ((((x << s->ps.sps->log2_min_pu_size) - x0) >> hshift) << sh);
                    const uint8_t *dst = dst1 + (((y << s->ps.sps->log2_min_pu_size) - y0) >> vshift) * stride_dst + ((((x << s->ps.sps->log2_min_pu_size) - x0) >> hshift) << sh);
                    for (n = 0; n < (min_pu_size >> vshift); n++) {
                        memcpy(src, dst, len);
                        src += stride_src;
                        dst += stride_dst;
                    }
                }
            }
        }
    }
}

#define CTB(tab, x, y) ((tab)[(y) * s->ps.sps->ctb_width + (x)])

static void sao_filter_CTB(const HEVCRpiContext * const s, const int x, const int y)
{
#if SAO_FILTER_N == 5
    static const uint8_t sao_tab[8] = { 0 /* 8 */, 1 /* 16 */, 2 /* 24 */, 2 /* 32 */, 3, 3 /* 48 */, 4, 4 /* 64 */};
#elif SAO_FILTER_N == 6
    static const uint8_t sao_tab[8] = { 0 /* 8 */, 1 /* 16 */, 5 /* 24 */, 2 /* 32 */, 3, 3 /* 48 */, 4, 4 /* 64 */};
#else
#error Confused by size of sao fn array
#endif
    int c_idx;
    int edges[4];  // 0 left 1 top 2 right 3 bottom
    int x_ctb                = x >> s->ps.sps->log2_ctb_size;
    int y_ctb                = y >> s->ps.sps->log2_ctb_size;
    int ctb_addr_rs          = y_ctb * s->ps.sps->ctb_width + x_ctb;
    int ctb_addr_ts          = s->ps.pps->ctb_addr_rs_to_ts[ctb_addr_rs];
    SAOParams *sao           = &CTB(s->sao, x_ctb, y_ctb);
    // flags indicating unfilterable edges
    uint8_t vert_edge[]      = { 0, 0 };
    uint8_t horiz_edge[]     = { 0, 0 };
    uint8_t diag_edge[]      = { 0, 0, 0, 0 };
    uint8_t lfase            = CTB(s->filter_slice_edges, x_ctb, y_ctb);
    uint8_t no_tile_filter   = s->ps.pps->tiles_enabled_flag &&
                               !s->ps.pps->loop_filter_across_tiles_enabled_flag;
    uint8_t restore          = no_tile_filter || !lfase;
    uint8_t left_tile_edge   = 0;
    uint8_t right_tile_edge  = 0;
    uint8_t up_tile_edge     = 0;
    uint8_t bottom_tile_edge = 0;
    const int sliced = 1;
    const int plane_count = sliced ? 2 : (ctx_cfmt(s) != 0 ? 3 : 1);

    edges[0]   = x_ctb == 0;
    edges[1]   = y_ctb == 0;
    edges[2]   = x_ctb == s->ps.sps->ctb_width  - 1;
    edges[3]   = y_ctb == s->ps.sps->ctb_height - 1;

#ifdef DISABLE_SAO
    return;
#endif

    if (restore) {
        if (!edges[0]) {
            left_tile_edge  = no_tile_filter && s->ps.pps->tile_id[ctb_addr_ts] != s->ps.pps->tile_id[s->ps.pps->ctb_addr_rs_to_ts[ctb_addr_rs-1]];
            vert_edge[0]    = (!lfase && CTB(s->tab_slice_address, x_ctb, y_ctb) != CTB(s->tab_slice_address, x_ctb - 1, y_ctb)) || left_tile_edge;
        }
        if (!edges[2]) {
            right_tile_edge = no_tile_filter && s->ps.pps->tile_id[ctb_addr_ts] != s->ps.pps->tile_id[s->ps.pps->ctb_addr_rs_to_ts[ctb_addr_rs+1]];
            vert_edge[1]    = (!lfase && CTB(s->tab_slice_address, x_ctb, y_ctb) != CTB(s->tab_slice_address, x_ctb + 1, y_ctb)) || right_tile_edge;
        }
        if (!edges[1]) {
            up_tile_edge     = no_tile_filter && s->ps.pps->tile_id[ctb_addr_ts] != s->ps.pps->tile_id[s->ps.pps->ctb_addr_rs_to_ts[ctb_addr_rs - s->ps.sps->ctb_width]];
            horiz_edge[0]    = (!lfase && CTB(s->tab_slice_address, x_ctb, y_ctb) != CTB(s->tab_slice_address, x_ctb, y_ctb - 1)) || up_tile_edge;
        }
        if (!edges[3]) {
            bottom_tile_edge = no_tile_filter && s->ps.pps->tile_id[ctb_addr_ts] != s->ps.pps->tile_id[s->ps.pps->ctb_addr_rs_to_ts[ctb_addr_rs + s->ps.sps->ctb_width]];
            horiz_edge[1]    = (!lfase && CTB(s->tab_slice_address, x_ctb, y_ctb) != CTB(s->tab_slice_address, x_ctb, y_ctb + 1)) || bottom_tile_edge;
        }
        if (!edges[0] && !edges[1]) {
            diag_edge[0] = (!lfase && CTB(s->tab_slice_address, x_ctb, y_ctb) != CTB(s->tab_slice_address, x_ctb - 1, y_ctb - 1)) || left_tile_edge || up_tile_edge;
        }
        if (!edges[1] && !edges[2]) {
            diag_edge[1] = (!lfase && CTB(s->tab_slice_address, x_ctb, y_ctb) != CTB(s->tab_slice_address, x_ctb + 1, y_ctb - 1)) || right_tile_edge || up_tile_edge;
        }
        if (!edges[2] && !edges[3]) {
            diag_edge[2] = (!lfase && CTB(s->tab_slice_address, x_ctb, y_ctb) != CTB(s->tab_slice_address, x_ctb + 1, y_ctb + 1)) || right_tile_edge || bottom_tile_edge;
        }
        if (!edges[0] && !edges[3]) {
            diag_edge[3] = (!lfase && CTB(s->tab_slice_address, x_ctb, y_ctb) != CTB(s->tab_slice_address, x_ctb - 1, y_ctb + 1)) || left_tile_edge || bottom_tile_edge;
        }
    }

    for (c_idx = 0; c_idx < plane_count; c_idx++) {
        const unsigned int vshift = ctx_vshift(s, c_idx);
        const unsigned int hshift = ctx_hshift(s, c_idx);
        const int x0 = x >> hshift;
        const int y0 = y >> vshift;
        const ptrdiff_t stride_src = frame_stride1(s->frame, c_idx);
        int ctb_size_h = (1 << (s->ps.sps->log2_ctb_size)) >> hshift;
        int ctb_size_v = (1 << (s->ps.sps->log2_ctb_size)) >> vshift;
        int width    = FFMIN(ctb_size_h, (s->ps.sps->width  >> hshift) - x0);
        const int height = FFMIN(ctb_size_v, (s->ps.sps->height >> vshift) - y0);
        int tab      = sao_tab[(FFALIGN(width, 8) >> 3) - 1];
        ptrdiff_t stride_dst;
        uint8_t *dst;

        const unsigned int sh = s->ps.sps->pixel_shift + (sliced && c_idx != 0);
        const int wants_lr = sao->type_idx[c_idx] == SAO_EDGE && sao->eo_class[c_idx] != 1 /* Vertical */;
        uint8_t * const src = !sliced ?
                &s->frame->data[c_idx][y0 * stride_src + (x0 << sh)] :
            c_idx == 0 ?
                av_rpi_sand_frame_pos_y(s->frame, x0, y0) :
                av_rpi_sand_frame_pos_c(s->frame, x0, y0);
        const uint8_t * const src_l = edges[0] || !wants_lr ? NULL :
            !sliced ? src - (1 << sh) :
            c_idx == 0 ?
                av_rpi_sand_frame_pos_y(s->frame, x0 - 1, y0) :
                av_rpi_sand_frame_pos_c(s->frame, x0 - 1, y0);
        const uint8_t * const src_r = edges[2] || !wants_lr ? NULL :
            !sliced ? src + (width << sh) :
            c_idx == 0 ?
                av_rpi_sand_frame_pos_y(s->frame, x0 + width, y0) :
                av_rpi_sand_frame_pos_c(s->frame, x0 + width, y0);

        if (sliced && c_idx > 1) {
            break;
        }

        switch (sao->type_idx[c_idx]) {
        case SAO_BAND:
            copy_CTB_to_hv(s, src, stride_src, x0, y0, width, height, c_idx,
                           x_ctb, y_ctb);
            if (s->ps.pps->transquant_bypass_enable_flag ||
                (s->ps.sps->pcm.loop_filter_disable_flag && s->ps.sps->pcm_enabled_flag)) {
                // Can't use the edge buffer here as it may be in use by the foreground
                DECLARE_ALIGNED(64, uint8_t, dstbuf)
                    [2*MAX_PB_SIZE*MAX_PB_SIZE];
                dst = dstbuf;
                stride_dst = 2*MAX_PB_SIZE;
                copy_CTB(dst, src, width << sh, height, stride_dst, stride_src);
                if (sliced && c_idx != 0)
                {
                    s->hevcdsp.sao_band_filter_c[tab](src, dst, stride_src, stride_dst,
                                                    sao->offset_val[1], sao->band_position[1],
                                                    sao->offset_val[2], sao->band_position[2],
                                                    width, height);
                }
                else
                {
                    s->hevcdsp.sao_band_filter[tab](src, dst, stride_src, stride_dst,
                                                    sao->offset_val[c_idx], sao->band_position[c_idx],
                                                    width, height);
                }
                restore_tqb_pixels(s, src, dst, stride_src, stride_dst,
                                   x, y, width, height, c_idx);
            } else {
                if (sliced && c_idx != 0)
                {
                    s->hevcdsp.sao_band_filter_c[tab](src, src, stride_src, stride_src,
                                                    sao->offset_val[1], sao->band_position[1],
                                                    sao->offset_val[2], sao->band_position[2],
                                                    width, height);
                }
                else
                {
                    s->hevcdsp.sao_band_filter[tab](src, src, stride_src, stride_src,
                                                    sao->offset_val[c_idx], sao->band_position[c_idx],
                                                    width, height);
                }
            }
            sao->type_idx[c_idx] = SAO_APPLIED;
            break;
        case SAO_EDGE:
        {
            const int w = s->ps.sps->width >> hshift;
            const int h = s->ps.sps->height >> vshift;
            int top_edge = edges[1];
            int bottom_edge = edges[3];
            // Can't use the edge buffer here as it may be in use by the foreground
            DECLARE_ALIGNED(64, uint8_t, dstbuf)
                [2*(MAX_PB_SIZE + AV_INPUT_BUFFER_PADDING_SIZE)*(MAX_PB_SIZE + 2) + 64];

            stride_dst = 2*MAX_PB_SIZE + AV_INPUT_BUFFER_PADDING_SIZE;
            dst = dstbuf + stride_dst + AV_INPUT_BUFFER_PADDING_SIZE;

            if (!top_edge) {
                uint8_t *dst1;
                int src_idx;
                const uint8_t * const src_spb = s->sao_pixel_buffer_h[c_idx] + (((2 * y_ctb - 1) * w + x0) << sh);

                dst1 = dst - stride_dst;

                if (src_l != NULL) {
                    src_idx = (CTB(s->sao, x_ctb-1, y_ctb-1).type_idx[c_idx] ==
                               SAO_APPLIED);
                    copy_pixel(dst1 - (1 << sh), src_idx ? src_spb - (1 << sh) : src_l - stride_src, sh);
                }

                src_idx = (CTB(s->sao, x_ctb, y_ctb-1).type_idx[c_idx] ==
                           SAO_APPLIED);
                memcpy(dst1, src_idx ? src_spb : src - stride_src, width << sh);

                if (src_r != NULL) {
                    src_idx = (CTB(s->sao, x_ctb+1, y_ctb-1).type_idx[c_idx] ==
                               SAO_APPLIED);
                    copy_pixel(dst1 + (width << sh), src_idx ? src_spb + (width << sh) : src_r - stride_src, sh);
                }
            }
            if (!bottom_edge) {
                uint8_t * const dst1 = dst + height * stride_dst;
                int src_idx;
                const uint8_t * const src_spb = s->sao_pixel_buffer_h[c_idx] + (((2 * y_ctb + 2) * w + x0) << sh);
                const unsigned int hoff = height * stride_src;

                if (src_l != NULL) {
                    src_idx = (CTB(s->sao, x_ctb-1, y_ctb+1).type_idx[c_idx] ==
                               SAO_APPLIED);
                    copy_pixel(dst1 - (1 << sh), src_idx ? src_spb - (1 << sh) : src_l + hoff, sh);
                }

                src_idx = (CTB(s->sao, x_ctb, y_ctb+1).type_idx[c_idx] ==
                           SAO_APPLIED);
                memcpy(dst1, src_idx ? src_spb : src + hoff, width << sh);

                if (src_r != NULL) {
                    src_idx = (CTB(s->sao, x_ctb+1, y_ctb+1).type_idx[c_idx] ==
                               SAO_APPLIED);
                    copy_pixel(dst1 + (width << sh), src_idx ? src_spb + (width << sh) : src_r + hoff, sh);
                }
            }
            if (src_l != NULL) {
                if (CTB(s->sao, x_ctb-1, y_ctb).type_idx[c_idx] == SAO_APPLIED) {
                    copy_vert(dst - (1 << sh),
                              s->sao_pixel_buffer_v[c_idx] + (((2 * x_ctb - 1) * h + y0) << sh),
                              sh, height, stride_dst, 1 << sh);
                } else {
                    copy_vert(dst - (1 << sh),
                              src_l,
                              sh, height, stride_dst, stride_src);
                }
            }
            if (src_r != NULL) {
                if (CTB(s->sao, x_ctb+1, y_ctb).type_idx[c_idx] == SAO_APPLIED) {
                    copy_vert(dst + (width << sh),
                              s->sao_pixel_buffer_v[c_idx] + (((2 * x_ctb + 2) * h + y0) << sh),
                              sh, height, stride_dst, 1 << sh);
                } else {
                    copy_vert(dst + (width << sh),
                              src_r,
                              sh, height, stride_dst, stride_src);
                }
            }

            copy_CTB(dst,
                     src,
                     width << sh,
                     height, stride_dst, stride_src);

            copy_CTB_to_hv(s, src, stride_src, x0, y0, width, height, c_idx,
                           x_ctb, y_ctb);
            if (sliced && c_idx != 0)
            {
                // Class always the same for both U & V (which is just as well :-))
                s->hevcdsp.sao_edge_filter_c[tab](src, dst, stride_src,
                                                sao->offset_val[1], sao->offset_val[2], sao->eo_class[1],
                                                width, height);
                s->hevcdsp.sao_edge_restore_c[restore](src, dst,
                                                    stride_src, stride_dst,
                                                    sao,
                                                    edges, width,
                                                    height, c_idx,
                                                    vert_edge,
                                                    horiz_edge,
                                                    diag_edge);
            }
            else
            {
                s->hevcdsp.sao_edge_filter[tab](src, dst, stride_src, sao->offset_val[c_idx],
                                                sao->eo_class[c_idx], width, height);
                s->hevcdsp.sao_edge_restore[restore](src, dst,
                                                    stride_src, stride_dst,
                                                    sao,
                                                    edges, width,
                                                    height, c_idx,
                                                    vert_edge,
                                                    horiz_edge,
                                                    diag_edge);
            }
            // ??? Does this actually work for chroma ???
            restore_tqb_pixels(s, src, dst, stride_src, stride_dst,
                               x, y, width, height, c_idx);
            sao->type_idx[c_idx] = SAO_APPLIED;
            break;
        }
        }
    }

#if RPI_ZC_SAND_8_IN_10_BUF
    if (s->frame->format == AV_PIX_FMT_SAND64_10 && s->frame->buf[RPI_ZC_SAND_8_IN_10_BUF] != NULL &&
        (((x + (1 << (s->ps.sps->log2_ctb_size))) & 255) == 0 || edges[2]))
    {
        const unsigned int stride1 = frame_stride1(s->frame, 1);
        const unsigned int stride2 = av_rpi_sand_frame_stride2(s->frame);
        const unsigned int xoff = (x >> 8) * stride2 * stride1;
        const unsigned int ctb_size = (1 << s->ps.sps->log2_ctb_size);
        const uint8_t * const sy = s->frame->data[0] + xoff * 4 + y * stride1;
        uint8_t * const dy = s->frame->buf[4]->data + xoff * 2 + y * stride1;
        const uint8_t * const sc = s->frame->data[1] + xoff * 4 + (y >> 1) * stride1;
        uint8_t * const dc = s->frame->buf[4]->data + (s->frame->data[1] - s->frame->data[0]) + xoff * 2 + (y >> 1) * stride1;
        const unsigned int wy = !edges[2] ? 256 : s->ps.sps->width - (x & ~255);
        const unsigned int hy = !edges[3] ? ctb_size : s->ps.sps->height - y;

//        printf("dy=%p/%p, stride1=%d, stride2=%d, sy=%p/%p, wy=%d, hy=%d, x=%d, y=%d, cs=%d\n", dy, dc, stride1, stride2, sy, sc, wy, hy, x, y, ctb_size);
        av_rpi_sand16_to_sand8(dy, stride1, stride2, sy, stride1, stride2, wy, hy, 3);
        av_rpi_sand16_to_sand8(dc, stride1, stride2, sc, stride1, stride2, wy, hy >> 1, 3);
    }
#endif
}

// Returns 2 or 0.
static int get_pcm(HEVCRpiContext *s, int x, int y)
{
    int log2_min_pu_size = s->ps.sps->log2_min_pu_size;
    int x_pu, y_pu;

    if (x < 0 || y < 0)
        return 2;

    x_pu = x >> log2_min_pu_size;
    y_pu = y >> log2_min_pu_size;

    if (x_pu >= s->ps.sps->min_pu_width || y_pu >= s->ps.sps->min_pu_height)
        return 2;
    return s->is_pcm[y_pu * s->ps.sps->min_pu_width + x_pu];
}

#define TC_CALC(qp, bs)                                                 \
    tctable[av_clip((qp) + DEFAULT_INTRA_TC_OFFSET * ((bs) - 1) +       \
                    (tc_offset & -2),                                   \
                    0, MAX_QP + DEFAULT_INTRA_TC_OFFSET)]

static void deblocking_filter_CTB(HEVCRpiContext *s, int x0, int y0)
{
    uint8_t *src;
    int x, y;
    int beta;
    int32_t tc[2];
    uint8_t no_p[2] = { 0 };
    uint8_t no_q[2] = { 0 };

    int log2_ctb_size = s->ps.sps->log2_ctb_size;
    int x_end, x_end2, y_end;
    int ctb_size        = 1 << log2_ctb_size;
    int ctb             = (x0 >> log2_ctb_size) +
                          (y0 >> log2_ctb_size) * s->ps.sps->ctb_width;
    int cur_tc_offset   = s->deblock[ctb].tc_offset;
    int cur_beta_offset = s->deblock[ctb].beta_offset;
    int left_tc_offset, left_beta_offset;
    int tc_offset, beta_offset;
    int pcmf = (s->ps.sps->pcm_enabled_flag &&
                s->ps.sps->pcm.loop_filter_disable_flag) ||
               s->ps.pps->transquant_bypass_enable_flag;

#ifdef DISABLE_DEBLOCK_NONREF
    if (!s->used_for_ref)
      return; // Don't deblock non-reference frames
#endif
#ifdef DISABLE_DEBLOCK
    return;
#endif
    if (!s->used_for_ref && s->avctx->skip_loop_filter >= AVDISCARD_NONREF)
        return;
    if (x0) {
        left_tc_offset   = s->deblock[ctb - 1].tc_offset;
        left_beta_offset = s->deblock[ctb - 1].beta_offset;
    } else {
        left_tc_offset   = 0;
        left_beta_offset = 0;
    }

    x_end = x0 + ctb_size;
    if (x_end > s->ps.sps->width)
        x_end = s->ps.sps->width;
    y_end = y0 + ctb_size;
    if (y_end > s->ps.sps->height)
        y_end = s->ps.sps->height;

    tc_offset   = cur_tc_offset;
    beta_offset = cur_beta_offset;

    x_end2 = x_end;
    if (x_end2 != s->ps.sps->width)
        x_end2 -= 8;
    for (y = y0; y < y_end; y += 8) {
        // vertical filtering luma
        for (x = x0 ? x0 : 8; x < x_end; x += 8) {
            const int bs0 = s->vertical_bs[(x +  y      * s->bs_width) >> 2];
            const int bs1 = s->vertical_bs[(x + (y + 4) * s->bs_width) >> 2];
            if (bs0 || bs1) {
                const int qp = (get_qPy(s, x - 1, y)     + get_qPy(s, x, y)     + 1) >> 1;

                beta = betatable[av_clip(qp + beta_offset, 0, MAX_QP)];

                tc[0]   = bs0 ? TC_CALC(qp, bs0) : 0;
                tc[1]   = bs1 ? TC_CALC(qp, bs1) : 0;
                if (pcmf) {
                    no_p[0] = get_pcm(s, x - 1, y);
                    no_p[1] = get_pcm(s, x - 1, y + 4);
                    no_q[0] = get_pcm(s, x, y);
                    no_q[1] = get_pcm(s, x, y + 4);
                }

                // This copes properly with no_p/no_q
                s->hevcdsp.hevc_v_loop_filter_luma2(av_rpi_sand_frame_pos_y(s->frame, x, y),
                                                 frame_stride1(s->frame, LUMA),
                                                 beta, tc, no_p, no_q,
                                                 av_rpi_sand_frame_pos_y(s->frame, x - 4, y));
                // *** VPU deblock lost here
            }
        }

        if(!y)
             continue;

        // horizontal filtering luma
        for (x = x0 ? x0 - 8 : 0; x < x_end2; x += 8) {
            const int bs0 = s->horizontal_bs[( x      + y * s->bs_width) >> 2];
            const int bs1 = s->horizontal_bs[((x + 4) + y * s->bs_width) >> 2];
            if (bs0 || bs1) {
                const int qp = (get_qPy(s, x, y - 1)     + get_qPy(s, x, y)     + 1) >> 1;

                tc_offset   = x >= x0 ? cur_tc_offset : left_tc_offset;
                beta_offset = x >= x0 ? cur_beta_offset : left_beta_offset;

                beta = betatable[av_clip(qp + beta_offset, 0, MAX_QP)];
                tc[0]   = bs0 ? TC_CALC(qp, bs0) : 0;
                tc[1]   = bs1 ? TC_CALC(qp, bs1) : 0;
                src = av_rpi_sand_frame_pos_y(s->frame, x, y);

                if (pcmf) {
                    no_p[0] = get_pcm(s, x, y - 1);
                    no_p[1] = get_pcm(s, x + 4, y - 1);
                    no_q[0] = get_pcm(s, x, y);
                    no_q[1] = get_pcm(s, x + 4, y);
                    s->hevcdsp.hevc_h_loop_filter_luma_c(src,
                                                         frame_stride1(s->frame, LUMA),
                                                         beta, tc, no_p, no_q);
                } else
#ifdef RPI_DEBLOCK_VPU
                if (s->enable_rpi_deblock) {
                    uint8_t (*setup)[2][2][4];
                    int num16 = (y>>4)*s->setup_width + (x>>4);
                    int a = ((x>>3) & 1) << 1;
                    int b = (y>>3) & 1;
                    setup = s->dvq->y_setup_arm[num16];
                    setup[1][b][0][a] = beta;
                    setup[1][b][0][a + 1] = beta;
                    setup[1][b][1][a] = tc[0];
                    setup[1][b][1][a + 1] = tc[1];
                } else
#endif
                    s->hevcdsp.hevc_h_loop_filter_luma(src,
                                                       frame_stride1(s->frame, LUMA),
                                                       beta, tc, no_p, no_q);
            }
        }
    }

    if (ctx_cfmt(s) != 0) {
        const int v = 2;
        const int h = 2;

        // vertical filtering chroma
        for (y = y0; y < y_end; y += 8 * v) {
//                const int demi_y = y + 4 * v >= s->ps.sps->height;
            const int demi_y = 0;
            for (x = x0 ? x0 : 8 * h; x < x_end; x += 8 * h) {
                const int bs0 = s->vertical_bs[(x +  y          * s->bs_width) >> 2];
                const int bs1 = s->vertical_bs[(x + (y + 4 * v) * s->bs_width) >> 2];

                if ((bs0 == 2) || (bs1 == 2)) {
                    const int qp0 = (get_qPy(s, x - 1, y)         + get_qPy(s, x, y)         + 1) >> 1;
                    const int qp1 = (get_qPy(s, x - 1, y + 4 * v) + get_qPy(s, x, y + 4 * v) + 1) >> 1;
                    unsigned int no_f = !demi_y ? 0 : 2 | 8;

                    // tc_offset here should be set to cur_tc_offset I think
                    const uint32_t tc4 =
                        ((bs0 != 2) ? 0 : chroma_tc(s, qp0, 1, cur_tc_offset) | (chroma_tc(s, qp0, 2, cur_tc_offset) << 16)) |
                        ((bs1 != 2) ? 0 : ((chroma_tc(s, qp1, 1, cur_tc_offset) | (chroma_tc(s, qp1, 2, cur_tc_offset) << 16)) << 8));

                    if (tc4 == 0)
                        continue;

                    if (pcmf) {
                        no_f =
                            (get_pcm(s, x - 1, y) ? 1 : 0) |
                            (get_pcm(s, x - 1, y + 4 * v) ? 2 : 0) |
                            (get_pcm(s, x, y) ? 4 : 0) |
                            (get_pcm(s, x, y + 4 * v) ? 8 : 0);
                        if (no_f == 0xf)
                            continue;
                    }

                    s->hevcdsp.hevc_v_loop_filter_uv2(av_rpi_sand_frame_pos_c(s->frame, x >> 1, y >> 1),
                                                   frame_stride1(s->frame, 1),
                                                   tc4,
                                                   av_rpi_sand_frame_pos_c(s->frame, (x >> 1) - 2, y >> 1),
                                                   no_f);
                }
            }

            if (y == 0)
                continue;

            // horizontal filtering chroma
            tc_offset = x0 ? left_tc_offset : cur_tc_offset;
            x_end2 = x_end;
            if (x_end != s->ps.sps->width)
                x_end2 = x_end - 8 * h;

            for (x = x0 ? x0 - 8 * h: 0; x < x_end2; x += 8 * h) {
//                    const int demi_x = x + 4 * v >= s->ps.sps->width;
                const int demi_x = 0;

                const int bs0 = s->horizontal_bs[( x          + y * s->bs_width) >> 2];
                const int bs1 = s->horizontal_bs[((x + 4 * h) + y * s->bs_width) >> 2];
                if ((bs0 == 2) || (bs1 == 2)) {
                    const int qp0 = bs0 == 2 ? (get_qPy(s, x,         y - 1) + get_qPy(s, x,         y) + 1) >> 1 : 0;
                    const int qp1 = bs1 == 2 ? (get_qPy(s, x + 4 * h, y - 1) + get_qPy(s, x + 4 * h, y) + 1) >> 1 : 0;
                    const uint32_t tc4 =
                        ((bs0 != 2) ? 0 : chroma_tc(s, qp0, 1, tc_offset) | (chroma_tc(s, qp0, 2, tc_offset) << 16)) |
                        ((bs1 != 2) ? 0 : ((chroma_tc(s, qp1, 1, cur_tc_offset) | (chroma_tc(s, qp1, 2, cur_tc_offset) << 16)) << 8));
                    unsigned int no_f = !demi_x ? 0 : 2 | 8;

                    if (tc4 == 0)
                        continue;

                    if (pcmf) {
                        no_f =
                            (get_pcm(s, x,         y - 1) ? 1 : 0) |
                            (get_pcm(s, x + 4 * h, y - 1) ? 2 : 0) |
                            (get_pcm(s, x,         y)     ? 4 : 0) |
                            (get_pcm(s, x + 4 * h, y)     ? 8 : 0);

                        if (no_f == 0xf)
                            continue;
                    }

                    s->hevcdsp.hevc_h_loop_filter_uv(av_rpi_sand_frame_pos_c(s->frame, x >> 1, y >> 1),
                                                     frame_stride1(s->frame, LUMA),
                                                     tc4, no_f);
                }
            }
            // **** VPU deblock code gone from here....
        }
    }
}


void ff_hevc_rpi_deblocking_boundary_strengths(const HEVCRpiContext * const s, HEVCRpiLocalContext * const lc, int x0, int y0,
                                           int log2_trafo_size)
{
    MvField *tab_mvf     = s->ref->tab_mvf;
    int log2_min_pu_size = s->ps.sps->log2_min_pu_size;
    int log2_min_tu_size = s->ps.sps->log2_min_tb_size;
    int min_pu_width     = s->ps.sps->min_pu_width;
    int min_tu_width     = s->ps.sps->min_tb_width;
    int boundary_upper, boundary_left;
    int i, j;
    const RefPicList *rpl = s->ref->refPicList;
    const unsigned int log2_dup = FFMIN(log2_min_pu_size, log2_trafo_size);
    const unsigned int min_pu_in_4pix = 1 << (log2_dup - 2);  // Dup
    const unsigned int trafo_in_min_pus = 1 << (log2_trafo_size - log2_dup); // Rep
    int y_pu             = y0 >> log2_min_pu_size;
    int x_pu             = x0 >> log2_min_pu_size;
    MvField *curr        = &tab_mvf[y_pu * min_pu_width + x_pu];
    int is_intra         = curr->pred_flag == PF_INTRA;
    int inc              = log2_min_pu_size == 2 ? 2 : 1;
    uint8_t *bs;

#ifdef DISABLE_STRENGTHS
    return;
#endif

    boundary_upper = y0 > 0 && !(y0 & 7);
    if (boundary_upper &&
        ((!s->sh.slice_loop_filter_across_slices_enabled_flag &&
          lc->boundary_flags & BOUNDARY_UPPER_SLICE &&
          (y0 % (1 << s->ps.sps->log2_ctb_size)) == 0) ||
         (!s->ps.pps->loop_filter_across_tiles_enabled_flag &&
          lc->boundary_flags & BOUNDARY_UPPER_TILE &&
          (y0 % (1 << s->ps.sps->log2_ctb_size)) == 0)))
        boundary_upper = 0;

    bs = &s->horizontal_bs[(x0 + y0 * s->bs_width) >> 2];

    if (boundary_upper) {
        const RefPicList *const rpl_top = (lc->boundary_flags & BOUNDARY_UPPER_SLICE) ?
                              ff_hevc_rpi_get_ref_list(s, s->ref, x0, y0 - 1) :
                              rpl;
        MvField *top = curr - min_pu_width;

        if (is_intra) {
            for (i = 0; i < (1 << log2_trafo_size); i += 4)
                bs[i >> 2] = 2;

        } else {
            int y_tu = y0 >> log2_min_tu_size;
            int x_tu = x0 >> log2_min_tu_size;
            uint8_t *curr_cbf_luma = &s->cbf_luma[y_tu * min_tu_width + x_tu];
            uint8_t *top_cbf_luma = curr_cbf_luma - min_tu_width;

            s->hevcdsp.hevc_deblocking_boundary_strengths(trafo_in_min_pus,
                    min_pu_in_4pix, sizeof (MvField), 4 >> 2,
                    rpl[0].list, rpl[1].list, rpl_top[0].list, rpl_top[1].list,
                    curr, top, bs);

            for (i = 0; i < (1 << log2_trafo_size); i += 4) {
                int i_pu = i >> log2_min_pu_size;
                int i_tu = i >> log2_min_tu_size;

                if (top[i_pu].pred_flag == PF_INTRA)
                    bs[i >> 2] = 2;
                else if (curr_cbf_luma[i_tu] || top_cbf_luma[i_tu])
                    bs[i >> 2] = 1;
            }
        }
    }

    if (!is_intra) {
        for (j = inc; j < trafo_in_min_pus; j += inc) {
            MvField *top;

            curr += min_pu_width * inc;
            top = curr - min_pu_width;
            bs += s->bs_width * inc << log2_min_pu_size >> 2;

            s->hevcdsp.hevc_deblocking_boundary_strengths(trafo_in_min_pus,
                    min_pu_in_4pix, sizeof (MvField), 4 >> 2,
                    rpl[0].list, rpl[1].list, rpl[0].list, rpl[1].list,
                    curr, top, bs);
        }
    }

    boundary_left = x0 > 0 && !(x0 & 7);
    if (boundary_left &&
        ((!s->sh.slice_loop_filter_across_slices_enabled_flag &&
          lc->boundary_flags & BOUNDARY_LEFT_SLICE &&
          (x0 % (1 << s->ps.sps->log2_ctb_size)) == 0) ||
         (!s->ps.pps->loop_filter_across_tiles_enabled_flag &&
          lc->boundary_flags & BOUNDARY_LEFT_TILE &&
          (x0 % (1 << s->ps.sps->log2_ctb_size)) == 0)))
        boundary_left = 0;

    curr = &tab_mvf[y_pu * min_pu_width + x_pu];
    bs = &s->vertical_bs[(x0 + y0 * s->bs_width) >> 2];

    if (boundary_left) {
        const RefPicList *rpl_left = (lc->boundary_flags & BOUNDARY_LEFT_SLICE) ?
                               ff_hevc_rpi_get_ref_list(s, s->ref, x0 - 1, y0) :
                               rpl;
        MvField *left = curr - 1;

        if (is_intra) {
            for (j = 0; j < (1 << log2_trafo_size); j += 4)
                bs[j * s->bs_width >> 2] = 2;

        } else {
            int y_tu = y0 >> log2_min_tu_size;
            int x_tu = x0 >> log2_min_tu_size;
            uint8_t *curr_cbf_luma = &s->cbf_luma[y_tu * min_tu_width + x_tu];
            uint8_t *left_cbf_luma = curr_cbf_luma - 1;

            s->hevcdsp.hevc_deblocking_boundary_strengths(trafo_in_min_pus,
                    min_pu_in_4pix, min_pu_width * sizeof (MvField), 4 * s->bs_width >> 2,
                    rpl[0].list, rpl[1].list, rpl_left[0].list, rpl_left[1].list,
                    curr, left, bs);

            for (j = 0; j < (1 << log2_trafo_size); j += 4) {
                int j_pu = j >> log2_min_pu_size;
                int j_tu = j >> log2_min_tu_size;

                if (left[j_pu * min_pu_width].pred_flag == PF_INTRA)
                    bs[j * s->bs_width >> 2] = 2;
                else if (curr_cbf_luma[j_tu * min_tu_width] || left_cbf_luma[j_tu * min_tu_width])
                    bs[j * s->bs_width >> 2] = 1;
            }
        }
    }

    if (!is_intra) {
        for (i = inc; i < trafo_in_min_pus; i += inc) {
            MvField *left;

            curr += inc;
            left = curr - 1;
            bs += inc << log2_min_pu_size >> 2;

            s->hevcdsp.hevc_deblocking_boundary_strengths(trafo_in_min_pus,
                    min_pu_in_4pix, min_pu_width * sizeof (MvField), 4 * s->bs_width >> 2,
                    rpl[0].list, rpl[1].list, rpl[0].list, rpl[1].list,
                    curr, left, bs);
        }
    }
}

#undef LUMA
#undef CB
#undef CR

#ifdef RPI_DEBLOCK_VPU
// ff_hevc_rpi_flush_buffer_lines
// flushes and invalidates all pixel rows in [start,end-1]
static void ff_hevc_rpi_flush_buffer_lines(HEVCRpiContext *s, int start, int end, int flush_luma, int flush_chroma)
{
    rpi_cache_buf_t cbuf;
    rpi_cache_flush_env_t * const rfe = rpi_cache_flush_init(&cbuf);
    rpi_cache_flush_add_frame_block(rfe, s->frame, RPI_CACHE_FLUSH_MODE_WB_INVALIDATE,
      0, start, s->ps.sps->width, end - start, ctx_vshift(s, 1), flush_luma, flush_chroma);
    rpi_cache_flush_finish(rfe);
}

/* rpi_deblock deblocks an entire row of ctbs using the VPU */
static void rpi_deblock(HEVCRpiContext *s, int y, int ctb_size)
{
  int num16high = (ctb_size+15)>>4;  // May go over bottom of the image, but setup will be zero for these so should have no effect.
  // TODO check that image allocation is large enough for this to be okay as well.
  
  // Flush image, 4 lines above to bottom of ctb stripe
  ff_hevc_rpi_flush_buffer_lines(s, FFMAX(y-4,0), y+ctb_size, 1, 1);
  // TODO flush buffer of beta/tc setup when it becomes cached

  // Prepare three commands at once to avoid calling overhead
  s->dvq->vpu_cmds_arm[0][0] = get_vc_address_y(s->frame) + s->frame->linesize[0] * y;
  s->dvq->vpu_cmds_arm[0][1] = s->frame->linesize[0];
  s->dvq->vpu_cmds_arm[0][2] = s->setup_width;
  s->dvq->vpu_cmds_arm[0][3] = (int) ( s->dvq->y_setup_vc + s->setup_width * (y>>4) );
  s->dvq->vpu_cmds_arm[0][4] = num16high;
  s->dvq->vpu_cmds_arm[0][5] = 2;

  s->dvq->vpu_cmds_arm[1][0] = get_vc_address_u(s->frame) + s->frame->linesize[1] * (y>> s->ps.sps->vshift[1]);
  s->dvq->vpu_cmds_arm[1][1] = s->frame->linesize[1];
  s->dvq->vpu_cmds_arm[1][2] = s->uv_setup_width;
  s->dvq->vpu_cmds_arm[1][3] = (int) ( s->dvq->uv_setup_vc + s->uv_setup_width * ((y>>4)>> s->ps.sps->vshift[1]) );
  s->dvq->vpu_cmds_arm[1][4] = (num16high + 1) >> s->ps.sps->vshift[1];
  s->dvq->vpu_cmds_arm[1][5] = 3;

  s->dvq->vpu_cmds_arm[2][0] = get_vc_address_v(s->frame) + s->frame->linesize[2] * (y>> s->ps.sps->vshift[2]);
  s->dvq->vpu_cmds_arm[2][1] = s->frame->linesize[2];
  s->dvq->vpu_cmds_arm[2][2] = s->uv_setup_width;
  s->dvq->vpu_cmds_arm[2][3] = (int) ( s->dvq->uv_setup_vc + s->uv_setup_width * ((y>>4)>> s->ps.sps->vshift[1]) );
  s->dvq->vpu_cmds_arm[2][4] = (num16high + 1) >> s->ps.sps->vshift[1];
  s->dvq->vpu_cmds_arm[2][5] = 4;
  
  // Call VPU
  {
      vpu_qpu_job_env_t qvbuf;
      const vpu_qpu_job_h vqj = vpu_qpu_job_init(&qvbuf);
      vpu_qpu_job_add_vpu(vqj, vpu_get_fn(s->ps.sps->bit_depth), s->dvq->vpu_cmds_vc, 3, 0, 0, 0, 5);  // 5 means to do all the commands
      vpu_qpu_job_add_sync_this(vqj, &s->dvq->cmd_id);
      vpu_qpu_job_finish(vqj);
  }

  s->dvq_n = (s->dvq_n + 1) & (RPI_DEBLOCK_VPU_Q_COUNT - 1);
  s->dvq = s->dvq_ents + s->dvq_n;

  vpu_qpu_wait(&s->dvq->cmd_id);
}

#endif

void ff_hevc_rpi_hls_filter(HEVCRpiContext * const s, const int x, const int y, const int ctb_size)
{
    const int x_end = x >= s->ps.sps->width  - ctb_size;

    if (s->avctx->skip_loop_filter < AVDISCARD_ALL)
        deblocking_filter_CTB(s, x, y);

#ifdef RPI_DEBLOCK_VPU
    if (s->enable_rpi_deblock && x_end)
    {
      int y_at_end = y >= s->ps.sps->height - ctb_size;
      int height = 64;  // Deblock in units 64 high to avoid too many VPU calls
      int y_start = y&~63;
      if (y_at_end) height = s->ps.sps->height - y_start;
      if ((((y+ctb_size)&63)==0) || y_at_end) {
        rpi_deblock(s, y_start, height);
      }
    }
#endif

    if (s->ps.sps->sao_enabled) {
        int y_end = y >= s->ps.sps->height - ctb_size;
        if (y != 0 && x != 0)
            sao_filter_CTB(s, x - ctb_size, y - ctb_size);
        if (x != 0 && y_end)
            sao_filter_CTB(s, x - ctb_size, y);
        if (y != 0 && x_end)
            sao_filter_CTB(s, x, y - ctb_size);
        if (x_end && y_end)
            sao_filter_CTB(s, x , y);
    }
}

void ff_hevc_rpi_hls_filters(HEVCRpiContext *s, int x_ctb, int y_ctb, int ctb_size)
{
    // * This can break strict L->R then U->D ordering - mostly it doesn't matter
    // Never called if rpi_enabled so no need for cache flush ops
    const int x_end = x_ctb >= s->ps.sps->width  - ctb_size;
    const int y_end = y_ctb >= s->ps.sps->height - ctb_size;
    if (y_ctb && x_ctb)
        ff_hevc_rpi_hls_filter(s, x_ctb - ctb_size, y_ctb - ctb_size, ctb_size);
    if (y_ctb && x_end)
    {
        ff_hevc_rpi_hls_filter(s, x_ctb, y_ctb - ctb_size, ctb_size);
        // Signal progress - this is safe for SAO
        if (s->threads_type == FF_THREAD_FRAME && y_ctb > ctb_size)
            ff_hevc_rpi_progress_signal_recon(s, y_ctb - ctb_size - 1);
    }
    if (x_ctb && y_end)
        ff_hevc_rpi_hls_filter(s, x_ctb - ctb_size, y_ctb, ctb_size);
    if (x_end && y_end)
    {
        ff_hevc_rpi_hls_filter(s, x_ctb, y_ctb, ctb_size);
        // All done - signal such
        if (s->threads_type == FF_THREAD_FRAME)
            ff_hevc_rpi_progress_signal_recon(s, INT_MAX);
    }
}
