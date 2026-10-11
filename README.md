# lamecap

**A C command-line screen recorder for GNOME/Wayland.** It uses the XDG
Desktop Portal ScreenCast API over D-Bus, takes the portal-provided PipeWire
file descriptor, and records H.264 video in an MP4 container through GStreamer.
It does not spawn `ffmpeg`, `wf-recorder`, `gdbus`, `gst-launch`, or shell commands.

> Status: early prototype (0.1.0). Source is provided for review and building;
> this environment did not have GLib/GStreamer development headers installed,
> so it could not be compiled or integration-tested here.

## Build requirements

Build-time headers and libraries:

- a C compiler and `make`
- `pkg-config`
- GLib/GIO development files
- GStreamer core development files

Runtime components supplied by the desktop/GStreamer installation:

- a running user D-Bus session
- `xdg-desktop-portal` and the GNOME portal backend
- PipeWire
- GStreamer elements: `pipewiresrc`, `queue`, `videoconvert`, `videorate`, `capsfilter`,
  `h264parse`, `mp4mux`, `filesink`, and one H.264 encoder (`x264enc`,
  `openh264enc`, or `avenc_h264`)
- for audio capture: `pulsesrc`, `audioconvert`, `audioresample`, `aacparse`,
  and one AAC encoder (`avenc_aac` or `voaacenc`)

Typical Ubuntu/Debian package set (package names may vary by release):

```sh
sudo apt install build-essential pkg-config libglib2.0-dev libgstreamer1.0-dev \
  gstreamer1.0-pipewire gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-libav
make
```

## Usage

```sh
./lamecap --help
./lamecap --output "$HOME/Videos/browser-capture.mp4"
```

The desktop permission dialog appears. Select the monitor or browser window,
then start/play the video. Press `Ctrl+C` **once** to send EOS and finalize the
MP4 container. Avoid a second interrupt unless you accept possible file damage.

### Capture browser audio too

Audio is opt-in so the app does not accidentally record a microphone. Find the
monitor source exposed by your PulseAudio-compatible PipeWire server:

```sh
pactl list short sources | grep -E '\.monitor|Monitor'
```

Pass the exact source name (first column):

```sh
./lamecap --audio-monitor 'NAME_OF_OUTPUT_MONITOR' \
  --output "$HOME/Videos/browser-capture.mp4"
```

The monitor source captures output mixed into that sink; depending on desktop
audio routing, it may include audio from other applications too.

## Important limitations

- This uses GNOME's authorized screen-capture portal, so the desktop's portal
  and PipeWire services must be running. Wayland intentionally does not provide
  an unrestricted direct-screen-read API to arbitrary programs.
- **The current build is not yet a dependency-free static executable.** It is a
  native C program and does not invoke external recorder utilities, but it uses
  the installed GLib/GIO and GStreamer shared libraries/plugins. A genuinely
  self-contained single-file build requires building GStreamer with the needed
  plugins statically registered and packaging a compatible GLib/GIO dependency
  closure. That is a separate packaging task and is not something this source
  tree can truthfully claim has already achieved.
- This prototype supports monitor/window selection through the portal. It does
  not bypass DRM/HDCP, browser restrictions, or website access controls.
- The installed GStreamer version must provide `pipewiresrc`'s `fd` and
  `target-object` properties. The portal's `pipewire-serial` property is used
  when available, with the returned node ID as a compatibility fallback.

## How it works

1. Calls `CreateSession`, `SelectSources`, and `Start` on the ScreenCast portal.
2. Waits for the associated `Request.Response` D-Bus signals.
3. Opens the restricted PipeWire remote with `OpenPipeWireRemote`.
4. Sends the PipeWire stream into an H.264 encoder and `mp4mux`.
5. On Ctrl+C, sends EOS so the muxer can finish writing the MP4 index.

## Development

```sh
make check
make clean
```

`make check` requires the same development headers as `make`.
