#ifndef TIMELINE_H_INCLUDED
#define TIMELINE_H_INCLUDED

#include <gtk/gtk.h>
#include "track.h"
#include "project.h"

G_BEGIN_DECLS

#define TIMELINE_HEADER_WIDTH      180  /* px: fixed-width track name column */
#define TIMELINE_TRACK_HEIGHT       80  /* px: default height per track row  */
#define TIMELINE_RULER_HEIGHT       28  /* px: time ruler                    */
#define TIMELINE_TRACK_MIN_HEIGHT   40  /* px: minimum resizable height      */
#define TIMELINE_TRACK_MAX_HEIGHT  600  /* px: maximum resizable height      */
#define TIMELINE_RESIZE_HANDLE_H     5  /* px: drag handle below each track  */

/* ========================================================================
 * JackDawTimeRuler — time axis with tick marks and playhead cursor
 * ======================================================================== */

#define JACKDAW_TYPE_TIME_RULER \
    (jackdaw_time_ruler_get_type())
#define JACKDAW_TIME_RULER(o) \
    (G_TYPE_CHECK_INSTANCE_CAST(o, JACKDAW_TYPE_TIME_RULER, JackDawTimeRuler))
#define JACKDAW_IS_TIME_RULER(o) \
    (G_TYPE_CHECK_INSTANCE_TYPE(o, JACKDAW_TYPE_TIME_RULER))

typedef struct _JackDawTimeRuler      JackDawTimeRuler;
typedef struct _JackDawTimeRulerClass JackDawTimeRulerClass;

struct _JackDawTimeRuler {
    GtkDrawingArea  parent_instance;
    GtkAdjustment  *time_adj;    /* value = start sample (gdouble) */
    GtkAdjustment  *zoom_adj;    /* value = samples per pixel (gdouble) */
    GtkAdjustment  *cursor_adj;  /* value = transport playhead in samples */
    JackDawProject *project;     /* weak ref — for bars/beats ruler mode */
    guint32         sample_rate;
};

struct _JackDawTimeRulerClass {
    GtkDrawingAreaClass parent_class;
};

GType      jackdaw_time_ruler_get_type(void);
GtkWidget *jackdaw_time_ruler_new(GtkAdjustment *time_adj,
                                   GtkAdjustment *zoom_adj,
                                   GtkAdjustment *cursor_adj,
                                   guint32        sample_rate);

/* ========================================================================
 * JackDawWaveView — per-track waveform drawing area
 * ======================================================================== */

#define JACKDAW_TYPE_WAVE_VIEW \
    (jackdaw_wave_view_get_type())
#define JACKDAW_WAVE_VIEW(o) \
    (G_TYPE_CHECK_INSTANCE_CAST(o, JACKDAW_TYPE_WAVE_VIEW, JackDawWaveView))
#define JACKDAW_IS_WAVE_VIEW(o) \
    (G_TYPE_CHECK_INSTANCE_TYPE(o, JACKDAW_TYPE_WAVE_VIEW))

typedef struct _JackDawWaveView      JackDawWaveView;
typedef struct _JackDawWaveViewClass JackDawWaveViewClass;
typedef struct _JackDawTimeline      JackDawTimeline;

struct _JackDawWaveView {
    GtkDrawingArea  parent_instance;
    JackDawTrack   *track;       /* strong ref */
    GtkAdjustment  *time_adj;    /* strong ref */
    GtkAdjustment  *zoom_adj;    /* strong ref */
    GtkAdjustment  *cursor_adj;  /* strong ref — transport playhead */
    JackDawProject *project;     /* weak ref — for tempo/grid */
    JackDawTimeline *timeline;   /* weak ref — for shared selection */
    gboolean        focused;
};

struct _JackDawWaveViewClass {
    GtkDrawingAreaClass parent_class;
};

GType      jackdaw_wave_view_get_type(void);
GtkWidget *jackdaw_wave_view_new(JackDawTrack  *track,
                                  GtkAdjustment *time_adj,
                                  GtkAdjustment *zoom_adj,
                                  GtkAdjustment *cursor_adj);
