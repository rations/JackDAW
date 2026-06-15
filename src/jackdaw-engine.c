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
#ifdef HAVE_SAMPLERATE
#  include <samplerate.h>
#endif

#include "jackdaw-engine.h"
#include "pluginhost.h"
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
    jack_port_t **midi_out;

    guint audio_in_count;
    guint audio_out_count;
    guint midi_in_count;
    guint midi_out_count;

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

    jack_nframes_t sample_rate;    /* cached at init */

    /* Pre-rendered metronome click (mono), built at init. */
    float *click_buf;
    int    click_len;

    /* Post-master-fader peak meter (master VU). Racy read is acceptable. */
    volatile gfloat master_peak_L;
    volatile gfloat master_peak_R;

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

    if (nev > 1) qsort(mev, nev, sizeof(PhMidiEvent), eng_midi_cmp);
    return nev;
}

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

    /* Second pass: process each track */
    for (i = 0; i < JACKDAW_MAX_TRACKS; i++) {
        JackDawTrack *t = engine.slots[i];
        if (!t) continue;

        gint tflags = g_atomic_int_get(&t->state_flags);
        gboolean skip = (tflags & TRACK_MUTED) ||
                        (any_soloed && !(tflags & TRACK_SOLOED));

        if (skip) continue;

        gboolean instr = jackdaw_track_is_instrument(t);
        size_t want = nframes * sizeof(float);

        if (instr) {
            /* Instrument track: the signal is generated by the instrument from
             * MIDI, so start from silence (no playback ringbuffer). */
            memset(engine.tmp_L, 0, want);
            memset(engine.tmp_R, 0, want);
        } else {
            /* Drain playback ringbuffers */
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
        }

        /* Live input monitoring: when armed, the input is summed into the
         * track signal (so it is heard through the FX chain), and the dry input
         * is captured to rec_buf when recording. */
        float *live_in = NULL;
        if (!instr && (tflags & TRACK_ARMED) && t->audio_in_idx >= 0 &&
            (guint)t->audio_in_idx < engine.audio_in_count &&
            engine.audio_in[(guint)t->audio_in_idx]) {
            live_in = jack_port_get_buffer(
                engine.audio_in[(guint)t->audio_in_idx], nframes);
        }
        if (live_in) {
            gfloat wf_mn = 0.0f, wf_mx = 0.0f;
            for (k = 0; k < nframes; k++) {
                float s = live_in[k];
                engine.tmp_L[k] += s;   /* pre-FX, pre-fader monitor sum */
                engine.tmp_R[k] += s;
                if (s < wf_mn) wf_mn = s;
                if (s > wf_mx) wf_mx = s;
            }
            /* Store one peak pair per JACK period for the real-time waveform */
            if ((flags & ENGINE_RECORDING) && t->rec_peak_buf) {
                gint pk = t->rec_peak_count;
                if (pk < REC_PEAK_MAX_BUCKETS) {
                    t->rec_peak_buf[pk * 2]     = wf_mn;
                    t->rec_peak_buf[pk * 2 + 1] = wf_mx;
                    t->rec_peak_count = pk + 1;
                }
            }
        }

        /* Run the per-track FX chain in place (pre-fader). The chain is an
         * immutable snapshot published by the main thread; pluginhost_process
         * is RT-safe and skips bypassed effects. */
        JackDawFxChain *chain = g_atomic_pointer_get(&t->rt_chain);
        if (instr) {
            /* Instrument track: fx[0] is the instrument — feed it this block's
             * MIDI (sequenced + live + flush) to render audio; fx[1..] are audio
             * effects applied after it. With no instrument loaded, stays silent. */
            PhMidiEvent mev[ENG_MIDI_MAX_EV];
            int nev = eng_gather_instrument_midi((int)i, t, blk_start, nframes,
                                                 (flags & ENGINE_PLAYING) != 0,
                                                 (tflags & TRACK_ARMED) != 0,
                                                 mev, ENG_MIDI_MAX_EV);
            if (chain && chain->n > 0) {
                pluginhost_process_midi((PluginInstance *)chain->fx[0], mev, nev,
                                        engine.tmp_L, engine.tmp_R, (int)nframes);
                for (int fi = 1; fi < chain->n; fi++)
                    pluginhost_process((PluginInstance *)chain->fx[fi],
                                       engine.tmp_L, engine.tmp_R, (int)nframes);
            }
        } else if (chain) {
            for (int fi = 0; fi < chain->n; fi++)
                pluginhost_process((PluginInstance *)chain->fx[fi],
                                   engine.tmp_L, engine.tmp_R, (int)nframes);
        }

        /* Constant-power pan law:
         *   angle = (pan + 1.0) * M_PI_4  maps [-1,1] -> [0, pi/2]
         *   L gain = vol * cos(angle), R gain = vol * sin(angle) */
        gfloat vol   = t->volume;
        gfloat pan   = t->pan;
        float angle  = (pan + 1.0f) * (float)M_PI_4;
        float gain_L = vol * cosf(angle);
        float gain_R = vol * sinf(angle);

        gfloat peak_L = 0.0f, peak_R = 0.0f;

        /* Apply fader/pan into the master mix and meter post-FX/post-fader. */
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

        /* Update peaks for VU — ballistic decay-hold applied here so the value
         * can be read non-destructively by any number of meters (track strip
         * AND mixer). Racy write/read is acceptable. */
        t->peak_L = (peak_L > t->peak_L) ? peak_L : t->peak_L * 0.92f;
        t->peak_R = (peak_R > t->peak_R) ? peak_R : t->peak_R * 0.92f;

        /* Capture live input to rec ringbuffers when recording */
        if (live_in && (flags & ENGINE_RECORDING)) {
            if (t->rec_buf_L)
                jack_ringbuffer_write(t->rec_buf_L,
                                      (const char *)live_in,
                                      nframes * sizeof(float));
            if (t->rec_buf_R)
                jack_ringbuffer_write(t->rec_buf_R,
                                      (const char *)live_in,
                                      nframes * sizeof(float));
        }

        /* Record MIDI: capture each event with its ABSOLUTE timeline frame so
         * the main thread can place it into a clip on stop. Instrument tracks
         * only; the same JACK port also feeds the live monitor above. */
        if (instr && (tflags & TRACK_ARMED) && (flags & ENGINE_RECORDING) &&
            t->midi_in_idx >= 0 &&
            (guint)t->midi_in_idx < engine.midi_in_count &&
            engine.midi_in[t->midi_in_idx] && t->midi_rec_buf) {
            void *mbuf = jack_port_get_buffer(
                engine.midi_in[t->midi_in_idx], nframes);
            uint32_t mc = jack_midi_get_event_count(mbuf);
            for (uint32_t m = 0; m < mc; m++) {
                jack_midi_event_t ev;
                if (jack_midi_event_get(&ev, mbuf, m) != 0 || ev.size < 1)
                    continue;
                if (!(ev.buffer[0] & 0x80)) continue;  /* status byte only */
                MidiRecEvent r;
                r.frame   = (gint64)(blk_start + (off_t)ev.time);
                r.size    = (guint8)(ev.size > 3 ? 3 : ev.size);
                r.data[0] = ev.buffer[0];
                r.data[1] = ev.size > 1 ? ev.buffer[1] : 0;
                r.data[2] = ev.size > 2 ? ev.buffer[2] : 0;
                if (jack_ringbuffer_write_space(t->midi_rec_buf) >= sizeof r)
                    jack_ringbuffer_write(t->midi_rec_buf,
                                          (const char *)&r, sizeof r);
            }
        }
    }

    /* Metronome click — mixed into the master before the master fader. */
    if ((flags & ENGINE_PLAYING) && engine.project &&
        engine.project->metronome_enabled && engine.click_buf &&
        engine.click_len > 0 && engine.project->bpm > 0.0) {
        double fpb = (double)engine.sample_rate * 60.0 / engine.project->bpm;
        guint  bpb = engine.project->beats_per_bar
                     ? engine.project->beats_per_bar : 4;
        float  click_gain = engine.project->metronome_gain;
        if (fpb > 1.0) {
            off_t base = engine.play_pos - (off_t)nframes;
            for (k = 0; k < nframes; k++) {
                off_t a = base + (off_t)k;
                if (a < 0) continue;
                off_t beat     = (off_t)((double)a / fpb);
                off_t boundary = (off_t)((double)beat * fpb + 0.5);
                off_t off      = a - boundary;
                if (off >= 0 && off < engine.click_len) {
                    float s = engine.click_buf[off] * click_gain;
                    if ((beat % (off_t)bpb) != 0) s *= 0.45f; /* accent downbeat */
                    engine.master_L[k] += s;
                    engine.master_R[k] += s;
                }
            }
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

    /* Clear all MIDI output buffers before any writes */
    for (oi = 0; oi < engine.midi_out_count; oi++) {
        if (!engine.midi_out[oi]) continue;
        void *mbuf = jack_port_get_buffer(engine.midi_out[oi], nframes);
        jack_midi_clear_buffer(mbuf);
    }

    /* MIDI thru: for each armed track, copy midi_in_N → midi_out_N */
    for (i = 0; i < JACKDAW_MAX_TRACKS; i++) {
        JackDawTrack *t = engine.slots[i];
        if (!t) continue;
        gint tflags = g_atomic_int_get(&t->state_flags);
        if (!(tflags & TRACK_ARMED)) continue;
        gint mi = t->midi_in_idx;
        if (mi < 0 || (guint)mi >= engine.midi_in_count  || !engine.midi_in[mi])  continue;
        if (              (guint)mi >= engine.midi_out_count || !engine.midi_out[mi]) continue;

        void *ibuf = jack_port_get_buffer(engine.midi_in[mi],  nframes);
        void *obuf = jack_port_get_buffer(engine.midi_out[mi], nframes);
        uint32_t mc = jack_midi_get_event_count(ibuf);
        uint32_t m;
        for (m = 0; m < mc; m++) {
            jack_midi_event_t ev;
            if (jack_midi_event_get(&ev, ibuf, m) != 0) continue;
            jack_midi_event_write(obuf, ev.time, ev.buffer, ev.size);
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
            if (!found) g_clear_pointer(&t->midi_src_port, g_free);
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
    engine.master_L = g_malloc0(bs * sizeof(float));
    engine.master_R = g_malloc0(bs * sizeof(float));
    engine.tmp_L    = g_malloc0(bs * sizeof(float));
    engine.tmp_R    = g_malloc0(bs * sizeof(float));

    /* Register callbacks */
    jack_set_process_callback(engine.client, engine_process, NULL);
    jack_set_buffer_size_callback(engine.client, engine_buffer_size_cb, NULL);
    jack_on_shutdown(engine.client, engine_shutdown_cb, NULL);
    jack_set_port_registration_callback(engine.client, engine_port_reg_cb, project);
    jack_set_port_connect_callback(engine.client, engine_port_connect_cb, project);

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
    engine.midi_in = g_new0(jack_port_t *, engine.midi_in_count);
    for (i = 0; i < engine.midi_in_count; i++) {
        g_snprintf(name, sizeof(name), "midi_in_%u", i + 1);
        engine.midi_in[i] = jack_port_register(engine.client, name,
            JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
        if (!engine.midi_in[i]) goto fail;
    }

    /* Register MIDI output ports: midi_out_1 .. midi_out_M */
    engine.midi_out = g_new0(jack_port_t *, engine.midi_out_count);
    for (i = 0; i < engine.midi_out_count; i++) {
        g_snprintf(name, sizeof(name), "midi_out_%u", i + 1);
        engine.midi_out[i] = jack_port_register(engine.client, name,
            JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);
        if (!engine.midi_out[i]) goto fail;
    }

    /* Activate — after this the process callback can be called at any time */
    if (jack_activate(engine.client) != 0) {
        user_error("jackdaw: jack_activate() failed");
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

    /* Start background threads */
    feeder_start();
    recorder_start();

    engine.active = TRUE;
    (void)rb_bytes; /* suppress unused-variable warning */
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

    jack_deactivate(engine.client);
    jack_client_close(engine.client);
    engine.client = NULL;
    engine.active = FALSE;

    g_free(engine.master_L); engine.master_L = NULL;
    g_free(engine.master_R); engine.master_R = NULL;
    g_free(engine.tmp_L);    engine.tmp_L    = NULL;
    g_free(engine.tmp_R);    engine.tmp_R    = NULL;
    g_free(engine.click_buf); engine.click_buf = NULL; engine.click_len = 0;
    g_free(engine.audio_in);  engine.audio_in  = NULL;
    g_free(engine.audio_out); engine.audio_out = NULL;
    g_free(engine.midi_in);   engine.midi_in   = NULL;
    g_free(engine.midi_out);  engine.midi_out  = NULL;
}

gboolean jackdaw_engine_is_running(void)
{
    return engine.active;
}

gboolean jackdaw_engine_is_recording(void)
{
    return (g_atomic_int_get(&engine.transport_flags) & ENGINE_RECORDING) != 0;
}

gboolean jackdaw_engine_is_playing(void)
{
    return (g_atomic_int_get(&engine.transport_flags) & ENGINE_PLAYING) != 0;
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
    engine.midi_in = g_renew(jack_port_t *, engine.midi_in, n);
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

gboolean jackdaw_engine_set_midi_out_count(guint n)
{
    guint i;
    char name[64];
    n = CLAMP(n, 1, 16);
    if (!engine.active) { engine.midi_out_count = n; return FALSE; }

    for (i = n; i < engine.midi_out_count; i++) {
        if (engine.midi_out[i])
            jack_port_unregister(engine.client, engine.midi_out[i]);
    }
    engine.midi_out = g_renew(jack_port_t *, engine.midi_out, n);
    for (i = engine.midi_out_count; i < n; i++) {
        g_snprintf(name, sizeof(name), "midi_out_%u", i + 1);
        engine.midi_out[i] = jack_port_register(engine.client, name,
            JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);
        if (!engine.midi_out[i]) return TRUE;
    }
    engine.midi_out_count = n;
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

    /* Auto-assign dedicated input ports: slot i maps to audio_in[i] / midi_in[i].
     * If the slot index exceeds configured port counts the track has no input. */
    track->audio_in_idx = ((guint)i < engine.audio_in_count) ? (gint)i : -1;
    track->midi_in_idx  = ((guint)i < engine.midi_in_count)  ? (gint)i : -1;

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
        if (track->midi_src_port && track->midi_in_idx >= 0 &&
            (guint)track->midi_in_idx < engine.midi_in_count &&
            engine.midi_in[(guint)track->midi_in_idx]) {
            jack_disconnect(engine.client, track->midi_src_port,
                            jack_port_name(engine.midi_in[(guint)track->midi_in_idx]));
        }
    }
    g_clear_pointer(&track->audio_src_port, g_free);
    g_clear_pointer(&track->midi_src_port,  g_free);

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

void jackdaw_engine_start_recording(void)
{
    for (guint i = 0; i < JACKDAW_MAX_TRACKS; i++) {
        JackDawTrack *t = engine.slots[i];
        if (!t) continue;
        if (!(g_atomic_int_get(&t->state_flags) & TRACK_ARMED)) continue;

        /* Record start frame for every armed track (audio + instrument). */
        t->rec_start_frame = (off_t)engine.play_pos;

        /* Instrument tracks capture live MIDI into a clip — clear the capture
         * ringbuffer now (RECORDING isn't set until the end of this function,
         * so the RT thread is not yet writing to it). No WAV file. */
        if (jackdaw_track_is_instrument(t)) {
            if (t->midi_rec_buf) jack_ringbuffer_reset(t->midi_rec_buf);
            continue;
        }

        if (t->audio_in_idx < 0) continue;
        recorder_open_slot(i, t, (off_t)engine.play_pos, 0 /* unlimited */);
    }

    /* Start rolling — RT callback begins filling rec_buf immediately */
    g_atomic_int_or(&engine.transport_flags, ENGINE_RECORDING | ENGINE_PLAYING);
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
        guint32   max_end    = 0;
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
                if (n.start + n.length > max_end) max_end = n.start + n.length;
                on_frame[ch][p] = -1;
            }
        }

        /* Close notes still held at the stop point (no note-off was captured). */
        off_t close_frame = (cut > last_frame) ? cut : last_frame;
        for (int ch = 0; ch < 16; ch++)
            for (int p = 0; p < 128; p++) {
                if (on_frame[ch][p] < 0) continue;
                gint64 sf = on_frame[ch][p] - origin;       if (sf < 0) sf = 0;
                gint64 ef = (gint64)close_frame - origin;   if (ef < sf) ef = sf;
                MidiNote n = { (guint32)((double)sf / f_per_tick),
                               (guint32)((double)(ef - sf) / f_per_tick),
                               (guint8)p, on_vel[ch][p], (guint8)ch };
                if (n.length < 1) n.length = 1;
                midi_clip_add_note(c, n);
                if (n.start + n.length > max_end) max_end = n.start + n.length;
            }

        if (midi_clip_note_count(c) == 0) { midi_clip_free(c); continue; }

        /* Merge recorded notes into the track's single clip. */
        MidiClip *dst = jackdaw_track_get_midi_clip(t);
        for (guint ni = 0; ni < midi_clip_note_count(c); ni++)
            midi_clip_add_note(dst, *midi_clip_note(c, ni));
        midi_clip_free(c);
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

gboolean jackdaw_engine_set_audio_source(JackDawTrack *t, const gchar *port_name)
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

gboolean jackdaw_engine_set_midi_source(JackDawTrack *t, const gchar *port_name)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), TRUE);
    if (!engine.active || !engine.client) return TRUE;

    gint mi = t->midi_in_idx;
    if (mi < 0 || (guint)mi >= engine.midi_in_count ||
        !engine.midi_in[(guint)mi]) return TRUE;

    const char *dst = jack_port_name(engine.midi_in[(guint)mi]);

    if (t->midi_src_port) {
        jack_disconnect(engine.client, t->midi_src_port, dst);
        g_clear_pointer(&t->midi_src_port, g_free);
    }

    if (port_name && *port_name) {
        int r = jack_connect(engine.client, port_name, dst);
        if (r != 0 && r != EEXIST) return TRUE;
        t->midi_src_port = g_strdup(port_name);
    }
    return FALSE;
}
