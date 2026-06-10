#ifndef ALSA_MIDI_H_INCLUDED
#define ALSA_MIDI_H_INCLUDED

#include <glib.h>

G_BEGIN_DECLS

/*
 * alsa-midi: enumerate ALSA sequencer ports that can act as MIDI sources.
 *
 * Returns a newly-allocated NULL-terminated array of strings in the format
 * "ClientName:PortName".  Caller must g_strfreev() the result.
 * Returns NULL if ALSA is unavailable or no readable ports exist.
 *
 * This function opens and closes the ALSA sequencer handle each call, so
 * call it only from the main thread and only when refreshing a UI list.
 */
gchar **alsa_midi_list_sources(void);

G_END_DECLS

#endif /* ALSA_MIDI_H_INCLUDED */
