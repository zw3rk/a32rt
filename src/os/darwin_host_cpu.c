/*
 * src/os/darwin_host_cpu.c — G02 bridge CPU + fault marshaling
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * ARM32 / Thumb-2 register, CPSR/IT-state and fault marshaling for the
 * narrow A32H_ABI 1.2 external Darwin bridge (src/os/darwin_host_bridge.c).
 * Split out so darwin_host_bridge.c stays under the 900-line limit.
 */

#define _GNU_SOURCE
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#define _XOPEN_SOURCE 600
#endif
#include "os/darwin_host_bridge.h"

#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>

/* AArch64 ESR WnR (bit 6 of the ISS) for a host sync-trap ucontext:
 * 1 = store, 0 = load, -1 = no ESR on this host. */
int a32_darwin_esr_wnr(void *uctx)
{
    if (uctx == NULL) {
        return -1;
    }
#if defined(__linux__)
    {
        const uint32_t *p = (const uint32_t *)
            ((const ucontext_t *)uctx)->uc_mcontext.__reserved;
        size_t n = sizeof(((const ucontext_t *)uctx)->uc_mcontext.__reserved)
            >> 2U;
        size_t i = 0U;
        while (i + 4U <= n) {
            uint32_t magic = p[i], size = p[i + 1U];
            size_t adv;
            if (magic == 0U && size == 0U) {
                break;
            }
            if (magic == 0x45535201U && size >= 16U) {
                uint64_t esr = (uint64_t)p[i + 2U] |
                    ((uint64_t)p[i + 3U] << 32);
                return (int)((esr >> 6) & 1ULL);
            }
            adv = ((size_t)size + 3U) >> 2U;
            if (adv < 4U || i + adv > n) {
                break;
            }
            i += adv;
        }
        return -1;
    }
#elif defined(__APPLE__)
    {
        uint32_t esr = ((const ucontext_t *)uctx)->uc_mcontext->__es.__esr;
        return (int)((esr >> 6) & 1U);
    }
#else
    return -1;
#endif
}

/* Thumb next-pc: 32-bit Thumb-2 when the first halfword >= 0xE800. */
static uint32_t thumb_next_pc(const a32_ctx_t *ctx, uint32_t pc)
{
    uint32_t a = pc & ~1U;
    uint16_t hw = 0U;
    if (!a32_guest_range_ok((a32_ctx_t *)ctx, a, 2U, (uint32_t)PROT_READ)) {
        return a + 2U;
    }
    memcpy(&hw, a32_guest_ptr(ctx, a), 2U);
    return a + ((hw >= 0xE800U) ? 4U : 2U);
}

/* r0..r15, pc, cpsr (NZCV + packed IT + T), fpscr, tls_tp; d0..d31 only
 * when full. The CPSR.IT bits are owned by ctx->itstate: clear the mask
 * and OR the packed value so a stale cpsr copy cannot desync the block. */
void a32_darwin_fill_cpu(const a32_ctx_t *ctx, a32h_darwin_cpu_state_t *cpu,
    bool full)
{
    bool t = (ctx->cpsr & A32_CPSR_T) != 0U;
    uint32_t it = ctx->itstate & 0xFFU;
    memset(cpu, 0, sizeof(*cpu));
    for (unsigned i = 0U; i < 16U; i++) {
        cpu->gpr[i] = ctx->regs[i];
    }
    cpu->pc = ctx->regs[15];
    cpu->next_pc = t ? thumb_next_pc(ctx, ctx->regs[15]) : ctx->regs[15] + 4U;
    cpu->lr = ctx->regs[14];
    cpu->cpsr_or_cr = (ctx->cpsr & ~(A32_CPSR_IT_MASK | A32_CPSR_T)) |
        (((it & 0xFCU) << 8) | ((it & 0x03U) << 25)) |
        (t ? A32_CPSR_T : 0U);
    cpu->fpscr = ctx->fpscr;
    cpu->tls_tp = ctx->tls_tp;
    if (full) {
        cpu->state_flags = A32H_DSTATE_FP_VALID;
        for (unsigned i = 0U; i < 32U; i++) {
            cpu->fpr[i] = ctx->vfp_d[i];
        }
    }
}

/* On A32_EXIT_SVC, both interp and DBT have already advanced ctx->regs[15]
 * past the SVC instruction (+2 in Thumb, +4 in ARM). The G02 contract
 * reports cpu->pc at the trapping SVC and cpu->next_pc at the resume PC. */
void a32_darwin_fill_svc_pc(const a32_ctx_t *ctx, a32h_darwin_cpu_state_t *cpu)
{
    uint32_t step = ((ctx->cpsr & A32_CPSR_T) != 0U) ? 2U : 4U;
    cpu->next_pc = ctx->regs[15];
    cpu->pc = ctx->regs[15] - step;
    cpu->gpr[15] = cpu->pc;
}

