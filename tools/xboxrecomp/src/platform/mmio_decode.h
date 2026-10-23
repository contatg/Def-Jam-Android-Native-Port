/*
 * mmio_decode.h -- one x86-64 instruction decoder for trapped MMIO.
 *
 * A device whose registers need semantics cannot be plain memory: the reads
 * have to be answered and the writes have to be seen. The way that works here
 * is to leave the page PAGE_NOACCESS, catch the access in a vectored handler,
 * decode the faulting instruction, service it against the device model, and
 * step over it.
 *
 * That decoder existed twice before this file -- once in nv2a_mmio_hook.c and
 * once in apu_mmio_hook.c -- as the same opcode table written out against two
 * different pairs of accessors. This is the same logic with the accessors
 * passed in, so a third device does not need a third copy. The two originals
 * still carry their own and can move onto this whenever they are next touched.
 *
 * Header-only and static inline: one function, one caller per device, and a
 * library for it would be more build wiring than code.
 *
 * The instructions covered are what the XDK device code actually emits against
 * registers -- moves both ways, the immediate forms, the zero-extending loads,
 * and the read-modify-write and flag-setting forms a poll loop is built from.
 * Anything outside that set returns 0 rather than guessing: stepping over an
 * instruction that was not understood corrupts the guest silently, which is
 * far worse than a fault naming the opcode.
 */
#ifndef MMIO_DECODE_H
#define MMIO_DECODE_H

#include <stdint.h>

/* The decoder below reads and writes the register file through a context with
 * Win32's field names. On Windows that is the vectored handler's CONTEXT.
 * Off Windows, the trap layer on an x86-64 host defines MMIO_X64_CONTEXT as a
 * structure with the same names, filled from the signal's ucontext, and
 * includes this file for the decoder alone. */
#if defined(_WIN32)
#include <windows.h>
typedef PCONTEXT mmio_ctx_t;
#define MMIO_HAVE_X64_DECODER 1
#elif defined(MMIO_X64_CONTEXT)
typedef MMIO_X64_CONTEXT *mmio_ctx_t;
#define MMIO_HAVE_X64_DECODER 1
#endif

#if defined(MMIO_HAVE_X64_DECODER)

/* Service one register access. dev is passed straight back to the callbacks. */
typedef uint64_t (*mmio_read_fn)(void *dev, uint32_t off, int size);
typedef void     (*mmio_write_fn)(void *dev, uint32_t off, uint64_t val, int size);

static inline uint64_t *mmio_ctx_reg(mmio_ctx_t c, int reg)
{
    switch (reg & 0xF) {
    case 0:  return (uint64_t *)&c->Rax;   case 1:  return (uint64_t *)&c->Rcx;
    case 2:  return (uint64_t *)&c->Rdx;   case 3:  return (uint64_t *)&c->Rbx;
    case 4:  return (uint64_t *)&c->Rsp;   case 5:  return (uint64_t *)&c->Rbp;
    case 6:  return (uint64_t *)&c->Rsi;   case 7:  return (uint64_t *)&c->Rdi;
    case 8:  return (uint64_t *)&c->R8;    case 9:  return (uint64_t *)&c->R9;
    case 10: return (uint64_t *)&c->R10;   case 11: return (uint64_t *)&c->R11;
    case 12: return (uint64_t *)&c->R12;   case 13: return (uint64_t *)&c->R13;
    case 14: return (uint64_t *)&c->R14;   case 15: return (uint64_t *)&c->R15;
    default: return NULL;
    }
}

static inline int mmio_modrm_len(const uint8_t *ip, int rex_b)
{
    uint8_t modrm = *ip;
    int mod = (modrm >> 6) & 3;
    int rm  = (modrm & 7) | (rex_b ? 8 : 0);
    int len = 1;

    if (mod == 3) return 1;
    if ((rm & 7) == 4) len += 1;                    /* SIB    */
    if (mod == 0 && (rm & 7) == 5) len += 4;        /* disp32 */
    else if (mod == 1) len += 1;
    else if (mod == 2) len += 4;
    return len;
}

/* Flags after a compare-shaped operation, so a poll loop branches correctly.
 * ZF, SF and CF only: those are what jz/jnz, js and jb/jae read, and inventing
 * an overflow flag nothing here sets is worse than leaving it alone. */
static inline void mmio_set_flags(mmio_ctx_t ctx, uint64_t result, int size,
                                  int carry)
{
    ctx->EFlags &= ~(0x0001u | 0x0040u | 0x0080u | 0x0800u);
    if (size < 8)
        result &= (1ULL << (size * 8)) - 1;
    if (result == 0)
        ctx->EFlags |= 0x0040u;                                   /* ZF */
    if (result & (1ULL << (size * 8 - 1)))
        ctx->EFlags |= 0x0080u;                                   /* SF */
    if (carry)
        ctx->EFlags |= 0x0001u;                                   /* CF */
}

/* The arithmetic forms below also need OF: a signed compare (jl/jg) reads it,
 * and a register ALU result can feed one directly. */
