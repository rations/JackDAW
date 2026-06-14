/* midiwindow.c — piano-roll MIDI editor.
 *
 * A per-track singleton top-level window.  Layout:
 *   [transport toolbar — matches mainwindow style]
 *   [separator]
 *   [ keyboard | ruler (bar numbers + playhead) ] [        ]
 *   [ keyboard | note grid (+ playhead)          ] [v-scroll]
 *   [          | velocity lane                   ]
 *   [          | h-scroll                        ]
 *
 * Notes are stored in TICKS (JACKDAW_PPQ/quarter).  After any edit we
 * re-publish the RT snapshot via jackdaw_track_commit_midi().
 * A 50 ms timer drives the playhead and auto-scroll (same rate as the
 * main timeline).
 */
#define _GNU_SOURCE
#include <math.h>
#include <string.h>
#include "midiwindow.h"
#include "jackdaw-engine.h"
#include "main.h"

#define KEY_W        42     /* keyboard width (px) */
#define VEL_H       110     /* velocity lane height (px) */
#define DEFAULT_KEYH   8    /* px per semitone row */
#define EDGE_PX        6    /* grab zone for note-resize (px) */
#define DEFAULT_VEL  100
#define RULER_H       18    /* time ruler height (px) */
#define VEL_BAR_W      8    /* velocity bar HIT ZONE width (px); drawn as 2px line */

typedef struct {
    JackDawTrack   *track;
    MidiClip       *clip;       /* track->midi_clip; absolute tick positions */
    JackDawProject *project;

    GtkWidget      *window;
    GtkWidget      *roll, *keys, *vel, *ruler;
    GtkWidget      *btn_play, *btn_pause, *btn_stop, *btn_loop;
    GtkWidget      *time_label;
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
    int      ctx_note_idx;      /* note saved when right-click menu opens */

    /* selection (editor-only; parallel to clip->notes by index) */
    gboolean *sel;              /* sel[i] = note i is selected */
    guint     sel_cap;          /* allocated entries in sel */

    /* group edit: original start/pitch/velocity of every note, at press */
    guint32  *grp_start;
    guint8   *grp_pitch;
    guint8   *grp_vel;
    guint     grp_cap;

    /* right-drag rubber-band selection box */
    gboolean  sel_dragging;     /* right button held down on the roll */
    gboolean  sel_moved;        /* pointer moved enough to be a box, not a menu */
    double    sel_x0, sel_y0;   /* anchor (button-press point) */
    double    sel_x1, sel_y1;   /* current pointer point */

    /* playhead */
    double   play_tick;         /* clip-relative tick of playhead; -1 = off / before region */
    off_t    prev_play_pos;     /* last seen engine position (for auto-scroll) */
    gboolean ruler_dragging;    /* left button held on the ruler (scrubbing) */
    int      loop_drag_edge;    /* 0 none, 1 dragging loop start, 2 loop end */

    guint    update_timer;      /* 50 ms timer id */
} MidiWindow;

/* ---- helpers ---- */

static int snap_step(MidiWindow *mw)
{
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
    return CLAMP(127 - row, 0, 127);
}

/* velocity 1..127 -> blue(low) .. green .. red(high) */
static void vel_color(int v, double *r, double *g, double *b)
{
    double f = CLAMP(v, 1, 127) / 127.0;
    if (f < 0.5) { double u = f / 0.5;        *r = 0.1;        *g = 0.3 + 0.6*u; *b = 0.9 - 0.7*u; }
    else         { double u = (f - 0.5) / 0.5; *r = 0.1 + 0.85*u; *g = 0.9 - 0.7*u; *b = 0.15; }
}

static gboolean is_black_key(int p) { int n = p % 12; return n==1||n==3||n==6||n==8||n==10; }

/* GTK CSS class add/remove (same helper as mainwindow). */
static void mw_set_class(GtkWidget *w, const char *cls, gboolean add)
{
    GtkStyleContext *ctx = gtk_widget_get_style_context(w);
    if (add) gtk_style_context_add_class(ctx, cls);
    else     gtk_style_context_remove_class(ctx, cls);
}

static void mw_commit(MidiWindow *mw)
{
    double fpb = jackdaw_project_frames_per_beat(mw->project,
                                                 jackdaw_engine_get_sample_rate());
    jackdaw_track_commit_midi(mw->track, fpb);
    gtk_widget_queue_draw(mw->roll);
    gtk_widget_queue_draw(mw->vel);
    gtk_widget_queue_draw(mw->ruler);
}

