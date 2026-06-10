#define _GNU_SOURCE
#include <config.h>
#include <math.h>

#include "trackstrip.h"
#include "timeline.h"       /* TIMELINE_HEADER_WIDTH */
#include "jackdaw-engine.h"

G_DEFINE_TYPE(JackDawTrackStrip, jackdaw_track_strip, GTK_TYPE_BOX)

/* ---- Cairo knob --------------------------------------------------------- */

#define KNOB_SIZE       28          /* px: drawing area width and height */
#define KNOB_START_ANG  (-M_PI * 5.0 / 4.0)   /* min value: ~7:30 position */
#define KNOB_END_ANG    (M_PI / 4.0)            /* max value: ~4:30 position */

typedef struct {
    double   value;
    double   min;
    double   max;
    double   drag_start_y;
    double   drag_start_val;
    gboolean dragging;
    void   (*on_change)(double val, gpointer user_data);
    gpointer user_data;
} KnobData;

static double knob_angle(KnobData *kd)
{
    double t = (kd->max > kd->min)
               ? (kd->value - kd->min) / (kd->max - kd->min)
               : 0.0;
    t = CLAMP(t, 0.0, 1.0);
    return KNOB_START_ANG + t * (KNOB_END_ANG - KNOB_START_ANG);
}

static gboolean knob_draw_cb(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    KnobData *kd = data;
    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);

    double cx = alloc.width  / 2.0;
    double cy = alloc.height / 2.0;
    double r  = MIN(cx, cy) - 2.5;
    double ang = knob_angle(kd);

    /* Background arc track */
    cairo_set_source_rgb(cr, 0.25, 0.25, 0.25);
    cairo_set_line_width(cr, 3.0);
    cairo_arc(cr, cx, cy, r - 1.5, KNOB_START_ANG, KNOB_END_ANG);
    cairo_stroke(cr);

    /* Value arc */
    cairo_set_source_rgb(cr, 0.2, 0.55, 0.9);
    cairo_set_line_width(cr, 3.0);
    cairo_arc(cr, cx, cy, r - 1.5, KNOB_START_ANG, ang);
    cairo_stroke(cr);

    /* Pointer dot on the rim */
    double px = cx + (r - 3.5) * cos(ang);
    double py = cy + (r - 3.5) * sin(ang);
    cairo_set_source_rgb(cr, 0.95, 0.95, 0.95);
    cairo_arc(cr, px, py, 2.0, 0.0, 2.0 * M_PI);
    cairo_fill(cr);

    return FALSE;
}

static gboolean knob_press(GtkWidget *widget, GdkEventButton *ev, gpointer data)
{
    KnobData *kd = data;
    if (ev->button != 1) return FALSE;
    kd->dragging       = TRUE;
    kd->drag_start_y   = ev->y_root;
    kd->drag_start_val = kd->value;
    gtk_widget_grab_focus(widget);
    return TRUE;
}

static gboolean knob_motion(GtkWidget *widget, GdkEventMotion *ev, gpointer data)
{
    KnobData *kd = data;
    if (!kd->dragging) return FALSE;
    double range = kd->max - kd->min;
    double delta = (kd->drag_start_y - ev->y_root) * range / 150.0;
    kd->value = CLAMP(kd->drag_start_val + delta, kd->min, kd->max);
    gtk_widget_queue_draw(widget);
    if (kd->on_change) kd->on_change(kd->value, kd->user_data);
    return TRUE;
}

static gboolean knob_release(GtkWidget *widget, GdkEventButton *ev, gpointer data)
{
    (void)widget; (void)ev;
    KnobData *kd = data;
    kd->dragging = FALSE;
    return TRUE;
}

static gboolean knob_scroll(GtkWidget *widget, GdkEventScroll *ev, gpointer data)
{
    KnobData *kd = data;
    double step = (kd->max - kd->min) / 200.0;
    if (ev->direction == GDK_SCROLL_UP   || ev->direction == GDK_SCROLL_RIGHT)
        kd->value = CLAMP(kd->value + step, kd->min, kd->max);
    else if (ev->direction == GDK_SCROLL_DOWN || ev->direction == GDK_SCROLL_LEFT)
        kd->value = CLAMP(kd->value - step, kd->min, kd->max);
    else
        return FALSE;
    gtk_widget_queue_draw(widget);
    if (kd->on_change) kd->on_change(kd->value, kd->user_data);
    return TRUE;
}

