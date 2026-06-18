/* rt_denormal.h — flush-to-zero / denormals-are-zero for the RT thread.
 *
 * JACK does not set FTZ/DAZ on a client's process thread, so plugins whose
 * filter/reverb/feedback paths produce subnormal floats stall the CPU 10-100x
 * and blow the RT deadline. Worse, a plugin may clear the MXCSR control word
 * inside its own process() call, leaving the REST of the chain exposed — so we
 * re-arm before every plugin, not just once per cycle (matching Reaper/Ardour).
 *
 * Cheap: two MXCSR register writes. Safe to include from C and C++. */
#ifndef JACKDAW_RT_DENORMAL_H
#define JACKDAW_RT_DENORMAL_H

#if defined(__SSE__) || defined(__x86_64__)
#  include <xmmintrin.h>
#  include <pmmintrin.h>
#  define JACKDAW_HAVE_SSE_DENORMAL 1
#endif

static inline void rt_set_denormal_mode(void)
{
#ifdef JACKDAW_HAVE_SSE_DENORMAL
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
}

#endif /* JACKDAW_RT_DENORMAL_H */
