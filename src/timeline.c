#define _GNU_SOURCE
#include <config.h>
#include <math.h>
#include <string.h>

#include "timeline.h"
#include "trackstrip.h"
#include "jackdaw-engine.h"
#include "main.h"

/* ========================================================================
 * JackDawTimeRuler
 * ======================================================================== */

G_DEFINE_TYPE(JackDawTimeRuler, jackdaw_time_ruler, GTK_TYPE_DRAWING_AREA)

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
        return FALSE;
    }

    /* Tick marks from find_timescale_points */
    off_t pts[256], mids[256], mins[512];
    int   npts  = 256,   nmids  = 256,   nmins  = 512;
    guint has_mids = find_timescale_points(r->sample_rate,
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
        get_time(r->sample_rate, pts[i], end_samp, tbuf, default_timescale_mode);
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

/* Draw the beat/bar grid behind the waveform when enabled on the project. */
static void wave_view_draw_grid(JackDawWaveView *wv, cairo_t *cr,
                                int w, int h, gdouble start, gdouble spp)
{
    if (!wv->project || !wv->project->grid_enabled || spp <= 0.0)
        return;
    guint32 sr  = (guint32)jackdaw_engine_get_sample_rate();
    gdouble fpb = jackdaw_project_frames_per_beat(wv->project, sr);
    if (fpb <= 0.0) return;
    guint bpb = wv->project->beats_per_bar ? wv->project->beats_per_bar : 4;

    long b0 = (long)floor(start / fpb);
    if (b0 < 0) b0 = 0;
    cairo_set_line_width(cr, 1.0);
    for (long b = b0; ; b++) {
        double x = ((gdouble)b * fpb - start) / spp;
        if (x > (double)w) break;
        if (x < 0.0) continue;
        if ((b % bpb) == 0)
            cairo_set_source_rgba(cr, 0.50, 0.50, 0.56, 0.50); /* bar */
        else
            cairo_set_source_rgba(cr, 0.30, 0.30, 0.33, 0.40); /* beat */
        cairo_move_to(cr, floor(x) + 0.5, 0);
        cairo_line_to(cr, floor(x) + 0.5, h);
        cairo_stroke(cr);
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

    GPtrArray *regs = wv->track ? jackdaw_track_get_regions(wv->track) : NULL;
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

                    double half  = (double)(band_h / 2);
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

            /* Region boundary line (left edge) — skip the very first region
             * at timeline 0 so the start edge isn't cluttered. */
            if (r_tl0 > 0) {
                double bx = ((gdouble)r_tl0 - start) / spp;
                if (bx >= 0.0 && bx < (double)w) {
                    cairo_set_source_rgba(cr, 0.95, 0.95, 0.55, 0.9);
                    cairo_set_line_width(cr, 1.0);
                    cairo_move_to(cr, bx + 0.5, 0);
                    cairo_line_to(cr, bx + 0.5, h);
                    cairo_stroke(cr);
                }
            }

            g_free(out_min);
            g_free(out_max);
        }
    }

    /* Selection overlay (shared across all tracks) */
    if (wv->timeline && wv->timeline->sel_active && spp > 0.0) {
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
            /* Red background */
            cairo_set_source_rgba(cr, 0.80, 0.08, 0.08, 0.25);
            cairo_rectangle(cr, x0, 0, x1 - x0, h);
            cairo_fill(cr);

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

                double half = h * 0.5;
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

    /* Focus border */
    if (wv->focused) {
        cairo_set_source_rgba(cr, 0.40, 0.60, 0.90, 0.85);
        cairo_set_line_width(cr, 2.0);
        cairo_rectangle(cr, 1.0, 1.0, (double)(w - 2), (double)(h - 2));
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

/* Set focused track, update border on all WaveViews */
static void timeline_set_focused(JackDawTimeline *tl, JackDawTrack *track)
{
    tl->focused_track = track;

    GHashTableIter iter;
    gpointer key, val;
    g_hash_table_iter_init(&iter, tl->wave_views);
    while (g_hash_table_iter_next(&iter, &key, &val)) {
        JackDawWaveView *wv = JACKDAW_WAVE_VIEW(val);
        jackdaw_wave_view_set_focused(wv, (JackDawTrack *)key == track);
    }

    g_signal_emit(tl, timeline_signals[SIGNAL_TRACK_FOCUSED], 0, track);
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

/* Select the region of `track` that covers `frame` (highlight its full span);
 * clears the selection if the frame is in a gap. */
static void timeline_select_region_at(JackDawTimeline *tl, JackDawTrack *track,
                                      off_t frame)
{
    ClipRegion *r = track
        ? clip_region_list_at(jackdaw_track_get_regions(track), frame) : NULL;
    if (r) {
        tl->sel_active = TRUE;
        tl->sel_start  = r->tl_pos;
        tl->sel_end    = clip_region_end(r);
    } else {
        tl->sel_active = FALSE;
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

/* ---- Region-edit undo/redo ---- */

static void region_snapshot_free(gpointer p)
{
    if (p) g_ptr_array_unref((GPtrArray *)p);
}

static void undo_queue_free(gpointer q)
{
    if (!q) return;
    g_queue_free_full((GQueue *)q, region_snapshot_free);
}

static GQueue *undo_stack_for(GHashTable *tbl, JackDawTrack *t)
{
    GQueue *q = g_hash_table_lookup(tbl, t);
    if (!q) { q = g_queue_new(); g_hash_table_insert(tbl, t, q); }
    return q;
}

static void timeline_push_undo(JackDawTimeline *tl, JackDawTrack *t)
{
    GQueue *u = undo_stack_for(tl->undo_stacks, t);
    g_queue_push_head(u, clip_region_list_copy(jackdaw_track_get_regions(t)));
    while (g_queue_get_length(u) > 64)
        region_snapshot_free(g_queue_pop_tail(u));
    /* A new edit invalidates the redo history for this track. */
    GQueue *r = g_hash_table_lookup(tl->redo_stacks, t);
    if (r) while (!g_queue_is_empty(r))
        region_snapshot_free(g_queue_pop_head(r));
}

/* Replace a track's region list with copies from `list` (does not consume). */
static void timeline_apply_regions(JackDawTrack *t, GPtrArray *list)
{
    GPtrArray *regs = jackdaw_track_get_regions(t);
    if (regs->len > 0)
        g_ptr_array_remove_range(regs, 0, regs->len);
    for (guint i = 0; i < list->len; i++)
        g_ptr_array_add(regs, clip_region_copy(g_ptr_array_index(list, i)));
    jackdaw_track_commit_regions(t);
}

void jackdaw_timeline_undo(JackDawTimeline *tl)
{
    g_return_if_fail(JACKDAW_IS_TIMELINE(tl));
    JackDawTrack *t = tl->focused_track;
    if (!t) return;
    GQueue *u = g_hash_table_lookup(tl->undo_stacks, t);
    if (!u || g_queue_is_empty(u)) return;
    GQueue *r = undo_stack_for(tl->redo_stacks, t);
    g_queue_push_head(r, clip_region_list_copy(jackdaw_track_get_regions(t)));
    GPtrArray *snap = g_queue_pop_head(u);
    timeline_apply_regions(t, snap);
    region_snapshot_free(snap);
    jackdaw_timeline_redraw_all(tl);
}

void jackdaw_timeline_redo(JackDawTimeline *tl)
{
    g_return_if_fail(JACKDAW_IS_TIMELINE(tl));
    JackDawTrack *t = tl->focused_track;
    if (!t) return;
    GQueue *r = g_hash_table_lookup(tl->redo_stacks, t);
    if (!r || g_queue_is_empty(r)) return;
    GQueue *u = undo_stack_for(tl->undo_stacks, t);
    g_queue_push_head(u, clip_region_list_copy(jackdaw_track_get_regions(t)));
    GPtrArray *snap = g_queue_pop_head(r);
    timeline_apply_regions(t, snap);
    region_snapshot_free(snap);
    jackdaw_timeline_redraw_all(tl);
}

/* ---- Region edit operations ---- */

/* Split `t` at the playhead and select the region to the right of the split. */
static void timeline_split_track_at_playhead(JackDawTimeline *tl,
                                             JackDawTrack *t)
{
    if (!t) return;
    off_t cur = (off_t)gtk_adjustment_get_value(tl->cursor_adj);
    timeline_push_undo(tl, t);
    clip_region_list_split_at(jackdaw_track_get_regions(t), cur,
                              (int)timeline_jack_sr());
    jackdaw_track_commit_regions(t);
    /* Focus the new right-hand region so it can be moved/deleted next. */
    timeline_select_region_at(tl, t, cur);
    jackdaw_timeline_redraw_all(tl);
}

void jackdaw_timeline_split_at_cursor(JackDawTimeline *tl)
{
    g_return_if_fail(JACKDAW_IS_TIMELINE(tl));
    timeline_split_track_at_playhead(tl, tl->focused_track);
}

/* ---- Context menu ---- */

static void menu_split_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    JackDawTimeline *tl = data;
    timeline_split_track_at_playhead(tl, tl->menu_track);
}

static void menu_delete_sel_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    JackDawTimeline *tl = data;
    if (!tl->menu_track || !tl->sel_active) return;
    off_t a = tl->sel_start, b = tl->sel_end;
    if (b < a) { off_t tmp = a; a = b; b = tmp; }
    timeline_push_undo(tl, tl->menu_track);
    clip_region_list_delete_range(jackdaw_track_get_regions(tl->menu_track),
                                  a, b, (int)timeline_jack_sr());
    jackdaw_track_commit_regions(tl->menu_track);
    jackdaw_timeline_redraw_all(tl);
}

static void menu_delete_region_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    JackDawTimeline *tl = data;
    if (!tl->menu_track) return;
    timeline_push_undo(tl, tl->menu_track);
    clip_region_list_remove_at(jackdaw_track_get_regions(tl->menu_track),
                               tl->menu_frame);
    jackdaw_track_commit_regions(tl->menu_track);
    jackdaw_timeline_redraw_all(tl);
}

static void menu_gain_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    JackDawTimeline *tl = data;
    if (!tl->menu_track || !tl->sel_active) return;
    off_t a = tl->sel_start, b = tl->sel_end;
    if (b < a) { off_t tmp = a; a = b; b = tmp; }

    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "Region Gain",
        GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(tl))),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Apply", GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget *sc  = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
                                              -24.0, 12.0, 0.5);
    gtk_range_set_value(GTK_RANGE(sc), 0.0);
    gtk_widget_set_size_request(sc, 260, -1);
    GtkWidget *lbl = gtk_label_new("Gain (dB) for the selected area:");
    gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(box), sc,  FALSE, FALSE, 4);
    gtk_widget_show_all(dlg);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        double db = gtk_range_get_value(GTK_RANGE(sc));
        gfloat g  = (gfloat)pow(10.0, db / 20.0);
        timeline_push_undo(tl, tl->menu_track);
        clip_region_list_set_gain_range(
            jackdaw_track_get_regions(tl->menu_track), a, b, g,
            (int)timeline_jack_sr());
        jackdaw_track_commit_regions(tl->menu_track);
        jackdaw_timeline_redraw_all(tl);
    }
    gtk_widget_destroy(dlg);
}

