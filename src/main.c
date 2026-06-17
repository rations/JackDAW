#include <config.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <errno.h>
#include <gtk/gtk.h>
#include <sys/mman.h>   /* mlockall */
#include <malloc.h>     /* mallopt */

#include "main.h"
#include "settings.h"
#include "project.h"
#include "jackdaw-engine.h"
#include "mainwindow.h"
#include "pluginhost.h"
#include "fxwindow.h"

/* ---- Globals ---- */

guint default_timescale_mode = 1;   /* TIMEMODE_REALLONG */

/* -----------------------------------------------------------------------
 * Time formatting
 * ----------------------------------------------------------------------- */

gchar *get_time(guint32 samplerate, off_t samples, off_t samplemax,
                gchar *timebuf, gint mode)
{
    static gchar static_buf[64];
    gfloat secs, ffps;
    guint mins, msecs, hours, maxhours, frames, ifps, isecs;
    guint fptm;

    if (samplemax == 0) samplemax = samples;
    if (!timebuf) timebuf = static_buf;
    if (mode > 6) mode = 0;

    if (mode == 2) {
        g_snprintf(timebuf, 50, "%05ld", (long)samples);
    } else if (mode < 2) {
        secs     = (gfloat)samples / (gfloat)samplerate;
        mins     = (guint)(secs / 60.0f);
        hours    = mins / 60;
        mins     = mins % 60;
        msecs    = ((guint)(secs * 1000.0f)) % 60000;
        maxhours = (guint)(samplemax / ((off_t)samplerate * 3600));
        if (mode == 0) {
            if (maxhours > 0)
                g_snprintf(timebuf, 50, "%d'%02d:%02d.%d",
                           hours, mins, msecs/1000, (msecs%1000)/100);
            else
                g_snprintf(timebuf, 50, "%02d:%02d.%d",
                           mins, msecs/1000, (msecs%1000)/100);
        } else {
            if (maxhours > 0)
                g_snprintf(timebuf, 50, "%d'%02d:%02d.%03d",
                           hours, mins, msecs/1000, msecs%1000);
            else
                g_snprintf(timebuf, 50, "%02d:%02d.%03d",
                           mins, msecs/1000, msecs%1000);
        }
    } else {
        secs = (gfloat)samples / (gfloat)samplerate;

        if      (mode == 3) { ffps = 24.0f; ifps = 24; }
        else if (mode == 4) { ffps = 25.0f; ifps = 25; }
        else if (mode == 5) { ffps = 30.0f * 1000.0f / 1001.0f; ifps = 30; }
        else                { ffps = 30.0f; ifps = 30; }

        frames = (guint)(secs * ffps);

        if (mode == 5)
            fptm = 60 * 30 * 10 - 2 * 9;
        else
            fptm = ifps * 600;

        mins   = 10 * (frames / fptm);
        frames = frames % fptm;

        if (mode != 5) {
            isecs   = frames / ifps;
            frames %= ifps;
            mins   += isecs / 60;
            isecs  %= 60;
        } else {
            if (frames >= 60 * 30) {
                mins++;
                frames -= 60 * 30;
                mins   += frames / (60 * 30 - 2);
                frames %= (60 * 30 - 2);
                frames += 2;
            }
            isecs   = frames / ifps;
            frames %= ifps;
        }

        hours = mins / 60;
        mins  = mins % 60;

        g_snprintf(timebuf, 50, "%02d:%02d:%02d[%02d]",
                   hours, mins, isecs, frames);
    }
    return timebuf;
}

/* -----------------------------------------------------------------------
 * Timescale point generation for the ruler
 * ----------------------------------------------------------------------- */

static const gint bigsizes[] = {
    1, 2, 5, 10, 20, 30, 60, 120, 180, 300, 600, 900, 1800, 3600, 36000
};
static const gboolean bigskip[] = {
    FALSE, TRUE, FALSE, FALSE, TRUE,
    FALSE, FALSE, TRUE, TRUE, FALSE, TRUE,
    TRUE, FALSE, FALSE
};
static const gint smallsizes_real[]  = { 1000, 100, 10 };
static const gint smallsizes_24fps[] = { 24, 12, 4 };
static const gint smallsizes_25fps[] = { 25, 5 };
static const gint smallsizes_30fps[] = { 30, 10, 5 };

