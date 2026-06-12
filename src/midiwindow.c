/* midiwindow.c — piano-roll MIDI editor.
 *
 * A per-track singleton top-level window (mirrors fxwindow's pattern). Layout:
 *   [ keyboard | note grid        ] [v-scroll]
 *   [          | velocity lane     ]
 *   [          | h-scroll          ]
 * Notes are stored in the shared MidiClip in TICKS (JACKDAW_PPQ/quarter). After
 * any edit we re-publish the track's RT event snapshot via
 * jackdaw_track_commit_midi() so playback hears the change immediately.
 */
#define _GNU_SOURCE
#include <math.h>
#include "midiwindow.h"
#include "jackdaw-engine.h"

#define KEY_W        56     /* keyboard width (px) */
#define VEL_H       110     /* velocity lane height (px) */
#define DEFAULT_KEYH  10    /* px per semitone row */
#define EDGE_PX        6    /* grab zone for note-resize (px) */
#define DEFAULT_VEL  100

typedef struct {
    JackDawTrack   *track;
    MidiRegion     *region;
    MidiClip       *clip;       /* borrowed (= region->clip) */
    JackDawProject *project;

    GtkWidget      *window;
    GtkWidget      *roll, *keys, *vel;
    GtkAdjustment  *h_adj;      /* value = leftmost tick */
    GtkAdjustment  *v_adj;      /* value = topmost row (0 = pitch 127) */
    double          tpx;        /* ticks per pixel */
    int             key_h;      /* px per semitone */

    /* interaction */
    int      drag_mode;         /* 0 none, 1 move, 2 resize, 3 velocity */
    int      drag_note;         /* index into clip->notes, or -1 */
    double   press_x, press_y;
    guint32  orig_start, orig_len;
    guint8   orig_pitch;
} MidiWindow;

/* ---- helpers ---- */

static int snap_step(MidiWindow *mw)
{
    /* 1/16 note grid; honour the project's snap toggle (free when off). */
    if (mw->project && !mw->project->snap_enabled) return 1;
    return JACKDAW_PPQ / 4;
}
static guint32 snap_tick(MidiWindow *mw, double t)
{
    int s = snap_step(mw);
    if (s <= 1 || t < 0) return (guint32)(t < 0 ? 0 : t);
    return (guint32)(floor(t / s + 0.5) * s);
}
static double tick_to_x(MidiWindow *mw, double tick)
{ return (tick - gtk_adjustment_get_value(mw->h_adj)) / mw->tpx; }
static double x_to_tick(MidiWindow *mw, double x)
{ double t = gtk_adjustment_get_value(mw->h_adj) + x * mw->tpx; return t < 0 ? 0 : t; }
static double pitch_to_y(MidiWindow *mw, int pitch)
{ return ((127 - pitch) - gtk_adjustment_get_value(mw->v_adj)) * mw->key_h; }
static int y_to_pitch(MidiWindow *mw, double y)
{
    int row = (int)floor(gtk_adjustment_get_value(mw->v_adj) + y / mw->key_h);
    int p = 127 - row;
    return CLAMP(p, 0, 127);
}

/* velocity 1..127 -> blue(low) .. green .. red(high) */
static void vel_color(int v, double *r, double *g, double *b)
{
    double f = CLAMP(v, 1, 127) / 127.0;
    if (f < 0.5) { double u = f / 0.5;        *r = 0.1;        *g = 0.3 + 0.6*u; *b = 0.9 - 0.7*u; }
    else         { double u = (f - 0.5) / 0.5; *r = 0.1 + 0.85*u; *g = 0.9 - 0.7*u; *b = 0.15; }
}

static gboolean is_black_key(int p) { int n = p % 12; return n==1||n==3||n==6||n==8||n==10; }

static void mw_commit(MidiWindow *mw)
{
    double fpb = jackdaw_project_frames_per_beat(mw->project,
                                                 jackdaw_engine_get_sample_rate());
    jackdaw_track_commit_midi(mw->track, fpb);
    gtk_widget_queue_draw(mw->roll);
    gtk_widget_queue_draw(mw->vel);
}

