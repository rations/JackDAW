#ifndef MIDIWINDOW_H_INCLUDED
#define MIDIWINDOW_H_INCLUDED

#include <gtk/gtk.h>
#include "track.h"
#include "midiclip.h"
#include "project.h"

G_BEGIN_DECLS

/* Open (or present) the piano-roll editor for `track`. Singleton per track. */
void jackdaw_midi_window_open(JackDawTrack *track, JackDawProject *project);

G_END_DECLS

#endif /* MIDIWINDOW_H_INCLUDED */
