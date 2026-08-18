# JackDAW

JackDAW is a multitrack digital audio workstation for Linux, built on top of
the JACK Audio Connection Kit. It records, edits, sequences and mixes audio and
MIDI across many tracks, hosts LV2 / VST2 / VST3 / CLAP / LADSPA plugins, and
runs each track's effect chain in parallel across a real-time worker pool.

All IPC is plain UNIX (sockets, files, JACK signalling).

- **Binary:** `jackdaw`
- **Config dir:** `~/.jackdaw/`
- **Audio backend:** JACK (with an ALSA fallback when JACK is not running)
- **License:** GNU GPL v2 (see [LICENSE](LICENSE))

---

## Features

- Multitrack audio + MIDI timeline with per-track undo/redo and session restore.
- Non-destructive editing (immutable chunk model): cut, copy, paste, normalize,
  fade, split, group, and more.
- Plugin hosting via a unified host: **LV2, VST2, VST3, CLAP, LADSPA**, with
  out-of-process scanning so a crashing plugin can't take down the app.
- Per-track FX chains processed in parallel across an RT worker pool (heavy
  plugins on separate tracks use separate CPU cores).
- Built-in **piano-roll MIDI editor** with quantize, velocity lane, loop region,
  and live note auditioning through the track's instrument.
- Metronome with count-in, loop playback, region rendering, and a configurable
  JACK port layout.
- Light/dark theming, rendered against JackDAW's own bundled GTK base rather
  than the desktop theme, so both modes look the same on every distro and
  desktop environment.

---

## Requirements

Runtime libraries (installed automatically by `install-jackdaw.sh`):

| Component        | Debian/Devuan package   |
|------------------|-------------------------|
| GTK+ 3           | `libgtk-3-0`            |
| JACK2            | `libjack-jackd2-0`      |
| libsndfile       | `libsndfile1`           |
| ALSA             | `libasound2`            |
| libsamplerate    | `libsamplerate0`        |
| LV2 (lilv)       | `liblilv-0-0`           |
| LV2 UI (suil)    | `libsuil-0-0`           |

You also need a running **JACK server** (e.g. `jackd`, or Jack Graph /
QjackCtl to manage it). For low-latency audio, add your user to the `audio`
group and grant realtime/memlock limits (see the note printed after install).

Building from source additionally needs the `-dev` packages plus
`gcc`/`g++`/`make`/`pkg-config` — these are installed for you only when a source
build is actually needed.

---

## Installation

### Getting the source

Release tarballs unpack ready to build — they ship the VST3 SDK inside `ext/`.
If you clone from git instead, fetch the submodule too, or the VST3 backend
cannot be built:

```sh
git clone --recurse-submodules https://github.com/rations/JackDAW.git
```

Already cloned without it? Fetch it after the fact:

```sh
git submodule update --init ext/vst3sdk
git -C ext/vst3sdk submodule update --init base pluginterfaces public.sdk
```

Only those three of the SDK's seven nested submodules are needed; skipping the
rest (`vstgui4`, `doc`, `tutorials`, `cmake`) avoids about 170 MB that JackDAW
never compiles. The installer detects an empty `ext/vst3sdk` and builds without
VST3 rather than failing, so a plain `git clone` still produces a working DAW —
just one that cannot host VST3 plugins.

### Running the installer

The installer is **distro-agnostic** (apt, dnf/yum, pacman, zypper, apk) and
needs no systemd. From the unpacked release directory:

```sh
./install-jackdaw.sh
```

It will:

1. **Ask where to install** — system-wide `/usr/local` (uses `sudo`) or
   per-user `~/.local`.
2. **Ask how to install** — **prebuilt binary (default)** or build from source.
   If you pick (or default to) the prebuilt binary but it is not usable on your
   machine, it automatically falls back to a source build.
3. Install the runtime dependencies for your distro.
4. Install the `jackdaw` binary, the optional LV2 UI helper binaries, the icon
   set, and a `jackdaw.desktop` launcher; then refresh the icon/desktop caches.

> The prebuilt binary is **never executed** to test it (it would open a window).
> Usability is checked with `readelf` (architecture match) and `ldd` (all shared
> libraries resolve). If either check fails, the installer builds from source.

### Command-line options