/* Topmost note whose rect contains (x,y) in roll coords; -1 if none. */
static int note_at(MidiWindow *mw, double x, double y, gboolean *on_edge)
{
    if (on_edge) *on_edge = FALSE;
    for (int i = (int)midi_clip_note_count(mw->clip) - 1; i >= 0; i--) {
        MidiNote *n = midi_clip_note(mw->clip, i);
        double nx = tick_to_x(mw, n->start);
        double nw = (double)n->length / mw->tpx;
        double ny = pitch_to_y(mw, n->pitch);
        if (x >= nx && x <= nx + nw && y >= ny && y <= ny + mw->key_h) {
            if (on_edge && x >= nx + nw - EDGE_PX) *on_edge = TRUE;
            return i;
        }
    }
    return -1;
}

/* ---- drawing ---- */

static gboolean roll_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    MidiWindow *mw = data;
    GtkAllocation a; gtk_widget_get_allocation(w, &a);

    cairo_set_source_rgb(cr, 0.16, 0.16, 0.18);
    cairo_paint(cr);

    /* horizontal rows: shade black-key rows darker */
    int top_row = (int)floor(gtk_adjustment_get_value(mw->v_adj));
    for (int row = top_row; row * mw->key_h - (int)(gtk_adjustment_get_value(mw->v_adj)*mw->key_h) < a.height + mw->key_h; row++) {
        int pitch = 127 - row;
        if (pitch < 0) break;
        double y = (row - gtk_adjustment_get_value(mw->v_adj)) * mw->key_h;
        if (is_black_key(pitch))
            cairo_set_source_rgb(cr, 0.13, 0.13, 0.15);
        else
            cairo_set_source_rgb(cr, 0.18, 0.18, 0.20);
        cairo_rectangle(cr, 0, y, a.width, mw->key_h);
        cairo_fill(cr);
        if (pitch % 12 == 0) {   /* C: a slightly stronger row line */
            cairo_set_source_rgba(cr, 0, 0, 0, 0.4);
            cairo_move_to(cr, 0, y + mw->key_h + 0.5);
            cairo_line_to(cr, a.width, y + mw->key_h + 0.5); cairo_stroke(cr);
        }
    }

    /* vertical beat / bar grid */
    guint bpb = (mw->project && mw->project->beats_per_bar) ? mw->project->beats_per_bar : 4;
    double tick0 = gtk_adjustment_get_value(mw->h_adj);
    long beat0 = (long)(tick0 / JACKDAW_PPQ);
    for (long beat = beat0; ; beat++) {
        double x = tick_to_x(mw, (double)beat * JACKDAW_PPQ);
        if (x > a.width) break;
        if (x < 0) continue;
        if (beat % bpb == 0) cairo_set_source_rgba(cr, 0.6, 0.6, 0.7, 0.55);
        else                 cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.25);
        cairo_move_to(cr, floor(x) + 0.5, 0);
        cairo_line_to(cr, floor(x) + 0.5, a.height); cairo_stroke(cr);
    }

    /* notes */
    guint nc = midi_clip_note_count(mw->clip);
    for (guint i = 0; i < nc; i++) {
        MidiNote *n = midi_clip_note(mw->clip, i);
        double nx = tick_to_x(mw, n->start);
        double nw = (double)n->length / mw->tpx;
        double ny = pitch_to_y(mw, n->pitch);
        if (nx + nw < 0 || nx > a.width) continue;
        if (nw < 2) nw = 2;
        double r, g, b; vel_color(n->velocity, &r, &g, &b);
        cairo_set_source_rgb(cr, r, g, b);
        cairo_rectangle(cr, nx, ny + 0.5, nw, mw->key_h - 1);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
        cairo_rectangle(cr, nx + 0.5, ny + 0.5, nw, mw->key_h - 1);
        cairo_stroke(cr);
    }
    return FALSE;
}

static gboolean keys_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    MidiWindow *mw = data;
    GtkAllocation a; gtk_widget_get_allocation(w, &a);
    cairo_set_source_rgb(cr, 0.10, 0.10, 0.11);
    cairo_paint(cr);

    int top_row = (int)floor(gtk_adjustment_get_value(mw->v_adj));
    for (int row = top_row; ; row++) {
        int pitch = 127 - row;
        if (pitch < 0) break;
        double y = (row - gtk_adjustment_get_value(mw->v_adj)) * mw->key_h;
        if (y > a.height) break;
        if (is_black_key(pitch)) cairo_set_source_rgb(cr, 0.08, 0.08, 0.09);
        else                     cairo_set_source_rgb(cr, 0.85, 0.85, 0.85);
        cairo_rectangle(cr, 0, y, a.width, mw->key_h);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.3);
        cairo_rectangle(cr, 0, y, a.width, mw->key_h); cairo_stroke(cr);
        if (pitch % 12 == 0 && mw->key_h >= 8) {   /* label Cn */
            cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
            char buf[8]; g_snprintf(buf, sizeof buf, "C%d", pitch / 12 - 1);
            cairo_move_to(cr, 4, y + mw->key_h - 2);
            cairo_show_text(cr, buf);
        }
    }
    return FALSE;
}

