#define _GNU_SOURCE
#include <config.h>
#include <math.h>
#include <string.h>

#include "timeline.h"
#include "trackstrip.h"
#include "jackdaw-engine.h"
#include "midiwindow.h"
#include "fxwindow.h"
#include "main.h"
#include "tempomap.h"

/* Frozen-pane colours, as cairo_set_source_rgb argument lists. The lane tone
 * matches wave_view_draw's own background so the empty area below the last
 * track is continuous with the lanes above it. */
#define TL_HEADER_BG  0.227, 0.227, 0.243
#define TL_LANE_BG    0.12,  0.12,  0.12
#define TL_DIVIDER    0.45,  0.45,  0.47

/* ========================================================================
 * JackDawTimeRuler
 * ======================================================================== */

G_DEFINE_TYPE(JackDawTimeRuler, jackdaw_time_ruler, GTK_TYPE_DRAWING_AREA)

/* Draw the loop-region band and the two draggable end tabs onto the ruler.
 * Shared by both ruler modes. x = (frame - start_samp) / spp. The tabs are
 * always meaningful: when no region is set both edges sit at frame 0. */
static void ruler_draw_loop(cairo_t *cr, gint w, gint h,
                            off_t start_samp, gdouble spp)
{
    if (spp <= 0.0) return;
    off_t ls, le;
    jackdaw_engine_get_loop_range(&ls, &le);
    gboolean has = jackdaw_engine_has_loop_region();
    gboolean on  = jackdaw_engine_get_loop_enabled();

    double x0 = ((double)ls - (double)start_samp) / spp;
    double x1 = ((double)le - (double)start_samp) / spp;

    /* Band between the tabs (only when a region exists). */
    if (has && x1 >= 0 && x0 <= (double)w) {
        double bx0 = CLAMP(x0, 0.0, (double)w);
        double bx1 = CLAMP(x1, 0.0, (double)w);
        cairo_set_source_rgba(cr, 0.95, 0.65, 0.10, on ? 0.30 : 0.15);
        cairo_rectangle(cr, bx0, 0, bx1 - bx0, h);
        cairo_fill(cr);
    }

    /* End tabs — solid amber handles a few px wide, grown to one side so they
     * stay visible even when both edges coincide at frame 0. */
    cairo_set_source_rgb(cr, 0.95, 0.65, 0.10);
    if (x0 >= -5.0 && x0 <= (double)w + 5.0) {
        cairo_rectangle(cr, x0, 0, 4, h);          /* start tab: right of x0 */
        cairo_fill(cr);
    }
    if (x1 >= -5.0 && x1 <= (double)w + 5.0) {
        cairo_rectangle(cr, x1 - 4, 0, 4, h);      /* end tab: left of x1 */
        cairo_fill(cr);
    }
}

static gboolean ruler_draw(GtkWidget *widget, cairo_t *cr)
{
    JackDawTimeRuler *r = JACKDAW_TIME_RULER(widget);
    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);
    gint w = alloc.width, h = alloc.height;

    gdouble start = gtk_adjustment_get_value(r->time_adj);
    gdouble spp   = gtk_adjustment_get_value(r->zoom_adj);

    /* Background */
    cairo_set_source_rgb(cr, 0.18, 0.18, 0.18);
    cairo_paint(cr);

    if (r->sample_rate == 0 || spp <= 0.0 || w <= 0)
        return FALSE;

    off_t start_samp = (off_t)start;
    off_t end_samp   = start_samp + (off_t)(w * spp) + 1;

    /* ---- Bars/beats mode: ticks and labels from BPM + time signature ---- */
    if (r->project && r->project->ruler_mode == JACKDAW_RULER_BARS) {
        gdouble fpb = jackdaw_project_frames_per_beat(r->project, r->sample_rate);
        if (fpb > 0.0) {
            guint bpb = r->project->beats_per_bar
                        ? r->project->beats_per_bar : 4;
            long b0 = (long)(start / fpb);
            if (b0 < 0) b0 = 0;
            cairo_set_font_size(cr, 9.0);
            for (long b = b0; ; b++) {
                double x = ((gdouble)b * fpb - start) / spp;
                if (x > (double)w) break;
                if (x < -1.0) continue;
                gboolean bar = (b % bpb) == 0;
                if (bar) {
                    cairo_set_source_rgb(cr, 0.78, 0.78, 0.78);
                    cairo_move_to(cr, x + 0.5, h - 10);
                } else {
                    cairo_set_source_rgb(cr, 0.45, 0.45, 0.45);
                    cairo_move_to(cr, x + 0.5, h - 5);
                }
                cairo_line_to(cr, x + 0.5, h);
                cairo_stroke(cr);
                if (bar) {
                    gchar bbuf[24];
                    g_snprintf(bbuf, sizeof(bbuf), "%ld", (b / bpb) + 1);
                    cairo_move_to(cr, x + 3.0, 10.0);
                    cairo_show_text(cr, bbuf);
                }
            }
        }
        /* Transport playhead */
        if (r->cursor_adj) {
            off_t cur = (off_t)gtk_adjustment_get_value(r->cursor_adj);
            if (cur >= start_samp && cur <= end_samp) {
                double cx = (cur - start_samp) / spp;
                cairo_set_source_rgba(cr, 1.0, 0.35, 0.0, 1.0);
                cairo_set_line_width(cr, 1.0);
                cairo_move_to(cr, cx + 0.5, 0);
                cairo_line_to(cr, cx + 0.5, h);
                cairo_stroke(cr);
            }
        }
        ruler_draw_loop(cr, w, h, start_samp, spp);
        return FALSE;
    }

    /* Tick marks from ruler_tick_positions. The tick budget is pixel-driven —
     * roughly one labelled major tick per 90px so labels stay readable — not
     * the array size, otherwise the ruler packs in far too many ticks. */
    off_t pts[256], mids[256], mins[512];
    int   npts  = CLAMP(w / 90, 2, 256);
    int   nmids = CLAMP(w / 45, 2, 256);
    int   nmins = CLAMP(w / 15, 2, 512);
    guint has_mids = ruler_tick_positions(r->sample_rate,
                                           start_samp, end_samp,
                                           pts,  &npts,
                                           mids, &nmids,
                                           mins, &nmins,
                                           default_timescale_mode);

    cairo_set_line_width(cr, 1.0);

    /* Minor ticks */
    cairo_set_source_rgb(cr, 0.40, 0.40, 0.40);
    for (int i = 0; i < nmins; i++) {
        double x = (mins[i] - start_samp) / spp;
        if (x < 0.0 || x > (double)w) continue;
        cairo_move_to(cr, x + 0.5, h - 4);
        cairo_line_to(cr, x + 0.5, h);
        cairo_stroke(cr);
    }

    /* Mid ticks */
    if (has_mids) {
        cairo_set_source_rgb(cr, 0.55, 0.55, 0.55);
        for (int i = 0; i < nmids; i++) {
            double x = (mids[i] - start_samp) / spp;
            if (x < 0.0 || x > (double)w) continue;
            cairo_move_to(cr, x + 0.5, h - 7);
            cairo_line_to(cr, x + 0.5, h);
            cairo_stroke(cr);
        }
    }

    /* Major ticks + labels */
    cairo_set_font_size(cr, 9.0);
    for (int i = 0; i < npts; i++) {
        double x = (pts[i] - start_samp) / spp;
        if (x < -1.0 || x > (double)(w + 4)) continue;

        cairo_set_source_rgb(cr, 0.75, 0.75, 0.75);
        cairo_move_to(cr, x + 0.5, h - 10);
        cairo_line_to(cr, x + 0.5, h);
        cairo_stroke(cr);

        gchar tbuf[32];
        format_timecode(r->sample_rate, pts[i], end_samp, tbuf, default_timescale_mode);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, tbuf, &ext);
        double lx = x + 3.0;
        if (lx + ext.width > (double)w) continue;
        if (lx < 2.0) lx = 2.0;
        cairo_move_to(cr, lx, 10.0);
        cairo_show_text(cr, tbuf);
    }

    /* Transport playhead */
    if (r->cursor_adj) {
        off_t cur = (off_t)gtk_adjustment_get_value(r->cursor_adj);
        if (cur >= start_samp && cur <= end_samp) {
            double cx = (cur - start_samp) / spp;
            cairo_set_source_rgba(cr, 1.0, 0.35, 0.0, 1.0); /* orange */
            cairo_set_line_width(cr, 1.0);
            cairo_move_to(cr, cx + 0.5, 0);
            cairo_line_to(cr, cx + 0.5, h);
            cairo_stroke(cr);
        }
    }

    ruler_draw_loop(cr, w, h, start_samp, spp);

    return FALSE;
}

static void ruler_adj_changed(GtkAdjustment *adj, gpointer data)
{
    (void)adj;
    gtk_widget_queue_draw(GTK_WIDGET(data));
}

static void jackdaw_time_ruler_finalize(GObject *obj)
{
    JackDawTimeRuler *r = JACKDAW_TIME_RULER(obj);
    g_object_unref(r->time_adj);
    g_object_unref(r->zoom_adj);
    if (r->cursor_adj) g_object_unref(r->cursor_adj);
    G_OBJECT_CLASS(jackdaw_time_ruler_parent_class)->finalize(obj);
}

static void jackdaw_time_ruler_class_init(JackDawTimeRulerClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = jackdaw_time_ruler_finalize;
    GTK_WIDGET_CLASS(klass)->draw   = ruler_draw;
}

static void jackdaw_time_ruler_init(JackDawTimeRuler *r)
{
    r->time_adj    = NULL;
    r->zoom_adj    = NULL;
    r->cursor_adj  = NULL;
    r->project     = NULL;
    r->sample_rate = 48000;
}

GtkWidget *jackdaw_time_ruler_new(GtkAdjustment *time_adj,
                                   GtkAdjustment *zoom_adj,
                                   GtkAdjustment *cursor_adj,
                                   guint32        sample_rate)
{
    JackDawTimeRuler *r = g_object_new(JACKDAW_TYPE_TIME_RULER, NULL);
    r->time_adj    = g_object_ref(time_adj);
    r->zoom_adj    = g_object_ref(zoom_adj);
    r->cursor_adj  = g_object_ref(cursor_adj);
    r->sample_rate = sample_rate;
    gtk_widget_set_size_request(GTK_WIDGET(r), -1, TIMELINE_RULER_HEIGHT);
    g_signal_connect_object(time_adj,   "value-changed",
                            G_CALLBACK(ruler_adj_changed), r, 0);
    g_signal_connect_object(zoom_adj,   "value-changed",
                            G_CALLBACK(ruler_adj_changed), r, 0);
    g_signal_connect_object(cursor_adj, "value-changed",
                            G_CALLBACK(ruler_adj_changed), r, 0);
    return GTK_WIDGET(r);
}

/* ========================================================================
 * JackDawWaveView
 * ======================================================================== */

G_DEFINE_TYPE(JackDawWaveView, jackdaw_wave_view, GTK_TYPE_DRAWING_AREA)

/* Generalized "section" accessors (audio ClipRegion* / instrument MidiRegion*),
 * defined further down but used by the drawing code here. */
static off_t sec_tl_pos(JackDawTrack *t, gpointer s);
static off_t sec_end(JackDawTimeline *tl, JackDawTrack *t, gpointer s);

/* Draw the beat/bar grid behind the waveform when enabled on the project. */
static void wave_view_draw_grid(JackDawWaveView *wv, cairo_t *cr,
                                int w, int h, gdouble start, gdouble spp)
{
    if (!wv->project || !wv->project->grid_enabled || spp <= 0.0)
        return;
    guint32 sr = (guint32)jackdaw_engine_get_sample_rate();
    TempoMap tm;
    tempomap_from_project(&tm, wv->project, sr);

    gdouble fpb  = tempomap_frames_per_beat(&tm);
    /* Draw the grid the user actually snaps to, not always whole beats. */
    gdouble step = tempomap_grid_frames(&tm,
                       (TempoMapGrid)wv->project->grid_unit);
    if (fpb <= 0.0 || step <= 0.0) return;
    /* Below a few pixels apart the lines are noise, not a grid. */
    if (step / spp < 4.0) step = fpb;
    if (step / spp < 4.0) return;

    gdouble fpbar = tempomap_frames_per_bar(&tm);

    long g0 = (long)floor(start / step);
    if (g0 < 0) g0 = 0;
    cairo_set_line_width(cr, 1.0);
    for (long g = g0; ; g++) {
        gdouble f = (gdouble)g * step;
        double  x = (f - start) / spp;
        if (x > (double)w) break;
        if (x < 0.0) continue;
        /* Emphasis: bar line > beat line > subdivision. */
        gdouble mod_bar  = fmod(f, fpbar);
        gdouble mod_beat = fmod(f, fpb);
        if (fpbar > 0.0 && (mod_bar < 1.0 || fpbar - mod_bar < 1.0))
            cairo_set_source_rgba(cr, 0.50, 0.50, 0.56, 0.50);   /* bar */
        else if (mod_beat < 1.0 || fpb - mod_beat < 1.0)
            cairo_set_source_rgba(cr, 0.30, 0.30, 0.33, 0.40);   /* beat */
        else
            cairo_set_source_rgba(cr, 0.26, 0.26, 0.29, 0.25);   /* subdivision */
        cairo_move_to(cr, floor(x) + 0.5, 0);
        cairo_line_to(cr, floor(x) + 0.5, h);
        cairo_stroke(cr);
    }
}

/* Draw MIDI sections (instrument tracks): one box per region, with its windowed
 * clip notes as mini note-rects, mirroring the audio region drawing so splits /
 * moves read the same way. */
static void wave_view_draw_midi(JackDawWaveView *wv, cairo_t *cr,
                                int w, int h, gdouble start, gdouble spp)
{
    if (spp <= 0.0 || !wv->project) return;
    GPtrArray *regs = jackdaw_track_get_midi_regions(wv->track);
    if (!regs) return;
    double fpb = jackdaw_project_frames_per_beat(wv->project,
                                                 jackdaw_engine_get_sample_rate());
    if (fpb <= 0.0) return;
    double f_per_tick = fpb / (double)JACKDAW_PPQ;

    for (guint ri = 0; ri < regs->len; ri++) {
        MidiRegion *reg = g_ptr_array_index(regs, ri);
        MidiClip   *clip = reg->clip;
        if (!clip) continue;
        off_t r_tl0 = reg->tl_pos;
        off_t r_tl1 = midi_region_end(reg, f_per_tick);
        double rx0 = ((double)r_tl0 - start) / spp;
        double rx1 = ((double)r_tl1 - start) / spp;
        if (rx1 < 0 || rx0 > w) continue;

        /* Faint section body so an empty region is still visible/grabbable. */
        double bx0 = CLAMP(rx0, 0.0, (double)w);
        double bx1 = CLAMP(rx1, 0.0, (double)w);
        if (bx1 > bx0) {
            cairo_set_source_rgba(cr, 0.20, 0.32, 0.45, 0.18);
            cairo_rectangle(cr, bx0, 0, bx1 - bx0, h);
            cairo_fill(cr);
        }

        /* Notes whose start falls within this region's window. */
        guint   nc   = midi_clip_note_count(clip);
        guint32 win0 = reg->clip_in;
        guint32 win1 = reg->clip_in + reg->length;
        cairo_set_source_rgb(cr, 0.80, 0.92, 1.0);
        for (guint i = 0; i < nc; i++) {
            MidiNote *n = midi_clip_note(clip, i);
            if (n->start < win0 || n->start >= win1) continue;
            guint32 nend = n->start + n->length;
            if (nend > win1) nend = win1;             /* clamp tail to the split */
            double nx = ((double)(r_tl0 + (off_t)((double)(n->start - win0) * f_per_tick))
                         - start) / spp;
            double nw = ((double)(nend - n->start) * f_per_tick) / spp;
            if (nw < 1) nw = 1;
            double ny = h - ((n->pitch / 127.0) * (h - 2)) - 1;
            if (nx + nw < 0 || nx > w) continue;
            cairo_rectangle(cr, nx, ny, nw, 2); cairo_fill(cr);
        }

        /* Region boundary lines (skip the very first region's start at 0). */
        cairo_set_source_rgba(cr, 0.95, 0.95, 0.55, 0.9);
        cairo_set_line_width(cr, 1.0);
        if (r_tl0 > 0 && rx0 >= 0.0 && rx0 < (double)w) {
            cairo_move_to(cr, rx0 + 0.5, 0); cairo_line_to(cr, rx0 + 0.5, h);
            cairo_stroke(cr);
        }
        if (rx1 >= 0.0 && rx1 < (double)w) {
            cairo_move_to(cr, rx1 + 0.5, 0); cairo_line_to(cr, rx1 + 0.5, h);
            cairo_stroke(cr);
        }
    }
}

