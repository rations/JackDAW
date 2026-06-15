#define _GNU_SOURCE
#include <config.h>
#include <math.h>

#include "trackstrip.h"
#include "timeline.h"       /* TIMELINE_HEADER_WIDTH */
#include "jackdaw-engine.h"
#include "knob.h"
#include "fxwindow.h"

G_DEFINE_TYPE(JackDawTrackStrip, jackdaw_track_strip, GTK_TYPE_BOX)

/* ---- Strip signal callbacks --------------------------------------------- */

static void on_vol_changed(double db, gpointer data)
{
    JackDawTrackStrip *strip = data;
    if (strip->suppress_update) return;
    gfloat linear = (gfloat)pow(10.0, db / 20.0);
    strip->self_update = TRUE;
    jackdaw_track_set_trim(strip->track, linear);   /* dial = input trim stage */
    strip->self_update = FALSE;
}

static void on_pan_changed(double pan, gpointer data)
{
    JackDawTrackStrip *strip = data;
    if (strip->suppress_update) return;
    strip->self_update = TRUE;
    jackdaw_track_set_pan(strip->track, (gfloat)pan);
    strip->self_update = FALSE;
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
    strip->self_update = TRUE;
    jackdaw_track_set_muted(strip->track, gtk_toggle_button_get_active(btn));
    strip->self_update = FALSE;
}

static void on_solo_toggled(GtkToggleButton *btn, gpointer data)
{
    JackDawTrackStrip *strip = data;
    if (strip->suppress_update) return;
    strip->self_update = TRUE;
    jackdaw_track_set_soloed(strip->track, gtk_toggle_button_get_active(btn));
    strip->self_update = FALSE;
}

/* Reflect external track changes (e.g. from the mixer) onto this strip's
 * mute/solo buttons and pan knob. Volume is intentionally left alone — the
 * track's volume dial is a separate control from the mixer fader. */
static void on_track_state_changed(JackDawTrack *t, gpointer data)
{
    JackDawTrackStrip *strip = data;
    if (strip->self_update) return;
    strip->suppress_update = TRUE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(strip->btn_mute),
                                 jackdaw_track_is_muted(t));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(strip->btn_solo),
                                 jackdaw_track_is_soloed(t));
    if (strip->pan_knob)
        knob_set_value(strip->pan_knob, (double)jackdaw_track_get_pan(t));
    strip->suppress_update = FALSE;
}

/* Input modes for the routing popover. */
enum { TS_MODE_MONO = 0, TS_MODE_STEREO = 1, TS_MODE_MIDI = 2 };

static void ts_update_input_label(JackDawTrackStrip *strip);
static void ts_set_mode(JackDawTrackStrip *strip, int mode);

/* The control-row St toggle mirrors the popover mode: Mo↔Mono, St↔Stereo. */
static void on_mono_toggled(GtkToggleButton *btn, gpointer data)
{
    JackDawTrackStrip *strip = data;
    if (strip->suppress_update) return;
    ts_set_mode(strip, gtk_toggle_button_get_active(btn)
                       ? TS_MODE_STEREO : TS_MODE_MONO);
}

static void on_fx_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;
    JackDawTrackStrip *strip = data;
    jackdaw_fx_window_open(strip->track, strip->project);
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

    strip->vu_peak_L = pk_L;
    strip->vu_peak_R = pk_R;
    gtk_widget_queue_draw(strip->vu_meter);

    /* Keep Fx button blue whenever the track has at least one effect loaded. */
    GtkStyleContext *fx_ctx = gtk_widget_get_style_context(strip->btn_fx);
    if (jackdaw_track_fx_count(strip->track) > 0)
        gtk_style_context_add_class(fx_ctx, "ts-fx-active");
    else
        gtk_style_context_remove_class(fx_ctx, "ts-fx-active");

    return G_SOURCE_CONTINUE;
}

/* ---- Input source selectors -------------------------------------------- */

/* The trailing "port" half of a JACK "client:port" name, for compact labels. */
static const char *port_short(const char *full)
{
    if (!full) return NULL;
    const char *colon = strchr(full, ':');
    return (colon && colon[1]) ? colon + 1 : full;
}