static void timeline_show_context_menu(JackDawTimeline *tl, GdkEventButton *ev)
{
    GtkWidget *menu = gtk_menu_new();
    struct { const char *label; GCallback cb; gboolean sens; } items[] = {
        { "Split at Playhead",   G_CALLBACK(menu_split_cb),         TRUE },
        { "Delete Selected Area",G_CALLBACK(menu_delete_sel_cb),    tl->sel_active },
        { "Set Selection Gain…", G_CALLBACK(menu_gain_cb),          tl->sel_active },
        { "Delete Region",       G_CALLBACK(menu_delete_region_cb), TRUE },
    };
    for (guint i = 0; i < G_N_ELEMENTS(items); i++) {
        GtkWidget *mi = gtk_menu_item_new_with_label(items[i].label);
        gtk_widget_set_sensitive(mi, items[i].sens);
        g_signal_connect(mi, "activate", items[i].cb, tl);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    }
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
}

/* Button press on a WaveView — focus, selection anchor, or context menu */
static gboolean timeline_wave_clicked(GtkWidget *widget,
                                       GdkEventButton *event, gpointer data)
{
    JackDawTimeline *tl = JACKDAW_TIMELINE(data);
    JackDawWaveView *wv = JACKDAW_WAVE_VIEW(widget);

    off_t sample = timeline_x_to_sample(tl, event->x);
    timeline_set_focused(tl, wv->track);

    if (event->button == 3) {
        tl->menu_track = wv->track;
        tl->menu_frame = sample;
        timeline_select_region_at(tl, wv->track, sample);
        timeline_show_context_menu(tl, event);
        return TRUE;
    }

    if (event->button != 1) return FALSE;

    /* Move the playhead to the click (seeks the engine when stopped) and start
     * a potential drag-selection. */
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
    tl->selecting = FALSE;
    /* A plain click (no drag) selects the region under the pointer. */
    if (!tl->sel_active) {
        JackDawWaveView *wv = JACKDAW_WAVE_VIEW(widget);
        timeline_select_region_at(tl, wv->track, tl->sel_start);
    }
    return FALSE;
}