| Option / variable | Effect |
|-------------------|--------|
| *(no flags)*      | Interactive: prompts for install location and method (**prebuilt is the default**). |
| `--prebuilt`      | Use the prebuilt binary without prompting; **fail** if it is unusable. |
| `--build`         | Force a source build, ignoring any prebuilt binary. |
| `--no-deps`       | Skip installing distro packages (assume they're already present). |
| `-h`, `--help`    | Show usage. |
| `PREFIX=/path`    | Install prefix; **skips the location prompt**. Common: `PREFIX=/usr/local` or `PREFIX=$HOME/.local`. |

Examples:

```sh
# Fully interactive (recommended for first install)
./install-jackdaw.sh

# Non-interactive, system-wide, prebuilt binary only
sudo PREFIX=/usr/local ./install-jackdaw.sh --prebuilt

# Per-user install, force a source build
PREFIX=$HOME/.local ./install-jackdaw.sh --build
```

If you install to `~/.local`, make sure `~/.local/bin` is on your `PATH`.

### Building from source manually

JackDAW uses a plain `Makefile` (not autotools):

```sh
make -j"$(nproc)"        # build src/jackdaw (+ LV2 UI helpers), VST3 included
make VST3=0 -j"$(nproc)" # skip the VST3 backend (no SDK needed, faster build)
make clean               # also removes build/ and any stray SDK objects
```

VST3 hosting is **on by default** and compiles the Steinberg SDK from
`ext/vst3sdk`. Its object files go to `build/`, deliberately outside the
submodule so the SDK checkout is never left dirty. Use `VST3=0` if you have not
fetched the submodule or want a lighter build.

### Uninstalling

```sh
./uninstall-jackdaw.sh
```

It prompts for the same location you installed to (or honours `PREFIX=`),
removes the binary, helpers, icons and launcher, and refreshes caches.

It never touches distro packages.

Your settings, plugin scan cache and recordings live in `~/.jackdaw` and are
**left in place** — the uninstaller prints the path and what is in it, including
a count of any recorded audio, so you can decide for yourself.

### Creating a release tarball

`release-tarball.sh` produces `jackdaw-<VERSION>.tar.gz` that unpacks into a
top-level `JackDAW/` directory containing the source, bundled headers, the VST3
SDK, icons, packaging scripts and the prebuilt binary. Because the SDK is
included the tarball is around 15 MB, and it builds VST3 support with no
network access or submodule fetch.

```sh
./release-tarball.sh           # version read from src/config.h
./release-tarball.sh 0.2.0     # override the version on the command line
./release-tarball.sh --no-binary   # source-only tarball
```

The default version comes from `#define VERSION` in `src/config.h`; edit the
`VERSION=` variable at the top of the script, or pass it as the first argument.

---

## Plugins

### Supported formats

| Format  | Notes |
|---------|-------|
| LV2     | Scanned in-process via lilv (reads `.ttl`). |
| VST2    | Via the bundled public-domain `vestige` header (no Steinberg SDK). |
| VST3    | Via the Steinberg VST3 SDK (MIT) vendored as the `ext/vst3sdk` submodule; on by default, disable with `make VST3=0`. |
| CLAP    | Bundled CLAP headers. |
| LADSPA  | Bundled LADSPA header. |

### Scanning

Scanning is **out-of-process and cached**, each plugin
is loaded and described in a throwaway child process
(`jackdaw --scan-plugin <FORMAT> <path>`), so a plugin that crashes on load
cannot bring down JackDAW. Results are cached on disk keyed by file path +
modification time, so only new or changed plugins are re-scanned. (LV2 is the
exception — lilv reads only the `.ttl` metadata, so it scans in-process.)

On startup JackDAW scans and reports any **newly discovered** plugins.

**Default search paths** (always present):

- User: `~/.lv2`, `~/.vst`, `~/.vst3`, `~/.clap`, `~/.ladspa`
- LV2: `/usr/lib/lv2`, `/usr/local/lib/lv2`, `/usr/lib/x86_64-linux-gnu/lv2`
- VST2: `/usr/lib/vst`, `/usr/local/lib/vst`
- VST3: `/usr/lib/vst3`, `/usr/local/lib/vst3`, `/usr/lib/x86_64-linux-gnu/vst3`
- CLAP: `/usr/lib/clap`, `/usr/local/lib/clap`
- LADSPA: `/usr/lib/ladspa`, `/usr/local/lib/ladspa`, `/usr/lib/x86_64-linux-gnu/ladspa`

**Adding more folders / rescanning:** open the **Plugins…** paths dialog. It
has **Add Folder…**, **Remove**, and **Scan** (rescan) buttons; extra paths are
saved to your settings and re-used on the next launch. A progress dialog is
shown while scanning runs.

**Using a plugin:** open a track's **FX window** and click **Add Effect** to get
the categorised plugin browser. Instruments are listed under **MIDI**; ordinary
effects appear under their own category. If nothing shows up, add a folder and
rescan from the Plugins… dialog.

---

## Keyboard shortcuts

### Main window

| Shortcut            | Action |
|---------------------|--------|
| `Space`             | Play / Stop |
| `Home`              | Locate to start |
| `←` / `→`           | Step playhead back / forward |
| `R`                 | Toggle record |
| `L`                 | Toggle loop |
| `S`                 | Split clip at cursor |
| `G`                 | Group selection |
| `Ctrl+C` / `Ctrl+V` | Copy selection / Paste at cursor |
| `Ctrl+Z` / `Ctrl+Y` | Undo / Redo |
| `Ctrl+O`            | Open project |
| `Ctrl+S`            | Save project |
| `Ctrl+Shift+S`      | Save project as… |
| `Ctrl+N`            | New session |
| `Ctrl++` / `Ctrl+-` | Zoom in / out |
| `Ctrl+Q`            | Quit |

> Plain-letter shortcuts (`R`, `L`, `S`, `G`) are ignored while you are typing
> in a text field.

**Timeline mouse:**

- **Click** a track to select it; **Ctrl+left-click** keeps a multi-track
  selection / toggles a section within one track.
- **Double-click** an instrument (MIDI) track to open the MIDI editor.
- **Right-click** selects the region under the pointer.
- **Ctrl+scroll** pans left/right.

### MIDI editor (piano roll)

| Shortcut            | Action |
|---------------------|--------|
| `Space`             | Play / Stop |
| `Home`              | Locate to start |
| `Q`                 | Quantize (selected notes, or all) to the 1/16 grid |
| `Ctrl+A`            | Select all notes |
| `Ctrl+C` / `Ctrl+V` | Copy / Paste notes |
| `Ctrl+Z` / `Ctrl+Y` | Undo / Redo |
| `Esc`               | Clear selection |

---

## The MIDI editor

Open it by **double-clicking an instrument (MIDI) track** in the timeline. The
window is laid out as: a transport toolbar on top, a piano keyboard down the
left, the note grid with the playhead in the centre, a velocity lane below, and
a beat/bar ruler across the top. Note timing is stored in musical ticks at 960
PPQ, so edits stay in time regardless of tempo.

**Editing notes with the mouse:**

- **Left-click an empty cell** to add a note (snapped to the grid, default
  velocity 100, length = the snap step or a 1/16 note).
- **Left-drag a note body** to move it; **left-drag a note's right edge** (a
  small grab zone) to change its length.
