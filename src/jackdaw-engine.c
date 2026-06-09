#define _GNU_SOURCE
#include <config.h>

#include <string.h>
#include <math.h>
#include <jack/jack.h>
#include <jack/midiport.h>
#include <jack/ringbuffer.h>

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
         *   angle = (pan + 1.0) * M_PI_4  maps [-1,1] → [0, π/2]
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
 * Buffer size callback — called by JACK when buffer size changes
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
    }

    return 0;
}

static void engine_shutdown_cb(void *arg)
{
    (void)arg;
    engine.active = FALSE;
    /* The main thread checks engine.active periodically via mainloop */
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
    engine.midi_in_count   = project->midi_in_count > 16u ? 16u : project->midi_in_count;

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

    engine.active = TRUE;
    (void)rb_bytes; /* used by engine_add_track */
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
