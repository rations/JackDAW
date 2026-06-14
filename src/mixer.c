#define _GNU_SOURCE
#include <config.h>
#include <math.h>

#include "mixer.h"
#include "knob.h"
#include "track.h"
#include "fxwindow.h"
#include "jackdaw-engine.h"
#include "settings.h"

G_DEFINE_TYPE(JackDawMixer, jackdaw_mixer, GTK_TYPE_BOX)

/* One channel strip. track == NULL marks the master strip. */
typedef struct {
    JackDawMixer *mixer;
    JackDawTrack *track;       /* NULL = master */
    GtkWidget    *vu;
    gfloat        pk_L, pk_R;
    GtkWidget    *fader;       /* vertical GtkScale, dB */
    GtkWidget    *pan;         /* knob (tracks only) */
    GtkWidget    *btn_mute;
    GtkWidget    *btn_solo;
    GtkWidget    *btn_fx;
    GtkWidget    *db_popup;    /* floating "+4.25dB" readout while dragging */
    GtkWidget    *db_popup_lbl;
    gboolean      suppress;    /* gate UI callbacks during programmatic set */
    gboolean      self_update; /* this strip is the source of a track change */
} MixerStrip;

/* ---- Fader taper ----
 * The fader spans -42 dB (bottom) .. +12 dB (top), linear in dB so the motion
 * is smooth and the 6 dB tick marks are evenly spaced. 0 dB (unity) sits at
 * 42/54 ≈ 0.78 of the travel. */
#define FADER_DB_MIN   (-42.0)
#define FADER_DB_MAX   ( 12.0)
#define FADER_DB_SPAN  ( 54.0)   /* MAX - MIN */

/* ---- VU meter (same look as the track strip meter) ---- */

static gboolean mix_vu_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    MixerStrip *s = data;
    GtkAllocation a;
    gtk_widget_get_allocation(w, &a);

    cairo_set_source_rgb(cr, 0.08, 0.08, 0.08);
    cairo_paint(cr);

    gfloat peaks[2] = { s->pk_L, s->pk_R };
    gint   bar_w    = (a.width - 3) / 2;
    for (int ch = 0; ch < 2; ch++) {
        gint bx = (ch == 0) ? 1 : (2 + bar_w);
        cairo_set_source_rgb(cr, 0.18, 0.18, 0.18);
        cairo_rectangle(cr, bx, 0, bar_w, a.height);
        cairo_fill(cr);

        gfloat pk = peaks[ch];
        if (pk > 0.0001f) {
            float db   = 20.0f * log10f(pk);
            float dbc  = CLAMP(db, -60.0f, 6.0f);
            float frac = (dbc + 60.0f) / 66.0f;
            gint  fh   = (gint)(frac * (float)a.height);
            if (fh > a.height) fh = a.height;
            if (fh > 0) {
                if (db >= 0.0f)        cairo_set_source_rgb(cr, 0.90, 0.15, 0.15);
                else if (db >= -12.0f) cairo_set_source_rgb(cr, 0.85, 0.78, 0.10);
                else                   cairo_set_source_rgb(cr, 0.15, 0.68, 0.20);
                cairo_rectangle(cr, bx, a.height - fh, bar_w, fh);
                cairo_fill(cr);
            }
        }
    }
    return FALSE;
}

/* ---- Fader taper ----
 * The fader runs 0..1 (bottom..top). 0 dB (unity) sits at 0.75 so there is a
 * +6 dB region in the top quarter and a -inf..0 dB region in the bottom three
 * quarters — like a normal mixing-desk fader. */
static double fader_pos_to_db(double p)
{
    return FADER_DB_MIN + CLAMP(p, 0.0, 1.0) * FADER_DB_SPAN;
}

static double fader_db_to_pos(double db)
{
    return (CLAMP(db, FADER_DB_MIN, FADER_DB_MAX) - FADER_DB_MIN) / FADER_DB_SPAN;
}