static gboolean wave_view_draw(GtkWidget *widget, cairo_t *cr)
{
    JackDawWaveView *wv = JACKDAW_WAVE_VIEW(widget);
    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);
    gint w = alloc.width, h = alloc.height;

    /* Background */
    cairo_set_source_rgb(cr, 0.12, 0.12, 0.12);
    cairo_paint(cr);

    if (w <= 0 || h <= 0)
        return FALSE;

    gdouble start = gtk_adjustment_get_value(wv->time_adj);
    gdouble spp   = gtk_adjustment_get_value(wv->zoom_adj);
    int     jack_sr = (int)jackdaw_engine_get_sample_rate();

    /* Beat/bar grid (drawn behind the waveform) */
    wave_view_draw_grid(wv, cr, w, h, start, spp);

    gboolean instr = wv->track && jackdaw_track_is_instrument(wv->track);
    if (instr)
        wave_view_draw_midi(wv, cr, w, h, start, spp);

    GPtrArray *regs = (wv->track && !instr) ? jackdaw_track_get_regions(wv->track) : NULL;
    if (regs && spp > 0.0) {
        off_t view0 = (off_t)start;
        off_t view1 = (off_t)(start + (gdouble)w * spp) + 1;

        for (guint ri = 0; ri < regs->len; ri++) {
            ClipRegion *r = g_ptr_array_index(regs, ri);
            if (!r->clip || r->clip->info.frames <= 0 || r->length <= 0)
                continue;

            /* Visible timeline span of this region */
            off_t r_tl0 = r->tl_pos;
            off_t r_tl1 = r->tl_pos + r->length;
            off_t vis0  = r_tl0 > view0 ? r_tl0 : view0;
            off_t vis1  = r_tl1 < view1 ? r_tl1 : view1;
            if (vis1 <= vis0) continue;

            int px0 = (int)(((gdouble)vis0 - start) / spp);
            int px1 = (int)(((gdouble)vis1 - start) / spp) + 1;
            if (px0 < 0) px0 = 0;
            if (px1 > w) px1 = w;
            int npx = px1 - px0;
            if (npx <= 0) continue;

            /* Corresponding clip-file frame range */
            int    clip_sr = r->clip->info.samplerate;
            double ratio   = (clip_sr > 0 && jack_sr > 0)
                             ? (double)clip_sr / (double)jack_sr : 1.0;
            sf_count_t cf0 = r->file_in + (sf_count_t)((vis0 - r_tl0) * ratio);
            sf_count_t cf1 = r->file_in + (sf_count_t)((vis1 - r_tl0) * ratio);
            if (cf1 <= cf0) cf1 = cf0 + 1;
            if (cf1 > r->clip->info.frames) cf1 = r->clip->info.frames;
            if (cf0 < 0) cf0 = 0;
            if (cf1 <= cf0) continue;

            int ch      = r->clip->info.channels;
            int draw_ch = ch > 2 ? 2 : ch;
            gfloat gain = r->gain;

            gfloat *out_min = g_new(gfloat, (gsize)npx * ch);
            gfloat *out_max = g_new(gfloat, (gsize)npx * ch);
            audio_clip_get_peaks(r->clip, cf0, cf1, npx, out_min, out_max);

            for (int c = 0; c < draw_ch; c++) {
                int band_h  = h / draw_ch;
                int band_y0 = c * band_h;
                int mid_y   = band_y0 + band_h / 2;

                cairo_set_source_rgb(cr, 0.15, 0.50, 0.20);
                for (int x = 0; x < npx; x++) {
                    gfloat mn = out_min[x * ch + c] * gain;
                    gfloat mx = out_max[x * ch + c] * gain;
                    if (mn > mx) continue;

                    double half  = (double)(band_h / 2) * 3.0;
                    double y_top = mid_y - mx * half;
                    double y_bot = mid_y - mn * half;
                    if (y_top < band_y0)          y_top = band_y0;
                    if (y_bot > band_y0 + band_h) y_bot = band_y0 + band_h;

                    cairo_move_to(cr, (px0 + x) + 0.5, y_top);
                    cairo_line_to(cr, (px0 + x) + 0.5, y_bot);
                }
                cairo_set_line_width(cr, 1.0);
                cairo_stroke(cr);

                cairo_set_source_rgba(cr, 0.30, 0.70, 0.35, 0.5);
                cairo_move_to(cr, px0, mid_y + 0.5);
                cairo_line_to(cr, px1, mid_y + 0.5);
                cairo_stroke(cr);
            }

            /* Region boundary lines — draw both the left (start) and right
             * (end) edge.  Drawing the right edge keeps a boundary visible after
             * a neighbouring section is deleted, so a gap reads as two real
             * edges.  Skip the very first region's start edge at timeline 0. */
            cairo_set_source_rgba(cr, 0.95, 0.95, 0.55, 0.9);
            cairo_set_line_width(cr, 1.0);
            if (r_tl0 > 0) {
                double bx = ((gdouble)r_tl0 - start) / spp;
                if (bx >= 0.0 && bx < (double)w) {
                    cairo_move_to(cr, bx + 0.5, 0);
                    cairo_line_to(cr, bx + 0.5, h);
                    cairo_stroke(cr);
                }
            }
            {
                double bx = ((gdouble)r_tl1 - start) / spp;
                if (bx >= 0.0 && bx < (double)w) {
                    cairo_move_to(cr, bx + 0.5, 0);
                    cairo_line_to(cr, bx + 0.5, h);
                    cairo_stroke(cr);
                }
            }

            g_free(out_min);
            g_free(out_max);
        }
    }

    /* Section selection overlay — highlight each selected region on its track. */
    if (wv->timeline && wv->timeline->sel_track == wv->track &&
        wv->timeline->sel_regions && wv->timeline->sel_regions->len > 0 &&
        spp > 0.0) {
        GPtrArray *sel = wv->timeline->sel_regions;
        for (guint si = 0; si < sel->len; si++) {
            gpointer r = g_ptr_array_index(sel, si);
            double sx0 = ((gdouble)sec_tl_pos(wv->track, r) - start) / spp;
            double sx1 = ((gdouble)sec_end(wv->timeline, wv->track, r) - start) / spp;
            sx0 = CLAMP(sx0, 0.0, (double)w);
            sx1 = CLAMP(sx1, 0.0, (double)w);
            if (sx1 > sx0) {
                cairo_set_source_rgba(cr, 0.40, 0.60, 0.90, 0.18);
                cairo_rectangle(cr, sx0, 0, sx1 - sx0, h);
                cairo_fill(cr);
                cairo_set_source_rgba(cr, 0.40, 0.60, 0.90, 0.6);
                cairo_set_line_width(cr, 1.0);
                cairo_move_to(cr, sx0 + 0.5, 0); cairo_line_to(cr, sx0 + 0.5, h);
                cairo_move_to(cr, sx1 + 0.5, 0); cairo_line_to(cr, sx1 + 0.5, h);
                cairo_stroke(cr);
            }
        }
    }
    /* Rubber-band range overlay (shared across all tracks) */
    else if (wv->timeline && wv->timeline->sel_active && spp > 0.0) {
        off_t a = wv->timeline->sel_start;
        off_t b = wv->timeline->sel_end;
        if (b < a) { off_t tmp = a; a = b; b = tmp; }
        double sx0 = ((gdouble)a - start) / spp;
        double sx1 = ((gdouble)b - start) / spp;
        sx0 = CLAMP(sx0, 0.0, (double)w);
        sx1 = CLAMP(sx1, 0.0, (double)w);
        if (sx1 > sx0) {
            cairo_set_source_rgba(cr, 0.40, 0.60, 0.90, 0.18);
            cairo_rectangle(cr, sx0, 0, sx1 - sx0, h);
            cairo_fill(cr);
            cairo_set_source_rgba(cr, 0.40, 0.60, 0.90, 0.6);
            cairo_set_line_width(cr, 1.0);
            cairo_move_to(cr, sx0 + 0.5, 0); cairo_line_to(cr, sx0 + 0.5, h);
            cairo_move_to(cr, sx1 + 0.5, 0); cairo_line_to(cr, sx1 + 0.5, h);
            cairo_stroke(cr);
        }
    }

    /* Loop-region band (shared across all tracks; amber). */
    if (spp > 0.0 && jackdaw_engine_has_loop_region()) {
        off_t ls, le;
        jackdaw_engine_get_loop_range(&ls, &le);
        double lx0 = ((gdouble)ls - start) / spp;
        double lx1 = ((gdouble)le - start) / spp;
        lx0 = CLAMP(lx0, 0.0, (double)w);
        lx1 = CLAMP(lx1, 0.0, (double)w);
        if (lx1 > lx0) {
            gboolean on = jackdaw_engine_get_loop_enabled();
            cairo_set_source_rgba(cr, 0.95, 0.65, 0.10, on ? 0.16 : 0.08);
            cairo_rectangle(cr, lx0, 0, lx1 - lx0, h);
            cairo_fill(cr);
            cairo_set_source_rgba(cr, 0.95, 0.65, 0.10, 0.7);
            cairo_set_line_width(cr, 1.0);
            cairo_move_to(cr, lx0 + 0.5, 0); cairo_line_to(cr, lx0 + 0.5, h);
            cairo_move_to(cr, lx1 + 0.5, 0); cairo_line_to(cr, lx1 + 0.5, h);
            cairo_stroke(cr);
        }
    }

    /* Real-time recording region: red background + live waveform. */
    if (wv->track && jackdaw_engine_is_recording() &&
        jackdaw_track_is_armed(wv->track) && spp > 0.0) {
        JackDawTrack *rec_t    = wv->track;
        off_t         rec_start = rec_t->rec_start_frame;
        off_t         rec_end   = jackdaw_engine_get_play_pos();
        gdouble x0 = ((gdouble)rec_start - start) / spp;
        gdouble x1 = ((gdouble)rec_end   - start) / spp;
        x0 = CLAMP(x0, 0.0, (gdouble)w);
        x1 = CLAMP(x1, 0.0, (gdouble)w);

        if (x1 > x0) {
            /* Red background (recording indicator, both audio + MIDI) */
            cairo_set_source_rgba(cr, 0.80, 0.08, 0.08, 0.25);
            cairo_rectangle(cr, x0, 0, x1 - x0, h);
            cairo_fill(cr);

          if (jackdaw_track_is_instrument(rec_t)) {
            /* Live MIDI notes captured so far — peeked non-destructively so the
             * RT thread keeps writing and the finalizer still gets them on stop.
             * Same pitch->y mapping as the finalized region preview. */
            guint nn = 0;
            const JackDawRecNote *rn = jackdaw_engine_rec_preview(rec_t, &nn);
            cairo_set_source_rgb(cr, 1.0, 0.88, 0.88);  /* bright over the red */
            for (guint i = 0; i < nn; i++) {
                double nx0 = ((double)rn[i].start_frame - start) / spp;
                double nx1 = ((double)rn[i].end_frame   - start) / spp;
                if (nx1 < 0 || nx0 > w) continue;
                double nw = nx1 - nx0; if (nw < 1) nw = 1;
                double ny = h - ((rn[i].pitch / 127.0) * (h - 2)) - 1;
                cairo_rectangle(cr, nx0, ny, nw, 2); cairo_fill(cr);
            }
          } else {
            /* Live waveform — read peak buffer written by the RT callback */
            gint    pk_count = rec_t->rec_peak_count; /* read once: RT may still write */
            gfloat *pk_buf   = rec_t->rec_peak_buf;
            guint   blk      = rec_t->rec_peak_block;

            if (pk_buf && pk_count > 0 && blk > 0) {
                cairo_set_source_rgba(cr, 1.0, 0.45, 0.45, 0.92);
                cairo_set_line_width(cr, 1.0);

                int ix0 = (int)x0;
                int ix1 = (int)x1;
                if (ix1 >= w) ix1 = w - 1;

                double half = h * 1.5;
                double mid  = h * 0.5;

                for (int x = ix0; x <= ix1; x++) {
                    /* Sample offset from recording start for the left edge of pixel x */
                    off_t soff0 = (off_t)(start + x * spp) - rec_start;
                    if (soff0 < 0) continue;

                    gint b0 = (gint)((guint64)soff0 / blk);
                    if (b0 >= pk_count) break;

                    /* Right edge of this pixel — may span multiple buckets */
                    off_t soff1 = (off_t)(start + (x + 1) * spp) - rec_start;
                    gint  b1    = (gint)((guint64)soff1 / blk);
                    if (b1 >= pk_count) b1 = pk_count - 1;

                    gfloat mn = pk_buf[b0 * 2];
                    gfloat mx = pk_buf[b0 * 2 + 1];
                    for (gint b = b0 + 1; b <= b1; b++) {
                        if (pk_buf[b * 2]     < mn) mn = pk_buf[b * 2];
                        if (pk_buf[b * 2 + 1] > mx) mx = pk_buf[b * 2 + 1];
                    }

                    double y_top = CLAMP(mid - mx * half, 0.0, (double)h);
                    double y_bot = CLAMP(mid - mn * half, 0.0, (double)h);
                    if (y_bot < y_top + 1.0) y_bot = y_top + 1.0;

                    cairo_move_to(cr, x + 0.5, y_top);
                    cairo_line_to(cr, x + 0.5, y_bot);
                }
                cairo_stroke(cr);
            }
          }
        }
    }

    /* Transport playhead (orange) */
    if (wv->cursor_adj && spp > 0.0) {
        double px = (gtk_adjustment_get_value(wv->cursor_adj) - start) / spp;
        if (px >= 0.0 && px < (double)w) {
            cairo_set_source_rgba(cr, 1.0, 0.35, 0.0, 1.0);
            cairo_set_line_width(cr, 1.0);
            cairo_move_to(cr, px + 0.5, 0);
            cairo_line_to(cr, px + 0.5, h);
            cairo_stroke(cr);
        }
    }

    /* Selection / active border — read from the project so the strip and the
     * timeline highlight as one unit. A merely-selected track gets a soft blue
     * border; the active (primary) track gets a brighter, thicker one. */
    if (wv->project && jackdaw_project_is_selected(wv->project, wv->track)) {
        gboolean active =
            (jackdaw_project_get_active_track(wv->project) == wv->track);
        if (active) {
            cairo_set_source_rgba(cr, 0.55, 0.78, 1.0, 1.0);
            cairo_set_line_width(cr, 2.5);
        } else {
            cairo_set_source_rgba(cr, 0.40, 0.60, 0.90, 0.70);
            cairo_set_line_width(cr, 1.5);
        }
        cairo_rectangle(cr, 1.5, 1.5, (double)(w - 3), (double)(h - 3));
        cairo_stroke(cr);
    }

    return FALSE;
}

static void wave_view_adj_changed(GtkAdjustment *adj, gpointer data)
{
    (void)adj;
    gtk_widget_queue_draw(GTK_WIDGET(data));
}

static void wave_view_track_state_changed(JackDawTrack *track, gpointer data)
{
    (void)track;
    gtk_widget_queue_draw(GTK_WIDGET(data));
}

static void jackdaw_wave_view_finalize(GObject *obj)
{
    JackDawWaveView *wv = JACKDAW_WAVE_VIEW(obj);
    g_object_unref(wv->track);
    g_object_unref(wv->time_adj);
    g_object_unref(wv->zoom_adj);
    if (wv->cursor_adj) g_object_unref(wv->cursor_adj);
    G_OBJECT_CLASS(jackdaw_wave_view_parent_class)->finalize(obj);
}

static void jackdaw_wave_view_class_init(JackDawWaveViewClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = jackdaw_wave_view_finalize;
    GTK_WIDGET_CLASS(klass)->draw   = wave_view_draw;
}

static void jackdaw_wave_view_init(JackDawWaveView *wv)
{
    wv->track      = NULL;
    wv->time_adj   = NULL;
    wv->zoom_adj   = NULL;
    wv->cursor_adj = NULL;
    wv->project    = NULL;
    wv->timeline   = NULL;
    wv->focused    = FALSE;
}

GtkWidget *jackdaw_wave_view_new(JackDawTrack  *track,
                                  GtkAdjustment *time_adj,
                                  GtkAdjustment *zoom_adj,
                                  GtkAdjustment *cursor_adj)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(track), NULL);

    JackDawWaveView *wv = g_object_new(JACKDAW_TYPE_WAVE_VIEW, NULL);
    wv->track      = g_object_ref(track);
    wv->time_adj   = g_object_ref(time_adj);
    wv->zoom_adj   = g_object_ref(zoom_adj);
    wv->cursor_adj = g_object_ref(cursor_adj);

    gtk_widget_set_size_request(GTK_WIDGET(wv), -1, TIMELINE_TRACK_MIN_HEIGHT);
    gtk_widget_add_events(GTK_WIDGET(wv),
                          GDK_BUTTON_PRESS_MASK | GDK_SCROLL_MASK |
                          GDK_SMOOTH_SCROLL_MASK);

    g_signal_connect_object(time_adj,   "value-changed",
                            G_CALLBACK(wave_view_adj_changed), wv, 0);
    g_signal_connect_object(zoom_adj,   "value-changed",
                            G_CALLBACK(wave_view_adj_changed), wv, 0);
    g_signal_connect_object(cursor_adj, "value-changed",
                            G_CALLBACK(wave_view_adj_changed), wv, 0);

    /* Redraw when clip changes on the track */
    g_signal_connect_object(track, "state-changed",
                            G_CALLBACK(wave_view_track_state_changed), wv, 0);

    return GTK_WIDGET(wv);
}

void jackdaw_wave_view_set_focused(JackDawWaveView *wv, gboolean focused)
{
    wv->focused = focused;
    gtk_widget_queue_draw(GTK_WIDGET(wv));
}