/* Host reply wins: gpr[14] is authoritative (a stale cpu->lr echo must
 * not clobber a host-installed signal-trampoline lr). Thumb mode resolves
 * from an odd pc first, else from the host CPSR.T; an even pc with a
 * clear T leaves Thumb (no stale T lock-in). */
void a32_darwin_apply_cpu(a32_ctx_t *ctx, const a32h_darwin_cpu_state_t *cpu)
{
    bool pc1 = (cpu->pc & 1U) != 0U;
    bool t = pc1 || ((cpu->cpsr_or_cr & A32_CPSR_T) != 0U);
    for (unsigned i = 0U; i < 16U; i++) {
        ctx->regs[i] = cpu->gpr[i];
    }
    ctx->regs[15] = t ? (cpu->pc & ~1U) : (cpu->pc & ~3U);
    ctx->cpsr = (cpu->cpsr_or_cr & ~A32_CPSR_T) | (t ? A32_CPSR_T : 0U);
    ctx->itstate = (((ctx->cpsr >> 8) & 0xFCU) | ((ctx->cpsr >> 25) & 0x03U));
    if ((ctx->itstate & 0x0FU) == 0U || (ctx->itstate & 0xF0U) == 0xF0U) {
        ctx->itstate = 0U;
    }
    ctx->tls_tp = cpu->tls_tp;
    ctx->fpscr = cpu->fpscr;
    if ((cpu->state_flags & A32H_DSTATE_FP_DIRTY) != 0U) {
        for (unsigned i = 0U; i < 32U; i++) {
            ctx->vfp_d[i] = cpu->fpr[i];
        }
    }
}

/* 16-bit Thumb store classifier: 1 = store, 0 = load/other. */
static uint32_t thumb16_is_store(uint16_t hw)
{
    switch (hw & 0xF000U) {
    case 0x5000U: /* register-offset LDR/STR family */
        return (((hw >> 9) & 7U) <= 2U ? 1U : 0U); /* STR/STRH/STRB */
    case 0x6000U: /* STR/LDR imm */
    case 0x7000U: /* STRB/LDRB imm */
    case 0x8000U: /* STRH/LDRH imm */
        return ((hw & 0x0800U) != 0U) ? 0U : 1U;
    case 0x9000U: /* SP-relative STR (0x90) vs LDR (0x98) */
        return ((hw & 0x0800U) != 0U) ? 0U : 1U;
    case 0xC000U: /* STM (0xC0) vs LDM (0xC8) */
        return ((hw & 0x0800U) != 0U) ? 0U : 1U;
    default:
        break;
    }
    if ((hw & 0xFE00U) == 0xB400U) { /* PUSH {.., [lr]} (0xB4xx / 0xB5xx) */
        return 1U;
    }
    return 0U; /* POP, ADR, CBZ/CBNZ, arithmetic: no store */
}

/* 32-bit Thumb-2 store classifier on the full word (L == bit 20 for integer
 * and VFP, L == bit 21 for Advanced SIMD VSTn/VLDn at 0xF9). */
static uint32_t thumb32_is_store(uint32_t w)
{
    uint32_t top = w >> 24U;
    if (top == 0xE8U || top == 0xE9U) { /* LDM/STM + LDREX/STREX */
        return ((w & 0x00100000U) != 0U) ? 0U : 1U;
    }
    if (top == 0xF8U) { /* LDR/STR imm12/imm8/reg */
        return ((w & 0x00100000U) != 0U) ? 0U : 1U;
    }
    if (top == 0xF9U) { /* LDRSB/LDRSH/PLI (L=bit 20) vs SIMD VSTn/VLDn (L=bit 21) */
        if ((w & 0x00100000U) != 0U) {
            return 0U;
        }
        return ((w & 0x00200000U) != 0U) ? 0U : 1U;
    }
    if (top == 0xECU || top == 0xEDU) { /* VSTM/VLDM/VSTR/VLDR */
        return ((w & 0x00100000U) != 0U) ? 0U : 1U;
    }
    return 0U;
}

