#pragma once

/// FpGuard: a clean floating-point state for our code while it runs inside game code (FpGuard.h, header only).

#include <cstdint>

namespace SwbfVr
{

/// Saves the game's x87/SSE state, gives our code a clean one, and restores the game's on scope exit.
/// Use it wherever our code runs in the middle of game code (thunks) or does float work inside a hook.
struct FpGuard
{
    struct alignas(16) Area
    {
        uint8_t bytes[512];
    } area;

    FpGuard()
    {
        __asm__ volatile("fxsave %0" : "=m"(area));
        __asm__ volatile("fninit"
                         :
                         :
                         : "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)", "fpsr", "fpcr");
        const uint32_t mxcsr = 0x1F80;
        __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
    }

    // fxrstor overwrites every x87 and SSE register; the clobber list tells the compiler not to keep values in them.
    ~FpGuard()
    {
        __asm__ volatile("fxrstor %0"
                         :
                         : "m"(area)
                         : "st",
                           "st(1)",
                           "st(2)",
                           "st(3)",
                           "st(4)",
                           "st(5)",
                           "st(6)",
                           "st(7)",
                           "fpsr",
                           "fpcr",
                           "xmm0",
                           "xmm1",
                           "xmm2",
                           "xmm3",
                           "xmm4",
                           "xmm5",
                           "xmm6",
                           "xmm7",
                           "mm0",
                           "mm1",
                           "mm2",
                           "mm3",
                           "mm4",
                           "mm5",
                           "mm6",
                           "mm7");
    }

    FpGuard(const FpGuard&) = delete;
    FpGuard& operator=(const FpGuard&) = delete;
};

} // namespace SwbfVr
