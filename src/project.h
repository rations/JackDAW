#ifndef PROJECT_H_INCLUDED
#define PROJECT_H_INCLUDED

#include <glib-object.h>
#include "track.h"
#include "undo.h"

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

/* Metronome routing: where the click is heard. The dedicated "metronome" JACK
 * output always carries the click; this only controls the main outs. */
typedef enum {
    METRONOME_ROUTE_MAIN = 0,    /* click on main outs + dedicated metro port */
    METRONOME_ROUTE_CLICK_PORT   /* "headphones only": dedicated metro port only */
} JackDawMetronomeRoute;

struct _JackDawProject {
    GObject parent_instance;

    GPtrArray   *tracks;          /* array of JackDawTrack* (strong refs) */
    GPtrArray   *sel_tracks;      /* multi-selection: borrowed JackDawTrack* */
    JackDawTrack *active_track;   /* primary/active track (borrowed, weak): the
                                   * single target for undo/paste/keyboard ops.
                                   * Always a member of sel_tracks, or NULL. */
    JackDawUndoManager *undo;     /* global undo/redo history (owned) */
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
    gdouble          metronome_volume_db; /* user-set, -25..+25 dB (0 = unity) */
    gfloat           metronome_gain;      /* linear gain derived from the dB,
                                           * read by the RT process callback */
    gint             metronome_route;     /* JackDawMetronomeRoute; read by RT */
    JackDawRulerMode ruler_mode;
};

struct _JackDawProjectClass {
    GObjectClass parent_class;

    void (*track_added)  (JackDawProject *project, JackDawTrack *track);
    void (*track_removed)(JackDawProject *project, JackDawTrack *track);
    void (*ports_changed)(JackDawProject *project);
    void (*timing_changed)(JackDawProject *project);
    void (*selection_changed)(JackDawProject *project);
    void (*tracks_reordered)(JackDawProject *project);
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

/* Emit "tracks-reordered" so views resync their row order to the array order. */
void          jackdaw_project_emit_tracks_reordered(JackDawProject *p);

/* Capture the current track list as an undo checkpoint. Call BEFORE a
 * structural change (add/delete/reorder) so a single Ctrl+Z restores the whole
 * list — including a deleted track (kept alive on the undo stack) at its
 * original position. `desc` is a short label (may be NULL). */
void          jackdaw_project_push_structural_undo(JackDawProject *p,
                                                   const char *desc);

/* Delete a track with full undo: pushes a structural checkpoint, then removes
 * the track from the engine and the project. Undo brings it back intact. The
 * single entry point used by every "Delete Track" menu. */
void          jackdaw_project_delete_track(JackDawProject *p, JackDawTrack *t);

/* ---- Track multi-selection (for render "selected tracks") ----
 * A set of tracks the user has marked on the track strips, distinct from the
 * timeline's keyboard focus. Emits "selection-changed" on every change. The
 * returned array is borrowed (do not free/modify); pointers are not ref'd. */
GPtrArray *jackdaw_project_get_selected_tracks(JackDawProject *p);
void       jackdaw_project_select_single  (JackDawProject *p, JackDawTrack *t);
void       jackdaw_project_toggle_selected(JackDawProject *p, JackDawTrack *t);
gboolean   jackdaw_project_is_selected    (JackDawProject *p, JackDawTrack *t);
void       jackdaw_project_clear_selection(JackDawProject *p);

/* The single active/primary track (the undo/paste/keyboard target). It is kept
 * in sync with the selection: select_single makes its track active; toggling
 * the active track off picks another selected track (or NULL). Setting it
 * also adds the track to the selection. May return NULL. */
JackDawTrack *jackdaw_project_get_active_track(JackDawProject *p);
void          jackdaw_project_set_active_track(JackDawProject *p, JackDawTrack *t);

/* ---- Global undo/redo ----
 * One chronological history for the whole project (region edits, track
 * add/delete, MIDI edits). All windows route Ctrl+Z/Ctrl+Y here. */
JackDawUndoManager *jackdaw_project_get_undo(JackDawProject *p);
void                jackdaw_project_undo(JackDawProject *p);
void                jackdaw_project_redo(JackDawProject *p);

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

/* Default root directory for project bundles (~/Music/JackDAW/Projects),
 * created if it does not exist. Use this as the save dialog's start folder.
 * Caller frees the returned string. */
gchar       *jackdaw_default_projects_dir(void);

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
/* Metronome click volume in dB (clamped to -25..+25). Updates the linear gain
 * read by the engine and persists the setting globally. */
void     jackdaw_project_set_metronome_volume(JackDawProject *p, gdouble db);
gdouble  jackdaw_project_get_metronome_volume(JackDawProject *p);
/* Metronome routing (main outs vs. dedicated click port only). */
void     jackdaw_project_set_metronome_route(JackDawProject *p,
                                             JackDawMetronomeRoute route);
JackDawMetronomeRoute jackdaw_project_get_metronome_route(JackDawProject *p);
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
