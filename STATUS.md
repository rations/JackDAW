# JackDAW — Development Status

**Last updated:** 2026-06-09  
**Build plan:** `/home/human/.claude/plans/this-is-a-fork-effervescent-hollerith.md`

---

## Build

```
cd /home/human/JackDAW
./autogen.sh && ./configure && make -j$(nproc)
./src/jackdaw
```

Binary: `src/jackdaw` (has debug_info, not stripped — suitable for GDB).

---

## Phase Status

| Phase | Description | Status |
|-------|-------------|--------|
| 0 | Cleanout — GTK3 only, remove legacy backends | **Done** |
| 1 | Track data model (`track.c/h`, `project.c/h`) + JACK engine (`jackdaw-engine.c/h`) | **Done** |
| 2 | Multi-track timeline widget (`timeline.c/h`, `mainwindow.c/h`) | **Done** |
| 3 | TrackStrip widget (`trackstrip.c/h`) + ALSA MIDI (`alsa-midi.c/h`) | **Not started** |
| 4 | Mixer dialog + VU meters | **Not started** |
| 5 | Plugin system (LADSPA/LV2/VST2/VST3/CLAP via `pluginhost.c/h`) | **Not started** |
| 6 | JACK port preferences tab in config dialog | **Not started** |

---

## Known Issues (as of 2026-06-09)

### CRASH: Segfault when loading any audio file (MP3 or WAV)

**Symptom:**  
After decoding completes (or directly for WAV), loading a file into a track causes a segfault. Crash is post-decode and post-document creation — it happens in the UI path when the track is added.

**What's been fixed already:**
- `src/filetypes.c:1048` — null-deref on `bar->progress_cur` when `bar=NULL` during `run_decoder` loop. Fixed with `bar ? o - bar->progress_cur : 0`. This moved the MP3 crash from Frame# 247 to after Frame# 12565 (decode now completes).
- `src/project.c` — double-unref in `jackdaw_project_remove_track`. Fixed with temp-ref pattern.

**Current crash location:** Unknown. Happens in the signal path after `jackdaw_project_add_track` emits `track-added`, which triggers `jackdaw_timeline_add_track` → `jackdaw_wave_view_new`. The crash may be in viewcache, the Document's StatusBar pointer, or a GTK widget allocation issue.

**To debug:**
```
cd /home/human/JackDAW
gdb src/jackdaw
(gdb) run
# load a WAV file, get backtrace on SIGSEGV
(gdb) bt
```

---

## What's Working

- JACK engine starts, registers `in_1`, `in_2`, `out_1`, `out_2`, `midi_in_1`
- All tracks mix to `out_1`/`out_2` (no per-track ports, no `master_out`)
- Timeline ruler with tick marks, shared `cursor_adj` adjustment
- Transport playhead (orange line) spans all track rows via shared `GtkAdjustment`
- Pause button (⏸) stops without rewind; Stop (■) halts + rewinds
- GObject refcount crash in `remove_track` fixed

---

## Port Model (implemented)

```
jackdaw:in_1       audio input
jackdaw:in_2       audio input
jackdaw:out_1      audio output (master L)
jackdaw:out_2      audio output (master R)
jackdaw:midi_in_1  MIDI input
```

No `track_N_out_L/R`. No `master_out_L/R`. All tracks mix internally.  
jackdaw never calls `jack_connect()`. User routes in qjackctl/Carla.

---

## Next Steps (Phase 3)

Once the file-load crash is fixed:

1. **`src/trackstrip.c/h`** — GTK3 widget (ARM/M/S buttons, vol/pan sliders, audio/MIDI input selectors). Replaces the placeholder `GtkLabel` in the 180px track header column.
2. **`src/alsa-midi.c/h`** — ALSA sequencer wrapper (`snd_seq_*`) for MIDI port enumeration. Used by TrackStrip MIDI input combo.

See the build plan for full API design.

---

## Key Files

| File | Purpose |
|------|---------|
| `src/jackdaw-engine.c/h` | JACK client, process callback, port management |
| `src/track.c/h` | `JackDawTrack` GObject (vol, pan, arm, ringbuffers) |
| `src/project.c/h` | `JackDawProject` GObject (track array, signals) |
| `src/timeline.c/h` | `JackDawTimeline`, `JackDawTimeRuler`, `JackDawWaveView` |
| `src/mainwindow.c/h` | Main window, toolbar, menus |
| `src/filetypes.c` | Audio file I/O (libsndfile + lame decoder for MP3) |
| `src/viewcache.c/h` | Waveform rendering cache (original mhwaveedit, kept as-is) |
| `src/document.c/h` | Undo/redo, selection, chunk management (original, kept as-is) |