/* Topmost note whose rect contains (x,y); -1 if none. */
static int note_at(MidiWindow *mw, double x, double y, gboolean *on_edge)
{
    if (on_edge) *on_edge = FALSE;
    for (int i = (int)midi_clip_note_count(mw->clip) - 1; i >= 0; i--) {
        MidiNote *n = midi_clip_note(mw->clip, (guint)i);
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

/* ---- selection ---- */

/* Grow the selection array to cover every note (new entries unselected). */
static void sel_ensure(MidiWindow *mw)
{
    guint nc = midi_clip_note_count(mw->clip);
    if (nc > mw->sel_cap) {
        mw->sel = g_realloc(mw->sel, nc * sizeof(gboolean));
        memset(mw->sel + mw->sel_cap, 0, (nc - mw->sel_cap) * sizeof(gboolean));
        mw->sel_cap = nc;
    }
}
static void sel_clear(MidiWindow *mw)
{
    if (mw->sel) memset(mw->sel, 0, mw->sel_cap * sizeof(gboolean));
}
static gboolean sel_is(MidiWindow *mw, guint i)
{
    return (i < mw->sel_cap) && mw->sel[i];
}
static guint sel_count(MidiWindow *mw)
{
    guint c = 0, nc = midi_clip_note_count(mw->clip);
    for (guint i = 0; i < nc && i < mw->sel_cap; i++)
        if (mw->sel[i]) c++;
    return c;
}
/* Redraw the surfaces that show the selection (roll + velocity lane). */
static void sel_redraw(MidiWindow *mw)
{
    gtk_widget_queue_draw(mw->roll);
    gtk_widget_queue_draw(mw->vel);
}
static void sel_all(MidiWindow *mw)
{
    sel_ensure(mw);
    guint nc = midi_clip_note_count(mw->clip);
    for (guint i = 0; i < nc; i++) mw->sel[i] = TRUE;
    sel_redraw(mw);
}

/* Snapshot every note's start/pitch so a group drag can apply a uniform
 * delta from the originals (avoids drift from re-reading moved notes). */
static void grp_capture(MidiWindow *mw)
{
    guint nc = midi_clip_note_count(mw->clip);
    if (nc > mw->grp_cap) {
        mw->grp_start = g_realloc(mw->grp_start, nc * sizeof(guint32));
        mw->grp_pitch = g_realloc(mw->grp_pitch, nc * sizeof(guint8));
        mw->grp_vel   = g_realloc(mw->grp_vel,   nc * sizeof(guint8));
        mw->grp_cap = nc;
    }
    for (guint i = 0; i < nc; i++) {
        MidiNote *n = midi_clip_note(mw->clip, i);
        mw->grp_start[i] = n->start;
        mw->grp_pitch[i] = n->pitch;
        mw->grp_vel[i]   = n->velocity;
    }
}

/* Recompute selection from the current rubber-band rectangle. */
static void sel_update_box(MidiWindow *mw)
{
    sel_ensure(mw);
    sel_clear(mw);
    double x0 = MIN(mw->sel_x0, mw->sel_x1), x1 = MAX(mw->sel_x0, mw->sel_x1);
    double y0 = MIN(mw->sel_y0, mw->sel_y1), y1 = MAX(mw->sel_y0, mw->sel_y1);
    guint nc = midi_clip_note_count(mw->clip);
    for (guint i = 0; i < nc; i++) {
        MidiNote *n = midi_clip_note(mw->clip, i);
        double nx = tick_to_x(mw, n->start);
        double nw = (double)n->length / mw->tpx;
        double ny = pitch_to_y(mw, n->pitch);
        /* select if the note's rect intersects the box */
        if (nx + nw >= x0 && nx <= x1 && ny + mw->key_h >= y0 && ny <= y1)
            mw->sel[i] = TRUE;
    }
}

/* ---- drawing ---- */

static gboolean roll_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    MidiWindow *mw = data;
    GtkAllocation a; gtk_widget_get_allocation(w, &a);

    cairo_set_source_rgb(cr, 0.16, 0.16, 0.18);
    cairo_paint(cr);

    /* horizontal rows */
    int top_row = (int)floor(gtk_adjustment_get_value(mw->v_adj));
    for (int row = top_row;
         row * mw->key_h - (int)(gtk_adjustment_get_value(mw->v_adj) * mw->key_h)
             < a.height + mw->key_h;
         row++) {
        int pitch = 127 - row;
        if (pitch < 0) break;
        double y = (row - gtk_adjustment_get_value(mw->v_adj)) * mw->key_h;
        if (is_black_key(pitch)) cairo_set_source_rgb(cr, 0.13, 0.13, 0.15);
        else                     cairo_set_source_rgb(cr, 0.18, 0.18, 0.20);
        cairo_rectangle(cr, 0, y, a.width, mw->key_h);
        cairo_fill(cr);
        if (pitch % 12 == 0) {
            cairo_set_source_rgba(cr, 0, 0, 0, 0.4);
            cairo_move_to(cr, 0, y + mw->key_h + 0.5);
            cairo_line_to(cr, a.width, y + mw->key_h + 0.5);
            cairo_stroke(cr);
        }
    }

    /* vertical beat / bar grid */
    guint bpb = (mw->project && mw->project->beats_per_bar) ? mw->project->beats_per_bar : 4;
    double tick0 = gtk_adjustment_get_value(mw->h_adj);
    long beat0 = (long)(tick0 / JACKDAW_PPQ);
    cairo_set_line_width(cr, 1.0);
    for (long beat = beat0; ; beat++) {
        double x = tick_to_x(mw, (double)beat * JACKDAW_PPQ);
        if (x > a.width) break;
        if (x < 0) continue;
        if (beat % (long)bpb == 0) cairo_set_source_rgba(cr, 0.6, 0.6, 0.7, 0.55);
        else                       cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.25);
        cairo_move_to(cr, floor(x) + 0.5, 0);
        cairo_line_to(cr, floor(x) + 0.5, a.height);
        cairo_stroke(cr);
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
        if (sel_is(mw, i)) {                 /* selected: bright outline */
            cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
            cairo_set_line_width(cr, 2.0);
            cairo_rectangle(cr, nx + 1.0, ny + 1.5, nw - 1, mw->key_h - 3);
            cairo_stroke(cr);
            cairo_set_line_width(cr, 1.0);
        }
    }

    /* rubber-band selection box */
    if (mw->sel_dragging && mw->sel_moved) {
        double x0 = MIN(mw->sel_x0, mw->sel_x1), x1 = MAX(mw->sel_x0, mw->sel_x1);
        double y0 = MIN(mw->sel_y0, mw->sel_y1), y1 = MAX(mw->sel_y0, mw->sel_y1);
        cairo_set_source_rgba(cr, 0.45, 0.7, 1.0, 0.18);
        cairo_rectangle(cr, x0, y0, x1 - x0, y1 - y0);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 0.55, 0.8, 1.0, 0.9);
        cairo_set_line_width(cr, 1.0);
        cairo_rectangle(cr, floor(x0) + 0.5, floor(y0) + 0.5,
                        floor(x1 - x0), floor(y1 - y0));
        cairo_stroke(cr);
    }

    /* loop-region band (faint amber over the grid) */
    if (jackdaw_engine_has_loop_region()) {
        double fpb = jackdaw_project_frames_per_beat(mw->project,
                                                     jackdaw_engine_get_sample_rate());
        if (fpb > 0.0) {
            off_t ls, le;
            jackdaw_engine_get_loop_range(&ls, &le);
            double x0 = tick_to_x(mw, (double)ls * JACKDAW_PPQ / fpb);
            double x1 = tick_to_x(mw, (double)le * JACKDAW_PPQ / fpb);
            x0 = CLAMP(x0, 0.0, (double)a.width);
            x1 = CLAMP(x1, 0.0, (double)a.width);
            if (x1 > x0) {
                gboolean on = jackdaw_engine_get_loop_enabled();
                cairo_set_source_rgba(cr, 0.95, 0.65, 0.10, on ? 0.10 : 0.05);
                cairo_rectangle(cr, x0, 0, x1 - x0, a.height);
                cairo_fill(cr);
            }
        }
    }

    /* playhead — drawn last so it's on top */
    if (mw->play_tick >= 0.0) {
        double cx = tick_to_x(mw, mw->play_tick);
        if (cx >= 0 && cx < (double)a.width) {
            cairo_set_source_rgba(cr, 1.0, 0.35, 0.0, 1.0);
            cairo_set_line_width(cr, 1.0);
            cairo_move_to(cr, floor(cx) + 0.5, 0);
            cairo_line_to(cr, floor(cx) + 0.5, a.height);
            cairo_stroke(cr);
        }
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
        cairo_set_line_width(cr, 1.0);
        cairo_rectangle(cr, 0, y, a.width, mw->key_h);
        cairo_stroke(cr);
        if (pitch % 12 == 0 && mw->key_h >= 8) {
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

    /* top separator */
    cairo_set_source_rgba(cr, 0.4, 0.4, 0.5, 0.6);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 0, 0.5);
    cairo_line_to(cr, a.width, 0.5);
    cairo_stroke(cr);

    cairo_set_font_size(cr, 9.0);
    guint nc = midi_clip_note_count(mw->clip);
    for (guint i = 0; i < nc; i++) {
        MidiNote *n = midi_clip_note(mw->clip, i);
        double nx = tick_to_x(mw, n->start);
        if (nx < -(double)VEL_BAR_W || nx > a.width) continue;
        double h = (n->velocity / 127.0) * (a.height - 4);
        gboolean selected = sel_is(mw, i);
        if (selected) {                  /* white halo behind selected bars */
            cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
            cairo_set_line_width(cr, 4.0);
            cairo_move_to(cr, floor(nx) + 1, a.height);
            cairo_line_to(cr, floor(nx) + 1, a.height - h);
            cairo_stroke(cr);
        }
        double r, g, b; vel_color(n->velocity, &r, &g, &b);
        /* velocity bar: drawn as a 2px line (same width as original) */
        cairo_set_source_rgb(cr, r, g, b);
        cairo_set_line_width(cr, 2.0);
        cairo_move_to(cr, floor(nx) + 1, a.height);
        cairo_line_to(cr, floor(nx) + 1, a.height - h);
        cairo_stroke(cr);
        /* numeric label above the bar */
        char buf[8]; g_snprintf(buf, sizeof buf, "%d", n->velocity);
        double label_y = a.height - h - 3;
        if (label_y < 11) label_y = 11;
        cairo_set_line_width(cr, 1.0);
        cairo_set_source_rgb(cr, selected ? 1.0 : 0.75,
                                 selected ? 1.0 : 0.75,
                                 selected ? 1.0 : 0.75);
        cairo_move_to(cr, floor(nx) - 2, label_y);
        cairo_show_text(cr, buf);
    }
    return FALSE;
}