void jackdaw_wave_view_invalidate(JackDawWaveView *wv)
{
    gtk_widget_queue_draw(GTK_WIDGET(wv));
}

/* ========================================================================
 * Track resize handle
 * ======================================================================== */

typedef struct {
    GtkWidget *outer;       /* vertical box containing row + handle */
    GtkWidget *strip;       /* track strip header (collapses as row shrinks) */
    GtkWidget *wv;          /* wave view (drops its min height when collapsing) */
    gint       base_h;      /* outer height at drag start */
    gdouble    drag_y_root; /* pointer y_root at drag start */
    gboolean   dragging;
} ResizeData;

static gboolean resize_handle_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    (void)data;
    GtkAllocation a;
    gtk_widget_get_allocation(w, &a);
    /* Dark trough with a subtle highlight line */
    cairo_set_source_rgb(cr, 0.20, 0.20, 0.20);
    cairo_paint(cr);
    cairo_set_source_rgb(cr, 0.35, 0.35, 0.35);
    cairo_set_line_width(cr, 1.0);
    double mid = a.height / 2.0;
    cairo_move_to(cr, 0,       mid + 0.5);
    cairo_line_to(cr, a.width, mid + 0.5);
    cairo_stroke(cr);
    return FALSE;
}

static gboolean resize_handle_enter(GtkWidget *w, GdkEventCrossing *ev,
                                     gpointer data)
{
    (void)ev; (void)data;
    GdkCursor *cur = gdk_cursor_new_for_display(
        gtk_widget_get_display(w), GDK_SB_V_DOUBLE_ARROW);
    gdk_window_set_cursor(gtk_widget_get_window(w), cur);
    g_object_unref(cur);
    return FALSE;
}

static gboolean resize_handle_leave(GtkWidget *w, GdkEventCrossing *ev,
                                     gpointer data)
{
    (void)ev; (void)data;
    gdk_window_set_cursor(gtk_widget_get_window(w), NULL);
    return FALSE;
}

static gboolean resize_handle_press(GtkWidget *w, GdkEventButton *ev,
                                     gpointer data)
{
    if (ev->button != 1) return FALSE;
    ResizeData *rd = data;
    GtkAllocation a;
    gtk_widget_get_allocation(rd->outer, &a);
    rd->base_h      = a.height;
    rd->drag_y_root = ev->y_root;
    rd->dragging    = TRUE;
    gtk_grab_add(w);
    return TRUE;
}

static gboolean resize_handle_motion(GtkWidget *w, GdkEventMotion *ev,
                                      gpointer data)
{
    (void)w;
    ResizeData *rd = data;
    if (!rd->dragging) return FALSE;
    gint new_h = rd->base_h + (gint)(ev->y_root - rd->drag_y_root);
    new_h = CLAMP(new_h,
                  TIMELINE_TRACK_MIN_HEIGHT + TIMELINE_RESIZE_HANDLE_H,
                  TIMELINE_TRACK_MAX_HEIGHT + TIMELINE_RESIZE_HANDLE_H);

    /* Collapse strip controls so the row can shrink below their natural size.
     * Hide first, then request the new height, so GTK recomputes the minimum. */
    gint content_h = new_h - TIMELINE_RESIZE_HANDLE_H;
    if (rd->strip)
        jackdaw_track_strip_set_height(JACKDAW_TRACK_STRIP(rd->strip), content_h);
    gtk_widget_set_size_request(rd->outer, -1, new_h);
    return TRUE;
}

static gboolean resize_handle_release(GtkWidget *w, GdkEventButton *ev,
                                       gpointer data)
{
    (void)ev;
    ResizeData *rd = data;
    if (!rd->dragging) return FALSE;
    rd->dragging = FALSE;
    gtk_grab_remove(w);
    return TRUE;
}

/* ========================================================================
 * JackDawTimeline
 * ======================================================================== */

G_DEFINE_TYPE(JackDawTimeline, jackdaw_timeline, GTK_TYPE_BOX)

enum {
    SIGNAL_TRACK_FOCUSED,
    SIGNAL_POSITION_CHANGED,
    LAST_SIGNAL
};
static guint timeline_signals[LAST_SIGNAL];

/* Project selection changed: mirror the active track into focused_track (kept
 * for the many internal readers), repaint every wave view, and re-emit the
 * timeline's track-focused signal for external listeners. */
static void timeline_selection_changed(JackDawProject *project, gpointer data)
{
    JackDawTimeline *tl = JACKDAW_TIMELINE(data);
    JackDawTrack *active = jackdaw_project_get_active_track(project);
    gboolean changed = (tl->focused_track != active);
    tl->focused_track = active;
    jackdaw_timeline_redraw_all(tl);
    if (changed)
        g_signal_emit(tl, timeline_signals[SIGNAL_TRACK_FOCUSED], 0, active);
}

/* ---- Shared helpers ---- */

void jackdaw_timeline_redraw_all(JackDawTimeline *tl)
{
    GHashTableIter it; gpointer k, v;
    g_hash_table_iter_init(&it, tl->wave_views);
    while (g_hash_table_iter_next(&it, &k, &v))
        gtk_widget_queue_draw(GTK_WIDGET(v));
    if (tl->ruler) gtk_widget_queue_draw(GTK_WIDGET(tl->ruler));
}

static guint32 timeline_jack_sr(void)
{
    return (guint32)jackdaw_engine_get_sample_rate();
}

static off_t timeline_x_to_sample(JackDawTimeline *tl, gdouble x)
{
    gdouble spp   = gtk_adjustment_get_value(tl->zoom_adj);
    gdouble start = gtk_adjustment_get_value(tl->time_adj);
    off_t   s     = (off_t)(start + x * spp);
    if (s < 0) s = 0;
    if (tl->project)
        s = jackdaw_project_snap_frame(tl->project, s, timeline_jack_sr());
    return s;
}

/* ---- Generalized timeline "section" ----------------------------------------
 * A "section" is a ClipRegion* on an audio track or a MidiRegion* on an
 * instrument track. The selection / move / drag / split machinery is shared
 * across both kinds via the dispatch helpers below; a section pointer is only
 * valid together with the track that owns it. */

static double timeline_frames_per_tick(JackDawTimeline *tl)
{
    if (!tl->project) return 0.0;
    double fpb = jackdaw_project_frames_per_beat(tl->project, timeline_jack_sr());
    return fpb / (double)JACKDAW_PPQ;
}

static GPtrArray *track_section_list(JackDawTrack *t)
{
    return jackdaw_track_is_instrument(t)
        ? jackdaw_track_get_midi_regions(t)
        : jackdaw_track_get_regions(t);
}

static off_t sec_tl_pos(JackDawTrack *t, gpointer s)
{
    return jackdaw_track_is_instrument(t)
        ? ((MidiRegion *)s)->tl_pos : ((ClipRegion *)s)->tl_pos;
}

static void sec_set_tl_pos(JackDawTrack *t, gpointer s, off_t pos)
{
    if (jackdaw_track_is_instrument(t)) ((MidiRegion *)s)->tl_pos = pos;
    else                                ((ClipRegion *)s)->tl_pos = pos;
}

static off_t sec_end(JackDawTimeline *tl, JackDawTrack *t, gpointer s)
{
    if (jackdaw_track_is_instrument(t))
        return midi_region_end((MidiRegion *)s, timeline_frames_per_tick(tl));
    return clip_region_end((ClipRegion *)s);
}

static gpointer sec_list_at(JackDawTimeline *tl, JackDawTrack *t, off_t frame)
{
    GPtrArray *list = track_section_list(t);
    if (jackdaw_track_is_instrument(t))
        return midi_region_list_at(list, frame, timeline_frames_per_tick(tl));
    return clip_region_list_at(list, frame);
}

/* Re-publish a track's RT snapshot after a section-list edit (kind-aware). */
static void track_commit_sections(JackDawTimeline *tl, JackDawTrack *t)
{
    if (jackdaw_track_is_instrument(t)) {
        double fpb = tl->project
            ? jackdaw_project_frames_per_beat(tl->project, timeline_jack_sr())
            : 0.0;
        jackdaw_track_commit_midi(t, fpb);
    } else {
        jackdaw_track_commit_regions(t);
    }
}

static void track_sort_sections(JackDawTrack *t)
{
    if (jackdaw_track_is_instrument(t))
        midi_region_list_sort(jackdaw_track_get_midi_regions(t));
    else
        clip_region_list_sort(jackdaw_track_get_regions(t));
}

/* Deep copy of a track's section list (for move undo mementos). */
static GPtrArray *track_section_list_copy(JackDawTrack *t)
{
    return jackdaw_track_is_instrument(t)
        ? midi_region_list_copy(jackdaw_track_get_midi_regions(t))
        : clip_region_list_copy(jackdaw_track_get_regions(t));
}

/* Replace a track's section list with copies from `list`, then republish. */
static void track_apply_section_list(JackDawTimeline *tl, JackDawTrack *t,
                                     GPtrArray *list)
{
    GPtrArray *dst = track_section_list(t);
    if (dst->len > 0) g_ptr_array_remove_range(dst, 0, dst->len);
    if (jackdaw_track_is_instrument(t)) {
        for (guint i = 0; i < list->len; i++)
            g_ptr_array_add(dst, midi_region_copy(g_ptr_array_index(list, i)));
    } else {
        for (guint i = 0; i < list->len; i++)
            g_ptr_array_add(dst, clip_region_copy(g_ptr_array_index(list, i)));
    }
    track_commit_sections(tl, t);
}

/* Find the track whose wave view contains root-space y. During a drag the
 * pointer is grabbed by the source view but travels over other rows; this maps
 * the vertical position back to a target track. NULL = outside any track. */
static JackDawTrack *timeline_track_at_root_y(JackDawTimeline *tl, gdouble y_root)
{
    GHashTableIter it; gpointer k, v;
    g_hash_table_iter_init(&it, tl->wave_views);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        GtkWidget *wv  = GTK_WIDGET(v);
        GdkWindow *win = gtk_widget_get_window(wv);
        if (!win || !gtk_widget_get_mapped(wv)) continue;
        gint ox, oy; gdk_window_get_origin(win, &ox, &oy);
        GtkAllocation a; gtk_widget_get_allocation(wv, &a);
        if (y_root >= oy && y_root < oy + a.height) return (JackDawTrack *)k;
    }
    return NULL;
}

/* Drop the whole section selection and any in-flight move. */
static void timeline_clear_section_sel(JackDawTimeline *tl)
{
    if (tl->sel_regions) g_ptr_array_set_size(tl->sel_regions, 0);
    tl->sel_track      = NULL;
    tl->move_armed     = FALSE;
    tl->moving         = FALSE;
    tl->move_committed = FALSE;
    tl->move_src       = NULL;
    g_clear_pointer(&tl->move_orig, g_free);
    if (tl->move_pre) {
        g_hash_table_destroy(tl->move_pre);
        tl->move_pre = NULL;
    }
}

static gboolean timeline_sel_contains(JackDawTimeline *tl, gpointer r)
{
    if (!tl->sel_regions) return FALSE;
    for (guint i = 0; i < tl->sel_regions->len; i++)
        if (g_ptr_array_index(tl->sel_regions, i) == r) return TRUE;
    return FALSE;
}

/* Select the single region of `track` that covers `frame` (highlight its full
 * span); clears the selection if the frame is in a gap. */
static void timeline_select_region_at(JackDawTimeline *tl, JackDawTrack *track,
                                      off_t frame)
{
    timeline_clear_section_sel(tl);
    tl->sel_active = FALSE;          /* section selection supersedes the range */
    gpointer r = track ? sec_list_at(tl, track, frame) : NULL;
    if (r) {
        tl->sel_track = track;
        g_ptr_array_add(tl->sel_regions, r);
    }
    jackdaw_timeline_redraw_all(tl);
}

/* Move the edit playhead to `frame` (and seek the engine when stopped). */
static void timeline_set_playhead(JackDawTimeline *tl, off_t frame)
{
    gtk_adjustment_set_value(tl->cursor_adj, (gdouble)frame);
    if (!jackdaw_engine_is_playing())
        jackdaw_engine_locate(frame);
}

/* ---- Region-edit undo/redo (routed through the project's global manager) ----
 *
 * One memento per edit: capture the track's region list before the edit, push
 * an action onto JackDawProject's undo manager. ctx carries {timeline, track}
 * so the restore can repaint and drop the now-stale section selection. */

typedef struct { JackDawTimeline *tl; JackDawTrack *t; } RegionUndoCtx;

static gpointer region_undo_capture(gpointer ctx)
{
    RegionUndoCtx *c = ctx;
    return track_section_list_copy(c->t);
}

static void region_undo_restore(gpointer ctx, gpointer state)
{
    RegionUndoCtx *c = ctx;
    track_apply_section_list(c->tl, c->t, (GPtrArray *)state);
}

static void region_undo_free_state(gpointer state)
{
    if (state) g_ptr_array_unref((GPtrArray *)state);
}

static void region_undo_after(gpointer ctx)
{
    RegionUndoCtx *c = ctx;
    timeline_clear_section_sel(c->tl);   /* region pointers are now stale */
    jackdaw_timeline_redraw_all(c->tl);
}

/* Snapshot the track's sections BEFORE an edit and push the undo action.
 * Works for audio (ClipRegion) and instrument (MidiRegion) tracks alike. */
static void timeline_push_undo(JackDawTimeline *tl, JackDawTrack *t)
{
    if (!tl->project) return;
    RegionUndoCtx *c = g_new0(RegionUndoCtx, 1);
    c->tl = tl; c->t = t;
    JackDawUndoAction a = {
        .ctx         = c,
        .saved_state = track_section_list_copy(t),
        .capture_fn  = region_undo_capture,
        .restore_fn  = region_undo_restore,
        .free_fn     = region_undo_free_state,
        .after_fn    = region_undo_after,
        .ctx_free_fn = g_free,
        .desc        = g_strdup("Region edit"),
    };
    undo_manager_push(jackdaw_project_get_undo(tl->project), &a);
}

/* ---- Combined multi-track move undo --------------------------------------
 * A section move can relocate across tracks, so the memento snapshots every
 * track the drag touched (captured pristine into tl->move_pre at drag start).
 * One Ctrl+Z then restores source and destination together. */
typedef struct {
    JackDawTimeline *tl;
    guint            n;
    JackDawTrack   **tracks;   /* strong refs */
} MoveUndoCtx;

static gpointer move_undo_capture(gpointer ctx)
{
    MoveUndoCtx *c = ctx;
    GPtrArray *lists = g_ptr_array_new();          /* of GPtrArray* */
    for (guint i = 0; i < c->n; i++)
        g_ptr_array_add(lists, track_section_list_copy(c->tracks[i]));
    return lists;
}

static void move_undo_restore(gpointer ctx, gpointer state)
{
    MoveUndoCtx *c = ctx;
    GPtrArray *lists = state;
    for (guint i = 0; i < c->n && i < lists->len; i++)
        track_apply_section_list(c->tl, c->tracks[i],
                                 g_ptr_array_index(lists, i));
}

static void move_undo_free_state(gpointer state)
{
    GPtrArray *lists = state;
    if (!lists) return;
    for (guint i = 0; i < lists->len; i++)
        g_ptr_array_unref(g_ptr_array_index(lists, i));
    g_ptr_array_free(lists, TRUE);
}

static void move_undo_after(gpointer ctx)
{
    MoveUndoCtx *c = ctx;
    timeline_clear_section_sel(c->tl);   /* pointers now stale */
    jackdaw_timeline_redraw_all(c->tl);
}

static void move_undo_ctx_free(gpointer ctx)
{
    MoveUndoCtx *c = ctx;
    for (guint i = 0; i < c->n; i++)
        if (c->tracks[i]) g_object_unref(c->tracks[i]);
    g_free(c->tracks);
    g_free(c);
}

/* Capture a track's pristine section list into tl->move_pre the first time the
 * drag touches it, so the eventual combined undo restores it correctly. */
static void move_pre_ensure(JackDawTimeline *tl, JackDawTrack *t)
{
    if (!tl->move_pre)
        tl->move_pre = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                             NULL,
                                             (GDestroyNotify)g_ptr_array_unref);
    if (!g_hash_table_contains(tl->move_pre, t))
        g_hash_table_insert(tl->move_pre, t, track_section_list_copy(t));
}

/* Push the combined undo built from every track captured in tl->move_pre. */
static void timeline_push_move_undo(JackDawTimeline *tl)
{
    if (!tl->project || !tl->move_pre) return;
    guint n = g_hash_table_size(tl->move_pre);
    if (n == 0) return;

    MoveUndoCtx *c = g_new0(MoveUndoCtx, 1);
    c->tl     = tl;
    c->n      = n;
    c->tracks = g_new0(JackDawTrack *, n);
    GPtrArray *saved = g_ptr_array_new();

    GHashTableIter it; gpointer k, v; guint i = 0;
    g_hash_table_iter_init(&it, tl->move_pre);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        c->tracks[i] = g_object_ref((JackDawTrack *)k);
        g_ptr_array_add(saved, g_ptr_array_ref((GPtrArray *)v));  /* pristine */
        i++;
    }

    JackDawUndoAction a = {
        .ctx         = c,
        .saved_state = saved,
        .capture_fn  = move_undo_capture,
        .restore_fn  = move_undo_restore,
        .free_fn     = move_undo_free_state,
        .after_fn    = move_undo_after,
        .ctx_free_fn = move_undo_ctx_free,
        .desc        = g_strdup("Move section"),
    };
    undo_manager_push(jackdaw_project_get_undo(tl->project), &a);
}

