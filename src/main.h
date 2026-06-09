#ifndef MAIN_H_INCLUDED
#define MAIN_H_INCLUDED

#include <glib.h>
#include <sys/types.h>   /* off_t */

/* ---- Time display modes ---- */
#define TIMEMODE_REAL      0
#define TIMEMODE_REALLONG  1
#define TIMEMODE_SAMPLES   2
#define TIMEMODE_24FPS     3
#define TIMEMODE_25FPS     4
#define TIMEMODE_NTSC      5
#define TIMEMODE_30FPS     6

extern guint default_timescale_mode;

/*
 * Format a sample position as a time string.
 * timebuf must be at least 64 bytes; pass NULL to use an internal static buffer.
 * Returns timebuf (or the internal buffer).
 */
gchar *get_time(guint32 samplerate, off_t samples, off_t samplemax,
                gchar *timebuf, gint mode);

/*
 * Compute ruler tick positions for a sample range.
 * Returns 1 if midpoints are populated, 0 otherwise.
 */
guint find_timescale_points(guint32 samplerate,
                             off_t start_samp, off_t end_samp,
                             off_t *points,       int *npoints,
                             off_t *midpoints,    int *nmidpoints,
                             off_t *minor_points, int *nminorpoints,
                             int timemode);

#define ARRAY_LENGTH(a) (sizeof(a) / sizeof((a)[0]))

#endif /* MAIN_H_INCLUDED */
