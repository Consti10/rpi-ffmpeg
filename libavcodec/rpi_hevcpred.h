/*
 * HEVC video Decoder
 *
 * Copyright (C) 2012 - 2013 Guillaume Martres
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

#ifndef AVCODEC_RPI_HEVCPRED_H
#define AVCODEC_RPI_HEVCPRED_H

#include <stddef.h>
#include <stdint.h>
#include "config.h"

struct HEVCRpiContext;
struct HEVCRpiLocalContext;

typedef struct HEVCRpiPredContext {
    void (*intra_pred[4])(const struct HEVCRpiContext * const s, struct HEVCRpiLocalContext * const lc, int x0, int y0, int c_idx);

    void (*pred_planar[4])(uint8_t *src, const uint8_t *top,
                           const uint8_t *left, ptrdiff_t stride);
    void (*pred_dc[4])(uint8_t *src, const uint8_t *top, const uint8_t *left,
                    ptrdiff_t stride);
    void (*pred_angular[4])(uint8_t *src, const uint8_t *top,
                            const uint8_t *left, ptrdiff_t stride,
                            int c_idx, int mode);
    void (*intra_pred_c[4])(const struct HEVCRpiContext * const s, struct HEVCRpiLocalContext * const lc, int x0, int y0, int c_idx);

    void (*pred_planar_c[4])(uint8_t *src, const uint8_t *top,
                           const uint8_t *left, ptrdiff_t stride);
    void (*pred_dc_c[4])(uint8_t *src, const uint8_t *top, const uint8_t *left,
                    ptrdiff_t stride);
    void (*pred_angular_c[4])(uint8_t *src, const uint8_t *top,
                            const uint8_t *left, ptrdiff_t stride,
                            int c_idx, int mode);
} HEVCRpiPredContext;

void ff_hevc_rpi_pred_init(HEVCRpiPredContext *hpc, int bit_depth);

#endif /* AVCODEC_RPI_HEVCPRED_H */
