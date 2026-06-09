# JackDAW — Development Guide

JackDAW is a multitrack DAW built from the mhwaveedit codebase.

## Identity
- Binary name: `jackdaw`
- Config dir: `~/.jackdaw/`
- Build system: GNU Autotools (configure.in + Makefile.am)

## Target Platform
- Devuan Excalibur (Debian Trixie base, sysvinit — NO systemd)
- No dependency on systemd APIs, libsystemd, logind, or D-Bus session bus.
- All IPC via plain UNIX mechanisms (sockets, files, JACK signaling).
- Do not use sd_notify, sd_bus, or any libsystemd symbols.

## GTK Version
- GTK+ 3.x only (libgtk-3-dev 3.24.49 installed)
- All code (new and inherited) targets the GTK3 C API.
- No GTK1, GTK2, or GTK4. No gtkmm.
- Drawing uses Cairo (draw signal, not expose_event).
- Containers: GtkBox/GtkGrid (not deprecated GtkHBox/GtkVBox).

## Language Policy
- Primary: C (C99/GNU99)
- C++ only where a dependency requires it (VST3 SDK).
  Keep C and C++ in separate translation units.
- Do not add gtkmm. Use the GTK3 C API directly from both C and C++ files.

## Coding Standards
- Never guess at API signatures. Verify against installed headers:
  - JACK2 (libjack-jackd2-dev 1.9.22): /usr/include/jack/jack.h,
    jack/midiport.h, jack/transport.h, jack/ringbuffer.h
  - GTK3: pkg-config --cflags gtk+-3.0
  - LV2: /usr/include/lilv/lilv.h, suil/suil.h
  - VST3: /home/human/third_party/vst3sdk/
  - CLAP: ext/clap/clap.h (bundled)
  - LADSPA: ext/ladspa.h (bundled)
  - ALSA: /usr/include/alsa/asoundlib.h
- No malloc/free/new/delete in the JACK RT process callback. All buffers
  pre-allocated at engine init based on jack_get_buffer_size(). Re-allocate
  outside the RT thread if jack_set_buffer_size_callback fires.
- No VLAs (variable-length arrays) in the RT callback — pre-allocate.
- All JACK ringbuffer access in the RT thread uses jack_ringbuffer_* (lock-free,
  from <jack/ringbuffer.h>). No mutexes in the RT path.
- Atomic control between RT and main thread: volatile integer bitmask (procctrl
  pattern) or g_atomic_pointer_get/set for pointer swaps.
- No blocking calls (malloc, mutex lock, file I/O, g_print) in the JACK
  process callback.
- VST3 entry point: GetPluginFactory (GetFactoryProc function pointer).
  VST2 entry: VSTPluginMain. CLAP entry: clap_entry (clap_plugin_entry_t struct).
- Constant-power pan law: angle = (pan+1) * M_PI_4; L = vol*cosf(angle);
  R = vol*sinf(angle). M_PI_4 from <math.h> with _GNU_SOURCE defined.
- Boolean convention (matches mhwaveedit): TRUE = failure, FALSE = success
  for gboolean error-return functions.
- No command injection: never pass unsanitised user strings to shell commands.
  Use g_spawn_async_with_pipes with explicit argv[]. Never system() or popen().
- Validate all data read from project files and inifile before use.

## JACK Port Model
- jackdaw registers N audio input, N audio output, and M MIDI input ports
  (counts set in Preferences → JACK Ports; defaults 2/2/1).
- These ports appear in patchbay tools (qjackctl, Carla, etc.) as the
  "jackdaw" node. Users connect sources to them externally.
- jackdaw never calls jack_connect(). Routing is entirely the user's.
- Track input selector: uses jackdaw_engine_get_audio_in_port() + 
  jack_port_get_connections() to show what is patched to each port.
  Ports listed whether connected or not so assignment can be pre-configured.
- Port naming: audio in_1…in_N, out_1…out_N, midi_in_1…midi_in_M,
  track_N_out_L/R, master_out_L, master_out_R.

## Plugin Formats
- LADSPA: ext/ladspa.h (bundled)
- LV2: liblilv-dev + libsuil-dev
- VST2: ext/vestige.h (bundled public-domain header; no Steinberg SDK needed)
- VST3: /home/human/third_party/vst3sdk (C++ required)
- CLAP: ext/clap/clap.h (bundled from github.com/free-audio/clap)
- All formats accessed through the unified pluginhost.h interface.

## Security Rules
- Plugin loading: validate path is absolute and under a standard scan dir before
  dlopen(). No '..' components, no null bytes. Check all dlsym returns before use.
- Project file loading: validate trackCount in [0,64], port counts in [1,64],
  port indices in [-1, port_count-1], file paths absolute and < PATH_MAX.
- Any external string (JACK port name, plugin metadata, ALSA port name) displayed
  in a GTK label: use gtk_label_set_text(), never gtk_label_set_markup().
- Port names registered with jack_port_register() are generated from the index
  ("in_1", "in_2" etc.) — never raw user input.

## Architecture
- Audio data: immutable Chunk model (chunk.h). Editing returns new Chunks.
- Multi-track JACK routing through jackdaw-engine.h/c only.
- Per-track RT state in fixed-size slot array; no pointers to heap objects
  that could be freed while the RT callback runs.
- New GObject subclasses use G_DEFINE_TYPE.
- No GTK1/GTK2 compatibility code anywhere in the tree.
- ALSA kept for: (1) audio fallback when JACK not running; (2) ALSA MIDI port
  enumeration via snd_seq_* API.

## What Must Be Preserved
- All existing edit operations (cut, copy, paste, normalize, fade, etc.) work
  on each track's Document exactly as they work today.
- Undo/redo history per track is maintained.
- Session restore works across all tracks.