/* Mouse-wheel on a WaveView: scroll (Ctrl = zoom) */
static gboolean timeline_wave_scroll(GtkWidget *widget,
                                      GdkEventScroll *event, gpointer data)
{
    JackDawTimeline *tl = JACKDAW_TIMELINE(data);
    (void)widget;

    if (event->state & GDK_CONTROL_MASK) {
        if (event->direction == GDK_SCROLL_UP ||
            (event->direction == GDK_SCROLL_SMOOTH && event->delta_y < 0))
            jackdaw_timeline_zoom_in(tl);
        else if (event->direction == GDK_SCROLL_DOWN ||
                 (event->direction == GDK_SCROLL_SMOOTH && event->delta_y > 0))
            jackdaw_timeline_zoom_out(tl);
    } else {
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
    }
    return TRUE;
}

/* 50 ms timer: update playhead and auto-scroll to follow it */
static gboolean timeline_update_timer(gpointer data)
{
    JackDawTimeline *tl = data;
    if (!JACKDAW_IS_TIMELINE(tl)) return G_SOURCE_REMOVE;

    /* Keep the horizontal scrollbar's range in sync with content + view. */
    {
        GtkAllocation ra;
        gtk_widget_get_allocation(GTK_WIDGET(tl->ruler), &ra);
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

static void on_project_timing_changed(JackDawProject *p, gpointer data)
{
    (void)p;
    jackdaw_timeline_redraw_all(JACKDAW_TIMELINE(data));
}

static void jackdaw_timeline_finalize(GObject *obj)
{
    JackDawTimeline *tl = JACKDAW_TIMELINE(obj);

    if (tl->update_timer) {
        g_source_remove(tl->update_timer);
        tl->update_timer = 0;
    }
    g_object_unref(tl->time_adj);
    g_object_unref(tl->zoom_adj);
    g_object_unref(tl->cursor_adj);
    if (tl->header_size_group) {
        g_object_unref(tl->header_size_group);
        tl->header_size_group = NULL;
    }
    g_hash_table_destroy(tl->wave_views);
    if (tl->undo_stacks) g_hash_table_destroy(tl->undo_stacks);
    if (tl->redo_stacks) g_hash_table_destroy(tl->redo_stacks);

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
    tl->tracks_box         = NULL;
    tl->focused_track      = NULL;
    tl->header_size_group  = NULL;
    tl->wave_views         = g_hash_table_new(g_direct_hash, g_direct_equal);
    tl->update_timer  = 0;
    tl->prev_play_pos = 0;
    tl->sel_active    = FALSE;
    tl->selecting     = FALSE;
    tl->sel_start     = 0;
    tl->sel_end       = 0;
    tl->menu_track    = NULL;
    tl->menu_frame    = 0;
    tl->hscroll       = NULL;
    tl->undo_stacks   = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                              NULL, undo_queue_free);
    tl->redo_stacks   = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                              NULL, undo_queue_free);

    gtk_orientable_set_orientation(GTK_ORIENTABLE(tl),
                                   GTK_ORIENTATION_VERTICAL);
    gtk_box_set_spacing(GTK_BOX(tl), 0);
}

