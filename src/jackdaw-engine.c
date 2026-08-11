#define _GNU_SOURCE
#include <config.h>

#include <string.h>
#include <stdlib.h>   /* qsort */
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <jack/jack.h>
#include <jack/midiport.h>
#include <jack/ringbuffer.h>
#include <jack/thread.h>     /* jack_client_create_thread (RT-priority workers) */
#include <semaphore.h>
#ifdef HAVE_SAMPLERATE
#  include <samplerate.h>
#endif

#include "rt_denormal.h"   /* FTZ/DAZ for the RT thread */

#include "jackdaw-engine.h"
#include "pluginhost.h"
#include "settings.h"
#include "message.h"

/* -----------------------------------------------------------------------
 * Internal state
 * ----------------------------------------------------------------------- */

/* Transport control flags — written by main thread, read by RT callback.
 * Use g_atomic_int_* for all accesses. */
#define ENGINE_PLAYING   (1 << 0)
#define ENGINE_RECORDING (1 << 1)

/* Diagnostics (JACKDAW_DIAG) — declared up here so engine_process() can write
 * them; the xrun callback + reporter thread live near jackdaw_engine_init(). */
static volatile gint   g_diag_xruns      = 0;

/* Per-plugin worst-case process() time (microseconds) for the current reporting
 * second, indexed [track slot][chain position]. Written by the RT thread that
 * owns the slot, drained by the diag thread. A plain compare-and-store rather
 * than a CAS loop: two threads racing on one element can at worst drop a sample
 * from a diagnostic maximum, which is not worth a CAS on the audio path. */
/* Ceilings for the JACK port pools. The arrays are allocated at these sizes once
 * and never resized, so the RT callback never indexes through a reallocation. */
#define ENG_MAX_AUDIO_PORTS 64
#define ENG_MAX_MIDI_PORTS  16

#define ENG_DIAG_FX_SLOTS 8
static volatile gint   g_diag_fx_us[JACKDAW_MAX_TRACKS][ENG_DIAG_FX_SLOTS];
static volatile gint64 g_diag_cb_last_us = 0;
static volatile gint64 g_diag_cb_max_us  = 0;
static volatile gint64 g_diag_period_us  = 0;
static gboolean        g_diag_on         = FALSE;
static GThread        *g_diag_thread     = NULL;
static volatile gint   g_diag_quit       = 0;

typedef struct {
    jack_client_t *client;
    JackDawProject *project;        /* weak ref — project owns the engine */

    /* Audio input/output ports — indexed [0..audio_in_count-1] etc.
     * audio_in[i] is each track's LEFT capture port (in_N); audio_in_r[i] is
     * the matching RIGHT capture port (in_NR) for true-stereo track input. */
    jack_port_t **audio_in;
    jack_port_t **audio_in_r;
    jack_port_t **audio_out;
    jack_port_t **midi_in;
    jack_port_t **midi_out;

    /* Dedicated control-surface MIDI input (footswitch / CC mappings). Read on
     * the RT thread, drained on the main thread. Separate from track routing. */
    jack_port_t  *control_in;
    gchar        *control_src_port;  /* connected source name, or NULL */

    /* Live port counts. Read by the RT callback, changed from the main thread
     * via the ordered publish in the set_*_count functions — volatile so the
     * callback re-reads them rather than caching a stale bound. */
    volatile guint audio_in_count;
    volatile guint audio_out_count;
    volatile guint midi_in_count;
    volatile guint midi_out_count;

    /* Pre-allocated mix buffers (sized to max buffer size at init) */
    float *master_L;
    float *master_R;
    float *tmp_L;
    float *tmp_R;
    jack_nframes_t buf_size;    /* current buffer size */

    /* Per-slot scratch for PARALLEL track processing: each track's worker writes
     * its post-fader stereo contribution into slot_L[i]/slot_R[i]; the RT thread
     * sums them into master after the barrier. Sized to buf_size (init +
     * buffer_size_cb). Each slot is touched by at most one worker per cycle. */
    float *slot_L[JACKDAW_MAX_TRACKS];
    float *slot_R[JACKDAW_MAX_TRACKS];

    /* Weak refs to active tracks — slots populated by engine_add_track */
    JackDawTrack *slots[JACKDAW_MAX_TRACKS];

    volatile gint transport_flags; /* ENGINE_PLAYING | ENGINE_RECORDING */
    volatile off_t play_pos;       /* sample counter, incremented by process cb */

    /* Loop region (frames). Looping is active only while loop_enabled is set and
     * loop_end > loop_start; the playhead wraps loop_end -> loop_start once it has
     * entered the region. Set/cleared from the main thread; read in the RT path. */
    volatile gint  loop_enabled;
    volatile off_t loop_start;
    volatile off_t loop_end;

    /* Punch in/out recording (independent of looping). record_mode is set from
     * the UI menu; punch_armed is set when playback starts in punch mode and the
     * RT path auto-engages ENGINE_RECORDING over the [loop_start, loop_end) region. */
    volatile gint  record_mode;    /* RECORD_MODE_NORMAL | RECORD_MODE_PUNCH */
    volatile gint  punch_armed;    /* 1 while a punch is pending/in progress */

    /* Count-in pre-roll. While countin_active the transport is in a metronome-
     * only lead-in: the project is frozen (play_pos does not advance) and nothing
     * records. The metronome clicks from countin_pos; when it reaches countin_len
     * the pending transport (PLAYING, plus RECORDING if countin_pending_rec)
     * engages and normal play begins from the unchanged play_pos. Set up on the
     * main thread (recorder slots pre-opened first when recording); the RT path
     * drives countin_pos and performs the hand-off. */
    volatile gint  countin_active;
    volatile gint  countin_pending_rec;
    volatile off_t countin_pos;
    volatile off_t countin_len;

    jack_nframes_t sample_rate;    /* cached at init */

    /* Pre-rendered metronome click (mono), built at init. */
    float *click_buf;
    int    click_len;

    /* Dedicated mono metronome output port ("metronome"). The click is always
     * mirrored here so it can be routed to a performer's headphones independently
     * of the main mix; whether it ALSO reaches the main outs is set per-project
     * (metronome_route). Never part of the master sum, meters, or render tap. */
    jack_port_t *metro_out;

    /* Post-master-fader peak meter (master VU). Racy read is acceptable. */
    volatile gfloat master_peak_L;
    volatile gfloat master_peak_R;

    /* --- Render support ---
     * render_suspend: while set, engine_process outputs silence and runs NO
     *   plugins, so whoever set it has exclusive use of every PluginInstance
     *   (no two threads in the same plugin). Used by the offline render worker
     *   AND by project load / app teardown — periods when the main thread is
     *   instantiating or freeing plugins and the RT graph must not touch them.
     * render_active: a realtime master tap — the final post-fader master block
     *   is copied to render_rb_L/R (lock-free) for a writer thread to drain.
     * render_tap_L/R are pre-allocated scratch sized to buf_size. */
    volatile gint      render_suspend;
    volatile gint      render_active;
    volatile off_t     render_end;     /* tap stops when play_pos >= render_end */
    volatile gint      render_done;    /* set by RT when render_end reached */
    float             *render_tap_L;
    float             *render_tap_R;
    jack_ringbuffer_t *render_rb_L;    /* RT -> render writer thread */
    jack_ringbuffer_t *render_rb_R;

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

/* Per-slot feeder state — only the atomic locate fields are shared with the
 * main thread; everything else is private to the feeder thread. */
typedef struct {
    SNDFILE      *sf;           /* open file for the current region; NULL = none */
    int           open_clip_sr; /* sample rate of the open file */
    int           open_clip_ch; /* channel count of the open file */
#ifdef HAVE_SAMPLERATE
    SRC_STATE    *src_L;        /* resampler for channel 0; NULL = no SRC */
    SRC_STATE    *src_R;        /* resampler for channel 1; NULL = mono or no SRC */
#endif
    /* Locate protocol: main thread writes locate_frame then sets locate_req=1.
     * Feeder does CAS(1->0) and applies the seek. */
    volatile gint locate_req;   /* g_atomic_int */
    off_t         locate_frame; /* timeline frame target; valid when locate_req==1 */

    off_t         play_frame;   /* feeder's current timeline frame position */
    ClipRegionSnapshot *snap;   /* snapshot the feeder currently holds a ref to */
    int           open_region;  /* index into snap of the open region; -1 = none */
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

/* Close a slot's open file and SRC states (feeder thread or stop path only).
 * Leaves play_frame and the held snapshot untouched. */
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
    feeder_slots[i].open_region = -1;
}

/* Full release: file handle, SRC, and snapshot reference. */
static void feeder_slot_release(guint i)
{
    feeder_slot_close(i);
    if (feeder_slots[i].snap) {
        clip_region_snapshot_unref(feeder_slots[i].snap);
        feeder_slots[i].snap = NULL;
    }
    feeder_slots[i].play_frame = 0;
    g_atomic_int_set(&feeder_slots[i].locate_req, 0);
}

/* Write n frames of feeder_L/feeder_R into a track's playback ringbuffers. */
static void feeder_write(JackDawTrack *t, size_t n)
{
    if (n == 0) return;
    jack_ringbuffer_write(t->play_buf_L, (const char *)feeder_L,
                          n * sizeof(float));
    jack_ringbuffer_write(t->play_buf_R, (const char *)feeder_R,
                          n * sizeof(float));
}

static void feeder_emit_silence(JackDawTrack *t, size_t n)
{
    memset(feeder_L, 0, n * sizeof(float));
    memset(feeder_R, 0, n * sizeof(float));
    feeder_write(t, n);
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

        int jack_sr = (int)jack_get_sample_rate(engine.client);

        for (i = 0; i < JACKDAW_MAX_TRACKS; i++) {
            JackDawTrack *t = engine.slots[i];

            if (!t) {
                if (feeder_slots[i].snap || feeder_slots[i].sf)
                    feeder_slot_release(i);
                continue;
            }
            if (!t->play_buf_L || !t->play_buf_R)
                continue;

            /* --- Refresh the immutable region snapshot --- */
            ClipRegionSnapshot *ns = jackdaw_track_ref_snapshot(t);
            if (ns != feeder_slots[i].snap) {
                feeder_slot_close(i);
                if (feeder_slots[i].snap)
                    clip_region_snapshot_unref(feeder_slots[i].snap);
                feeder_slots[i].snap = ns;
            } else {
                clip_region_snapshot_unref(ns);
            }
            ClipRegionSnapshot *snap = feeder_slots[i].snap;

            /* --- Handle locate request from the main thread --- */
            if (g_atomic_int_compare_and_exchange(
                    &feeder_slots[i].locate_req, 1, 0)) {
                feeder_slots[i].play_frame = feeder_slots[i].locate_frame;
                feeder_slot_close(i);
            }

            /* --- Inner fill loop: top up the ringbuffer each wakeup --- */
            while (TRUE) {
                size_t space_L = jack_ringbuffer_write_space(t->play_buf_L)
                                 / sizeof(float);
                size_t space_R = jack_ringbuffer_write_space(t->play_buf_R)
                                 / sizeof(float);
                size_t out_want = (space_L < space_R) ? space_L : space_R;
                if (out_want > FEEDER_CHUNK_FRAMES)
                    out_want = FEEDER_CHUNK_FRAMES;
                if (out_want == 0)
                    break; /* buffer full */

                off_t pf = feeder_slots[i].play_frame;

                /* --- Loop wrap: keep the feeder's stream inside the loop
                 * region so the ringbuffer never buffers audio past loop_end.
                 * Production is clamped to stop exactly on loop_start (when
                 * approaching from before) and on loop_end (when inside); the
                 * wrap then fires precisely at loop_end. A position already
                 * past loop_end (playhead placed after the region) is left
                 * alone, so playback there does not loop. --- */
                if (g_atomic_int_get(&engine.loop_enabled)) {
                    off_t l_start = engine.loop_start;
                    off_t l_end   = engine.loop_end;
                    if (l_end > l_start) {
                        if (pf == l_end) {
                            pf = l_start;
                            feeder_slots[i].play_frame = pf;
                            feeder_slot_close(i);  /* force re-seek of source */
                        }
                        if (pf < l_start) {
                            off_t room = l_start - pf;  /* stop at region start */
                            if ((off_t)out_want > room) out_want = (size_t)room;
                        } else if (pf < l_end) {
                            off_t room = l_end - pf;     /* stop at region end */
                            if ((off_t)out_want > room) out_want = (size_t)room;
                        }
                    }
                }

                /* Locate the region covering pf and the next region after it. */
                ClipRegion *reg = NULL;
                int   reg_idx    = -1;
                off_t next_start = -1;
                for (int k = 0; snap && k < snap->n; k++) {
                    ClipRegion *r = &snap->r[k];
                    if (pf >= r->tl_pos && pf < r->tl_pos + r->length) {
                        reg = r; reg_idx = k; break;
                    }
                    if (r->tl_pos > pf &&
                        (next_start < 0 || r->tl_pos < next_start))
                        next_start = r->tl_pos;
                }

                /* --- Gap / before first / past end: emit silence --- */
                if (!reg) {
                    size_t sil = out_want;
                    if (next_start >= 0) {
                        off_t to_next = next_start - pf;
                        if (to_next > 0 && (off_t)sil > to_next)
                            sil = (size_t)to_next;
                    }
                    if (sil == 0) break;
                    if (feeder_slots[i].sf) feeder_slot_close(i);
                    feeder_emit_silence(t, sil);
                    feeder_slots[i].play_frame = pf + (off_t)sil;
                    continue;
                }

                int clip_sr = reg->clip ? reg->clip->info.samplerate : jack_sr;
                int clip_ch = reg->clip ? reg->clip->info.channels   : 1;
                int eff_ch  = (clip_ch > FEEDER_MAX_CHANNELS)
                              ? FEEDER_MAX_CHANNELS : clip_ch;
                gboolean needs_src = (clip_sr != jack_sr);
#ifndef HAVE_SAMPLERATE
                needs_src = FALSE;
#endif
                off_t d         = pf - reg->tl_pos;       /* timeline frames in */
                off_t reg_remain = reg->length - d;       /* timeline frames left */
                if (reg_remain <= 0) {
                    feeder_slots[i].play_frame = reg->tl_pos + reg->length;
                    continue;
                }

                /* --- Open / seek file for this region if needed --- */
                if (reg_idx != feeder_slots[i].open_region ||
                    !feeder_slots[i].sf) {
                    feeder_slot_close(i);
                    SF_INFO sfi = {0};
                    SNDFILE *sf = reg->clip
                        ? sf_open(reg->clip->path, SFM_READ, &sfi) : NULL;
                    if (!sf) {
                        /* Cannot read — render the rest of the region as silence */
                        size_t sil = out_want;
                        if ((off_t)sil > reg_remain) sil = (size_t)reg_remain;
                        feeder_emit_silence(t, sil);
                        feeder_slots[i].play_frame = pf + (off_t)sil;
                        continue;
                    }
                    off_t file_off = reg->file_in +
                        ((clip_sr == jack_sr)
                         ? d
                         : (off_t)((double)d * clip_sr / jack_sr + 0.5));
                    sf_seek(sf, file_off, SEEK_SET);
                    feeder_slots[i].sf           = sf;
                    feeder_slots[i].open_clip_sr = clip_sr;
                    feeder_slots[i].open_clip_ch = clip_ch;
                    feeder_slots[i].open_region  = reg_idx;
#ifdef HAVE_SAMPLERATE
                    if (needs_src) {
                        int e = 0;
                        feeder_slots[i].src_L = src_new(SRC_SINC_FASTEST, 1, &e);
                        if (eff_ch > 1)
                            feeder_slots[i].src_R =
                                src_new(SRC_SINC_FASTEST, 1, &e);
                    }
#endif
                }

                size_t want = out_want;
                if ((off_t)want > reg_remain) want = (size_t)reg_remain;
                gfloat gain = reg->gain;

                if (!needs_src) {
                    /* ---- Direct copy path ---- */
                    sf_count_t got = sf_readf_float(feeder_slots[i].sf,
                                                    feeder_raw,
                                                    (sf_count_t)want);
                    if (got < 0) got = 0;
                    if (eff_ch == 1) {
                        for (sf_count_t f = 0; f < got; f++)
                            feeder_L[f] = feeder_R[f] = feeder_raw[f] * gain;
                    } else {
                        for (sf_count_t f = 0; f < got; f++) {
                            feeder_L[f] = feeder_raw[f * eff_ch]     * gain;
                            feeder_R[f] = feeder_raw[f * eff_ch + 1] * gain;
                        }
                    }
                    if ((size_t)got < want) {
                        size_t pad = want - (size_t)got;
                        memset(feeder_L + got, 0, pad * sizeof(float));
                        memset(feeder_R + got, 0, pad * sizeof(float));
                    }
                    feeder_write(t, want);
                    feeder_slots[i].play_frame = pf + (off_t)want;
                }
#ifdef HAVE_SAMPLERATE
                else if (feeder_slots[i].src_L) {
                    /* ---- Resampled path (clip SR != JACK SR) ---- */
                    double ratio   = (double)jack_sr / (double)clip_sr;
                    long   want_l  = (long)want;
                    long   in_need = (long)ceil((double)want / ratio) + 8;
                    if (in_need > FEEDER_RAW_FRAMES) in_need = FEEDER_RAW_FRAMES;
                    int    eoi     = (want == (size_t)reg_remain);

                    sf_count_t got = sf_readf_float(feeder_slots[i].sf,
                                                    feeder_raw,
                                                    (sf_count_t)in_need);
                    if (got < 0) got = 0;

                    for (sf_count_t f = 0; f < got; f++)
                        feeder_mono[f] = feeder_raw[f * eff_ch];
                    SRC_DATA sd_L = {
                        .data_in = feeder_mono, .data_out = feeder_L,
                        .input_frames = (long)got, .output_frames = want_l,
                        .src_ratio = ratio, .end_of_input = eoi
                    };
                    src_process(feeder_slots[i].src_L, &sd_L);
                    long out_gen = sd_L.output_frames_gen;

                    if (eff_ch > 1 && feeder_slots[i].src_R) {
                        for (sf_count_t f = 0; f < got; f++)
                            feeder_mono[f] = feeder_raw[f * eff_ch + 1];
                        SRC_DATA sd_R = {
                            .data_in = feeder_mono, .data_out = feeder_R,
                            .input_frames = (long)got, .output_frames = want_l,
                            .src_ratio = ratio, .end_of_input = eoi
                        };
                        src_process(feeder_slots[i].src_R, &sd_R);
                    } else if (out_gen > 0) {
                        memcpy(feeder_R, feeder_L,
                               (size_t)out_gen * sizeof(float));
                    }

                    for (long k = 0; k < out_gen; k++) {
                        feeder_L[k] *= gain;
                        feeder_R[k] *= gain;
                    }
                    if (out_gen < want_l) {
                        size_t pad = (size_t)(want_l - out_gen);
                        memset(feeder_L + out_gen, 0, pad * sizeof(float));
                        memset(feeder_R + out_gen, 0, pad * sizeof(float));
                    }
                    feeder_write(t, want);
                    feeder_slots[i].play_frame = pf + (off_t)want;

                    /* Rewind file input SRC did not consume so the next read
                     * stays aligned with the timeline. */
                    long used = sd_L.input_frames_used;
                    if (used < got)
                        sf_seek(feeder_slots[i].sf,
                                -(sf_count_t)(got - used), SEEK_CUR);
                }
#endif /* HAVE_SAMPLERATE */
                else {
                    /* SRC needed but unavailable — emit silence for the region */
                    feeder_emit_silence(t, want);
                    feeder_slots[i].play_frame = pf + (off_t)want;
                }
            } /* inner fill loop */
        } /* for each slot */
    } /* while !feeder_stop_flag */

    /* Clean up all open handles and snapshots on thread exit */
    for (i = 0; i < JACKDAW_MAX_TRACKS; i++)
        feeder_slot_release(i);

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
    for (guint i = 0; i < JACKDAW_MAX_TRACKS; i++)
        feeder_slots[i].open_region = -1;

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
 * Recorder thread — drains rec_buf_L/R into WAV files on disk.
 * No malloc/free/file-I/O in the RT callback; all disk work is here.
 * ----------------------------------------------------------------------- */

#define REC_SCRATCH_FRAMES    4096
/* Max JACK periods capturable for real-time waveform: ~46 min at 48kHz/1024 */
#define REC_PEAK_MAX_BUCKETS 131072

typedef struct {
    SNDFILE *sf;              /* open for writing; NULL = idle */
    char     path[512];       /* destination file path */
    off_t    written;         /* frames written so far */
    volatile off_t expected_frames; /* frames to capture; 0 = recording, set at stop */
    gint     finalize_req;    /* g_atomic: stop_recording sets to 1 */
    int      channels;        /* 1 = mono, 2 = stereo; set when file is opened */
    int      punch;           /* 1 = punch take; overwrites a region on finalize */
    off_t    punch_tl_start;  /* loop_start at arm time (timeline frames) */
    off_t    punch_tl_end;    /* loop_end   at arm time (timeline frames) */
} RecorderSlot;

static RecorderSlot  recorder_slots[JACKDAW_MAX_TRACKS];
static pthread_t     recorder_tid;
static volatile int  recorder_stop_flag;
static gboolean      recorder_started = FALSE;

static float *rec_scratch_L;
static float *rec_scratch_R;
static float *rec_interleaved;  /* 2 * REC_SCRATCH_FRAMES for stereo write */

typedef struct {
    JackDawTrack *track;    /* strong ref — released by idle */
    char          path[512];
    int           punch;          /* 1 = overwrite [punch_tl_start, punch_tl_end) */
    off_t         punch_tl_start;
    off_t         punch_tl_end;
} RecordFinalize;

static gboolean recorder_finalize_idle(gpointer data)
{
    RecordFinalize *rf = data;
    GError    *err  = NULL;
    AudioClip *clip = audio_clip_new(rf->path, &err);
    if (clip && rf->punch) {
        /* Punch take: overwrite the existing audio in the tab region, then drop
         * the new clip in at the punch-in point (no latency shift — it must line
         * up exactly with the cleared range). */
        int sr = (int)jackdaw_engine_get_sample_rate();
        clip_region_list_delete_range(rf->track->regions,
                                      rf->punch_tl_start, rf->punch_tl_end, sr);
        rf->track->clip_start = rf->punch_tl_start;
        jackdaw_track_place_clip(rf->track, clip, rf->punch_tl_start);
    } else if (clip) {
        /* Place the recording as a new region at the point where recording
         * started, shifted earlier by the JACK capture latency so the audio
         * sits under the waveform the user actually played against. */
        off_t tl = rf->track->rec_start_frame - rf->track->rec_latency;
        if (tl < 0) tl = 0;
        rf->track->clip_start = tl;
        jackdaw_track_place_clip(rf->track, clip, tl);  /* consumes clip ref */
    } else {
        g_warning("jackdaw: could not load recording %s: %s",
                  rf->path, err ? err->message : "unknown");
        if (err) g_error_free(err);
    }
    /* Free the real-time waveform peak buffer now that the clip is loaded */
    rf->track->rec_peak_count = 0;
    g_free(rf->track->rec_peak_buf);
    rf->track->rec_peak_buf = NULL;

    g_object_unref(rf->track);
    g_free(rf);
    return G_SOURCE_REMOVE;
}

/* Drain remaining data from a slot's rec_buf, close the WAV file, and
 * schedule a main-thread callback to create the AudioClip. */
static void recorder_slot_finalize(guint i)
{
    RecorderSlot *rs = &recorder_slots[i];
    if (!rs->sf) return;

    JackDawTrack *t = engine.slots[i];
    if (t && t->rec_buf_L && t->rec_buf_R) {
        while (TRUE) {
            size_t avL = jack_ringbuffer_read_space(t->rec_buf_L) / sizeof(float);
            size_t avR = jack_ringbuffer_read_space(t->rec_buf_R) / sizeof(float);
            size_t av  = avL < avR ? avL : avR;
            if (av > REC_SCRATCH_FRAMES) av = REC_SCRATCH_FRAMES;
            /* Cap at expected_frames so the WAV ends exactly at the stop point */
            if (rs->expected_frames > 0) {
                off_t rem = rs->expected_frames - rs->written;
                if (rem <= 0) break;
                if ((off_t)av > rem) av = (size_t)rem;
            }
            if (av == 0) break;

            jack_ringbuffer_read(t->rec_buf_L, (char *)rec_scratch_L, av * sizeof(float));
            jack_ringbuffer_read(t->rec_buf_R, (char *)rec_scratch_R, av * sizeof(float));

            if (rs->channels == 1) {
                rs->written += sf_writef_float(rs->sf, rec_scratch_L, (sf_count_t)av);
            } else {
                for (size_t f = 0; f < av; f++) {
                    rec_interleaved[f * 2]     = rec_scratch_L[f];
                    rec_interleaved[f * 2 + 1] = rec_scratch_R[f];
                }
                rs->written += sf_writef_float(rs->sf, rec_interleaved, (sf_count_t)av);
            }
        }
    }

    sf_close(rs->sf);
    rs->sf = NULL;

    if (t && rs->written > 0) {
        RecordFinalize *rf = g_new0(RecordFinalize, 1);
        rf->track = g_object_ref(t);
        g_strlcpy(rf->path, rs->path, sizeof(rf->path));
        rf->punch          = rs->punch;
        rf->punch_tl_start = rs->punch_tl_start;
        /* Overwrite exactly the span we captured: on an early stop (punch-out
         * never reached) only the recorded portion is replaced, leaving the rest
         * of the old audio in the region intact. */
        rf->punch_tl_end   = rs->punch_tl_start + rs->written;
        g_idle_add(recorder_finalize_idle, rf);
    }
    rs->written = 0;
    rs->punch   = 0;
}

static void *recorder_thread_func(void *arg)
{
    (void)arg;
    guint i;
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 2000000 }; /* 2 ms */

    while (!recorder_stop_flag) {
        nanosleep(&ts, NULL);

        for (i = 0; i < JACKDAW_MAX_TRACKS; i++) {
            /* Main thread signals finalize when recording stops */
            if (g_atomic_int_compare_and_exchange(&recorder_slots[i].finalize_req, 1, 0)) {
                recorder_slot_finalize(i);
                continue;
            }

            RecorderSlot *rs = &recorder_slots[i];
            if (!rs->sf) continue;

            JackDawTrack *t = engine.slots[i];
            if (!t || !t->rec_buf_L || !t->rec_buf_R) continue;

            while (TRUE) {
                size_t avL = jack_ringbuffer_read_space(t->rec_buf_L) / sizeof(float);
                size_t avR = jack_ringbuffer_read_space(t->rec_buf_R) / sizeof(float);
                size_t av  = avL < avR ? avL : avR;
                if (av > REC_SCRATCH_FRAMES) av = REC_SCRATCH_FRAMES;
                /* Cap at expected_frames if stop has been signalled */
                if (rs->expected_frames > 0) {
                    off_t rem = rs->expected_frames - rs->written;
                    if (rem <= 0) break;
                    if ((off_t)av > rem) av = (size_t)rem;
                }
                if (av == 0) break;

                jack_ringbuffer_read(t->rec_buf_L, (char *)rec_scratch_L, av * sizeof(float));
                jack_ringbuffer_read(t->rec_buf_R, (char *)rec_scratch_R, av * sizeof(float));

                if (rs->channels == 1) {
                    rs->written += sf_writef_float(rs->sf, rec_scratch_L, (sf_count_t)av);
                } else {
                    for (size_t f = 0; f < av; f++) {
                        rec_interleaved[f * 2]     = rec_scratch_L[f];
                        rec_interleaved[f * 2 + 1] = rec_scratch_R[f];
                    }
                    rs->written += sf_writef_float(rs->sf, rec_interleaved, (sf_count_t)av);
                }
            }
        }
    }

    /* On thread exit: close any still-open files (engine quit path) */
    for (i = 0; i < JACKDAW_MAX_TRACKS; i++) {
        if (recorder_slots[i].sf) {
            sf_close(recorder_slots[i].sf);
            recorder_slots[i].sf = NULL;
        }
    }
    return NULL;
}

