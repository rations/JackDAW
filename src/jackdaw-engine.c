#define _GNU_SOURCE
#include <config.h>

#include <string.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <jack/jack.h>
#include <jack/midiport.h>
#include <jack/ringbuffer.h>
#ifdef HAVE_SAMPLERATE
#  include <samplerate.h>
#endif

#include "jackdaw-engine.h"
#include "settings.h"
#include "um.h"

/* -----------------------------------------------------------------------
 * Internal state
 * ----------------------------------------------------------------------- */

/* Transport control flags — written by main thread, read by RT callback.
 * Use g_atomic_int_* for all accesses. */
#define ENGINE_PLAYING   (1 << 0)
#define ENGINE_RECORDING (1 << 1)

typedef struct {
    jack_client_t *client;
    JackDawProject *project;        /* weak ref — project owns the engine */

    /* Audio input/output ports — indexed [0..audio_in_count-1] etc. */
    jack_port_t **audio_in;
    jack_port_t **audio_out;
    jack_port_t **midi_in;

    guint audio_in_count;
    guint audio_out_count;
    guint midi_in_count;

    /* Pre-allocated mix buffers (sized to max buffer size at init) */
    float *master_L;
    float *master_R;
    float *tmp_L;
    float *tmp_R;
    jack_nframes_t buf_size;    /* current buffer size */

    /* Weak refs to active tracks — slots populated by engine_add_track */
    JackDawTrack *slots[JACKDAW_MAX_TRACKS];

    volatile gint transport_flags; /* ENGINE_PLAYING | ENGINE_RECORDING */
    volatile off_t play_pos;       /* sample counter, incremented by process cb */

    gboolean active;
} JackDawEngine;

static JackDawEngine engine;

/* -----------------------------------------------------------------------
 * Phase 2.5: Playback feeder thread
 *
 * A dedicated pthread fills play_buf_L/R ringbuffers from AudioClip data
 * so the RT callback always has audio to drain.  No malloc/free/mutex
 * in the feeder — only jack_ringbuffer_* (lock-free) and libsndfile reads.
 * ----------------------------------------------------------------------- */

/* Output frames produced per slot per inner-loop pass (controls granularity).
 * Small enough to fit in L1 cache; large enough to reduce syscall overhead. */
#define FEEDER_CHUNK_FRAMES   2048
/* Raw interleaved frames to allocate for one read (covers up to 6:1 downsample,
 * e.g. a 192 kHz clip playing into a 32 kHz JACK session). */
#define FEEDER_RAW_FRAMES     (FEEDER_CHUNK_FRAMES * 6)
/* Maximum clip channels we deinterleave.  Clips with more channels still open
 * correctly via sf_readf_float; we only pick ch 0 and 1. */
#define FEEDER_MAX_CHANNELS   8

/* Per-slot feeder state — only the two atomic fields are shared; everything
 * else is private to the feeder thread. */
typedef struct {
    SNDFILE      *sf;           /* open sequential file handle; NULL = not open */
#ifdef HAVE_SAMPLERATE
    SRC_STATE    *src_L;        /* resampler for channel 0; NULL = no SRC */
    SRC_STATE    *src_R;        /* resampler for channel 1; NULL = mono or no SRC */
#endif
    /* Locate protocol: main thread writes locate_frame then sets locate_req=1.
     * Feeder does CAS(1->0) and applies the seek. */
    volatile gint locate_req;   /* g_atomic_int */
    off_t         locate_frame; /* JACK frame target; valid when locate_req==1 */
    off_t         clip_pos;     /* feeder's current read position in clip frames */
} FeederSlot;

static FeederSlot   feeder_slots[JACKDAW_MAX_TRACKS];
static pthread_t    feeder_tid;
static volatile int feeder_stop_flag;
static gboolean     feeder_started = FALSE;

/* Scratch buffers owned exclusively by the feeder thread */
static float *feeder_raw;    /* FEEDER_RAW_FRAMES * FEEDER_MAX_CHANNELS interleaved */
static float *feeder_mono;   /* FEEDER_RAW_FRAMES — one deinterleaved channel pre-SRC */
static float *feeder_L;      /* FEEDER_CHUNK_FRAMES — output left channel */
static float *feeder_R;      /* FEEDER_CHUNK_FRAMES — output right channel */

/* Close a slot's file handle and SRC states (feeder thread or stop path only). */
static void feeder_slot_close(guint i)
{
    if (feeder_slots[i].sf) {
        sf_close(feeder_slots[i].sf);
        feeder_slots[i].sf = NULL;
    }
#ifdef HAVE_SAMPLERATE
    if (feeder_slots[i].src_L) {
        src_delete(feeder_slots[i].src_L);
        feeder_slots[i].src_L = NULL;
    }
    if (feeder_slots[i].src_R) {
        src_delete(feeder_slots[i].src_R);
        feeder_slots[i].src_R = NULL;
    }
#endif
    feeder_slots[i].clip_pos = 0;
    g_atomic_int_set(&feeder_slots[i].locate_req, 0);
}