/* ARM32 store classifier: 1 = store, 0 = load/other. */
static uint32_t arm_is_store(uint32_t w)
{
    uint32_t top = w >> 24U;
    if (top == 0xF4U && (w & 0x00100000U) == 0U) { /* Advanced SIMD VSTn vs VLDn (L == bit 21) */
        return ((w & 0x00200000U) != 0U) ? 0U : 1U;
    }
    if (top == 0xECU || top == 0xEDU) { /* VSTM/VLDM/VSTR/VLDR, LDC/STC */
        return ((w & 0x00100000U) != 0U) ? 0U : 1U;
    }
    if ((top >> 4U) == 0xFU) { /* Other unconditional space (PLD/PLI/etc.): no store */
        return 0U;
    }
    if (((w & 0x0FB000F0U) == 0x01000090U) || /* SWP/SWPB */
        ((w & 0x0F9000F0U) == 0x01800090U)) { /* STREX/STREXB/STREXH/STREXD */
        return 1U;
    }
    if (((w & 0x0E000090U) == 0x00000090U) && /* Extra load/store: STRH (op2=01) & STRD (op2=11) */
        ((w & 0x00100020U) == 0x00000020U)) { /* L == 0 && H (bit 5) == 1 (excludes LDRD op2=10) */
        return 1U;
    }
    if (((w & 0x0C000000U) == 0x04000000U) || /* STR/LDR single */
        ((w & 0x0C000000U) == 0x08000000U)) { /* STM/LDM */
        return ((w & 0x00100000U) != 0U) ? 0U : 1U;
    }
    return 0U; /* LDREX{B,H,D}, LDRD, MUL, arithmetic: no store */
}

/* DEXIT_FAULT classifier: EXEC when VA is the PC, else the host ESR WnR
 * when valid, else the complete ARM + Thumb-1/Thumb-2 store classifier. */
static uint32_t fault_kind_for(const a32_ctx_t *ctx)
{
    uint32_t pc = ctx->regs[15];
    if (ctx->fault_addr == (pc & ~1U)) {
        return A32H_DFAULT_EXEC;
    }
    if (ctx->fault_host_valid) {
        return (ctx->fault_is_write != 0U) ? A32H_DFAULT_WRITE
                                           : A32H_DFAULT_READ;
    }
    if ((ctx->cpsr & A32_CPSR_T) != 0U) {
        uint32_t a = pc & ~1U;
        uint16_t hw = 0U, hw1 = 0U;
        uint32_t w;
        if (!a32_guest_range_ok((a32_ctx_t *)ctx, a, 2U,
                (uint32_t)PROT_READ)) {
            return A32H_DFAULT_READ;
        }
        memcpy(&hw, a32_guest_ptr(ctx, a), 2U);
        if (hw < 0xE800U) {
            return thumb16_is_store(hw) ? A32H_DFAULT_WRITE
                                        : A32H_DFAULT_READ;
        }
        if (!a32_guest_range_ok((a32_ctx_t *)ctx, a, 4U,
                (uint32_t)PROT_READ)) {
            return A32H_DFAULT_READ;
        }
        memcpy(&hw1, a32_guest_ptr(ctx, a + 2U), 2U);
        /* Disassembly order (first halfword high, e.g. 0xE8800002) so
         * `top` sees the opcode halfword, not the operand halfword. */
        w = ((uint32_t)hw << 16) | (uint32_t)hw1;
        return thumb32_is_store(w) ? A32H_DFAULT_WRITE : A32H_DFAULT_READ;
    }
    {
        uint32_t a = pc & ~3U;
        uint32_t w = 0U;
        if (!a32_guest_range_ok((a32_ctx_t *)ctx, a, 4U,
                (uint32_t)PROT_READ)) {
            return A32H_DFAULT_READ;
        }
        memcpy(&w, a32_guest_ptr(ctx, a), 4U);
        return arm_is_store(w) ? A32H_DFAULT_WRITE : A32H_DFAULT_READ;
    }
}

uint32_t a32_darwin_fault_kind_for(const a32_ctx_t *ctx)
{
    if (ctx == NULL) {
        return A32H_DFAULT_READ;
    }
    return fault_kind_for(ctx);
}

/* BKPT #imm at the faulting pc (ARM: insn & 0xffff, Thumb: insn & 0xff;
 * 0xDEFE reports 0xFE). */
uint32_t a32_darwin_bkpt_imm_for(const a32_ctx_t *ctx)
{
    uint32_t pc;
    if (ctx == NULL) {
        return 0U;
    }
    pc = ctx->regs[15];
    if ((ctx->cpsr & A32_CPSR_T) != 0U) {
        uint32_t a = pc & ~1U;
        uint16_t hw = 0U;
        if (!a32_guest_range_ok((a32_ctx_t *)ctx, a, 2U,
                (uint32_t)PROT_READ)) {
            return 0U;
        }
        memcpy(&hw, a32_guest_ptr(ctx, a), 2U);
        return (uint32_t)hw & 0xFFU;
    }
    {
        uint32_t a = pc & ~3U;
        uint32_t w = 0U;
        if (!a32_guest_range_ok((a32_ctx_t *)ctx, a, 4U,
                (uint32_t)PROT_READ)) {
            return 0U;
        }
        memcpy(&w, a32_guest_ptr(ctx, a), 4U);
        return w & 0xFFFFU;
    }
}
