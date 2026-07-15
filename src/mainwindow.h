#ifndef MAINWINDOW_H_INCLUDED
#define MAINWINDOW_H_INCLUDED

#include <gtk/gtk.h>
#include "project.h"
#include "timeline.h"

G_BEGIN_DECLS

#define JACKDAW_TYPE_MAIN_WINDOW \
    (jackdaw_main_window_get_type())
#define JACKDAW_MAIN_WINDOW(o) \
    (G_TYPE_CHECK_INSTANCE_CAST(o, JACKDAW_TYPE_MAIN_WINDOW, JackDawMainWindow))
#define JACKDAW_IS_MAIN_WINDOW(o) \
    (G_TYPE_CHECK_INSTANCE_TYPE(o, JACKDAW_TYPE_MAIN_WINDOW))

typedef struct _JackDawMainWindow      JackDawMainWindow;
typedef struct _JackDawMainWindowClass JackDawMainWindowClass;

struct _JackDawMainWindow {
    GtkWindow        parent_instance;

    JackDawProject  *project;       /* strong ref — window owns project */
    JackDawTimeline *timeline;

    /* Transport toolbar widgets */
    GtkWidget *play_button;
    GtkWidget *record_button;
    GtkWidget *record_glyph;    /* GtkDrawingArea child of record_button (Cairo glyph) */
    GtkWidget *loop_button;
    GtkWidget *time_label;

    /* Mixer dock */
    GtkWidget *mixer;
    GtkWidget *paned;
    GtkWidget *mixer_window;   /* top-level host when "in window" mode is on; NULL until created */
    GtkWidget *mixer_button;   /* the "Mixer" toggle button, so menu/window can sync it */
    gboolean   mixer_in_window;/* current mode */

    GtkWidget *metro_window;   /* metronome settings window; NULL until created */
    GtkWidget *countin_window; /* count-in settings window; NULL until created */
    GtkWidget *io_window;      /* Inputs/Outputs settings window; NULL until created */
    GtkWidget *midictl_window; /* MIDI control-surface window; NULL until created */

    guint      track_counter;   /* incremented for "Track N" names */
    guint      transport_timer; /* 100 ms source id for time display */
    guint      midictl_timer;   /* control-surface drain timer source id */
};

struct _JackDawMainWindowClass {
    GtkWindowClass parent_class;
};

GType      jackdaw_main_window_get_type(void);
GtkWidget *jackdaw_main_window_new(JackDawProject *project);

G_END_DECLS

#endif /* MAINWINDOW_H_INCLUDED */