static void *feeder_thread_func(void *arg)
{
    (void)arg;
    guint i;
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 2000000 }; /* 2 ms sleep */

    while (!feeder_stop_flag) {
        nanosleep(&ts, NULL);

        if (!(g_atomic_int_get(&engine.transport_flags) & ENGINE_PLAYING))
            continue;
        if (!engine.client)
            continue;

        jack_nframes_t jack_sr = jack_get_sample_rate(engine.client);

        for (i = 0; i < JACKDAW_MAX_TRACKS; i++) {
            JackDawTrack *t = engine.slots[i];

            /* No track in this slot — release any open resources */
            if (!t) {
                if (feeder_slots[i].sf)
                    feeder_slot_close(i);
                continue;
            }

            AudioClip *clip = t->clip;
            if (!clip || !t->play_buf_L || !t->play_buf_R)
                continue;

            int        clip_ch     = clip->info.channels;
            sf_count_t clip_frames = clip->info.frames;
            int        clip_sr     = clip->info.samplerate;
            int        eff_ch      = (clip_ch > FEEDER_MAX_CHANNELS)
                                     ? FEEDER_MAX_CHANNELS : clip_ch;

            /* --- Handle locate request from main thread --- */
            if (g_atomic_int_compare_and_exchange(
                    &feeder_slots[i].locate_req, 1, 0)) {
                off_t jpos = feeder_slots[i].locate_frame;
                off_t cpos;
                if (clip_sr == (int)jack_sr) {
                    cpos = jpos;
                } else {
                    cpos = (off_t)((double)jpos
                                   * (double)clip_sr / (double)jack_sr);
                }
                if (cpos < 0)           cpos = 0;
                if (cpos > clip_frames) cpos = clip_frames;
                feeder_slots[i].clip_pos = cpos;
                if (feeder_slots[i].sf)
                    sf_seek(feeder_slots[i].sf, cpos, SEEK_SET);
#ifdef HAVE_SAMPLERATE
                if (feeder_slots[i].src_L)
                    src_reset(feeder_slots[i].src_L);
                if (feeder_slots[i].src_R)
                    src_reset(feeder_slots[i].src_R);
#endif
            }

            /* --- Open file if not already open --- */
            if (!feeder_slots[i].sf) {
                SF_INFO sfi = {0};
                SNDFILE *sf = sf_open(clip->path, SFM_READ, &sfi);
                if (!sf) continue;
                sf_seek(sf, feeder_slots[i].clip_pos, SEEK_SET);
                feeder_slots[i].sf = sf;
            }

            /* --- Decide whether SRC is needed --- */
            gboolean needs_src = (clip_sr != (int)jack_sr);
#ifndef HAVE_SAMPLERATE
            /* No SRC library compiled in — play at native rate (pitch will shift
             * if clip SR != JACK SR, but will not crash). */
            needs_src = FALSE;
#endif

#ifdef HAVE_SAMPLERATE
            /* Create SRC states on demand */
            if (needs_src && !feeder_slots[i].src_L) {
                int src_err = 0;
                feeder_slots[i].src_L = src_new(SRC_SINC_FASTEST, 1, &src_err);
                if (!feeder_slots[i].src_L) {
                    needs_src = FALSE;
                } else if (clip_ch > 1) {
                    feeder_slots[i].src_R =
                        src_new(SRC_SINC_FASTEST, 1, &src_err);
                    /* src_R may remain NULL for mono clips — handled below */
                }
            }
            /* Drop SRC states if clip SR now matches JACK SR */
            if (!needs_src) {
                if (feeder_slots[i].src_L) {
                    src_delete(feeder_slots[i].src_L);
                    feeder_slots[i].src_L = NULL;
                }
                if (feeder_slots[i].src_R) {
                    src_delete(feeder_slots[i].src_R);
                    feeder_slots[i].src_R = NULL;
                }
            }
#endif /* HAVE_SAMPLERATE */

            /* --- Inner fill loop: top up the ringbuffer each wakeup --- */
            gboolean keep_filling = TRUE;
            while (keep_filling) {
                size_t space_L = jack_ringbuffer_write_space(t->play_buf_L)
                                 / sizeof(float);
                size_t space_R = jack_ringbuffer_write_space(t->play_buf_R)
                                 / sizeof(float);
                size_t out_want = (space_L < space_R) ? space_L : space_R;
                if (out_want > FEEDER_CHUNK_FRAMES)
                    out_want = FEEDER_CHUNK_FRAMES;
                if (out_want == 0)
                    break; /* buffer full */

                off_t cpos = feeder_slots[i].clip_pos;

                /* Clip finished — fill with silence so RT stays running */
                if (cpos >= clip_frames) {
                    memset(feeder_L, 0, out_want * sizeof(float));
                    memset(feeder_R, 0, out_want * sizeof(float));
                    jack_ringbuffer_write(t->play_buf_L,
                                         (const char *)feeder_L,
                                         out_want * sizeof(float));
                    jack_ringbuffer_write(t->play_buf_R,
                                         (const char *)feeder_R,
                                         out_want * sizeof(float));
                    break;
                }

                /* ---- SRC path (only when HAVE_SAMPLERATE is defined) ---- */
#ifdef HAVE_SAMPLERATE
                gboolean did_src = FALSE;
                if (needs_src && feeder_slots[i].src_L) {
                    double ratio = (double)jack_sr / (double)clip_sr;
                    long in_want = (long)ceil((double)out_want / ratio) + 8;
                    sf_count_t avail = clip_frames - cpos;
                    if ((sf_count_t)in_want > avail)
                        in_want = (long)avail;
                    if (in_want <= 0) {
                        /* Clip exhausted through SRC lookahead */
                        memset(feeder_L, 0, out_want * sizeof(float));
                        memset(feeder_R, 0, out_want * sizeof(float));
                        jack_ringbuffer_write(t->play_buf_L,
                                             (const char *)feeder_L,
                                             out_want * sizeof(float));
                        jack_ringbuffer_write(t->play_buf_R,
                                             (const char *)feeder_R,
                                             out_want * sizeof(float));
                        break;
                    }
                    if (in_want > FEEDER_RAW_FRAMES)
                        in_want = FEEDER_RAW_FRAMES;

                    sf_count_t got = sf_readf_float(feeder_slots[i].sf,
                                                    feeder_raw,
                                                    (sf_count_t)in_want);
                    if (got <= 0) {
                        memset(feeder_L, 0, out_want * sizeof(float));
                        memset(feeder_R, 0, out_want * sizeof(float));
                        jack_ringbuffer_write(t->play_buf_L,
                                             (const char *)feeder_L,
                                             out_want * sizeof(float));
                        jack_ringbuffer_write(t->play_buf_R,
                                             (const char *)feeder_R,
                                             out_want * sizeof(float));
                        feeder_slots[i].clip_pos = clip_frames;
                        keep_filling = FALSE;
                        break;
                    }

                    gboolean near_end =
                        (feeder_slots[i].clip_pos + got >= clip_frames);

                    /* Deinterleave channel 0 and resample to feeder_L */
                    sf_count_t f;
                    for (f = 0; f < got; f++)
                        feeder_mono[f] = feeder_raw[f * eff_ch];

                    SRC_DATA sd_L = {
                        .data_in       = feeder_mono,
                        .data_out      = feeder_L,
                        .input_frames  = (long)got,
                        .output_frames = (long)out_want,
                        .src_ratio     = ratio,
                        .end_of_input  = near_end ? 1 : 0
                    };
                    src_process(feeder_slots[i].src_L, &sd_L);
                    long out_gen = sd_L.output_frames_gen;

                    /* Deinterleave channel 1 and resample, or copy mono to R */
                    if (eff_ch > 1 && feeder_slots[i].src_R) {
                        for (f = 0; f < got; f++)
                            feeder_mono[f] = feeder_raw[f * eff_ch + 1];
                        SRC_DATA sd_R = {
                            .data_in       = feeder_mono,
                            .data_out      = feeder_R,
                            .input_frames  = (long)got,
                            .output_frames = (long)out_want,
                            .src_ratio     = ratio,
                            .end_of_input  = near_end ? 1 : 0
                        };
                        src_process(feeder_slots[i].src_R, &sd_R);
                    } else {
                        if (out_gen > 0)
                            memcpy(feeder_R, feeder_L,
                                   (size_t)out_gen * sizeof(float));
                    }

                    if (out_gen < (long)out_want) {
                        size_t pad = (size_t)((long)out_want - out_gen);
                        memset(feeder_L + out_gen, 0, pad * sizeof(float));
                        memset(feeder_R + out_gen, 0, pad * sizeof(float));
                        if (near_end) keep_filling = FALSE;
                    }

                    /* Advance clip_pos by actual input consumed by SRC */
                    feeder_slots[i].clip_pos += sd_L.input_frames_used;

                    jack_ringbuffer_write(t->play_buf_L,
                                         (const char *)feeder_L,
                                         out_want * sizeof(float));
                    jack_ringbuffer_write(t->play_buf_R,
                                         (const char *)feeder_R,
                                         out_want * sizeof(float));
                    did_src = TRUE;
                }
                if (!did_src) {
#else  /* !HAVE_SAMPLERATE */
                {
#endif /* HAVE_SAMPLERATE */
                    /* ---- Direct copy path (no SRC needed) ---- */
                    sf_count_t to_read = (sf_count_t)out_want;
                    sf_count_t avail   = clip_frames - cpos;
                    if (to_read > avail) to_read = avail;

                    sf_count_t got = sf_readf_float(feeder_slots[i].sf,
                                                    feeder_raw, to_read);
                    if (got <= 0) {
                        memset(feeder_L, 0, out_want * sizeof(float));
                        memset(feeder_R, 0, out_want * sizeof(float));
                        jack_ringbuffer_write(t->play_buf_L,
                                             (const char *)feeder_L,
                                             out_want * sizeof(float));
                        jack_ringbuffer_write(t->play_buf_R,
                                             (const char *)feeder_R,
                                             out_want * sizeof(float));
                        feeder_slots[i].clip_pos = clip_frames;
                        keep_filling = FALSE;
                    } else {
                        sf_count_t f;
                        if (eff_ch == 1) {
                            for (f = 0; f < got; f++)
                                feeder_L[f] = feeder_R[f] = feeder_raw[f];
                        } else {
                            for (f = 0; f < got; f++) {
                                feeder_L[f] = feeder_raw[f * eff_ch];
                                feeder_R[f] = feeder_raw[f * eff_ch + 1];
                            }
                        }
                        /* Zero-pad if near EOF */
                        if ((size_t)got < out_want) {
                            size_t pad = out_want - (size_t)got;
                            memset(feeder_L + got, 0, pad * sizeof(float));
                            memset(feeder_R + got, 0, pad * sizeof(float));
                            keep_filling = FALSE;
                        }
                        feeder_slots[i].clip_pos += got;
                        jack_ringbuffer_write(t->play_buf_L,
                                             (const char *)feeder_L,
                                             out_want * sizeof(float));
                        jack_ringbuffer_write(t->play_buf_R,
                                             (const char *)feeder_R,
                                             out_want * sizeof(float));
                    }
                }
            } /* inner fill loop */
        } /* for each slot */
    } /* while !feeder_stop_flag */

    /* Clean up all open handles on thread exit */
    for (i = 0; i < JACKDAW_MAX_TRACKS; i++)
        feeder_slot_close(i);

    return NULL;
}