/* dB scale column drawn beside the fader. We draw the 6 dB labels ourselves
 * instead of using gtk_scale_add_mark(), because scale marks act as snap
 * "stop values" and make the fader drag in steps rather than smoothly.
 *
 * FADER_SLIDER_HALF is half the styled slider cap's height (see the mix-fader
 * CSS); the trough margins equal it, so the slider centre travels from
 * FADER_SLIDER_HALF..(height-FADER_SLIDER_HALF) and the labels line up. */
#define FADER_SLIDER_HALF 7.0

static gboolean mix_scale_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    (void)data;
    GtkAllocation a; gtk_widget_get_allocation(w, &a);
    double usable = a.height - 2.0 * FADER_SLIDER_HALF;
    if (usable < 1.0) usable = a.height;

    /* dB label colour follows the chrome theme: dark text on light, light
     * text on dark (the scale shares the GTK panel background). */
    gboolean dark = settings_get_uint32("dark_mode", 1) != 0;

    cairo_set_font_size(cr, 14.0);
    for (int d = (int)FADER_DB_MAX; d >= (int)FADER_DB_MIN; d -= 6) {
        double pos = fader_db_to_pos((double)d);          /* 1 = top */
        double y   = FADER_SLIDER_HALF + (1.0 - pos) * usable;
        char m[8]; g_snprintf(m, sizeof m, "%d", d);
        /* tick (right edge, next to the fader) */
        cairo_set_source_rgb(cr, 0.44, 0.44, 0.47);
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, a.width - 6.0, floor(y) + 0.5);
        cairo_line_to(cr, a.width - 1.0, floor(y) + 0.5);
        cairo_stroke(cr);
        /* right-aligned label, vertically centred on the tick */
        cairo_text_extents_t ext; cairo_text_extents(cr, m, &ext);
        if (dark) cairo_set_source_rgb(cr, 0.90, 0.90, 0.90); /* light */
        else      cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);    /* black */
        cairo_move_to(cr, a.width - 9.0 - ext.width,
                          y - (ext.height / 2.0 + ext.y_bearing));
        cairo_show_text(cr, m);
    }
    return FALSE;
}

/* ---- Floating dB read-out (shown beside the fader while dragging) ---- */

static void mix_db_popup_show(MixerStrip *s, double db)
{
    if (!s->db_popup) {
        s->db_popup = gtk_window_new(GTK_WINDOW_POPUP);
        gtk_window_set_type_hint(GTK_WINDOW(s->db_popup),
                                 GDK_WINDOW_TYPE_HINT_TOOLTIP);
        gtk_window_set_resizable(GTK_WINDOW(s->db_popup), FALSE);
        s->db_popup_lbl = gtk_label_new("");
        gtk_style_context_add_class(
            gtk_widget_get_style_context(s->db_popup_lbl), "mix-db-pop");
        gtk_container_add(GTK_CONTAINER(s->db_popup), s->db_popup_lbl);
        gtk_widget_show(s->db_popup_lbl);
    }

    char buf[32];
    if (db <= FADER_DB_MIN + 0.01) g_snprintf(buf, sizeof buf, "-inf");
    else                           g_snprintf(buf, sizeof buf, "%+.2fdB", db);
    gtk_label_set_text(GTK_LABEL(s->db_popup_lbl), buf);

    /* Position to the right of the fader, level with the current handle. */
    GtkWidget  *top = gtk_widget_get_toplevel(s->fader);
    GdkWindow  *tw  = top ? gtk_widget_get_window(top) : NULL;
    if (tw) {
        gint ox, oy, fx = 0, fy = 0;
        gdk_window_get_origin(tw, &ox, &oy);
        gtk_widget_translate_coordinates(s->fader, top, 0, 0, &fx, &fy);
        GtkAllocation fa; gtk_widget_get_allocation(s->fader, &fa);
        double pos = gtk_range_get_value(GTK_RANGE(s->fader)); /* 0..1, 1 = top */
        gtk_window_move(GTK_WINDOW(s->db_popup),
                        ox + fx + fa.width + 2,
                        oy + fy + (gint)((1.0 - pos) * fa.height) - 9);
    }
    gtk_widget_show(s->db_popup);
}