static gboolean ruler_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    MidiWindow *mw = data;
    GtkAllocation a; gtk_widget_get_allocation(w, &a);
    cairo_set_source_rgb(cr, 0.11, 0.11, 0.13);
    cairo_paint(cr);

    /* bottom border */
    cairo_set_source_rgba(cr, 0.35, 0.35, 0.45, 0.9);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 0, a.height - 0.5);
    cairo_line_to(cr, a.width, a.height - 0.5);
    cairo_stroke(cr);

    guint bpb = (mw->project && mw->project->beats_per_bar) ? mw->project->beats_per_bar : 4;
    double tick0 = gtk_adjustment_get_value(mw->h_adj);
    long beat0 = (long)(tick0 / JACKDAW_PPQ);
    cairo_set_font_size(cr, 10.0);

    for (long beat = beat0; ; beat++) {
        double x = tick_to_x(mw, (double)beat * JACKDAW_PPQ);
        if (x > a.width) break;
        if (x < 0) continue;
        if (beat % (long)bpb == 0) {
            long bar_num = beat / (long)bpb + 1;
            cairo_set_source_rgba(cr, 0.65, 0.65, 0.78, 0.9);
            cairo_move_to(cr, floor(x) + 0.5, 0);
            cairo_line_to(cr, floor(x) + 0.5, a.height - 1);
            cairo_stroke(cr);
            char buf[16]; g_snprintf(buf, sizeof buf, "%ld", bar_num);
            cairo_set_source_rgb(cr, 0.85, 0.87, 0.95);
            cairo_move_to(cr, floor(x) + 3, a.height - 4);
            cairo_show_text(cr, buf);
        } else {
            cairo_set_source_rgba(cr, 0.38, 0.38, 0.48, 0.55);
            cairo_move_to(cr, floor(x) + 0.5, (double)a.height * 0.55);
            cairo_line_to(cr, floor(x) + 0.5, a.height - 1);
            cairo_stroke(cr);
        }
    }

    /* playhead — orange, same colour as the main timeline */
    if (mw->play_tick >= 0.0) {
        double cx = tick_to_x(mw, mw->play_tick);
        if (cx >= 0 && cx < (double)a.width) {
            cairo_set_source_rgba(cr, 1.0, 0.35, 0.0, 1.0);
            cairo_set_line_width(cr, 1.0);
            cairo_move_to(cr, floor(cx) + 0.5, 0);
            cairo_line_to(cr, floor(cx) + 0.5, a.height);
            cairo_stroke(cr);
        }
    }

    /* loop-region band + end tabs (amber) */
    {
        double fpb = jackdaw_project_frames_per_beat(mw->project,
                                                     jackdaw_engine_get_sample_rate());
        if (fpb > 0.0) {
            off_t ls, le;
            jackdaw_engine_get_loop_range(&ls, &le);
            gboolean has = jackdaw_engine_has_loop_region();
            gboolean on  = jackdaw_engine_get_loop_enabled();
            double x0 = tick_to_x(mw, (double)ls * JACKDAW_PPQ / fpb);
            double x1 = tick_to_x(mw, (double)le * JACKDAW_PPQ / fpb);
            if (has && x1 >= 0 && x0 <= (double)a.width) {
                double bx0 = CLAMP(x0, 0.0, (double)a.width);
                double bx1 = CLAMP(x1, 0.0, (double)a.width);
                cairo_set_source_rgba(cr, 0.95, 0.65, 0.10, on ? 0.30 : 0.15);
                cairo_rectangle(cr, bx0, 0, bx1 - bx0, a.height);
                cairo_fill(cr);
            }
            cairo_set_source_rgb(cr, 0.95, 0.65, 0.10);
            if (x0 >= -5.0 && x0 <= (double)a.width + 5.0) {
                cairo_rectangle(cr, x0, 0, 4, a.height);
                cairo_fill(cr);
            }
            if (x1 >= -5.0 && x1 <= (double)a.width + 5.0) {
                cairo_rectangle(cr, x1 - 4, 0, 4, a.height);
                cairo_fill(cr);
            }
        }
    }
    return FALSE;
}

/* ---- ruler playhead seek / scrub ---- */

/* Move the engine playhead to the tick under ruler x-coordinate. */
static void ruler_seek_to_x(MidiWindow *mw, double x)
{
    double tick = x_to_tick(mw, x);          /* absolute timeline tick */
    if (tick < 0) tick = 0;
    double fpb = jackdaw_project_frames_per_beat(mw->project,
                                                 jackdaw_engine_get_sample_rate());
    if (fpb <= 0.0) return;
    off_t frame = (off_t)(tick * fpb / (double)JACKDAW_PPQ);
    jackdaw_engine_locate(frame);
    /* reflect immediately (the 50 ms timer only updates while engine runs) */
    mw->play_tick = tick;
    gtk_widget_queue_draw(mw->ruler);
    gtk_widget_queue_draw(mw->roll);
}

/* Hit-test the ruler x against the loop tabs (frame edges -> ticks -> x).
 * Returns 1 (start), 2 (end), or 0. When no region exists both tabs sit at
 * frame 0; a hit grabs the end tab so the user drags right to create it. */