/* Refresh the menu button's label/tooltip from the track's current sources. */
static void ts_update_input_label(JackDawTrackStrip *strip)
{
    const char *l = strip->track->audio_src_port;
    const char *r = strip->track->audio_src_port_r;
    const char *m = strip->track->midi_src_port;

    gchar *summary;
    if (!l && !r && !m) {
        summary = g_strdup("In: None");
    } else if (l && r) {
        summary = g_strdup_printf("In: %s+%s", port_short(l), port_short(r));
    } else if (l || r) {
        summary = g_strdup_printf("In: %s", port_short(l ? l : r));
    } else {
        summary = g_strdup_printf("In: MIDI %s", port_short(m));
    }
    /* When both audio and MIDI are set, flag the MIDI presence too. */
    if ((l || r) && m) {
        gchar *both = g_strdup_printf("%s +MIDI", summary);
        g_free(summary);
        summary = both;
    }
    gtk_button_set_label(GTK_BUTTON(strip->input_button), summary);

    gchar *tip = g_strdup_printf("Left: %s\nRight: %s\nMIDI: %s",
                                 l ? l : "None", r ? r : "None", m ? m : "None");
    gtk_widget_set_tooltip_text(strip->input_button, tip);
    g_free(tip);
    g_free(summary);
}

/* Selected JACK port name from a source combo, or NULL for the "None" row. */
static gchar *combo_selected_port(GtkComboBox *combo)
{
    GtkTreeIter iter;
    if (!gtk_combo_box_get_active_iter(combo, &iter)) return NULL;
    gchar *port = NULL;
    gtk_tree_model_get(gtk_combo_box_get_model(combo), &iter,
                       PCOL_PORT, &port, -1);
    return port;   /* caller frees */
}

/* Show only the source rows relevant to the active mode. */
static void ts_apply_mode_visibility(JackDawTrackStrip *strip, int mode)
{
    gboolean audio  = (mode != TS_MODE_MIDI);
    gboolean stereo = (mode == TS_MODE_STEREO);
    gboolean midi   = (mode == TS_MODE_MIDI);

    gtk_label_set_text(GTK_LABEL(strip->lbl_src), stereo ? "Left" : "Source");
    gtk_widget_set_visible(strip->lbl_src,        audio);
    gtk_widget_set_visible(strip->in_combo_l,     audio);
    gtk_widget_set_visible(strip->lbl_right,      stereo);
    gtk_widget_set_visible(strip->in_combo_r,     stereo);
    gtk_widget_set_visible(strip->lbl_midi,       midi);
    gtk_widget_set_visible(strip->in_combo_midi,  midi);
}

/* Apply an input mode: sync the radios + St button, show the right rows, and
 * (re)wire the engine from the current combo selections. */
static void ts_set_mode(JackDawTrackStrip *strip, int mode)
{
    JackDawTrack *t = strip->track;
    gboolean was = strip->suppress_update;
    strip->suppress_update = TRUE;

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(strip->rb_mono),
                                 mode == TS_MODE_MONO);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(strip->rb_stereo),
                                 mode == TS_MODE_STEREO);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(strip->rb_midi),
                                 mode == TS_MODE_MIDI);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(strip->btn_mono),
                                 mode == TS_MODE_STEREO);
    gtk_button_set_label(GTK_BUTTON(strip->btn_mono),
                         mode == TS_MODE_STEREO ? "St" : "Mo");

    ts_apply_mode_visibility(strip, mode);

    if (mode == TS_MODE_MIDI) {
        jackdaw_engine_set_track_stereo(t, FALSE);   /* drops right port */
        jackdaw_engine_set_audio_source_l(t, NULL);
        gchar *mp = combo_selected_port(GTK_COMBO_BOX(strip->in_combo_midi));
        jackdaw_engine_set_midi_source(t, mp);
        g_free(mp);
        gtk_combo_box_set_active(GTK_COMBO_BOX(strip->in_combo_l), 0);
        gtk_combo_box_set_active(GTK_COMBO_BOX(strip->in_combo_r), 0);
    } else {
        gboolean stereo = (mode == TS_MODE_STEREO);
        jackdaw_engine_set_midi_source(t, NULL);
        gtk_combo_box_set_active(GTK_COMBO_BOX(strip->in_combo_midi), 0);
        /* Register/unregister the right port before wiring its source. */
        jackdaw_engine_set_track_stereo(t, stereo);
        gchar *lp = combo_selected_port(GTK_COMBO_BOX(strip->in_combo_l));
        jackdaw_engine_set_audio_source_l(t, lp);
        g_free(lp);
        if (stereo) {
            gchar *rp = combo_selected_port(GTK_COMBO_BOX(strip->in_combo_r));
            jackdaw_engine_set_audio_source_r(t, rp);
            g_free(rp);
        } else {
            gtk_combo_box_set_active(GTK_COMBO_BOX(strip->in_combo_r), 0);
        }
    }

    strip->suppress_update = was;
    ts_update_input_label(strip);
}