static GtkWidget *knob_new(double min, double max, double value,
                            void (*on_change)(double, gpointer), gpointer user_data)
{
    KnobData *kd     = g_new0(KnobData, 1);
    kd->min          = min;
    kd->max          = max;
    kd->value        = CLAMP(value, min, max);
    kd->on_change    = on_change;
    kd->user_data    = user_data;

    GtkWidget *da = gtk_drawing_area_new();
    gtk_widget_set_size_request(da, KNOB_SIZE, KNOB_SIZE);
    gtk_widget_add_events(da,
        GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
        GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK);
    gtk_widget_set_can_focus(da, TRUE);

    g_object_set_data_full(G_OBJECT(da), "knob-data", kd, g_free);
    g_signal_connect(da, "draw",                  G_CALLBACK(knob_draw_cb),  kd);
    g_signal_connect(da, "button-press-event",    G_CALLBACK(knob_press),    kd);
    g_signal_connect(da, "motion-notify-event",   G_CALLBACK(knob_motion),   kd);
    g_signal_connect(da, "button-release-event",  G_CALLBACK(knob_release),  kd);
    g_signal_connect(da, "scroll-event",          G_CALLBACK(knob_scroll),   kd);

    return da;
}

/* ---- Strip signal callbacks --------------------------------------------- */

static void on_vol_changed(double db, gpointer data)
{
    JackDawTrackStrip *strip = data;
    if (strip->suppress_update) return;
    gfloat linear = (gfloat)pow(10.0, db / 20.0);
    jackdaw_track_set_volume(strip->track, linear);
}

static void on_pan_changed(double pan, gpointer data)
{
    JackDawTrackStrip *strip = data;
    if (strip->suppress_update) return;
    jackdaw_track_set_pan(strip->track, (gfloat)pan);
}

static void on_name_changed(GtkEntry *entry, gpointer data)
{
    JackDawTrackStrip *strip = data;
    if (strip->suppress_update) return;
    jackdaw_track_set_name(strip->track, gtk_entry_get_text(entry));
}

static void on_arm_toggled(GtkToggleButton *btn, gpointer data)
{
    JackDawTrackStrip *strip = data;
    if (strip->suppress_update) return;
    jackdaw_track_set_armed(strip->track, gtk_toggle_button_get_active(btn));
}

static void on_mute_toggled(GtkToggleButton *btn, gpointer data)
{
    JackDawTrackStrip *strip = data;
    if (strip->suppress_update) return;
    jackdaw_track_set_muted(strip->track, gtk_toggle_button_get_active(btn));
}

static void on_solo_toggled(GtkToggleButton *btn, gpointer data)
{
    JackDawTrackStrip *strip = data;
    if (strip->suppress_update) return;
    jackdaw_track_set_soloed(strip->track, gtk_toggle_button_get_active(btn));
}

static void on_mono_toggled(GtkToggleButton *btn, gpointer data)
{
    JackDawTrackStrip *strip = data;
    if (strip->suppress_update) return;
    gboolean stereo = gtk_toggle_button_get_active(btn);
    strip->track->mono_record = !stereo;
    gtk_button_set_label(GTK_BUTTON(btn), stereo ? "St" : "Mo");
}

/* ---- VU meter ----------------------------------------------------------- */

static gboolean vu_draw_cb(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    JackDawTrackStrip *strip = data;
    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);
    gint w = alloc.width;
    gint h = alloc.height;

    cairo_set_source_rgb(cr, 0.08, 0.08, 0.08);
    cairo_paint(cr);

    gfloat peaks[2] = { strip->vu_peak_L, strip->vu_peak_R };
    gint   bar_w    = (w - 3) / 2;  /* 1px border each side + 1px gap between */

    for (int ch = 0; ch < 2; ch++) {
        gint bx = (ch == 0) ? 1 : (2 + bar_w);

        /* Empty trough */
        cairo_set_source_rgb(cr, 0.18, 0.18, 0.18);
        cairo_rectangle(cr, bx, 0, bar_w, h);
        cairo_fill(cr);

        gfloat pk = peaks[ch];
        if (pk > 0.0001f) {
            float db      = 20.0f * log10f(pk);
            float db_clip = CLAMP(db, -60.0f, 6.0f);
            float frac    = (db_clip + 60.0f) / 66.0f;  /* 0 = -60dBFS, 1 = +6dBFS */
            gint  fill_h  = (gint)(frac * (float)h);
            if (fill_h > h) fill_h = h;

            if (fill_h > 0) {
                /* Color: green below -12dB, yellow -12..0dB, red above 0dB */
                if (db >= 0.0f)
                    cairo_set_source_rgb(cr, 0.90, 0.15, 0.15);
                else if (db >= -12.0f)
                    cairo_set_source_rgb(cr, 0.85, 0.78, 0.10);
                else
                    cairo_set_source_rgb(cr, 0.15, 0.68, 0.20);

                cairo_rectangle(cr, bx, h - fill_h, bar_w, fill_h);
                cairo_fill(cr);
            }
        }
    }
    return FALSE;
}

