/* rt_ringbuffer.h — lock-free single-producer / single-consumer byte ring.
 *
 * A drop-in for the subset of <jack/ringbuffer.h> JackDAW uses: same calls,
 * same arguments, same capacity rule (the size is rounded up to a power of
 * two and one byte of it is never filled), same "reset is not thread safe".
 * One deliberate difference: read_advance is clamped to what is readable, so
 * skipping a record can never move the reader past the writer.
 *
 * WHY NOT jack_ringbuffer_*. Those live in whatever libjack the user runs, and
 * two of the libjacks in the field do not order the data copy against the
 * index update: jack2 before 1.9.22 (Debian 12 / Raspberry Pi OS bookworm ship
 * 1.9.21) and PipeWire's pipewire-jack (plain volatile indices, no barrier).
 * On x86 that is harmless, because stores become visible in program order. On
 * a weakly ordered CPU (aarch64) the consumer can see the advanced write index
 * before the bytes behind it land, and read stale audio, MIDI or atom data.
 * Owning the ring makes the ordering a property of JackDAW, not of the host's
 * libjack.
 *
 * Ordering: each index is written only by its own side, with a release store
 * after the copy; the other side reads it with an acquire load before touching
 * the bytes. That covers both hazards — reading bytes not yet written, and
 * overwriting bytes not yet read.
 *
 * RT-safe: read, peek, read_advance, read_space, write and write_space do no
 * allocation, locking or system calls. create, free and mlock are not RT. */
#ifndef JACKDAW_RT_RINGBUFFER_H
#define JACKDAW_RT_RINGBUFFER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rt_ringbuffer rt_ringbuffer_t;

rt_ringbuffer_t *rt_ringbuffer_create(size_t sz);
void   rt_ringbuffer_free(rt_ringbuffer_t *rb);
int    rt_ringbuffer_mlock(rt_ringbuffer_t *rb);
void   rt_ringbuffer_reset(rt_ringbuffer_t *rb);

size_t rt_ringbuffer_read_space(const rt_ringbuffer_t *rb);
size_t rt_ringbuffer_write_space(const rt_ringbuffer_t *rb);

size_t rt_ringbuffer_read(rt_ringbuffer_t *rb, char *dest, size_t cnt);
size_t rt_ringbuffer_peek(rt_ringbuffer_t *rb, char *dest, size_t cnt);
void   rt_ringbuffer_read_advance(rt_ringbuffer_t *rb, size_t cnt);
size_t rt_ringbuffer_write(rt_ringbuffer_t *rb, const char *src, size_t cnt);

#ifdef __cplusplus
}
#endif

#endif /* JACKDAW_RT_RINGBUFFER_H */
