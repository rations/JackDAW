#define _GNU_SOURCE
#include <config.h>
#include "alsa-midi.h"

#ifdef HAVE_ALSA
#include <alsa/asoundlib.h>

gchar **alsa_midi_list_sources(void)
{
    snd_seq_t *seq = NULL;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, 0) < 0)
        return NULL;

    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);

    snd_seq_client_info_t *cinfo;
    snd_seq_port_info_t   *pinfo;
    snd_seq_client_info_alloca(&cinfo);
    snd_seq_port_info_alloca(&pinfo);

    snd_seq_client_info_set_client(cinfo, -1);
    while (snd_seq_query_next_client(seq, cinfo) >= 0) {
        int client = snd_seq_client_info_get_client(cinfo);
        if (client == SND_SEQ_CLIENT_SYSTEM)
            continue;

        snd_seq_port_info_set_client(pinfo, client);
        snd_seq_port_info_set_port(pinfo, -1);
        while (snd_seq_query_next_port(seq, pinfo) >= 0) {
            unsigned int cap = snd_seq_port_info_get_capability(pinfo);
            /* Only ports we can subscribe to for reading (MIDI sources) */
            if ((cap & (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ)) !=
                       (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ))
                continue;

            const char *client_name = snd_seq_client_info_get_name(cinfo);
            const char *port_name   = snd_seq_port_info_get_name(pinfo);

            if (!client_name || !port_name || *client_name == '\0')
                continue;

            g_ptr_array_add(names, g_strdup_printf("%s:%s",
                                                    client_name, port_name));
        }
    }

    snd_seq_close(seq);

    if (names->len == 0) {
        g_ptr_array_free(names, TRUE);
        return NULL;
    }

    g_ptr_array_add(names, NULL);
    return (gchar **)g_ptr_array_free(names, FALSE);
}

#else /* !HAVE_ALSA */

gchar **alsa_midi_list_sources(void)
{
    return NULL;
}

#endif /* HAVE_ALSA */
