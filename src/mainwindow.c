#define _GNU_SOURCE
#include <config.h>
#include <string.h>

#include "mainwindow.h"
#include "jackdaw-engine.h"
#include "audio_clip.h"
#include "main.h"
#include "um.h"

G_DEFINE_TYPE(JackDawMainWindow, jackdaw_main_window, GTK_TYPE_WINDOW)

/* ---- helpers ---- */

static JackDawTimeline *mw_timeline(GtkWidget *widget)
{
    return JACKDAW_MAIN_WINDOW(widget)->timeline;
}

/* ---- File menu ---- */

static void mw_new_project_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    guint n = jackdaw_project_track_count(win->project);
    while (n-- > 0) {
        JackDawTrack *t = jackdaw_project_get_track(win->project, 0);
        jackdaw_engine_remove_track(t);
        jackdaw_project_remove_track(win->project, t);
    }
    win->track_counter = 0;
    jackdaw_project_set_file(win->project, NULL);
}

static void mw_load_file_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);

    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Load Audio File",
        GTK_WINDOW(win),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Open",   GTK_RESPONSE_ACCEPT,
        NULL);

    GtkFileFilter *ff = gtk_file_filter_new();
    gtk_file_filter_set_name(ff, "Audio files");
    gtk_file_filter_add_mime_type(ff, "audio/*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), ff);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        gchar *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (path) {
            GError    *err  = NULL;
            AudioClip *clip = audio_clip_new(path, &err);
            if (!clip) {
                gchar *msg = g_strdup_printf("Could not load file:\n%s\n%s",
                                             path,
                                             err ? err->message : "unknown error");
                user_error(msg);
                g_free(msg);
                if (err) g_error_free(err);
            } else {
                win->track_counter++;
                gchar *name = g_strdup_printf("Track %u", win->track_counter);
                /* clip ownership transferred to track */
                JackDawTrack *t = jackdaw_track_new(name, clip);
                g_free(name);

                if (jackdaw_engine_add_track(t)) {
                    user_error("Engine: could not add track (slot limit reached)");
                    g_object_unref(t);
                } else {
                    jackdaw_project_add_track(win->project, t);
                    g_object_unref(t);
                }
            }
            g_free(path);
        }
    }
    gtk_widget_destroy(dlg);
}

static void mw_quit_cb(GtkMenuItem *item, gpointer data)
{
    (void)item; (void)data;
    gtk_main_quit();
}

/* ---- Track menu ---- */

static void mw_add_track_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);

    win->track_counter++;
    gchar *name = g_strdup_printf("Track %u", win->track_counter);
    JackDawTrack *t = jackdaw_track_new(name, NULL);
    g_free(name);

    if (jackdaw_engine_add_track(t)) {
        user_error("Engine: could not add track (slot limit reached)");
        g_object_unref(t);
        return;
    }
    jackdaw_project_add_track(win->project, t);
    g_object_unref(t);
}

static void mw_remove_track_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    JackDawTrack *t = jackdaw_timeline_get_focused(win->timeline);
    if (!t) return;
    jackdaw_engine_remove_track(t);
    jackdaw_project_remove_track(win->project, t);
}

/* ---- Transport ---- */

static void mw_transport_play_cb(GtkWidget *widget, gpointer data)
{
    (void)data;
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)))
        jackdaw_engine_start_playback();
    else
        jackdaw_engine_stop_playback();
}

static void mw_pause_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    jackdaw_engine_stop_playback();
    jackdaw_engine_stop_recording();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(win->play_button),   FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(win->record_button), FALSE);
}

static void mw_transport_stop_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    jackdaw_engine_stop_playback();
    jackdaw_engine_stop_recording();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(win->play_button),   FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(win->record_button), FALSE);
}

static void mw_transport_record_cb(GtkWidget *widget, gpointer data)
{
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget))) {
        jackdaw_engine_start_recording();
        /* start_recording sets ENGINE_PLAYING; keep the play button in sync */
        if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(win->play_button)))
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(win->play_button), TRUE);
    } else {
        jackdaw_engine_stop_recording();
    }
}

static void mw_locate_start_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    jackdaw_engine_locate(0);
    jackdaw_timeline_set_cursor(win->timeline, 0);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(win->play_button),   FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(win->record_button), FALSE);
}

/* ---- Edit menu — undo/redo (Phase 4+ when editing is added) ---- */

static void mw_undo_cb(GtkMenuItem *item, gpointer data)
{
    (void)item; (void)data;
    /* Placeholder: edit operations will be added in a later phase */
}

static void mw_redo_cb(GtkMenuItem *item, gpointer data)
{
    (void)item; (void)data;
}

/* ---- View menu ---- */

static void mw_zoom_in_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    jackdaw_timeline_zoom_in(mw_timeline(GTK_WIDGET(data)));
}