static int ruler_loop_hit(MidiWindow *mw, double x)
{
    double fpb = jackdaw_project_frames_per_beat(mw->project,
                                                 jackdaw_engine_get_sample_rate());
    if (fpb <= 0.0) return 0;
    off_t ls, le;
    jackdaw_engine_get_loop_range(&ls, &le);
    double x0 = tick_to_x(mw, (double)ls * JACKDAW_PPQ / fpb);
    double x1 = tick_to_x(mw, (double)le * JACKDAW_PPQ / fpb);
    if (!jackdaw_engine_has_loop_region())
        return (fabs(x - x0) <= 6.0) ? 2 : 0;
    if (fabs(x - x0) <= 6.0) return 1;
    if (fabs(x - x1) <= 6.0) return 2;
    return 0;
}

/* Apply a loop-tab drag to ruler x, snapping to the grid and clamping so the
 * dragged edge cannot cross the other one. */
static void ruler_loop_drag_to(MidiWindow *mw, double x)
{
    double fpb = jackdaw_project_frames_per_beat(mw->project,
                                                 jackdaw_engine_get_sample_rate());
    if (fpb <= 0.0) return;
    double tick = snap_tick(mw, x_to_tick(mw, x));
    if (tick < 0) tick = 0;
    off_t frame = (off_t)(tick * fpb / (double)JACKDAW_PPQ);
    off_t ls, le;
    jackdaw_engine_get_loop_range(&ls, &le);
    if (mw->loop_drag_edge == 1) {            /* start tab */
        if (frame > le) frame = le;
        jackdaw_engine_set_loop_range(frame, le);
    } else {                                  /* end tab */
        if (frame < ls) frame = ls;
        jackdaw_engine_set_loop_range(ls, frame);
    }
    gtk_widget_queue_draw(mw->ruler);
    gtk_widget_queue_draw(mw->roll);
}

static gboolean ruler_press(GtkWidget *w, GdkEventButton *e, gpointer data)
{
    (void)w;
    MidiWindow *mw = data;
    if (e->button != 1) return FALSE;
    int edge = ruler_loop_hit(mw, e->x);
    if (edge) {
        mw->loop_drag_edge = edge;
        ruler_loop_drag_to(mw, e->x);
        return TRUE;
    }
    mw->ruler_dragging = TRUE;
    ruler_seek_to_x(mw, e->x);
    return TRUE;
}

static gboolean ruler_motion(GtkWidget *w, GdkEventMotion *e, gpointer data)
{
    (void)w;
    MidiWindow *mw = data;
    if (mw->loop_drag_edge && (e->state & GDK_BUTTON1_MASK)) {
        ruler_loop_drag_to(mw, e->x);
        return TRUE;
    }
    if (mw->ruler_dragging && (e->state & GDK_BUTTON1_MASK))
        ruler_seek_to_x(mw, e->x);
    return TRUE;
}

static gboolean ruler_release(GtkWidget *w, GdkEventButton *e, gpointer data)
{
    (void)w; (void)e;
    MidiWindow *mw = data;
    mw->loop_drag_edge = 0;
    mw->ruler_dragging = FALSE;
    return TRUE;
}

/* ---- quantize ---- */

/* Snap note starts to the grid.  If any notes are selected, only those are
 * quantized; otherwise every note is.  Always snaps (independent of the Snap
 * toggle, which only governs live drag-editing). */
static void quantize_notes(MidiWindow *mw)
{
    int step = JACKDAW_PPQ / 4;          /* 1/16-note grid */
    guint nc = midi_clip_note_count(mw->clip);
    guint sc = sel_count(mw);
    for (guint i = 0; i < nc; i++) {
        if (sc > 0 && !sel_is(mw, i)) continue;   /* selection-only when any selected */
        MidiNote *n = midi_clip_note(mw->clip, i);
        n->start = (guint32)(floor((double)n->start / step + 0.5) * step);
    }
    mw_commit(mw);
}

/* ---- context menu ---- */

static void mw_ctx_delete_note(GtkMenuItem *item, gpointer data)
{
    (void)item;
    MidiWindow *mw = data;
    if (sel_count(mw) > 0) {              /* delete every highlighted note */
        guint nc = midi_clip_note_count(mw->clip);
        for (int i = (int)nc - 1; i >= 0; i--)   /* high→low keeps indices valid */
            if (sel_is(mw, (guint)i))
                midi_clip_remove_note(mw->clip, (guint)i);
        sel_clear(mw);
        mw->ctx_note_idx = -1;
        mw_commit(mw);
        return;
    }
    if (mw->ctx_note_idx >= 0) {
        midi_clip_remove_note(mw->clip, (guint)mw->ctx_note_idx);
        mw->ctx_note_idx = -1;
        mw_commit(mw);
    }
}

static void mw_ctx_quantize_all(GtkMenuItem *item, gpointer data)
{
    (void)item;
    quantize_notes((MidiWindow *)data);
}

static void mw_ctx_select_all(GtkMenuItem *item, gpointer data)
{
    (void)item;
    sel_all((MidiWindow *)data);
}

static void mw_ctx_clear_loop(GtkMenuItem *item, gpointer data)
{
    (void)item;
    MidiWindow *mw = data;
    jackdaw_engine_set_loop_range(0, 0);
    jackdaw_engine_set_loop_enabled(FALSE);
    gtk_widget_queue_draw(mw->ruler);
    gtk_widget_queue_draw(mw->roll);
}

static void roll_show_context_menu(MidiWindow *mw, GdkEventButton *ev, int note_idx)
{
    mw->ctx_note_idx = note_idx;
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *mi;

    guint sc = sel_count(mw);
    mi = gtk_menu_item_new_with_label(sc > 0 ? "Delete Selected" : "Delete Note");
    gtk_widget_set_sensitive(mi, note_idx >= 0 || sc > 0);
    g_signal_connect(mi, "activate", G_CALLBACK(mw_ctx_delete_note), mw);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    mi = gtk_menu_item_new_with_label("Select All");
    g_signal_connect(mi, "activate", G_CALLBACK(mw_ctx_select_all), mw);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);

    mi = gtk_menu_item_new_with_label(sel_count(mw) > 0
                                      ? "Quantize Selected  [Q]"
                                      : "Quantize All  [Q]");
    g_signal_connect(mi, "activate", G_CALLBACK(mw_ctx_quantize_all), mw);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    mi = gtk_menu_item_new_with_label("Clear Loop Region");
    gtk_widget_set_sensitive(mi, jackdaw_engine_has_loop_region());
    g_signal_connect(mi, "activate", G_CALLBACK(mw_ctx_clear_loop), mw);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);

    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
}

/* ---- roll interaction ---- */

