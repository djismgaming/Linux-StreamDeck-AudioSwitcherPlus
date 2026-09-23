# StreamDeck-AudioSwitcherPlus

An audio-switcher plugin for the [Stream Deck](https://www.elgato.com/us/en/s/welcome-to-stream-deck) that runs **natively on Linux** via [OpenDeck](https://github.com/nekename/OpenDeck). This fork exists specifically to add Linux support — audio switching is implemented over **PulseAudio/PipeWire**, with no COM, no Wine, and no Windows emulation layer.

> **This is a fork** of [Fred Emmott](https://github.com/fredemmott)'s original [StreamDeck-AudioOutputSwitcher](https://github.com/fredemmott/StreamDeck-AudioOutputSwitcher), extended with a native Linux/PipeWire port and the additional features listed below. All credit for the original plugin, its design, and the underlying audio-switching approach goes to Fred Emmott — please check out [his other Stream Deck plugins](https://github.com/fredemmott) as well.

## Features

- Set the **input** or **output** audio device
- Either **one button per device**, or **one button that toggles** between two devices
- **Custom icons** per button (earbuds, headphones, mic, speaker) with **custom colors** for On and Off states
- **Simplified device selection**:
  - hide disabled / unplugged devices
  - hide the device type suffix (which Windows gets wrong often)
- **Fuzzy device matching** so buttons keep working even if a device's ID changes between boots

## Supported platforms

| Platform | Launcher | Status |
|---|---|---|
| **Linux** | [OpenDeck](https://github.com/nekename/OpenDeck) | **Supported — native build** (this fork's purpose) |
| Windows | Stream Deck | Supported (native build) |
| macOS | — | Not supported |

OpenDeck is an open-source alternative to the Stream Deck app that runs on Linux (the official Stream Deck app is Windows/macOS-only). On Linux the plugin controls audio through **libpulse**, which talks to PipeWire's PulseAudio-compatible server on modern desktops.

## How audio switching works on Linux

- **Sinks/sources become devices.** Every PulseAudio sink (output) or source (input) appears in the device list; each of its selectable **ports** also appears as its own device. A sound card with both a **Line Out** and a **Headphones** jack exposes both as separate devices, so a toggle button switches between the two jacks of one card.
- **"Communication" role == default device.** PulseAudio only has one default device per direction, so the default and communication roles are the same thing — use the **"All"** role (the default for new buttons).
- Device IDs are PulseAudio names like `alsa_output.pci-0000_0b_00.4.analog-stereo|analog-output-headphones`; the fuzzy-matching fallback still applies if a device's ID changes between boots.

## Using the plugin in OpenDeck

The build installs the plugin into OpenDeck's plugin directory and OpenDeck lists it automatically — no special setup beyond building and installing it.

### 1. Clone the repo

```bash
git clone https://github.com/djismgaming/Linux-StreamDeck-AudioSwitcherPlus
cd Linux-StreamDeck-AudioSwitcherPlus
```

### 2. Build and install the native plugin

Requires a C++20 compiler, CMake 3.15+, pkg-config, and the PulseAudio client headers (on Fedora: `sudo dnf install pulseaudio-libs-devel`; on Debian/Ubuntu: `sudo apt install libpulse-dev`). The build fetches fredemmott's StreamDeck C++ SDK automatically.

```bash
cd audio-switcher-exe
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
cmake --install build
```

This compiles the plugin and installs it into `~/.config/opendeck/plugins/com.morganscruggs.audioswitcherplus.sdPlugin`.

If OpenDeck is running, fully quit it (or restart it / its systemd unit) before installing and relaunch it afterward so it picks up the new binary.

### 3. Add a button

1. Launch **OpenDeck**.
2. Create a **Toggle Audio Device** (to switch between two devices) or **Set Audio Device** button.
3. In the property inspector, pick your devices — for example **Headphones** as one and **Line Out** as the other.
4. Press the button — your audio output switches between the two devices.

The devices OpenDeck offers are the same ones `pactl list sinks` shows on your system.

### Watching the plugin

OpenDeck writes a plugin log you can tail to confirm the plugin registered and to see what it's doing:

```bash
tail -f ~/.local/share/opendeck/logs/plugins/com.morganscruggs.audioswitcherplus.sdPlugin.log
```

## Custom Icons & Colors

Both "Set Audio Device" and "Toggle Audio Device" buttons let you pick an icon (Earbuds, Headphones, Mic, or Speaker) instead of the plugin's default images, plus a color for each of the button's states:

- **Set Audio Device** has one icon with two colors — "On" (shown when the configured device is the active one) and "Off" (shown otherwise, defaulting to a dimmed grey).
- **Toggle Audio Device** has two icons — one per device it switches between — each with its own color, defaulting to white.

Icon choices are picked up automatically from [`shared/AudioDevicesIcons/`](shared/AudioDevicesIcons/) (copied into the plugin at build time); each file must be a white silhouette on a transparent background (the colors above are applied by recoloring the shape at runtime, not by swapping images). Drop in a new `<name>.png` file there to add another choice without any code changes.

## Building on Windows

The same native build also works on the official Stream Deck app for Windows. See the [exe README](audio-switcher-exe/README.md#building-from-source) for the Windows build steps (requires Visual Studio 2022). The Windows build installs into `%APPDATA%\Elgato\StreamDeck\Plugins\` — fully quit Stream Deck before building, then relaunch it afterward.

## Getting Help

OpenDeck keeps a per-plugin log — see [Using the plugin in OpenDeck](#using-the-plugin-in-opendeck) to find it. This is a personal project shared in the hope that others find it useful; I'm unable to offer support or to act on bug reports or feature requests. Do not contact me for help via any means, including GitHub, Discord, Twitter, Reddit, or email.

If 'fuzzy matching' is required — or not functioning properly for you — ask your device manufacturer to fix their device/drivers to not change device IDs; on Linux this is rarely an issue since PulseAudio names are stable.

## Thanks

- Thanks to [Fred Emmott](https://github.com/fredemmott) for creating the original [StreamDeck-AudioOutputSwitcher](https://github.com/fredemmott/StreamDeck-AudioOutputSwitcher) that this project is forked from.
- Thanks to "EreTIk" for finding/documenting the COM interface the Windows build relies on.
- Thanks to "LordValgor" for the idea of making this plugin.

## License

This project is [MIT-licensed](LICENSE)