static void recorder_start(void)
{
    if (recorder_started) return;

    rec_scratch_L   = g_new(float, REC_SCRATCH_FRAMES);
    rec_scratch_R   = g_new(float, REC_SCRATCH_FRAMES);
    rec_interleaved = g_new(float, REC_SCRATCH_FRAMES * 2);

    recorder_stop_flag = 0;
    memset(recorder_slots, 0, sizeof(recorder_slots));

    if (pthread_create(&recorder_tid, NULL, recorder_thread_func, NULL) != 0) {
        g_free(rec_scratch_L);
        g_free(rec_scratch_R);
        g_free(rec_interleaved);
        rec_scratch_L = rec_scratch_R = rec_interleaved = NULL;
        return;
    }
    recorder_started = TRUE;
}

static void recorder_stop(void)
{
    if (!recorder_started) return;
    recorder_stop_flag = 1;
    pthread_join(recorder_tid, NULL);
    recorder_started = FALSE;
    g_free(rec_scratch_L);   rec_scratch_L   = NULL;
    g_free(rec_scratch_R);   rec_scratch_R   = NULL;
    g_free(rec_interleaved); rec_interleaved = NULL;
}

/* -----------------------------------------------------------------------
 * JACK process callback — RT thread, no malloc/free/mutex/file I/O
 * ----------------------------------------------------------------------- */

/* ---- Instrument-track MIDI scheduling (RT) ----
 * Per-slot active-note bookkeeping so a stop/seek can release sounding notes
 * (no stuck notes), and a flush request set from the main thread. */
#define ENG_MIDI_MAX_EV 1024
static guint8        eng_active_notes[JACKDAW_MAX_TRACKS][16][128];
static volatile gint eng_midi_flush[JACKDAW_MAX_TRACKS];   /* 1 = all-notes-off */

/* This cycle's merged MIDI block per slot: flushed note-offs + sequenced clip
 * events + live thru + preview, sorted by time. Gathered once by the worker
 * that owns the slot, then consumed twice — fed to the track's instrument
 * plugin, and written to the track's JACK MIDI output port by the JACK thread
 * after the barrier, so external hardware hears the same notes as the plugin.
 * Preallocated: nothing here is sized at RT time. */
static PhMidiEvent   eng_block_ev[JACKDAW_MAX_TRACKS][ENG_MIDI_MAX_EV];
static int           eng_block_nev[JACKDAW_MAX_TRACKS];

/* ---- Preview-note injection (main thread -> RT) ----
 * The main thread queues short MIDI messages tagged with a track slot; the RT
 * thread drains the ring once per cycle into per-slot scratch, and the gather
 * function emits them at block offset 0. Lock-free SPSC via jack_ringbuffer. */
#define ENG_PREVIEW_MAX 32                    /* per-slot events drained per cycle */
typedef struct { gint32 slot; guint8 data[3]; } EngPrevMsg;
static jack_ringbuffer_t *eng_preview_rb;     /* SPSC: main -> RT */
static jack_ringbuffer_t *eng_control_rb;     /* SPSC: RT -> main (control surface) */
static guint8 eng_preview_data[JACKDAW_MAX_TRACKS][ENG_PREVIEW_MAX][3];
static int    eng_preview_n[JACKDAW_MAX_TRACKS];

/* One recorded MIDI event: absolute timeline frame + up to 3 bytes. Written by
 * the RT thread to t->midi_rec_buf, drained on the main thread when recording
 * stops (midi_finalize_idle) and turned into clip notes. */
typedef struct { gint64 frame; guint8 size; guint8 data[3]; } MidiRecEvent;
/* Transport frame at the last stop — closes notes still held when recording ends. */
static volatile off_t eng_midi_rec_cut;

static int eng_midi_cmp(const void *a, const void *b)
{
    const PhMidiEvent *ea = a, *eb = b;
    if (ea->time < eb->time) return -1;
    if (ea->time > eb->time) return  1;
    return 0;
}

/* Build this block's MIDI events for an instrument track: stop/seek flush
 * note-offs, then sequenced events from the immutable snapshot (while playing),
 * then live JACK MIDI input (while armed). Tracks sounding notes per slot. */
static int eng_gather_instrument_midi(int slot, JackDawTrack *t, off_t blk_start,
                                      jack_nframes_t nframes, gboolean playing,
                                      gboolean armed, PhMidiEvent *mev, int cap)
{
    int nev = 0;

    if (g_atomic_int_compare_and_exchange(&eng_midi_flush[slot], 1, 0)) {
        for (int ch = 0; ch < 16; ch++)
            for (int p = 0; p < 128; p++)
                if (eng_active_notes[slot][ch][p] && nev < cap) {
                    mev[nev].time = 0; mev[nev].size = 3;
                    mev[nev].data[0] = (guint8)(0x80 | ch);
                    mev[nev].data[1] = (guint8)p; mev[nev].data[2] = 0;
                    nev++;
                    eng_active_notes[slot][ch][p] = 0;
                }
    }

    if (playing) {
        MidiEventSnapshot *ms = g_atomic_pointer_get(&t->rt_midi);
        if (ms && ms->n) {
            off_t end = blk_start + nframes;
            guint lo = 0, hi = ms->n;             /* lower_bound(blk_start) */
            while (lo < hi) {
                guint mid = (lo + hi) / 2;
                if (ms->ev[mid].frame < blk_start) lo = mid + 1; else hi = mid;
            }
            for (guint e = lo; e < ms->n && ms->ev[e].frame < end && nev < cap; e++) {
                MidiSnapEvent *se = &ms->ev[e];
                mev[nev].time = (guint32)(se->frame - blk_start);
                mev[nev].size = 3;
                mev[nev].data[0] = se->s; mev[nev].data[1] = se->d1; mev[nev].data[2] = se->d2;
                int ch = se->s & 0x0F, p = se->d1 & 0x7F;
                if ((se->s & 0xF0) == 0x90 && se->d2 > 0) eng_active_notes[slot][ch][p] = 1;
                else if ((se->s & 0xF0) == 0x80)          eng_active_notes[slot][ch][p] = 0;
                nev++;
            }
        }
    }

    if (armed && t->midi_in_idx >= 0 &&
        (guint)t->midi_in_idx < engine.midi_in_count && engine.midi_in[t->midi_in_idx]) {
        void *mbuf = jack_port_get_buffer(engine.midi_in[t->midi_in_idx], nframes);
        uint32_t mc = jack_midi_get_event_count(mbuf);
        for (uint32_t m = 0; m < mc && nev < cap; m++) {
            jack_midi_event_t ev;
            if (jack_midi_event_get(&ev, mbuf, m) != 0 || ev.size < 1) continue;
            /* Drop realtime messages (clock, start/stop, active sensing, reset).
             * These get echoed straight back out midi_out below, and the common
             * wiring has midi_out looped to the same device the events came
             * from, which feeds the source its own clock. */
            if (ev.buffer[0] >= 0xF8) continue;
            mev[nev].time = ev.time;
            mev[nev].size = (guint8)(ev.size > 3 ? 3 : ev.size);
            mev[nev].data[0] = ev.buffer[0];
            mev[nev].data[1] = ev.size > 1 ? ev.buffer[1] : 0;
            mev[nev].data[2] = ev.size > 2 ? ev.buffer[2] : 0;
            int st = ev.buffer[0] & 0xF0, ch = ev.buffer[0] & 0x0F, p = mev[nev].data[1] & 0x7F;
            if (st == 0x90 && mev[nev].data[2] > 0) eng_active_notes[slot][ch][p] = 1;
            else if (st == 0x80)                    eng_active_notes[slot][ch][p] = 0;
            nev++;
        }
    }

    /* Preview notes queued from the main thread (piano-roll keyboard) — emitted
     * at block start and tracked so a later flush releases them. */
    if (slot >= 0 && slot < JACKDAW_MAX_TRACKS) {
        for (int pi = 0; pi < eng_preview_n[slot] && nev < cap; pi++) {
            guint8 *d = eng_preview_data[slot][pi];
            mev[nev].time = 0; mev[nev].size = 3;
            mev[nev].data[0] = d[0]; mev[nev].data[1] = d[1]; mev[nev].data[2] = d[2];
            int st = d[0] & 0xF0, ch = d[0] & 0x0F, p = d[1] & 0x7F;
            if (st == 0x90 && d[2] > 0) eng_active_notes[slot][ch][p] = 1;
            else if (st == 0x80)        eng_active_notes[slot][ch][p] = 0;
            nev++;
        }
    }

    if (nev > 1) qsort(mev, nev, sizeof(PhMidiEvent), eng_midi_cmp);
    return nev;
}

