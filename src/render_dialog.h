#ifndef RENDER_DIALOG_H_INCLUDED
#define RENDER_DIALOG_H_INCLUDED

#include <gtk/gtk.h>
#include "project.h"
#include "render.h"

G_BEGIN_DECLS

/* Open the modal Render dialog. `initial_scope` preselects Entire-project vs
 * Selected-region (the File menu's "Render…" vs "Render Region…"). On OK it
 * launches the render and shows a progress popup. */
void render_dialog_open(GtkWindow *parent, JackDawProject *project,
                        RenderScope initial_scope);

G_END_DECLS

#endif /* RENDER_DIALOG_H_INCLUDED */
