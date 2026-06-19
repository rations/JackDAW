#include <config.h>

#include <string.h>
#include "midiclip.h"

/* ---- MidiClip ---- */

MidiClip *midi_clip_new(guint32 length_ticks)
{
    MidiClip *c = g_new0(MidiClip, 1);
    c->notes    = g_array_new(FALSE, FALSE, sizeof(MidiNote));
    c->length   = length_ticks;
    c->refcount = 1;
    return c;
}

MidiClip *midi_clip_ref(MidiClip *c)
{
    if (c) g_atomic_int_inc(&c->refcount);
    return c;
}

void midi_clip_free(MidiClip *c)
{
    if (!c) return;
    if (!g_atomic_int_dec_and_test(&c->refcount)) return;
    if (c->notes) g_array_free(c->notes, TRUE);
    g_free(c);
}

MidiClip *midi_clip_copy(MidiClip *c)
{
    if (!c) return NULL;
    MidiClip *d = midi_clip_new(c->length);
    if (c->notes && c->notes->len > 0)
        g_array_append_vals(d->notes, c->notes->data, c->notes->len);
    return d;
}

guint midi_clip_add_note(MidiClip *c, MidiNote note)
{
    g_array_append_val(c->notes, note);
    return c->notes->len - 1;
}

void midi_clip_remove_note(MidiClip *c, guint index)
{
    if (index < c->notes->len)
        g_array_remove_index(c->notes, index);
}

guint midi_clip_note_count(MidiClip *c)
{
    return c ? c->notes->len : 0;
}

MidiNote *midi_clip_note(MidiClip *c, guint index)
{
    if (!c || index >= c->notes->len) return NULL;
    return &g_array_index(c->notes, MidiNote, index);
}

/* ---- MidiRegion ---- */

MidiRegion *midi_region_new(MidiClip *clip, guint32 clip_in,
                            guint32 length, off_t tl_pos)
{
    MidiRegion *r = g_new0(MidiRegion, 1);
    r->clip    = midi_clip_ref(clip);
    r->clip_in = clip_in;
    r->length  = length;
    r->tl_pos  = tl_pos;
    return r;
}

void midi_region_free(MidiRegion *r)
{
    if (!r) return;
    midi_clip_free(r->clip);
    g_free(r);
}

GPtrArray *midi_region_list_new(void)
{
    return g_ptr_array_new_with_free_func((GDestroyNotify)midi_region_free);
}

/* ---- Event snapshot ---- */

static inline off_t ticks_to_frames(guint32 ticks, double frames_per_beat)
{
    /* PPQ ticks per quarter note; one beat = one quarter note here. */
    return (off_t)((double)ticks / (double)JACKDAW_PPQ * frames_per_beat + 0.5);
}

static int snap_ev_cmp(const void *a, const void *b)
{
    const MidiSnapEvent *ea = a, *eb = b;
    if (ea->frame < eb->frame) return -1;
    if (ea->frame > eb->frame) return  1;
    /* Same frame: emit note-offs (0x80) before note-ons (0x90) so a retrigger
     * of the same pitch doesn't get immediately silenced. */
    return (int)(ea->s & 0xF0) - (int)(eb->s & 0xF0);
}

MidiEventSnapshot *midi_event_snapshot_new(MidiClip *clip,
                                           double frames_per_beat)
{
    MidiEventSnapshot *s = g_new0(MidiEventSnapshot, 1);
    if (!clip || !clip->notes || clip->notes->len == 0 || frames_per_beat <= 0.0) {
        s->n = 0; s->ev = NULL;
        return s;
    }

    GArray *out = g_array_new(FALSE, FALSE, sizeof(MidiSnapEvent));

    for (guint ni = 0; ni < clip->notes->len; ni++) {
        MidiNote *nt = &g_array_index(clip->notes, MidiNote, ni);
        if (nt->velocity == 0) continue;
        guint8 ch = nt->channel & 0x0F;
        MidiSnapEvent on  = { ticks_to_frames(nt->start, frames_per_beat),
                              (guint8)(0x90 | ch), nt->pitch, nt->velocity };
        MidiSnapEvent off = { ticks_to_frames(nt->start + nt->length, frames_per_beat),
                              (guint8)(0x80 | ch), nt->pitch, 0 };
        g_array_append_val(out, on);
        g_array_append_val(out, off);
    }

    s->n  = out->len;
    s->ev = (MidiSnapEvent *)g_array_free(out, FALSE);
    if (s->n > 1)
        qsort(s->ev, s->n, sizeof(MidiSnapEvent), snap_ev_cmp);
    return s;
}

void midi_event_snapshot_free(MidiEventSnapshot *s)
{
    if (!s) return;
    g_free(s->ev);
    g_free(s);
}

off_t midi_event_snapshot_total_frames(const MidiEventSnapshot *s)
{
    if (!s || s->n == 0) return 0;
    return s->ev[s->n - 1].frame;   /* sorted ascending */
}