/* Enable flush-to-zero + denormals-are-zero on the calling thread's SSE unit.
 * Cheap (two MXCSR register writes); safe to call every cycle. Re-armed before
 * every plugin in pluginhost_process() too, in case a plugin clears MXCSR. */
static inline void engine_rt_set_denormal_mode(void)
{
    rt_set_denormal_mode();
}

/* JACK thread-init callback: runs once per RT thread JACK spawns, before any
 * process cycle. Covers auxiliary RT worker threads as well as the main one. */
static void engine_thread_init_cb(void *arg)
{
    (void)arg;
    engine_rt_set_denormal_mode();
}

/* -----------------------------------------------------------------------
 * Parallel track processing
 *
 * Track FX chains are independent of each other; only the master sum depends on
 * all of them. So each cycle the JACK RT thread fans the tracks out across a
 * pool of RT-priority worker threads (work-stealing), joins on a barrier, then
 * sums each track's output into the master bus and runs the master chain. This
 * is what lets two heavy amp-sims on two tracks run on two cores at once instead
 * of serially on one (the difference between fitting the deadline and not).
 *
 * RT-safety: workers only ever touch ONE track's state at a time (its own
 * ringbuffers, plugins, scratch and peak fields — never shared), so no locks are
 * needed beyond the per-cycle go/done semaphores. jack_port_get_buffer() is
 * called only on the JACK thread (the pre-fetch pass below); workers use the
 * cached pointers. Denormal flush is re-armed per plugin in pluginhost_process.
 * ----------------------------------------------------------------------- */
#define RT_MAX_WORKERS 64
static jack_native_thread_t g_rt_worker[RT_MAX_WORKERS];
static int   g_rt_nworkers = 0;        /* 0 = serial (RT thread does all tracks) */
static sem_t g_rt_sem_go;              /* main posts N; each worker waits one */
static sem_t g_rt_sem_done;            /* each worker posts one; main waits N */
static volatile gint g_rt_workers_quit = 0;

/* Cycle parameters published to the workers (set before the barrier opens; the
 * semaphores provide the memory barrier). */
static volatile jack_nframes_t g_rt_nframes;
static volatile gint   g_rt_flags;
static volatile off_t  g_rt_blk_start;
static volatile gint   g_rt_any_soloed;
static volatile gint   g_rt_task_next;   /* next index into g_rt_task_slot */

/* This cycle's occupied slots, compacted. Stealing used to walk all
 * JACKDAW_MAX_TRACKS indices to find the few that hold a track, so a one-track
 * project still ran up to 64 contended atomic increments per worker per cycle
 * against one cache line, plus a full pool wake — pure dispatch overhead
 * measured in the same order as the audio work itself. */
static int             g_rt_task_slot[JACKDAW_MAX_TRACKS];
static volatile gint   g_rt_task_count;

/* Monotonic RT cycle counter, incremented once per process callback. Used off
 * the RT thread to establish that the audio thread has moved past a retired
 * object (see jackdaw_track_fx_collect). Wraps harmlessly: readers compare
 * unsigned differences. */
static volatile gint   g_rt_cycle;

/* Port buffers pre-fetched on the JACK thread (only it may call
 * jack_port_get_buffer); workers read these cached pointers. */
static float *g_slot_live_L[JACKDAW_MAX_TRACKS];
static float *g_slot_live_R[JACKDAW_MAX_TRACKS];
static void  *g_slot_midi_buf[JACKDAW_MAX_TRACKS];

/* Process one track fully (drain/instrument, live monitor, FX chain, fader/pan,
 * metering, capture) into engine.slot_L[i]/slot_R[i]. Reads cycle params from
 * the g_rt_* publish slots. Runs on the JACK thread or a worker. */
static void engine_process_track(int i)
{
    JackDawTrack *t = engine.slots[i];
    if (!t) return;

    jack_nframes_t nframes = g_rt_nframes;
    gint   flags     = g_rt_flags;
    off_t  blk_start = g_rt_blk_start;
    gboolean any_soloed = g_rt_any_soloed != 0;
    jack_nframes_t k;
    size_t want = nframes * sizeof(float);

    float *bL = engine.slot_L[i];
    float *bR = engine.slot_R[i];

    gint tflags = g_atomic_int_get(&t->state_flags);
    gboolean muted = (tflags & TRACK_MUTED) ||
                     (any_soloed && !(tflags & TRACK_SOLOED));
    gboolean instr = jackdaw_track_is_instrument(t);

    if (instr) {
        memset(bL, 0, want);
        memset(bR, 0, want);
    } else {
        size_t got_L = 0, got_R = 0;
        if (t->play_buf_L && (flags & ENGINE_PLAYING))
            got_L = jack_ringbuffer_read(t->play_buf_L, (char *)bL, want);
        if (t->play_buf_R && (flags & ENGINE_PLAYING))
            got_R = jack_ringbuffer_read(t->play_buf_R, (char *)bR, want);
        if (got_L < want) memset((char *)bL + got_L, 0, want - got_L);
        if (got_R < want) memset((char *)bR + got_R, 0, want - got_R);
    }

    /* Live input monitor (pre-fetched pointers). */
    float *live_L = g_slot_live_L[i];
    float *live_R = g_slot_live_R[i];
    if (live_L && !live_R) live_R = live_L;   /* mono input → duplicate */
    if (live_L && (flags & ENGINE_RECORDING)) {
        memset(bL, 0, want);
        memset(bR, 0, want);
    }
    if (live_L) {
        gfloat wf_mn = 0.0f, wf_mx = 0.0f;
        for (k = 0; k < nframes; k++) {
            float sl = live_L[k], sr = live_R[k];
            bL[k] += sl;
            bR[k] += sr;
            if (sl < wf_mn) wf_mn = sl;
            if (sl > wf_mx) wf_mx = sl;
            if (sr < wf_mn) wf_mn = sr;
            if (sr > wf_mx) wf_mx = sr;
        }
        if ((flags & ENGINE_RECORDING) && t->rec_peak_buf) {
            gint pk = t->rec_peak_count;
            if (pk < REC_PEAK_MAX_BUCKETS) {
                t->rec_peak_buf[pk * 2]     = wf_mn;
                t->rec_peak_buf[pk * 2 + 1] = wf_mx;
                t->rec_peak_count = pk + 1;
            }
        }
    }

    /* Per-track FX chain (in place on bL/bR). */
    JackDawFxChain *chain = g_atomic_pointer_get(&t->rt_chain);
    if (instr) {
        /* Gathered into per-slot storage rather than a local, so the JACK thread
         * can also write this block to the track's MIDI output port after the
         * worker barrier (see the midi_out loop in engine_process). Each worker
         * owns exactly one slot per cycle, so there is no sharing. */
        PhMidiEvent *mev = eng_block_ev[i];
        int nev = eng_gather_instrument_midi(i, t, blk_start, nframes,
                                             (flags & ENGINE_PLAYING) != 0,
                                             (tflags & TRACK_ARMED) != 0,
                                             mev, ENG_MIDI_MAX_EV);
        eng_block_nev[i] = nev;
        if (chain && chain->n > 0) {
            pluginhost_process_midi((PluginInstance *)chain->fx[0], mev, nev,
                                    bL, bR, (int)nframes);
            for (int fi = 1; fi < chain->n; fi++)
                pluginhost_process((PluginInstance *)chain->fx[fi], bL, bR,
                                   (int)nframes);
        }
    } else if (chain) {
        for (int fi = 0; fi < chain->n; fi++)
            pluginhost_process((PluginInstance *)chain->fx[fi], bL, bR,
                               (int)nframes);
    }

    /* Fold this cycle's per-plugin timings into the diag table here, on the RT
     * thread that just produced them, while the chain snapshot is provably
     * alive. The diag thread used to walk t->rt_chain itself and call into each
     * PluginInstance — but an FX remove can retire and free those instances at
     * any moment, so that read raced a use-after-free. It now touches only
     * atomics in this table. */
    if (g_diag_on && chain) {
        for (int di = 0; di < chain->n; di++) {
            gint us = (gint)pluginhost_diag_take_max_us(
                (PluginInstance *)chain->fx[di]);
            int fs = (di < ENG_DIAG_FX_SLOTS) ? di : ENG_DIAG_FX_SLOTS - 1;
            if (us > g_atomic_int_get(&g_diag_fx_us[i][fs]))
                g_atomic_int_set(&g_diag_fx_us[i][fs], us);
        }
    }

    /* Constant-power pan + fader, written in place; meter post-fader. */
    gfloat vol   = muted ? 0.0f : t->volume;
    float  angle = (t->pan + 1.0f) * (float)M_PI_4;
    float  gain_L = vol * cosf(angle);
    float  gain_R = vol * sinf(angle);
    gfloat peak_L = 0.0f, peak_R = 0.0f;
    for (k = 0; k < nframes; k++) {
        float sL = bL[k] * gain_L;
        float sR = bR[k] * gain_R;
        bL[k] = sL;
        bR[k] = sR;
        if (sL < 0.0f) sL = -sL;
        if (sR < 0.0f) sR = -sR;
        if (sL > peak_L) peak_L = sL;
        if (sR > peak_R) peak_R = sR;
    }
    t->peak_L = (peak_L > t->peak_L) ? peak_L : t->peak_L * 0.92f;
    t->peak_R = (peak_R > t->peak_R) ? peak_R : t->peak_R * 0.92f;

    /* Capture dry input to rec ringbuffers while recording. */
    if (live_L && (flags & ENGINE_RECORDING)) {
        if (t->rec_buf_L)
            jack_ringbuffer_write(t->rec_buf_L, (const char *)live_L, want);
        if (t->rec_buf_R)
            jack_ringbuffer_write(t->rec_buf_R, (const char *)live_R, want);
    }

    /* Record MIDI with absolute timeline frames (instrument tracks). */
    if (instr && (tflags & TRACK_ARMED) && (flags & ENGINE_RECORDING) &&
        g_slot_midi_buf[i] && t->midi_rec_buf) {
        void *mbuf = g_slot_midi_buf[i];
        uint32_t mc = jack_midi_get_event_count(mbuf);
        for (uint32_t m = 0; m < mc; m++) {
            jack_midi_event_t ev;
            if (jack_midi_event_get(&ev, mbuf, m) != 0 || ev.size < 1) continue;
            if (!(ev.buffer[0] & 0x80)) continue;
            MidiRecEvent r;
            r.frame   = (gint64)(blk_start + (off_t)ev.time);
            r.size    = (guint8)(ev.size > 3 ? 3 : ev.size);
            r.data[0] = ev.buffer[0];
            r.data[1] = ev.size > 1 ? ev.buffer[1] : 0;
            r.data[2] = ev.size > 2 ? ev.buffer[2] : 0;
            if (jack_ringbuffer_write_space(t->midi_rec_buf) >= sizeof r)
                jack_ringbuffer_write(t->midi_rec_buf, (const char *)&r, sizeof r);
        }
    }
}

/* Work-stealing runner: claim the next slot index and process it until none
 * remain. Called by both the JACK thread and every worker. */
static void rt_run_tasks(void)
{
    int idx;
    const int n = g_atomic_int_get(&g_rt_task_count);
    while ((idx = g_atomic_int_add(&g_rt_task_next, 1)) < n)
        engine_process_track(g_rt_task_slot[idx]);
}

static void rt_sem_wait(sem_t *s)
{
    while (sem_wait(s) != 0 && errno == EINTR) { /* retry */ }
}

static void *rt_worker_main(void *arg)
{
    (void)arg;
    rt_set_denormal_mode();
    for (;;) {
        rt_sem_wait(&g_rt_sem_go);
        if (g_atomic_int_get(&g_rt_workers_quit)) break;
        rt_run_tasks();
        sem_post(&g_rt_sem_done);
    }
    return NULL;
}