guint find_timescale_points(guint32 samplerate,
                             off_t start_samp, off_t end_samp,
                             off_t *points,       int *npoints,
                             off_t *midpoints,    int *nmidpoints,
                             off_t *minor_points, int *nminorpoints,
                             int timemode)
{
    guint pctr = 0, mpctr = 0, midpctr = 0;
    off_t p, q, r, s;
    int i;
    const int *ss;
    int ssl;
    int max_points       = *npoints;
    int max_minorpoints  = *nminorpoints;
    int max_midpoints    = *nmidpoints;
    *nmidpoints = 0;

    if (timemode == TIMEMODE_SAMPLES) {
        p = 1; q = start_samp; r = end_samp;
        while (r - q >= (off_t)(max_points - 1)) { q /= 10; r /= 10; p *= 10; }
        for (s = q; s <= r + 1; s++) {
            points[pctr++] = s * p;
            if ((int)pctr >= max_points) break;
        }
        p = 1; q = start_samp; r = end_samp;
        while (r - q >= (off_t)(max_minorpoints - 1)) { q /= 10; r /= 10; p *= 10; }
        for (s = q; s <= r + 1; s++) {
            minor_points[mpctr++] = s * p;
            if ((int)mpctr >= max_minorpoints) break;
        }
        *npoints = (int)pctr; *nminorpoints = (int)mpctr;
        return 0;
    }

    i = 0;
    while (i < (int)(ARRAY_LENGTH(bigsizes) - 1) &&
           (end_samp - start_samp) / (bigsizes[i] * (off_t)samplerate)
               > (off_t)(max_points - 2))
        i++;
    q = start_samp / (bigsizes[i] * (off_t)samplerate);
    r = end_samp   / (bigsizes[i] * (off_t)samplerate);
    while (1) {
        points[pctr++] = (q++) * bigsizes[i] * (off_t)samplerate;
        if ((int)pctr >= max_points) break;
        if (q > r) break;
    }
    *npoints = (int)pctr;

    if (i > 0) {
        i--;
        while (bigskip[i]) i--;
        if ((end_samp - start_samp) / (bigsizes[i] * (off_t)samplerate)
                >= (off_t)(max_minorpoints - 2)) {
            *nminorpoints = 0; return 0;
        }
        while (i > 0 &&
               (end_samp - start_samp) / (bigsizes[i-1] * (off_t)samplerate)
                   < (off_t)(max_minorpoints - 2))
            i--;
        q = start_samp / (bigsizes[i] * (off_t)samplerate);
        r = end_samp   / (bigsizes[i] * (off_t)samplerate);
        for (s = q; s <= r + 1 && (int)mpctr < max_minorpoints; s++)
            minor_points[mpctr++] = s * bigsizes[i] * (off_t)samplerate;
        *nminorpoints = (int)mpctr;
        return 0;
    }

    if (timemode == TIMEMODE_NTSC) {
        q = (start_samp * 30 * 1000) / ((off_t)samplerate * 1001);
        r = (end_samp   * 30 * 1000) / ((off_t)samplerate * 1001);
        if (r - q >= max_minorpoints) { *nminorpoints = 0; return 0; }
        for (s = q; s <= r && (int)mpctr < max_minorpoints; s++)
            minor_points[mpctr++] = (s * (off_t)samplerate * 1001 + 29999) / 30000;
        *nminorpoints = (int)mpctr;
        return 1;
    }

    switch (timemode) {
    case TIMEMODE_REAL: case TIMEMODE_REALLONG:
        ss = smallsizes_real;  ssl = ARRAY_LENGTH(smallsizes_real);  break;
    case TIMEMODE_24FPS:
        ss = smallsizes_24fps; ssl = ARRAY_LENGTH(smallsizes_24fps); break;
    case TIMEMODE_25FPS:
        ss = smallsizes_25fps; ssl = ARRAY_LENGTH(smallsizes_25fps); break;
    case TIMEMODE_30FPS: default:
        ss = smallsizes_30fps; ssl = ARRAY_LENGTH(smallsizes_30fps); break;
    }

    i = 0;
    while (i < ssl &&
           ((end_samp - start_samp) * ss[i]) / (off_t)samplerate
               >= (off_t)(max_minorpoints - 2))
        i++;
    if (i >= ssl) { *nminorpoints = 0; return 0; }

    q = (start_samp * ss[i]) / (off_t)samplerate;
    r = (end_samp   * ss[i]) / (off_t)samplerate;
    for (s = q; s <= r + 1 && (int)mpctr < max_minorpoints; s++)
        minor_points[mpctr++] = (s * (off_t)samplerate + ss[i] - 1) / ss[i];
    *nminorpoints = (int)mpctr;

    do { i++; } while (i < ssl &&
        ((end_samp - start_samp) * ss[i]) / (off_t)samplerate
            >= (off_t)(max_midpoints - 2));
    if (i >= ssl) return 1;

    q = (start_samp * ss[i]) / (off_t)samplerate;
    r = (end_samp   * ss[i]) / (off_t)samplerate;
    for (s = q; s <= r + 1 && (int)midpctr < max_midpoints; s++)
        midpoints[midpctr++] = (s * (off_t)samplerate + ss[i] - 1) / ss[i];
    *nmidpoints = (int)midpctr;
    return 1;
}