static void feeder_start(void)
{
    if (feeder_started) return;

    feeder_raw  = g_new(float, FEEDER_RAW_FRAMES * FEEDER_MAX_CHANNELS);
    feeder_mono = g_new(float, FEEDER_RAW_FRAMES);
    feeder_L    = g_new(float, FEEDER_CHUNK_FRAMES);
    feeder_R    = g_new(float, FEEDER_CHUNK_FRAMES);

    feeder_stop_flag = 0;
    memset(feeder_slots, 0, sizeof(feeder_slots));

    if (pthread_create(&feeder_tid, NULL, feeder_thread_func, NULL) != 0) {
        g_free(feeder_raw);  g_free(feeder_mono);
        g_free(feeder_L);    g_free(feeder_R);
        feeder_raw = feeder_mono = feeder_L = feeder_R = NULL;
        return;
    }
    feeder_started = TRUE;
}

static void feeder_stop(void)
{
    if (!feeder_started) return;
    feeder_stop_flag = 1;
    pthread_join(feeder_tid, NULL);
    feeder_started = FALSE;
    g_free(feeder_raw);  feeder_raw  = NULL;
    g_free(feeder_mono); feeder_mono = NULL;
    g_free(feeder_L);    feeder_L    = NULL;
    g_free(feeder_R);    feeder_R    = NULL;
}