static gboolean vu_timer_cb(gpointer data)
{
    JackDawTrackStrip *strip = data;
    if (!JACKDAW_IS_TRACK_STRIP(strip)) return G_SOURCE_REMOVE;

    gfloat pk_L = 0.0f, pk_R = 0.0f;
    jackdaw_track_get_peaks(strip->track, &pk_L, &pk_R);

    /* Instant rise, ~-20dB/s decay (0.89 per 50ms tick) */
    strip->vu_peak_L = (pk_L > strip->vu_peak_L) ? pk_L : strip->vu_peak_L * 0.89f;
    strip->vu_peak_R = (pk_R > strip->vu_peak_R) ? pk_R : strip->vu_peak_R * 0.89f;

    gtk_widget_queue_draw(strip->vu_meter);
    return G_SOURCE_CONTINUE;
}

/* ---- Input combo -------------------------------------------------------- */

static gboolean input_row_sep_func(GtkTreeModel *model, GtkTreeIter *iter,
                                    gpointer data)
{
    (void)data;
    gboolean is_sep = FALSE;
    gtk_tree_model_get(model, iter, ICOL_IS_SEP, &is_sep, -1);
    return is_sep;
}

static void on_input_combo_changed(GtkComboBox *combo, gpointer data)
{
    JackDawTrackStrip *strip = data;
    if (strip->suppress_update) return;

    GtkTreeIter iter;
    if (!gtk_combo_box_get_active_iter(combo, &iter)) return;

    GtkTreeModel *model = gtk_combo_box_get_model(combo);
    gchar    *port      = NULL;
    gboolean  is_audio  = FALSE;
    gboolean  sensitive = FALSE;
    gboolean  is_sep    = FALSE;

    gtk_tree_model_get(model, &iter,
        ICOL_PORT,      &port,
        ICOL_IS_AUDIO,  &is_audio,
        ICOL_SENSITIVE, &sensitive,
        ICOL_IS_SEP,    &is_sep,
        -1);

    if (is_sep || !sensitive) {
        /* Header or separator — bounce back to None */
        strip->suppress_update = TRUE;
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
        strip->suppress_update = FALSE;
        g_free(port);
        return;
    }

    if (!port) {
        /* "None" selected */
        jackdaw_engine_set_audio_source(strip->track, NULL);
        jackdaw_engine_set_midi_source(strip->track,  NULL);
    } else if (is_audio) {
        jackdaw_engine_set_audio_source(strip->track, port);
        jackdaw_engine_set_midi_source(strip->track,  NULL);
    } else {
        jackdaw_engine_set_midi_source(strip->track,  port);
        jackdaw_engine_set_audio_source(strip->track, NULL);
    }
    g_free(port);
}

static void on_ports_changed(JackDawProject *project, gpointer data)
{
    (void)project;
    jackdaw_track_strip_refresh_ports(JACKDAW_TRACK_STRIP(data));
}

/* ---- Port combo refresh ------------------------------------------------- */

