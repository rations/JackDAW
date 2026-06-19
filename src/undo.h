#ifndef UNDO_H_INCLUDED
#define UNDO_H_INCLUDED

#include <glib.h>

G_BEGIN_DECLS

/*
 * Global undo/redo for JackDAW.
 *
 * One chronological history for the whole project, owned by JackDawProject and
 * reachable from every window (main/midi/fx). A single Ctrl+Z undoes the last
 * action regardless of which subsystem produced it.
 *
 * Each action is a memento (capture/restore/free): the action stores the state
 * BEFORE the edit; undo() swaps it with the current state and moves the action
 * to the redo stack, so redo re-applies. This exactly generalizes the old
 * per-track region-snapshot undo:
 *   capture_fn  = clip_region_list_copy(track regions)
 *   restore_fn  = timeline_apply_regions(track, list)
 *   free_fn     = g_ptr_array_unref
 *
 * Subsystems push an action AFTER capturing the pre-edit state and BEFORE (or
 * just after) mutating; capture/restore/free encapsulate the rest.
 */

typedef struct _JackDawUndoManager JackDawUndoManager;

typedef struct {
    gpointer  ctx;                  /* edit target (JackDawTrack*/
                                    /* JackDawProject*/ /* or a small struct) */
    gpointer  saved_state;          /* pre-edit memento (capture_fn output) */
    gpointer (*capture_fn)(gpointer ctx);                 /* snapshot current */
    void      (*restore_fn)(gpointer ctx, gpointer state);/* apply a snapshot */
    void      (*free_fn)(gpointer state);                 /* free a memento */
    void      (*after_fn)(gpointer ctx);                  /* optional: redraw */
    void      (*ctx_free_fn)(gpointer ctx);  /* optional: free ctx on drop */
    char     *desc;                 /* short label, optional */
} JackDawUndoAction;

JackDawUndoManager *undo_manager_new(guint cap);
void     undo_manager_free (JackDawUndoManager *m);
void     undo_manager_clear(JackDawUndoManager *m);

/* Push a new action. Takes ownership of saved_state/ctx/desc (freed via the
 * provided free_fn/ctx_free_fn/g_free). Clears the redo stack. A copy of the
 * struct is stored; the caller's struct may be stack-allocated. */
void     undo_manager_push (JackDawUndoManager *m, const JackDawUndoAction *act);

gboolean undo_manager_can_undo(JackDawUndoManager *m);
gboolean undo_manager_can_redo(JackDawUndoManager *m);
void     undo_manager_undo (JackDawUndoManager *m);
void     undo_manager_redo (JackDawUndoManager *m);

G_END_DECLS

#endif /* UNDO_H_INCLUDED */