void jackdaw_timeline_undo(JackDawTimeline *tl)
{
    g_return_if_fail(JACKDAW_IS_TIMELINE(tl));
    if (tl->project) jackdaw_project_undo(tl->project);
}

void jackdaw_timeline_redo(JackDawTimeline *tl)
{
    g_return_if_fail(JACKDAW_IS_TIMELINE(tl));
    if (tl->project) jackdaw_project_redo(tl->project);
}

/* ---- Region edit operations ---- */

/* Split `t` at the playhead and select the region to the right of the split. */
static void timeline_split_track_at_playhead(JackDawTimeline *tl,
                                             JackDawTrack *t)
{
    if (!t) return;
    off_t cur = (off_t)gtk_adjustment_get_value(tl->cursor_adj);
    timeline_push_undo(tl, t);
    if (jackdaw_track_is_instrument(t))
        midi_region_list_split_at(jackdaw_track_get_midi_regions(t), cur,
                                  timeline_frames_per_tick(tl));
    else
        clip_region_list_split_at(jackdaw_track_get_regions(t), cur,
                                  (int)timeline_jack_sr());
    track_commit_sections(tl, t);
    /* Focus the new right-hand region so it can be moved/deleted next. */
    timeline_select_region_at(tl, t, cur);
    jackdaw_timeline_redraw_all(tl);
}

void jackdaw_timeline_split_at_cursor(JackDawTimeline *tl)
{
    g_return_if_fail(JACKDAW_IS_TIMELINE(tl));
    timeline_split_track_at_playhead(tl, tl->focused_track);
}

/* Merge the selected sections (single track) into single regions where they are
 * adjacent and share a source clip. */
void jackdaw_timeline_group_selection(JackDawTimeline *tl)
{
    g_return_if_fail(JACKDAW_IS_TIMELINE(tl));
    if (!tl->sel_track || !tl->sel_regions || tl->sel_regions->len < 2) return;
    if (jackdaw_track_is_instrument(tl->sel_track)) return;   /* audio only */

    JackDawTrack *track = tl->sel_track;
    guint n = tl->sel_regions->len;
    off_t *tlpos = g_new(off_t, n);
    for (guint i = 0; i < n; i++)
        tlpos[i] = ((ClipRegion *)g_ptr_array_index(tl->sel_regions, i))->tl_pos;

    timeline_push_undo(tl, track);
    clip_region_list_group(jackdaw_track_get_regions(track),
                           tlpos, n, (int)timeline_jack_sr());
    g_free(tlpos);

    /* The merge frees the absorbed regions, so the selection pointers are now
     * stale — drop the selection. */
    timeline_clear_section_sel(tl);
    jackdaw_track_commit_regions(track);
    jackdaw_timeline_redraw_all(tl);
}

/* ---- Context menu ---- */

static void menu_split_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    JackDawTimeline *tl = data;
    timeline_split_track_at_playhead(tl, tl->menu_track);
}

/* Delete the current selection. `fallback` is the track the rubber-band range
 * applies to when there is no section selection — the right-clicked track from
 * the context menu, the focused track from the Edit menu. */
static void timeline_delete_selection(JackDawTimeline *tl,
                                      JackDawTrack *fallback)
{
    int sr = (int)timeline_jack_sr();

    /* Instrument track: remove the selected MIDI sections outright. */
    if (tl->sel_track && jackdaw_track_is_instrument(tl->sel_track) &&
        tl->sel_regions && tl->sel_regions->len > 0) {
        JackDawTrack *track = tl->sel_track;
        timeline_push_undo(tl, track);
        GPtrArray *regs = jackdaw_track_get_midi_regions(track);
        for (guint i = 0; i < tl->sel_regions->len; i++)
            g_ptr_array_remove(regs, g_ptr_array_index(tl->sel_regions, i));
        timeline_clear_section_sel(tl);
        track_commit_sections(tl, track);
        jackdaw_timeline_redraw_all(tl);
        return;
    }

    /* Prefer the section selection; fall back to the rubber-band range. */
    if (tl->sel_track && tl->sel_regions && tl->sel_regions->len > 0) {
        JackDawTrack *track = tl->sel_track;
        guint n = tl->sel_regions->len;
        off_t *aa = g_new(off_t, n), *bb = g_new(off_t, n);
        for (guint i = 0; i < n; i++) {
            ClipRegion *r = g_ptr_array_index(tl->sel_regions, i);
            aa[i] = r->tl_pos;
            bb[i] = clip_region_end(r);
        }
        timeline_push_undo(tl, track);
        GPtrArray *regs = jackdaw_track_get_regions(track);
        for (guint i = 0; i < n; i++)          /* spans cached: pointers freed */
            clip_region_list_delete_range(regs, aa[i], bb[i], sr);
        g_free(aa); g_free(bb);
        timeline_clear_section_sel(tl);
        jackdaw_track_commit_regions(track);
        jackdaw_timeline_redraw_all(tl);
        return;
    }

    if (!fallback || !tl->sel_active) return;
    off_t a = tl->sel_start, b = tl->sel_end;
    if (b < a) { off_t tmp = a; a = b; b = tmp; }
    timeline_push_undo(tl, fallback);
    clip_region_list_delete_range(jackdaw_track_get_regions(fallback), a, b, sr);
    jackdaw_track_commit_regions(fallback);
    jackdaw_timeline_redraw_all(tl);
}

static void menu_delete_sel_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    JackDawTimeline *tl = data;
    timeline_delete_selection(tl, tl->menu_track);
}

void jackdaw_timeline_delete_selection(JackDawTimeline *tl)
{
    g_return_if_fail(JACKDAW_IS_TIMELINE(tl));
    timeline_delete_selection(tl, tl->focused_track);
}

/* Replace the clipboard with `regs` (consumed): copies normalized so the
 * earliest region starts at frame 0. */
static void timeline_clipboard_set(JackDawTimeline *tl, GPtrArray *regs)
{
    g_ptr_array_set_size(tl->clipboard, 0);
    if (regs->len == 0) return;
    off_t origin = ((ClipRegion *)g_ptr_array_index(regs, 0))->tl_pos;
    for (guint i = 1; i < regs->len; i++) {
        off_t p = ((ClipRegion *)g_ptr_array_index(regs, i))->tl_pos;
        if (p < origin) origin = p;
    }
    for (guint i = 0; i < regs->len; i++) {
        ClipRegion *c = clip_region_copy(g_ptr_array_index(regs, i));
        c->tl_pos -= origin;
        g_ptr_array_add(tl->clipboard, c);
    }
}

/* Copy the selected area to the clipboard.  Mirrors menu_delete_sel_cb's
 * selection model: prefer the section selection, else the rubber-band range
 * (sliced sample-rate-correctly out of `range_track`). */
/* Replace the MIDI clipboard with copies of `regs` (MidiRegion*), normalized so
 * the earliest starts at frame 0.
 *
 * The copies are frozen (auto_grow = FALSE) so a pasted section keeps the size
 * it was copied at, instead of silently expanding to cover every note in the
 * shared source clip the way an untouched full-clip region does. */
static void timeline_midi_clipboard_set(JackDawTimeline *tl, GPtrArray *regs)
{
    g_ptr_array_set_size(tl->midi_clipboard, 0);
    if (!regs || regs->len == 0) return;

    off_t origin = ((MidiRegion *)g_ptr_array_index(regs, 0))->tl_pos;
    for (guint i = 1; i < regs->len; i++) {
        off_t p = ((MidiRegion *)g_ptr_array_index(regs, i))->tl_pos;
        if (p < origin) origin = p;
    }
    for (guint i = 0; i < regs->len; i++) {
        MidiRegion *c = midi_region_copy(g_ptr_array_index(regs, i));
        c->tl_pos  -= origin;
        c->auto_grow = FALSE;
        g_ptr_array_add(tl->midi_clipboard, c);
    }
}

static void timeline_copy_selection(JackDawTimeline *tl, JackDawTrack *range_track)
{
    int sr = (int)timeline_jack_sr();

    if (tl->sel_track && tl->sel_regions && tl->sel_regions->len > 0) {
        if (jackdaw_track_is_instrument(tl->sel_track)) {
            timeline_midi_clipboard_set(tl, tl->sel_regions);
            g_ptr_array_set_size(tl->clipboard, 0);   /* one clipboard is live */
            return;
        }
        timeline_clipboard_set(tl, tl->sel_regions);
        g_ptr_array_set_size(tl->midi_clipboard, 0);
        return;
    }

    if (!range_track || jackdaw_track_is_instrument(range_track) ||
        !tl->sel_active) return;
    off_t a = tl->sel_start, b = tl->sel_end;
    if (b < a) { off_t tmp = a; a = b; b = tmp; }
    if (b <= a) return;

    /* Deep-copy the track, then trim everything outside [a,b] using the same
     * sample-rate-aware delete used by Delete Selected Area. */
    GPtrArray *tmp = clip_region_list_copy(jackdaw_track_get_regions(range_track));
    off_t big = clip_region_list_total_frames(tmp) + 1;
    if (a > 0)  clip_region_list_delete_range(tmp, 0, a, sr);
    if (b < big) clip_region_list_delete_range(tmp, b, big, sr);
    timeline_clipboard_set(tl, tmp);
    g_ptr_array_unref(tmp);
}

/* Paste the MIDI clipboard onto an instrument track at the playhead, overwriting
 * the paste span: split the existing sections at both edges, drop whatever now
 * falls entirely inside, then place the copies. */
static void timeline_paste_midi_to_track(JackDawTimeline *tl, JackDawTrack *dest)
{
    if (!tl->midi_clipboard || tl->midi_clipboard->len == 0) return;

    double fpt = timeline_frames_per_tick(tl);
    if (fpt <= 0.0) return;

    off_t at = (off_t)gtk_adjustment_get_value(tl->cursor_adj);
    if (at < 0) at = 0;

    off_t span = 0;                    /* normalized width of the clipboard */
    for (guint i = 0; i < tl->midi_clipboard->len; i++) {
        off_t end = midi_region_end(g_ptr_array_index(tl->midi_clipboard, i), fpt);
        if (end > span) span = end;
    }
    if (span <= 0) return;

    GPtrArray *regs = jackdaw_track_get_midi_regions(dest);
    timeline_push_undo(tl, dest);

    midi_region_list_split_at(regs, at,        fpt);
    midi_region_list_split_at(regs, at + span, fpt);

    /* Drop the sections now wholly inside the paste span. One tick of slack
     * absorbs the rounding in the tick<->frame conversion at the split edges. */
    off_t eps = (off_t)fpt + 1;
    for (guint i = regs->len; i > 0; ) {
        MidiRegion *r = g_ptr_array_index(regs, --i);
        off_t s = r->tl_pos, e = midi_region_end(r, fpt);
        if (s >= at - eps && e <= at + span + eps)
            g_ptr_array_remove_index(regs, i);
    }

    for (guint i = 0; i < tl->midi_clipboard->len; i++) {
        MidiRegion *c = midi_region_copy(g_ptr_array_index(tl->midi_clipboard, i));
        c->tl_pos += at;
        g_ptr_array_add(regs, c);
    }
    midi_region_list_sort(regs);
    track_commit_sections(tl, dest);
    jackdaw_timeline_redraw_all(tl);
}

/* Paste the clipboard onto `dest`, anchored at the playhead.  The paste span is
 * cleared first (overwrite) so no regions overlap. */
static void timeline_paste_to_track(JackDawTimeline *tl, JackDawTrack *dest)
{
    if (!dest) return;
    if (jackdaw_track_is_instrument(dest)) {
        timeline_paste_midi_to_track(tl, dest);
        return;
    }
    if (!tl->clipboard || tl->clipboard->len == 0) return;

    off_t at = (off_t)gtk_adjustment_get_value(tl->cursor_adj);
    if (at < 0) at = 0;

    off_t span = 0;                         /* normalized width of the clipboard */
    for (guint i = 0; i < tl->clipboard->len; i++) {
        off_t end = clip_region_end(g_ptr_array_index(tl->clipboard, i));
        if (end > span) span = end;
    }

    int sr = (int)timeline_jack_sr();
    GPtrArray *regs = jackdaw_track_get_regions(dest);
    timeline_push_undo(tl, dest);
    clip_region_list_delete_range(regs, at, at + span, sr);   /* clear the span */
    for (guint i = 0; i < tl->clipboard->len; i++) {
        ClipRegion *c = clip_region_copy(g_ptr_array_index(tl->clipboard, i));
        c->tl_pos += at;
        g_ptr_array_add(regs, c);
    }
    clip_region_list_sort(regs);
    jackdaw_track_commit_regions(dest);
    jackdaw_timeline_redraw_all(tl);
}

/* Public entry points: keyboard Ctrl+C / Ctrl+V act on the focused track. */
void jackdaw_timeline_copy_selection(JackDawTimeline *tl)
{
    g_return_if_fail(JACKDAW_IS_TIMELINE(tl));
    timeline_copy_selection(tl, tl->focused_track);
}
void jackdaw_timeline_paste_at_cursor(JackDawTimeline *tl)
{
    g_return_if_fail(JACKDAW_IS_TIMELINE(tl));
    timeline_paste_to_track(tl, tl->focused_track);
}

/* Right-click menu: act on the track under the pointer. */
static void menu_copy_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    JackDawTimeline *tl = data;
    timeline_copy_selection(tl, tl->menu_track);
}

static void menu_paste_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    JackDawTimeline *tl = data;
    timeline_paste_to_track(tl, tl->menu_track);
}

static void menu_group_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    jackdaw_timeline_group_selection((JackDawTimeline *)data);
}

static void menu_delete_region_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    JackDawTimeline *tl = data;
    if (!tl->menu_track) return;
    timeline_push_undo(tl, tl->menu_track);
    if (jackdaw_track_is_instrument(tl->menu_track)) {
        GPtrArray *regs = jackdaw_track_get_midi_regions(tl->menu_track);
        MidiRegion *r = midi_region_list_at(regs, tl->menu_frame,
                                            timeline_frames_per_tick(tl));
        if (r) g_ptr_array_remove(regs, r);
    } else {
        clip_region_list_remove_at(jackdaw_track_get_regions(tl->menu_track),
                                   tl->menu_frame);
    }
    track_commit_sections(tl, tl->menu_track);
    jackdaw_timeline_redraw_all(tl);
}

static void menu_clear_loop_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    JackDawTimeline *tl = data;
    jackdaw_engine_set_loop_range(0, 0);
    jackdaw_engine_set_loop_enabled(FALSE);
    gtk_widget_queue_draw(GTK_WIDGET(tl->ruler));
    jackdaw_timeline_redraw_all(tl);
}

static void menu_open_midi_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    JackDawTimeline *tl = data;
    if (!tl->menu_track || !jackdaw_track_is_instrument(tl->menu_track)) return;
    jackdaw_midi_window_open(tl->menu_track, tl->project);
}

/* Delete the RIGHT-CLICKED track (tl->menu_track), not the active one. */
static void menu_delete_track_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    JackDawTimeline *tl = data;
    if (!tl->menu_track || !tl->project) return;
    jackdaw_project_delete_track(tl->project, tl->menu_track);
}

static void menu_gain_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    JackDawTimeline *tl = data;

    gboolean have_sections =
        tl->sel_track && tl->sel_regions && tl->sel_regions->len > 0;
    JackDawTrack *track = have_sections ? tl->sel_track : tl->menu_track;
    if (!track) return;
    if (jackdaw_track_is_instrument(track)) return;   /* gain is audio-only */
    if (!have_sections && !tl->sel_active) return;
    off_t a = tl->sel_start, b = tl->sel_end;
    if (b < a) { off_t tmp = a; a = b; b = tmp; }

    /* Seed the slider with the selection's current gain so re-opening the dialog
     * shows (and adjusts from) the value already applied. */
    ClipRegion *cur = have_sections
        ? g_ptr_array_index(tl->sel_regions, 0)
        : clip_region_list_at(jackdaw_track_get_regions(track), a);
    double cur_db = (cur && cur->gain > 0.0f)
        ? CLAMP(20.0 * log10((double)cur->gain), -25.0, 25.0) : 0.0;

    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "Region Gain",
        GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(tl))),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Apply", GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget *sc  = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
                                              -25.0, 25.0, 0.5);
    gtk_range_set_value(GTK_RANGE(sc), cur_db);
    gtk_widget_set_size_request(sc, 260, -1);
    GtkWidget *lbl = gtk_label_new("Gain (dB) for the selected area:");
    gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(box), sc,  FALSE, FALSE, 4);
    gtk_widget_show_all(dlg);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        double db = gtk_range_get_value(GTK_RANGE(sc));
        gfloat g  = (gfloat)pow(10.0, db / 20.0);
        int sr = (int)timeline_jack_sr();
        GPtrArray *regs = jackdaw_track_get_regions(track);
        timeline_push_undo(tl, track);
        if (have_sections) {
            guint n = tl->sel_regions->len;
            off_t *aa = g_new(off_t, n), *bb = g_new(off_t, n);
            for (guint i = 0; i < n; i++) {
                ClipRegion *r = g_ptr_array_index(tl->sel_regions, i);
                aa[i] = r->tl_pos; bb[i] = clip_region_end(r);
            }
            for (guint i = 0; i < n; i++)
                clip_region_list_set_gain_range(regs, aa[i], bb[i], g, sr);
            g_free(aa); g_free(bb);
        } else {
            clip_region_list_set_gain_range(regs, a, b, g, sr);
        }
        jackdaw_track_commit_regions(track);
        jackdaw_timeline_redraw_all(tl);
    }
    gtk_widget_destroy(dlg);
}