static void mw_zoom_out_cb(GtkMenuItem *item, gpointer data)
{
    (void)item;
    jackdaw_timeline_zoom_out(mw_timeline(GTK_WIDGET(data)));
}

/* ---- Timeline position-changed signal ---- */

static void mw_on_position_changed(JackDawTimeline *tl, gint64 sample,
                                    gpointer data)
{
    (void)tl;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(data);
    guint32 sr = jackdaw_engine_is_running()
                 ? (guint32)jackdaw_engine_get_sample_rate()
                 : 48000u;
    gchar tbuf[64];
    get_time(sr, (off_t)sample, (off_t)sample, tbuf, default_timescale_mode);
    gtk_label_set_text(GTK_LABEL(win->time_label), tbuf);
}

/* ---- 100 ms transport display timer ---- */

static gboolean mw_transport_timer(gpointer data)
{
    JackDawMainWindow *win = data;
    if (!JACKDAW_IS_MAIN_WINDOW(win)) return G_SOURCE_REMOVE;

    guint32 sr = jackdaw_engine_is_running()
                 ? (guint32)jackdaw_engine_get_sample_rate()
                 : 48000u;
    off_t pos = jackdaw_engine_get_play_pos();
    gchar tbuf[64];
    get_time(sr, pos, pos, tbuf, default_timescale_mode);
    gtk_label_set_text(GTK_LABEL(win->time_label), tbuf);
    return G_SOURCE_CONTINUE;
}

/* ---- Key press (Space = play/stop, Home = locate start) ---- */

static gboolean mw_key_press(GtkWidget *widget, GdkEventKey *event,
                               gpointer data)
{
    (void)data;
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(widget);

    switch (event->keyval) {
    case GDK_KEY_space:
    case GDK_KEY_KP_Space: {
        gboolean active =
            !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(win->play_button));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(win->play_button), active);
        return TRUE;
    }
    case GDK_KEY_Home:
    case GDK_KEY_KP_Home:
        mw_locate_start_cb(NULL, win);
        return TRUE;
    default:
        break;
    }
    return FALSE;
}

/* ---- Delete-event → quit gtk_main loop ---- */

static gboolean mw_delete_event(GtkWidget *widget, GdkEvent *event,
                                  gpointer data)
{
    (void)widget; (void)event; (void)data;
    gtk_main_quit();
    return FALSE;
}

/* ---- Menu construction helper ---- */

static GtkWidget *make_submenu_item(GtkWidget *menubar, const gchar *label)
{
    GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);
    GtkWidget *menu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), item);
    return menu;
}