/* -----------------------------------------------------------------------
 * JACK process callback — RT thread, no malloc/free/mutex/file I/O
 * ----------------------------------------------------------------------- */

static int engine_process(jack_nframes_t nframes, void *arg)
{
    (void)arg;
    guint i;
    gint flags;
    gboolean any_soloed = FALSE;
    float *port_buf;
    jack_nframes_t k;

    /* Clear master mix buffers */
    memset(engine.master_L, 0, nframes * sizeof(float));
    memset(engine.master_R, 0, nframes * sizeof(float));

    flags = g_atomic_int_get(&engine.transport_flags);

    if (flags & ENGINE_PLAYING)
        engine.play_pos += nframes;

    /* First pass: check for any soloed track */
    for (i = 0; i < JACKDAW_MAX_TRACKS; i++) {
        JackDawTrack *t = engine.slots[i];
        if (!t) continue;
        if (g_atomic_int_get(&t->state_flags) & TRACK_SOLOED) {
            any_soloed = TRUE;
            break;
        }
    }

    /* Second pass: process each track */
    for (i = 0; i < JACKDAW_MAX_TRACKS; i++) {
        JackDawTrack *t = engine.slots[i];
        if (!t) continue;

        gint tflags = g_atomic_int_get(&t->state_flags);
        gboolean skip = (tflags & TRACK_MUTED) ||
                        (any_soloed && !(tflags & TRACK_SOLOED));

        if (skip) continue;

        /* Drain playback ringbuffers */
        size_t want = nframes * sizeof(float);
        size_t got_L = 0, got_R = 0;

        if (t->play_buf_L && (flags & ENGINE_PLAYING))
            got_L = jack_ringbuffer_read(t->play_buf_L,
                                         (char *)engine.tmp_L, want);
        if (t->play_buf_R && (flags & ENGINE_PLAYING))
            got_R = jack_ringbuffer_read(t->play_buf_R,
                                         (char *)engine.tmp_R, want);

        /* Zero-pad if ringbuffer ran dry */
        if (got_L < want)
            memset((char *)engine.tmp_L + got_L, 0, want - got_L);
        if (got_R < want)
            memset((char *)engine.tmp_R + got_R, 0, want - got_R);

        /* Constant-power pan law:
         *   angle = (pan + 1.0) * M_PI_4  maps [-1,1] -> [0, pi/2]
         *   L gain = vol * cos(angle), R gain = vol * sin(angle) */
        gfloat vol   = t->volume;
        gfloat pan   = t->pan;
        float angle  = (pan + 1.0f) * (float)M_PI_4;
        float gain_L = vol * cosf(angle);
        float gain_R = vol * sinf(angle);

        gfloat peak_L = 0.0f, peak_R = 0.0f;

        for (k = 0; k < nframes; k++) {
            float sL = engine.tmp_L[k] * gain_L;
            float sR = engine.tmp_R[k] * gain_R;
            engine.master_L[k] += sL;
            engine.master_R[k] += sR;
            if (sL < 0.0f) sL = -sL;
            if (sR < 0.0f) sR = -sR;
            if (sL > peak_L) peak_L = sL;
            if (sR > peak_R) peak_R = sR;
        }

        /* Update peaks for VU — racy write is acceptable */
        if (peak_L > t->peak_L) t->peak_L = peak_L;
        if (peak_R > t->peak_R) t->peak_R = peak_R;

        /* Record audio: feed rec ringbuffers from the assigned input port */
        if ((tflags & TRACK_ARMED) && (flags & ENGINE_RECORDING) &&
            t->audio_in_idx >= 0 &&
            (guint)t->audio_in_idx < engine.audio_in_count) {
            float *src = jack_port_get_buffer(
                engine.audio_in[t->audio_in_idx], nframes);
            if (t->rec_buf_L)
                jack_ringbuffer_write(t->rec_buf_L,
                                      (const char *)src,
                                      nframes * sizeof(float));
            if (t->rec_buf_R)
                jack_ringbuffer_write(t->rec_buf_R,
                                      (const char *)src,
                                      nframes * sizeof(float));
        }

        /* Record MIDI */
        if ((tflags & TRACK_ARMED) && (flags & ENGINE_RECORDING) &&
            t->midi_in_idx >= 0 &&
            (guint)t->midi_in_idx < engine.midi_in_count &&
            t->midi_rec_buf) {
            void *mbuf = jack_port_get_buffer(
                engine.midi_in[t->midi_in_idx], nframes);
            uint32_t mc = jack_midi_get_event_count(mbuf);
            uint32_t m;
            for (m = 0; m < mc; m++) {
                jack_midi_event_t ev;
                if (jack_midi_event_get(&ev, mbuf, m) != 0) continue;
                jack_ringbuffer_write(t->midi_rec_buf,
                                      (const char *)&ev.size,
                                      sizeof(ev.size));
                jack_ringbuffer_write(t->midi_rec_buf,
                                      (const char *)ev.buffer,
                                      ev.size);
            }
        }
    }

    /* Apply master volume and write to out_1 (L) / out_2 (R).
     * Additional outputs (out_3+) are zeroed so no stale data leaks out. */
    gfloat mvol = engine.project ? engine.project->master_volume : 1.0f;
    guint oi;
    for (oi = 0; oi < engine.audio_out_count; oi++) {
        if (!engine.audio_out[oi]) continue;
        port_buf = jack_port_get_buffer(engine.audio_out[oi], nframes);
        if (oi == 0) {
            for (k = 0; k < nframes; k++)
                port_buf[k] = engine.master_L[k] * mvol;
        } else if (oi == 1) {
            for (k = 0; k < nframes; k++)
                port_buf[k] = engine.master_R[k] * mvol;
        } else {
            memset(port_buf, 0, nframes * sizeof(float));
        }
    }

    return 0;
}

