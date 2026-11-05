# 声桥 — Shengqiao

[简体中文](README.md) | **English**

Use a Mac as a Bluetooth audio receiver and play audio from Nintendo Switch / Switch 2 and other Bluetooth audio sources through the Mac's speakers.

The project started with a simple need: a docked Switch 2 was connected to a monitor without speakers, and the user wanted to hear game audio through a Mac. The current version is **1.0.7**, with single-source playback and automatic takeover by a newly connected device.

## AI-assisted development and maintenance

The account owner defines the requirements and primarily uses AI (Codex) to help write, modify, test, and maintain this project. **The account owner has no programming knowledge and cannot independently review the entire implementation.** The source is published to preserve the work, share practical experience, and help other developers and AI tools improve it.

This is a personal-use project. Professional human auditing, ongoing maintenance, and compatibility with every device are not guaranteed. Documentation distinguishes automated tests, simulations, and real-device experience. Issues and pull requests are welcome. Viewing, downloading, or forking this public repository does not grant write access; proposed changes are merged at the maintainer's discretion.

## Before using it

**Experimental software developed and used on macOS 27 with an M4 MacBook Pro, relying on private macOS Skywalk Bluetooth interfaces.** Actual validation is limited to that environment. Compatibility with other macOS versions, chips, and future updates is unconfirmed.

While receiving, the supervisor temporarily takes over the Mac's built-in Bluetooth controller. This may affect system Bluetooth devices such as headphones, mice, and keyboards. It attempts to restore the previous state when reception stops. **This is not an ordinary player that leaves system Bluetooth unaffected.** The project does not change SIP, install kernel patches, or unload system Bluetooth services. Building and synthetic tests do not take over Bluetooth; real reception requires macOS Bluetooth permission.

Dependencies include BTstack under personal/noncommercial terms. See the license section below: the entire repository must not be treated as uniformly MIT-licensed or unrestricted for commercial use.

## Features

- A menu bar control for receiving SBC / A2DP audio.
- Single-source playback: a newly connected device replaces the previous audio source without requiring a manual disconnect on the Mac.
- Volume, mute, device aliases, and an “Allow new device pairing” option. Trusted devices can reauthenticate when new pairing is disabled.
- Local control commands for an existing SSH / iOS Shortcuts setup. The app does not enable SSH or configure credentials automatically.

Normal app launch starts with reception off. The controller is taken over only after the user explicitly enables reception. The Mac must remain awake. Publishing this source does not change reception or system settings on the original machine.

## Build

The actual development and use environment is **macOS 27 on Apple Silicon (M4)**. Building also requires Xcode Command Line Tools, CMake 3.20+, and Python 3. The repository includes a pinned BTstack source subset and a prebuilt arm64 PortAudio 19.7.0 library. That library is a dependency, not pairing data or a personal runtime file.

The `14.0` values in the build scripts and app metadata are configured minimum deployment targets, not the development OS version or a verified minimum runtime version. The bundled PortAudio library separately declares a minimum OS version of `26.0`; this does not establish that the complete app works on macOS 26. **The documented real-world use is on macOS 27. Compatibility with macOS 14–26 has not been verified.**

```sh
python3 build_app.py --output build/声桥.app
```

Building creates the app without installing it or taking over Bluetooth. When ready to install, quit the old app, copy the new app to Applications, and open it. To use your own PortAudio build, pass `--portaudio-root` pointing to its headers and library directory.

## Tests and validation scope

```sh
./build/mixer-test
./build/receiver-policy-test
python3 pairing_test.py ./build
python3 supervisor_test.py ./build/session-supervisor
```

These tests use fake transports, temporary pairing data, or simulated helpers; they do not open real Bluetooth hardware. Additional local-control tests are documented in the [technical notes](docs/technical-notes.md).

The user previously confirmed actual Switch 2 connection and audio playback, as well as playback from a Windows device. They also reported normal operation after updating the original Mac's OS. Simulations alone do not establish full real-device validation of automatic source takeover, NS2 reconnection with new pairing disabled, or compatibility across Macs. No guaranteed end-to-end latency or comprehensive device coverage has been established.

## Remote control

Through an existing SSH connection as the same logged-in Mac user, run:

```sh
"/Applications/声桥.app/Contents/MacOS/btmixctl" status
"/Applications/声桥.app/Contents/MacOS/btmixctl" toggle
```

The client also supports `on` and `off`. Credentials remain in the user's own SSH / Shortcuts configuration and must not be committed. The control interface uses a same-user Unix socket; it does not add a TCP listener.

## Licenses and upstream sources

Original project code is covered by [MIT](LICENSE). **Third-party code and derived files retain their original license terms:**

- [BTstack](https://github.com/bluekitchen/btstack), pinned to commit `51e11ce3692abfe31b4ee3d66cffdeb1ae1626f2`: the [included license](BTSTACK-LICENSE.txt) restricts use to personal, noncommercial purposes; commercial use requires separate licensing arrangements. `sdp_server_trace.c` is derived from this version.
- [PortAudio](https://www.portaudio.com/) 19.7.0: [copyright and license](PORTAUDIO-LICENSE.txt). An arm64 dynamic library is currently included.
- SBC codecs and related components: [notices](SBC-NOTICES.txt), [Apache 2.0 text](APACHE-2.0.txt), and the original source-file headers.
- Pinned dependency versions are listed in [vendor/versions.json](vendor/versions.json). Other third-party notices remain in their source files. See also [license scope (Chinese)](LICENSES.md).

The repository and source packages exclude real pairing keys, device trust lists, personal aliases, logs, audio recordings, and private shortcuts. Please read the [contribution guidelines (Chinese)](CONTRIBUTING.md) before reporting issues. The [maintenance policy (Chinese)](MAINTENANCE.md) explains how the project is maintained with AI assistance at the account owner's request.
