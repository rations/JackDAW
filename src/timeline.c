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
            cairo_set_line_width(cr, 2.0);
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

    AudioClip *clip = wv->track ? jackdaw_track_get_clip(wv->track) : NULL;

    gdouble start = gtk_adjustment_get_value(wv->time_adj);
    gdouble spp   = gtk_adjustment_get_value(wv->zoom_adj);

    if (clip && clip->info.frames > 0 && spp > 0.0) {
        sf_count_t start_samp = (sf_count_t)start;
        sf_count_t end_samp   = start_samp + (sf_count_t)(w * spp) + 1;
        if (end_samp > clip->info.frames) end_samp = clip->info.frames;

        if (end_samp > start_samp) {
            int ch = clip->info.channels;
            int draw_ch = ch > 2 ? 2 : ch;  /* show at most 2 channels */

            gfloat *out_min = g_new(gfloat, (gsize)w * ch);
            gfloat *out_max = g_new(gfloat, (gsize)w * ch);

            audio_clip_get_peaks(clip, start_samp, end_samp, w,
                                 out_min, out_max);

            for (int c = 0; c < draw_ch; c++) {
                int band_h  = h / draw_ch;
                int band_y0 = c * band_h;
                int mid_y   = band_y0 + band_h / 2;

                /* Waveform fill: dark green */
                cairo_set_source_rgb(cr, 0.15, 0.50, 0.20);

                for (int x = 0; x < w; x++) {
                    gfloat mn = out_min[x * ch + c];
                    gfloat mx = out_max[x * ch + c];
                    if (mn > mx) continue; /* no data */

                    double half = (double)(band_h / 2);
                    double y_top = mid_y - mx * half;
                    double y_bot = mid_y - mn * half;

                    if (y_top < band_y0)         y_top = band_y0;
                    if (y_bot > band_y0 + band_h) y_bot = band_y0 + band_h;

                    cairo_move_to(cr, x + 0.5, y_top);
                    cairo_line_to(cr, x + 0.5, y_bot);
                }
                cairo_set_line_width(cr, 1.0);
                cairo_stroke(cr);

                /* Centre line */
                cairo_set_source_rgba(cr, 0.30, 0.70, 0.35, 0.5);
                cairo_set_line_width(cr, 1.0);
                cairo_move_to(cr, 0,     mid_y + 0.5);
                cairo_line_to(cr, w,     mid_y + 0.5);
                cairo_stroke(cr);
            }

            g_free(out_min);
            g_free(out_max);
        }
    }

    /* Transport playhead (orange) */
    if (wv->cursor_adj && spp > 0.0) {
        double px = (gtk_adjustment_get_value(wv->cursor_adj) - start) / spp;
        if (px >= 0.0 && px < (double)w) {
            cairo_set_source_rgba(cr, 1.0, 0.35, 0.0, 1.0);
            cairo_set_line_width(cr, 2.0);
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

/* Button press on a WaveView — update focus and emit position */
static gboolean timeline_wave_clicked(GtkWidget *widget,
                                       GdkEventButton *event, gpointer data)
{
    JackDawTimeline *tl = JACKDAW_TIMELINE(data);
    JackDawWaveView *wv = JACKDAW_WAVE_VIEW(widget);

    if (event->button != 1) return FALSE;

    gdouble spp    = gtk_adjustment_get_value(tl->zoom_adj);
    gdouble start  = gtk_adjustment_get_value(tl->time_adj);
    off_t   sample = (off_t)(start + event->x * spp);
    if (sample < 0) sample = 0;

    timeline_set_focused(tl, wv->track);
    g_signal_emit(tl, timeline_signals[SIGNAL_POSITION_CHANGED], 0,
                  (gint64)sample);
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

    if (!jackdaw_engine_is_running()) return G_SOURCE_CONTINUE;

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
    g_hash_table_destroy(tl->wave_views);

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
    tl->project       = NULL;
    tl->time_adj      = NULL;
    tl->zoom_adj      = NULL;
    tl->cursor_adj    = NULL;
    tl->ruler         = NULL;
    tl->tracks_scroll = NULL;
    tl->tracks_box    = NULL;
    tl->focused_track = NULL;
    tl->wave_views    = g_hash_table_new(g_direct_hash, g_direct_equal);
    tl->update_timer  = 0;
    tl->prev_play_pos = 0;

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

    /* ---- Ruler row ---- */
    GtkWidget *ruler_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(spacer, TIMELINE_HEADER_WIDTH, -1);

    guint32 sr = jackdaw_engine_is_running()
                 ? (guint32)jackdaw_engine_get_sample_rate()
                 : 48000u;

    tl->ruler = JACKDAW_TIME_RULER(
        jackdaw_time_ruler_new(tl->time_adj, tl->zoom_adj, tl->cursor_adj, sr));

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

    gtk_widget_show_all(GTK_WIDGET(tl));

    g_signal_connect_object(project, "track-added",
                            G_CALLBACK(on_project_track_added), tl, 0);
    g_signal_connect_object(project, "track-removed",
                            G_CALLBACK(on_project_track_removed), tl, 0);

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

    /* Track strip header (ARM/M/S, vol, pan, input selectors) */
    GtkWidget *strip = jackdaw_track_strip_new(track, tl->project);

    /* WaveView */
    GtkWidget *wv = jackdaw_wave_view_new(track, tl->time_adj, tl->zoom_adj,
                                          tl->cursor_adj);
    g_signal_connect(wv, "button-press-event",
                     G_CALLBACK(timeline_wave_clicked), tl);
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