static void on_mode_mono(GtkToggleButton *b, gpointer data)
{
    JackDawTrackStrip *strip = data;
    if (strip->suppress_update || !gtk_toggle_button_get_active(b)) return;
    ts_set_mode(strip, TS_MODE_MONO);
}

static void on_mode_stereo(GtkToggleButton *b, gpointer data)
{
    JackDawTrackStrip *strip = data;
    if (strip->suppress_update || !gtk_toggle_button_get_active(b)) return;
    ts_set_mode(strip, TS_MODE_STEREO);
}

static void on_mode_midi(GtkToggleButton *b, gpointer data)
{
    JackDawTrackStrip *strip = data;
    if (strip->suppress_update || !gtk_toggle_button_get_active(b)) return;
    ts_set_mode(strip, TS_MODE_MIDI);
}

static void on_in_l_changed(GtkComboBox *combo, gpointer data)
{
    JackDawTrackStrip *strip = data;
    if (strip->suppress_update) return;
    gchar *port = combo_selected_port(combo);
    jackdaw_engine_set_audio_source_l(strip->track, port);
    gboolean stereo = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(strip->rb_stereo));
    gboolean cleared = (port == NULL);
    g_free(port);
    /* Clearing a channel of a stereo pair drops the track back to mono. */
    if (stereo && cleared) { ts_set_mode(strip, TS_MODE_MONO); return; }
    ts_update_input_label(strip);
}

static void on_in_r_changed(GtkComboBox *combo, gpointer data)
{
    JackDawTrackStrip *strip = data;
    if (strip->suppress_update) return;
    gchar *port = combo_selected_port(combo);
    jackdaw_engine_set_audio_source_r(strip->track, port);
    gboolean cleared = (port == NULL);
    g_free(port);
    /* Removing the right source ends stereo — revert to mono (left kept). */
    if (cleared) { ts_set_mode(strip, TS_MODE_MONO); return; }
    ts_update_input_label(strip);
}

static void on_in_midi_changed(GtkComboBox *combo, gpointer data)
{
    JackDawTrackStrip *strip = data;
    if (strip->suppress_update) return;
    gchar *port = combo_selected_port(combo);
    jackdaw_engine_set_midi_source(strip->track, port);
    g_free(port);
    ts_update_input_label(strip);
}

static void on_ports_changed(JackDawProject *project, gpointer data)
{
    (void)project;
    jackdaw_track_strip_refresh_ports(JACKDAW_TRACK_STRIP(data));
}

/* Fill a source store with a "None" row plus the given NULL-terminated ports. */
static void source_store_fill(GtkListStore *store, gchar **ports)
{
    GtkTreeIter iter;
    gtk_list_store_clear(store);
    gtk_list_store_append(store, &iter);
    gtk_list_store_set(store, &iter, PCOL_TEXT, "None", PCOL_PORT, NULL, -1);
    if (ports) {
        for (gchar **p = ports; *p; p++) {
            gtk_list_store_append(store, &iter);
            gtk_list_store_set(store, &iter, PCOL_TEXT, *p, PCOL_PORT, *p, -1);
        }
    }
}

/* Set a combo's active row to the one whose PCOL_PORT matches `want`, else the
 * "None" row (index 0). */
static void source_combo_select(GtkComboBox *combo, const char *want)
{
    GtkTreeModel *model = gtk_combo_box_get_model(combo);
    GtkTreeIter   it;
    gint active = 0, row = 0;
    if (want && gtk_tree_model_get_iter_first(model, &it)) {
        do {
            gchar *port = NULL;
            gtk_tree_model_get(model, &it, PCOL_PORT, &port, -1);
            if (port && strcmp(port, want) == 0) { active = row; g_free(port); break; }
            g_free(port);
            row++;
        } while (gtk_tree_model_iter_next(model, &it));
    }
    gtk_combo_box_set_active(combo, active);
}