static gboolean roll_press(GtkWidget *w, GdkEventButton *e, gpointer data)
{
    (void)w;
    MidiWindow *mw = data;
    gboolean edge = FALSE;
    int idx = note_at(mw, e->x, e->y, &edge);

    if (e->button == 3) {
        /* Begin a potential rubber-band selection.  If the pointer never
         * moves, the release handler pops up the context menu instead. */
        mw->sel_dragging = TRUE;
        mw->sel_moved    = FALSE;
        mw->sel_x0 = mw->sel_x1 = e->x;
        mw->sel_y0 = mw->sel_y1 = e->y;
        mw->ctx_note_idx = idx;
        return TRUE;
    }
    if (e->button != 1) return FALSE;

    /* With an active selection, a left action operates on the selection
     * rather than creating notes. */
    if (sel_count(mw) > 0) {
        if (idx >= 0 && sel_is(mw, (guint)idx)) {   /* grab the group → move it */
            grp_capture(mw);
            mw->drag_mode = 4;
            mw->drag_note = idx;
            mw->press_x = e->x; mw->press_y = e->y;
            return TRUE;
        }
        sel_clear(mw);                  /* clicked away: just deselect, place nothing */
        sel_redraw(mw);
        return TRUE;
    }

    if (idx < 0) {                        /* empty: add a note */
        MidiNote n;
        n.start    = snap_tick(mw, x_to_tick(mw, e->x));
        n.length   = snap_step(mw) > 1 ? (guint32)snap_step(mw) : JACKDAW_PPQ / 4;
        n.pitch    = (guint8)y_to_pitch(mw, e->y);
        n.velocity = DEFAULT_VEL;
        n.channel  = 0;
        idx = (int)midi_clip_add_note(mw->clip, n);
        mw->drag_mode = 1;
    } else {
        mw->drag_mode = edge ? 2 : 1;
    }
    MidiNote *n = midi_clip_note(mw->clip, (guint)idx);
    mw->drag_note   = idx;
    mw->press_x     = e->x; mw->press_y = e->y;
    mw->orig_start  = n->start; mw->orig_len = n->length; mw->orig_pitch = n->pitch;
    mw_commit(mw);
    return TRUE;
}

static gboolean roll_motion(GtkWidget *w, GdkEventMotion *e, gpointer data)
{
    MidiWindow *mw = data;
    if (mw->sel_dragging) {                   /* right-drag rubber band */
        mw->sel_x1 = e->x; mw->sel_y1 = e->y;
        if (!mw->sel_moved &&
            (fabs(e->x - mw->sel_x0) > 3.0 || fabs(e->y - mw->sel_y0) > 3.0))
            mw->sel_moved = TRUE;
        if (mw->sel_moved) sel_update_box(mw);
        sel_redraw(mw);
        return TRUE;
    }
    if (mw->drag_mode == 0 || mw->drag_note < 0) {
        gboolean edge = FALSE; note_at(mw, e->x, e->y, &edge);
        GdkWindow *gw = gtk_widget_get_window(w);
        if (gw) {
            GdkCursor *c = edge
                ? gdk_cursor_new_from_name(gdk_display_get_default(), "ew-resize") : NULL;
            gdk_window_set_cursor(gw, c);
            if (c) g_object_unref(c);
        }
        return FALSE;
    }
    if (mw->drag_mode == 4) {                 /* move the whole selection */
        double rawdt = (e->x - mw->press_x) * mw->tpx;
        int step = snap_step(mw);
        long dticks = (step > 1)
            ? (long)(floor(rawdt / step + 0.5) * step) : (long)rawdt;
        int dp = (int)floor((e->y - mw->press_y) / mw->key_h + 0.5);
        guint nc = midi_clip_note_count(mw->clip);
        for (guint i = 0; i < nc && i < mw->grp_cap; i++) {
            if (!sel_is(mw, i)) continue;
            MidiNote *gn = midi_clip_note(mw->clip, i);
            long ns = (long)mw->grp_start[i] + dticks;
            gn->start = (guint32)(ns < 0 ? 0 : ns);
            gn->pitch = (guint8)CLAMP((int)mw->grp_pitch[i] - dp, 0, 127);
        }
        mw_commit(mw);
        return TRUE;
    }

    MidiNote *n = midi_clip_note(mw->clip, (guint)mw->drag_note);
    if (!n) { mw->drag_mode = 0; return FALSE; }
    double dt = (e->x - mw->press_x) * mw->tpx;
    if (mw->drag_mode == 1) {
        long ns = (long)mw->orig_start + (long)dt;
        n->start = snap_tick(mw, ns < 0 ? 0.0 : (double)ns);
        int dp = (int)floor((e->y - mw->press_y) / mw->key_h + 0.5);
        n->pitch = (guint8)CLAMP((int)mw->orig_pitch - dp, 0, 127);
    } else if (mw->drag_mode == 2) {
        long nl = (long)mw->orig_len + (long)dt;
        int step = snap_step(mw) > 1 ? snap_step(mw) : 1;
        if (nl < step) nl = step;
        n->length = snap_tick(mw, (double)nl);
        if (n->length < (guint32)step) n->length = (guint32)step;
    }
    mw_commit(mw);
    return TRUE;
}

static gboolean roll_release(GtkWidget *w, GdkEventButton *e, gpointer data)
{
    (void)w;
    MidiWindow *mw = data;
    if (e->button == 3 && mw->sel_dragging) {
        mw->sel_dragging = FALSE;
        if (mw->sel_moved) {                  /* it was a box drag: keep selection */
            mw->sel_moved = FALSE;
            sel_redraw(mw);                   /* drop the box outline, keep highlight */
        } else {                              /* a plain click: open the menu */
            roll_show_context_menu(mw, e, mw->ctx_note_idx);
        }
        return TRUE;
    }
    mw->drag_mode = 0; mw->drag_note = -1;
    return FALSE;
}

/* ---- velocity lane ---- */

/* Find the note whose bar is nearest to x; within VEL_BAR_W px hit zone. */
static int vel_note_at_x(MidiWindow *mw, double x)
{
    int best = -1; double bestd = 1e18;
    guint nc = midi_clip_note_count(mw->clip);
    for (guint i = 0; i < nc; i++) {
        MidiNote *n = midi_clip_note(mw->clip, i);
        double nx = tick_to_x(mw, n->start);
        double d = fabs(nx - x);
        if (d < bestd && d <= (double)VEL_BAR_W) { bestd = d; best = (int)i; }
    }
    return best;
}

static void vel_apply_y(MidiWindow *mw, int note_idx, double y)
{
    GtkAllocation a; gtk_widget_get_allocation(mw->vel, &a);
    int v = (int)((1.0 - CLAMP(y, 0.0, (double)a.height) / (double)a.height) * 127.0 + 0.5);
    v = CLAMP(v, 1, 127);
    if (sel_count(mw) > 0 && sel_is(mw, (guint)note_idx) && note_idx < (int)mw->grp_cap) {
        /* grabbed bar is selected: shift every selected note by the same delta
         * from its captured original, preserving relative dynamics. */
        int delta = v - (int)mw->grp_vel[note_idx];
        guint nc = midi_clip_note_count(mw->clip);
        for (guint i = 0; i < nc && i < mw->grp_cap; i++)
            if (sel_is(mw, i))
                midi_clip_note(mw->clip, i)->velocity =
                    (guint8)CLAMP((int)mw->grp_vel[i] + delta, 1, 127);
    } else {
        MidiNote *n = midi_clip_note(mw->clip, (guint)note_idx);
        if (!n) return;
        n->velocity = (guint8)v;
    }
    mw_commit(mw);
}