static void mix_db_popup_hide(MixerStrip *s)
{
    if (s->db_popup) gtk_widget_hide(s->db_popup);
}

/* ---- Callbacks ---- */

static void mix_fader_changed(GtkRange *range, gpointer data)
{
    MixerStrip *s = data;
    if (s->suppress) return;
    double db  = fader_pos_to_db(gtk_range_get_value(range));
    gfloat lin = (gfloat)pow(10.0, db / 20.0);
    s->self_update = TRUE;
    if (s->track)
        jackdaw_track_set_fader(s->track, lin);   /* fader = channel level stage */
    else
        jackdaw_project_set_master_volume(s->mixer->project, lin);
    s->self_update = FALSE;
    mix_db_popup_show(s, db);
}

static gboolean mix_fader_button(GtkWidget *w, GdkEventButton *e, gpointer data)
{
    (void)w;
    MixerStrip *s = data;
    /* Double-click returns the fader to 0 dB (unity). */
    if (e->type == GDK_2BUTTON_PRESS && e->button == 1) {
        gtk_range_set_value(GTK_RANGE(s->fader), fader_db_to_pos(0.0));
        return TRUE;
    }
    return FALSE;
}

static gboolean mix_fader_release(GtkWidget *w, GdkEventButton *e, gpointer data)
{
    (void)w; (void)e;
    mix_db_popup_hide((MixerStrip *)data);
    return FALSE;
}

static void mix_pan_changed(double pan, gpointer data)
{
    MixerStrip *s = data;
    if (s->suppress || !s->track) return;
    s->self_update = TRUE;
    jackdaw_track_set_pan(s->track, (gfloat)pan);
    s->self_update = FALSE;
}

static void mix_mute_toggled(GtkToggleButton *b, gpointer data)
{
    MixerStrip *s = data;
    if (s->suppress || !s->track) return;
    s->self_update = TRUE;
    jackdaw_track_set_muted(s->track, gtk_toggle_button_get_active(b));
    s->self_update = FALSE;
}

static void mix_solo_toggled(GtkToggleButton *b, gpointer data)
{
    MixerStrip *s = data;
    if (s->suppress || !s->track) return;
    s->self_update = TRUE;
    jackdaw_track_set_soloed(s->track, gtk_toggle_button_get_active(b));
    s->self_update = FALSE;
}

static void mix_fx_clicked(GtkButton *b, gpointer data)
{
    (void)b;
    MixerStrip *s = data;
    if (s->track) jackdaw_fx_window_open(s->track, s->mixer->project);
}

/* Reflect external track changes (e.g. from the track strip) onto this strip. */
static void mix_track_state_changed(JackDawTrack *t, gpointer data)
{
    MixerStrip *s = g_object_get_data(G_OBJECT(data), "mixer-strip");
    if (!s || s->self_update) return;
    s->suppress = TRUE;
    if (s->btn_mute)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->btn_mute),
                                     jackdaw_track_is_muted(t));
    if (s->btn_solo)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->btn_solo),
                                     jackdaw_track_is_soloed(t));
    if (s->pan)
        knob_set_value(s->pan, (double)jackdaw_track_get_pan(t));
    s->suppress = FALSE;
}

static void mix_strip_destroy(GtkWidget *w, gpointer data)
{
    (void)w;
    MixerStrip *s = data;
    if (s->db_popup) { gtk_widget_destroy(s->db_popup); s->db_popup = NULL; }
}

/* ---- Strip construction ---- */