/* -----------------------------------------------------------------------
 * Buffer size callback — called by JACK when buffer size changes.
 * Must NOT block; reallocates mix buffers and track ringbuffers.
 * ----------------------------------------------------------------------- */

static int engine_buffer_size_cb(jack_nframes_t nframes, void *arg)
{
    (void)arg;

    /* Reallocate mix scratch buffers */
    g_free(engine.master_L);
    g_free(engine.master_R);
    g_free(engine.tmp_L);
    g_free(engine.tmp_R);

    engine.master_L = g_malloc0(nframes * sizeof(float));
    engine.master_R = g_malloc0(nframes * sizeof(float));
    engine.tmp_L    = g_malloc0(nframes * sizeof(float));
    engine.tmp_R    = g_malloc0(nframes * sizeof(float));
    engine.buf_size = nframes;

    /* Reallocate per-track ringbuffers sized to 2 seconds at current rate */
    jack_nframes_t sr = jack_get_sample_rate(engine.client);
    size_t rb_bytes = (size_t)(2 * sr) * sizeof(float);
    guint i;
    for (i = 0; i < JACKDAW_MAX_TRACKS; i++) {
        JackDawTrack *t = engine.slots[i];
        if (!t) continue;
        if (t->play_buf_L) jack_ringbuffer_free(t->play_buf_L);
        if (t->play_buf_R) jack_ringbuffer_free(t->play_buf_R);
        if (t->rec_buf_L)  jack_ringbuffer_free(t->rec_buf_L);
        if (t->rec_buf_R)  jack_ringbuffer_free(t->rec_buf_R);
        t->play_buf_L = jack_ringbuffer_create(rb_bytes);
        t->play_buf_R = jack_ringbuffer_create(rb_bytes);
        t->rec_buf_L  = jack_ringbuffer_create(rb_bytes);
        t->rec_buf_R  = jack_ringbuffer_create(rb_bytes);
        if (t->play_buf_L) jack_ringbuffer_mlock(t->play_buf_L);
        if (t->play_buf_R) jack_ringbuffer_mlock(t->play_buf_R);
        if (t->rec_buf_L)  jack_ringbuffer_mlock(t->rec_buf_L);
        if (t->rec_buf_R)  jack_ringbuffer_mlock(t->rec_buf_R);
        /* Ringbuffers just emptied — tell feeder to re-seek to play position */
        feeder_slots[i].locate_frame = (off_t)engine.play_pos;
        g_atomic_int_set(&feeder_slots[i].locate_req, 1);
    }

    return 0;
}