static int engine_process(jack_nframes_t nframes, void *arg)
{
    (void)arg;
    guint i;
    gint flags;
    gboolean any_soloed = FALSE;
    float *port_buf;
    jack_nframes_t k;

    /* One increment per cycle. Off-RT code reads this to prove the audio thread
     * has moved past a retired FX chain before freeing the instances in it. */
    g_atomic_int_inc((gint *)&g_rt_cycle);

    /* Diagnostics: mark this thread as the RT callback (so the VST3 host context
     * can flag plugins that allocate here) and time the whole cycle. */
    ph_rt_mark(1);
    gint64 _diag_t0 = 0;
    if (g_diag_on) {
        struct timespec _ts; clock_gettime(CLOCK_MONOTONIC, &_ts);
        _diag_t0 = (gint64)_ts.tv_sec * 1000000 + _ts.tv_nsec / 1000;
    }

    /* Flush denormals to zero on this RT thread (belt; thread-init cb is the
     * suspenders). Guards against plugin-generated subnormals stalling the CPU. */
    engine_rt_set_denormal_mode();

    /* Offline render in progress: the render worker owns every PluginInstance,
     * so the live graph must touch none of them. Output silence, freeze the
     * transport, and return before any plugin or mix work. */
    if (g_atomic_int_get(&engine.render_suspend)) {
        for (i = 0; i < engine.audio_out_count; i++) {
            if (!engine.audio_out[i]) continue;
            port_buf = jack_port_get_buffer(engine.audio_out[i], nframes);
            memset(port_buf, 0, nframes * sizeof(float));
        }
        for (i = 0; i < engine.midi_out_count; i++) {
            if (!engine.midi_out[i]) continue;
            jack_midi_clear_buffer(
                jack_port_get_buffer(engine.midi_out[i], nframes));
        }
        if (engine.metro_out) {
            port_buf = jack_port_get_buffer(engine.metro_out, nframes);
            memset(port_buf, 0, nframes * sizeof(float));
        }
        ph_rt_mark(0);
        return 0;
    }

    /* Clear master mix buffers */
    memset(engine.master_L, 0, nframes * sizeof(float));
    memset(engine.master_R, 0, nframes * sizeof(float));

    flags = g_atomic_int_get(&engine.transport_flags);

    /* Count-in pre-roll: a metronome-only lead-in. While active the project is
     * frozen (play_pos does not advance) and nothing records; the metronome
     * clicks from countin_pos. When the pre-roll length elapses the pending
     * transport (PLAYING, optionally RECORDING) engages and normal play begins
     * this block from the unchanged play_pos. Period-granular, like loop/punch. */
    gboolean preroll = FALSE;
    off_t    preroll_base = 0;
    if (g_atomic_int_get(&engine.countin_active)) {
        if (engine.countin_pos >= engine.countin_len) {
            gint pend = g_atomic_int_get(&engine.countin_pending_rec);
            g_atomic_int_set(&engine.countin_active, 0);
            g_atomic_int_or(&engine.transport_flags,
                            ENGINE_PLAYING | (pend ? ENGINE_RECORDING : 0));
            flags = g_atomic_int_get(&engine.transport_flags);
        } else {
            preroll      = TRUE;
            preroll_base = engine.countin_pos;
            engine.countin_pos += (off_t)nframes;
        }
    }

    if (flags & ENGINE_PLAYING)
        engine.play_pos += nframes;

    /* Drain main-thread preview-note requests into per-slot scratch for this
     * cycle (reset counts first; each event is delivered exactly once). */
    for (i = 0; i < JACKDAW_MAX_TRACKS; i++) eng_preview_n[i] = 0;
    if (eng_preview_rb) {
        EngPrevMsg msg;
        while (jack_ringbuffer_read_space(eng_preview_rb) >= sizeof msg) {
            jack_ringbuffer_read(eng_preview_rb, (char *)&msg, sizeof msg);
            if (msg.slot >= 0 && msg.slot < JACKDAW_MAX_TRACKS &&
                eng_preview_n[msg.slot] < ENG_PREVIEW_MAX) {
                guint8 *d = eng_preview_data[msg.slot][eng_preview_n[msg.slot]++];
                d[0] = msg.data[0]; d[1] = msg.data[1]; d[2] = msg.data[2];
            }
        }
    }

    /* Drain the dedicated control-surface input into the RT->main ring; the main
     * thread interprets the mappings. RT side only does a bounded lock-free
     * write (drops on overflow) — no malloc/lock/log here. */
    if (engine.control_in && eng_control_rb) {
        void *cbuf = jack_port_get_buffer(engine.control_in, nframes);
        uint32_t cn = jack_midi_get_event_count(cbuf);
        for (uint32_t c = 0; c < cn; c++) {
            jack_midi_event_t ev;
            if (jack_midi_event_get(&ev, cbuf, c) != 0 || ev.size < 1) continue;
            JackDawCtlEvent cm;
            cm.size    = (guint8)(ev.size > 3 ? 3 : ev.size);
            cm.data[0] = ev.buffer[0];
            cm.data[1] = ev.size > 1 ? ev.buffer[1] : 0;
            cm.data[2] = ev.size > 2 ? ev.buffer[2] : 0;
            if (jack_ringbuffer_write_space(eng_control_rb) >= sizeof cm)
                jack_ringbuffer_write(eng_control_rb, (const char *)&cm, sizeof cm);
        }
    }

    /* Loop wrap (master clock — drives MIDI scheduling, metronome, plugin
     * transport and the UI playhead). Only engages when this block started
     * inside the region: blk_start in [loop_start, loop_end). A playhead placed
     * after the region therefore plays straight through without looping. The
     * remainder past loop_end is carried over so the clock's loop period equals
     * the region length, matching the feeder. Checked at block granularity, so
     * the loop point quantizes to the JACK period (acceptable for now). */
    if ((flags & ENGINE_PLAYING) && g_atomic_int_get(&engine.loop_enabled)) {
        off_t l_start = engine.loop_start;
        off_t l_end   = engine.loop_end;
        off_t bstart  = engine.play_pos - (off_t)nframes;
        if (l_end > l_start && bstart >= l_start && bstart < l_end &&
            engine.play_pos >= l_end)
            engine.play_pos = l_start + (engine.play_pos - l_end);
    }

    /* Punch in/out (independent of looping): auto-engage recording over the tab
     * region while a punch is armed. Same block-granular crossing test as the
     * loop wrap; punch_armed gates it so normal recording is never affected. The
     * local `flags` is updated in step so the capture passes below act this block. */
    if ((flags & ENGINE_PLAYING) && g_atomic_int_get(&engine.punch_armed)) {
        off_t ls = engine.loop_start;
        off_t le = engine.loop_end;
        off_t bstart = engine.play_pos - (off_t)nframes;
        if (le > ls) {
            if (!(flags & ENGINE_RECORDING)) {
                if (engine.play_pos > ls && bstart < le) {   /* crossed into region */
                    g_atomic_int_or(&engine.transport_flags, ENGINE_RECORDING);
                    flags |= ENGINE_RECORDING;
                }
            } else if (engine.play_pos >= le) {              /* reached region end */
                g_atomic_int_and(&engine.transport_flags, ~ENGINE_RECORDING);
                flags &= ~ENGINE_RECORDING;
                for (guint p = 0; p < JACKDAW_MAX_TRACKS; p++)
                    if (recorder_slots[p].sf && recorder_slots[p].punch)
                        g_atomic_int_set(&recorder_slots[p].finalize_req, 1);
                g_atomic_int_set(&engine.punch_armed, 0);
            }
        }
    }

    /* Block start frame + transport for plugins that query host time (VST2
     * audioMasterGetTime, VST3 processContext). play_pos was just advanced. */
    off_t blk_start = (flags & ENGINE_PLAYING) ? engine.play_pos - (off_t)nframes
                                               : engine.play_pos;
    {
        double bpm = (engine.project && engine.project->bpm > 0.0)
                         ? engine.project->bpm : 120.0;
        pluginhost_set_transport(bpm, (double)engine.sample_rate,
                                 (gint64)blk_start, (flags & ENGINE_PLAYING) != 0);
    }

    /* First pass: check for any soloed track */
    for (i = 0; i < JACKDAW_MAX_TRACKS; i++) {
        JackDawTrack *t = engine.slots[i];
        if (!t) continue;
        if (g_atomic_int_get(&t->state_flags) & TRACK_SOLOED) {
            any_soloed = TRUE;
            break;
        }
    }

    /* Second pass: pre-fetch each armed track's input port buffers on THIS (the
     * JACK) thread — jack_port_get_buffer must not be called from the workers —
     * then fan the tracks out across the worker pool and sum the results.
     *
     * Pre-fetch: live audio input (for monitoring + dry capture) and the MIDI
     * input buffer (for MIDI recording). Mirrors the gating that used to live in
     * the per-track loop. */
    for (i = 0; i < JACKDAW_MAX_TRACKS; i++) {
        g_slot_live_L[i]   = NULL;
        g_slot_live_R[i]   = NULL;
        g_slot_midi_buf[i] = NULL;
        JackDawTrack *t = engine.slots[i];
        if (!t) continue;
        gint tflags = g_atomic_int_get(&t->state_flags);
        gboolean instr = jackdaw_track_is_instrument(t);
        if (!instr && (tflags & TRACK_ARMED) && t->audio_in_idx >= 0 &&
            (guint)t->audio_in_idx < engine.audio_in_count &&
            engine.audio_in[(guint)t->audio_in_idx]) {
            g_slot_live_L[i] = jack_port_get_buffer(
                engine.audio_in[(guint)t->audio_in_idx], nframes);
            if (t->audio_src_port_r && engine.audio_in_r[(guint)t->audio_in_idx])
                g_slot_live_R[i] = jack_port_get_buffer(
                    engine.audio_in_r[(guint)t->audio_in_idx], nframes);
        }
        if (instr && (tflags & TRACK_ARMED) && (flags & ENGINE_RECORDING) &&
            t->midi_in_idx >= 0 &&
            (guint)t->midi_in_idx < engine.midi_in_count &&
            engine.midi_in[t->midi_in_idx] && t->midi_rec_buf)
            g_slot_midi_buf[i] = jack_port_get_buffer(
                engine.midi_in[t->midi_in_idx], nframes);
    }

    /* Publish cycle params and dispatch. The JACK thread participates as a
     * worker (work-stealing), so a single core still does all the work when no
     * worker threads are configured. */
    g_rt_nframes    = nframes;
    g_rt_flags      = flags;
    g_rt_blk_start  = blk_start;
    g_rt_any_soloed = any_soloed ? 1 : 0;
    /* Clear every slot's MIDI block up front; only instrument tracks refill it,
     * so a slot that is empty or non-instrument this cycle writes nothing out. */
    for (i = 0; i < JACKDAW_MAX_TRACKS; i++) eng_block_nev[i] = 0;

    /* Compact this cycle's occupied slots so stealing scans only real work. */
    int ntasks = 0;
    for (i = 0; i < JACKDAW_MAX_TRACKS; i++)
        if (engine.slots[i]) g_rt_task_slot[ntasks++] = (int)i;
    g_atomic_int_set(&g_rt_task_count, ntasks);
    g_atomic_int_set(&g_rt_task_next, 0);

    /* Wake only as many workers as there is parallel work for: the JACK thread
     * takes one track itself, so N tracks need at most N-1 helpers. Waking the
     * whole pool for a one- or two-track project cost two semaphore round-trips
     * per worker per cycle and had every woken thread contend for a task list it
     * would find empty — enough wall-clock to push a heavy plugin past its
     * deadline even though the CPU work was unchanged. With one track nobody is
     * woken and the barrier is skipped entirely. */
    int wake = ntasks - 1;
    if (wake > g_rt_nworkers) wake = g_rt_nworkers;
    if (wake < 0)             wake = 0;

    if (wake > 0) {
        for (int w = 0; w < wake; w++) sem_post(&g_rt_sem_go);
        rt_run_tasks();
        for (int w = 0; w < wake; w++) rt_sem_wait(&g_rt_sem_done);
    } else {
        rt_run_tasks();
    }

    /* Sum each processed track's post-fader contribution into the master. */
    for (i = 0; i < JACKDAW_MAX_TRACKS; i++) {
        JackDawTrack *t = engine.slots[i];
        if (!t) continue;
        const float *sl = engine.slot_L[i];
        const float *sr = engine.slot_R[i];
        for (k = 0; k < nframes; k++) {
            engine.master_L[k] += sl[k];
            engine.master_R[k] += sr[k];
        }
    }


    /* Master bus track: run its FX chain in place on the summed mix, then take
     * the master gain (and mute) from the master track. Audio track, so the
     * whole chain is effects (no instrument at index 0). */
    JackDawTrack *mt = engine.project ? engine.project->master_track : NULL;
    gfloat mvol = engine.project ? engine.project->master_volume : 1.0f;
    if (mt) {
        JackDawFxChain *mchain = g_atomic_pointer_get(&mt->rt_chain);
        if (mchain) {
            for (int fi = 0; fi < mchain->n; fi++)
                pluginhost_process((PluginInstance *)mchain->fx[fi],
                                   engine.master_L, engine.master_R, (int)nframes);
        }
        mvol = mt->volume;
        if (g_atomic_int_get(&mt->state_flags) & TRACK_MUTED) mvol = 0.0f;
    }
    gfloat mpk_L = 0.0f, mpk_R = 0.0f;
    guint oi;
    for (oi = 0; oi < engine.audio_out_count; oi++) {
        if (!engine.audio_out[oi]) continue;
        port_buf = jack_port_get_buffer(engine.audio_out[oi], nframes);
        if (oi == 0) {
            for (k = 0; k < nframes; k++) {
                float s = engine.master_L[k] * mvol;
                port_buf[k] = s;
                if (s < 0.0f) s = -s;
                if (s > mpk_L) mpk_L = s;
            }
        } else if (oi == 1) {
            for (k = 0; k < nframes; k++) {
                float s = engine.master_R[k] * mvol;
                port_buf[k] = s;
                if (s < 0.0f) s = -s;
                if (s > mpk_R) mpk_R = s;
            }
        } else {
            memset(port_buf, 0, nframes * sizeof(float));
        }
    }
    if (mpk_L > engine.master_peak_L) engine.master_peak_L = mpk_L;
    if (mpk_R > engine.master_peak_R) engine.master_peak_R = mpk_R;

    /* Realtime render tap: copy the exact post-fader master we just monitored
     * into the render rings for the writer thread (lock-free, no file I/O in
     * the RT path). Signal completion once the playhead reaches the end. */
    if (g_atomic_int_get(&engine.render_active) &&
        engine.render_rb_L && engine.render_rb_R) {
        /* Capture only the frames of this block that fall before render_end, so
         * the rendered file ends exactly at the requested point (the transport
         * keeps running until the main thread stops it ~one tick later). */
        off_t blk_start = engine.play_pos - (off_t)nframes;
        off_t in_range  = engine.render_end - blk_start;
        if (in_range < 0) in_range = 0;
        if (in_range > (off_t)nframes) in_range = (off_t)nframes;
        if (in_range > 0) {
            for (k = 0; k < (jack_nframes_t)in_range; k++) {
                engine.render_tap_L[k] = engine.master_L[k] * mvol;
                engine.render_tap_R[k] = engine.master_R[k] * mvol;
            }
            size_t bytes = (size_t)in_range * sizeof(float);
            if (jack_ringbuffer_write_space(engine.render_rb_L) >= bytes &&
                jack_ringbuffer_write_space(engine.render_rb_R) >= bytes) {
                jack_ringbuffer_write(engine.render_rb_L,
                                      (const char *)engine.render_tap_L, bytes);
                jack_ringbuffer_write(engine.render_rb_R,
                                      (const char *)engine.render_tap_R, bytes);
            }
        }
        if (engine.play_pos >= engine.render_end)
            g_atomic_int_set(&engine.render_done, 1);
    }

    /* Metronome click — monitored by mixing straight onto the audio outputs,
     * AFTER the master fader, the master FX chain, the master peak meter and the
     * render tap. The click is therefore audible but completely separate from
     * the project signal: it cannot raise the master meters, it never passes
     * through the master FX chain, and it is excluded from rendered/exported
     * files. The click is also independent of the master volume/mute.
     *
     * The dedicated "metronome" port always carries the click (a standalone feed
     * the user can route to a performer's headphones). metronome_route decides
     * whether the click ALSO bleeds into the main outs (MAIN) or stays on the
     * dedicated port only (CLICK_PORT = "headphones only"). The metro port is
     * cleared every cycle so it never plays stale buffer contents. */
    {
        float *metro_buf = engine.metro_out
            ? (float *)jack_port_get_buffer(engine.metro_out, nframes) : NULL;
        if (metro_buf) memset(metro_buf, 0, nframes * sizeof(float));

        /* The click sounds either for the normal metronome (playing + enabled) or
         * for a count-in pre-roll (always, regardless of the metronome toggle —
         * the count-in IS the click). The two cases differ only in the position
         * the beat grid is measured from. */
        gboolean play_click = (flags & ENGINE_PLAYING) && engine.project &&
            engine.project->metronome_enabled;
        if ((play_click || preroll) && engine.project && engine.click_buf &&
            engine.click_len > 0 && engine.project->bpm > 0.0) {
            gboolean to_main =
                engine.project->metronome_route == METRONOME_ROUTE_MAIN;
            double fpb = (double)engine.sample_rate * 60.0 / engine.project->bpm;
            guint  bpb = engine.project->beats_per_bar
                         ? engine.project->beats_per_bar : 4;
            float  click_gain = engine.project->metronome_gain;
            if (fpb > 1.0) {
                off_t  base = preroll ? preroll_base
                                      : engine.play_pos - (off_t)nframes;
                float *out_buf[2] = { NULL, NULL };
                if (to_main) {
                    for (oi = 0; oi < engine.audio_out_count && oi < 2; oi++)
                        if (engine.audio_out[oi])
                            out_buf[oi] = jack_port_get_buffer(
                                engine.audio_out[oi], nframes);
                }
                for (k = 0; k < nframes; k++) {
                    off_t a = base + (off_t)k;
                    if (a < 0) continue;
                    /* The pre-roll sounds exactly `beats` clicks: the click at
                     * the resolution point (count-in position countin_len)
                     * belongs to the project's first downbeat, which the
                     * hand-off cycle plays from play_pos. Sounding it here too
                     * produced a doubled click — an audible flam/pop right at
                     * the count-in -> transport transition. */
                    if (preroll && a >= engine.countin_len) break;
                    off_t beat     = (off_t)((double)a / fpb);
                    off_t boundary = (off_t)((double)beat * fpb + 0.5);
                    off_t off      = a - boundary;
                    if (off >= 0 && off < engine.click_len) {
                        float s = engine.click_buf[off] * click_gain;
                        if ((beat % (off_t)bpb) != 0) s *= 0.45f; /* accent downbeat */
                        if (metro_buf)  metro_buf[k]  += s;
                        if (out_buf[0]) out_buf[0][k] += s;
                        if (out_buf[1]) out_buf[1][k] += s;
                    }
                }
            }
        }
    }

    /* Clear all MIDI output buffers before any writes */
    for (oi = 0; oi < engine.midi_out_count; oi++) {
        if (!engine.midi_out[oi]) continue;
        void *mbuf = jack_port_get_buffer(engine.midi_out[oi], nframes);
        jack_midi_clear_buffer(mbuf);
    }

    /* MIDI output, midi_out_N, where N is the track's own input index so the
     * in_N/out_N pair belongs to one track. Two kinds of source, and a given
     * output port must only be written by one of them per block:
     *
     *  - Instrument tracks emit the whole block gathered above: sequenced clip
     *    notes, live thru, preview notes and any flushed note-offs, already
     *    merged and sorted by time. Without this, clip playback reached the
     *    track's instrument plugin and nothing else — an external synth or
     *    hardware module wired to midi_out heard silence.
     *  - Armed non-instrument tracks have no gathered block, so they still get
     *    the plain thru copy from midi_in_N.
     *
     * Several tracks may share one midi_in (same hardware source), so each
     * output index is written at most once per block; midi_in_count <= 16, so a
     * u32 mask covers every index. */
    guint32 out_done = 0;
    for (i = 0; i < JACKDAW_MAX_TRACKS; i++) {
        JackDawTrack *t = engine.slots[i];
        if (!t) continue;
        gint mi = t->midi_in_idx;
        if (mi < 0 || (guint)mi >= engine.midi_out_count || !engine.midi_out[mi])
            continue;
        if (out_done & (1u << mi)) continue;

        gboolean instr = jackdaw_track_is_instrument(t);
        gint tflags = g_atomic_int_get(&t->state_flags);
        if (!instr && !(tflags & TRACK_ARMED)) continue;

        void *obuf = jack_port_get_buffer(engine.midi_out[mi], nframes);
        out_done |= (1u << mi);

        if (instr) {
            for (int e = 0; e < eng_block_nev[i]; e++)
                jack_midi_event_write(obuf, eng_block_ev[i][e].time,
                                      eng_block_ev[i][e].data,
                                      eng_block_ev[i][e].size);
            continue;
        }

        if ((guint)mi >= engine.midi_in_count || !engine.midi_in[mi]) continue;
        void *ibuf = jack_port_get_buffer(engine.midi_in[mi], nframes);
        uint32_t mc = jack_midi_get_event_count(ibuf);
        uint32_t m;
        for (m = 0; m < mc; m++) {
            jack_midi_event_t ev;
            if (jack_midi_event_get(&ev, ibuf, m) != 0 || ev.size < 1) continue;
            /* Same realtime-message filter as the instrument path: the default
             * wiring loops midi_out back to the source device. */
            if (ev.buffer[0] >= 0xF8) continue;
            jack_midi_event_write(obuf, ev.time, ev.buffer, ev.size);
        }
    }

    if (_diag_t0) {
        struct timespec _ts; clock_gettime(CLOCK_MONOTONIC, &_ts);
        gint64 _t1 = (gint64)_ts.tv_sec * 1000000 + _ts.tv_nsec / 1000;
        gint64 _d = _t1 - _diag_t0;
        g_diag_cb_last_us = _d;
        if (_d > g_diag_cb_max_us) g_diag_cb_max_us = _d;
    }
    ph_rt_mark(0);
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
    g_free(engine.render_tap_L);
    g_free(engine.render_tap_R);

    engine.master_L    = g_malloc0(nframes * sizeof(float));
    engine.master_R    = g_malloc0(nframes * sizeof(float));
    engine.tmp_L       = g_malloc0(nframes * sizeof(float));
    engine.tmp_R       = g_malloc0(nframes * sizeof(float));
    engine.render_tap_L = g_malloc0(nframes * sizeof(float));
    engine.render_tap_R = g_malloc0(nframes * sizeof(float));
    for (guint s = 0; s < JACKDAW_MAX_TRACKS; s++) {
        g_free(engine.slot_L[s]);
        g_free(engine.slot_R[s]);
        engine.slot_L[s] = g_malloc0(nframes * sizeof(float));
        engine.slot_R[s] = g_malloc0(nframes * sizeof(float));
    }
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

/* Count physical JACK ports of the given type and direction flags.
 * Used for auto-detecting port counts at startup.
 * Returns at least 1 so we never register zero ports. */
static guint count_physical_ports(jack_client_t *c, const char *type,
                                   unsigned long flags)
{
    const char **ports = jack_get_ports(c, NULL, type,
                                        flags | JackPortIsPhysical);
    guint n = 0;
    if (ports) {
        for (; ports[n]; n++);
        jack_free(ports);
    }
    return n ? n : 1;
}

/* ---- Port topology callbacks (JACK notification thread → main thread) ---- */

/* Scheduled via g_idle_add: emits ports-changed so all strips refresh. */
static gboolean ports_changed_idle(gpointer data)
{
    JackDawProject *p = data;
    if (JACKDAW_IS_PROJECT(p))
        jackdaw_project_emit_ports_changed(p);
    g_object_unref(p);
    return G_SOURCE_REMOVE;
}

/* Scheduled when a connection is made or broken: verifies that stored
 * src_port names are still actually connected; clears them if not,
 * then fires ports-changed so strips show "None" again. */
static gboolean connection_changed_idle(gpointer data)
{
    JackDawProject *p = data;
    if (!JACKDAW_IS_PROJECT(p) || !engine.active || !engine.client)
        goto done;

    for (guint i = 0; i < JACKDAW_MAX_TRACKS; i++) {
        JackDawTrack *t = engine.slots[i];
        if (!t) continue;

        /* Check audio connection still live */
        if (t->audio_src_port && t->audio_in_idx >= 0 &&
            (guint)t->audio_in_idx < engine.audio_in_count &&
            engine.audio_in[(guint)t->audio_in_idx]) {
            jack_port_t *jp = engine.audio_in[(guint)t->audio_in_idx];
            const char **conns = jack_port_get_all_connections(engine.client, jp);
            gboolean found = FALSE;
            if (conns) {
                for (const char **c = conns; *c; c++)
                    if (strcmp(*c, t->audio_src_port) == 0) { found = TRUE; break; }
                jack_free(conns);
            }
            if (!found) g_clear_pointer(&t->audio_src_port, g_free);
        }

        /* Check audio right-channel connection still live */
        if (t->audio_src_port_r && t->audio_in_idx >= 0 &&
            (guint)t->audio_in_idx < engine.audio_in_count &&
            engine.audio_in_r[(guint)t->audio_in_idx]) {
            jack_port_t *jp = engine.audio_in_r[(guint)t->audio_in_idx];
            const char **conns = jack_port_get_all_connections(engine.client, jp);
            gboolean found = FALSE;
            if (conns) {
                for (const char **c = conns; *c; c++)
                    if (strcmp(*c, t->audio_src_port_r) == 0) { found = TRUE; break; }
                jack_free(conns);
            }
            if (!found) g_clear_pointer(&t->audio_src_port_r, g_free);
        }

        /* Check MIDI connection still live */
        if (t->midi_src_port && t->midi_in_idx >= 0 &&
            (guint)t->midi_in_idx < engine.midi_in_count &&
            engine.midi_in[(guint)t->midi_in_idx]) {
            jack_port_t *jp = engine.midi_in[(guint)t->midi_in_idx];
            const char **conns = jack_port_get_all_connections(engine.client, jp);
            gboolean found = FALSE;
            if (conns) {
                for (const char **c = conns; *c; c++)
                    if (strcmp(*c, t->midi_src_port) == 0) { found = TRUE; break; }
                jack_free(conns);
            }
            /* Source vanished (e.g. unplugged or re-patched away): detach the
             * track from the now-dead input so it stops claiming that port. */
            if (!found) {
                g_clear_pointer(&t->midi_src_port, g_free);
                t->midi_in_idx = -1;
            }
        }
    }
    jackdaw_project_emit_ports_changed(p);

done:
    g_object_unref(p);
    return G_SOURCE_REMOVE;
}

/* Fires on any port registration/unregistration */
static void engine_port_reg_cb(jack_port_id_t port_id, int registered, void *arg)
{
    (void)port_id; (void)registered;
    g_idle_add(ports_changed_idle, g_object_ref((JackDawProject *)arg));
}

/* Fires on any connection or disconnection */
static void engine_port_connect_cb(jack_port_id_t a, jack_port_id_t b,
                                    int connected, void *arg)
{
    (void)a; (void)b; (void)connected;
    g_idle_add(connection_changed_idle, g_object_ref((JackDawProject *)arg));
}

/* -----------------------------------------------------------------------
 * Diagnostics (opt-in via JACKDAW_DIAG=1). RT-safe: the process callback only
 * writes plain volatiles / atomics; a separate thread prints a summary once a
 * second so we can see WHERE the RT cycle time goes (per-plugin process() µs),
 * how it compares to the JACK period, and whether VST3 plugins allocate on the
 * RT thread. Zero cost when JACKDAW_DIAG is unset (timing is gated per-cycle).
 * State is declared near the top so engine_process() can write it.
 * ----------------------------------------------------------------------- */
static int engine_xrun_cb(void *arg)
{
    (void)arg;
    g_atomic_int_inc(&g_diag_xruns);   /* RT-safe */
    return 0;
}

static gpointer diag_thread_func(gpointer arg)
{
    (void)arg;
    int last_xruns = 0;
    guint64 last_alloc = 0;
    while (!g_atomic_int_get(&g_diag_quit)) {
        g_usleep(1000000);   /* 1 s */
        int x = g_atomic_int_get(&g_diag_xruns);
        int dx = x - last_xruns; last_xruns = x;
        guint64 a = ph_vst3_rt_alloc_count();
        guint64 da = a - last_alloc; last_alloc = a;
        gint64 cb_max = g_diag_cb_max_us; g_diag_cb_max_us = 0;
        gint64 period = g_diag_period_us;

        GString *s = g_string_new(NULL);
        g_string_printf(s,
            "[diag] xruns +%d (total %d)  cb_last=%ldus cb_max=%ldus period=%ldus  "
            "vst3_rt_allocs +%lu",
            dx, x, (long)g_diag_cb_last_us, (long)cb_max, (long)period,
            (unsigned long)da);

        /* Per-plugin worst case for the last second, as microseconds and as a
         * share of the period budget — this is what attributes an xrun to one
         * plugin rather than to the callback as a whole. Read from the table the
         * RT thread fills; no chain snapshot or PluginInstance is touched here,
         * because either may be freed by an FX edit at any time. */
        for (int i = 0; i < JACKDAW_MAX_TRACKS; i++) {
            for (int fi = 0; fi < ENG_DIAG_FX_SLOTS; fi++) {
                gint us = g_atomic_int_get(&g_diag_fx_us[i][fi]);
                if (us <= 0) continue;
                g_atomic_int_set(&g_diag_fx_us[i][fi], 0);
                g_string_append_printf(s,
                    "\n        track%d fx%d: %ldus (%.1f%% of period)",
                    i, fi, (long)us,
                    period > 0 ? 100.0 * (double)us / (double)period : 0.0);
            }
        }
        g_message("%s", s->str);
        g_string_free(s, TRUE);
    }
    return NULL;
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
        jackdaw_error("Could not connect to JACK server.\n"
                   "Is jackd or pipewire-jack running?");
        return TRUE;
    }

    /* Auto-detect port counts from physical JACK ports when the project has
     * stored value 0 (first run or user reset to auto). */
    if (project->audio_in_count == 0)
        engine.audio_in_count = count_physical_ports(engine.client,
            JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput);
    else
        engine.audio_in_count = CLAMP(project->audio_in_count, 1, 64);

    if (project->audio_out_count == 0)
        engine.audio_out_count = count_physical_ports(engine.client,
            JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput);
    else
        engine.audio_out_count = CLAMP(project->audio_out_count, 1, 64);

    if (project->midi_in_count == 0)
        engine.midi_in_count = count_physical_ports(engine.client,
            JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput);
    else
        engine.midi_in_count = CLAMP(project->midi_in_count, 1, 16);

    if (project->midi_out_count == 0)
        engine.midi_out_count = count_physical_ports(engine.client,
            JACK_DEFAULT_MIDI_TYPE, JackPortIsInput);
    else
        engine.midi_out_count = CLAMP(project->midi_out_count, 1, 16);

    /* Query JACK's actual sample rate and buffer size */
    sr = jack_get_sample_rate(engine.client);
    bs = jack_get_buffer_size(engine.client);
    engine.buf_size    = bs;
    engine.sample_rate = sr;
    rb_bytes = (size_t)(2 * sr) * sizeof(float);

    /* Pre-render the metronome click: a 1 kHz tone with a fast decay. */
    engine.click_len = (int)((double)sr * 0.025);
    engine.click_buf = g_malloc0((size_t)engine.click_len * sizeof(float));
    for (int n = 0; n < engine.click_len; n++) {
        double tt  = (double)n / (double)sr;
        double env = exp(-tt * 120.0);
        engine.click_buf[n] = (float)(0.5 * sin(2.0 * M_PI * 1000.0 * tt) * env);
    }

    /* Allocate mix scratch buffers */
    engine.master_L    = g_malloc0(bs * sizeof(float));
    engine.master_R    = g_malloc0(bs * sizeof(float));
    engine.tmp_L       = g_malloc0(bs * sizeof(float));
    engine.tmp_R       = g_malloc0(bs * sizeof(float));
    engine.render_tap_L = g_malloc0(bs * sizeof(float));
    engine.render_tap_R = g_malloc0(bs * sizeof(float));

    /* Per-slot scratch for parallel track processing (all slots, occupied or
     * not, so a worker can always write its slot). */
    for (guint s = 0; s < JACKDAW_MAX_TRACKS; s++) {
        engine.slot_L[s] = g_malloc0(bs * sizeof(float));
        engine.slot_R[s] = g_malloc0(bs * sizeof(float));
    }

    /* Register callbacks */
    jack_set_thread_init_callback(engine.client, engine_thread_init_cb, NULL);
    jack_set_process_callback(engine.client, engine_process, NULL);
    jack_set_buffer_size_callback(engine.client, engine_buffer_size_cb, NULL);
    jack_set_xrun_callback(engine.client, engine_xrun_cb, NULL);
    jack_on_shutdown(engine.client, engine_shutdown_cb, NULL);
    jack_set_port_registration_callback(engine.client, engine_port_reg_cb, project);
    jack_set_port_connect_callback(engine.client, engine_port_connect_cb, project);

    /* Register MIDI ports before audio so they sort to the top in patchbays
     * (JACK lists ports by registration order). Audio ports — which the user
     * can grow at runtime — then always appear below the MIDI ports. */

    /* Every port array is allocated at its ceiling and never resized, so the RT
     * callback can index it without ever racing a reallocation when the user
     * changes a port count (see the set_*_count functions). */
    engine.midi_in = g_new0(jack_port_t *, ENG_MAX_MIDI_PORTS);
    for (i = 0; i < engine.midi_in_count; i++) {
        g_snprintf(name, sizeof(name), "midi_in_%u", i + 1);
        engine.midi_in[i] = jack_port_register(engine.client, name,
            JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
        if (!engine.midi_in[i]) goto fail;
    }

    /* Register MIDI output ports: midi_out_1 .. midi_out_M */
    engine.midi_out = g_new0(jack_port_t *, ENG_MAX_MIDI_PORTS);
    for (i = 0; i < engine.midi_out_count; i++) {
        g_snprintf(name, sizeof(name), "midi_out_%u", i + 1);
        engine.midi_out[i] = jack_port_register(engine.client, name,
            JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);
        if (!engine.midi_out[i]) goto fail;
    }

    /* Dedicated control-surface MIDI input. Literal name (never user input). */
    engine.control_in = jack_port_register(engine.client, "control_in",
        JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
    if (!engine.control_in) goto fail;

    /* Register audio input ports: in_1 .. in_N (mono by default). The matching
     * right-channel port in_NR is registered lazily, per track, only when that
     * track is switched to stereo — so mono tracks stay single in the patchbay
     * and only stereo tracks appear as a pair. */
    engine.audio_in   = g_new0(jack_port_t *, ENG_MAX_AUDIO_PORTS);
    engine.audio_in_r = g_new0(jack_port_t *, ENG_MAX_AUDIO_PORTS);
    for (i = 0; i < engine.audio_in_count; i++) {
        g_snprintf(name, sizeof(name), "in_%u", i + 1);
        engine.audio_in[i] = jack_port_register(engine.client, name,
            JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
        if (!engine.audio_in[i]) goto fail;
    }

    /* Register audio output ports: out_1 .. out_N */
    engine.audio_out = g_new0(jack_port_t *, ENG_MAX_AUDIO_PORTS);
    for (i = 0; i < engine.audio_out_count; i++) {
        g_snprintf(name, sizeof(name), "out_%u", i + 1);
        engine.audio_out[i] = jack_port_register(engine.client, name,
            JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
        if (!engine.audio_out[i]) goto fail;
    }

    /* Dedicated metronome output (mono). Routed by the user (e.g. to a
     * performer's headphone bus) via the patchbay; not auto-connected. */
    engine.metro_out = jack_port_register(engine.client, "metronome",
        JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    if (!engine.metro_out) goto fail;

    /* Preview-note queue (main thread -> RT). Sized for many in-flight clicks. */
    eng_preview_rb = jack_ringbuffer_create(256 * sizeof(EngPrevMsg));
    if (eng_preview_rb) jack_ringbuffer_mlock(eng_preview_rb);

    /* Control-surface input queue (RT -> main). Footswitch events are sparse;
     * 1024 slots is ample and the RT side drops on overflow, never blocks. */
    eng_control_rb = jack_ringbuffer_create(1024 * sizeof(JackDawCtlEvent));
    if (eng_control_rb) jack_ringbuffer_mlock(eng_control_rb);

    /* Activate — after this the process callback can be called at any time */
    if (jack_activate(engine.client) != 0) {
        jackdaw_error("jackdaw: jack_activate() failed");
        goto fail;
    }


    /* Auto-connect midi_out_N → physical MIDI playback ports.
     * Audio outputs are handled by the system JACK auto-connect mechanism;
     * MIDI is not, so we wire it explicitly here. EEXIST is silently ignored
     * in case an external patchbay already made the connection. */
    {
        const char **phys_midi_in = jack_get_ports(engine.client, NULL,
            JACK_DEFAULT_MIDI_TYPE, JackPortIsInput | JackPortIsPhysical);
        if (phys_midi_in) {
            guint pi;
            for (pi = 0; pi < engine.midi_out_count && phys_midi_in[pi]; pi++) {
                const char *src = jack_port_name(engine.midi_out[pi]);
                int r = jack_connect(engine.client, src, phys_midi_in[pi]);
                (void)r; /* EEXIST is fine */
            }
            jack_free(phys_midi_in);
        }
    }

    /* Auto-connect out_N → physical audio playback ports by matching index, so
     * out_1→playback_1, out_2→playback_2, ... Don't rely on JACK's own
     * auto-connect (it's off unless the client requests it and many setups
     * disable it), so wire it here the same way MIDI is. EEXIST is fine. */
    {
        const char **phys_audio_in = jack_get_ports(engine.client, NULL,
            JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput | JackPortIsPhysical);
        if (phys_audio_in) {
            guint pi;
            for (pi = 0; pi < engine.audio_out_count && phys_audio_in[pi]; pi++) {
                const char *src = jack_port_name(engine.audio_out[pi]);
                int r = jack_connect(engine.client, src, phys_audio_in[pi]);
                (void)r; /* EEXIST is fine */
            }
            jack_free(phys_audio_in);
        }
    }

    /* Auto-connect physical MIDI capture ports → midi_in_N by matching index, so
     * capture_1→midi_in_1, capture_2→midi_in_2, ... This makes each midi_in port
     * a stable mirror of the hardware input of the same number (the way a track's
     * MIDI source maps in other DAWs). Tracks then just pick which midi_in to
     * read; they no longer wire their own connection. EEXIST is fine. */
    {
        const char **phys_midi_out = jack_get_ports(engine.client, NULL,
            JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput | JackPortIsPhysical);
        if (phys_midi_out) {
            guint pi;
            for (pi = 0; pi < engine.midi_in_count && phys_midi_out[pi]; pi++) {
                const char *dst = jack_port_name(engine.midi_in[pi]);
                int r = jack_connect(engine.client, phys_midi_out[pi], dst);
                (void)r; /* EEXIST is fine */
            }
            jack_free(phys_midi_out);
        }
    }

    /* Start background threads */
    feeder_start();
    recorder_start();

    engine.active = TRUE;
    (void)rb_bytes; /* suppress unused-variable warning */

    /* Spawn the RT-priority worker pool for parallel track processing. Count
     * defaults to (CPU cores − 1) so the JACK thread itself is the Nth worker;
     * override with JACKDAW_RT_THREADS (0 = serial, for A/B comparison). Workers
     * are created via jack_client_create_thread so they inherit JACK's real-time
     * scheduling — a plain pthread would be preempted and make xruns worse. */
    {
        int want = (int)g_get_num_processors() - 1;
        const char *env = g_getenv("JACKDAW_RT_THREADS");
        if (env) want = atoi(env);
        if (want < 0) want = 0;
        if (want > RT_MAX_WORKERS) want = RT_MAX_WORKERS;

        if (want > 0) {
            int prio = jack_client_real_time_priority(engine.client);
            int rt   = (prio > 0) ? 1 : 0;
            sem_init(&g_rt_sem_go, 0, 0);
            sem_init(&g_rt_sem_done, 0, 0);
            g_atomic_int_set(&g_rt_workers_quit, 0);
            int spawned = 0;
            for (int w = 0; w < want; w++) {
                if (jack_client_create_thread(engine.client, &g_rt_worker[w],
                        prio, rt, rt_worker_main, NULL) != 0)
                    break;
                spawned++;
            }
            g_rt_nworkers = spawned;
            if (spawned == 0) { sem_destroy(&g_rt_sem_go); sem_destroy(&g_rt_sem_done); }
        }
        g_message("jackdaw: %d RT worker thread(s) for parallel track processing "
                  "(%d cores)", g_rt_nworkers, (int)g_get_num_processors());
    }

    /* Diagnostics: only when JACKDAW_DIAG is set. */
    g_diag_on = (g_getenv("JACKDAW_DIAG") != NULL);
    if (g_diag_on) {
        jack_nframes_t sr = jack_get_sample_rate(engine.client);
        g_diag_period_us = sr ? (gint64)engine.buf_size * 1000000 / sr : 0;
        g_atomic_int_set(&g_diag_quit, 0);
        g_diag_thread = g_thread_new("jackdaw-diag", diag_thread_func, NULL);
        g_message("[diag] enabled: buf=%u period=%ldus — watching xruns / "
                  "per-plugin process() time / VST3 RT allocations",
                  engine.buf_size, (long)g_diag_period_us);
    }
    return FALSE;   /* success */

fail:
    jack_client_close(engine.client);
    engine.client = NULL;
    g_free(engine.master_L); g_free(engine.master_R);
    g_free(engine.tmp_L);    g_free(engine.tmp_R);
    g_free(engine.audio_in); g_free(engine.audio_out);
    g_free(engine.midi_in);  g_free(engine.midi_out);
    return TRUE;
}

void jackdaw_engine_quit(void)
{
    if (!engine.active || !engine.client) return;

    /* Stop background threads before deactivating JACK */
    feeder_stop();
    recorder_stop();
    if (g_rt_nworkers > 0) {
        g_atomic_int_set(&g_rt_workers_quit, 1);
        for (int w = 0; w < g_rt_nworkers; w++) sem_post(&g_rt_sem_go);
        for (int w = 0; w < g_rt_nworkers; w++) pthread_join(g_rt_worker[w], NULL);
        sem_destroy(&g_rt_sem_go);
        sem_destroy(&g_rt_sem_done);
        g_rt_nworkers = 0;
    }
    if (g_diag_thread) {
        g_atomic_int_set(&g_diag_quit, 1);
        g_thread_join(g_diag_thread);
        g_diag_thread = NULL;
    }

    jack_deactivate(engine.client);
    jack_client_close(engine.client);
    engine.client = NULL;
    engine.active = FALSE;

    g_free(engine.master_L); engine.master_L = NULL;
    g_free(engine.master_R); engine.master_R = NULL;
    g_free(engine.tmp_L);    engine.tmp_L    = NULL;
    g_free(engine.tmp_R);    engine.tmp_R    = NULL;
    g_free(engine.render_tap_L); engine.render_tap_L = NULL;
    g_free(engine.render_tap_R); engine.render_tap_R = NULL;
    for (guint s = 0; s < JACKDAW_MAX_TRACKS; s++) {
        g_free(engine.slot_L[s]); engine.slot_L[s] = NULL;
        g_free(engine.slot_R[s]); engine.slot_R[s] = NULL;
    }
    if (engine.render_rb_L) { jack_ringbuffer_free(engine.render_rb_L); engine.render_rb_L = NULL; }
    if (engine.render_rb_R) { jack_ringbuffer_free(engine.render_rb_R); engine.render_rb_R = NULL; }
    g_free(engine.click_buf); engine.click_buf = NULL; engine.click_len = 0;
    g_free(engine.audio_in);  engine.audio_in  = NULL;
    g_free(engine.audio_out); engine.audio_out = NULL;
    g_free(engine.midi_in);   engine.midi_in   = NULL;
    g_free(engine.midi_out);  engine.midi_out  = NULL;

    if (eng_preview_rb) { jack_ringbuffer_free(eng_preview_rb); eng_preview_rb = NULL; }
    if (eng_control_rb) { jack_ringbuffer_free(eng_control_rb); eng_control_rb = NULL; }
    engine.control_in = NULL;   /* unregistered by jack_client_close above */
    g_clear_pointer(&engine.control_src_port, g_free);
}

/* ---- MIDI control surface ---- */

gboolean jackdaw_engine_control_poll(JackDawCtlEvent *out)
{
    if (!eng_control_rb || !out) return FALSE;
    if (jack_ringbuffer_read_space(eng_control_rb) < sizeof(JackDawCtlEvent))
        return FALSE;
    jack_ringbuffer_read(eng_control_rb, (char *)out, sizeof(JackDawCtlEvent));
    return TRUE;
}

gboolean jackdaw_engine_set_control_source(const gchar *port_name)
{
    if (!engine.active || !engine.client || !engine.control_in) return TRUE;
    const char *dst = jack_port_name(engine.control_in);

    if (engine.control_src_port) {
        jack_disconnect(engine.client, engine.control_src_port, dst);
        g_clear_pointer(&engine.control_src_port, g_free);
    }
    if (port_name && *port_name) {
        int r = jack_connect(engine.client, port_name, dst);
        if (r != 0 && r != EEXIST) return TRUE;
        engine.control_src_port = g_strdup(port_name);
    }
    return FALSE;
}

const gchar *jackdaw_engine_get_control_source(void)
{
    return engine.control_src_port;
}

void jackdaw_engine_preview_note(JackDawTrack *t, guint8 pitch,
                                 guint8 velocity, guint8 channel, gboolean on)
{
    if (!engine.active || !eng_preview_rb || !t) return;
    if (t->slot >= JACKDAW_MAX_TRACKS) return;   /* not registered with engine */

    EngPrevMsg msg;
    guint8 ch   = (guint8)(channel & 0x0F);
    msg.slot    = (gint32)t->slot;
    msg.data[0] = (guint8)((on ? 0x90 : 0x80) | ch);
    msg.data[1] = (guint8)(pitch & 0x7F);
    msg.data[2] = on ? (velocity ? velocity : 1) : 0;

    if (jack_ringbuffer_write_space(eng_preview_rb) >= sizeof msg)
        jack_ringbuffer_write(eng_preview_rb, (const char *)&msg, sizeof msg);
}

gboolean jackdaw_engine_is_running(void)
{
    return engine.active;
}

gboolean jackdaw_engine_is_counting_in(void)
{
    return g_atomic_int_get(&engine.countin_active) != 0;
}

guint jackdaw_engine_get_cycle_count(void)
{
    return (guint)g_atomic_int_get((gint *)&g_rt_cycle);
}

/* TRUE while the JACK client is active, i.e. while the RT callback can still be
 * running. When this is FALSE nothing reads the published FX chains at all. */
gboolean jackdaw_engine_is_processing(void)
{
    return engine.active && engine.client != NULL &&
           !g_atomic_int_get(&engine.render_suspend);
}

guint jackdaw_engine_get_xrun_count(void)
{
    return (guint)g_atomic_int_get(&g_diag_xruns);
}

gboolean jackdaw_engine_is_recording(void)
{
    return (g_atomic_int_get(&engine.transport_flags) & ENGINE_RECORDING) != 0;
}

gboolean jackdaw_engine_is_playing(void)
{
    return (g_atomic_int_get(&engine.transport_flags) & ENGINE_PLAYING) != 0;
}

/* ---- Port count management ----
 *
 * The port arrays are allocated once at their ceiling (ENG_MAX_AUDIO_PORTS /
 * ENG_MAX_MIDI_PORTS) and never resized, and the count is published in the
 * order that keeps the RT callback safe at every instant:
 *
 *   grow   — register the new ports FIRST, then raise the count.
 *   shrink — lower the count FIRST, then unregister the ports it dropped.
 *
 * The old code did the opposite on both counts: it g_renew()'d the arrays the
 * callback was indexing (so a concurrent cycle could read through a freed
 * pointer) and unregistered shrinking ports before lowering the count (so the
 * callback could jack_port_get_buffer() a port that no longer existed). */

/* Publish a new count with release semantics so the port writes above it are
 * visible to the RT thread before the count that exposes them. */
static inline void eng_publish_count(volatile guint *slot, guint n)
{
    g_atomic_int_set((gint *)slot, (gint)n);
}

gboolean jackdaw_engine_set_audio_in_count(guint n)
{
    guint i, old;
    char name[64];
    n = CLAMP(n, 1, ENG_MAX_AUDIO_PORTS);
    if (!engine.active || !engine.client) {
        engine.audio_in_count = n;
        settings_set_uint32("jackAudioInCount", n);
        return FALSE;
    }
    old = engine.audio_in_count;
    if (n == old) return FALSE;

    if (n > old) {
        /* Right ports stay NULL until a track goes stereo (set_track_stereo). */
        for (i = old; i < n; i++) {
            engine.audio_in_r[i] = NULL;
            g_snprintf(name, sizeof(name), "in_%u", i + 1);
            engine.audio_in[i] = jack_port_register(engine.client, name,
                JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
            if (!engine.audio_in[i]) return TRUE;
        }
        eng_publish_count(&engine.audio_in_count, n);
    } else {
        eng_publish_count(&engine.audio_in_count, n);
        for (i = n; i < old; i++) {
            if (engine.audio_in[i]) {
                jack_port_unregister(engine.client, engine.audio_in[i]);
                engine.audio_in[i] = NULL;
            }
            if (engine.audio_in_r[i]) {
                jack_port_unregister(engine.client, engine.audio_in_r[i]);
                engine.audio_in_r[i] = NULL;
            }
        }
    }
    settings_set_uint32("jackAudioInCount", n);
    if (engine.project)
        jackdaw_project_emit_ports_changed(engine.project);
    return FALSE;
}

gboolean jackdaw_engine_set_audio_out_count(guint n)
{
    guint i, old;
    char name[64];
    n = CLAMP(n, 1, ENG_MAX_AUDIO_PORTS);
    if (!engine.active || !engine.client) {
        engine.audio_out_count = n;
        settings_set_uint32("jackAudioOutCount", n);
        return FALSE;
    }
    old = engine.audio_out_count;
    if (n == old) return FALSE;

    if (n > old) {
        for (i = old; i < n; i++) {
            g_snprintf(name, sizeof(name), "out_%u", i + 1);
            engine.audio_out[i] = jack_port_register(engine.client, name,
                JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
            if (!engine.audio_out[i]) return TRUE;
        }
        eng_publish_count(&engine.audio_out_count, n);
    } else {
        eng_publish_count(&engine.audio_out_count, n);
        for (i = n; i < old; i++) {
            if (engine.audio_out[i]) {
                jack_port_unregister(engine.client, engine.audio_out[i]);
                engine.audio_out[i] = NULL;
            }
        }
    }
    settings_set_uint32("jackAudioOutCount", n);
    if (engine.project)
        jackdaw_project_emit_ports_changed(engine.project);
    return FALSE;
}

gboolean jackdaw_engine_set_midi_in_count(guint n)
{
    guint i, old;
    char name[64];
    if (n > ENG_MAX_MIDI_PORTS) n = ENG_MAX_MIDI_PORTS;
    if (!engine.active || !engine.client) {
        engine.midi_in_count = n;
        settings_set_uint32("jackMidiInCount", n);
        return FALSE;
    }
    old = engine.midi_in_count;
    if (n == old) return FALSE;

    if (n > old) {
        for (i = old; i < n; i++) {
            g_snprintf(name, sizeof(name), "midi_in_%u", i + 1);
            engine.midi_in[i] = jack_port_register(engine.client, name,
                JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
            if (!engine.midi_in[i]) return TRUE;
        }
        eng_publish_count(&engine.midi_in_count, n);
    } else {
        eng_publish_count(&engine.midi_in_count, n);
        for (i = n; i < old; i++) {
            if (engine.midi_in[i]) {
                jack_port_unregister(engine.client, engine.midi_in[i]);
                engine.midi_in[i] = NULL;
            }
        }
    }
    settings_set_uint32("jackMidiInCount", n);
    if (engine.project)
        jackdaw_project_emit_ports_changed(engine.project);
    return FALSE;
}

gboolean jackdaw_engine_set_midi_out_count(guint n)
{
    guint i, old;
    char name[64];
    n = CLAMP(n, 1, ENG_MAX_MIDI_PORTS);
    if (!engine.active || !engine.client) {
        engine.midi_out_count = n;
        settings_set_uint32("jackMidiOutCount", n);
        return FALSE;
    }
    old = engine.midi_out_count;
    if (n == old) return FALSE;

    if (n > old) {
        for (i = old; i < n; i++) {
            g_snprintf(name, sizeof(name), "midi_out_%u", i + 1);
            engine.midi_out[i] = jack_port_register(engine.client, name,
                JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);
            if (!engine.midi_out[i]) return TRUE;
        }
        eng_publish_count(&engine.midi_out_count, n);
    } else {
        eng_publish_count(&engine.midi_out_count, n);
        for (i = n; i < old; i++) {
            if (engine.midi_out[i]) {
                jack_port_unregister(engine.client, engine.midi_out[i]);
                engine.midi_out[i] = NULL;
            }
        }
    }
    settings_set_uint32("jackMidiOutCount", n);
    if (engine.project)
        jackdaw_project_emit_ports_changed(engine.project);
    return FALSE;
}

guint jackdaw_engine_get_audio_in_count (void) { return engine.audio_in_count;  }
guint jackdaw_engine_get_audio_out_count(void) { return engine.audio_out_count; }
guint jackdaw_engine_get_midi_in_count  (void) { return engine.midi_in_count;   }
guint jackdaw_engine_get_midi_out_count (void) { return engine.midi_out_count;  }

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

jack_port_t *jackdaw_engine_get_midi_out_port(guint idx)
{
    if (!engine.midi_out || idx >= engine.midi_out_count) return NULL;
    return engine.midi_out[idx];
}

/* ---- Track management ---- */

/* MIDI input ports are a shared pool claimed only by instrument (MIDI) tracks,
 * independent of the track's slot index. This keeps audio tracks — which never
 * use a MIDI input — from consuming MIDI port indices and starving MIDI tracks
 * that are added later or at a higher slot. Returns the lowest midi_in index
 * not held by another live track; if every current port is taken it grows the
 * pool by one (up to the JACK MIDI-in ceiling). Returns -1 only when the pool
 * is already at its maximum. Main thread only. */
static gint engine_claim_free_midi_in(JackDawTrack *self)
{
    guint idx, s;
    for (idx = 0; idx < engine.midi_in_count; idx++) {
        gboolean used = FALSE;
        for (s = 0; s < JACKDAW_MAX_TRACKS; s++) {
            JackDawTrack *o = engine.slots[s];
            if (o && o != self && o->midi_in_idx == (gint)idx) { used = TRUE; break; }
        }
        if (!used) return (gint)idx;
    }
    /* All current ports are spoken for — grow the pool by one if there's room.
     * set_midi_in_count registers the new midi_in_N port and returns FALSE on
     * success (TRUE == failure, per the project boolean convention). */
    if (engine.midi_in_count < 16u) {
        guint want = engine.midi_in_count + 1;
        if (!jackdaw_engine_set_midi_in_count(want))
            return (gint)(want - 1);
    }
    return -1;
}

/* Return the midi_in index currently wired to external source `src`, or -1 if
 * none is. Physical captures are auto-connected by number at startup (capture_N
 * → midi_in_N), so this resolves a chosen source to the matching port even when
 * the user has re-patched things by hand. Main thread only. */
static gint engine_midi_in_for_source(const char *src)
{
    guint k;
    if (!src || !engine.client) return -1;
    for (k = 0; k < engine.midi_in_count; k++) {
        if (!engine.midi_in[k]) continue;
        const char **conns =
            jack_port_get_all_connections(engine.client, engine.midi_in[k]);
        gint found = -1;
        if (conns) {
            for (int c = 0; conns[c]; c++)
                if (strcmp(conns[c], src) == 0) { found = (gint)k; break; }
            jack_free(conns);
        }
        if (found >= 0) return found;
    }
    return -1;
}

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
        jackdaw_error("jackdaw: maximum track count reached");
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
        jackdaw_error("jackdaw: ringbuffer allocation failed");
        return TRUE;
    }

    jack_ringbuffer_mlock(track->play_buf_L);
    jack_ringbuffer_mlock(track->play_buf_R);
    jack_ringbuffer_mlock(track->rec_buf_L);
    jack_ringbuffer_mlock(track->rec_buf_R);

    track->slot   = i;
    engine.slots[i] = track; /* RT callback can see this now */

    /* Assign input ports by track kind, not by slot index. An audio track takes
     * audio_in[slot] (its left/right capture). An instrument track gets no MIDI
     * input until the user selects a source: midi_in ports mirror the hardware
     * captures by number (wired at startup), and set_midi_source() resolves the
     * chosen source to the matching port — so the track can be added at any slot,
     * any time, and a source picked later maps to the right midi_in. */
    if (jackdaw_track_is_instrument(track)) {
        track->audio_in_idx = -1;
        track->midi_in_idx  = -1;
    } else {
        track->audio_in_idx = ((guint)i < engine.audio_in_count) ? (gint)i : -1;
        track->midi_in_idx  = -1;
    }

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

    /* Disconnect external sources before releasing the slot */
    if (engine.active && engine.client) {
        if (track->audio_src_port && track->audio_in_idx >= 0 &&
            (guint)track->audio_in_idx < engine.audio_in_count &&
            engine.audio_in[(guint)track->audio_in_idx]) {
            jack_disconnect(engine.client, track->audio_src_port,
                            jack_port_name(engine.audio_in[(guint)track->audio_in_idx]));
        }
        if (track->audio_src_port_r && track->audio_in_idx >= 0 &&
            (guint)track->audio_in_idx < engine.audio_in_count &&
            engine.audio_in_r[(guint)track->audio_in_idx]) {
            jack_disconnect(engine.client, track->audio_src_port_r,
                            jack_port_name(engine.audio_in_r[(guint)track->audio_in_idx]));
        }
        /* MIDI: the capture→midi_in wiring is system-wide (auto-connected at
         * startup, and possibly shared by other tracks), so removing a track
         * must NOT tear it down — just detaching the slot is enough. */

        /* Release the lazily-registered right capture port. It is created only
         * when this track goes stereo (set_track_stereo) and belongs to the
         * track holding this index, exactly as the mono path there assumes —
         * but removing the track used to leave it registered, so every
         * add/stereo/remove cycle leaked a JACK port. */
        if (track->audio_in_idx >= 0 &&
            (guint)track->audio_in_idx < engine.audio_in_count &&
            engine.audio_in_r[(guint)track->audio_in_idx]) {
            jack_port_unregister(engine.client,
                                 engine.audio_in_r[(guint)track->audio_in_idx]);
            engine.audio_in_r[(guint)track->audio_in_idx] = NULL;
        }
    }
    g_clear_pointer(&track->audio_src_port,   g_free);
    g_clear_pointer(&track->audio_src_port_r, g_free);
    g_clear_pointer(&track->midi_src_port,    g_free);

    track->audio_in_idx = -1;
    track->midi_in_idx  = -1;
    track->slot = G_MAXUINT;
}

/* ---- Transport ---- */

static gboolean recorder_open_slot(guint i, JackDawTrack *t,
                                   off_t start_frame, off_t expected_frames);

void jackdaw_engine_start_playback(void)
{
    /* Punch in/out: when the record mode is punch and a tab region lies at or
     * ahead of the playhead, pre-open recorder slots for every armed audio track.
     * The RT path engages recording as the playhead crosses loop_start and stops
     * it at loop_end (see engine_process). Independent of the loop button — only
     * the tab positions are used, and playback never wraps. */
    if (g_atomic_int_get(&engine.record_mode) == RECORD_MODE_PUNCH &&
        !g_atomic_int_get(&engine.punch_armed) &&
        engine.loop_end > engine.loop_start &&
        engine.play_pos < engine.loop_end) {
        off_t    ls  = engine.loop_start, le = engine.loop_end;
        gboolean any = FALSE;
        for (guint i = 0; i < JACKDAW_MAX_TRACKS; i++) {
            JackDawTrack *t = engine.slots[i];
            if (!t) continue;
            if (!(g_atomic_int_get(&t->state_flags) & TRACK_ARMED)) continue;
            if (jackdaw_track_is_instrument(t)) continue;  /* audio only */
            if (t->audio_in_idx < 0) continue;
            if (!recorder_open_slot(i, t, ls, le - ls)) continue;
            recorder_slots[i].punch          = 1;
            recorder_slots[i].punch_tl_start = ls;
            recorder_slots[i].punch_tl_end   = le;
            any = TRUE;
        }
        if (any) g_atomic_int_set(&engine.punch_armed, 1);
    }

    g_atomic_int_or(&engine.transport_flags, ENGINE_PLAYING);
}

void jackdaw_engine_stop_playback(void)
{
    g_atomic_int_and(&engine.transport_flags, ~ENGINE_PLAYING);

    /* Cancel a count-in pre-roll that never reached its hand-off. Discard any
     * capture slots that were pre-opened for a record count-in (an unwritten
     * take is finalized empty and dropped by the recorder thread). */
    if (g_atomic_int_get(&engine.countin_active)) {
        gboolean was_rec = g_atomic_int_get(&engine.countin_pending_rec);
        g_atomic_int_set(&engine.countin_active, 0);
        g_atomic_int_set(&engine.countin_pending_rec, 0);
        if (was_rec) {
            for (guint i = 0; i < JACKDAW_MAX_TRACKS; i++)
                if (recorder_slots[i].sf) {
                    recorder_slots[i].expected_frames = 0;
                    g_atomic_int_set(&recorder_slots[i].finalize_req, 1);
                }
        }
    }

    /* Cancel any pending/in-progress punch: stop capture and let the recorder
     * thread finalize whatever was recorded (an empty take is discarded). */
    if (g_atomic_int_get(&engine.punch_armed)) {
        g_atomic_int_set(&engine.punch_armed, 0);
        g_atomic_int_and(&engine.transport_flags, ~ENGINE_RECORDING);
        for (guint i = 0; i < JACKDAW_MAX_TRACKS; i++)
            if (recorder_slots[i].sf && recorder_slots[i].punch)
                g_atomic_int_set(&recorder_slots[i].finalize_req, 1);
    }

    /* Release any notes sounding on instrument tracks (no stuck notes on stop
     * or seek — jackdaw_engine_locate() also calls through here). */
    for (guint i = 0; i < JACKDAW_MAX_TRACKS; i++)
        g_atomic_int_set(&eng_midi_flush[i], 1);
}

void jackdaw_engine_set_record_mode(int mode)
{
    g_atomic_int_set(&engine.record_mode,
                     mode == RECORD_MODE_PUNCH ? RECORD_MODE_PUNCH : RECORD_MODE_NORMAL);
}

int jackdaw_engine_get_record_mode(void)
{
    return g_atomic_int_get(&engine.record_mode);
}

/* Open a WAV recorder slot for one armed audio track. start_frame is the timeline
 * position the take begins at; expected_frames caps the capture length (0 =
 * unlimited, set later at stop). The caller must have verified the track is an
 * armed audio track with a valid input. Returns TRUE on success. */
static gboolean recorder_open_slot(guint i, JackDawTrack *t,
                                   off_t start_frame, off_t expected_frames)
{
    if (recorder_slots[i].sf) return FALSE; /* already open */

    /* Create recordings directory under ~/.jackdaw/ (settings_init made the parent) */
    gchar *rec_dir = g_build_filename(g_get_home_dir(), ".jackdaw", "recordings", NULL);
    g_mkdir_with_parents(rec_dir, 0700);

    jack_nframes_t sr  = engine.client ? jack_get_sample_rate(engine.client) : 48000;
    GDateTime     *now = g_date_time_new_now_local();
    gchar *ts    = g_date_time_format(now, "%Y%m%d_%H%M%S");
    gchar *fname = g_strdup_printf("track_%u_%s.wav", i + 1, ts);
    gchar *fpath = g_build_filename(rec_dir, fname, NULL);
    g_strlcpy(recorder_slots[i].path, fpath, sizeof(recorder_slots[i].path));
    g_free(fpath); g_free(fname); g_free(ts);
    g_date_time_unref(now);
    g_free(rec_dir);

    int channels = t->mono_record ? 1 : 2;

    SF_INFO sfi    = { 0 };
    sfi.samplerate = (int)sr;
    sfi.channels   = channels;
    sfi.format     = SF_FORMAT_WAV | SF_FORMAT_PCM_24;

    SNDFILE *sf = sf_open(recorder_slots[i].path, SFM_WRITE, &sfi);
    if (!sf) {
        g_warning("jackdaw: could not open recording file %s: %s",
                  recorder_slots[i].path, sf_strerror(NULL));
        return FALSE;
    }
    recorder_slots[i].sf              = sf;
    recorder_slots[i].written         = 0;
    recorder_slots[i].expected_frames = expected_frames;
    recorder_slots[i].channels        = channels;
    recorder_slots[i].punch           = 0;
    g_atomic_int_set(&recorder_slots[i].finalize_req, 0);

    t->rec_start_frame = start_frame;

    /* Allocate real-time waveform peak buffer (1 min/max pair per JACK period) */
    if (!t->rec_peak_buf)
        t->rec_peak_buf = g_new(gfloat, REC_PEAK_MAX_BUCKETS * 2);
    t->rec_peak_count = 0;
    t->rec_peak_block = engine.client ? jack_get_buffer_size(engine.client) : 1024;

    /* Capture latency of the input port, for alignment compensation. */
    t->rec_latency = 0;
    if ((guint)t->audio_in_idx < engine.audio_in_count &&
        engine.audio_in[(guint)t->audio_in_idx]) {
        jack_latency_range_t lr = { 0, 0 };
        jack_port_get_latency_range(
            engine.audio_in[(guint)t->audio_in_idx],
            JackCaptureLatency, &lr);
        t->rec_latency = (off_t)lr.max;
    }
    return TRUE;
}

/* Arm every armed track for capture at the current play_pos: open a WAV slot for
 * audio tracks, reset the MIDI capture ringbuffer for instrument tracks. Must run
 * on the main thread (does file I/O) BEFORE ENGINE_RECORDING is set, so the RT
 * thread is not yet touching the slots/buffers. Shared by immediate recording and
 * count-in recording (where the slots wait, pre-opened, through the pre-roll). */
static void recorder_arm_all(void)
{
    for (guint i = 0; i < JACKDAW_MAX_TRACKS; i++) {
        JackDawTrack *t = engine.slots[i];
        if (!t) continue;
        if (!(g_atomic_int_get(&t->state_flags) & TRACK_ARMED)) continue;

        /* Record start frame for every armed track (audio + instrument). */
        t->rec_start_frame = (off_t)engine.play_pos;

        /* Instrument tracks capture live MIDI into a clip — clear the capture
         * ringbuffer now (RECORDING isn't set yet, so the RT thread is not yet
         * writing to it). No WAV file. */
        if (jackdaw_track_is_instrument(t)) {
            if (t->midi_rec_buf) jack_ringbuffer_reset(t->midi_rec_buf);
            continue;
        }

        if (t->audio_in_idx < 0) continue;
        recorder_open_slot(i, t, (off_t)engine.play_pos, 0 /* unlimited */);
    }
}

void jackdaw_engine_start_recording(void)
{
    recorder_arm_all();

    /* Start rolling — RT callback begins filling rec_buf immediately */
    g_atomic_int_or(&engine.transport_flags, ENGINE_RECORDING | ENGINE_PLAYING);
}

/* Begin a count-in pre-roll, then start playback (record=FALSE) or recording
 * (record=TRUE) when it elapses. `beats` is the number of metronome clicks to
 * sound first. Returns FALSE (caller should start immediately) when no pre-roll
 * is possible: engine not running, beats==0, or a degenerate tempo. The project
 * playhead stays put during the pre-roll, so when recording the take begins
 * exactly at the cursor. */
gboolean jackdaw_engine_begin_countin(guint beats, gboolean record)
{
    if (!engine.active || beats == 0) return FALSE;
    if (g_atomic_int_get(&engine.countin_active)) return TRUE; /* already counting */

    double bpm = (engine.project && engine.project->bpm > 0.0)
                     ? engine.project->bpm : 120.0;
    double fpb = (double)engine.sample_rate * 60.0 / bpm;
    off_t  len = (off_t)(fpb * (double)beats + 0.5);
    if (len <= 0) return FALSE;

    /* For a record count-in, pre-open the capture slots now so recording can
     * engage instantly (no file I/O on the RT thread) when the pre-roll ends. */
    if (record) recorder_arm_all();

    engine.countin_pos = 0;
    engine.countin_len = len;
    g_atomic_int_set(&engine.countin_pending_rec, record ? 1 : 0);
    g_atomic_int_set(&engine.countin_active, 1);   /* publish last */
    return TRUE;
}

/* Drain each instrument track's captured MIDI into a new clip + region on the
 * timeline. Runs on the MAIN thread (g_idle) after RECORDING is cleared, so the
 * RT thread is no longer writing the capture ringbuffer (single reader/writer). */
static gboolean midi_finalize_idle(gpointer data)
{
    (void)data;
    double bpm = (engine.project && engine.project->bpm > 0.0)
                     ? engine.project->bpm : 120.0;
    double fpb = (double)engine.sample_rate * 60.0 / bpm;
    double f_per_tick = (fpb > 0.0) ? fpb / (double)JACKDAW_PPQ : 1.0;
    off_t  cut = eng_midi_rec_cut;

    for (guint i = 0; i < JACKDAW_MAX_TRACKS; i++) {
        JackDawTrack *t = engine.slots[i];
        if (!t || !jackdaw_track_is_instrument(t) || !t->midi_rec_buf) continue;
        if (jack_ringbuffer_read_space(t->midi_rec_buf) < sizeof(MidiRecEvent))
            continue;

        off_t  origin = t->rec_start_frame;
        gint64 on_frame[16][128];
        guint8 on_vel[16][128];
        for (int ch = 0; ch < 16; ch++)
            for (int p = 0; p < 128; p++) on_frame[ch][p] = -1;

        MidiClip *c = midi_clip_new(0);
        gint64    last_frame = origin;

        MidiRecEvent r;
        while (jack_ringbuffer_read_space(t->midi_rec_buf) >= sizeof r) {
            jack_ringbuffer_read(t->midi_rec_buf, (char *)&r, sizeof r);
            if (r.frame > last_frame) last_frame = r.frame;
            int st = r.data[0] & 0xF0, ch = r.data[0] & 0x0F, p = r.data[1] & 0x7F;
            if (st == 0x90 && r.data[2] > 0) {
                on_frame[ch][p] = r.frame;
                on_vel[ch][p]   = r.data[2];
            } else if (st == 0x80 || (st == 0x90 && r.data[2] == 0)) {
                if (on_frame[ch][p] < 0) continue;
                gint64 dur = r.frame - on_frame[ch][p]; if (dur < 0) dur = 0;
                MidiNote n = { (guint32)((double)on_frame[ch][p] / f_per_tick),
                               (guint32)((double)dur / f_per_tick),
                               (guint8)p, on_vel[ch][p], (guint8)ch };
                if (n.length < 1) n.length = 1;
                midi_clip_add_note(c, n);
                on_frame[ch][p] = -1;
            }
        }

        /* Close notes still held at the stop point (no note-off was captured).
         *
         * Notes are stored at ABSOLUTE ticks (tick 0 = timeline frame 0), which
         * is what the paired-note path above does. This path used to subtract
         * `origin`, so a note still held when recording stopped landed earlier
         * than the notes around it by the whole record start offset. */
        off_t close_frame = (cut > last_frame) ? cut : last_frame;
        for (int ch = 0; ch < 16; ch++)
            for (int p = 0; p < 128; p++) {
                if (on_frame[ch][p] < 0) continue;
                gint64 sf = on_frame[ch][p];
                gint64 ef = (gint64)close_frame;   if (ef < sf) ef = sf;
                MidiNote n = { (guint32)((double)sf / f_per_tick),
                               (guint32)((double)(ef - sf) / f_per_tick),
                               (guint8)p, on_vel[ch][p], (guint8)ch };
                if (n.length < 1) n.length = 1;
                midi_clip_add_note(c, n);
            }

        if (midi_clip_note_count(c) == 0) { midi_clip_free(c); continue; }

        /* Merge recorded notes into the track's single clip. */
        MidiClip *dst = jackdaw_track_get_midi_clip(t);
        for (guint ni = 0; ni < midi_clip_note_count(c); ni++)
            midi_clip_add_note(dst, *midi_clip_note(c, ni));
        midi_clip_free(c);
        /* Re-seed a default region if every section was moved off this track, so
         * the freshly recorded notes are audible. */
        jackdaw_track_ensure_midi_region(t);
        jackdaw_track_commit_midi(t, fpb);  /* publishes RT snapshot + redraws */
    }
    return G_SOURCE_REMOVE;
}

#define ENG_REC_PREVIEW_MAX 16384
const JackDawRecNote *jackdaw_engine_rec_preview(JackDawTrack *t, guint *count)
{
    static MidiRecEvent   ev[ENG_REC_PREVIEW_MAX];
    static JackDawRecNote notes[ENG_REC_PREVIEW_MAX];
    if (count) *count = 0;
    if (!t || !t->midi_rec_buf) return NULL;

    size_t avail = jack_ringbuffer_read_space(t->midi_rec_buf);
    guint  ne    = (guint)(avail / sizeof(MidiRecEvent));
    if (ne == 0) return NULL;
    if (ne > ENG_REC_PREVIEW_MAX) ne = ENG_REC_PREVIEW_MAX;
    size_t got = jack_ringbuffer_peek(t->midi_rec_buf, (char *)ev,
                                      (size_t)ne * sizeof(MidiRecEvent));
    ne = (guint)(got / sizeof(MidiRecEvent));

    off_t now = (off_t)engine.play_pos;
    gint  on_idx[16][128];
    for (int ch = 0; ch < 16; ch++)
        for (int p = 0; p < 128; p++) on_idx[ch][p] = -1;

    guint nn = 0;
    for (guint e = 0; e < ne; e++) {
        int st = ev[e].data[0] & 0xF0, ch = ev[e].data[0] & 0x0F,
            p  = ev[e].data[1] & 0x7F;
        gboolean is_on  = (st == 0x90 && ev[e].data[2] > 0);
        gboolean is_off = (st == 0x80 || (st == 0x90 && ev[e].data[2] == 0));

        /* Any note-on or note-off for this pitch ends a note that is still open,
         * so a released OR re-triggered note can never keep extending to the
         * playhead. Only the genuinely-held note (open at the end) stays "now". */
        if ((is_on || is_off) && on_idx[ch][p] >= 0) {
            notes[on_idx[ch][p]].end_frame = (off_t)ev[e].frame;
            on_idx[ch][p] = -1;
        }
        if (is_on) {
            if (nn >= ENG_REC_PREVIEW_MAX) break;
            notes[nn].start_frame = (off_t)ev[e].frame;
            notes[nn].end_frame   = now;            /* held -> extend to playhead */
            notes[nn].pitch       = (guint8)p;
            notes[nn].velocity    = ev[e].data[2];
            notes[nn].channel     = (guint8)ch;
            on_idx[ch][p] = (gint)nn;
            nn++;
        }
    }
    if (count) *count = nn;
    return nn ? notes : NULL;
}

void jackdaw_engine_stop_recording(void)
{
    g_atomic_int_and(&engine.transport_flags, ~ENGINE_RECORDING);

    /* A record count-in that hasn't engaged yet: clear it so the pre-roll won't
     * hand off into recording. The pre-opened slots are finalized (empty/dropped)
     * by the loop below, shared with the normal stop path. */
    g_atomic_int_set(&engine.countin_active, 0);
    g_atomic_int_set(&engine.countin_pending_rec, 0);

    /* Capture play_pos NOW — this is the exact cut point for all recording tracks.
     * Write expected_frames before setting finalize_req so the recorder thread sees
     * the cap (g_atomic_int_set below provides the release barrier). */
    off_t cut = (off_t)engine.play_pos;

    for (guint i = 0; i < JACKDAW_MAX_TRACKS; i++) {
        if (!recorder_slots[i].sf) continue;
        JackDawTrack *t = engine.slots[i];
        off_t exp = t ? (cut - t->rec_start_frame) : 0;
        recorder_slots[i].expected_frames = exp > 0 ? exp : 0;
        g_atomic_int_set(&recorder_slots[i].finalize_req, 1);
    }

    /* Silence anything still sounding on an instrument track before the take is
     * handed over. Stopping the transport flushes note-offs, but stopping only
     * the *recording* did not, so a key held at the stop point left the note
     * ringing on the plugin (and on any external synth) until the next flush. */
    for (guint i = 0; i < JACKDAW_MAX_TRACKS; i++)
        g_atomic_int_set(&eng_midi_flush[i], 1);

    /* Convert any captured MIDI into clips on the main thread (RT has stopped
     * writing the capture buffer now that RECORDING is cleared). */
    eng_midi_rec_cut = cut;
    g_idle_add(midi_finalize_idle, NULL);
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

/* Re-seek every feeder slot to the current play_pos without moving the
 * playhead, so the ringbuffers refill with audio that respects the current
 * loop state. While stopped this is a clean reset (RT is not draining, same as
 * jackdaw_engine_locate); while playing it is best-effort — already-buffered
 * audio plays out first. */
static void engine_loop_reseek(void)
{
    off_t pos = engine.play_pos;
    gboolean playing =
        (g_atomic_int_get(&engine.transport_flags) & ENGINE_PLAYING) != 0;
    for (guint i = 0; i < JACKDAW_MAX_TRACKS; i++) {
        JackDawTrack *t = engine.slots[i];
        if (!t) continue;
        if (!playing) {
            t->played_frames = pos;
            if (t->play_buf_L) jack_ringbuffer_reset(t->play_buf_L);
            if (t->play_buf_R) jack_ringbuffer_reset(t->play_buf_R);
        }
        feeder_slots[i].locate_frame = pos;
        g_atomic_int_set(&feeder_slots[i].locate_req, 1);
    }
}

void jackdaw_engine_set_loop_range(off_t start, off_t end)
{
    if (start < 0) start = 0;
    if (end   < 0) end   = 0;
    if (end < start) { off_t tmp = start; start = end; end = tmp; }
    engine.loop_start = start;
    engine.loop_end   = end;
    engine_loop_reseek();
}

void jackdaw_engine_get_loop_range(off_t *start, off_t *end)
{
    if (start) *start = engine.loop_start;
    if (end)   *end   = engine.loop_end;
}

void jackdaw_engine_set_loop_enabled(gboolean on)
{
    g_atomic_int_set(&engine.loop_enabled, on ? 1 : 0);
    engine_loop_reseek();
}

gboolean jackdaw_engine_get_loop_enabled(void)
{
    return g_atomic_int_get(&engine.loop_enabled) != 0;
}

gboolean jackdaw_engine_has_loop_region(void)
{
    return engine.loop_end > engine.loop_start;
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

void jackdaw_engine_get_master_peaks(gfloat *out_L, gfloat *out_R)
{
    if (out_L) { *out_L = engine.master_peak_L; engine.master_peak_L = 0.0f; }
    if (out_R) { *out_R = engine.master_peak_R; engine.master_peak_R = 0.0f; }
}

/* -----------------------------------------------------------------------
 * Render support (offline bounce). All functions here run on the render
 * worker or the main thread — never the RT callback.
 * ----------------------------------------------------------------------- */

/* Scratch frames for one reader read. Covers up to 6:1 upsample headroom for
 * the resampler, matching the feeder's FEEDER_RAW_FRAMES rationale. */
#define RR_RAW_FRAMES   (4096 * 6)
#define RR_MAX_CHANNELS 8

/* A synchronous, render-only equivalent of one feeder slot: reads a contiguous
 * span of a track's timeline audio into caller buffers, resampling clip→render
 * SR as needed. It deliberately duplicates ~60 lines of the feeder's clip-walk
 * (feeder_thread_func) rather than refactoring the live path, to avoid any risk
 * of regressing realtime playback. TODO: factor a shared core once stable. */
struct EngTrackReader {
    int                 render_sr;
    ClipRegionSnapshot *snap;       /* held ref; track regions are stable while
                                     * the engine is render-suspended */
    SNDFILE            *sf;
    int                 open_region;
    int                 open_clip_sr, open_clip_ch;
#ifdef HAVE_SAMPLERATE
    SRC_STATE          *src_L, *src_R;
#endif
    float              *raw;         /* RR_RAW_FRAMES * RR_MAX_CHANNELS */
    float              *mono;        /* RR_RAW_FRAMES */
};

static void eng_reader_close_file(EngTrackReader *r)
{
    if (r->sf) { sf_close(r->sf); r->sf = NULL; }
#ifdef HAVE_SAMPLERATE
    if (r->src_L) { src_delete(r->src_L); r->src_L = NULL; }
    if (r->src_R) { src_delete(r->src_R); r->src_R = NULL; }
#endif
    r->open_region = -1;
}

EngTrackReader *engine_track_reader_new(JackDawTrack *t, int render_sr)
{
    EngTrackReader *r = g_new0(EngTrackReader, 1);
    r->render_sr   = render_sr;
    r->open_region = -1;
    r->snap        = jackdaw_track_ref_snapshot(t);   /* may be NULL */
    r->raw         = g_new(float, RR_RAW_FRAMES * RR_MAX_CHANNELS);
    r->mono        = g_new(float, RR_RAW_FRAMES);
    return r;
}

void engine_track_reader_free(EngTrackReader *r)
{
    if (!r) return;
    eng_reader_close_file(r);
    if (r->snap) clip_region_snapshot_unref(r->snap);
    g_free(r->raw);
    g_free(r->mono);
    g_free(r);
}

/* Fill outL/outR (caller-owned, >= n frames) with the track's timeline audio in
 * [start, start+n), region gain applied, resampled to render_sr. Gaps/missing
 * files become silence. n must be <= RR_RAW_FRAMES. Returns FALSE (success). */
gboolean engine_track_reader_read(EngTrackReader *r, JackDawTrack *t,
                                  off_t start, jack_nframes_t n,
                                  float *outL, float *outR)
{
    (void)t;
    memset(outL, 0, n * sizeof(float));
    memset(outR, 0, n * sizeof(float));
    ClipRegionSnapshot *snap = r->snap;
    if (!snap || snap->n == 0) return FALSE;

    int sr = r->render_sr;
    jack_nframes_t done = 0;
    off_t pf = start;

    while (done < n) {
        jack_nframes_t want = n - done;

        /* Region covering pf, plus the nearest region start after pf. */
        ClipRegion *reg = NULL;
        int   reg_idx    = -1;
        off_t next_start = -1;
        for (int k = 0; k < snap->n; k++) {
            ClipRegion *cr = &snap->r[k];
            if (pf >= cr->tl_pos && pf < cr->tl_pos + cr->length) {
                reg = cr; reg_idx = k; break;
            }
            if (cr->tl_pos > pf && (next_start < 0 || cr->tl_pos < next_start))
                next_start = cr->tl_pos;
        }

        if (!reg) {
            /* Gap / before first / past end → leave silence, advance. */
            off_t sil = want;
            if (next_start >= 0) {
                off_t to_next = next_start - pf;
                if (to_next > 0 && to_next < (off_t)sil) sil = to_next;
            }
            done += (jack_nframes_t)sil;
            pf   += sil;
            if (next_start < 0) break;   /* nothing more ahead */
            continue;
        }

        int clip_sr = reg->clip ? reg->clip->info.samplerate : sr;
        int clip_ch = reg->clip ? reg->clip->info.channels   : 1;
        int eff_ch  = clip_ch > RR_MAX_CHANNELS ? RR_MAX_CHANNELS : clip_ch;
        gboolean needs_src = (clip_sr != sr);
#ifndef HAVE_SAMPLERATE
        needs_src = FALSE;
#endif
        off_t d          = pf - reg->tl_pos;
        off_t reg_remain = reg->length - d;
        if (reg_remain <= 0) { pf = reg->tl_pos + reg->length; continue; }

        off_t chunk = want;
        if (chunk > reg_remain) chunk = (off_t)reg_remain;

        if (reg_idx != r->open_region || !r->sf) {
            eng_reader_close_file(r);
            SF_INFO sfi = {0};
            SNDFILE *sf = reg->clip ? sf_open(reg->clip->path, SFM_READ, &sfi)
                                    : NULL;
            if (!sf) { done += (jack_nframes_t)chunk; pf += chunk; continue; }
            off_t file_off = reg->file_in +
                ((clip_sr == sr) ? d
                                 : (off_t)((double)d * clip_sr / sr + 0.5));
            sf_seek(sf, file_off, SEEK_SET);
            r->sf = sf;
            r->open_clip_sr = clip_sr;
            r->open_clip_ch = clip_ch;
            r->open_region  = reg_idx;
#ifdef HAVE_SAMPLERATE
            if (needs_src) {
                int e = 0;
                r->src_L = src_new(SRC_SINC_FASTEST, 1, &e);
                if (eff_ch > 1) r->src_R = src_new(SRC_SINC_FASTEST, 1, &e);
            }
#endif
        }

        gfloat gain = reg->gain;
        float *dstL = outL + done;
        float *dstR = outR + done;

        if (!needs_src) {
            sf_count_t got = sf_readf_float(r->sf, r->raw, (sf_count_t)chunk);
            if (got < 0) got = 0;
            if (eff_ch == 1) {
                for (sf_count_t f = 0; f < got; f++)
                    dstL[f] = dstR[f] = r->raw[f] * gain;
            } else {
                for (sf_count_t f = 0; f < got; f++) {
                    dstL[f] = r->raw[f * eff_ch]     * gain;
                    dstR[f] = r->raw[f * eff_ch + 1] * gain;
                }
            }
            /* tail beyond `got` stays zero from the initial memset */
        }
#ifdef HAVE_SAMPLERATE
        else if (r->src_L) {
            double ratio   = (double)sr / (double)clip_sr;
            long   want_l  = (long)chunk;
            long   in_need = (long)ceil((double)chunk / ratio) + 8;
            if (in_need > RR_RAW_FRAMES) in_need = RR_RAW_FRAMES;
            int    eoi     = (chunk == reg_remain);

            sf_count_t got = sf_readf_float(r->sf, r->raw, (sf_count_t)in_need);
            if (got < 0) got = 0;

            for (sf_count_t f = 0; f < got; f++)
                r->mono[f] = r->raw[f * eff_ch];
            SRC_DATA sd_L = {
                .data_in = r->mono, .data_out = dstL,
                .input_frames = (long)got, .output_frames = want_l,
                .src_ratio = ratio, .end_of_input = eoi
            };
            src_process(r->src_L, &sd_L);
            long out_gen = sd_L.output_frames_gen;

            if (eff_ch > 1 && r->src_R) {
                for (sf_count_t f = 0; f < got; f++)
                    r->mono[f] = r->raw[f * eff_ch + 1];
                SRC_DATA sd_R = {
                    .data_in = r->mono, .data_out = dstR,
                    .input_frames = (long)got, .output_frames = want_l,
                    .src_ratio = ratio, .end_of_input = eoi
                };
                src_process(r->src_R, &sd_R);
            } else if (out_gen > 0) {
                memcpy(dstR, dstL, (size_t)out_gen * sizeof(float));
            }
            for (long x = 0; x < out_gen; x++) { dstL[x] *= gain; dstR[x] *= gain; }

            long used = sd_L.input_frames_used;
            if (used < got) sf_seek(r->sf, -(sf_count_t)(got - used), SEEK_CUR);
        }
#endif
        /* else: SRC needed but unavailable — leave silence */

        done += (jack_nframes_t)chunk;
        pf   += chunk;
    }
    return FALSE;
}

/* Render-only MIDI gather for an instrument track: emit just the sequenced
 * events from the published snapshot that fall in [blk_start, blk_start+n).
 * No live input, preview, or flush state (none of that applies offline). */
int eng_gather_render_midi(JackDawTrack *t, off_t blk_start,
                           jack_nframes_t nframes, PhMidiEvent *mev, int cap)
{
    int nev = 0;
    MidiEventSnapshot *ms = g_atomic_pointer_get(&t->rt_midi);
    if (!ms || !ms->n) return 0;

    off_t end = blk_start + nframes;
    guint lo = 0, hi = ms->n;             /* lower_bound(blk_start) */
    while (lo < hi) {
        guint mid = (lo + hi) / 2;
        if (ms->ev[mid].frame < blk_start) lo = mid + 1; else hi = mid;
    }
    for (guint e = lo; e < ms->n && ms->ev[e].frame < end && nev < cap; e++) {
        MidiSnapEvent *se = &ms->ev[e];
        mev[nev].time    = (guint32)(se->frame - blk_start);
        mev[nev].size    = 3;
        mev[nev].data[0] = se->s;
        mev[nev].data[1] = se->d1;
        mev[nev].data[2] = se->d2;
        nev++;
    }
    return nev;
}

void jackdaw_engine_render_suspend(gboolean on)
{
    g_atomic_int_set(&engine.render_suspend, on ? 1 : 0);
}

/* Suspend the live audio graph while the main thread instantiates or frees
 * plugins (project load, app teardown). Same effect as render_suspend: the RT
 * callback outputs silence and runs no plugins, so heavy non-RT work (dlopen,
 * setupProcessing, buffer allocation, dlclose) can't stall the audio thread
 * into an xrun. There is nothing to play during a load/quit anyway. */
void jackdaw_engine_set_suspended(gboolean on)
{
    g_atomic_int_set(&engine.render_suspend, on ? 1 : 0);
}

void jackdaw_engine_render_tap_start(off_t end_frame)
{
    size_t bytes = (size_t)engine.buf_size * 64 * sizeof(float);
    if (bytes < 65536) bytes = 65536;
    if (!engine.render_rb_L) engine.render_rb_L = jack_ringbuffer_create(bytes);
    if (!engine.render_rb_R) engine.render_rb_R = jack_ringbuffer_create(bytes);
    if (engine.render_rb_L) jack_ringbuffer_reset(engine.render_rb_L);
    if (engine.render_rb_R) jack_ringbuffer_reset(engine.render_rb_R);
    engine.render_end = end_frame;
    g_atomic_int_set(&engine.render_done, 0);
    g_atomic_int_set(&engine.render_active, 1);
}

void jackdaw_engine_render_tap_stop(void)
{
    g_atomic_int_set(&engine.render_active, 0);
    /* Rings are kept for reuse; freed in jackdaw_engine_quit(). */
}

gboolean jackdaw_engine_render_tap_done(void)
{
    return g_atomic_int_get(&engine.render_done) != 0;
}

size_t jackdaw_engine_render_tap_read(float *L, float *R, size_t max_frames)
{
    if (!engine.render_rb_L || !engine.render_rb_R) return 0;
    size_t avL = jack_ringbuffer_read_space(engine.render_rb_L) / sizeof(float);
    size_t avR = jack_ringbuffer_read_space(engine.render_rb_R) / sizeof(float);
    size_t av  = avL < avR ? avL : avR;
    if (av > max_frames) av = max_frames;
    if (av == 0) return 0;
    jack_ringbuffer_read(engine.render_rb_L, (char *)L, av * sizeof(float));
    jack_ringbuffer_read(engine.render_rb_R, (char *)R, av * sizeof(float));
    return av;
}

/* ---- Port enumeration ---- */

/* Build a gchar** list of external JACK ports of the given type and direction.
 * Excludes jackdaw's own ports. Caller must g_strfreev() the result. */
static gchar **list_ports(const char *type, unsigned long flags)
{
    if (!engine.active || !engine.client) return NULL;

    const char **ports = jack_get_ports(engine.client, NULL, type, flags);
    if (!ports) return NULL;

    const char *my_name = jack_get_client_name(engine.client);
    gsize        my_len  = strlen(my_name);

    GPtrArray *arr = g_ptr_array_new();
    for (const char **p = ports; *p; p++) {
        /* Skip jackdaw's own ports */
        if (strncmp(*p, my_name, my_len) == 0 && (*p)[my_len] == ':')
            continue;
        g_ptr_array_add(arr, g_strdup(*p));
    }
    jack_free(ports);

    if (arr->len == 0) {
        g_ptr_array_free(arr, TRUE);
        return NULL;
    }
    g_ptr_array_add(arr, NULL);
    return (gchar **)g_ptr_array_free(arr, FALSE);
}

gchar **jackdaw_engine_list_audio_sources(void)
{
    return list_ports(JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput);
}

gchar **jackdaw_engine_list_midi_sources(void)
{
    return list_ports(JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput);
}

/* ---- Track input routing ---- */

/* Switch a track between mono and stereo input. In stereo the track's right
 * capture port (in_NR) is registered so it appears in the patchbay; in mono it
 * is unregistered (and any right source disconnected) so mono tracks stay
 * single. Sets t->mono_record accordingly. Returns FALSE on success. */
gboolean jackdaw_engine_set_track_stereo(JackDawTrack *t, gboolean stereo)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), TRUE);

    t->mono_record = !stereo;

    if (!engine.active || !engine.client) return FALSE;

    gint ai = t->audio_in_idx;
    if (ai < 0 || (guint)ai >= engine.audio_in_count) return FALSE;

    if (stereo) {
        if (!engine.audio_in_r[(guint)ai]) {
            char name[64];
            g_snprintf(name, sizeof(name), "in_%uR", (guint)ai + 1);
            engine.audio_in_r[(guint)ai] = jack_port_register(engine.client,
                name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
            if (!engine.audio_in_r[(guint)ai]) return TRUE;
        }
    } else {
        /* Clear the right source first so the RT callback stops reading it,
         * then drop the port. */
        if (t->audio_src_port_r) {
            if (engine.audio_in_r[(guint)ai])
                jack_disconnect(engine.client, t->audio_src_port_r,
                                jack_port_name(engine.audio_in_r[(guint)ai]));
            g_clear_pointer(&t->audio_src_port_r, g_free);
        }
        if (engine.audio_in_r[(guint)ai]) {
            jack_port_unregister(engine.client, engine.audio_in_r[(guint)ai]);
            engine.audio_in_r[(guint)ai] = NULL;
        }
    }
    return FALSE;
}

gboolean jackdaw_engine_set_audio_source_l(JackDawTrack *t, const gchar *port_name)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), TRUE);
    if (!engine.active || !engine.client) return TRUE;

    gint ai = t->audio_in_idx;
    if (ai < 0 || (guint)ai >= engine.audio_in_count ||
        !engine.audio_in[(guint)ai]) return TRUE;

    const char *dst = jack_port_name(engine.audio_in[(guint)ai]);

    /* Disconnect current source if any */
    if (t->audio_src_port) {
        jack_disconnect(engine.client, t->audio_src_port, dst);
        g_clear_pointer(&t->audio_src_port, g_free);
    }

    if (port_name && *port_name) {
        int r = jack_connect(engine.client, port_name, dst);
        if (r != 0 && r != EEXIST) return TRUE;
        t->audio_src_port = g_strdup(port_name);
    }
    return FALSE;
}

gboolean jackdaw_engine_set_audio_source_r(JackDawTrack *t, const gchar *port_name)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), TRUE);
    if (!engine.active || !engine.client) return TRUE;

    gint ai = t->audio_in_idx;
    if (ai < 0 || (guint)ai >= engine.audio_in_count ||
        !engine.audio_in_r[(guint)ai]) return TRUE;

    const char *dst = jack_port_name(engine.audio_in_r[(guint)ai]);

    if (t->audio_src_port_r) {
        jack_disconnect(engine.client, t->audio_src_port_r, dst);
        g_clear_pointer(&t->audio_src_port_r, g_free);
    }

    if (port_name && *port_name) {
        int r = jack_connect(engine.client, port_name, dst);
        if (r != 0 && r != EEXIST) return TRUE;
        t->audio_src_port_r = g_strdup(port_name);
    }
    return FALSE;
}

