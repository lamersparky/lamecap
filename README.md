# lamecap

**A C command-line screen recorder for X11 and Wayland.** `lamecap` uses GStreamer APIs in-process and writes H.264 video to MP4. It does not spawn `ffmpeg`, `wf-recorder`, `gdbus`, `gst-launch`, or shell commands.

## What changed in 0.2.0

- **X11:** direct desktop capture with GStreamer's `ximagesrc`, without a desktop screen-selection portal.
- **Wayland:** uses the XDG Desktop Portal ScreenCast API over D-Bus and the portal-provided PipeWire file descriptor.
- Automatic backend selection based on the session environment, with `--backend` overrides.
- Optional system-output audio capture through a PulseAudio-compatible monitor source.
- H.264 MP4 output and clean EOS finalization when you press `Ctrl+C` once.

> This source package has not been compiled or integration-tested in the packaging environment because GLib/GStreamer development packages are unavailable there. Compile and test it on the target system before treating it as a release.

## Build requirements

- C compiler, `make`, and `pkg-config`
- GLib/GIO development files
- GStreamer core development files

Example Ubuntu/Debian packages (names may vary by release):

```sh
sudo apt install build-essential pkg-config libglib2.0-dev libgstreamer1.0-dev \
  gstreamer1.0-x gstreamer1.0-pipewire gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
  gstreamer1.0-plugins-ugly gstreamer1.0-libav
make
make check
```

The exact runtime plugins needed are:

- Common: `queue`, `videoconvert`, `videorate`, `capsfilter`, `h264parse`, `mp4mux`, `filesink`, and an H.264 encoder (`x264enc`, `openh264enc`, or `avenc_h264`).
- X11 backend: `ximagesrc` (often provided by `gstreamer1.0-x`).
- Portal backend: `pipewiresrc`, a user D-Bus session, `xdg-desktop-portal`, an appropriate portal backend, and PipeWire.
- Optional audio: `pulsesrc`, `audioconvert`, `audioresample`, `aacparse`, and `avenc_aac` or `voaacenc`.

## Usage

```sh
# Auto-detect: direct capture on X11, portal on Wayland
./lamecap --output "$HOME/Videos/browser-capture.mp4"

# Explicitly select the X11 backend
./lamecap --backend x11 -o "$HOME/Videos/desktop.mp4"

# Force the portal/PipeWire backend
./lamecap --backend portal -o "$HOME/Videos/portal-capture.mp4"

# Show available options
./lamecap --help
```

On X11, the default backend records the full X11 desktop (including all monitors arranged in the virtual desktop) directly and does not open the portal screen-selection dialog. On Wayland, screen capture remains mediated by the desktop portal; the program does not attempt to circumvent Wayland's compositor security model.

Press `Ctrl+C` **once** to send EOS and finalize the MP4 container. Avoid a second interrupt unless you accept possible file damage.

### Capture system audio too

Audio is opt-in, so the program will not silently record the microphone. Find the monitor source exposed by the PulseAudio-compatible PipeWire server:

```sh
pactl list short sources | grep -E '\.monitor|Monitor'
```

Pass the exact source name from the first column:

```sh
./lamecap --audio-monitor 'NAME_OF_OUTPUT_MONITOR' \
  --output "$HOME/Videos/browser-capture.mp4"
```

The monitor source captures audio mixed into that sink and may include other applications using the same output device.

### Portal source selection

The `--source` option applies to the portal backend only:

```sh
./lamecap --backend portal --source monitor
./lamecap --backend portal --source window
./lamecap --backend portal --source either
```

## Limitations

- X11 capture depends on access to the X server and records the rendered desktop. It cannot guarantee capture of protected video that a browser, driver, or site deliberately blanks.
- Wayland screen capture uses the authorized desktop portal; Wayland intentionally does not expose a universal unrestricted screen-read API to ordinary applications.
- Audio monitor selection is explicit. The utility does not yet discover and select the default output monitor automatically.
- This is a native C program but not a dependency-free static executable. It relies on installed GLib/GIO, GStreamer shared libraries, and plugins.

## How it works

- **X11:** `ximagesrc` feeds `videoconvert`, `videorate`, an H.264 encoder, `h264parse`, and `mp4mux`.
- **Wayland:** calls `CreateSession`, `SelectSources`, and `Start` on the ScreenCast portal, opens the portal's restricted PipeWire remote, and sends the stream through the same encoder/muxer chain.
- **Shutdown:** a single `Ctrl+C` sends EOS so the muxer can finalize the MP4 index.

## Development

```sh
make check
make clean
make
```