static gboolean vel_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    MidiWindow *mw = data;
    GtkAllocation a; gtk_widget_get_allocation(w, &a);
    cairo_set_source_rgb(cr, 0.12, 0.12, 0.14);
    cairo_paint(cr);

    guint nc = midi_clip_note_count(mw->clip);
    for (guint i = 0; i < nc; i++) {
        MidiNote *n = midi_clip_note(mw->clip, i);
        double nx = tick_to_x(mw, n->start);
        if (nx < 0 || nx > a.width) continue;
        double h = (n->velocity / 127.0) * (a.height - 4);
        double r, g, b; vel_color(n->velocity, &r, &g, &b);
        cairo_set_source_rgb(cr, r, g, b);
        cairo_set_line_width(cr, 2.0);
        cairo_move_to(cr, floor(nx) + 1, a.height);
        cairo_line_to(cr, floor(nx) + 1, a.height - h);
        cairo_stroke(cr);
    }
    cairo_set_line_width(cr, 1.0);
    return FALSE;
}

/* ---- interaction ---- */

static gboolean roll_press(GtkWidget *w, GdkEventButton *e, gpointer data)
{
    MidiWindow *mw = data;
    gboolean edge = FALSE;
    int idx = note_at(mw, e->x, e->y, &edge);

    if (e->button == 3) {                 /* right-click: delete */
        if (idx >= 0) { midi_clip_remove_note(mw->clip, idx); mw_commit(mw); }
        return TRUE;
    }
    if (e->button != 1) return FALSE;

    if (idx < 0) {                        /* empty: add a note */
        MidiNote n;
        n.start    = snap_tick(mw, x_to_tick(mw, e->x));
        n.length   = snap_step(mw) > 1 ? snap_step(mw) : JACKDAW_PPQ / 4;
        n.pitch    = (guint8)y_to_pitch(mw, e->y);
        n.velocity = DEFAULT_VEL;
        n.channel  = 0;
        idx = midi_clip_add_note(mw->clip, n);
        mw->drag_mode = 1;                /* let the user drag the new note */
    } else {
        mw->drag_mode = edge ? 2 : 1;
    }
    MidiNote *n = midi_clip_note(mw->clip, idx);
    mw->drag_note   = idx;
    mw->press_x     = e->x; mw->press_y = e->y;
    mw->orig_start  = n->start; mw->orig_len = n->length; mw->orig_pitch = n->pitch;
    mw_commit(mw);
    return TRUE;
}

static gboolean roll_motion(GtkWidget *w, GdkEventMotion *e, gpointer data)
{
    MidiWindow *mw = data;
    if (mw->drag_mode == 0 || mw->drag_note < 0) {
        /* hover cursor hint over a resize edge */
        gboolean edge = FALSE; note_at(mw, e->x, e->y, &edge);
        GdkWindow *gw = gtk_widget_get_window(w);
        if (gw) {
            GdkCursor *c = edge ? gdk_cursor_new_from_name(gdk_display_get_default(), "ew-resize") : NULL;
            gdk_window_set_cursor(gw, c);
            if (c) g_object_unref(c);
        }
        return FALSE;
    }
    MidiNote *n = midi_clip_note(mw->clip, mw->drag_note);
    if (!n) { mw->drag_mode = 0; return FALSE; }
    double dt = (e->x - mw->press_x) * mw->tpx;

    if (mw->drag_mode == 1) {             /* move: time + pitch */
        long ns = (long)mw->orig_start + (long)dt;
        n->start = snap_tick(mw, ns < 0 ? 0 : ns);
        int dp = (int)floor((e->y - mw->press_y) / mw->key_h + 0.5);
        int np = CLAMP((int)mw->orig_pitch - dp, 0, 127);
        n->pitch = (guint8)np;
    } else if (mw->drag_mode == 2) {      /* resize length */
        long nl = (long)mw->orig_len + (long)dt;
        int step = snap_step(mw) > 1 ? snap_step(mw) : 1;
        if (nl < step) nl = step;
        n->length = snap_tick(mw, nl);
        if (n->length < (guint32)step) n->length = step;
    }
    mw_commit(mw);
    return TRUE;
}