static void timeline_show_context_menu(JackDawTimeline *tl, GdkEventButton *ev)
{
    GtkWidget *menu = gtk_menu_new();
    /* The track the section ops will act on (selection track, else the one under
     * the pointer). Gain/Group remain audio-only; Copy/Paste now work on MIDI
     * sections too, through a separate clipboard. */
    JackDawTrack *op_track = (tl->sel_regions && tl->sel_regions->len > 0)
        ? tl->sel_track : tl->menu_track;
    gboolean op_audio = op_track && !jackdaw_track_is_instrument(op_track);
    gboolean op_midi  = op_track && jackdaw_track_is_instrument(op_track);
    gboolean have_sel = (tl->sel_regions && tl->sel_regions->len > 0) ||
                        tl->sel_active;
    gboolean can_group = tl->sel_regions && tl->sel_regions->len >= 2 && op_audio;

    /* A MIDI copy only pastes onto an instrument track, an audio copy only onto
     * an audio track — the source material and the destination must agree. */
    gboolean can_copy = have_sel &&
                        (op_audio ||
                         (op_midi && tl->sel_regions && tl->sel_regions->len > 0));
    gboolean dest_midi = tl->menu_track &&
                         jackdaw_track_is_instrument(tl->menu_track);
    gboolean can_paste = tl->menu_track &&
        (dest_midi ? (tl->midi_clipboard && tl->midi_clipboard->len > 0)
                   : (tl->clipboard      && tl->clipboard->len      > 0));
    struct { const char *label; GCallback cb; gboolean sens; } items[] = {
        { "Split at Playhead",   G_CALLBACK(menu_split_cb),         TRUE },
        { "Delete Selected Area",G_CALLBACK(menu_delete_sel_cb),    have_sel },
        { "Copy",                G_CALLBACK(menu_copy_cb),          can_copy },
        { "Paste at Playhead",   G_CALLBACK(menu_paste_cb),         can_paste },
        { "Set Selection Gain…", G_CALLBACK(menu_gain_cb),          have_sel && op_audio },
        { "Group Sections",      G_CALLBACK(menu_group_cb),         can_group },
        { "Delete Region",       G_CALLBACK(menu_delete_region_cb), TRUE },
        { "Clear Loop Region",   G_CALLBACK(menu_clear_loop_cb),
          jackdaw_engine_has_loop_region() },
    };
    for (guint i = 0; i < G_N_ELEMENTS(items); i++) {
        GtkWidget *mi = gtk_menu_item_new_with_label(items[i].label);
        gtk_widget_set_sensitive(mi, items[i].sens);
        g_signal_connect(mi, "activate", items[i].cb, tl);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    }

    if (tl->menu_track && jackdaw_track_is_instrument(tl->menu_track)) {
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        GtkWidget *mi = gtk_menu_item_new_with_label("Open MIDI Editor");
        g_signal_connect(mi, "activate", G_CALLBACK(menu_open_midi_cb), tl);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    }

    if (tl->menu_track) {
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        GtkWidget *mi = gtk_menu_item_new_with_label("Delete Track");
        g_signal_connect(mi, "activate", G_CALLBACK(menu_delete_track_cb), tl);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    }

    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
}

/* Snap a section-move delta (timeline frames).  When snap is enabled, consider
 * both the beat grid (block leading edge) and the edges of non-selected regions
 * on the move track, and apply the smallest correction within ~10px. */
static off_t timeline_snap_move_delta(JackDawTimeline *tl, off_t raw_delta)
{
    if (!tl->project || !tl->project->snap_enabled) return raw_delta;
    if (!tl->sel_track || !tl->sel_regions || !tl->move_orig) return raw_delta;

    gdouble spp    = gtk_adjustment_get_value(tl->zoom_adj);
    off_t   thresh = (off_t)(spp * 10.0);     /* ~10px snap radius */
    if (thresh < 1) thresh = 1;
    int   sr = (int)timeline_jack_sr();
    guint n  = tl->sel_regions->len;

    gboolean have = FALSE;
    off_t best_corr = 0;
    off_t best_abs  = thresh + 1;

    /* Candidate: snap the block's leading (leftmost) edge to the beat grid. */
    off_t lead = G_MAXINT64;
    for (guint i = 0; i < n; i++) {
        off_t e = tl->move_orig[i] + raw_delta;
        if (e < lead) lead = e;
    }
    {
        off_t corr = jackdaw_project_snap_frame(tl->project, lead, sr) - lead;
        off_t a = corr < 0 ? -corr : corr;
        if (a <= thresh && a < best_abs) { best_abs = a; best_corr = corr; have = TRUE; }
    }

    /* Candidate: snap any moving edge to any non-selected section edge. */
    GPtrArray *regs = track_section_list(tl->sel_track);
    for (guint i = 0; i < n; i++) {
        gpointer m = g_ptr_array_index(tl->sel_regions, i);
        off_t m_len = sec_end(tl, tl->sel_track, m) -
                      sec_tl_pos(tl->sel_track, m);
        off_t mine[2] = { tl->move_orig[i] + raw_delta,
                          tl->move_orig[i] + raw_delta + m_len };
        for (guint j = 0; j < regs->len; j++) {
            gpointer o = g_ptr_array_index(regs, j);
            if (timeline_sel_contains(tl, o)) continue;   /* skip moving sections */
            off_t edges[2] = { sec_tl_pos(tl->sel_track, o),
                               sec_end(tl, tl->sel_track, o) };
            for (int em = 0; em < 2; em++)
                for (int eo = 0; eo < 2; eo++) {
                    off_t corr = edges[eo] - mine[em];
                    off_t a = corr < 0 ? -corr : corr;
                    if (a <= thresh && a < best_abs) {
                        best_abs = a; best_corr = corr; have = TRUE;
                    }
                }
        }
    }

    return have ? raw_delta + best_corr : raw_delta;
}

/* Button press on a WaveView — focus, selection anchor, or context menu */
static gboolean timeline_wave_clicked(GtkWidget *widget,
                                       GdkEventButton *event, gpointer data)
{
    JackDawTimeline *tl = JACKDAW_TIMELINE(data);
    JackDawWaveView *wv = JACKDAW_WAVE_VIEW(widget);

    off_t sample = timeline_x_to_sample(tl, event->x);
    /* Unify strip + timeline selection: a plain click (left or right) selects
     * just this track so both the strip and the timeline highlight as one unit.
     * Ctrl+left-click keeps any existing track multi-selection and only makes
     * this the active track — the waveform Ctrl+click handling further down is
     * for region/section selection within the track, not track-unit toggling. */
    if (tl->project && wv->track) {
        if (event->button == 1 && (event->state & GDK_CONTROL_MASK))
            jackdaw_project_set_active_track(tl->project, wv->track);
        else
            jackdaw_project_select_single(tl->project, wv->track);
    }

    /* Double-click on an instrument track opens the MIDI window. */
    if (event->type == GDK_2BUTTON_PRESS && wv->track &&
        jackdaw_track_is_instrument(wv->track)) {
        timeline_clear_section_sel(tl);
        tl->sel_active = FALSE;
        tl->selecting  = FALSE;
        jackdaw_midi_window_open(wv->track, tl->project);
        jackdaw_timeline_redraw_all(tl);
        return TRUE;
    }

    if (event->button == 3) {
        tl->menu_track = wv->track;
        tl->menu_frame = sample;
        gpointer r = wv->track ? sec_list_at(tl, wv->track, sample) : NULL;
        /* Keep an existing multi-selection if the user right-clicked one of its
         * members; otherwise select the region under the pointer. */
        if (!(r && tl->sel_track == wv->track && timeline_sel_contains(tl, r)))
            timeline_select_region_at(tl, wv->track, sample);
        timeline_show_context_menu(tl, event);
        return TRUE;
    }

    if (event->button != 1) return FALSE;

    gpointer r = wv->track ? sec_list_at(tl, wv->track, sample) : NULL;

    /* Ctrl+click toggles a section in the multi-selection (single track). */
    if (event->state & GDK_CONTROL_MASK) {
        if (r) {
            if (tl->sel_track != wv->track) {
                timeline_clear_section_sel(tl);
                tl->sel_active = FALSE;
                tl->sel_track  = wv->track;
            }
            if (timeline_sel_contains(tl, r))
                g_ptr_array_remove(tl->sel_regions, r);
            else
                g_ptr_array_add(tl->sel_regions, r);
            if (tl->sel_regions->len == 0) tl->sel_track = NULL;
        }
        jackdaw_timeline_redraw_all(tl);
        return TRUE;
    }

    /* Plain press on an already-selected section → arm a potential move-drag.
     * If the pointer doesn't move, the release falls through to a plain seek. */
    if (r && tl->sel_track == wv->track && timeline_sel_contains(tl, r)) {
        tl->move_armed        = TRUE;
        tl->moving            = FALSE;
        tl->move_committed    = FALSE;
        tl->move_press_x      = event->x;
        tl->move_press_y_root = event->y_root;
        tl->move_src          = wv->track;
        guint n = tl->sel_regions->len;
        g_free(tl->move_orig);
        tl->move_orig = g_new(off_t, n);
        for (guint i = 0; i < n; i++)
            tl->move_orig[i] =
                sec_tl_pos(wv->track, g_ptr_array_index(tl->sel_regions, i));
        return TRUE;
    }

    /* Otherwise: move the playhead and start a potential rubber-band range. */
    timeline_clear_section_sel(tl);
    timeline_set_playhead(tl, sample);
    tl->selecting  = TRUE;
    tl->sel_active = FALSE;
    tl->sel_start  = sample;
    tl->sel_end    = sample;
    jackdaw_timeline_redraw_all(tl);

    g_signal_emit(tl, timeline_signals[SIGNAL_POSITION_CHANGED], 0,
                  (gint64)sample);
    return FALSE;
}

static gboolean timeline_wave_motion(GtkWidget *widget,
                                     GdkEventMotion *event, gpointer data)
{
    (void)widget;
    JackDawTimeline *tl = JACKDAW_TIMELINE(data);

    /* A drag armed on a selected section becomes a real move once the pointer
     * travels past a small threshold in either axis (horizontal = slide,
     * vertical = move to another track); until then it may be a plain click. */
    if (tl->move_armed && !tl->moving) {
        if (fabs(event->x - tl->move_press_x) > 3.0 ||
            fabs(event->y_root - tl->move_press_y_root) > 3.0)
            tl->moving = TRUE;
        else
            return TRUE;   /* swallow tiny jitters; keep waiting */
    }

    /* Section move-drag: shift every selected section by a snapped delta, and
     * relocate the whole block to whichever (compatible) track the pointer is
     * over for a vertical move. */
    if (tl->moving && tl->sel_regions && tl->move_orig && tl->sel_track) {
        /* Vertical: move the block onto the track under the pointer, if it is a
         * different track of the same kind (audio↔audio, instrument↔instrument).
         * Relocate the section pointers between the two lists; tl_pos is set
         * below from move_orig so the horizontal offset is preserved. */
        JackDawTrack *tgt = timeline_track_at_root_y(tl, event->y_root);
        if (tgt && tgt != tl->sel_track &&
            jackdaw_track_is_instrument(tgt) ==
                jackdaw_track_is_instrument(tl->sel_track)) {
            move_pre_ensure(tl, tl->sel_track);   /* pristine source */
            move_pre_ensure(tl, tgt);             /* pristine destination */
            tl->move_committed = TRUE;

            GPtrArray *from = track_section_list(tl->sel_track);
            GPtrArray *to   = track_section_list(tgt);
            guint n = tl->sel_regions->len;
            for (guint i = 0; i < n; i++) {
                gpointer s = g_ptr_array_index(tl->sel_regions, i);
                guint idx;
                if (g_ptr_array_find(from, s, &idx)) {
                    g_ptr_array_steal_index_fast(from, idx);
                    g_ptr_array_add(to, s);
                }
            }
            tl->sel_track = tgt;
        }

        gdouble spp = gtk_adjustment_get_value(tl->zoom_adj);
        off_t   raw = (off_t)((event->x - tl->move_press_x) * spp);
        off_t   delta = timeline_snap_move_delta(tl, raw);
        guint   n = tl->sel_regions->len;

        /* Clamp so no section starts before 0. */
        off_t min_orig = G_MAXINT64;
        for (guint i = 0; i < n; i++)
            if (tl->move_orig[i] < min_orig) min_orig = tl->move_orig[i];
        if (min_orig + delta < 0) delta = -min_orig;

        if (delta != 0 && !tl->move_committed) {
            move_pre_ensure(tl, tl->sel_track);   /* captures pre-move state */
            tl->move_committed = TRUE;
        }
        for (guint i = 0; i < n; i++)
            sec_set_tl_pos(tl->sel_track,
                           g_ptr_array_index(tl->sel_regions, i),
                           tl->move_orig[i] + delta);
        jackdaw_timeline_redraw_all(tl);
        return TRUE;
    }

    if (!tl->selecting) return FALSE;
    off_t sample = timeline_x_to_sample(tl, event->x);
    tl->sel_end = sample;
    off_t span = sample - tl->sel_start;
    if (span < 0) span = -span;
    /* Treat as a selection once the drag exceeds a few pixels' worth. */
    gdouble spp = gtk_adjustment_get_value(tl->zoom_adj);
    if ((gdouble)span > spp * 3.0) tl->sel_active = TRUE;
    jackdaw_timeline_redraw_all(tl);
    return TRUE;
}

static gboolean timeline_wave_released(GtkWidget *widget,
                                       GdkEventButton *event, gpointer data)
{
    JackDawTimeline *tl = JACKDAW_TIMELINE(data);
    if (event->button != 1) return FALSE;

    /* Finalize a section move-drag: re-sort and rebuild the RT snapshot for
     * every track the drag touched (source + any destination), then push one
     * combined undo. */
    if (tl->moving) {
        tl->moving     = FALSE;
        tl->move_armed = FALSE;
        if (tl->move_pre) {
            GHashTableIter it; gpointer k, v;
            g_hash_table_iter_init(&it, tl->move_pre);
            while (g_hash_table_iter_next(&it, &k, &v)) {
                JackDawTrack *t = k;
                track_sort_sections(t);
                track_commit_sections(tl, t);
            }
            timeline_push_move_undo(tl);          /* reads pristine from move_pre */
            g_hash_table_destroy(tl->move_pre);
            tl->move_pre = NULL;
        } else if (tl->sel_track) {
            track_sort_sections(tl->sel_track);
            track_commit_sections(tl, tl->sel_track);
        }
        tl->move_src = NULL;
        g_clear_pointer(&tl->move_orig, g_free);
        jackdaw_timeline_redraw_all(tl);
        return TRUE;
    }

    /* Armed but never dragged → it was a plain click on a selected section:
     * seek the playhead there and keep the selection. */
    if (tl->move_armed) {
        tl->move_armed = FALSE;
        g_clear_pointer(&tl->move_orig, g_free);
        off_t sample = timeline_x_to_sample(tl, event->x);
        timeline_set_playhead(tl, sample);
        g_signal_emit(tl, timeline_signals[SIGNAL_POSITION_CHANGED], 0,
                      (gint64)sample);
        return TRUE;
    }

    /* Only a rubber-band drag (started in the press handler) selects on release;
     * a Ctrl+click multi-select must not be collapsed here. */
    gboolean was_selecting = tl->selecting;
    tl->selecting = FALSE;
    if (was_selecting && !tl->sel_active) {
        JackDawWaveView *wv = JACKDAW_WAVE_VIEW(widget);
        timeline_select_region_at(tl, wv->track, tl->sel_start);
    }
    return FALSE;
}

/* Mouse-wheel on a WaveView: zoom (Ctrl = pan) */
static gboolean timeline_wave_scroll(GtkWidget *widget,
                                      GdkEventScroll *event, gpointer data)
{
    JackDawTimeline *tl = JACKDAW_TIMELINE(data);
    (void)widget;

    if (event->state & GDK_CONTROL_MASK) {
        /* Ctrl+scroll: pan left/right */
        gdouble spp  = gtk_adjustment_get_value(tl->zoom_adj);
        gdouble step = spp * 80.0;  /* 80 pixels worth of samples */
        gdouble val  = gtk_adjustment_get_value(tl->time_adj);
        if (event->direction == GDK_SCROLL_UP   ||
            event->direction == GDK_SCROLL_LEFT ||
            (event->direction == GDK_SCROLL_SMOOTH && event->delta_y < 0))
            val -= step;
        else
            val += step;
        if (val < 0.0) val = 0.0;
        gtk_adjustment_set_value(tl->time_adj, val);
    } else {
        /* Plain scroll: zoom in/out */
        if (event->direction == GDK_SCROLL_UP ||
            (event->direction == GDK_SCROLL_SMOOTH && event->delta_y < 0))
            jackdaw_timeline_zoom_in(tl);
        else if (event->direction == GDK_SCROLL_DOWN ||
                 (event->direction == GDK_SCROLL_SMOOTH && event->delta_y > 0))
            jackdaw_timeline_zoom_out(tl);
    }
    return TRUE;
}