static inline void mmio_set_arith_flags(mmio_ctx_t ctx, uint64_t a, uint64_t b,
                                        uint64_t res, int size, int sub)
{
    uint64_t mask = (size < 8) ? ((1ULL << (size * 8)) - 1) : ~0ULL;
    uint64_t sign = 1ULL << (size * 8 - 1);
    int carry, ovf;

    a &= mask; b &= mask; res &= mask;
    carry = sub ? (a < b) : (res < a);
    ovf   = sub ? (((a ^ b) & (a ^ res) & sign) != 0)
                : ((~(a ^ b) & (a ^ res) & sign) != 0);
    mmio_set_flags(ctx, res, size, carry);
    if (ovf)
        ctx->EFlags |= 0x0800u;                                   /* OF */
}

/* Write a register the way the CPU would for an operand of this size: a
 * 32-bit result clears the upper half, 8 and 16 bits merge. */
static inline void mmio_put_reg(mmio_ctx_t ctx, int reg, uint64_t v, int size)
{
    uint64_t *dst = mmio_ctx_reg(ctx, reg);
    if (size == 1)      *dst = (*dst & ~0xFFULL)   | (v & 0xFF);
    else if (size == 2) *dst = (*dst & ~0xFFFFULL) | (v & 0xFFFF);
    else if (size == 4) *dst = v & 0xFFFFFFFFULL;
    else                *dst = v;
}

/* One ALU operation by its group-1 index (the /n of 80/81/83; also bits 5:3 of
 * the two-operand opcodes). Sets the flags; returns 0 for ADC/SBB, which need
 * the incoming carry and are not something device code does to a register. */
static inline int mmio_alu(mmio_ctx_t ctx, int n, uint64_t a, uint64_t b,
                           int size, uint64_t *res)
{
    switch (n) {
    case 0: *res = a + b; mmio_set_arith_flags(ctx, a, b, *res, size, 0); return 1;
    case 1: *res = a | b; mmio_set_flags(ctx, *res, size, 0);           return 1;
    case 4: *res = a & b; mmio_set_flags(ctx, *res, size, 0);           return 1;
    case 5:
    case 7: *res = a - b; mmio_set_arith_flags(ctx, a, b, *res, size, 1); return 1;
    case 6: *res = a ^ b; mmio_set_flags(ctx, *res, size, 0);           return 1;
    default: return 0;
    }
}