/* -----------------------------------------------------------------------
 * Entry point
 * ----------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    /* Out-of-process plugin scanner: `jackdaw --scan-plugin <FMT> <path>` loads
     * one plugin in this throwaway process and prints its metadata, then exits —
     * before any GTK/JACK/locale init. Keeps Wine/yabridge code out of the main
     * process during scanning. */
    if (argc >= 4 && !strcmp(argv[1], "--scan-plugin"))
        return pluginhost_scan_helper_main(argc, argv);

    /* Real-time memory hardening (the main process only — never the throwaway
     * scanner above). The JACK process thread must never take a major page fault
     * or wait on the allocator returning memory to the kernel, or it misses its
     * deadline and JACK reports an xrun. This is the standard pro-audio setup
     * (Ardour/Reaper do the equivalent) and is the main thing JackDAW was missing.
     *
     *  - mlockall: keep all current + future pages resident, so RT reads never
     *    fault on a paged-out page (GUI/plugin-editor activity won't evict audio).
     *  - mallopt(M_TRIM_THRESHOLD,-1) + (M_MMAP_MAX,0): stop glibc handing freed
     *    heap back to the kernel via munmap, which causes page-table churn / TLB
     *    shootdowns that stall even an SCHED_FIFO thread.
     *
     * mlockall needs RLIMIT_MEMLOCK headroom (audio group / limits.conf); if the
     * user lacks it we warn and continue rather than refuse to run. */
    mallopt(M_TRIM_THRESHOLD, -1);
    mallopt(M_MMAP_MAX, 0);
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        g_warning("mlockall() failed (%s) — RT memory not locked; expect xruns. "
                  "Grant this user RLIMIT_MEMLOCK (e.g. audio group / "
                  "limits.conf 'memlock unlimited').", g_strerror(errno));

    setlocale(LC_ALL, "");
    setlocale(LC_NUMERIC, "POSIX");

    gtk_init(&argc, &argv);
    pluginhost_ui_init(&argc, &argv);   /* suil_init for native LV2 editors */
    settings_init();

    default_timescale_mode = settings_get_uint32("timescaleMode", 1);

    JackDawProject *project = jackdaw_project_new();
    if (jackdaw_engine_init(project))
        g_warning("JACK engine failed to start — running without audio");

    /* Plugin host: size buffers to the engine's rate / block. */
    pluginhost_init((double)jackdaw_engine_get_sample_rate(),
                    (int)jackdaw_engine_get_buffer_size());
    pluginhost_load_paths_from_settings();

    GtkWidget *win = jackdaw_main_window_new(project);
    g_object_unref(project);  /* main window holds its own ref */

    /* Scan plugin paths at launch (progress dialog), announce plugins added
     * since last run. */
    jackdaw_fx_startup_scan(GTK_WINDOW(win));

    gtk_main();

    pluginhost_shutdown();
    jackdaw_engine_quit();
    settings_set_uint32("timescaleMode", default_timescale_mode);
    settings_quit();

    return 0;
}
