#include <config.h>

#include "undo.h"

struct _JackDawUndoManager {
    GQueue *undo;   /* head = most recent; JackDawUndoAction* */
    GQueue *redo;
    guint   cap;
};

JackDawUndoManager *undo_manager_new(guint cap)
{
    JackDawUndoManager *m = g_new0(JackDawUndoManager, 1);
    m->undo = g_queue_new();
    m->redo = g_queue_new();
    m->cap  = cap ? cap : 64;
    return m;
}

/* Free an action and everything it owns. */
static void action_free(JackDawUndoAction *a)
{
    if (!a) return;
    if (a->saved_state && a->free_fn) a->free_fn(a->saved_state);
    if (a->ctx && a->ctx_free_fn)     a->ctx_free_fn(a->ctx);
    g_free(a->desc);
    g_free(a);
}

static void queue_clear(GQueue *q)
{
    JackDawUndoAction *a;
    while ((a = g_queue_pop_head(q))) action_free(a);
}

void undo_manager_clear(JackDawUndoManager *m)
{
    if (!m) return;
    queue_clear(m->undo);
    queue_clear(m->redo);
}

void undo_manager_free(JackDawUndoManager *m)
{
    if (!m) return;
    undo_manager_clear(m);
    g_queue_free(m->undo);
    g_queue_free(m->redo);
    g_free(m);
}

void undo_manager_push(JackDawUndoManager *m, const JackDawUndoAction *act)
{
    if (!m || !act) return;

    /* A new edit invalidates the redo history. */
    queue_clear(m->redo);

    JackDawUndoAction *a = g_new0(JackDawUndoAction, 1);
    *a = *act;                       /* shallow copy; we now own the pointers */
    g_queue_push_head(m->undo, a);

    /* Cap: evict (and free) the oldest. For a structural action this is what
     * finally drops the last ref to a deleted track, freeing it. */
    while (g_queue_get_length(m->undo) > m->cap)
        action_free(g_queue_pop_tail(m->undo));
}

gboolean undo_manager_can_undo(JackDawUndoManager *m)
{ return m && !g_queue_is_empty(m->undo); }

gboolean undo_manager_can_redo(JackDawUndoManager *m)
{ return m && !g_queue_is_empty(m->redo); }

/* Pop from `from`, swap state with the live target, push onto `to`. */
static void step(GQueue *from, GQueue *to)
{
    JackDawUndoAction *a = g_queue_pop_head(from);
    if (!a) return;
    gpointer cur = a->capture_fn ? a->capture_fn(a->ctx) : NULL;
    if (a->restore_fn) a->restore_fn(a->ctx, a->saved_state);
    if (a->saved_state && a->free_fn) a->free_fn(a->saved_state);
    a->saved_state = cur;            /* now holds the opposite-direction state */
    if (a->after_fn) a->after_fn(a->ctx);
    g_queue_push_head(to, a);
}

void undo_manager_undo(JackDawUndoManager *m)
{
    if (!m) return;
    step(m->undo, m->redo);
}

void undo_manager_redo(JackDawUndoManager *m)
{
    if (!m) return;
    step(m->redo, m->undo);
}
