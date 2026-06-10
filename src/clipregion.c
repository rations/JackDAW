#include <config.h>
#include <string.h>
#include "clipregion.h"

/* Convert a timeline-frame offset within a region to a clip-file-frame offset. */
static off_t tl_to_file(const ClipRegion *r, off_t d_timeline, int jack_sr)
{
    int clip_sr = r->clip ? r->clip->info.samplerate : jack_sr;
    if (clip_sr <= 0 || jack_sr <= 0 || clip_sr == jack_sr)
        return d_timeline;
    return (off_t)((double)d_timeline * (double)clip_sr / (double)jack_sr + 0.5);
}

ClipRegion *clip_region_new(AudioClip *clip, off_t file_in,
                            off_t length, off_t tl_pos)
{
    ClipRegion *r = g_new0(ClipRegion, 1);
    r->clip    = audio_clip_ref(clip);
    r->file_in = file_in;
    r->length  = length;
    r->tl_pos  = tl_pos;
    r->gain    = 1.0f;
    return r;
}

ClipRegion *clip_region_copy(const ClipRegion *r)
{
    ClipRegion *c = g_new0(ClipRegion, 1);
    *c = *r;
    c->clip = audio_clip_ref(r->clip);
    return c;
}

void clip_region_free(ClipRegion *r)
{
    if (!r) return;
    audio_clip_free(r->clip);
    g_free(r);
}

/* ---- List ---- */

GPtrArray *clip_region_list_new(void)
{
    return g_ptr_array_new_with_free_func((GDestroyNotify)clip_region_free);
}

static gint region_cmp(gconstpointer a, gconstpointer b)
{
    const ClipRegion *ra = *(const ClipRegion * const *)a;
    const ClipRegion *rb = *(const ClipRegion * const *)b;
    if (ra->tl_pos < rb->tl_pos) return -1;
    if (ra->tl_pos > rb->tl_pos) return  1;
    return 0;
}

void clip_region_list_sort(GPtrArray *list)
{
    if (list) g_ptr_array_sort(list, region_cmp);
}

GPtrArray *clip_region_list_copy(GPtrArray *list)
{
    GPtrArray *out = clip_region_list_new();
    if (list) {
        for (guint i = 0; i < list->len; i++)
            g_ptr_array_add(out,
                clip_region_copy(g_ptr_array_index(list, i)));
    }
    return out;
}

off_t clip_region_list_total_frames(GPtrArray *list)
{
    off_t max = 0;
    if (!list) return 0;
    for (guint i = 0; i < list->len; i++) {
        ClipRegion *r = g_ptr_array_index(list, i);
        off_t e = clip_region_end(r);
        if (e > max) max = e;
    }
    return max;
}

ClipRegion *clip_region_list_at(GPtrArray *list, off_t frame)
{
    if (!list) return NULL;
    for (guint i = 0; i < list->len; i++) {
        ClipRegion *r = g_ptr_array_index(list, i);
        if (frame >= r->tl_pos && frame < clip_region_end(r))
            return r;
    }
    return NULL;
}

void clip_region_list_split_at(GPtrArray *list, off_t frame, int jack_sr)
{
    if (!list) return;
    /* Find a region strictly straddling `frame` (a split at an existing edge
     * is a no-op). */
    for (guint i = 0; i < list->len; i++) {
        ClipRegion *r = g_ptr_array_index(list, i);
        if (frame > r->tl_pos && frame < clip_region_end(r)) {
            off_t d = frame - r->tl_pos;                 /* timeline frames in */
            ClipRegion *right = clip_region_copy(r);
            right->tl_pos  = frame;
            right->length  = r->length - d;
            right->file_in = r->file_in + tl_to_file(r, d, jack_sr);
            r->length      = d;                          /* left keeps file_in */
            g_ptr_array_add(list, right);
            clip_region_list_sort(list);
            return;
        }
    }
}

void clip_region_list_delete_range(GPtrArray *list, off_t a, off_t b, int jack_sr)
{
    if (!list || b <= a) return;
    /* Split at both boundaries first so every region is either fully inside
     * [a,b] or fully outside. */
    clip_region_list_split_at(list, a, jack_sr);
    clip_region_list_split_at(list, b, jack_sr);

    for (guint i = 0; i < list->len; ) {
        ClipRegion *r = g_ptr_array_index(list, i);
        if (r->tl_pos >= a && clip_region_end(r) <= b)
            g_ptr_array_remove_index(list, i);   /* free func runs */
        else
            i++;
    }
    clip_region_list_sort(list);
}

void clip_region_list_set_gain_range(GPtrArray *list, off_t a, off_t b,
                                     gfloat gain, int jack_sr)
{
    if (!list || b <= a) return;
    clip_region_list_split_at(list, a, jack_sr);
    clip_region_list_split_at(list, b, jack_sr);
    for (guint i = 0; i < list->len; i++) {
        ClipRegion *r = g_ptr_array_index(list, i);
        if (r->tl_pos >= a && clip_region_end(r) <= b)
            r->gain = gain;
    }
}

void clip_region_list_remove_at(GPtrArray *list, off_t frame)
{
    if (!list) return;
    for (guint i = 0; i < list->len; i++) {
        ClipRegion *r = g_ptr_array_index(list, i);
        if (frame >= r->tl_pos && frame < clip_region_end(r)) {
            g_ptr_array_remove_index(list, i);
            return;
        }
    }
}

/* ---- Snapshot ---- */

ClipRegionSnapshot *clip_region_snapshot_new(GPtrArray *list)
{
    ClipRegionSnapshot *s = g_new0(ClipRegionSnapshot, 1);
    s->refcount = 1;
    s->n = list ? (int)list->len : 0;
    if (s->n > 0) {
        s->r = g_new0(ClipRegion, s->n);
        for (int i = 0; i < s->n; i++) {
            ClipRegion *src = g_ptr_array_index(list, i);
            s->r[i] = *src;
            s->r[i].clip = audio_clip_ref(src->clip);
        }
    }
    return s;
}

ClipRegionSnapshot *clip_region_snapshot_ref(ClipRegionSnapshot *s)
{
    if (s) g_atomic_int_inc(&s->refcount);
    return s;
}

void clip_region_snapshot_unref(ClipRegionSnapshot *s)
{
    if (!s) return;
    if (!g_atomic_int_dec_and_test(&s->refcount)) return;
    for (int i = 0; i < s->n; i++)
        audio_clip_free(s->r[i].clip);
    g_free(s->r);
    g_free(s);
}