/* A source-selector combo: text-rendered, ellipsized, backed by `model`. */
static GtkWidget *ts_make_source_combo(GtkTreeModel *model)
{
    GtkWidget *combo = gtk_combo_box_new_with_model(model);
    GtkCellRenderer *cr = gtk_cell_renderer_text_new();
    g_object_set(cr, "ellipsize", PANGO_ELLIPSIZE_MIDDLE, NULL);
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(combo), cr, TRUE);
    gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(combo), cr,
        "text", PCOL_TEXT, NULL);
    return combo;
}

/* ---- Port combo refresh ------------------------------------------------- */

void jackdaw_track_strip_refresh_ports(JackDawTrackStrip *strip)
{
    g_return_if_fail(JACKDAW_IS_TRACK_STRIP(strip));

    strip->suppress_update = TRUE;

    /* Audio sources feed both the Left and Right combos (shared store). */
    gchar **audio_ports = jackdaw_engine_list_audio_sources();
    source_store_fill(strip->audio_store, audio_ports);
    g_strfreev(audio_ports);

    gchar **midi_ports = jackdaw_engine_list_midi_sources();
    source_store_fill(strip->midi_store, midi_ports);
    g_strfreev(midi_ports);

    source_combo_select(GTK_COMBO_BOX(strip->in_combo_l),
                        strip->track->audio_src_port);
    source_combo_select(GTK_COMBO_BOX(strip->in_combo_r),
                        strip->track->audio_src_port_r);
    source_combo_select(GTK_COMBO_BOX(strip->in_combo_midi),
                        strip->track->midi_src_port);

    /* Reflect (not change) the track's current input mode in the UI. */
    int mode;
    if (strip->track->midi_src_port ||
        (jackdaw_track_is_instrument(strip->track) &&
         !strip->track->audio_src_port))
        mode = TS_MODE_MIDI;
    else if (!strip->track->mono_record)
        mode = TS_MODE_STEREO;
    else
        mode = TS_MODE_MONO;

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(strip->rb_mono),
                                 mode == TS_MODE_MONO);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(strip->rb_stereo),
                                 mode == TS_MODE_STEREO);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(strip->rb_midi),
                                 mode == TS_MODE_MIDI);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(strip->btn_mono),
                                 mode == TS_MODE_STEREO);
    gtk_button_set_label(GTK_BUTTON(strip->btn_mono),
                         mode == TS_MODE_STEREO ? "St" : "Mo");
    ts_apply_mode_visibility(strip, mode);

    ts_update_input_label(strip);

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
    if (strip->audio_store) g_object_unref(strip->audio_store);
    if (strip->midi_store)  g_object_unref(strip->midi_store);
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
    strip->btn_fx          = NULL;
    strip->ctrl_row        = NULL;
    strip->vol_knob        = NULL;
    strip->pan_knob        = NULL;
    strip->input_button    = NULL;
    strip->rb_mono         = NULL;
    strip->rb_stereo       = NULL;
    strip->rb_midi         = NULL;
    strip->lbl_src         = NULL;
    strip->lbl_right       = NULL;
    strip->lbl_midi        = NULL;
    strip->in_combo_l      = NULL;
    strip->in_combo_r      = NULL;
    strip->in_combo_midi   = NULL;
    strip->audio_store     = NULL;
    strip->midi_store      = NULL;
    strip->vu_meter        = NULL;
    strip->vu_peak_L       = 0.0f;
    strip->vu_peak_R       = 0.0f;
    strip->vu_timer        = 0;
    strip->suppress_update = FALSE;
    strip->self_update     = FALSE;

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
    strip->ctrl_row = ctrl_row;

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

    gfloat trim_linear = jackdaw_track_get_trim(track);
    double trim_db = (trim_linear > 0.0f)
                    ? CLAMP(20.0 * log10((double)trim_linear), -25.0, 25.0)
                    : -25.0;
    strip->vol_knob = knob_new(-25.0, 25.0, trim_db, 0.0, KNOB_DB,
                               on_vol_changed, strip);
    strip->pan_knob = knob_new( -1.0,  1.0,
                               (double)jackdaw_track_get_pan(track), 0.0,
                               KNOB_PAN, on_pan_changed, strip);
    gtk_widget_set_tooltip_text(strip->vol_knob,
        "Input trim/gain (separate from the mixer fader). Centre = 0 dB");
    gtk_widget_set_tooltip_text(strip->pan_knob,
        "Pan: drag up/down or scroll. Centre = C");

    GtkWidget *vol_lbl = gtk_label_new("V");
    GtkWidget *pan_lbl = gtk_label_new("P");

    gtk_style_context_add_class(gtk_widget_get_style_context(strip->btn_arm),
                                "ts-arm");
    gtk_style_context_add_class(gtk_widget_get_style_context(strip->btn_mute),
                                "ts-mute");
    gtk_style_context_add_class(gtk_widget_get_style_context(strip->btn_solo),
                                "ts-solo");
    gtk_style_context_add_class(gtk_widget_get_style_context(strip->btn_mono),
                                "ts-mono");

    strip->btn_fx = gtk_button_new_with_label("Fx");
    gtk_widget_set_size_request(strip->btn_fx, 24, 20);
    gtk_widget_set_tooltip_text(strip->btn_fx, "Open the effects window for this track");
    gtk_style_context_add_class(gtk_widget_get_style_context(strip->btn_fx), "ts-fx");
    g_signal_connect(strip->btn_fx, "clicked", G_CALLBACK(on_fx_clicked), strip);

    gtk_box_pack_start(GTK_BOX(ctrl_row), strip->btn_arm,  FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ctrl_row), strip->btn_mute, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ctrl_row), strip->btn_solo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ctrl_row), strip->btn_mono, FALSE, FALSE, 1);
    gtk_box_pack_start(GTK_BOX(ctrl_row), strip->btn_fx,   FALSE, FALSE, 1);
    gtk_box_pack_start(GTK_BOX(ctrl_row), vol_lbl,         FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(ctrl_row), strip->vol_knob, FALSE, FALSE, 1);
    gtk_box_pack_start(GTK_BOX(ctrl_row), pan_lbl,         FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(ctrl_row), strip->pan_knob, FALSE, FALSE, 1);
    gtk_box_pack_start(GTK_BOX(left_box), ctrl_row, FALSE, FALSE, 0);

    /* Row 3: input-source menu button. Its popover offers an input mode —
     * Mono / Stereo / MIDI — and the source combo(s) for that mode. */
    strip->audio_store = gtk_list_store_new(PCOL_COUNT,
        G_TYPE_STRING, G_TYPE_STRING);
    strip->midi_store  = gtk_list_store_new(PCOL_COUNT,
        G_TYPE_STRING, G_TYPE_STRING);

    strip->in_combo_l    = ts_make_source_combo(GTK_TREE_MODEL(strip->audio_store));
    strip->in_combo_r    = ts_make_source_combo(GTK_TREE_MODEL(strip->audio_store));
    strip->in_combo_midi = ts_make_source_combo(GTK_TREE_MODEL(strip->midi_store));

    /* Mode radios (grouped) */
    strip->rb_mono   = gtk_radio_button_new_with_label(NULL, "Mono");
    strip->rb_stereo = gtk_radio_button_new_with_label_from_widget(
        GTK_RADIO_BUTTON(strip->rb_mono), "Stereo");
    strip->rb_midi   = gtk_radio_button_new_with_label_from_widget(
        GTK_RADIO_BUTTON(strip->rb_mono), "MIDI");

    GtkWidget *mode_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(mode_row), strip->rb_mono,   FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(mode_row), strip->rb_stereo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(mode_row), strip->rb_midi,   FALSE, FALSE, 0);

    strip->lbl_src   = gtk_label_new("Source");
    strip->lbl_right = gtk_label_new("Right");
    strip->lbl_midi  = gtk_label_new("MIDI");
    gtk_widget_set_halign(strip->lbl_src,   GTK_ALIGN_START);
    gtk_widget_set_halign(strip->lbl_right, GTK_ALIGN_START);
    gtk_widget_set_halign(strip->lbl_midi,  GTK_ALIGN_START);
    gtk_widget_set_hexpand(strip->in_combo_l, TRUE);
    gtk_widget_set_hexpand(strip->in_combo_r, TRUE);
    gtk_widget_set_hexpand(strip->in_combo_midi, TRUE);

    /* Visibility is driven by the active mode, so keep these out of show_all. */
    gtk_widget_set_no_show_all(strip->lbl_src,       TRUE);
    gtk_widget_set_no_show_all(strip->in_combo_l,    TRUE);
    gtk_widget_set_no_show_all(strip->lbl_right,     TRUE);
    gtk_widget_set_no_show_all(strip->in_combo_r,    TRUE);
    gtk_widget_set_no_show_all(strip->lbl_midi,      TRUE);
    gtk_widget_set_no_show_all(strip->in_combo_midi, TRUE);

    GtkWidget *pop_grid = gtk_grid_new();
    gtk_container_set_border_width(GTK_CONTAINER(pop_grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(pop_grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(pop_grid), 8);

    gtk_grid_attach(GTK_GRID(pop_grid), mode_row,             0, 0, 2, 1);
    gtk_grid_attach(GTK_GRID(pop_grid), strip->lbl_src,       0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(pop_grid), strip->in_combo_l,    1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(pop_grid), strip->lbl_right,     0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(pop_grid), strip->in_combo_r,    1, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(pop_grid), strip->lbl_midi,      0, 3, 1, 1);
    gtk_grid_attach(GTK_GRID(pop_grid), strip->in_combo_midi, 1, 3, 1, 1);
    gtk_widget_show_all(pop_grid);

    GtkWidget *popover = gtk_popover_new(NULL);
    gtk_container_add(GTK_CONTAINER(popover), pop_grid);

    strip->input_button = gtk_menu_button_new();
    gtk_button_set_label(GTK_BUTTON(strip->input_button), "In: None");
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(strip->input_button), popover);
    gtk_widget_set_size_request(strip->input_button, 1, -1);

    gtk_box_pack_start(GTK_BOX(left_box), strip->input_button, FALSE, FALSE, 0);

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
    g_signal_connect(strip->rb_mono,   "toggled",
                     G_CALLBACK(on_mode_mono),   strip);
    g_signal_connect(strip->rb_stereo, "toggled",
                     G_CALLBACK(on_mode_stereo), strip);
    g_signal_connect(strip->rb_midi,   "toggled",
                     G_CALLBACK(on_mode_midi),   strip);
    g_signal_connect(strip->in_combo_l, "changed",
                     G_CALLBACK(on_in_l_changed), strip);
    g_signal_connect(strip->in_combo_r, "changed",
                     G_CALLBACK(on_in_r_changed), strip);
    g_signal_connect(strip->in_combo_midi, "changed",
                     G_CALLBACK(on_in_midi_changed), strip);

    /* Auto-disconnect when strip is finalized */
    g_signal_connect_object(project, "ports-changed",
                            G_CALLBACK(on_ports_changed), strip, 0);
    /* Stay in sync with mute/solo/pan changes made elsewhere (e.g. the mixer) */
    g_signal_connect_object(track, "state-changed",
                            G_CALLBACK(on_track_state_changed), strip, 0);

    /* Initial combo population */
    jackdaw_track_strip_refresh_ports(strip);

    /* Start VU meter refresh timer */
    strip->vu_timer = g_timeout_add(50, vu_timer_cb, strip);

    return GTK_WIDGET(strip);
}

