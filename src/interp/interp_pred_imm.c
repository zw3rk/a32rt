/*
 * src/interp/interp_pred_imm.c — Predicated immediates & logical-imm helpers (A32-R20-P0)
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "a64_emit.h"

void a64_emit_movn_w(a64_buf_t *b, unsigned rd, uint16_t imm16, unsigned hw)
{
    a64_emit(b, 0x12800000U | ((hw & 1U) << 21) | ((uint32_t)imm16 << 5) | (rd & 31U));
}

static uint32_t ror_elem(uint32_t v, unsigned rot, unsigned esize)
{
    rot %= esize;
    if (rot == 0U) {
        return v & ((esize >= 32U) ? 0xFFFFFFFFU : ((1U << esize) - 1U));
    }
    uint32_t mask = (esize >= 32U) ? 0xFFFFFFFFU : ((1U << esize) - 1U);
    v &= mask;
    return ((v >> rot) | (v << (esize - rot))) & mask;
}

bool a64_encode_logical_imm32(uint32_t imm, unsigned *out_imms, unsigned *out_immr)
{
    static const unsigned k_sizes[5] = { 32U, 16U, 8U, 4U, 2U };
    for (unsigned si = 0U; si < 5U; si++) {
        unsigned esize = k_sizes[si];
        uint32_t emask = (esize >= 32U) ? 0xFFFFFFFFU : ((1U << esize) - 1U);
        uint32_t elem = imm & emask;
        unsigned reps = 32U / esize;
        uint32_t rep = 0U;
        for (unsigned r = 0U; r < reps; r++) {
            rep = (rep << esize) | elem;
        }
        if (rep != imm) {
            continue;
        }
        for (unsigned ones = 1U; ones < esize; ones++) {
            uint32_t base;
            if (ones >= 32U) {
                base = 0xFFFFFFFFU;
            } else if (esize >= 32U) {
                base = (ones == 32U) ? 0xFFFFFFFFU : ((1U << ones) - 1U);
            } else {
                base = (uint32_t)((1U << ones) - 1U);
            }
            for (unsigned rot = 0U; rot < esize; rot++) {
                if (ror_elem(base, rot, esize) != elem) {
                    continue;
                }
                unsigned s = ones - 1U;
                unsigned imms = 0U;
                unsigned immr = 0U;
                if (esize == 32U) {
                    imms = s & 0x3FU;
                    immr = (esize - rot) % esize;
                } else if (esize == 16U) {
                    imms = 0x20U | (s & 0xFU);
                    immr = rot & 0xFU;
                } else if (esize == 8U) {
                    imms = 0x30U | (s & 0x7U);
                    immr = rot & 0x7U;
                } else if (esize == 4U) {
                    imms = 0x38U | (s & 0x3U);
                    immr = rot & 0x3U;
                } else {
                    imms = 0x3CU | (s & 0x1U);
                    immr = rot & 0x1U;
                }
                if (out_imms != NULL) {
                    *out_imms = imms;
                }
                if (out_immr != NULL) {
                    *out_immr = immr;
                }
                return true;
            }
        }
    }
    return false;
}

void a64_emit_logical_imm_w(a64_buf_t *b, unsigned opc, unsigned rd, unsigned rn, uint32_t imm)
{
    unsigned imms = 0U;
    unsigned immr = 0U;
    if (!a64_encode_logical_imm32(imm, &imms, &immr)) {
        return;
    }
    a64_emit(b, ((opc & 3U) << 29) | 0x12000000U | ((immr & 0x3FU) << 16) |
        ((imms & 0x3FU) << 10) | ((rn & 31U) << 5) | (rd & 31U));
}

void a64_emit_csinc_w(a64_buf_t *b, unsigned rd, unsigned rn, unsigned rm, unsigned cond)
{
    a64_emit(b, 0x1A800400U | ((rm & 31U) << 16) | ((cond & 15U) << 12) |
        ((rn & 31U) << 5) | (rd & 31U));
}

void a64_emit_csinv_w(a64_buf_t *b, unsigned rd, unsigned rn, unsigned rm, unsigned cond)
{
    a64_emit(b, 0x5A800000U | ((rm & 31U) << 16) | ((cond & 15U) << 12) |
        ((rn & 31U) << 5) | (rd & 31U));
}