static GtkWidget *mixer_strip_new(JackDawMixer *mixer, JackDawTrack *track)
{
    MixerStrip *s = g_new0(MixerStrip, 1);
    s->mixer = mixer;
    s->track = track;

    gboolean is_master =
        (track && track == jackdaw_project_get_master_track(mixer->project));

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_size_request(box, 80, -1);
    gtk_container_set_border_width(GTK_CONTAINER(box), 3);
    g_object_set_data_full(G_OBJECT(box), "mixer-strip", s, g_free);

    /* Name */
    GtkWidget *name = gtk_label_new(track ? jackdaw_track_get_name(track)
                                          : "Master");
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
    gtk_widget_set_size_request(name, 58, -1);
    gtk_box_pack_start(GTK_BOX(box), name, FALSE, FALSE, 0);

    /* Pan (real tracks only — not the master bus) */
    if (track && !is_master) {
        s->pan = knob_new(-1.0, 1.0, (double)jackdaw_track_get_pan(track),
                          0.0, KNOB_PAN, mix_pan_changed, s);
        gtk_widget_set_halign(s->pan, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(box), s->pan, FALSE, FALSE, 0);
    }

    /* Fader + VU side by side, expanding vertically */
    GtkWidget *mid = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);

    s->fader = gtk_scale_new_with_range(GTK_ORIENTATION_VERTICAL,
                                        0.0, 1.0, 0.001);   /* fine = smooth */
    gtk_range_set_inverted(GTK_RANGE(s->fader), TRUE);  /* up = louder */
    gtk_scale_set_draw_value(GTK_SCALE(s->fader), FALSE);
    /* Fine value resolution so the motion is continuous (not stepped). */
    gtk_scale_set_digits(GTK_SCALE(s->fader), 3);
    gtk_range_set_round_digits(GTK_RANGE(s->fader), 3);
    gtk_widget_set_size_request(s->fader, 26, 150);
    gtk_style_context_add_class(gtk_widget_get_style_context(s->fader),
                                "mix-fader");
    {
        gfloat vol = track ? jackdaw_track_get_fader(track)
                           : jackdaw_project_get_master_volume(mixer->project);
        double db  = (vol > 0.0001f) ? 20.0 * log10((double)vol) : FADER_DB_MIN;
        s->suppress = TRUE;
        gtk_range_set_value(GTK_RANGE(s->fader), fader_db_to_pos(db));
        s->suppress = FALSE;
    }
    gtk_widget_add_events(s->fader, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
    g_signal_connect(s->fader, "value-changed",
                     G_CALLBACK(mix_fader_changed), s);
    g_signal_connect(s->fader, "button-press-event",
                     G_CALLBACK(mix_fader_button), s);
    g_signal_connect(s->fader, "button-release-event",
                     G_CALLBACK(mix_fader_release), s);

    s->vu = gtk_drawing_area_new();
    gtk_widget_set_size_request(s->vu, 18, 120);
    g_signal_connect(s->vu, "draw", G_CALLBACK(mix_vu_draw), s);

    /* dB scale labels (drawn, not GtkScale marks — see mix_scale_draw). */
    GtkWidget *scale_lbl = gtk_drawing_area_new();
    gtk_widget_set_size_request(scale_lbl, 34, -1);
    gtk_widget_set_vexpand(scale_lbl, TRUE);
    g_signal_connect(scale_lbl, "draw", G_CALLBACK(mix_scale_draw), NULL);

    gtk_box_pack_start(GTK_BOX(mid), scale_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(mid), s->fader,  FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(mid), s->vu,     FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), mid, TRUE, TRUE, 0);

    /* Mute / Solo / Fx (tracks only) — same look & padding as the track strip.
     * The master bus has no Solo (it would be a no-op against the solo bus). */
    if (track) {
        GtkWidget *ms = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
        gtk_widget_set_halign(ms, GTK_ALIGN_CENTER);
        s->btn_mute = gtk_toggle_button_new_with_label("M");
        s->btn_fx   = gtk_button_new_with_label("Fx");
        gtk_widget_set_size_request(s->btn_mute, 20, 20);
        gtk_widget_set_size_request(s->btn_fx,   24, 20);
        gtk_widget_set_tooltip_text(s->btn_mute, "Mute");
        gtk_widget_set_tooltip_text(s->btn_fx,   "Open the effects window for this track");
        gtk_style_context_add_class(gtk_widget_get_style_context(s->btn_mute), "ts-mute");
        gtk_style_context_add_class(gtk_widget_get_style_context(s->btn_fx),   "ts-fx");
        s->suppress = TRUE;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->btn_mute),
                                     jackdaw_track_is_muted(track));
        s->suppress = FALSE;
        g_signal_connect(s->btn_mute, "toggled",
                         G_CALLBACK(mix_mute_toggled), s);
        g_signal_connect(s->btn_fx, "clicked",
                         G_CALLBACK(mix_fx_clicked), s);
        gtk_box_pack_start(GTK_BOX(ms), s->btn_mute, FALSE, FALSE, 0);
        if (!is_master) {                       /* real tracks keep Solo */
            s->btn_solo = gtk_toggle_button_new_with_label("S");
            gtk_widget_set_size_request(s->btn_solo, 20, 20);
            gtk_widget_set_tooltip_text(s->btn_solo, "Solo");
            gtk_style_context_add_class(gtk_widget_get_style_context(s->btn_solo), "ts-solo");
            s->suppress = TRUE;
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->btn_solo),
                                         jackdaw_track_is_soloed(track));
            s->suppress = FALSE;
            g_signal_connect(s->btn_solo, "toggled",
                             G_CALLBACK(mix_solo_toggled), s);
            gtk_box_pack_start(GTK_BOX(ms), s->btn_solo, FALSE, FALSE, 0);
        }
        gtk_box_pack_start(GTK_BOX(ms), s->btn_fx,   FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), ms, FALSE, FALSE, 0);

        /* Keep this strip in sync when the track is changed elsewhere. */
        g_signal_connect_object(track, "state-changed",
                                G_CALLBACK(mix_track_state_changed), box, 0);
    }

    g_signal_connect(box, "destroy", G_CALLBACK(mix_strip_destroy), s);

    if (is_master) mixer->master = s;
    return box;
}

