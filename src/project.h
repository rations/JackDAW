#ifndef PROJECT_H_INCLUDED
#define PROJECT_H_INCLUDED

#include <glib-object.h>
#include "track.h"

G_BEGIN_DECLS

#define JACKDAW_TYPE_PROJECT (jackdaw_project_get_type())
#define JACKDAW_PROJECT(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST(obj, JACKDAW_TYPE_PROJECT, JackDawProject))
#define JACKDAW_IS_PROJECT(obj) \
    (G_TYPE_CHECK_INSTANCE_TYPE(obj, JACKDAW_TYPE_PROJECT))

typedef struct _JackDawProject      JackDawProject;
typedef struct _JackDawProjectClass JackDawProjectClass;

/* Ruler display mode */
typedef enum {
    JACKDAW_RULER_TIME = 0,   /* HH:MM:SS / samples (existing) */
    JACKDAW_RULER_BARS        /* bars.beats from BPM + time signature */
} JackDawRulerMode;

struct _JackDawProject {
    GObject parent_instance;

    GPtrArray   *tracks;          /* array of JackDawTrack* (strong refs) */
    gchar       *project_file;    /* NULL if unsaved */

    gfloat       master_volume;
    volatile gpointer master_rt_chain;  /* swapped atomically (Phase 5) */
    JackDawTrack *master_track;         /* the master bus as a real track
                                         * (owns master gain/FX/mute; NOT in tracks) */

    /* JACK port counts (0 = auto-detect from physical JACK ports at startup).
     * Non-zero values are user overrides saved in the inifile. */
    guint        audio_in_count;
    guint        audio_out_count;
    guint        midi_in_count;
    guint        midi_out_count;

    /* Tempo / grid (Phase 3) */
    gdouble          bpm;            /* beats per minute (default 120) */
    guint            beats_per_bar;  /* time-sig numerator (default 4) */
    guint            beat_unit;      /* time-sig denominator (default 4) */
    gboolean         grid_enabled;   /* draw beat/bar grid on tracks */
    gboolean         snap_enabled;   /* snap edits/cursor to grid */
    gboolean         metronome_enabled;
    JackDawRulerMode ruler_mode;
};

struct _JackDawProjectClass {
    GObjectClass parent_class;

    void (*track_added)  (JackDawProject *project, JackDawTrack *track);
    void (*track_removed)(JackDawProject *project, JackDawTrack *track);
    void (*ports_changed)(JackDawProject *project);
    void (*timing_changed)(JackDawProject *project);
};

GType          jackdaw_project_get_type(void);
JackDawProject *jackdaw_project_new(void);

/* Track management */
void          jackdaw_project_add_track   (JackDawProject *p, JackDawTrack *t);
void          jackdaw_project_remove_track(JackDawProject *p, JackDawTrack *t);
guint         jackdaw_project_track_count (JackDawProject *p);
JackDawTrack *jackdaw_project_get_track   (JackDawProject *p, guint idx);
/* Index of t in the track array, or -1 if not present. */
gint          jackdaw_project_track_index (JackDawProject *p, JackDawTrack *t);
/* Move the track at `from` to position `to`, shifting the others. The engine
 * slot / JACK ports are unaffected (they follow track->slot, not array order);
 * this only changes display + save order. */
void          jackdaw_project_move_track  (JackDawProject *p, guint from, guint to);

/* Master volume */
void   jackdaw_project_set_master_volume(JackDawProject *p, gfloat vol);
gfloat jackdaw_project_get_master_volume(JackDawProject *p);

/* The master bus track (owns master gain, FX chain, mute). Never NULL. */
JackDawTrack *jackdaw_project_get_master_track(JackDawProject *p);

/* Project file */
void         jackdaw_project_set_file(JackDawProject *p, const gchar *path);
const gchar *jackdaw_project_get_file(JackDawProject *p);

/* Save/load the whole session (tracks: audio regions, MIDI regions+notes, FX
 * chain incl. the instrument; tempo/grid/master). Boolean convention: TRUE =
 * failure, FALSE = success (matches the codebase). load() first clears all
 * existing tracks and re-registers the loaded ones with the engine. */
gboolean     jackdaw_project_save(JackDawProject *p, const gchar *path);
gboolean     jackdaw_project_load(JackDawProject *p, const gchar *path);

/* Signal to refresh port selectors after port count change */
void jackdaw_project_emit_ports_changed(JackDawProject *p);

/* ---- Tempo / grid (emit "timing-changed" on set) ---- */
void     jackdaw_project_set_bpm          (JackDawProject *p, gdouble bpm);
gdouble  jackdaw_project_get_bpm          (JackDawProject *p);
void     jackdaw_project_set_time_signature(JackDawProject *p,
                                            guint num, guint den);
void     jackdaw_project_set_grid_enabled (JackDawProject *p, gboolean on);
void     jackdaw_project_set_snap_enabled (JackDawProject *p, gboolean on);
void     jackdaw_project_set_metronome    (JackDawProject *p, gboolean on);
void     jackdaw_project_set_ruler_mode   (JackDawProject *p, JackDawRulerMode m);
void     jackdaw_project_emit_timing_changed(JackDawProject *p);

/* Grid geometry helpers (timeline frames at the given sample rate). */
gdouble  jackdaw_project_frames_per_beat(JackDawProject *p, guint32 sample_rate);
gdouble  jackdaw_project_frames_per_bar (JackDawProject *p, guint32 sample_rate);
/* Snap a timeline frame to the nearest beat when snap is enabled. */
off_t    jackdaw_project_snap_frame(JackDawProject *p, off_t frame,
                                    guint32 sample_rate);

G_END_DECLS

#endif /* PROJECT_H_INCLUDED */
