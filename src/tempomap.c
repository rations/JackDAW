#include <math.h>

#include "tempomap.h"

void tempomap_from_project(TempoMap *tm, JackDawProject *p, guint32 sample_rate)
{
    tm->bpm = (p && p->bpm > 0.0) ? p->bpm : 120.0;
    tm->beats_per_bar = (p && p->beats_per_bar) ? p->beats_per_bar : 4;
    tm->beat_unit = (p && p->beat_unit) ? p->beat_unit : 4;
    tm->sample_rate = sample_rate ? sample_rate : 48000;
}

gdouble tempomap_frames_per_beat(const TempoMap *tm)
{
    if (tm->bpm <= 0.0)
        return 0.0;
    return (gdouble)tm->sample_rate * 60.0 / tm->bpm;
}

gdouble tempomap_frames_per_bar(const TempoMap *tm)
{
    return tempomap_frames_per_beat(tm) * (gdouble)(tm->beats_per_bar ? tm->beats_per_bar : 1);
}

gdouble tempomap_frame_to_beat(const TempoMap *tm, off_t frame)
{
    gdouble fpb = tempomap_frames_per_beat(tm);
    if (fpb <= 0.0)
        return 0.0;
    return (gdouble)frame / fpb;
}

off_t tempomap_beat_to_frame(const TempoMap *tm, gdouble beat)
{
    gdouble fpb = tempomap_frames_per_beat(tm);
    off_t f = (off_t)(beat * fpb + 0.5);
    return f < 0 ? 0 : f;
}

gdouble tempomap_frame_to_seconds(const TempoMap *tm, off_t frame)
{
    if (tm->sample_rate == 0)
        return 0.0;
    return (gdouble)frame / (gdouble)tm->sample_rate;
}

void tempomap_frame_to_bbt(const TempoMap *tm, off_t frame, TempoMapBBT *out)
{
    guint bpb = tm->beats_per_bar ? tm->beats_per_bar : 4;
    gdouble beat_f = tempomap_frame_to_beat(tm, frame);
    if (beat_f < 0.0)
        beat_f = 0.0;

    /* Ticks per beat: one beat is a 1/beat_unit note; a quarter carries
     * TEMPOMAP_PPQ ticks. */
    gdouble tpb = (gdouble)TEMPOMAP_PPQ * 4.0 / (gdouble)(tm->beat_unit ? tm->beat_unit : 4);

    off_t whole_beat = (off_t)beat_f;
    gdouble frac = beat_f - (gdouble)whole_beat;

    out->bar = (guint)(whole_beat / bpb) + 1;
    out->beat = (guint)(whole_beat % bpb) + 1;
    out->tick = (guint)(frac * tpb);
}

gdouble tempomap_grid_frames(const TempoMap *tm, TempoMapGrid grid)
{
    gdouble fpb = tempomap_frames_per_beat(tm);
    switch (grid) {
        case TEMPOMAP_GRID_BAR:
            return tempomap_frames_per_bar(tm);
        case TEMPOMAP_GRID_BEAT:
            return fpb;
        case TEMPOMAP_GRID_BEAT_DIV2:
            return fpb / 2.0;
        case TEMPOMAP_GRID_BEAT_DIV3:
            return fpb / 3.0;
        case TEMPOMAP_GRID_BEAT_DIV4:
            return fpb / 4.0;
        default:
            return fpb;
    }
}

off_t tempomap_snap_frame(const TempoMap *tm, off_t frame, TempoMapGrid grid)
{
    gdouble step = tempomap_grid_frames(tm, grid);
    if (step <= 0.0)
        return frame < 0 ? 0 : frame;
    gdouble n = (gdouble)frame / step;
    off_t snapped = (off_t)(floor(n + 0.5) * step + 0.5);
    return snapped < 0 ? 0 : snapped;
}

const gchar *tempomap_grid_name(TempoMapGrid grid)
{
    switch (grid) {
        case TEMPOMAP_GRID_BAR:
            return "Bar";
        case TEMPOMAP_GRID_BEAT:
            return "Beat";
        case TEMPOMAP_GRID_BEAT_DIV2:
            return "1/2 Beat";
        case TEMPOMAP_GRID_BEAT_DIV3:
            return "1/3 Beat";
        case TEMPOMAP_GRID_BEAT_DIV4:
            return "1/4 Beat";
        default:
            return "Beat";
    }
}