GtkWidget *jackdaw_timeline_new(JackDawProject *project)
{
    g_return_val_if_fail(JACKDAW_IS_PROJECT(project), NULL);

    JackDawTimeline *tl = g_object_new(JACKDAW_TYPE_TIMELINE, NULL);
    tl->project = project;

    tl->time_adj   = gtk_adjustment_new(0.0, 0.0, (gdouble)G_MAXINT64,
                                        1024.0, 4096.0, 0.0);
    tl->zoom_adj   = gtk_adjustment_new(1000.0, 1.0, 2000000.0,
                                        100.0, 1000.0, 0.0);
    tl->cursor_adj = gtk_adjustment_new(0.0, 0.0, (gdouble)G_MAXINT64,
                                        1.0, 1.0, 0.0);

    /* Size group: keeps ruler spacer and every track strip at the same width.
     * No fixed pixel values needed — GTK negotiates based on actual content. */
    tl->header_size_group = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);

    /* ---- Ruler row ---- */
    GtkWidget *ruler_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(spacer, TIMELINE_HEADER_WIDTH, -1);
    gtk_size_group_add_widget(tl->header_size_group, spacer);

    guint32 sr = jackdaw_engine_is_running()
                 ? (guint32)jackdaw_engine_get_sample_rate()
                 : 48000u;

    tl->ruler = JACKDAW_TIME_RULER(
        jackdaw_time_ruler_new(tl->time_adj, tl->zoom_adj, tl->cursor_adj, sr));
    tl->ruler->project = project;

    gtk_box_pack_start(GTK_BOX(ruler_row), spacer,         FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ruler_row), GTK_WIDGET(tl->ruler), TRUE, TRUE, 0);

    /* ---- Scrolled window for track rows ---- */
    tl->tracks_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(tl->tracks_scroll),
        GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

    tl->tracks_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_container_add(GTK_CONTAINER(tl->tracks_scroll), tl->tracks_box);

    gtk_box_pack_start(GTK_BOX(tl), ruler_row,         FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tl), tl->tracks_scroll, TRUE,  TRUE,  0);

    /* Horizontal scrollbar bound to the shared time adjustment. Its bounds
     * are kept in sync with the project length by the update timer. */
    tl->hscroll = gtk_scrollbar_new(GTK_ORIENTATION_HORIZONTAL, tl->time_adj);
    gtk_box_pack_start(GTK_BOX(tl), tl->hscroll, FALSE, FALSE, 0);

    gtk_widget_show_all(GTK_WIDGET(tl));

    g_signal_connect_object(project, "track-added",
                            G_CALLBACK(on_project_track_added), tl, 0);
    g_signal_connect_object(project, "track-removed",
                            G_CALLBACK(on_project_track_removed), tl, 0);
    g_signal_connect_object(project, "timing-changed",
                            G_CALLBACK(on_project_timing_changed), tl, 0);

    tl->update_timer = g_timeout_add(50, timeline_update_timer, tl);

    return GTK_WIDGET(tl);
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

    /* Track strip header (ARM/M/S, vol, pan, input selectors).
     * Adding to header_size_group keeps it aligned with the ruler spacer. */
    GtkWidget *strip = jackdaw_track_strip_new(track, tl->project);
    gtk_size_group_add_widget(tl->header_size_group, strip);

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

    gtk_box_pack_start(GTK_BOX(row), strip, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), wv,    TRUE,  TRUE,  0);

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
    g_object_set_data_full(G_OBJECT(handle), "resize-data", rd, g_free);

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
