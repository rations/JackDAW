/* JackDAW — hand-written config.h
 * Target: Devuan Excalibur / Debian Trixie amd64
 *
 *   apt install libgtk-3-dev libjack-jackd2-dev libsndfile1-dev \
 *               libsamplerate0-dev libasound2-dev liblilv-dev libsuil-dev
 */
#ifndef CONFIG_H
#define CONFIG_H

#define PACKAGE         "jackdaw"

/* VERSION and PACKAGE_VERSION come from the VERSION file at the repo root,
 * passed in by the Makefile, so the x86_64 and aarch64 builds and the release
 * tarball name all read one number. Bump it there, not here. */
#ifndef VERSION
#  error "VERSION not defined: build with the Makefile (it reads ./VERSION)"
#endif

#define HAVE_SCHED_H     1
#define HAVE_SCHED_YIELD 1

#endif /* CONFIG_H */