static gboolean roll_release(GtkWidget *w, GdkEventButton *e, gpointer data)
{
    (void)w; (void)e;
    MidiWindow *mw = data;
    mw->drag_mode = 0; mw->drag_note = -1;
    return FALSE;
}

/* velocity lane: drag sets the nearest note's velocity from y */
static void vel_set_from(MidiWindow *mw, double x, double y)
{
    GtkAllocation a; gtk_widget_get_allocation(mw->vel, &a);
    int best = -1; double bestd = 1e18;
    guint nc = midi_clip_note_count(mw->clip);
    for (guint i = 0; i < nc; i++) {
        MidiNote *n = midi_clip_note(mw->clip, i);
        double nx = tick_to_x(mw, n->start);
        double d = fabs(nx - x);
        if (d < bestd && d < 8.0) { bestd = d; best = (int)i; }
    }
    if (best < 0) return;
    int v = (int)((1.0 - CLAMP(y, 0, a.height) / a.height) * 127.0 + 0.5);
    midi_clip_note(mw->clip, best)->velocity = (guint8)CLAMP(v, 1, 127);
    mw_commit(mw);
}
static gboolean vel_press(GtkWidget *w, GdkEventButton *e, gpointer data)
{ (void)w; if (e->button == 1) { vel_set_from(data, e->x, e->y); } return TRUE; }
static gboolean vel_motion(GtkWidget *w, GdkEventMotion *e, gpointer data)
{ (void)w; if (e->state & GDK_BUTTON1_MASK) vel_set_from(data, e->x, e->y); return TRUE; }

static void redraw_all(MidiWindow *mw)
{ gtk_widget_queue_draw(mw->roll); gtk_widget_queue_draw(mw->keys);
  gtk_widget_queue_draw(mw->vel); }

static gboolean roll_scroll(GtkWidget *w, GdkEventScroll *e, gpointer data)
{
    (void)w;
    MidiWindow *mw = data;
    gdouble dx = 0, dy = 0;
    if (e->direction == GDK_SCROLL_SMOOTH) gdk_event_get_scroll_deltas((GdkEvent *)e, &dx, &dy);
    else if (e->direction == GDK_SCROLL_UP)   dy = -1;
    else if (e->direction == GDK_SCROLL_DOWN) dy =  1;
    else if (e->direction == GDK_SCROLL_LEFT) dx = -1;
    else if (e->direction == GDK_SCROLL_RIGHT)dx =  1;

    if (e->state & GDK_CONTROL_MASK) {          /* zoom (horizontal) */
        double f = (dy > 0) ? 1.2 : 0.8;
        mw->tpx = CLAMP(mw->tpx * f, 0.5, 200.0);
    } else if (e->state & GDK_SHIFT_MASK) {     /* horizontal pan */
        double v = gtk_adjustment_get_value(mw->h_adj) + dy * mw->tpx * 60.0;
        gtk_adjustment_set_value(mw->h_adj, CLAMP(v, 0, gtk_adjustment_get_upper(mw->h_adj)));
    } else {                                     /* vertical pan (pitch) */
        double v = gtk_adjustment_get_value(mw->v_adj) + dy * 3.0;
        gtk_adjustment_set_value(mw->v_adj, CLAMP(v, 0, gtk_adjustment_get_upper(mw->v_adj)));
    }
    redraw_all(mw);
    return TRUE;
}

static void adj_changed(GtkAdjustment *a, gpointer data) { (void)a; redraw_all(data); }

/* ---- window lifecycle ---- */

static gboolean mw_delete(GtkWidget *w, GdkEvent *e, gpointer data)
{
    (void)w; (void)e;
    MidiWindow *mw = data;
    g_object_set_data(G_OBJECT(mw->track), "midi-window", NULL);
    gtk_widget_destroy(mw->window);
    g_free(mw);
    return TRUE;
}