void       jackdaw_wave_view_set_focused(JackDawWaveView *wv, gboolean focused);
void       jackdaw_wave_view_invalidate (JackDawWaveView *wv);

/* ========================================================================
 * JackDawTimeline — composite vertical GtkBox containing ruler + track rows
 * ======================================================================== */

#define JACKDAW_TYPE_TIMELINE \
    (jackdaw_timeline_get_type())
#define JACKDAW_TIMELINE(o) \
    (G_TYPE_CHECK_INSTANCE_CAST(o, JACKDAW_TYPE_TIMELINE, JackDawTimeline))
#define JACKDAW_IS_TIMELINE(o) \
    (G_TYPE_CHECK_INSTANCE_TYPE(o, JACKDAW_TYPE_TIMELINE))

typedef struct _JackDawTimelineClass JackDawTimelineClass;

struct _JackDawTimeline {
    GtkBox           parent_instance;

    JackDawProject  *project;       /* weak ref */
    GtkAdjustment   *time_adj;      /* owned — start sample */
    GtkAdjustment   *zoom_adj;      /* owned — samples per pixel */
    GtkAdjustment   *cursor_adj;    /* owned — transport playhead in samples */

    JackDawTimeRuler *ruler;         /* child widget */
    GtkWidget        *tracks_scroll; /* GtkScrolledWindow child */
    GtkWidget        *tracks_box;    /* GtkBox vertical inside scroll */

    JackDawTrack     *focused_track; /* weak ref; NULL when nothing focused */

    /* Shared selection range (timeline frames). sel_active = a region is set. */
    gboolean          sel_active;
    gboolean          selecting;   /* button1 drag in progress */
    off_t             sel_start;
    off_t             sel_end;

    /* Right-click context: track + timeline frame under the pointer */
    JackDawTrack     *menu_track;
    off_t             menu_frame;

    /* Per-track region-edit undo/redo stacks (GHashTable track→GQueue of
     * GPtrArray* region-list snapshots). */
    GHashTable       *undo_stacks;
    GHashTable       *redo_stacks;

    GtkWidget        *hscroll;     /* horizontal scrollbar bound to time_adj */

    /* Keeps ruler spacer and all track strips at the same width automatically */
    GtkSizeGroup     *header_size_group;

    /* JackDawTrack* → JackDawWaveView* */
    GHashTable       *wave_views;

    guint             update_timer;   /* 50 ms GSource id */
    off_t             prev_play_pos;  /* detects playhead motion for auto-scroll */

    /* Ruler drag-playhead state */
    gboolean          ruler_drag_active;
    gdouble           ruler_drag_last_x;  /* last pointer x within the ruler */
    guint             ruler_drag_scroll;  /* GSource id for edge auto-scroll */
};

struct _JackDawTimelineClass {
    GtkBoxClass parent_class;

    void (*track_focused)   (JackDawTimeline *tl, JackDawTrack *track);
    void (*position_changed)(JackDawTimeline *tl, gint64        sample);
};

GType         jackdaw_timeline_get_type(void);
GtkWidget    *jackdaw_timeline_new(JackDawProject *project);

void          jackdaw_timeline_add_track   (JackDawTimeline *tl, JackDawTrack *track);
void          jackdaw_timeline_remove_track(JackDawTimeline *tl, JackDawTrack *track);

JackDawTrack *jackdaw_timeline_get_focused(JackDawTimeline *tl);
void          jackdaw_timeline_zoom_in    (JackDawTimeline *tl);
void          jackdaw_timeline_zoom_out   (JackDawTimeline *tl);
void          jackdaw_timeline_set_cursor (JackDawTimeline *tl, off_t sample);

/* Region editing (operate on the focused track / current selection) */
void          jackdaw_timeline_split_at_cursor(JackDawTimeline *tl);
void          jackdaw_timeline_undo           (JackDawTimeline *tl);
void          jackdaw_timeline_redo           (JackDawTimeline *tl);

/* Redraw every track's wave view (e.g. after a timing or selection change). */
void          jackdaw_timeline_redraw_all(JackDawTimeline *tl);

G_END_DECLS

#endif /* TIMELINE_H_INCLUDED */