void jackdaw_track_strip_refresh_ports(JackDawTrackStrip *strip)
{
    g_return_if_fail(JACKDAW_IS_TRACK_STRIP(strip));

    strip->suppress_update = TRUE;

    GtkListStore *store = strip->input_store;
    GtkTreeIter   iter;

    gtk_list_store_clear(store);

    /* "None" row */
    gtk_list_store_append(store, &iter);
    gtk_list_store_set(store, &iter,
        ICOL_TEXT,      "None",
        ICOL_PORT,      NULL,
        ICOL_IS_AUDIO,  FALSE,
        ICOL_IS_SEP,    FALSE,
        ICOL_SENSITIVE, TRUE,
        -1);

    /* Audio section */
    gchar **audio_ports = jackdaw_engine_list_audio_sources();
    if (audio_ports && audio_ports[0]) {
        /* Separator line */
        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter,
            ICOL_TEXT, "", ICOL_PORT, NULL,
            ICOL_IS_AUDIO, FALSE, ICOL_IS_SEP, TRUE, ICOL_SENSITIVE, FALSE, -1);
        /* "— Audio —" header */
        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter,
            ICOL_TEXT,      "\342\200\224 Audio \342\200\224",
            ICOL_PORT,      NULL,
            ICOL_IS_AUDIO,  TRUE,
            ICOL_IS_SEP,    FALSE,
            ICOL_SENSITIVE, FALSE,
            -1);
        for (gchar **p = audio_ports; *p; p++) {
            gtk_list_store_append(store, &iter);
            gtk_list_store_set(store, &iter,
                ICOL_TEXT,      *p,
                ICOL_PORT,      *p,
                ICOL_IS_AUDIO,  TRUE,
                ICOL_IS_SEP,    FALSE,
                ICOL_SENSITIVE, TRUE,
                -1);
        }
    }
    g_strfreev(audio_ports);

    /* MIDI section */
    gchar **midi_ports = jackdaw_engine_list_midi_sources();
    if (midi_ports && midi_ports[0]) {
        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter,
            ICOL_TEXT, "", ICOL_PORT, NULL,
            ICOL_IS_AUDIO, FALSE, ICOL_IS_SEP, TRUE, ICOL_SENSITIVE, FALSE, -1);
        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter,
            ICOL_TEXT,      "\342\200\224 MIDI \342\200\224",
            ICOL_PORT,      NULL,
            ICOL_IS_AUDIO,  FALSE,
            ICOL_IS_SEP,    FALSE,
            ICOL_SENSITIVE, FALSE,
            -1);
        for (gchar **p = midi_ports; *p; p++) {
            gtk_list_store_append(store, &iter);
            gtk_list_store_set(store, &iter,
                ICOL_TEXT,      *p,
                ICOL_PORT,      *p,
                ICOL_IS_AUDIO,  FALSE,
                ICOL_IS_SEP,    FALSE,
                ICOL_SENSITIVE, TRUE,
                -1);
        }
    }
    g_strfreev(midi_ports);

    /* Restore active selection: prefer audio_src_port, then midi_src_port */
    const gchar *want_audio = strip->track->audio_src_port;
    const gchar *want_midi  = strip->track->midi_src_port;
    gint active_row = 0;
    gint row = 0;

    GtkTreeModel *model = GTK_TREE_MODEL(store);
    GtkTreeIter  it;
    if (gtk_tree_model_get_iter_first(model, &it)) {
        do {
            gchar    *port     = NULL;
            gboolean  is_audio = FALSE;
            gtk_tree_model_get(model, &it,
                ICOL_PORT,     &port,
                ICOL_IS_AUDIO, &is_audio,
                -1);
            if (port) {
                if (is_audio && want_audio && strcmp(port, want_audio) == 0)
                    active_row = row;
                else if (!is_audio && want_midi && strcmp(port, want_midi) == 0)
                    active_row = row;
            }
            g_free(port);
            row++;
        } while (gtk_tree_model_iter_next(model, &it));
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(strip->input_combo), active_row);

    strip->suppress_update = FALSE;
}

/* ---- Fixed-width override ----------------------------------------------- */

static void jackdaw_track_strip_get_preferred_width(GtkWidget *widget,
                                                     gint *minimum,
                                                     gint *natural)
{
    (void)widget;
    *minimum = *natural = TIMELINE_HEADER_WIDTH;
}

/* ---- GObject boilerplate ------------------------------------------------ */

static void jackdaw_track_strip_finalize(GObject *obj)
{
    JackDawTrackStrip *strip = JACKDAW_TRACK_STRIP(obj);
    if (strip->vu_timer) {
        g_source_remove(strip->vu_timer);
        strip->vu_timer = 0;
    }
    g_object_unref(strip->track);
    g_object_unref(strip->project);
    if (strip->input_store) g_object_unref(strip->input_store);
    G_OBJECT_CLASS(jackdaw_track_strip_parent_class)->finalize(obj);
}

static void jackdaw_track_strip_class_init(JackDawTrackStripClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = jackdaw_track_strip_finalize;
    GTK_WIDGET_CLASS(klass)->get_preferred_width =
        jackdaw_track_strip_get_preferred_width;
}