/* ---- Height-adaptive collapse --------------------------------------------
 * As the user drags a track shorter than its full layout, drop the rows that
 * no longer fit so the strip never blocks the row from shrinking. The name
 * entry is always kept, so the smallest a track collapses to is its name.
 *   >= TS_FULL_MIN : name + controls + input combo
 *   >= TS_CTRL_MIN : name + controls (no input combo)
 *   <  TS_CTRL_MIN : name only
 */
#define TS_FULL_MIN 72   /* px: room for all three rows */
#define TS_CTRL_MIN 48   /* px: room for name + control row */

void jackdaw_track_strip_set_height(JackDawTrackStrip *strip, gint content_h)
{
    g_return_if_fail(JACKDAW_IS_TRACK_STRIP(strip));

    gboolean show_input = content_h >= TS_FULL_MIN;
    gboolean show_ctrl  = content_h >= TS_CTRL_MIN;

    if (strip->input_button)
        gtk_widget_set_visible(strip->input_button, show_input);
    if (strip->ctrl_row)
        gtk_widget_set_visible(strip->ctrl_row, show_ctrl);
    if (strip->vu_meter)
        gtk_widget_set_visible(strip->vu_meter, show_ctrl);
}

GtkWidget *jackdaw_track_strip_get_vu_meter(JackDawTrackStrip *strip)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK_STRIP(strip), NULL);
    return strip->vu_meter;
}