static gboolean vel_press(GtkWidget *w, GdkEventButton *e, gpointer data)
{
    (void)w;
    MidiWindow *mw = data;
    if (e->button != 1) return TRUE;
    int idx = vel_note_at_x(mw, e->x);
    if (idx >= 0) {
        mw->drag_mode = 3;
        mw->drag_note = idx;
        grp_capture(mw);            /* snapshot originals for relative group edit */
        vel_apply_y(mw, idx, e->y);
    }
    return TRUE;
}

static gboolean vel_motion(GtkWidget *w, GdkEventMotion *e, gpointer data)
{
    (void)w;
    MidiWindow *mw = data;
    if ((e->state & GDK_BUTTON1_MASK) && mw->drag_mode == 3 && mw->drag_note >= 0)
        vel_apply_y(mw, mw->drag_note, e->y);
    return TRUE;
}

static gboolean vel_release(GtkWidget *w, GdkEventButton *e, gpointer data)
{
    (void)w; (void)e;
    MidiWindow *mw = data;
    if (mw->drag_mode == 3) { mw->drag_mode = 0; mw->drag_note = -1; }
    return FALSE;
}

/* ---- scroll / adjustments ---- */

static void redraw_all(MidiWindow *mw)
{
    gtk_widget_queue_draw(mw->roll);
    gtk_widget_queue_draw(mw->keys);
    gtk_widget_queue_draw(mw->vel);
    gtk_widget_queue_draw(mw->ruler);
}

static gboolean roll_scroll(GtkWidget *w, GdkEventScroll *e, gpointer data)
{
    (void)w;
    MidiWindow *mw = data;
    gdouble dx = 0, dy = 0;
    if (e->direction == GDK_SCROLL_SMOOTH) gdk_event_get_scroll_deltas((GdkEvent *)e, &dx, &dy);
    else if (e->direction == GDK_SCROLL_UP)    dy = -1;
    else if (e->direction == GDK_SCROLL_DOWN)  dy =  1;
    else if (e->direction == GDK_SCROLL_LEFT)  dx = -1;
    else if (e->direction == GDK_SCROLL_RIGHT) dx =  1;

    if (e->state & GDK_CONTROL_MASK) {
        double f = (dy > 0) ? 1.2 : 0.8;
        mw->tpx = CLAMP(mw->tpx * f, 0.5, 200.0);
    } else if (e->state & GDK_SHIFT_MASK) {
        double v = gtk_adjustment_get_value(mw->h_adj) + dy * mw->tpx * 60.0;
        gtk_adjustment_set_value(mw->h_adj, CLAMP(v, 0, gtk_adjustment_get_upper(mw->h_adj)));
    } else {
        double v = gtk_adjustment_get_value(mw->v_adj) + dy * 3.0;
        gtk_adjustment_set_value(mw->v_adj, CLAMP(v, 0, gtk_adjustment_get_upper(mw->v_adj)));
    }
    redraw_all(mw);
    return TRUE;
}

static void adj_changed(GtkAdjustment *a, gpointer data) { (void)a; redraw_all(data); }

/* ---- transport (matches mainwindow style — no recording in MIDI window) ---- */

/* Forward-declared so the 50ms timer can block/unblock the signal. */
static void mw_play_toggled(GtkToggleButton *b, gpointer data);

static void mw_play_toggled(GtkToggleButton *b, gpointer data)
{
    (void)data;
    gboolean on = gtk_toggle_button_get_active(b);
    if (on) jackdaw_engine_start_playback();
    else    jackdaw_engine_stop_playback();
    mw_set_class(GTK_WIDGET(b), "transport-play", on);
}

static void mw_loop_toggled(GtkToggleButton *b, gpointer data)
{
    MidiWindow *mw = data;
    gboolean on = gtk_toggle_button_get_active(b);
    jackdaw_engine_set_loop_enabled(on);
    mw_set_class(GTK_WIDGET(b), "transport-loop", on);
    gtk_widget_queue_draw(mw->ruler);
    gtk_widget_queue_draw(mw->roll);
}

/* Shared helper: stop playback and sync the play toggle (no locate). */
static void mw_stop_transport(MidiWindow *mw)
{
    jackdaw_engine_stop_playback();
    g_signal_handlers_block_by_func(mw->btn_play, mw_play_toggled, mw);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(mw->btn_play), FALSE);
    mw_set_class(mw->btn_play, "transport-play", FALSE);
    g_signal_handlers_unblock_by_func(mw->btn_play, mw_play_toggled, mw);
}

static void mw_pause_cb(GtkButton *b, gpointer data)
{
    (void)b;
    mw_stop_transport((MidiWindow *)data);
}

static void mw_transport_stop_cb(GtkButton *b, gpointer data)
{
    (void)b;
    mw_stop_transport((MidiWindow *)data);
}

/* |◀  — return to zero, stop, scroll roll back to the start. */
static void mw_locate_start_cb(GtkButton *b, gpointer data)
{
    (void)b;
    MidiWindow *mw = data;
    jackdaw_engine_locate(0);
    mw_stop_transport(mw);
    gtk_adjustment_set_value(mw->h_adj, 0.0);
}

static off_t mw_one_frame(void)
{
    jack_nframes_t sr = jackdaw_engine_is_running()
                        ? jackdaw_engine_get_sample_rate() : 48000u;
    return (off_t)(sr / 25);
}

static void mw_step_back_cb(GtkButton *b, gpointer data)
{
    (void)b; (void)data;
    off_t pos  = jackdaw_engine_get_play_pos();
    off_t step = mw_one_frame();
    jackdaw_engine_locate((pos > step) ? pos - step : 0);
}

static void mw_step_fwd_cb(GtkButton *b, gpointer data)
{
    (void)b; (void)data;
    jackdaw_engine_locate(jackdaw_engine_get_play_pos() + mw_one_frame());
}

static void mw_locate_next_cb(GtkButton *b, gpointer data)
{
    (void)b;
    MidiWindow *mw = data;
    off_t cursor = jackdaw_engine_get_play_pos();
    off_t next   = G_MAXINT64;
    guint n = jackdaw_project_track_count(mw->project);
    for (guint i = 0; i < n; i++) {
        JackDawTrack *t = jackdaw_project_get_track(mw->project, i);
        GPtrArray *regions = jackdaw_track_get_regions(t);
        for (guint j = 0; j < regions->len; j++) {
            ClipRegion *r = g_ptr_array_index(regions, j);
            if (r->tl_pos > cursor && r->tl_pos < next) next = r->tl_pos;
            off_t end = clip_region_end(r);
            if (end > cursor && end < next) next = end;
        }
    }
    if (next != G_MAXINT64)
        jackdaw_engine_locate(next);
}