static void jackdaw_track_strip_init(JackDawTrackStrip *strip)
{
    strip->track           = NULL;
    strip->project         = NULL;
    strip->name_entry      = NULL;
    strip->btn_arm         = NULL;
    strip->btn_mute        = NULL;
    strip->btn_solo        = NULL;
    strip->btn_mono        = NULL;
    strip->vol_knob        = NULL;
    strip->pan_knob        = NULL;
    strip->input_combo     = NULL;
    strip->input_store     = NULL;
    strip->vu_meter        = NULL;
    strip->vu_peak_L       = 0.0f;
    strip->vu_peak_R       = 0.0f;
    strip->vu_timer        = 0;
    strip->suppress_update = FALSE;

    /* Horizontal: [controls | vu_meter] */
    gtk_orientable_set_orientation(GTK_ORIENTABLE(strip),
                                   GTK_ORIENTATION_HORIZONTAL);
    gtk_box_set_spacing(GTK_BOX(strip), 0);
    gtk_container_set_border_width(GTK_CONTAINER(strip), 1);
}

/* ---- Constructor -------------------------------------------------------- */

GtkWidget *jackdaw_track_strip_new(JackDawTrack   *track,
                                    JackDawProject *project)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(track),    NULL);
    g_return_val_if_fail(JACKDAW_IS_PROJECT(project), NULL);

    JackDawTrackStrip *strip =
        g_object_new(JACKDAW_TYPE_TRACK_STRIP, NULL);

    strip->track   = g_object_ref(track);
    strip->project = g_object_ref(project);

    /* ---- Left controls box (vertical) ---- */
    GtkWidget *left_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    /* Row 1: editable track name */
    strip->name_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(strip->name_entry),
                       jackdaw_track_get_name(track));
    gtk_entry_set_placeholder_text(GTK_ENTRY(strip->name_entry), "Track name");
    gtk_entry_set_has_frame(GTK_ENTRY(strip->name_entry), FALSE);
    gtk_widget_set_size_request(strip->name_entry, 1, -1);
    gtk_box_pack_start(GTK_BOX(left_box), strip->name_entry, FALSE, FALSE, 0);

    /* Row 2: [A][M][S][Mo] | V knob | P knob */
    GtkWidget *ctrl_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);

    strip->btn_arm  = gtk_toggle_button_new_with_label("A");
    strip->btn_mute = gtk_toggle_button_new_with_label("M");
    strip->btn_solo = gtk_toggle_button_new_with_label("S");
    /* Mono/stereo record toggle — default: Mo (mono) */
    strip->btn_mono = gtk_toggle_button_new_with_label("Mo");

    gtk_widget_set_size_request(strip->btn_arm,  20, 20);
    gtk_widget_set_size_request(strip->btn_mute, 20, 20);
    gtk_widget_set_size_request(strip->btn_solo, 20, 20);
    gtk_widget_set_size_request(strip->btn_mono, 26, 20);

    gtk_widget_set_tooltip_text(strip->btn_arm,  "Arm for recording");
    gtk_widget_set_tooltip_text(strip->btn_mute, "Mute");
    gtk_widget_set_tooltip_text(strip->btn_solo, "Solo");
    gtk_widget_set_tooltip_text(strip->btn_mono,
        "Record mode: Mo = mono (single channel), St = stereo");

    gfloat vol_linear = jackdaw_track_get_volume(track);
    double vol_db = (vol_linear > 0.0f)
                    ? CLAMP(20.0 * log10((double)vol_linear), -25.0, 25.0)
                    : -25.0;
    strip->vol_knob = knob_new(-25.0, 25.0, vol_db, on_vol_changed, strip);
    strip->pan_knob = knob_new( -1.0,  1.0,
                               (double)jackdaw_track_get_pan(track),
                               on_pan_changed, strip);
    gtk_widget_set_tooltip_text(strip->vol_knob,
        "Volume: drag up/down or scroll. Centre = 0 dB");
    gtk_widget_set_tooltip_text(strip->pan_knob,
        "Pan: drag up/down or scroll. Centre = C");

    GtkWidget *vol_lbl = gtk_label_new("V");
    GtkWidget *pan_lbl = gtk_label_new("P");

    gtk_box_pack_start(GTK_BOX(ctrl_row), strip->btn_arm,  FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ctrl_row), strip->btn_mute, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ctrl_row), strip->btn_solo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ctrl_row), strip->btn_mono, FALSE, FALSE, 1);
    gtk_box_pack_start(GTK_BOX(ctrl_row), vol_lbl,         FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(ctrl_row), strip->vol_knob, FALSE, FALSE, 1);
    gtk_box_pack_start(GTK_BOX(ctrl_row), pan_lbl,         FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(ctrl_row), strip->pan_knob, FALSE, FALSE, 1);
    gtk_box_pack_start(GTK_BOX(left_box), ctrl_row, FALSE, FALSE, 0);

    /* Row 3: Single input combo (audio + MIDI grouped) */
    strip->input_store = gtk_list_store_new(ICOL_COUNT,
        G_TYPE_STRING,   /* ICOL_TEXT      */
        G_TYPE_STRING,   /* ICOL_PORT      */
        G_TYPE_BOOLEAN,  /* ICOL_IS_AUDIO  */
        G_TYPE_BOOLEAN,  /* ICOL_IS_SEP    */
        G_TYPE_BOOLEAN); /* ICOL_SENSITIVE */

    strip->input_combo = gtk_combo_box_new_with_model(
        GTK_TREE_MODEL(strip->input_store));
    gtk_combo_box_set_row_separator_func(GTK_COMBO_BOX(strip->input_combo),
        input_row_sep_func, NULL, NULL);
    gtk_widget_set_size_request(strip->input_combo, 1, -1);
    gtk_widget_set_tooltip_text(strip->input_combo,
        "Input source: audio or MIDI port");

    GtkCellRenderer *cr = gtk_cell_renderer_text_new();
    g_object_set(cr, "ellipsize", PANGO_ELLIPSIZE_MIDDLE, NULL);
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(strip->input_combo), cr, TRUE);
    gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(strip->input_combo), cr,
        "text",      ICOL_TEXT,
        "sensitive", ICOL_SENSITIVE,
        NULL);

    gtk_box_pack_start(GTK_BOX(left_box), strip->input_combo, FALSE, FALSE, 0);

    /* ---- VU meter (right side, 20px wide) ---- */
    strip->vu_meter = gtk_drawing_area_new();
    gtk_widget_set_size_request(strip->vu_meter, 20, -1);
    gtk_widget_add_events(strip->vu_meter, 0);
    g_signal_connect(strip->vu_meter, "draw", G_CALLBACK(vu_draw_cb), strip);

    /* Pack left controls + VU into the strip (horizontal) */
    gtk_box_pack_start(GTK_BOX(strip), left_box,      TRUE,  TRUE,  0);
    gtk_box_pack_start(GTK_BOX(strip), strip->vu_meter, FALSE, FALSE, 0);

    /* ---- Set initial button states (suppress callbacks) ---- */
    strip->suppress_update = TRUE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(strip->btn_arm),
                                 jackdaw_track_is_armed(track));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(strip->btn_mute),
                                 jackdaw_track_is_muted(track));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(strip->btn_solo),
                                 jackdaw_track_is_soloed(track));
    /* btn_mono: active = stereo, inactive = mono (default) */
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(strip->btn_mono),
                                 !track->mono_record);
    strip->suppress_update = FALSE;

    /* ---- Connect UI signals ---- */
    g_signal_connect(strip->name_entry, "changed",
                     G_CALLBACK(on_name_changed),  strip);
    g_signal_connect(strip->btn_arm,  "toggled",
                     G_CALLBACK(on_arm_toggled),  strip);
    g_signal_connect(strip->btn_mute, "toggled",
                     G_CALLBACK(on_mute_toggled), strip);
    g_signal_connect(strip->btn_solo, "toggled",
                     G_CALLBACK(on_solo_toggled), strip);
    g_signal_connect(strip->btn_mono, "toggled",
                     G_CALLBACK(on_mono_toggled), strip);
    g_signal_connect(strip->input_combo, "changed",
                     G_CALLBACK(on_input_combo_changed), strip);

    /* Auto-disconnect when strip is finalized */
    g_signal_connect_object(project, "ports-changed",
                            G_CALLBACK(on_ports_changed), strip, 0);

    /* Initial combo population */
    jackdaw_track_strip_refresh_ports(strip);

    /* Start VU meter refresh timer */
    strip->vu_timer = g_timeout_add(50, vu_timer_cb, strip);

    return GTK_WIDGET(strip);
}