static void menu_item(GtkWidget *menu, const gchar *label,
                       GCallback cb, gpointer data,
                       guint accel_key, GdkModifierType accel_mods,
                       GtkAccelGroup *ag)
{
    GtkWidget *item;
    if (!label) {
        item = gtk_separator_menu_item_new();
    } else {
        item = gtk_menu_item_new_with_mnemonic(label);
        if (cb) g_signal_connect(item, "activate", cb, data);
        if (accel_key && ag) {
            gtk_widget_add_accelerator(item, "activate", ag,
                                       accel_key, accel_mods,
                                       GTK_ACCEL_VISIBLE);
        }
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}

/* ---- GObject boilerplate ---- */

static void jackdaw_main_window_finalize(GObject *obj)
{
    JackDawMainWindow *win = JACKDAW_MAIN_WINDOW(obj);
    if (win->transport_timer) {
        g_source_remove(win->transport_timer);
        win->transport_timer = 0;
    }
    g_object_unref(win->project);
    G_OBJECT_CLASS(jackdaw_main_window_parent_class)->finalize(obj);
}

static void jackdaw_main_window_class_init(JackDawMainWindowClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = jackdaw_main_window_finalize;
}

static void jackdaw_main_window_init(JackDawMainWindow *win)
{
    win->project         = NULL;
    win->timeline        = NULL;
    win->play_button     = NULL;
    win->record_button   = NULL;
    win->time_label      = NULL;
    win->track_counter   = 0;
    win->transport_timer = 0;
}

/* ---- Constructor ---- */

GtkWidget *jackdaw_main_window_new(JackDawProject *project)
{
    g_return_val_if_fail(JACKDAW_IS_PROJECT(project), NULL);

    JackDawMainWindow *win =
        g_object_new(JACKDAW_TYPE_MAIN_WINDOW, NULL);

    win->project = g_object_ref(project);

    gtk_window_set_title(GTK_WINDOW(win), "JackDAW 0.1.0");
    gtk_window_set_default_size(GTK_WINDOW(win), 1200, 700);
    g_signal_connect(win, "delete-event", G_CALLBACK(mw_delete_event), NULL);
    g_signal_connect(win, "key-press-event", G_CALLBACK(mw_key_press), NULL);

    GtkAccelGroup *ag = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(win), ag);
    g_object_unref(ag);

    /* ---- Root vbox ---- */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    /* ---- Menu bar ---- */
    GtkWidget *menubar = gtk_menu_bar_new();
    gtk_box_pack_start(GTK_BOX(vbox), menubar, FALSE, FALSE, 0);

    /* File */
    GtkWidget *m = make_submenu_item(menubar, "_File");
    menu_item(m, "_Load File as New Track…",
              G_CALLBACK(mw_load_file_cb), win, 0, 0, ag);
    menu_item(m, NULL, NULL, NULL, 0, 0, ag);
    menu_item(m, "_New Session",
              G_CALLBACK(mw_new_project_cb), win,
              GDK_KEY_n, GDK_CONTROL_MASK, ag);
    menu_item(m, NULL, NULL, NULL, 0, 0, ag);
    menu_item(m, "_Quit",
              G_CALLBACK(mw_quit_cb), win,
              GDK_KEY_q, GDK_CONTROL_MASK, ag);

    /* Track */
    m = make_submenu_item(menubar, "_Track");
    menu_item(m, "_Add Empty Track",
              G_CALLBACK(mw_add_track_cb), win, 0, 0, ag);
    menu_item(m, "_Remove Focused Track",
              G_CALLBACK(mw_remove_track_cb), win, 0, 0, ag);

    /* Transport */
    m = make_submenu_item(menubar, "T_ransport");
    menu_item(m, "_Play / Stop  [Space]",
              NULL, NULL, 0, 0, ag);
    menu_item(m, "_Stop",
              G_CALLBACK(mw_transport_stop_cb), win, 0, 0, ag);
    menu_item(m, "Locate to _Start  [Home]",
              G_CALLBACK(mw_locate_start_cb), win, 0, 0, ag);

    /* Edit */
    m = make_submenu_item(menubar, "_Edit");
    menu_item(m, "_Undo",
              G_CALLBACK(mw_undo_cb), win,
              GDK_KEY_z, GDK_CONTROL_MASK, ag);
    menu_item(m, "_Redo",
              G_CALLBACK(mw_redo_cb), win,
              GDK_KEY_y, GDK_CONTROL_MASK, ag);

    /* View */
    m = make_submenu_item(menubar, "_View");
    menu_item(m, "Zoom _In   [Ctrl++]",
              G_CALLBACK(mw_zoom_in_cb), win,
              GDK_KEY_equal, GDK_CONTROL_MASK, ag);
    menu_item(m, "Zoom _Out  [Ctrl+-]",
              G_CALLBACK(mw_zoom_out_cb), win,
              GDK_KEY_minus, GDK_CONTROL_MASK, ag);

    /* ---- Transport toolbar ---- */
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(toolbar), 3);
    gtk_box_pack_start(GTK_BOX(vbox), toolbar, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 0);

    GtkWidget *btn_start = gtk_button_new_with_label("|◀");
    g_signal_connect(btn_start, "clicked",
                     G_CALLBACK(mw_locate_start_cb), win);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_start, FALSE, FALSE, 0);

    win->play_button = gtk_toggle_button_new_with_label("▶");
    g_signal_connect(win->play_button, "toggled",
                     G_CALLBACK(mw_transport_play_cb), win);
    gtk_box_pack_start(GTK_BOX(toolbar), win->play_button, FALSE, FALSE, 0);

    GtkWidget *btn_pause = gtk_button_new_with_label("⏸");
    g_signal_connect(btn_pause, "clicked", G_CALLBACK(mw_pause_cb), win);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_pause, FALSE, FALSE, 0);

    GtkWidget *btn_stop = gtk_button_new_with_label("■");
    g_signal_connect(btn_stop, "clicked",
                     G_CALLBACK(mw_transport_stop_cb), win);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_stop, FALSE, FALSE, 0);

    win->record_button = gtk_toggle_button_new_with_label("⏺");
    g_signal_connect(win->record_button, "toggled",
                     G_CALLBACK(mw_transport_record_cb), win);
    gtk_box_pack_start(GTK_BOX(toolbar), win->record_button, FALSE, FALSE, 0);

    win->time_label = gtk_label_new("00:00.0");
    gtk_widget_set_size_request(win->time_label, 90, -1);
    gtk_box_pack_start(GTK_BOX(toolbar), win->time_label, FALSE, FALSE, 8);

    /* ---- Timeline ---- */
    GtkWidget *tl_widget = jackdaw_timeline_new(project);
    win->timeline = JACKDAW_TIMELINE(tl_widget);
    gtk_box_pack_start(GTK_BOX(vbox), tl_widget, TRUE, TRUE, 0);

    g_signal_connect(tl_widget, "position-changed",
                     G_CALLBACK(mw_on_position_changed), win);

    win->transport_timer = g_timeout_add(100, mw_transport_timer, win);

    gtk_widget_show_all(GTK_WIDGET(win));

    return GTK_WIDGET(win);
}