/* 50 ms timer: update playhead, auto-scroll, and sync transport button state. */
static gboolean transport_update(gpointer data)
{
    MidiWindow *mw = data;

    /* --- Sync play-toggle visual state with actual engine state --- */
    gboolean playing = jackdaw_engine_is_playing();
    gboolean btn_on  = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(mw->btn_play));
    if (btn_on != playing) {
        g_signal_handlers_block_by_func(mw->btn_play, mw_play_toggled, mw);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(mw->btn_play), playing);
        mw_set_class(mw->btn_play, "transport-play", playing);
        g_signal_handlers_unblock_by_func(mw->btn_play, mw_play_toggled, mw);
    }

    /* --- Sync loop-toggle with engine loop state (may be toggled elsewhere) --- */
    {
        gboolean loop_on  = jackdaw_engine_get_loop_enabled();
        gboolean lbtn_on  = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(mw->btn_loop));
        if (lbtn_on != loop_on) {
            g_signal_handlers_block_by_func(mw->btn_loop, mw_loop_toggled, mw);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(mw->btn_loop), loop_on);
            mw_set_class(mw->btn_loop, "transport-loop", loop_on);
            g_signal_handlers_unblock_by_func(mw->btn_loop, mw_loop_toggled, mw);
        }
    }

    /* --- Update time display (same as mainwindow's 100ms timer) --- */
    if (jackdaw_engine_is_running()) {
        guint32 sr = jackdaw_engine_get_sample_rate();
        gchar tbuf[64];
        off_t tpos = jackdaw_engine_get_play_pos();
        get_time(sr, tpos, tpos, tbuf, default_timescale_mode);
        gtk_label_set_text(GTK_LABEL(mw->time_label), tbuf);
    }

    if (!jackdaw_engine_is_running()) return G_SOURCE_CONTINUE;

    /* --- Update playhead position --- */
    off_t pos = jackdaw_engine_get_play_pos();
    {
        guint32 sr = jackdaw_engine_get_sample_rate();
        double  fpb = (sr > 0) ? jackdaw_project_frames_per_beat(mw->project, sr) : 0.0;
        mw->play_tick = (fpb > 1.0)
            ? (double)pos / fpb * (double)JACKDAW_PPQ
            : -1.0;
    }

    /* --- Auto-scroll: keep playhead visible while playing --- */
    if (playing && mw->play_tick >= 0.0 && pos != mw->prev_play_pos) {
        mw->prev_play_pos = pos;
        gint view_w = gtk_widget_get_allocated_width(mw->roll);
        if (view_w > 0) {
            double view_ticks = (double)view_w * mw->tpx;
            double h_val      = gtk_adjustment_get_value(mw->h_adj);
            if (mw->play_tick > h_val + view_ticks) {
                double new_val = mw->play_tick - 0.10 * view_ticks;
                if (new_val < 0.0) new_val = 0.0;
                gtk_adjustment_set_value(mw->h_adj, new_val);
                /* adj_changed fires redraw_all via the value-changed signal */
                return G_SOURCE_CONTINUE;
            }
        }
    }

    gtk_widget_queue_draw(mw->roll);
    gtk_widget_queue_draw(mw->ruler);
    return G_SOURCE_CONTINUE;
}

/* ---- keyboard shortcuts ---- */

static gboolean mw_key_press(GtkWidget *w, GdkEventKey *e, gpointer data)
{
    (void)w;
    MidiWindow *mw = data;
    switch (e->keyval) {
    case GDK_KEY_space:
    case GDK_KEY_KP_Space: {
        gboolean active = !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(mw->btn_play));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(mw->btn_play), active);
        return TRUE;
    }
    case GDK_KEY_Home:
    case GDK_KEY_KP_Home:
        mw_locate_start_cb(NULL, mw);
        return TRUE;
    case GDK_KEY_q:
    case GDK_KEY_Q:
        quantize_notes(mw);
        return TRUE;
    case GDK_KEY_a:
    case GDK_KEY_A:
        if (e->state & GDK_CONTROL_MASK) { sel_all(mw); return TRUE; }
        break;
    case GDK_KEY_Escape:
        sel_clear(mw);
        sel_redraw(mw);
        return TRUE;
    default:
        break;
    }
    return FALSE;
}

/* ---- snap toggle ---- */

static void mw_snap_btn_toggled(GtkToggleButton *b, gpointer data)
{
    MidiWindow *mw = data;
    jackdaw_project_set_snap_enabled(mw->project,
                                     gtk_toggle_button_get_active(b));
}

/* ---- window lifecycle ---- */

static gboolean mw_delete(GtkWidget *w, GdkEvent *e, gpointer data)
{
    (void)w; (void)e;
    MidiWindow *mw = data;
    if (mw->update_timer) { g_source_remove(mw->update_timer); mw->update_timer = 0; }
    g_object_set_data(G_OBJECT(mw->track), "midi-window", NULL);
    gtk_widget_destroy(mw->window);
    g_free(mw->sel);
    g_free(mw->grp_start);
    g_free(mw->grp_pitch);
    g_free(mw->grp_vel);
    g_free(mw);
    return TRUE;
}

