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

#include "rpi_hevcdec.h"

#include "rpi_hevcpred.h"

#define PRED_C 0
#define BIT_DEPTH 8
#include "rpi_hevcpred_template.c"
#undef BIT_DEPTH

#define BIT_DEPTH 9
#include "rpi_hevcpred_template.c"
#undef BIT_DEPTH

#define BIT_DEPTH 10
#include "rpi_hevcpred_template.c"
#undef BIT_DEPTH

#define BIT_DEPTH 12
#include "rpi_hevcpred_template.c"
#undef BIT_DEPTH
#undef PRED_C

#define PRED_C 1
#define BIT_DEPTH 8
#include "rpi_hevcpred_template.c"
#undef BIT_DEPTH

#define BIT_DEPTH 9
#include "rpi_hevcpred_template.c"
#undef BIT_DEPTH

#define BIT_DEPTH 10
#include "rpi_hevcpred_template.c"
#undef BIT_DEPTH

#define BIT_DEPTH 12
#include "rpi_hevcpred_template.c"
#undef BIT_DEPTH
#undef PRED_C

void ff_hevc_rpi_pred_init(HEVCPredContext *hpc, int bit_depth)
{
#undef FUNC
#define FUNC(a, depth) a ## _ ## depth

#undef FUNCC
#define FUNCC(a, depth) a ## _ ## depth ## _c

#define HEVC_PRED_Y(depth)                                \
    hpc->intra_pred[0]   = FUNC(intra_pred_2, depth);   \
    hpc->intra_pred[1]   = FUNC(intra_pred_3, depth);   \
    hpc->intra_pred[2]   = FUNC(intra_pred_4, depth);   \
    hpc->intra_pred[3]   = FUNC(intra_pred_5, depth);   \
    hpc->pred_planar[0]  = FUNC(pred_planar_0, depth);  \
    hpc->pred_planar[1]  = FUNC(pred_planar_1, depth);  \
    hpc->pred_planar[2]  = FUNC(pred_planar_2, depth);  \
    hpc->pred_planar[3]  = FUNC(pred_planar_3, depth);  \
    hpc->pred_dc         = FUNC(pred_dc, depth);        \
    hpc->pred_angular[0] = FUNC(pred_angular_0, depth); \
    hpc->pred_angular[1] = FUNC(pred_angular_1, depth); \
    hpc->pred_angular[2] = FUNC(pred_angular_2, depth); \
    hpc->pred_angular[3] = FUNC(pred_angular_3, depth);

#define HEVC_PRED_C(depth)                                \
    hpc->intra_pred_c[0]   = FUNCC(intra_pred_2, depth);   \
    hpc->intra_pred_c[1]   = FUNCC(intra_pred_3, depth);   \
    hpc->intra_pred_c[2]   = FUNCC(intra_pred_4, depth);   \
    hpc->intra_pred_c[3]   = FUNCC(intra_pred_5, depth);   \
    hpc->pred_planar_c[0]  = FUNCC(pred_planar_0, depth);  \
    hpc->pred_planar_c[1]  = FUNCC(pred_planar_1, depth);  \
    hpc->pred_planar_c[2]  = FUNCC(pred_planar_2, depth);  \
    hpc->pred_planar_c[3]  = FUNCC(pred_planar_3, depth);  \
    hpc->pred_dc_c         = FUNCC(pred_dc, depth);        \
    hpc->pred_angular_c[0] = FUNCC(pred_angular_0, depth); \
    hpc->pred_angular_c[1] = FUNCC(pred_angular_1, depth); \
    hpc->pred_angular_c[2] = FUNCC(pred_angular_2, depth); \
    hpc->pred_angular_c[3] = FUNCC(pred_angular_3, depth);

#define HEVC_PRED(depth) \
    HEVC_PRED_Y(depth); \
    HEVC_PRED_C(depth);

    switch (bit_depth) {
    case 9:
        HEVC_PRED(9);
        break;
    case 10:
        HEVC_PRED(10);
        break;
    case 12:
        HEVC_PRED(12);
        break;
    default:
        HEVC_PRED(8);
        break;
    }

    if (ARCH_MIPS)
        ff_hevc_rpi_pred_init_mips(hpc, bit_depth);
}