/* Back-compat: setting "the" audio source sets the left channel only. */
gboolean jackdaw_engine_set_audio_source(JackDawTrack *t, const gchar *port_name)
{
    return jackdaw_engine_set_audio_source_l(t, port_name);
}

gboolean jackdaw_engine_set_midi_source(JackDawTrack *t, const gchar *port_name)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), TRUE);
    if (!engine.active || !engine.client) return TRUE;

    /* Clearing the source just detaches the track from its input port. The
     * capture→midi_in connection itself is system-wide (set up at startup and
     * possibly shared by other tracks), so we never tear it down here. */
    if (!port_name || !*port_name) {
        t->midi_in_idx = -1;
        g_clear_pointer(&t->midi_src_port, g_free);
        return FALSE;
    }

    /* Point the track at whichever midi_in port the chosen source feeds. Physical
     * captures are auto-connected by number at startup, so e.g. selecting
     * midi:capture_3 resolves to midi_in_3. Tracks sharing a source share that
     * port (read-only fan-out — fine for live input and instrument MIDI). */
    gint mi = engine_midi_in_for_source(port_name);

    /* A source that isn't a physical capture (e.g. a software synth's MIDI out)
     * has no pre-wired port — claim a free midi_in and connect it on demand. */
    if (mi < 0) {
        mi = engine_claim_free_midi_in(t);
        if (mi < 0 || (guint)mi >= engine.midi_in_count || !engine.midi_in[(guint)mi])
            return TRUE;
        int r = jack_connect(engine.client, port_name,
                             jack_port_name(engine.midi_in[(guint)mi]));
        if (r != 0 && r != EEXIST) return TRUE;
    }

    t->midi_in_idx = mi;
    g_free(t->midi_src_port);
    t->midi_src_port = g_strdup(port_name);
    return FALSE;
}