/* ---- VU refresh timer ---- */

static gboolean mixer_vu_tick(gpointer data)
{
    JackDawMixer *m = data;
    if (!JACKDAW_IS_MIXER(m)) return G_SOURCE_REMOVE;

    /* Master */
    if (m->master) {
        MixerStrip *s = m->master;
        gfloat l = 0.0f, r = 0.0f;
        jackdaw_engine_get_master_peaks(&l, &r);
        s->pk_L = (l > s->pk_L) ? l : s->pk_L * 0.89f;
        s->pk_R = (r > s->pk_R) ? r : s->pk_R * 0.89f;
        gtk_widget_queue_draw(s->vu);
        if (s->btn_fx && s->track) {
            GtkStyleContext *fx = gtk_widget_get_style_context(s->btn_fx);
            if (jackdaw_track_fx_count(s->track) > 0)
                gtk_style_context_add_class(fx, "ts-fx-active");
            else
                gtk_style_context_remove_class(fx, "ts-fx-active");
        }
    }

    GHashTableIter it; gpointer k, v;
    g_hash_table_iter_init(&it, m->strips);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        MixerStrip *s = g_object_get_data(G_OBJECT(v), "mixer-strip");
        if (!s || !s->track) continue;
        gfloat l = 0.0f, r = 0.0f;
        jackdaw_track_get_peaks(s->track, &l, &r);
        s->pk_L = l;
        s->pk_R = r;
        gtk_widget_queue_draw(s->vu);

        if (s->btn_fx) {
            GtkStyleContext *fx = gtk_widget_get_style_context(s->btn_fx);
            if (jackdaw_track_fx_count(s->track) > 0)
                gtk_style_context_add_class(fx, "ts-fx-active");
            else
                gtk_style_context_remove_class(fx, "ts-fx-active");
        }
    }
    return G_SOURCE_CONTINUE;
}

