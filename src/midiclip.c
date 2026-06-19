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

MidiRegion *midi_region_copy(const MidiRegion *r)
{
    if (!r) return NULL;
    MidiRegion *c = g_new0(MidiRegion, 1);
    *c = *r;
    c->clip = midi_clip_ref(r->clip);
    return c;
}

void midi_region_free(MidiRegion *r)
{
    if (!r) return;
    midi_clip_free(r->clip);
    g_free(r);
}

off_t midi_region_end(const MidiRegion *r, double frames_per_tick)
{
    if (!r) return 0;
    return r->tl_pos + (off_t)((double)r->length * frames_per_tick + 0.5);
}

GPtrArray *midi_region_list_new(void)
{
    return g_ptr_array_new_with_free_func((GDestroyNotify)midi_region_free);
}

static gint midi_region_cmp(gconstpointer a, gconstpointer b)
{
    const MidiRegion *ra = *(const MidiRegion * const *)a;
    const MidiRegion *rb = *(const MidiRegion * const *)b;
    if (ra->tl_pos < rb->tl_pos) return -1;
    if (ra->tl_pos > rb->tl_pos) return  1;
    return 0;
}

void midi_region_list_sort(GPtrArray *list)
{
    if (list) g_ptr_array_sort(list, midi_region_cmp);
}

GPtrArray *midi_region_list_copy(GPtrArray *list)
{
    GPtrArray *out = midi_region_list_new();
    if (list)
        for (guint i = 0; i < list->len; i++)
            g_ptr_array_add(out, midi_region_copy(g_ptr_array_index(list, i)));
    return out;
}

MidiRegion *midi_region_list_at(GPtrArray *list, off_t frame,
                                double frames_per_tick)
{
    if (!list) return NULL;
    for (guint i = 0; i < list->len; i++) {
        MidiRegion *r = g_ptr_array_index(list, i);
        if (frame >= r->tl_pos && frame < midi_region_end(r, frames_per_tick))
            return r;
    }
    return NULL;
}

off_t midi_region_list_total_frames(GPtrArray *list, double frames_per_tick)
{
    off_t max = 0;
    if (!list) return 0;
    for (guint i = 0; i < list->len; i++) {
        off_t e = midi_region_end(g_ptr_array_index(list, i), frames_per_tick);
        if (e > max) max = e;
    }
    return max;
}

void midi_region_list_split_at(GPtrArray *list, off_t frame,
                               double frames_per_tick)
{
    if (!list || frames_per_tick <= 0.0) return;
    for (guint i = 0; i < list->len; i++) {
        MidiRegion *r = g_ptr_array_index(list, i);
        if (frame > r->tl_pos && frame < midi_region_end(r, frames_per_tick)) {
            /* Tick offset of the split within the region (snap to tick grid). */
            guint32 d = (guint32)((double)(frame - r->tl_pos) / frames_per_tick + 0.5);
            if (d == 0 || d >= r->length) return;       /* lands on an edge */
            MidiRegion *right = midi_region_copy(r);
            right->clip_in = r->clip_in + d;
            right->length  = r->length  - d;
            right->tl_pos  = r->tl_pos +
                             (off_t)((double)d * frames_per_tick + 0.5);
            right->auto_grow = FALSE;
            r->length      = d;
            r->auto_grow   = FALSE;       /* both halves now deliberately sized */
            g_ptr_array_add(list, right);
            midi_region_list_sort(list);
            return;
        }
    }
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

MidiEventSnapshot *midi_event_snapshot_new_regions(GPtrArray *regions,
                                                   double frames_per_beat)
{
    MidiEventSnapshot *s = g_new0(MidiEventSnapshot, 1);
    if (!regions || regions->len == 0 || frames_per_beat <= 0.0) {
        s->n = 0; s->ev = NULL;
        return s;
    }

    GArray *out = g_array_new(FALSE, FALSE, sizeof(MidiSnapEvent));

    for (guint ri = 0; ri < regions->len; ri++) {
        MidiRegion *reg = g_ptr_array_index(regions, ri);
        MidiClip   *clip = reg->clip;
        if (!clip || !clip->notes) continue;
        guint32 win0 = reg->clip_in;             /* window start tick */
        guint32 win1 = reg->clip_in + reg->length;/* window end tick (exclusive) */

        for (guint ni = 0; ni < clip->notes->len; ni++) {
            MidiNote *nt = &g_array_index(clip->notes, MidiNote, ni);
            if (nt->velocity == 0) continue;
            if (nt->start < win0 || nt->start >= win1) continue;  /* outside window */

            /* Clamp the note's tail to the region window so a split cuts it. */
            guint32 note_end = nt->start + nt->length;
            if (note_end > win1) note_end = win1;

            guint8 ch = nt->channel & 0x0F;
            /* tl_pos is the frame at which tick `clip_in` plays. */
            off_t on_f  = reg->tl_pos +
                          ticks_to_frames(nt->start  - win0, frames_per_beat);
            off_t off_f = reg->tl_pos +
                          ticks_to_frames(note_end   - win0, frames_per_beat);
            MidiSnapEvent on  = { on_f,  (guint8)(0x90 | ch), nt->pitch, nt->velocity };
            MidiSnapEvent off = { off_f, (guint8)(0x80 | ch), nt->pitch, 0 };
            g_array_append_val(out, on);
            g_array_append_val(out, off);
        }
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
