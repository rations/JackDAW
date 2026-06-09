#ifndef JACKDAW_ENGINE_H_INCLUDED
#define JACKDAW_ENGINE_H_INCLUDED

#include <jack/jack.h>
#include <jack/midiport.h>
#include "project.h"
#include "track.h"

G_BEGIN_DECLS

/*
 * jackdaw-engine: one JACK client, named "jackdaw".
 *
 * Registered ports:
 *   audio input:  in_1..in_N
 *   audio output: out_1..out_N  (out_1=L master, out_2=R master; out_3+ unused)
 *   midi input:   midi_in_1..midi_in_M
 *
 * All tracks mix to out_1/out_2.  jackdaw never calls jack_connect().
 */

/* Initialise the engine and activate the JACK client.
 * Must be called from the main thread before adding any tracks.
 * Returns FALSE on success, TRUE on failure (mhwaveedit convention). */
gboolean jackdaw_engine_init(JackDawProject *project);

/* Deactivate and close the JACK client. Safe to call even if not initialised. */
void jackdaw_engine_quit(void);

/* Returns TRUE if the JACK client is currently active. */
gboolean jackdaw_engine_is_running(void);

/* --- Port count management ---
 * Unregisters ports being removed and registers new ones.
 * Saves the new count to inifile and emits project::ports-changed.
 * Returns FALSE on success, TRUE on failure. */
gboolean jackdaw_engine_set_audio_in_count (guint n);
gboolean jackdaw_engine_set_audio_out_count(guint n);
gboolean jackdaw_engine_set_midi_in_count  (guint n);

/* Query current port counts */
guint jackdaw_engine_get_audio_in_count (void);
guint jackdaw_engine_get_audio_out_count(void);
guint jackdaw_engine_get_midi_in_count  (void);

/* Return a port handle for the track input selector.
 * Caller may call jack_port_short_name() and jack_port_get_connections(). */
jack_port_t *jackdaw_engine_get_audio_in_port(guint idx);
jack_port_t *jackdaw_engine_get_midi_in_port (guint idx);

/* --- Track management --- */
/* Add a track to the engine; allocates track->slot and registers per-track
 * output ports. Returns FALSE on success, TRUE on failure. */
gboolean jackdaw_engine_add_track   (JackDawTrack *track);
void     jackdaw_engine_remove_track(JackDawTrack *track);

/* --- Transport --- */
void jackdaw_engine_start_playback (void);
void jackdaw_engine_stop_playback  (void);
void jackdaw_engine_start_recording(void);
void jackdaw_engine_stop_recording (void);
void jackdaw_engine_locate         (off_t sample);

/* Sample rate reported by JACK (valid after jackdaw_engine_init) */
jack_nframes_t jackdaw_engine_get_sample_rate(void);
jack_nframes_t jackdaw_engine_get_buffer_size(void);

/* Monotonically-increasing playback position in samples.
 * Reset by jackdaw_engine_locate(); increments nframes per process cycle
 * while ENGINE_PLAYING is set. Read by main thread for display only. */
off_t jackdaw_engine_get_play_pos(void);

G_END_DECLS

#endif /* JACKDAW_ENGINE_H_INCLUDED */