/* ---- Project track add/remove ---- */

static void on_track_added(JackDawProject *p, JackDawTrack *t, gpointer data)
{
    (void)p;
    JackDawMixer *m = JACKDAW_MIXER(data);
    if (g_hash_table_contains(m->strips, t)) return;
    GtkWidget *strip = mixer_strip_new(m, t);
    gtk_box_pack_start(GTK_BOX(m->strips_box), strip, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(m->strips_box),
                       gtk_separator_new(GTK_ORIENTATION_VERTICAL),
                       FALSE, FALSE, 0);
    gtk_widget_show_all(strip);
    g_hash_table_insert(m->strips, t, strip);
}

static void on_track_removed(JackDawProject *p, JackDawTrack *t, gpointer data)
{
    (void)p;
    JackDawMixer *m = JACKDAW_MIXER(data);
    GtkWidget *strip = g_hash_table_lookup(m->strips, t);
    if (!strip) return;
    g_hash_table_remove(m->strips, t);
    gtk_widget_destroy(strip);
}

/* ---- GObject ---- */

static void jackdaw_mixer_finalize(GObject *obj)
{
    JackDawMixer *m = JACKDAW_MIXER(obj);
    if (m->vu_timer) { g_source_remove(m->vu_timer); m->vu_timer = 0; }
    if (m->strips)   g_hash_table_destroy(m->strips);
    if (m->project)  g_object_unref(m->project);
    G_OBJECT_CLASS(jackdaw_mixer_parent_class)->finalize(obj);
}

static void jackdaw_mixer_class_init(JackDawMixerClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = jackdaw_mixer_finalize;
}

static void jackdaw_mixer_init(JackDawMixer *m)
{
    m->project    = NULL;
    m->strips     = g_hash_table_new(g_direct_hash, g_direct_equal);
    m->master     = NULL;
    m->vu_timer   = 0;
    gtk_orientable_set_orientation(GTK_ORIENTABLE(m),
                                   GTK_ORIENTATION_VERTICAL);
}

GtkWidget *jackdaw_mixer_new(JackDawProject *project)
{
    g_return_val_if_fail(JACKDAW_IS_PROJECT(project), NULL);

    JackDawMixer *m = g_object_new(JACKDAW_TYPE_MIXER, NULL);
    m->project = g_object_ref(project);

    GtkWidget *hdr = gtk_label_new("Mixer");
    gtk_widget_set_halign(hdr, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(m), hdr, FALSE, FALSE, 2);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    m->strips_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_container_add(GTK_CONTAINER(scroll), m->strips_box);
    gtk_box_pack_start(GTK_BOX(m), scroll, TRUE, TRUE, 0);

    /* Master strip, pinned far left, then a divider */
    GtkWidget *master = mixer_strip_new(m, jackdaw_project_get_master_track(project));
    gtk_box_pack_start(GTK_BOX(m->strips_box), master, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(m->strips_box),
                       gtk_separator_new(GTK_ORIENTATION_VERTICAL),
                       FALSE, FALSE, 0);

    /* Existing tracks */
    guint n = jackdaw_project_track_count(project);
    for (guint i = 0; i < n; i++)
        on_track_added(project, jackdaw_project_get_track(project, i), m);

    g_signal_connect_object(project, "track-added",
                            G_CALLBACK(on_track_added), m, 0);
    g_signal_connect_object(project, "track-removed",
                            G_CALLBACK(on_track_removed), m, 0);

    m->vu_timer = g_timeout_add(50, mixer_vu_tick, m);

    gtk_widget_show_all(GTK_WIDGET(m));
    return GTK_WIDGET(m);
}
