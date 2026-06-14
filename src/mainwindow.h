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
    GtkWidget *loop_button;
    GtkWidget *time_label;

    /* Mixer dock */
    GtkWidget *mixer;
    GtkWidget *paned;

    guint      track_counter;   /* incremented for "Track N" names */
    guint      transport_timer; /* 100 ms source id for time display */
};

struct _JackDawMainWindowClass {
    GtkWindowClass parent_class;
};

GType      jackdaw_main_window_get_type(void);
GtkWidget *jackdaw_main_window_new(JackDawProject *project);

G_END_DECLS

#endif /* MAINWINDOW_H_INCLUDED */