void jackdaw_midi_window_open(JackDawTrack *track, MidiRegion *region,
                             JackDawProject *project)
{
    g_return_if_fail(JACKDAW_IS_TRACK(track));
    if (!region || !region->clip) return;

    MidiWindow *mw = g_object_get_data(G_OBJECT(track), "midi-window");
    if (mw) {                                   /* retarget existing window */
        mw->region = region; mw->clip = region->clip;
        gtk_window_present(GTK_WINDOW(mw->window));
        redraw_all(mw);
        return;
    }

    mw = g_new0(MidiWindow, 1);
    mw->track = track; mw->region = region; mw->clip = region->clip;
    mw->project = project;
    mw->tpx = 4.0; mw->key_h = DEFAULT_KEYH; mw->drag_note = -1;

    mw->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gchar *title = g_strdup_printf("Piano Roll: %s", jackdaw_track_get_name(track));
    gtk_window_set_title(GTK_WINDOW(mw->window), title);
    g_free(title);
    gtk_window_set_default_size(GTK_WINDOW(mw->window), 900, 560);
    g_signal_connect(mw->window, "delete-event", G_CALLBACK(mw_delete), mw);

    /* Adjustments. v_adj rows: 128 pitches; start scrolled to the middle (~C3). */
    mw->h_adj = gtk_adjustment_new(0, 0, (gdouble)mw->clip->length + JACKDAW_PPQ * 16,
                                   JACKDAW_PPQ / 4, JACKDAW_PPQ, 0);
    mw->v_adj = gtk_adjustment_new(48, 0, 128, 1, 12, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_container_add(GTK_CONTAINER(mw->window), grid);

    mw->keys = gtk_drawing_area_new();
    gtk_widget_set_size_request(mw->keys, KEY_W, -1);
    gtk_widget_set_vexpand(mw->keys, TRUE);

    mw->roll = gtk_drawing_area_new();
    gtk_widget_set_hexpand(mw->roll, TRUE);
    gtk_widget_set_vexpand(mw->roll, TRUE);
    gtk_widget_add_events(mw->roll, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                          GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);

    mw->vel = gtk_drawing_area_new();
    gtk_widget_set_size_request(mw->vel, -1, VEL_H);
    gtk_widget_set_hexpand(mw->vel, TRUE);
    gtk_widget_add_events(mw->vel, GDK_BUTTON_PRESS_MASK | GDK_POINTER_MOTION_MASK);

    GtkWidget *vscroll = gtk_scrollbar_new(GTK_ORIENTATION_VERTICAL, mw->v_adj);
    GtkWidget *hscroll = gtk_scrollbar_new(GTK_ORIENTATION_HORIZONTAL, mw->h_adj);

    /* grid layout: col0=keys, col1=roll, col2=vscroll; rows: roll, vel, hscroll */
    gtk_grid_attach(GTK_GRID(grid), mw->keys, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), mw->roll, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), vscroll,  2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), mw->vel,  1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), hscroll,  1, 2, 1, 1);

    g_signal_connect(mw->roll, "draw",                 G_CALLBACK(roll_draw),    mw);
    g_signal_connect(mw->keys, "draw",                 G_CALLBACK(keys_draw),    mw);
    g_signal_connect(mw->vel,  "draw",                 G_CALLBACK(vel_draw),     mw);
    g_signal_connect(mw->roll, "button-press-event",   G_CALLBACK(roll_press),   mw);
    g_signal_connect(mw->roll, "button-release-event", G_CALLBACK(roll_release), mw);
    g_signal_connect(mw->roll, "motion-notify-event",  G_CALLBACK(roll_motion),  mw);
    g_signal_connect(mw->roll, "scroll-event",         G_CALLBACK(roll_scroll),  mw);
    g_signal_connect(mw->vel,  "button-press-event",   G_CALLBACK(vel_press),    mw);
    g_signal_connect(mw->vel,  "motion-notify-event",  G_CALLBACK(vel_motion),   mw);
    g_signal_connect(mw->h_adj, "value-changed",       G_CALLBACK(adj_changed),  mw);
    g_signal_connect(mw->v_adj, "value-changed",       G_CALLBACK(adj_changed),  mw);

    g_object_set_data(G_OBJECT(track), "midi-window", mw);
    gtk_widget_show_all(mw->window);
}
