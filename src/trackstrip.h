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

    GtkWidget    *label;         /* track name */
    GtkWidget    *btn_arm;
    GtkWidget    *btn_mute;
    GtkWidget    *btn_solo;

    GtkWidget    *vol_knob;      /* GtkDrawingArea; KnobData via g_object_set_data */
    GtkWidget    *pan_knob;

    GtkWidget    *input_combo;   /* GtkComboBox backed by input_store */
    GtkListStore *input_store;

    gboolean      suppress_update;
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

G_END_DECLS

#endif /* TRACKSTRIP_H_INCLUDED */