- **Right-drag** to rubber-band **select** a group of notes. With a selection
  active, **left-drag** any selected note to move the whole group; click empty
  space to deselect.
- **Right-click** (without dragging) opens the context menu: *Delete Note /
  Delete Selected, Copy, Paste, Select All, Quantize, Clear Loop Region*.

**Velocity lane:** drag the bar under a note to set its velocity. Bars are
colour-coded from blue (soft) through green to red (loud).

**Piano keyboard:** click or drag down the keys on the left to **audition**
pitches live through the track's instrument plugin (dragging glides across
notes).

**Ruler & loop:** click or drag on the ruler to scrub the playhead; drag the
loop tabs to set a loop region (toggle looping with the loop button or `L`).

**Snapping & quantize:** live drag-editing snaps to the grid when the project's
**Snap** toggle is on. `Q` (or the context menu) quantizes note starts to the
1/16 grid regardless of the Snap toggle — selected notes if any are selected,
otherwise all notes.

**Scrolling/zoom in the roll:** scroll = vertical, `Shift+scroll` = horizontal,
`Ctrl+scroll` = zoom (time resolution).

---

## JACK port model

JackDAW registers N audio inputs, N audio outputs and M MIDI inputs (defaults
2 / 2 / 1, configurable in **Preferences → JACK Ports**). The track in slot *i*
is automatically assigned input port *i* when one is available. Each track's
input selector lists the external JACK ports and connects them for you, so the
common recording case needs no separate patchbay. External-to-external routing
(e.g. hardware to other apps) remains your patchbay's job.

---

## Diagnostics

An opt-in, RT-safe profiler for chasing xruns is available — it is disabled
unless the environment variable is set:

```sh
JACKDAW_DIAG=1 jackdaw
```

A background thread then prints one line per second with xrun counts, callback
timing versus the buffer deadline, and the worst-case `process()` time per
plugin. When unset, the per-cycle cost is a couple of branch checks.