/* 50 ms timer: update playhead and auto-scroll to follow it */
static void master_vu_tick(JackDawTimeline *tl);   /* defined with master row */

static GtkWidget *timeline_lane_ref(JackDawTimeline *tl);

static gboolean timeline_update_timer(gpointer data)
{
    JackDawTimeline *tl = data;
    if (!JACKDAW_IS_TIMELINE(tl)) return G_SOURCE_REMOVE;

    master_vu_tick(tl);   /* refresh the master header meter (decays when idle) */

    /* Keep the horizontal scrollbar's range in sync with content + view.
     *
     * Measure a lane, not the ruler. The two are inset to the same width now,
     * but the visible time span is a property of the lanes; deriving the page
     * size from the ruler is what let the scrollbar range and the drawn
     * waveforms disagree whenever the two widths drifted apart. Fall back to
     * the ruler when there are no tracks to measure. */
    {
        GtkAllocation ra;
        gtk_widget_get_allocation(timeline_lane_ref(tl), &ra);
        if (ra.width > 0) {
            gdouble spp  = gtk_adjustment_get_value(tl->zoom_adj);
            gdouble page = (gdouble)ra.width * spp;
            off_t   maxf = 0;
            if (tl->project) {
                guint n = jackdaw_project_track_count(tl->project);
                for (guint i = 0; i < n; i++) {
                    off_t tf = jackdaw_track_total_frames(
                        jackdaw_project_get_track(tl->project, i));
                    if (tf > maxf) maxf = tf;
                }
            }
            /* While rolling (play OR record), grow the range with the playhead so
             * recording past the end of existing content can still auto-scroll —
             * otherwise set_value below clamps to the content length. */
            if (jackdaw_engine_is_playing()) {
                off_t pp = jackdaw_engine_get_play_pos();
                if (pp > maxf) maxf = pp;
            }
            gdouble start = gtk_adjustment_get_value(tl->time_adj);
            gdouble upper = (gdouble)maxf + page;
            if (upper < start + page) upper = start + page;
            gtk_adjustment_set_page_size(tl->time_adj, page);
            gtk_adjustment_set_upper(tl->time_adj, upper);
        }
    }

    if (!jackdaw_engine_is_running()) return G_SOURCE_CONTINUE;

    /* Only drive the playhead from the transport while actually playing; when
     * stopped, the cursor stays where the user clicked it. */
    if (!jackdaw_engine_is_playing()) return G_SOURCE_CONTINUE;

    off_t pos = jackdaw_engine_get_play_pos();
    gtk_adjustment_set_value(tl->cursor_adj, (gdouble)pos);

    /* Auto-scroll: if playhead moved and is past the right edge, jump so
     * the playhead lands at ~20% from the left — gives lookahead room. */
    if (pos != tl->prev_play_pos) {
        tl->prev_play_pos = pos;

        GtkAllocation alloc;
        gtk_widget_get_allocation(GTK_WIDGET(tl->ruler), &alloc);
        gint view_w = alloc.width;
        if (view_w > 0) {
            gdouble spp   = gtk_adjustment_get_value(tl->zoom_adj);
            gdouble start = gtk_adjustment_get_value(tl->time_adj);
            gdouble end   = start + (gdouble)view_w * spp;
            gdouble pos_d = (gdouble)pos;
            if (pos_d > end) {
                gdouble new_start = pos_d - 0.10 * (gdouble)view_w * spp;
                if (new_start < 0.0) new_start = 0.0;
                gtk_adjustment_set_value(tl->time_adj, new_start);
            }
        }
    }
    return G_SOURCE_CONTINUE;
}

/* Project signal handlers */
static void on_project_track_added(JackDawProject *p, JackDawTrack *t,
                                    gpointer data)
{
    (void)p;
    jackdaw_timeline_add_track(JACKDAW_TIMELINE(data), t);
}

static void on_project_track_removed(JackDawProject *p, JackDawTrack *t,
                                      gpointer data)
{
    (void)p;
    jackdaw_timeline_remove_track(JACKDAW_TIMELINE(data), t);
}

static void timeline_sync_track_order(JackDawTimeline *tl);  /* fwd decl */

/* Project track order changed (e.g. an undo/redo of add/delete/reorder): resync
 * the widget rows to match the array order. */
static void on_project_tracks_reordered(JackDawProject *p, gpointer data)
{
    (void)p;
    timeline_sync_track_order(JACKDAW_TIMELINE(data));
}

static void on_project_timing_changed(JackDawProject *p, gpointer data)
{
    (void)p;
    jackdaw_timeline_redraw_all(JACKDAW_TIMELINE(data));
}

/* ---- Ruler drag-playhead ---- */

static gboolean ruler_edge_scroll(gpointer data)
{
    JackDawTimeline *tl = JACKDAW_TIMELINE(data);
    if (!tl->ruler_drag_active) {
        tl->ruler_drag_scroll = 0;
        return G_SOURCE_REMOVE;
    }

    GtkAllocation alloc;
    gtk_widget_get_allocation(GTK_WIDGET(tl->ruler), &alloc);
    gdouble w   = (gdouble)alloc.width;
    gdouble x   = tl->ruler_drag_last_x;
    gdouble spp = gtk_adjustment_get_value(tl->zoom_adj);

    gdouble overshoot;
    if (x < 0.0)    overshoot = x;        /* negative: past left edge */
    else if (x > w) overshoot = x - w;    /* positive: past right edge */
    else {
        tl->ruler_drag_scroll = 0;
        return G_SOURCE_REMOVE;
    }

    /* Scroll proportional to overshoot (pixels per 40 ms tick). */
    gdouble scroll_px = CLAMP(overshoot * 0.4, -400.0, 400.0);
    gdouble new_start = gtk_adjustment_get_value(tl->time_adj) + scroll_px * spp;
    if (new_start < 0.0) new_start = 0.0;
    gtk_adjustment_set_value(tl->time_adj, new_start);

    /* Keep playhead pinned to the edge being crossed. */
    gdouble edge_x = (x < 0.0) ? 0.0 : w;
    off_t frame = (off_t)(gtk_adjustment_get_value(tl->time_adj) + edge_x * spp);
    if (frame < 0) frame = 0;
    timeline_set_playhead(tl, frame);

    return G_SOURCE_CONTINUE;
}

/* Apply a loop-tab drag to the timeline x-coordinate, clamping so the dragged
 * edge cannot cross the other one. */
static void ruler_loop_drag_to(JackDawTimeline *tl, gdouble x)
{
    off_t frame = timeline_x_to_sample(tl, x);   /* snapped, clamped >= 0 */
    off_t ls, le;
    jackdaw_engine_get_loop_range(&ls, &le);
    if (tl->loop_drag_edge == 1) {            /* dragging start tab */
        if (frame > le) frame = le;
        jackdaw_engine_set_loop_range(frame, le);
    } else {                                  /* dragging end tab */
        if (frame < ls) frame = ls;
        jackdaw_engine_set_loop_range(ls, frame);
    }
    gtk_widget_queue_draw(GTK_WIDGET(tl->ruler));
    jackdaw_timeline_redraw_all(tl);
}

/* Hit-test the ruler x against the loop tabs. Returns 1 (start), 2 (end), or 0.
 * When no region exists both tabs sit at frame 0; a hit grabs the end tab so
 * the user drags right to create the region. */
static int ruler_loop_hit(JackDawTimeline *tl, gdouble x)
{
    gdouble spp   = gtk_adjustment_get_value(tl->zoom_adj);
    gdouble start = gtk_adjustment_get_value(tl->time_adj);
    if (spp <= 0.0) return 0;
    off_t ls, le;
    jackdaw_engine_get_loop_range(&ls, &le);
    double x0 = ((double)ls - start) / spp;
    double x1 = ((double)le - start) / spp;
    if (!jackdaw_engine_has_loop_region())
        return (fabs(x - x0) <= 6.0) ? 2 : 0;
    if (fabs(x - x0) <= 6.0) return 1;
    if (fabs(x - x1) <= 6.0) return 2;
    return 0;
}

static gboolean ruler_button_press_cb(GtkWidget *widget, GdkEventButton *ev,
                                       gpointer data)
{
    (void)widget;
    if (ev->button != 1) return FALSE;
    JackDawTimeline *tl = JACKDAW_TIMELINE(data);

    /* Grab a loop tab if the click landed on one — takes priority over scrub. */
    int edge = ruler_loop_hit(tl, ev->x);
    if (edge) {
        tl->loop_drag_edge = edge;
        ruler_loop_drag_to(tl, ev->x);
        return TRUE;
    }

    gdouble spp   = gtk_adjustment_get_value(tl->zoom_adj);
    gdouble start = gtk_adjustment_get_value(tl->time_adj);
    off_t frame   = (off_t)(start + ev->x * spp);
    if (frame < 0) frame = 0;

    tl->ruler_drag_active = TRUE;
    tl->ruler_drag_last_x = ev->x;
    timeline_set_playhead(tl, frame);
    return TRUE;
}

static gboolean ruler_motion_cb(GtkWidget *widget, GdkEventMotion *ev,
                                  gpointer data)
{
    JackDawTimeline *tl = JACKDAW_TIMELINE(data);

    if (tl->loop_drag_edge) {
        ruler_loop_drag_to(tl, ev->x);
        return TRUE;
    }

    if (!tl->ruler_drag_active) return FALSE;

    tl->ruler_drag_last_x = ev->x;

    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);
    gdouble w     = (gdouble)alloc.width;
    gdouble spp   = gtk_adjustment_get_value(tl->zoom_adj);
    gdouble start = gtk_adjustment_get_value(tl->time_adj);

    if (ev->x >= 0.0 && ev->x <= w) {
        /* Inside ruler: move playhead directly. */
        off_t frame = (off_t)(start + ev->x * spp);
        if (frame < 0) frame = 0;
        timeline_set_playhead(tl, frame);

        if (tl->ruler_drag_scroll) {
            g_source_remove(tl->ruler_drag_scroll);
            tl->ruler_drag_scroll = 0;
        }
    } else if (!tl->ruler_drag_scroll) {
        /* Past an edge: start auto-scroll timer. */
        tl->ruler_drag_scroll = g_timeout_add(40, ruler_edge_scroll, tl);
    }
    return TRUE;
}

static gboolean ruler_button_release_cb(GtkWidget *widget, GdkEventButton *ev,
                                          gpointer data)
{
    (void)widget;
    if (ev->button != 1) return FALSE;
    JackDawTimeline *tl = JACKDAW_TIMELINE(data);
    tl->loop_drag_edge = 0;
    tl->ruler_drag_active = FALSE;
    if (tl->ruler_drag_scroll) {
        g_source_remove(tl->ruler_drag_scroll);
        tl->ruler_drag_scroll = 0;
    }
    return TRUE;
}

static void jackdaw_timeline_finalize(GObject *obj)
{
    JackDawTimeline *tl = JACKDAW_TIMELINE(obj);

    if (tl->update_timer) {
        g_source_remove(tl->update_timer);
        tl->update_timer = 0;
    }
    if (tl->ruler_drag_scroll) {
        g_source_remove(tl->ruler_drag_scroll);
        tl->ruler_drag_scroll = 0;
    }
    g_object_unref(tl->time_adj);
    g_object_unref(tl->zoom_adj);
    g_object_unref(tl->cursor_adj);
    if (tl->header_size_group) {
        g_object_unref(tl->header_size_group);
        tl->header_size_group = NULL;
    }
    g_hash_table_destroy(tl->wave_views);
    if (tl->move_pre) g_hash_table_destroy(tl->move_pre);
    if (tl->sel_regions) g_ptr_array_unref(tl->sel_regions);
    if (tl->clipboard) g_ptr_array_unref(tl->clipboard);
    if (tl->midi_clipboard) g_ptr_array_unref(tl->midi_clipboard);
    g_free(tl->move_orig);

    G_OBJECT_CLASS(jackdaw_timeline_parent_class)->finalize(obj);
}

static void jackdaw_timeline_class_init(JackDawTimelineClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = jackdaw_timeline_finalize;

    timeline_signals[SIGNAL_TRACK_FOCUSED] = g_signal_new(
        "track-focused", G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_FIRST,
        G_STRUCT_OFFSET(JackDawTimelineClass, track_focused),
        NULL, NULL, NULL, G_TYPE_NONE, 1, JACKDAW_TYPE_TRACK);

    timeline_signals[SIGNAL_POSITION_CHANGED] = g_signal_new(
        "position-changed", G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_FIRST,
        G_STRUCT_OFFSET(JackDawTimelineClass, position_changed),
        NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT64);
}

static void jackdaw_timeline_init(JackDawTimeline *tl)
{
    tl->project            = NULL;
    tl->time_adj           = NULL;
    tl->zoom_adj           = NULL;
    tl->cursor_adj         = NULL;
    tl->ruler              = NULL;
    tl->tracks_scroll      = NULL;
    tl->tracks_bg          = NULL;
    tl->tracks_box         = NULL;
    tl->focused_track      = NULL;
    tl->header_size_group  = NULL;
    tl->wave_views         = g_hash_table_new(g_direct_hash, g_direct_equal);
    tl->update_timer  = 0;
    tl->prev_play_pos = 0;
    tl->ruler_drag_active = FALSE;
    tl->ruler_drag_last_x = 0.0;
    tl->ruler_drag_scroll = 0;
    tl->master_row    = NULL;
    tl->master_mute   = NULL;
    tl->master_vu     = NULL;
    tl->master_vu_L   = 0.0f;
    tl->master_vu_R   = 0.0f;
    tl->master_suppress = FALSE;
    tl->master_sig_connected = FALSE;
    tl->sel_active    = FALSE;
    tl->selecting     = FALSE;
    tl->sel_start     = 0;
    tl->sel_end       = 0;
    tl->sel_track     = NULL;
    tl->sel_regions   = g_ptr_array_new();
    tl->move_armed    = FALSE;
    tl->moving        = FALSE;
    tl->move_committed = FALSE;
    tl->move_press_x  = 0.0;
    tl->move_press_y_root = 0.0;
    tl->move_orig     = NULL;
    tl->move_src      = NULL;
    tl->move_pre      = NULL;
    tl->menu_track    = NULL;
    tl->menu_frame    = 0;
    tl->clipboard     = g_ptr_array_new_with_free_func(
                            (GDestroyNotify)clip_region_free);
    tl->midi_clipboard = g_ptr_array_new_with_free_func(
                            (GDestroyNotify)midi_region_free);
    tl->hscroll       = NULL;

    gtk_orientable_set_orientation(GTK_ORIENTABLE(tl),
                                   GTK_ORIENTATION_VERTICAL);
    gtk_box_set_spacing(GTK_BOX(tl), 0);
}

static gboolean tracks_box_draw_bg   (GtkWidget *w, cairo_t *cr, gpointer data);
static gboolean tracks_box_draw_after(GtkWidget *w, cairo_t *cr, gpointer data);
static gboolean tracks_bg_button_press(GtkWidget *w, GdkEventButton *ev,
                                       gpointer data);

/* Width the vertical scrollbar takes out of the track area. The ruler and the
 * horizontal scrollbar are inset by the same amount so that the moment the
 * track list overflows and the scrollbar appears, the lanes do not shrink out
 * from under the ruler. Overlay scrolling is disabled and the policy is ALWAYS
 * so this is a constant, not a function of how many tracks exist. */
static gint timeline_vscrollbar_width(GtkWidget *scrolled)
{
    GtkWidget *vsb = gtk_scrolled_window_get_vscrollbar(
        GTK_SCROLLED_WINDOW(scrolled));
    gint min = 0, nat = 0;
    if (vsb) gtk_widget_get_preferred_width(vsb, &min, &nat);
    return (nat > 0) ? nat : (min > 0 ? min : 13);
}

