#ifndef TRACKSTRIP_H_INCLUDED
#define TRACKSTRIP_H_INCLUDED

#include <gtk/gtk.h>
#include "track.h"
#include "project.h"

G_BEGIN_DECLS

/* ========================================================================
 * JackDawTrackStrip — 180px-wide track header widget (fits in 80px height)
 *
 * Layout (vertical):
 *   Row 1: [A][M][S] toggle buttons + track name label
 *   Row 2: Vol knob (Cairo) + Pan knob (Cairo)
 *   Row 3: Single input combo (audio and MIDI sources grouped with separator)
 * ======================================================================== */

#define JACKDAW_TYPE_TRACK_STRIP \
    (jackdaw_track_strip_get_type())
#define JACKDAW_TRACK_STRIP(o) \
    (G_TYPE_CHECK_INSTANCE_CAST(o, JACKDAW_TYPE_TRACK_STRIP, JackDawTrackStrip))
#define JACKDAW_IS_TRACK_STRIP(o) \
    (G_TYPE_CHECK_INSTANCE_TYPE(o, JACKDAW_TYPE_TRACK_STRIP))

typedef struct _JackDawTrackStrip      JackDawTrackStrip;
typedef struct _JackDawTrackStripClass JackDawTrackStripClass;

/* Columns in the input combo GtkListStore */
enum {
    ICOL_TEXT = 0,      /* display text */
    ICOL_PORT,          /* JACK port name; NULL for headers / None / separators */
    ICOL_IS_AUDIO,      /* TRUE = audio port; FALSE = MIDI or non-port row */
    ICOL_IS_SEP,        /* TRUE = GTK row separator (invisible divider) */
    ICOL_SENSITIVE,     /* FALSE = header row (shown grayed, not selectable) */
    ICOL_COUNT
};

struct _JackDawTrackStrip {
    GtkBox parent_instance;

    JackDawTrack   *track;    /* strong ref */
    JackDawProject *project;  /* strong ref */

    GtkWidget    *name_entry;    /* editable track name (GtkEntry) */
    GtkWidget    *btn_arm;
    GtkWidget    *btn_mute;
    GtkWidget    *btn_solo;
    GtkWidget    *btn_mono;      /* mono/stereo record toggle */
    GtkWidget    *btn_fx;        /* FX window toggle */

    GtkWidget    *ctrl_row;      /* [A][M][S][Mo][Fx] + vol/pan row */
    GtkWidget    *vol_knob;      /* GtkDrawingArea; KnobData via g_object_set_data */
    GtkWidget    *pan_knob;

    GtkWidget    *input_combo;   /* GtkComboBox backed by input_store */
    GtkListStore *input_store;

    GtkWidget    *vu_meter;      /* GtkDrawingArea: L/R level bars */
    gfloat        vu_peak_L;
    gfloat        vu_peak_R;
    guint         vu_timer;

    gboolean      suppress_update;
    gboolean      self_update;   /* this strip is the source of a track change */
};

struct _JackDawTrackStripClass {
    GtkBoxClass parent_class;
};

GType      jackdaw_track_strip_get_type(void);

GtkWidget *jackdaw_track_strip_new(JackDawTrack   *track,
                                    JackDawProject *project);

/* Rebuild input combo from current JACK port state.
 * Called automatically when project emits "ports-changed". */
void jackdaw_track_strip_refresh_ports(JackDawTrackStrip *strip);

/* Adapt the strip to the available track-row height: progressively hide the
 * input combo, then the control row + VU meter, so the row can shrink down to
 * just the track name. `content_h` is the row height (excluding resize handle). */
void jackdaw_track_strip_set_height(JackDawTrackStrip *strip, gint content_h);

/* The VU meter drawing area — a click-inert column the timeline uses as an
 * always-available grip for drag-to-reorder. */
GtkWidget *jackdaw_track_strip_get_vu_meter(JackDawTrackStrip *strip);

G_END_DECLS

#endif /* TRACKSTRIP_H_INCLUDED */