static void engine_shutdown_cb(void *arg)
{
    (void)arg;
    engine.active = FALSE;
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

gboolean jackdaw_engine_init(JackDawProject *project)
{
    jack_status_t status;
    jack_nframes_t sr, bs;
    size_t rb_bytes;
    char name[64];
    guint i;

    if (engine.active) return FALSE; /* already running */

    memset(&engine, 0, sizeof(engine));
    engine.project = project;

    /* Read port counts from project (loaded from inifile in project_init) */
    engine.audio_in_count  = CLAMP(project->audio_in_count,  1, 64);
    engine.audio_out_count = CLAMP(project->audio_out_count, 1, 64);
    engine.midi_in_count   = project->midi_in_count > 16u ? 16u
                             : project->midi_in_count;

    engine.client = jack_client_open("jackdaw", JackNullOption, &status);
    if (!engine.client) {
        user_error("Could not connect to JACK server.\n"
                   "Is jackd or pipewire-jack running?");
        return TRUE;
    }

    /* Query JACK's actual sample rate and buffer size */
    sr = jack_get_sample_rate(engine.client);
    bs = jack_get_buffer_size(engine.client);
    engine.buf_size = bs;
    rb_bytes = (size_t)(2 * sr) * sizeof(float);

    /* Allocate mix scratch buffers */
    engine.master_L = g_malloc0(bs * sizeof(float));
    engine.master_R = g_malloc0(bs * sizeof(float));
    engine.tmp_L    = g_malloc0(bs * sizeof(float));
    engine.tmp_R    = g_malloc0(bs * sizeof(float));

    /* Register callbacks */
    jack_set_process_callback(engine.client, engine_process, NULL);
    jack_set_buffer_size_callback(engine.client, engine_buffer_size_cb, NULL);
    jack_on_shutdown(engine.client, engine_shutdown_cb, NULL);

    /* Register audio input ports: in_1 .. in_N */
    engine.audio_in = g_new0(jack_port_t *, engine.audio_in_count);
    for (i = 0; i < engine.audio_in_count; i++) {
        g_snprintf(name, sizeof(name), "in_%u", i + 1);
        engine.audio_in[i] = jack_port_register(engine.client, name,
            JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
        if (!engine.audio_in[i]) goto fail;
    }

    /* Register audio output ports: out_1 .. out_N */
    engine.audio_out = g_new0(jack_port_t *, engine.audio_out_count);
    for (i = 0; i < engine.audio_out_count; i++) {
        g_snprintf(name, sizeof(name), "out_%u", i + 1);
        engine.audio_out[i] = jack_port_register(engine.client, name,
            JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
        if (!engine.audio_out[i]) goto fail;
    }

    /* Register MIDI input ports: midi_in_1 .. midi_in_M */
    engine.midi_in = g_new0(jack_port_t *, engine.midi_in_count + 1);
    for (i = 0; i < engine.midi_in_count; i++) {
        g_snprintf(name, sizeof(name), "midi_in_%u", i + 1);
        engine.midi_in[i] = jack_port_register(engine.client, name,
            JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
        if (!engine.midi_in[i]) goto fail;
    }

    /* Activate — after this the process callback can be called at any time */
    if (jack_activate(engine.client) != 0) {
        user_error("jackdaw: jack_activate() failed");
        goto fail;
    }

    /* Start feeder thread — keeps play_buf_L/R filled from AudioClip */
    feeder_start();

    engine.active = TRUE;
    (void)rb_bytes; /* suppress unused-variable warning */
    return FALSE;   /* success */

fail:
    jack_client_close(engine.client);
    engine.client = NULL;
    g_free(engine.master_L); g_free(engine.master_R);
    g_free(engine.tmp_L);    g_free(engine.tmp_R);
    g_free(engine.audio_in); g_free(engine.audio_out); g_free(engine.midi_in);
    return TRUE;
}

void jackdaw_engine_quit(void)
{
    if (!engine.active || !engine.client) return;

    /* Stop feeder before deactivating JACK so the thread exits cleanly */
    feeder_stop();

    jack_deactivate(engine.client);
    jack_client_close(engine.client);
    engine.client = NULL;
    engine.active = FALSE;

    g_free(engine.master_L); engine.master_L = NULL;
    g_free(engine.master_R); engine.master_R = NULL;
    g_free(engine.tmp_L);    engine.tmp_L    = NULL;
    g_free(engine.tmp_R);    engine.tmp_R    = NULL;
    g_free(engine.audio_in);  engine.audio_in  = NULL;
    g_free(engine.audio_out); engine.audio_out = NULL;
    g_free(engine.midi_in);   engine.midi_in   = NULL;
}

gboolean jackdaw_engine_is_running(void)
{
    return engine.active;
}

/* ---- Port count management ---- */

gboolean jackdaw_engine_set_audio_in_count(guint n)
{
    guint i;
    char name[64];
    n = CLAMP(n, 1, 64);
    if (!engine.active) { engine.audio_in_count = n; return FALSE; }

    /* Unregister ports being removed */
    for (i = n; i < engine.audio_in_count; i++) {
        if (engine.audio_in[i])
            jack_port_unregister(engine.client, engine.audio_in[i]);
    }
    engine.audio_in = g_renew(jack_port_t *, engine.audio_in, n);
    /* Register new ports */
    for (i = engine.audio_in_count; i < n; i++) {
        g_snprintf(name, sizeof(name), "in_%u", i + 1);
        engine.audio_in[i] = jack_port_register(engine.client, name,
            JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
        if (!engine.audio_in[i]) return TRUE;
    }
    engine.audio_in_count = n;
    settings_set_uint32("jackAudioInCount", n);
    if (engine.project)
        jackdaw_project_emit_ports_changed(engine.project);
    return FALSE;
}

gboolean jackdaw_engine_set_audio_out_count(guint n)
{
    guint i;
    char name[64];
    n = CLAMP(n, 1, 64);
    if (!engine.active) { engine.audio_out_count = n; return FALSE; }

    for (i = n; i < engine.audio_out_count; i++) {
        if (engine.audio_out[i])
            jack_port_unregister(engine.client, engine.audio_out[i]);
    }
    engine.audio_out = g_renew(jack_port_t *, engine.audio_out, n);
    for (i = engine.audio_out_count; i < n; i++) {
        g_snprintf(name, sizeof(name), "out_%u", i + 1);
        engine.audio_out[i] = jack_port_register(engine.client, name,
            JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
        if (!engine.audio_out[i]) return TRUE;
    }
    engine.audio_out_count = n;
    settings_set_uint32("jackAudioOutCount", n);
    if (engine.project)
        jackdaw_project_emit_ports_changed(engine.project);
    return FALSE;
}

gboolean jackdaw_engine_set_midi_in_count(guint n)
{
    guint i;
    char name[64];
    if (n > 16u) n = 16u;
    if (!engine.active) { engine.midi_in_count = n; return FALSE; }

    for (i = n; i < engine.midi_in_count; i++) {
        if (engine.midi_in[i])
            jack_port_unregister(engine.client, engine.midi_in[i]);
    }
    engine.midi_in = g_renew(jack_port_t *, engine.midi_in, n + 1);
    for (i = engine.midi_in_count; i < n; i++) {
        g_snprintf(name, sizeof(name), "midi_in_%u", i + 1);
        engine.midi_in[i] = jack_port_register(engine.client, name,
            JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
        if (!engine.midi_in[i]) return TRUE;
    }
    engine.midi_in_count = n;
    settings_set_uint32("jackMidiInCount", n);
    if (engine.project)
        jackdaw_project_emit_ports_changed(engine.project);
    return FALSE;
}

guint jackdaw_engine_get_audio_in_count (void) { return engine.audio_in_count;  }
guint jackdaw_engine_get_audio_out_count(void) { return engine.audio_out_count; }
guint jackdaw_engine_get_midi_in_count  (void) { return engine.midi_in_count;   }

jack_port_t *jackdaw_engine_get_audio_in_port(guint idx)
{
    if (!engine.audio_in || idx >= engine.audio_in_count) return NULL;
    return engine.audio_in[idx];
}

jack_port_t *jackdaw_engine_get_midi_in_port(guint idx)
{
    if (!engine.midi_in || idx >= engine.midi_in_count) return NULL;
    return engine.midi_in[idx];
}

/* ---- Track management ---- */

gboolean jackdaw_engine_add_track(JackDawTrack *track)
{
    guint i;
    jack_nframes_t sr;
    size_t rb_bytes;

    g_return_val_if_fail(JACKDAW_IS_TRACK(track), TRUE);

    /* Find a free slot */
    for (i = 0; i < JACKDAW_MAX_TRACKS; i++) {
        if (!engine.slots[i]) break;
    }
    if (i == JACKDAW_MAX_TRACKS) {
        user_error("jackdaw: maximum track count reached");
        return TRUE;
    }

    /* Allocate ringbuffers sized to 2 seconds at the detected sample rate */
    sr = engine.client ? jack_get_sample_rate(engine.client) : 48000;
    rb_bytes = (size_t)(2 * sr) * sizeof(float);

    track->play_buf_L = jack_ringbuffer_create(rb_bytes);
    track->play_buf_R = jack_ringbuffer_create(rb_bytes);
    track->rec_buf_L  = jack_ringbuffer_create(rb_bytes);
    track->rec_buf_R  = jack_ringbuffer_create(rb_bytes);
    track->midi_rec_buf = jack_ringbuffer_create(TRACK_MIDI_RINGBUF_BYTES);

    if (!track->play_buf_L || !track->play_buf_R ||
        !track->rec_buf_L  || !track->rec_buf_R  || !track->midi_rec_buf) {
        user_error("jackdaw: ringbuffer allocation failed");
        return TRUE;
    }

    jack_ringbuffer_mlock(track->play_buf_L);
    jack_ringbuffer_mlock(track->play_buf_R);
    jack_ringbuffer_mlock(track->rec_buf_L);
    jack_ringbuffer_mlock(track->rec_buf_R);

    track->slot   = i;
    engine.slots[i] = track; /* RT callback can see this now */

    /* If playback is in progress, sync the new track to the current position
     * so it starts in the right place rather than always from frame 0. */
    if (engine.active &&
        (g_atomic_int_get(&engine.transport_flags) & ENGINE_PLAYING)) {
        feeder_slots[i].locate_frame = (off_t)engine.play_pos;
        g_atomic_int_set(&feeder_slots[i].locate_req, 1);
    }

    return FALSE;
}

void jackdaw_engine_remove_track(JackDawTrack *track)
{
    guint i;
    g_return_if_fail(JACKDAW_IS_TRACK(track));
    i = track->slot;
    if (i >= JACKDAW_MAX_TRACKS || engine.slots[i] != track) return;

    engine.slots[i] = NULL; /* RT callback stops using this slot */
    track->slot = G_MAXUINT;
}

/* ---- Transport ---- */

void jackdaw_engine_start_playback(void)
{
    g_atomic_int_or(&engine.transport_flags, ENGINE_PLAYING);
}

void jackdaw_engine_stop_playback(void)
{
    g_atomic_int_and(&engine.transport_flags, ~ENGINE_PLAYING);
}

void jackdaw_engine_start_recording(void)
{
    g_atomic_int_or(&engine.transport_flags, ENGINE_RECORDING);
}

void jackdaw_engine_stop_recording(void)
{
    g_atomic_int_and(&engine.transport_flags, ~ENGINE_RECORDING);
}

void jackdaw_engine_locate(off_t sample)
{
    guint i;
    jackdaw_engine_stop_playback();
    jackdaw_engine_stop_recording();
    engine.play_pos = sample;
    for (i = 0; i < JACKDAW_MAX_TRACKS; i++) {
        JackDawTrack *t = engine.slots[i];
        if (!t) continue;
        t->played_frames = sample;
        if (t->play_buf_L) jack_ringbuffer_reset(t->play_buf_L);
        if (t->play_buf_R) jack_ringbuffer_reset(t->play_buf_R);
        /* Tell feeder to re-seek each slot to the new position */
        feeder_slots[i].locate_frame = sample;
        g_atomic_int_set(&feeder_slots[i].locate_req, 1);
    }
}

jack_nframes_t jackdaw_engine_get_sample_rate(void)
{
    if (!engine.client) return 48000;
    return jack_get_sample_rate(engine.client);
}

jack_nframes_t jackdaw_engine_get_buffer_size(void)
{
    if (!engine.client) return 1024;
    return jack_get_buffer_size(engine.client);
}

off_t jackdaw_engine_get_play_pos(void)
{
    return engine.play_pos;
}