GtkWidget *jackdaw_timeline_new(JackDawProject *project)
{
    g_return_val_if_fail(JACKDAW_IS_PROJECT(project), NULL);

    JackDawTimeline *tl = g_object_new(JACKDAW_TYPE_TIMELINE, NULL);
    tl->project = project;

    /* ref_sink, not a bare assignment: gtk_adjustment_new() returns a FLOATING
     * reference, and gtk_scrollbar_new() below sinks it and takes ownership of
     * time_adj. The timeline's own finalize then unref'd an adjustment it did
     * not hold a reference to — once the scrollbar and the last wave view were
     * destroyed the object was already gone, and shutdown ended in
     * "g_object_unref: assertion 'G_IS_OBJECT (object)' failed". Sinking here
     * gives the timeline a real reference to match that unref, whatever the
     * children do with theirs. */
    tl->time_adj   = g_object_ref_sink(gtk_adjustment_new(
                         0.0, 0.0, (gdouble)G_MAXINT64, 1024.0, 4096.0, 0.0));
    tl->zoom_adj   = g_object_ref_sink(gtk_adjustment_new(
                         1000.0, 1.0, 2000000.0, 100.0, 1000.0, 0.0));
    tl->cursor_adj = g_object_ref_sink(gtk_adjustment_new(
                         0.0, 0.0, (gdouble)G_MAXINT64, 1.0, 1.0, 0.0));

    /* Size group: keeps the ruler spacer and every track strip at the same
     * width. Every member is independently pinned to TIMELINE_HEADER_WIDTH
     * (the strip via its get_preferred_width* overrides, the spacers via a size
     * request) — the group only guarantees they stay in step, it must never be
     * the thing that decides the width, or one over-wide strip would drag the
     * whole column and the ruler with it. */
    tl->header_size_group = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);

    /* ---- Ruler row ---- */
    GtkWidget *ruler_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(spacer, TIMELINE_HEADER_WIDTH, -1);
    gtk_widget_set_hexpand(spacer, FALSE);
    gtk_size_group_add_widget(tl->header_size_group, spacer);

    guint32 sr = jackdaw_engine_is_running()
                 ? (guint32)jackdaw_engine_get_sample_rate()
                 : 48000u;

    tl->ruler = JACKDAW_TIME_RULER(
        jackdaw_time_ruler_new(tl->time_adj, tl->zoom_adj, tl->cursor_adj, sr));
    tl->ruler->project = project;

    gtk_widget_add_events(GTK_WIDGET(tl->ruler),
        GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
        GDK_BUTTON1_MOTION_MASK);
    g_signal_connect(GTK_WIDGET(tl->ruler), "button-press-event",
        G_CALLBACK(ruler_button_press_cb), tl);
    g_signal_connect(GTK_WIDGET(tl->ruler), "motion-notify-event",
        G_CALLBACK(ruler_motion_cb), tl);
    g_signal_connect(GTK_WIDGET(tl->ruler), "button-release-event",
        G_CALLBACK(ruler_button_release_cb), tl);

    /* ---- Scrolled window for track rows ---- */
    tl->tracks_scroll = gtk_scrolled_window_new(NULL, NULL);
    /* ALWAYS + no overlay scrolling: the gutter is a constant the ruler and the
     * horizontal scrollbar can be inset by. With AUTOMATIC the bar appears the
     * moment the track list overflows and silently narrows every lane (the
     * ruler, being outside this scrolled window, does not narrow) — the lanes
     * visibly resized and drifted out of register with the ruler by the
     * scrollbar's width as soon as one track too many was added. */
    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(tl->tracks_scroll),
        GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
    gtk_scrolled_window_set_overlay_scrolling(
        GTK_SCROLLED_WINDOW(tl->tracks_scroll), FALSE);

    gint gutter = timeline_vscrollbar_width(tl->tracks_scroll);

    GtkWidget *ruler_gutter = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(ruler_gutter, gutter, -1);
    gtk_widget_set_hexpand(ruler_gutter, FALSE);

    gtk_box_pack_start(GTK_BOX(ruler_row), spacer,         FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ruler_row), GTK_WIDGET(tl->ruler), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(ruler_row), ruler_gutter,   FALSE, FALSE, 0);

    /* Spacing 0: a 1px gap between rows was a full-width band of bare theme
     * colour that cut straight through the header column and its divider. */
    tl->tracks_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(tl->tracks_box, TRUE);
    /* Background under the rows, divider + reorder insertion line over them. */
    g_signal_connect(tl->tracks_box, "draw",
                     G_CALLBACK(tracks_box_draw_bg), tl);
    g_signal_connect_after(tl->tracks_box, "draw",
                           G_CALLBACK(tracks_box_draw_after), tl);

    /* Input-only wrapper: picks up clicks that land in the header column or the
     * lane area below the last track, where there is no strip or wave view to
     * receive them. Invisible, so it cannot paint over the background above. */
    tl->tracks_bg = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(tl->tracks_bg), FALSE);
    gtk_widget_add_events(tl->tracks_bg, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(tl->tracks_bg, "button-press-event",
                     G_CALLBACK(tracks_bg_button_press), tl);
    gtk_container_add(GTK_CONTAINER(tl->tracks_bg), tl->tracks_box);
    gtk_container_add(GTK_CONTAINER(tl->tracks_scroll), tl->tracks_bg);

    gtk_box_pack_start(GTK_BOX(tl), ruler_row,         FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tl), tl->tracks_scroll, TRUE,  TRUE,  0);

    /* Horizontal scrollbar bound to the shared time adjustment. Its bounds
     * are kept in sync with the project length by the update timer. Inset to
     * span exactly the lane area, so the thumb position is honest about which
     * part of the timeline is on screen. */
    GtkWidget *hscroll_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *hs_spacer   = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(hs_spacer, TIMELINE_HEADER_WIDTH, -1);
    gtk_widget_set_hexpand(hs_spacer, FALSE);
    gtk_size_group_add_widget(tl->header_size_group, hs_spacer);
    GtkWidget *hs_gutter = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(hs_gutter, gutter, -1);
    gtk_widget_set_hexpand(hs_gutter, FALSE);

    tl->hscroll = gtk_scrollbar_new(GTK_ORIENTATION_HORIZONTAL, tl->time_adj);
    gtk_box_pack_start(GTK_BOX(hscroll_row), hs_spacer,   FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hscroll_row), tl->hscroll, TRUE,  TRUE,  0);
    gtk_box_pack_start(GTK_BOX(hscroll_row), hs_gutter,   FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tl), hscroll_row, FALSE, FALSE, 0);

    gtk_widget_show_all(GTK_WIDGET(tl));

    g_signal_connect_object(project, "track-added",
                            G_CALLBACK(on_project_track_added), tl, 0);
    g_signal_connect_object(project, "track-removed",
                            G_CALLBACK(on_project_track_removed), tl, 0);
    g_signal_connect_object(project, "timing-changed",
                            G_CALLBACK(on_project_timing_changed), tl, 0);
    g_signal_connect_object(project, "selection-changed",
                            G_CALLBACK(timeline_selection_changed), tl, 0);
    g_signal_connect_object(project, "tracks-reordered",
                            G_CALLBACK(on_project_tracks_reordered), tl, 0);

    tl->update_timer = g_timeout_add(50, timeline_update_timer, tl);

    return GTK_WIDGET(tl);
}

/* ========================================================================
 * Track drag-to-reorder
 *
 * Same mechanism as the FX list: each track strip is wrapped in an event box
 * that acts as a drag source + drop target. Because the strip is crowded, the
 * (click-inert) VU meter is registered as a second grip so there is always an
 * obvious place to grab. The engine slot / JACK ports follow track->slot, not
 * array order, so reordering is purely a display + save-order change.
 * ======================================================================== */

static const GtkTargetEntry TRACK_ROW_DND[] = {
    { (gchar *)"JACKDAW_TRACK_ROW", GTK_TARGET_SAME_APP, 0 }
};

typedef struct {
    JackDawTimeline *tl;
    JackDawTrack    *track;
    GtkWidget       *snap;   /* widget to render as the drag icon (the strip) */
} TrackDnd;

/* Render a realized widget (and its children) into a fresh surface — used as the
 * drag icon so the pointer carries a ghost of the grabbed row. */
static cairo_surface_t *jackdaw_widget_snapshot(GtkWidget *w)
{
    if (!w || !gtk_widget_get_realized(w)) return NULL;
    GtkAllocation a;
    gtk_widget_get_allocation(w, &a);
    if (a.width <= 0 || a.height <= 0) return NULL;

    GdkWindow *win = gtk_widget_get_window(w);
    cairo_surface_t *s = win
        ? gdk_window_create_similar_surface(win, CAIRO_CONTENT_COLOR_ALPHA,
                                            a.width, a.height)
        : cairo_image_surface_create(CAIRO_FORMAT_ARGB32, a.width, a.height);
    cairo_t *cr = cairo_create(s);
    gtk_widget_draw(w, cr);
    /* Dim it a touch so it reads as a "ghost" being moved. DEST_IN scales the
     * snapshot's alpha by the source alpha (× the paint alpha), so the source
     * must stay opaque — only the 0.75 paint alpha should ghost it. */
    cairo_set_source_rgba(cr, 0, 0, 0, 1.0);
    cairo_set_operator(cr, CAIRO_OPERATOR_DEST_IN);
    cairo_paint_with_alpha(cr, 0.75);
    cairo_destroy(cr);
    return s;
}

/* Outer box (direct child of tracks_box) for a given track, or NULL. */
static GtkWidget *track_outer_for(JackDawTimeline *tl, JackDawTrack *t)
{
    GtkWidget *wv = g_hash_table_lookup(tl->wave_views, t);
    if (!wv) return NULL;
    GtkWidget *row = gtk_widget_get_parent(wv);
    return row ? gtk_widget_get_parent(row) : NULL;
}

/* Is the pointer (in grip-widget coords) over the top half of the track row?
 * Decides whether a drop lands above or below the hovered track. */
static gboolean track_drop_above(JackDawTimeline *tl, JackDawTrack *t,
                                 GtkWidget *grip, gint x, gint y)
{
    GtkWidget *outer = track_outer_for(tl, t);
    if (!outer) return TRUE;
    gint ox, oy;
    gtk_widget_translate_coordinates(grip, outer, x, y, &ox, &oy);
    GtkAllocation a;
    gtk_widget_get_allocation(outer, &a);
    return oy < a.height / 2;
}

/* Re-position every track's outer box to match the project's track order. The
 * master row, when shown, stays pinned at child index 0. */
static void timeline_sync_track_order(JackDawTimeline *tl)
{
    guint base = tl->master_row ? 1 : 0;
    guint n    = jackdaw_project_track_count(tl->project);
    for (guint i = 0; i < n; i++) {
        JackDawTrack *t     = jackdaw_project_get_track(tl->project, i);
        GtkWidget    *outer = track_outer_for(tl, t);
        if (outer)
            gtk_box_reorder_child(GTK_BOX(tl->tracks_box), outer,
                                  (gint)(base + i));
    }
}

static void track_drag_begin(GtkWidget *w, GdkDragContext *ctx, gpointer data)
{
    TrackDnd *td = data;
    cairo_surface_t *s = jackdaw_widget_snapshot(td->snap ? td->snap : w);
    if (s) { gtk_drag_set_icon_surface(ctx, s); cairo_surface_destroy(s); }
}

static gboolean track_drag_motion(GtkWidget *w, GdkDragContext *ctx,
                                  gint x, gint y, guint time, gpointer data)
{
    TrackDnd  *td    = data;
    GtkWidget *outer = track_outer_for(td->tl, td->track);
    if (outer) {
        gboolean above = track_drop_above(td->tl, td->track, w, x, y);
        GtkAllocation a;
        gtk_widget_get_allocation(outer, &a);
        gint tx, ty;
        gtk_widget_translate_coordinates(outer, td->tl->tracks_box,
                                         0, above ? 0 : a.height, &tx, &ty);
        td->tl->drop_y      = ty;
        td->tl->drop_active = TRUE;
        gtk_widget_queue_draw(td->tl->tracks_box);
    }
    gdk_drag_status(ctx, GDK_ACTION_MOVE, time);
    return TRUE;
}

static void track_drag_leave(GtkWidget *w, GdkDragContext *ctx,
                             guint time, gpointer data)
{
    (void)w; (void)ctx; (void)time;
    TrackDnd *td = data;
    if (td->tl->drop_active) {
        td->tl->drop_active = FALSE;
        gtk_widget_queue_draw(td->tl->tracks_box);
    }
}

static void track_drag_data_get(GtkWidget *w, GdkDragContext *ctx,
                                GtkSelectionData *sel, guint info,
                                guint time, gpointer data)
{
    (void)w; (void)ctx; (void)info; (void)time;
    TrackDnd *td  = data;
    gint      idx = jackdaw_project_track_index(td->tl->project, td->track);
    gtk_selection_data_set(sel, gtk_selection_data_get_target(sel),
                           8, (const guchar *)&idx, sizeof idx);
}

static void track_drag_data_received(GtkWidget *w, GdkDragContext *ctx,
                                     gint x, gint y, GtkSelectionData *sel,
                                     guint info, guint time, gpointer data)
{
    (void)info;
    TrackDnd *td = data;
    td->tl->drop_active = FALSE;
    gtk_widget_queue_draw(td->tl->tracks_box);

    gboolean ok = (gtk_selection_data_get_length(sel) == (gint)sizeof(gint));
    if (ok) {
        gint from = *(const gint *)gtk_selection_data_get_data(sel);
        gint tgt  = jackdaw_project_track_index(td->tl->project, td->track);
        guint n   = jackdaw_project_track_count(td->tl->project);
        if (from >= 0 && tgt >= 0) {
            /* Honour the insertion line: dropping on the top half lands before
             * the hovered track, the bottom half after it. */
            gboolean above = track_drop_above(td->tl, td->track, w, x, y);
            gint ins   = above ? tgt : tgt + 1;          /* slot in [0, n] */
            gint final = (from < ins) ? ins - 1 : ins;   /* index after move  */
            final = CLAMP(final, 0, (gint)n - 1);
            if (final != from) {
                jackdaw_project_push_structural_undo(td->tl->project,
                                                     "Reorder tracks");
                jackdaw_project_move_track(td->tl->project,
                                           (guint)from, (guint)final);
                timeline_sync_track_order(td->tl);
            }
        }
    }
    gtk_drag_finish(ctx, ok, FALSE, time);
}

/* Frozen-pane background, painted UNDER the track rows for the full height of
 * the track area — not just the span the rows happen to occupy.
 *
 * Track strips are per-row cells, so before this existed there was no header
 * *column* at all: with zero tracks, or in the space below the last track, the
 * bare window background showed through and the timeline read as one
 * undifferentiated surface with no boundary between the strips and the lanes.
 * Filling both bands here makes the divider (stroked on top in
 * tracks_box_draw_after) present regardless of track count. */
static gboolean tracks_box_draw_bg(GtkWidget *w, cairo_t *cr, gpointer data)
{
    (void)data;
    GtkAllocation a;
    gtk_widget_get_allocation(w, &a);

    cairo_set_source_rgb(cr, TL_HEADER_BG);
    cairo_rectangle(cr, 0, 0, TIMELINE_HEADER_WIDTH, a.height);
    cairo_fill(cr);

    cairo_set_source_rgb(cr, TL_LANE_BG);
    cairo_rectangle(cr, TIMELINE_HEADER_WIDTH, 0,
                    a.width - TIMELINE_HEADER_WIDTH, a.height);
    cairo_fill(cr);

    return FALSE;   /* let the rows draw on top */
}

/* Column divider (always) + the reorder insertion line (during a drag), both
 * drawn after the rows so neither can be painted over by a strip background. */
static gboolean tracks_box_draw_after(GtkWidget *w, cairo_t *cr, gpointer data)
{
    JackDawTimeline *tl = data;
    GtkAllocation a;
    gtk_widget_get_allocation(w, &a);

    cairo_set_source_rgb(cr, TL_DIVIDER);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, TIMELINE_HEADER_WIDTH - 0.5, 0);
    cairo_line_to(cr, TIMELINE_HEADER_WIDTH - 0.5, a.height);
    cairo_stroke(cr);

    if (!tl->drop_active) return FALSE;
    double yy = tl->drop_y + 0.5;
    cairo_set_source_rgb(cr, 0.20, 0.55, 1.0);     /* accent blue */
    cairo_set_line_width(cr, 2.0);
    cairo_move_to(cr, 0,       yy);
    cairo_line_to(cr, a.width, yy);
    cairo_stroke(cr);
    /* End caps so the line reads as an insertion marker. */
    cairo_arc(cr, 3,           yy, 3, 0, 2 * G_PI);
    cairo_arc(cr, a.width - 3, yy, 3, 0, 2 * G_PI);
    cairo_fill(cr);
    return FALSE;
}

/* A widget whose width equals the visible lane span: any wave view, else the
 * ruler (which is inset to the same width) when the project has no tracks. */
static GtkWidget *timeline_lane_ref(JackDawTimeline *tl)
{
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, tl->wave_views);
    if (g_hash_table_iter_next(&it, &k, &v))
        return GTK_WIDGET(v);
    return GTK_WIDGET(tl->ruler);
}

/* Clicks in the empty part of the track area. Strips, wave views and resize
 * handles all own their own GdkWindows, so anything that reaches the input-only
 * wrapper landed on genuinely empty space — the header column, or the lane area
 * below the last track.
 *
 * Left-click drops the current selection, matching a click on empty canvas
 * elsewhere. Everything else is left unhandled so it propagates up to the
 * viewport, where the main window already provides the Add Track / Add MIDI
 * Track / Show Master Track menu on button 3. */
static gboolean tracks_bg_button_press(GtkWidget *w, GdkEventButton *ev,
                                       gpointer data)
{
    (void)w;
    JackDawTimeline *tl = data;

    if (ev->type != GDK_BUTTON_PRESS || ev->button != 1) return FALSE;

    timeline_clear_section_sel(tl);
    tl->sel_active = FALSE;
    jackdaw_timeline_redraw_all(tl);
    return TRUE;
}