void jackdaw_midi_window_open(JackDawTrack *track, JackDawProject *project)
{
    g_return_if_fail(JACKDAW_IS_TRACK(track));

    MidiWindow *mw = g_object_get_data(G_OBJECT(track), "midi-window");
    if (mw) {
        gtk_window_present(GTK_WINDOW(mw->window));
        redraw_all(mw);
        return;
    }

    mw = g_new0(MidiWindow, 1);
    mw->track = track; mw->clip = jackdaw_track_get_midi_clip(track);
    mw->project = project;
    mw->tpx = 20.0; mw->key_h = DEFAULT_KEYH;
    mw->drag_note = -1; mw->ctx_note_idx = -1;
    mw->play_tick = -1.0; mw->prev_play_pos = -1;

    mw->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gchar *title = g_strdup_printf("Piano Roll: %s", jackdaw_track_get_name(track));
    gtk_window_set_title(GTK_WINDOW(mw->window), title);
    g_free(title);
    gtk_window_set_default_size(GTK_WINDOW(mw->window), 900, 580);
    g_signal_connect(mw->window, "delete-event",    G_CALLBACK(mw_delete),    mw);
    g_signal_connect(mw->window, "key-press-event", G_CALLBACK(mw_key_press), mw);

    mw->h_adj = gtk_adjustment_new(0, 0, (gdouble)(JACKDAW_PPQ * 4 * 1000),
                                   JACKDAW_PPQ / 4, JACKDAW_PPQ, 0);
    mw->v_adj = gtk_adjustment_new(48, 0, 128, 1, 12, 0);

    /* Outer vertical box: transport toolbar then content grid. */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(mw->window), vbox);

    /* --- Transport toolbar — same buttons/order as mainwindow, no recording --- */
    GtkWidget *tb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(tb), 3);

    GtkWidget *btn_start     = gtk_button_new_with_label("|◀");
    GtkWidget *btn_step_back = gtk_button_new_with_label("|<<");
    GtkWidget *btn_step_fwd  = gtk_button_new_with_label(">>|");
    GtkWidget *btn_next      = gtk_button_new_with_label("▶|");
    mw->btn_play             = gtk_toggle_button_new_with_label("▶");
    mw->btn_pause            = gtk_button_new_with_label("||");
    mw->btn_stop             = gtk_button_new_with_label("■");
    mw->btn_loop             = gtk_toggle_button_new_with_label("⟳");

    gtk_widget_set_tooltip_text(btn_start,     "Return to start  [Home]");
    gtk_widget_set_tooltip_text(btn_step_back, "Step back one frame (25fps)");
    gtk_widget_set_tooltip_text(btn_step_fwd,  "Step forward one frame (25fps)");
    gtk_widget_set_tooltip_text(btn_next,      "Jump to next clip boundary");
    gtk_widget_set_tooltip_text(mw->btn_play,  "Play / Stop  [Space]");
    gtk_widget_set_tooltip_text(mw->btn_pause, "Pause");
    gtk_widget_set_tooltip_text(mw->btn_stop,  "Stop");
    gtk_widget_set_tooltip_text(mw->btn_loop,  "Loop region");

    g_signal_connect(btn_start,     "clicked", G_CALLBACK(mw_locate_start_cb),   mw);
    g_signal_connect(btn_step_back, "clicked", G_CALLBACK(mw_step_back_cb),      mw);
    g_signal_connect(btn_step_fwd,  "clicked", G_CALLBACK(mw_step_fwd_cb),       mw);
    g_signal_connect(btn_next,      "clicked", G_CALLBACK(mw_locate_next_cb),    mw);
    g_signal_connect(mw->btn_play,  "toggled", G_CALLBACK(mw_play_toggled),      mw);
    g_signal_connect(mw->btn_pause, "clicked", G_CALLBACK(mw_pause_cb),          mw);
    g_signal_connect(mw->btn_stop,  "clicked", G_CALLBACK(mw_transport_stop_cb), mw);
    g_signal_connect(mw->btn_loop,  "toggled", G_CALLBACK(mw_loop_toggled),      mw);

    gtk_box_pack_start(GTK_BOX(tb), btn_start,     FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tb), btn_step_back, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tb), btn_step_fwd,  FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tb), btn_next,      FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tb),
                       gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(tb), mw->btn_play,  FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tb), mw->btn_pause, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tb), mw->btn_stop,  FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tb), mw->btn_loop,  FALSE, FALSE, 0);

    mw->time_label = gtk_label_new("00:00.0");
    gtk_style_context_add_class(gtk_widget_get_style_context(mw->time_label),
                                "transport-time");
    gtk_widget_set_size_request(mw->time_label, 160, -1);
    gtk_box_pack_start(GTK_BOX(tb), mw->time_label, FALSE, FALSE, 8);

    GtkWidget *btn_snap = gtk_toggle_button_new_with_label("Snap");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn_snap),
                                 mw->project && mw->project->snap_enabled);
    gtk_widget_set_tooltip_text(btn_snap, "Snap notes to grid");
    g_signal_connect(btn_snap, "toggled", G_CALLBACK(mw_snap_btn_toggled), mw);
    gtk_box_pack_end(GTK_BOX(tb), btn_snap, FALSE, FALSE, 4);

    gtk_box_pack_start(GTK_BOX(vbox), tb, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);

    /* --- Content grid ---
     *  col 0 = keyboard (KEY_W fixed)
     *  col 1 = ruler / note grid / velocity / h-scroll (expanding)
     *  col 2 = vertical scrollbar
     *  row 0 = ruler (col 1 only)
     *  row 1 = keys + roll + vscroll
     *  row 2 = velocity lane
     *  row 3 = horizontal scrollbar
     */
    GtkWidget *grid = gtk_grid_new();
    gtk_box_pack_start(GTK_BOX(vbox), grid, TRUE, TRUE, 0);

    mw->ruler = gtk_drawing_area_new();
    gtk_widget_set_size_request(mw->ruler, -1, RULER_H);
    gtk_widget_set_hexpand(mw->ruler, TRUE);
    gtk_widget_add_events(mw->ruler, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                          GDK_POINTER_MOTION_MASK);

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
    gtk_widget_add_events(mw->vel, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                          GDK_POINTER_MOTION_MASK);

    GtkWidget *vscroll = gtk_scrollbar_new(GTK_ORIENTATION_VERTICAL, mw->v_adj);
    GtkWidget *hscroll = gtk_scrollbar_new(GTK_ORIENTATION_HORIZONTAL, mw->h_adj);

    gtk_grid_attach(GTK_GRID(grid), mw->ruler,  1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), mw->keys,   0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), mw->roll,   1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), vscroll,    2, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), mw->vel,    1, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), hscroll,    1, 3, 1, 1);

    g_signal_connect(mw->roll,  "draw",                 G_CALLBACK(roll_draw),    mw);
    g_signal_connect(mw->keys,  "draw",                 G_CALLBACK(keys_draw),    mw);
    g_signal_connect(mw->vel,   "draw",                 G_CALLBACK(vel_draw),     mw);
    g_signal_connect(mw->ruler, "draw",                 G_CALLBACK(ruler_draw),   mw);
    g_signal_connect(mw->ruler, "button-press-event",   G_CALLBACK(ruler_press),  mw);
    g_signal_connect(mw->ruler, "button-release-event", G_CALLBACK(ruler_release),mw);
    g_signal_connect(mw->ruler, "motion-notify-event",  G_CALLBACK(ruler_motion), mw);
    g_signal_connect(mw->roll,  "button-press-event",   G_CALLBACK(roll_press),   mw);
    g_signal_connect(mw->roll,  "button-release-event", G_CALLBACK(roll_release), mw);
    g_signal_connect(mw->roll,  "motion-notify-event",  G_CALLBACK(roll_motion),  mw);
    g_signal_connect(mw->roll,  "scroll-event",         G_CALLBACK(roll_scroll),  mw);
    g_signal_connect(mw->vel,   "button-press-event",   G_CALLBACK(vel_press),    mw);
    g_signal_connect(mw->vel,   "motion-notify-event",  G_CALLBACK(vel_motion),   mw);
    g_signal_connect(mw->vel,   "button-release-event", G_CALLBACK(vel_release),  mw);
    g_signal_connect(mw->h_adj, "value-changed",        G_CALLBACK(adj_changed),  mw);
    g_signal_connect(mw->v_adj, "value-changed",        G_CALLBACK(adj_changed),  mw);

    /* 50 ms timer — same rate as the main timeline's playhead/scroll timer. */
    mw->update_timer = g_timeout_add(50, transport_update, mw);

    g_object_set_data(G_OBJECT(track), "midi-window", mw);
    gtk_widget_show_all(mw->window);
}
