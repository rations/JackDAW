#define _GNU_SOURCE
#include <config.h>
#include <math.h>

#include "mixer.h"
#include "knob.h"
#include "track.h"
#include "jackdaw-engine.h"

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
    gboolean      suppress;
} MixerStrip;

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
    if (p >= 0.75) return (p - 0.75) / 0.25 * 6.0;   /* 0 .. +6 dB */
    return -60.0 * (1.0 - p / 0.75);                  /* -60 .. 0 dB */
}

static double fader_db_to_pos(double db)
{
    if (db >= 0.0)   return 0.75 + CLAMP(db, 0.0, 6.0) / 6.0 * 0.25;
    if (db <= -60.0) return 0.0;
    return 0.75 * (1.0 + db / 60.0);
}

/* ---- Callbacks ---- */

static void mix_fader_changed(GtkRange *range, gpointer data)
{
    MixerStrip *s = data;
    if (s->suppress) return;
    double db  = fader_pos_to_db(gtk_range_get_value(range));
    gfloat lin = (db <= -59.5) ? 0.0f : (gfloat)pow(10.0, db / 20.0);
    if (s->track)
        jackdaw_track_set_volume(s->track, lin);
    else
        jackdaw_project_set_master_volume(s->mixer->project, lin);
}

static void mix_pan_changed(double pan, gpointer data)
{
    MixerStrip *s = data;
    if (s->suppress || !s->track) return;
    jackdaw_track_set_pan(s->track, (gfloat)pan);
}

static void mix_mute_toggled(GtkToggleButton *b, gpointer data)
{
    MixerStrip *s = data;
    if (s->suppress || !s->track) return;
    jackdaw_track_set_muted(s->track, gtk_toggle_button_get_active(b));
}

static void mix_solo_toggled(GtkToggleButton *b, gpointer data)
{
    MixerStrip *s = data;
    if (s->suppress || !s->track) return;
    jackdaw_track_set_soloed(s->track, gtk_toggle_button_get_active(b));
}

/* ---- Strip construction ---- */

static GtkWidget *mixer_strip_new(JackDawMixer *mixer, JackDawTrack *track)
{
    MixerStrip *s = g_new0(MixerStrip, 1);
    s->mixer = mixer;
    s->track = track;

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_size_request(box, 64, -1);
    gtk_container_set_border_width(GTK_CONTAINER(box), 3);
    g_object_set_data_full(G_OBJECT(box), "mixer-strip", s, g_free);

    /* Name */
    GtkWidget *name = gtk_label_new(track ? jackdaw_track_get_name(track)
                                          : "Master");
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
    gtk_widget_set_size_request(name, 58, -1);
    gtk_box_pack_start(GTK_BOX(box), name, FALSE, FALSE, 0);

    /* Pan (tracks only) */
    if (track) {
        s->pan = knob_new(-1.0, 1.0, (double)jackdaw_track_get_pan(track),
                          0.0, KNOB_PAN, mix_pan_changed, s);
        gtk_widget_set_halign(s->pan, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(box), s->pan, FALSE, FALSE, 0);
    }

    /* Fader + VU side by side, expanding vertically */
    GtkWidget *mid = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);

    s->fader = gtk_scale_new_with_range(GTK_ORIENTATION_VERTICAL,
                                        0.0, 1.0, 0.005);
    gtk_range_set_inverted(GTK_RANGE(s->fader), TRUE);  /* up = louder */
    gtk_scale_set_draw_value(GTK_SCALE(s->fader), FALSE);
    gtk_widget_set_size_request(s->fader, 34, 130);
    /* Reference marks at 0 dB (~¾) and +6 dB (top). */
    gtk_scale_add_mark(GTK_SCALE(s->fader), fader_db_to_pos(0.0),
                       GTK_POS_LEFT, "0");
    gtk_scale_add_mark(GTK_SCALE(s->fader), fader_db_to_pos(-12.0),
                       GTK_POS_LEFT, "-12");
    {
        gfloat vol = track ? jackdaw_track_get_volume(track)
                           : jackdaw_project_get_master_volume(mixer->project);
        double db  = (vol > 0.0001f) ? 20.0 * log10((double)vol) : -60.0;
        s->suppress = TRUE;
        gtk_range_set_value(GTK_RANGE(s->fader), fader_db_to_pos(db));
        s->suppress = FALSE;
    }
    g_signal_connect(s->fader, "value-changed",
                     G_CALLBACK(mix_fader_changed), s);

    s->vu = gtk_drawing_area_new();
    gtk_widget_set_size_request(s->vu, 18, 120);
    g_signal_connect(s->vu, "draw", G_CALLBACK(mix_vu_draw), s);

    gtk_box_pack_start(GTK_BOX(mid), s->fader, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(mid), s->vu,    FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), mid, TRUE, TRUE, 0);

    /* Mute / solo (tracks only) */
    if (track) {
        GtkWidget *ms = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
        s->btn_mute = gtk_toggle_button_new_with_label("M");
        s->btn_solo = gtk_toggle_button_new_with_label("S");
        gtk_widget_set_size_request(s->btn_mute, 26, 20);
        gtk_widget_set_size_request(s->btn_solo, 26, 20);
        s->suppress = TRUE;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->btn_mute),
                                     jackdaw_track_is_muted(track));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->btn_solo),
                                     jackdaw_track_is_soloed(track));
        s->suppress = FALSE;
        g_signal_connect(s->btn_mute, "toggled",
                         G_CALLBACK(mix_mute_toggled), s);
        g_signal_connect(s->btn_solo, "toggled",
                         G_CALLBACK(mix_solo_toggled), s);
        gtk_box_pack_start(GTK_BOX(ms), s->btn_mute, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(ms), s->btn_solo, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(box), ms, FALSE, FALSE, 0);
    }

    if (!track) mixer->master = s;
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
    }

    GHashTableIter it; gpointer k, v;
    g_hash_table_iter_init(&it, m->strips);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        MixerStrip *s = g_object_get_data(G_OBJECT(v), "mixer-strip");
        if (!s || !s->track) continue;
        gfloat l = 0.0f, r = 0.0f;
        jackdaw_track_get_peaks(s->track, &l, &r);
        s->pk_L = (l > s->pk_L) ? l : s->pk_L * 0.89f;
        s->pk_R = (r > s->pk_R) ? r : s->pk_R * 0.89f;
        gtk_widget_queue_draw(s->vu);
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
    GtkWidget *master = mixer_strip_new(m, NULL);
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