/* Make `w` a drag grip (source + drop target) for the track described by td. */
static void track_dnd_grip(GtkWidget *w, TrackDnd *td)
{
    if (!w) return;
    gtk_drag_source_set(w, GDK_BUTTON1_MASK, TRACK_ROW_DND, 1, GDK_ACTION_MOVE);
    /* No DEFAULT_HIGHLIGHT — we draw our own insertion line instead. */
    gtk_drag_dest_set(w, GTK_DEST_DEFAULT_MOTION | GTK_DEST_DEFAULT_DROP,
                      TRACK_ROW_DND, 1, GDK_ACTION_MOVE);
    g_signal_connect(w, "drag-begin",  G_CALLBACK(track_drag_begin),  td);
    g_signal_connect(w, "drag-motion", G_CALLBACK(track_drag_motion), td);
    g_signal_connect(w, "drag-leave",  G_CALLBACK(track_drag_leave),  td);
    g_signal_connect(w, "drag-data-get",
                     G_CALLBACK(track_drag_data_get), td);
    g_signal_connect(w, "drag-data-received",
                     G_CALLBACK(track_drag_data_received), td);
}

/* ---- Track-strip context menu (right-click the header) ---- */

static void strip_menu_delete_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    JackDawTrackStrip *strip = JACKDAW_TRACK_STRIP(data);
    if (strip->track && strip->project)
        jackdaw_project_delete_track(strip->project, strip->track);
}

static void strip_menu_midi_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    JackDawTrackStrip *strip = JACKDAW_TRACK_STRIP(data);
    if (strip->track && jackdaw_track_is_instrument(strip->track))
        jackdaw_midi_window_open(strip->track, strip->project);
}

/* Right-click anywhere on a track strip header: select that track (so strip and
 * timeline both highlight) and pop a per-track menu acting on THIS track. */
static gboolean strip_button_press(GtkWidget *w, GdkEventButton *ev,
                                   gpointer data)
{
    (void)w;
    if (ev->type != GDK_BUTTON_PRESS || ev->button != 3) return FALSE;
    JackDawTrackStrip *strip = JACKDAW_TRACK_STRIP(data);
    if (!strip->track || !strip->project) return FALSE;

    jackdaw_project_select_single(strip->project, strip->track);

    GtkWidget *menu = gtk_menu_new();
    if (jackdaw_track_is_instrument(strip->track)) {
        GtkWidget *mi = gtk_menu_item_new_with_label("Open MIDI Editor");
        g_signal_connect(mi, "activate", G_CALLBACK(strip_menu_midi_cb), strip);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    }
    GtkWidget *del = gtk_menu_item_new_with_label("Delete Track");
    g_signal_connect(del, "activate", G_CALLBACK(strip_menu_delete_cb), strip);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), del);

    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
    return TRUE;
}

void jackdaw_timeline_add_track(JackDawTimeline *tl, JackDawTrack *track)
{
    g_return_if_fail(JACKDAW_IS_TIMELINE(tl));
    g_return_if_fail(JACKDAW_IS_TRACK(track));

    if (g_hash_table_contains(tl->wave_views, track)) return;

    /* Outer wrapper: vertical box [row][resize handle] */
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(outer, -1,
        TIMELINE_TRACK_HEIGHT + TIMELINE_RESIZE_HANDLE_H);

    /* Track row: [TrackStrip 180px][waveview →] */
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

    /* Track strip header (ARM/M/S, vol, pan, input selectors). Wrapped in an
     * event box so the strip background can be grabbed for drag-to-reorder (a
     * drag source on the strip's child widgets would conflict with their own
     * clicks; the event box's own window catches the gaps/labels). Adding the
     * wrapper to header_size_group keeps it aligned with the ruler spacer. */
    GtkWidget *strip  = jackdaw_track_strip_new(track, tl->project);
    GtkWidget *strip_box = gtk_event_box_new();
    gtk_container_add(GTK_CONTAINER(strip_box), strip);
    /* Pin the wrapper too, and refuse to expand: the size group keeps the
     * column members in step, but each one must fix its own width so no single
     * over-wide strip can widen the column (and the ruler spacer with it). */
    gtk_widget_set_size_request(strip_box, TIMELINE_HEADER_WIDTH, -1);
    gtk_widget_set_hexpand(strip_box, FALSE);
    gtk_size_group_add_widget(tl->header_size_group, strip_box);
    /* Right-click the strip header → per-track context menu (Delete Track). */
    g_signal_connect(strip_box, "button-press-event",
                     G_CALLBACK(strip_button_press), strip);

    TrackDnd *td = g_new0(TrackDnd, 1);
    td->tl = tl; td->track = track; td->snap = strip_box;

    /* WaveView */
    GtkWidget *wv = jackdaw_wave_view_new(track, tl->time_adj, tl->zoom_adj,
                                          tl->cursor_adj);
    JACKDAW_WAVE_VIEW(wv)->project  = tl->project;
    JACKDAW_WAVE_VIEW(wv)->timeline = tl;
    gtk_widget_add_events(wv,
        GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK |
        GDK_BUTTON1_MOTION_MASK);
    g_signal_connect(wv, "button-press-event",
                     G_CALLBACK(timeline_wave_clicked), tl);
    g_signal_connect(wv, "motion-notify-event",
                     G_CALLBACK(timeline_wave_motion), tl);
    g_signal_connect(wv, "button-release-event",
                     G_CALLBACK(timeline_wave_released), tl);
    g_signal_connect(wv, "scroll-event",
                     G_CALLBACK(timeline_wave_scroll), tl);

    gtk_box_pack_start(GTK_BOX(row), strip_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), wv,        TRUE,  TRUE,  0);

    /* Drag-to-reorder grips: the strip background plus the VU meter (a wide,
     * click-inert column that is always present as an obvious grab point). */
    track_dnd_grip(strip_box, td);
    track_dnd_grip(jackdaw_track_strip_get_vu_meter(JACKDAW_TRACK_STRIP(strip)),
                   td);

    /* Resize handle: 5px drawing area the user drags to change track height */
    GtkWidget *handle = gtk_drawing_area_new();
    gtk_widget_set_size_request(handle, -1, TIMELINE_RESIZE_HANDLE_H);
    gtk_widget_set_name(handle, "track-resize-handle");
    gtk_widget_add_events(handle,
        GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK |
        GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
        GDK_POINTER_MOTION_MASK);

    ResizeData *rd   = g_new0(ResizeData, 1);
    rd->outer        = outer;
    rd->strip        = strip;
    rd->wv           = wv;
    g_object_set_data_full(G_OBJECT(handle), "resize-data", rd, g_free);

    /* td is shared by both reorder grips; free it once with the row. */
    g_object_set_data_full(G_OBJECT(outer), "track-dnd", td, g_free);

    g_signal_connect(handle, "draw",
                     G_CALLBACK(resize_handle_draw),    NULL);
    g_signal_connect(handle, "enter-notify-event",
                     G_CALLBACK(resize_handle_enter),   rd);
    g_signal_connect(handle, "leave-notify-event",
                     G_CALLBACK(resize_handle_leave),   rd);
    g_signal_connect(handle, "button-press-event",
                     G_CALLBACK(resize_handle_press),   rd);
    g_signal_connect(handle, "motion-notify-event",
                     G_CALLBACK(resize_handle_motion),  rd);
    g_signal_connect(handle, "button-release-event",
                     G_CALLBACK(resize_handle_release), rd);

    /* Assemble: row fills outer, handle is pinned at bottom */
    gtk_box_pack_start(GTK_BOX(outer), row,    TRUE,  TRUE,  0);
    gtk_box_pack_start(GTK_BOX(outer), handle, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(tl->tracks_box), outer, FALSE, FALSE, 0);
    gtk_widget_show_all(outer);

    g_hash_table_insert(tl->wave_views, track, JACKDAW_WAVE_VIEW(wv));
}

/* ---- Master-bus row (simplified header + display-only lane) ---- */

static void mw_master_mute_toggled(GtkToggleButton *b, gpointer data)
{
    JackDawTimeline *tl = data;
    if (tl->master_suppress) return;
    jackdaw_track_set_muted(jackdaw_project_get_master_track(tl->project),
                            gtk_toggle_button_get_active(b));
}

static void mw_master_fx_clicked(GtkButton *b, gpointer data)
{
    (void)b;
    JackDawTimeline *tl = data;
    jackdaw_fx_window_open(jackdaw_project_get_master_track(tl->project),
                           tl->project);
}

/* Keep the header's Mute button in sync with master changes from the mixer. */
static void mw_master_state_changed(JackDawTrack *t, gpointer data)
{
    JackDawTimeline *tl = data;
    if (!tl->master_row) return;
    tl->master_suppress = TRUE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tl->master_mute),
                                 jackdaw_track_is_muted(t));
    tl->master_suppress = FALSE;
}

/* L/R level meter for the master header — same look as the track-strip VU. */
static gboolean master_vu_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    JackDawTimeline *tl = data;
    GtkAllocation a; gtk_widget_get_allocation(w, &a);
    cairo_set_source_rgb(cr, 0.08, 0.08, 0.08);
    cairo_paint(cr);

    gfloat peaks[2] = { tl->master_vu_L, tl->master_vu_R };
    gint   bar_w    = (a.width - 3) / 2;
    for (int ch = 0; ch < 2; ch++) {
        gint bx = (ch == 0) ? 1 : (2 + bar_w);
        cairo_set_source_rgb(cr, 0.18, 0.18, 0.18);
        cairo_rectangle(cr, bx, 0, bar_w, a.height);
        cairo_fill(cr);
        gfloat pk = peaks[ch];
        if (pk > 0.0001f) {
            float db   = 20.0f * log10f(pk);
            float dbc  = CLAMP(db, -60.0f, 6.0f);
            float frac = (dbc + 60.0f) / 66.0f;
            gint  fh   = (gint)(frac * (float)a.height);
            if (fh > a.height) fh = a.height;
            if (fh > 0) {
                if (db >= 0.0f)        cairo_set_source_rgb(cr, 0.90, 0.15, 0.15);
                else if (db >= -12.0f) cairo_set_source_rgb(cr, 0.85, 0.78, 0.10);
                else                   cairo_set_source_rgb(cr, 0.15, 0.68, 0.20);
                cairo_rectangle(cr, bx, a.height - fh, bar_w, fh);
                cairo_fill(cr);
            }
        }
    }
    return FALSE;
}

/* Called from the 50 ms timeline timer to refresh the master meter. */
static void master_vu_tick(JackDawTimeline *tl)
{
    if (!tl->master_row || !tl->master_vu) return;
    gfloat l = 0.0f, r = 0.0f;
    jackdaw_engine_get_master_peaks(&l, &r);
    tl->master_vu_L = (l > tl->master_vu_L) ? l : tl->master_vu_L * 0.89f;
    tl->master_vu_R = (r > tl->master_vu_R) ? r : tl->master_vu_R * 0.89f;
    gtk_widget_queue_draw(tl->master_vu);
}

void jackdaw_timeline_set_master_visible(JackDawTimeline *tl, gboolean show)
{
    g_return_if_fail(JACKDAW_IS_TIMELINE(tl));
    if (show == (tl->master_row != NULL)) return;

    JackDawTrack *mt = jackdaw_project_get_master_track(tl->project);

    if (!show) {
        if (tl->master_row) {
            g_hash_table_remove(tl->wave_views, mt);
            gtk_widget_destroy(tl->master_row);
            tl->master_row = tl->master_mute = tl->master_vu = NULL;
        }
        return;
    }

    /* Outer wrapper sized like a normal track row. */
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(outer, -1, TIMELINE_TRACK_HEIGHT);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

    /* Simplified header: "Master" label + M + Fx (no Solo/input/arm/mono) and
     * an L/R level meter on the right, like a normal track strip. */
    GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_container_set_border_width(GTK_CONTAINER(hdr), 4);
    /* Pinned like every other column member: its content is measured, so
     * without this the master row alone could widen the whole header column. */
    gtk_widget_set_size_request(hdr, TIMELINE_HEADER_WIDTH, -1);
    gtk_widget_set_hexpand(hdr, FALSE);
    gtk_size_group_add_widget(tl->header_size_group, hdr);

    GtkWidget *lbl = gtk_label_new("Master");
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    tl->master_mute = gtk_toggle_button_new_with_label("M");
    GtkWidget *fx   = gtk_button_new_with_label("Fx");
    gtk_widget_set_size_request(tl->master_mute, 20, 20);
    gtk_widget_set_size_request(fx, 24, 20);
    gtk_style_context_add_class(gtk_widget_get_style_context(tl->master_mute), "ts-mute");
    gtk_style_context_add_class(gtk_widget_get_style_context(fx), "ts-fx");

    tl->master_suppress = TRUE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tl->master_mute),
                                 jackdaw_track_is_muted(mt));
    tl->master_suppress = FALSE;

    g_signal_connect(tl->master_mute, "toggled",
                     G_CALLBACK(mw_master_mute_toggled), tl);
    g_signal_connect(fx, "clicked", G_CALLBACK(mw_master_fx_clicked), tl);
    if (!tl->master_sig_connected) {   /* connect exactly once across show/hide */
        g_signal_connect_object(mt, "state-changed",
                                G_CALLBACK(mw_master_state_changed), tl, 0);
        tl->master_sig_connected = TRUE;
    }

    tl->master_vu = gtk_drawing_area_new();
    gtk_widget_set_size_request(tl->master_vu, 20, -1);
    g_signal_connect(tl->master_vu, "draw", G_CALLBACK(master_vu_draw), tl);

    gtk_box_pack_start(GTK_BOX(hdr), lbl, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(hdr), tl->master_mute, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hdr), fx, FALSE, FALSE, 0);
    gtk_box_pack_end  (GTK_BOX(hdr), tl->master_vu, FALSE, FALSE, 0);

    /* Display-only lane: a normal wave view (grid + playhead; no regions). */
    GtkWidget *wv = jackdaw_wave_view_new(mt, tl->time_adj, tl->zoom_adj,
                                          tl->cursor_adj);
    JACKDAW_WAVE_VIEW(wv)->project  = tl->project;
    JACKDAW_WAVE_VIEW(wv)->timeline = tl;

    gtk_box_pack_start(GTK_BOX(row), hdr, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), wv,  TRUE,  TRUE,  0);
    gtk_box_pack_start(GTK_BOX(outer), row, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(tl->tracks_box), outer, FALSE, FALSE, 0);
    gtk_box_reorder_child(GTK_BOX(tl->tracks_box), outer, 0);  /* pin to top */
    gtk_widget_show_all(outer);

    tl->master_row = outer;
    g_hash_table_insert(tl->wave_views, mt, JACKDAW_WAVE_VIEW(wv));
}

void jackdaw_timeline_remove_track(JackDawTimeline *tl, JackDawTrack *track)
{
    g_return_if_fail(JACKDAW_IS_TIMELINE(tl));
    g_return_if_fail(JACKDAW_IS_TRACK(track));

    GtkWidget *wv_widget =
        GTK_WIDGET(g_hash_table_lookup(tl->wave_views, track));
    if (!wv_widget) return;

    g_hash_table_remove(tl->wave_views, track);
    if (tl->focused_track == track)
        tl->focused_track = NULL;
    if (tl->sel_track == track)
        timeline_clear_section_sel(tl);

    /* hierarchy: wv -> row -> outer; destroy outer tears down everything */
    GtkWidget *row   = gtk_widget_get_parent(wv_widget);
    GtkWidget *outer = row ? gtk_widget_get_parent(row) : NULL;
    gtk_widget_destroy(outer ? outer : (row ? row : wv_widget));
}

JackDawTrack *jackdaw_timeline_get_focused(JackDawTimeline *tl)
{
    g_return_val_if_fail(JACKDAW_IS_TIMELINE(tl), NULL);
    return tl->focused_track;
}

void jackdaw_timeline_zoom_in(JackDawTimeline *tl)
{
    g_return_if_fail(JACKDAW_IS_TIMELINE(tl));
    gdouble spp = gtk_adjustment_get_value(tl->zoom_adj);
    spp /= 1.5;
    if (spp < gtk_adjustment_get_lower(tl->zoom_adj))
        spp  = gtk_adjustment_get_lower(tl->zoom_adj);
    gtk_adjustment_set_value(tl->zoom_adj, spp);
}

void jackdaw_timeline_zoom_out(JackDawTimeline *tl)
{
    g_return_if_fail(JACKDAW_IS_TIMELINE(tl));
    gdouble spp = gtk_adjustment_get_value(tl->zoom_adj);
    spp *= 1.5;
    if (spp > gtk_adjustment_get_upper(tl->zoom_adj))
        spp  = gtk_adjustment_get_upper(tl->zoom_adj);
    gtk_adjustment_set_value(tl->zoom_adj, spp);
}

void jackdaw_timeline_set_cursor(JackDawTimeline *tl, off_t sample)
{
    g_return_if_fail(JACKDAW_IS_TIMELINE(tl));
    gtk_adjustment_set_value(tl->cursor_adj, (gdouble)sample);
}