/* 1 if the instruction at ctx->Rip was serviced and Rip advanced past it. */
static inline int mmio_emulate(mmio_ctx_t ctx, uint32_t off, void *dev,
                               mmio_read_fn rd, mmio_write_fn wr)
{
    const uint8_t *ip = (const uint8_t *)ctx->Rip;
    int prefix = 0, has66 = 0, rex = 0, has_rex = 0;
    const uint8_t *op;
    int size, rex_w, rex_r, rex_b, mlen, reg;

    if (!rd || !wr)
        return 0;

    for (;;) {
        uint8_t b = ip[prefix];
        if (b == 0x66)                   { has66 = 1; prefix++; }
        else if (b == 0xF2 || b == 0xF3) { prefix++; }
        else if (b >= 0x40 && b <= 0x4F) { rex = b; has_rex = 1; prefix++; }
        else break;
    }
    rex_w = has_rex && (rex & 0x08);
    rex_r = has_rex && (rex & 0x04);
    rex_b = has_rex && (rex & 0x01);

    op   = ip + prefix;
    size = has66 ? 2 : (rex_w ? 8 : 4);

    switch (op[0]) {
    case 0x88: case 0x89:                            /* MOV r/m, r   (write) */
        if (op[0] == 0x88) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        wr(dev, off, *mmio_ctx_reg(ctx, reg), size);
        ctx->Rip += prefix + 1 + mlen;
        return 1;

    case 0xC7:                                       /* MOV r/m, imm32       */
        mlen = mmio_modrm_len(op + 1, rex_b);
        wr(dev, off, *(const uint32_t *)(op + 1 + mlen), size);
        ctx->Rip += prefix + 1 + mlen + 4;
        return 1;

    case 0xC6:                                       /* MOV r/m8, imm8       */
        mlen = mmio_modrm_len(op + 1, rex_b);
        wr(dev, off, op[1 + mlen], 1);
        ctx->Rip += prefix + 1 + mlen + 1;
        return 1;

    case 0x8A: case 0x8B: {                          /* MOV r, r/m   (read)  */
        uint64_t v, *dst;
        if (op[0] == 0x8A) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        v    = rd(dev, off, size);
        dst  = mmio_ctx_reg(ctx, reg);
        if (size == 1)      *dst = (*dst & ~0xFFULL)   | (v & 0xFF);
        else if (size == 2) *dst = (*dst & ~0xFFFFULL) | (v & 0xFFFF);
        else if (size == 4) *dst = v & 0xFFFFFFFFULL;  /* 32-bit clears high */
        else                *dst = v;
        ctx->Rip += prefix + 1 + mlen;
        return 1;
    }

    case 0x84: case 0x85: {                          /* TEST r/m, r          */
        uint64_t m, r;
        if (op[0] == 0x84) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        m    = rd(dev, off, size);
        r    = *mmio_ctx_reg(ctx, reg);
        mmio_set_flags(ctx, m & r, size, 0);
        ctx->Rip += prefix + 1 + mlen;
        return 1;
    }

    case 0x38: case 0x39: {                          /* CMP r/m, r           */
        uint64_t m, r, mask;
        if (op[0] == 0x38) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        m    = rd(dev, off, size);
        r    = *mmio_ctx_reg(ctx, reg);
        mask = (size < 8) ? ((1ULL << (size * 8)) - 1) : ~0ULL;
        m &= mask; r &= mask;
        mmio_set_flags(ctx, m - r, size, m < r);
        ctx->Rip += prefix + 1 + mlen;
        return 1;
    }

    case 0x08: case 0x09:                            /* OR  r/m, r           */
        if (op[0] == 0x08) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        wr(dev, off, rd(dev, off, size) | *mmio_ctx_reg(ctx, reg), size);
        ctx->Rip += prefix + 1 + mlen;
        return 1;

    case 0x20: case 0x21:                            /* AND r/m, r           */
        if (op[0] == 0x20) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        wr(dev, off, rd(dev, off, size) & *mmio_ctx_reg(ctx, reg), size);
        ctx->Rip += prefix + 1 + mlen;
        return 1;

    /* ALU r, r/m: the register is the destination and the register read is
     * the source -- the shape a compiler makes of `x & *reg` or `x - *reg`
     * when it folds the load into the operation. GCC does; MSVC tended not
     * to, which is why these first turned up on Linux. Only the 16/32/64-bit
     * forms: an 8-bit register operand without REX can name AH..BH. */
    case 0x03: case 0x0B: case 0x23: case 0x2B: case 0x33: case 0x3B: {
        uint64_t m, r, res;
        int n = (op[0] >> 3) & 7;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        m    = rd(dev, off, size);
        r    = *mmio_ctx_reg(ctx, reg);
        if (!mmio_alu(ctx, n, r, m, size, &res))
            return 0;
        if (n != 7)                                  /* CMP keeps the register */
            mmio_put_reg(ctx, reg, res, size);
        ctx->Rip += prefix + 1 + mlen;
        return 1;
    }

    /* Group 1, r/m op imm: the register is the destination. CMP only reads. */
    case 0x80: case 0x81: case 0x83: {
        uint64_t m, imm, res;
        int n = (op[1] >> 3) & 7, ilen;
        if (op[0] == 0x80) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        if (op[0] == 0x81 && size == 2) {
            imm = *(const uint16_t *)(op + 1 + mlen); ilen = 2;
        } else if (op[0] == 0x81) {
            imm = (uint64_t)(int64_t)*(const int32_t *)(op + 1 + mlen); ilen = 4;
        } else {
            imm = (uint64_t)(int64_t)(int8_t)op[1 + mlen]; ilen = 1;
        }
        m = rd(dev, off, size);
        if (!mmio_alu(ctx, n, m, imm, size, &res))
            return 0;
        if (n != 7)
            wr(dev, off, res, size);
        ctx->Rip += prefix + 1 + mlen + ilen;
        return 1;
    }

    case 0xF6: case 0xF7: {                          /* TEST r/m, imm        */
        uint64_t imm;
        int ilen;
        if (((op[1] >> 3) & 7) != 0)                 /* NOT/NEG/MUL/DIV      */
            return 0;
        if (op[0] == 0xF6) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        if (size == 1)      { imm = op[1 + mlen]; ilen = 1; }
        else if (size == 2) { imm = *(const uint16_t *)(op + 1 + mlen); ilen = 2; }
        else { imm = (uint64_t)(int64_t)*(const int32_t *)(op + 1 + mlen); ilen = 4; }
        mmio_set_flags(ctx, rd(dev, off, size) & imm, size, 0);
        ctx->Rip += prefix + 1 + mlen + ilen;
        return 1;
    }

    case 0x0F:
        if (op[1] == 0xB6 || op[1] == 0xB7) {        /* MOVZX r32, r/m8|16   */
            int s = (op[1] == 0xB6) ? 1 : 2;
            mlen  = mmio_modrm_len(op + 2, rex_b);
            reg   = ((op[2] >> 3) & 7) | (rex_r ? 8 : 0);
            *mmio_ctx_reg(ctx, reg) = rd(dev, off, s) & ((1ULL << (s * 8)) - 1);
            ctx->Rip += prefix + 2 + mlen;
            return 1;
        }
        return 0;

    default:
        return 0;
    }
}

#else /* no x86-64 decoder wanted here */

/* Off Windows the context is the signal handler's ucontext and the decoding
 * belongs to the trap layer, which knows the host's instruction set. */
#include "mmio_trap.h"

typedef mmio_trap_read_fn  mmio_read_fn;
typedef mmio_trap_write_fn mmio_write_fn;

static inline int mmio_emulate(void *ctx, uint32_t off, void *dev,
                               mmio_read_fn rd, mmio_write_fn wr)
{
    return mmio_trap_emulate(ctx, off, dev, rd, wr);
}

#endif /* MMIO_HAVE_X64_DECODER */
#endif /* MMIO_DECODE_H */
