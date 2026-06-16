#ifndef FXWINDOW_H_INCLUDED
#define FXWINDOW_H_INCLUDED

#include <gtk/gtk.h>
#include "track.h"
#include "project.h"

G_BEGIN_DECLS

/* Open (or present, if already open) the per-track FX window. */
void jackdaw_fx_window_open(JackDawTrack *track, JackDawProject *project);

/* Open the global plugin paths / rescan dialog. */
void jackdaw_fx_paths_dialog(GtkWindow *parent);

/* Launch-time plugin scan: shows a progress dialog while scanning, then pops a
 * scrollable list of any plugins added since the last run (nothing if none). */
void jackdaw_fx_startup_scan(GtkWindow *parent);

G_END_DECLS

#endif /* FXWINDOW_H_INCLUDED */
