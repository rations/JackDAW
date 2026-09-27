/* rt_denormal.h — flush-to-zero / denormals-are-zero for the RT thread.
 *
 * JACK does not set FTZ/DAZ on a client's process thread, so plugins whose
 * filter/reverb/feedback paths produce subnormal floats stall the CPU 10-100x
 * and blow the RT deadline. Worse, a plugin may clear the FP control register
 * inside its own process() call, leaving the REST of the chain exposed — so we
 * re-arm before every plugin, not just once per cycle (matching Reaper/Ardour).
 *
 * x86-64 needs two MXCSR bits: FTZ flushes subnormal results, DAZ subnormal
 * inputs. AArch64 needs one: FPCR bit 24 (FZ) does both, for scalar and
 * Advanced SIMD alike, and there is no separate DAZ control. The bit position
 * is the FPCR layout in glibc's aarch64 <fpu_control.h>. The register is
 * accessed with mrs/msr directly rather than through that header so the code
 * does not depend on glibc.
 *
 * Before the aarch64 branch existed this compiled to an empty function on ARM,
 * silently leaving every RT thread unprotected there.
 *
 * Cheap: two register writes on x86; on aarch64 one read, and a write only when
 * FZ is found clear. Safe to include from C and C++. */
#ifndef JACKDAW_RT_DENORMAL_H
#define JACKDAW_RT_DENORMAL_H

#if defined(__SSE__) || defined(__x86_64__)
#  include <xmmintrin.h>
#  include <pmmintrin.h>
#  define JACKDAW_HAVE_SSE_DENORMAL 1
#elif defined(__aarch64__)
#  define JACKDAW_HAVE_AARCH64_DENORMAL 1
#endif

static inline void rt_set_denormal_mode(void)
{
#if defined(JACKDAW_HAVE_SSE_DENORMAL)
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#elif defined(JACKDAW_HAVE_AARCH64_DENORMAL)
    /* Read-modify-write: every other FPCR bit (the rounding mode above all)
     * belongs to someone else, and a blind write would reset it. The write is
     * skipped when FZ is already set — the common case, since this runs before
     * every plugin and an FPCR write can serialise the pipeline on some cores.
     * unsigned long is 64-bit on aarch64 Linux, as mrs/msr need an X register. */
    unsigned long fpcr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    if (!(fpcr & (1UL << 24)))
        __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr | (1UL << 24)));
#endif
}

#endif /* JACKDAW_RT_DENORMAL_H */
