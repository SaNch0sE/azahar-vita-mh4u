// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <cmath>
#include <cstring>
#include <limits>
#include "common/common_types.h"

namespace Core::VitaHle {

/**
 * Monster Hunter 4 Ultimate's one short-vector routine (USA v1.1 and the Heavenfall build of it),
 * natively. The ARM11 runs it with FPSCR.LEN=3 (vectors of four); the Cortex-A9 has no short
 * vectors, so it used to run in the DynCom interpreter, ~0.9 ms a call, ~150 calls a second with
 * the camera on the busy side of the ship's deck: an audio filter, two loops over a block of
 * samples. Its vector part (0x110C2C-0x110DA4) is:
 *
 *   loop 1 (count r11 = r2, 8 a pass), entered at its first vector instruction 0x110C64, where
 *   the trap comes, its loads already done:
 *     s8..s15 += s16..s23 * s0;  [r5 + r7*4] = s8..s15;  r7 = (r7 + 8) & r8
 *     s16..s23 -= s8..s15 * s0;  s24..s31 = s16..s23 * s1;  [r9]! = s24..s31
 *     next pass: s8..s15 = [r9];  s16..s23 = [r5 + r6*4];  r6 = (r6 + 8) & r8
 *   [r0+0x6c] = r6; [r0+0x70] = r7; r9 = [r1]; r10 = [r1+4]; r5..r8 = [r0+0x78]; s0..s3 = [r0+0xa4]
 *   loop 2 (count r2, 8 a pass), twice per pass, the second time with the taps wrapped by r3:
 *     s8..s11 = [r9];  s8..s11 += sum over k of [r4 + rk*4]..+4 * s_k (rk = r5..r8, each += 4)
 *     s8..s11 = to int32 toward zero (scalar);  [r9]! = [r10]! = s8..s11
 *   [r0+0x78] = r5..r8; r12 = pop; FPSCR = r12; continue at 0x110DA8.
 *
 * Arithmetic exactly as the interpreter's host-float path (vfpsingle.cpp CpdoFast): inputs and
 * results flushed when FPSCR.FZ, default NaN when FPSCR.DN, a multiply-accumulate rounding twice.
 */
constexpr u32 Mh4uFilterEntry = 0x00110C64;
constexpr u32 Mh4uFilterExit = 0x00110DA8;

struct HleState {
    u32* r;     ///< r0-r15
    u32* s;     ///< s0-s31, as bits
    u32* cpsr;
    u32* fpscr;
};

/// `Mem` provides u32 Read(u32 vaddr), void Write(u32 vaddr, u32 value) and u8* Span(u32 vaddr,
/// u32 size): a host pointer to a run inside one page of plain memory, or nullptr.
template <typename Mem>
void RunMh4uFilter(HleState st, Mem& mem) {
    u32* const r = st.r;
    u32* const s = st.s;
    const u32 fpscr = *st.fpscr;
    const bool fz = (fpscr & (1u << 24)) != 0;
    const bool dn = (fpscr & (1u << 25)) != 0;
    const auto flush = [&](u32 bits) {
        return fz && (bits & 0x7f800000u) == 0 ? (bits & 0x80000000u) : bits;
    };
    const auto f = [&](u32 bits) {
        float v;
        const u32 b = flush(bits);
        std::memcpy(&v, &b, sizeof(v));
        return v;
    };
    const auto out = [&](float v) {
        u32 b;
        std::memcpy(&b, &v, sizeof(b));
        b = flush(b);
        if (dn && (b & 0x7f800000u) == 0x7f800000u && (b & 0x007fffffu) != 0) {
            b = 0x7fc00000u;
        }
        return b;
    };
    // Vector ops of length 4 (no register wraps past its bank in this routine); m a scalar.
    const auto fmac = [&](u32 d, u32 n, u32 m) {
        for (u32 i = 0; i < 4; i++) {
            const float p = f(s[n + i]) * f(s[m]);
            s[d + i] = out(f(s[d + i]) + p);
        }
    };
    const auto fnmac = [&](u32 d, u32 n, u32 m) {
        for (u32 i = 0; i < 4; i++) {
            const float p = f(s[n + i]) * f(s[m]);
            s[d + i] = out(f(s[d + i]) - p);
        }
    };
    const auto fmul = [&](u32 d, u32 n, u32 m) {
        for (u32 i = 0; i < 4; i++) {
            s[d + i] = out(f(s[n + i]) * f(s[m]));
        }
    };
    // FTOSIZS: toward zero, saturating, NaN to zero.
    const auto ftosiz = [&](u32 reg) {
        const float v = f(s[reg]);
        s32 i;
        if (std::isnan(v)) {
            i = 0;
        } else if (v >= 2147483648.0f) {
            i = std::numeric_limits<s32>::max();
        } else if (v < -2147483648.0f) {
            i = std::numeric_limits<s32>::min();
        } else {
            i = static_cast<s32>(v);
        }
        s[reg] = static_cast<u32>(i);
    };
    // Runs of 4 or 8 words: one host pointer when the run stays in a page (Mem::Span).
    const auto load = [&](u32 first, u32 count, u32 addr) {
        if (const u8* p = mem.Span(addr, count * 4)) {
            std::memcpy(&s[first], p, count * 4);
            return;
        }
        for (u32 i = 0; i < count; i++) {
            s[first + i] = mem.Read(addr + 4 * i);
        }
    };
    const auto store = [&](u32 first, u32 count, u32 addr) {
        if (u8* p = mem.Span(addr, count * 4)) {
            std::memcpy(p, &s[first], count * 4);
            return;
        }
        for (u32 i = 0; i < count; i++) {
            mem.Write(addr + 4 * i, s[first + i]);
        }
    };
    // subs rd, rd, #8: NZCV.
    const auto subs8 = [&](u32& reg) {
        const u32 a = reg;
        const u32 res = a - 8;
        reg = res;
        u32 flags = 0;
        flags |= (res & 0x80000000u);                       // N
        flags |= (res == 0) ? (1u << 30) : 0;               // Z
        flags |= (a >= 8) ? (1u << 29) : 0;                 // C: no borrow
        flags |= (((a ^ 8) & (a ^ res)) & 0x80000000u) >> 3; // V
        *st.cpsr = (*st.cpsr & 0x0fffffffu) | flags;
        return res != 0;
    };

    // Loop 1, from 0x110C64.
    for (;;) {
        fmac(8, 16, 0);
        fmac(12, 20, 0);
        r[10] = r[5] + (r[7] << 2);
        store(8, 8, r[10]);
        r[7] = (r[7] + 8) & r[8];
        fnmac(16, 8, 0);
        fnmac(20, 12, 0);
        fmul(24, 16, 1);
        fmul(28, 20, 1);
        store(24, 8, r[9]);
        r[9] += 32;
        if (!subs8(r[11])) {
            break;
        }
        load(8, 8, r[9]);
        r[10] = r[5] + (r[6] << 2);
        load(16, 8, r[10]);
        r[6] = (r[6] + 8) & r[8];
    }
    // 0x110C98
    mem.Write(r[0] + 0x6c, r[6]);
    mem.Write(r[0] + 0x70, r[7]);
    r[9] = mem.Read(r[1]);
    r[10] = mem.Read(r[1] + 4);
    r[12] = r[0] + 0x78;
    for (u32 i = 0; i < 4; i++) {
        r[5 + i] = mem.Read(r[12] + 4 * i);
    }
    r[12] = r[0] + 0xa4;
    load(0, 4, r[12]);
    // Loop 2, 0x110CB8.
    for (;;) {
        for (u32 half = 0; half < 2; half++) {
            const u32 acc = half == 0 ? 8 : 20;
            load(acc, 4, r[9]);
            for (u32 k = 0; k < 4; k++) {
                r[11] = r[4] + (r[5 + k] << 2);
                r[5 + k] += 4;
                if (half == 1) {
                    r[5 + k] &= r[3];
                }
                const u32 tap = (k & 1) == 0 ? 12 : 16;
                load(tap, 4, r[11]);
                fmac(acc, tap, k);
            }
            for (u32 i = 0; i < 4; i++) {
                ftosiz(acc + i);
            }
            store(acc, 4, r[9]);
            r[9] += 16;
            store(acc, 4, r[10]);
            r[10] += 16;
        }
        if (!subs8(r[2])) {
            break;
        }
    }
    // 0x110D98
    r[12] = r[0] + 0x78;
    for (u32 i = 0; i < 4; i++) {
        mem.Write(r[12] + 4 * i, r[5 + i]);
    }
    r[12] = mem.Read(r[13]);
    r[13] += 4;
    *st.fpscr = r[12];
    r[15] = Mh4uFilterExit;
}

} // namespace Core::VitaHle
