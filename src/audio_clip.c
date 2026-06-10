#include <config.h>
#include <string.h>
#include <sndfile.h>
#include "audio_clip.h"

AudioClip *audio_clip_new(const gchar *path, GError **err)
{
    SF_INFO info = {0};
    SNDFILE *sf = sf_open(path, SFM_READ, &info);
    if (!sf) {
        g_set_error(err, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                    "%s: %s", path, sf_strerror(NULL));
        return NULL;
    }

    if (info.frames <= 0 || info.channels <= 0 || info.samplerate <= 0) {
        sf_close(sf);
        g_set_error(err, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                    "%s: invalid audio parameters", path);
        return NULL;
    }

    AudioClip *clip = g_new0(AudioClip, 1);
    clip->path = g_strdup(path);
    clip->info = info;
    clip->refcount = 1;

    int ch = info.channels;
    clip->n_blocks = (guint32)((info.frames + AUDIO_CLIP_PEAK_BLOCK - 1)
                               / AUDIO_CLIP_PEAK_BLOCK);
    clip->block_peaks = g_new(gfloat, clip->n_blocks * ch * 2);

    gfloat *buf = g_new(gfloat, AUDIO_CLIP_PEAK_BLOCK * ch);

    for (guint32 b = 0; b < clip->n_blocks; b++) {
        sf_count_t n = sf_readf_float(sf, buf, AUDIO_CLIP_PEAK_BLOCK);
        if (n <= 0) {
            for (guint32 bb = b; bb < clip->n_blocks; bb++)
                for (int c = 0; c < ch; c++) {
                    clip->block_peaks[(bb * ch + c) * 2]     = 0.0f;
                    clip->block_peaks[(bb * ch + c) * 2 + 1] = 0.0f;
                }
            break;
        }
        for (int c = 0; c < ch; c++) {
            gfloat mn =  1.0f, mx = -1.0f;
            for (sf_count_t s = 0; s < n; s++) {
                gfloat v = buf[s * ch + c];
                if (v < mn) mn = v;
                if (v > mx) mx = v;
            }
            clip->block_peaks[(b * ch + c) * 2]     = mn;
            clip->block_peaks[(b * ch + c) * 2 + 1] = mx;
        }
    }

    g_free(buf);
    sf_close(sf);
    return clip;
}

AudioClip *audio_clip_ref(AudioClip *clip)
{
    if (!clip) return NULL;
    g_atomic_int_inc(&clip->refcount);
    return clip;
}

void audio_clip_free(AudioClip *clip)
{
    if (!clip) return;
    if (!g_atomic_int_dec_and_test(&clip->refcount))
        return;   /* still referenced elsewhere */
    g_free(clip->block_peaks);
    g_free(clip->path);
    g_free(clip);
}

void audio_clip_get_peaks(AudioClip  *clip,
                           sf_count_t  start,
                           sf_count_t  end,
                           gint        xres,
                           gfloat     *out_min,
                           gfloat     *out_max)
{
    if (!clip || xres <= 0 || end <= start || !clip->block_peaks) {
        for (gint i = 0; i < xres * (clip ? clip->info.channels : 1); i++) {
            if (out_min) out_min[i] =  0.0f;
            if (out_max) out_max[i] = -1.0f;
        }
        return;
    }

    int ch = clip->info.channels;
    gdouble spp = (gdouble)(end - start) / (gdouble)xres;

    for (gint x = 0; x < xres; x++) {
        sf_count_t s0 = start + (sf_count_t)(x * spp);
        sf_count_t s1 = start + (sf_count_t)((x + 1) * spp);
        if (s1 <= s0) s1 = s0 + 1;
        if (s1 > end) s1 = end;

        guint32 b0 = (guint32)(s0 / AUDIO_CLIP_PEAK_BLOCK);
        guint32 b1 = (guint32)((s1 - 1) / AUDIO_CLIP_PEAK_BLOCK) + 1;
        if (b1 > clip->n_blocks) b1 = clip->n_blocks;

        for (int c = 0; c < ch; c++) {
            gfloat mn =  1.0f, mx = -1.0f;
            for (guint32 b = b0; b < b1; b++) {
                gfloat bmin = clip->block_peaks[(b * ch + c) * 2];
                gfloat bmax = clip->block_peaks[(b * ch + c) * 2 + 1];
                if (bmin < mn) mn = bmin;
                if (bmax > mx) mx = bmax;
            }
            out_min[x * ch + c] = mn;
            out_max[x * ch + c] = mx;
        }
    }
}

sf_count_t audio_clip_read_frames(AudioClip  *clip,
                                   sf_count_t  start_frame,
                                   sf_count_t  n_frames,
                                   gfloat     *out)
{
    if (!clip || n_frames <= 0) return 0;

    SF_INFO info = {0};
    SNDFILE *sf = sf_open(clip->path, SFM_READ, &info);
    if (!sf) return 0;

    sf_seek(sf, start_frame, SEEK_SET);
    sf_count_t got = sf_readf_float(sf, out, n_frames);
    sf_close(sf);
    return got;
}
