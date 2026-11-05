# 声桥 1.0.7 source

Native menu-bar A2DP receiver for the owner's tested Apple Silicon Mac. Runtime consists of AppKit/Swift UI, native C session supervisor + guardian, BTstack with a native Skywalk HCI/ACL transport, SBC decoders and per-source mixing, CoreAudio through bundled PortAudio, and a native Bluetooth-power helper. Python is only a build/test tool.

## Rebuild offline

Requires Apple Silicon macOS, Xcode Command Line Tools (clang/swiftc), CMake 3.20+, Python 3. The pinned source subset and arm64 PortAudio 19.7.0 header/dylib are included in `vendor/`; no Homebrew or online checkout is needed during compilation.

```sh
python3 build_app.py
```

Default build directory is `build/`; app output is `../声桥.app`. Override with `--build-dir` or `--output`. Optional dependency overrides: `--btstack-root`, `--portaudio-root`. Normal app launch starts receiving OFF. There is no installation script to run for ordinary use: quit the old app, copy the new .app to Applications, then open it.

The original deployment-only helper is intentionally omitted from this public repository. Quit the old app and manually copy the newly built app to Applications; building does not install or start reception.

## Offline checks

```sh
./build/mixer-test
python3 supervisor_test.py ./build/session-supervisor
xcrun swiftc -swift-version 5 -parse-as-library ReceivingControl.swift ReceivingControlTests.swift -o ./build/receiving-control-test
./build/receiving-control-test
xcrun swiftc -swift-version 5 -parse-as-library ReceivingControl.swift LocalControlServer.swift ControlTestHost.swift -o ./build/control-test-host
xcrun clang -fobjc-arc -Wall -Wextra -Werror -Wno-unused-function -DBTMIX_TEST_SOCKET=1 btmixctl.m -framework Foundation -o ./build/btmixctl-test
python3 control_ipc_test.py ./build/control-test-host ./build/btmixctl-test
```

Mixer tests cover independent 48/44.1 kHz sources and actual two-tone summed output, source/master gains, saturation/mute, stale burst trimming, recovery when another playing source pauses or disconnects, and FLUSH preserving controls while removing old PCM. These do not connect Bluetooth or measure resource use.

Pairing tests must use a fresh temporary directory and synthetic keys only. Run `pair-store-test <directory> write`, then `read`, `delete`, `empty` as separate invocations; verify directory 0700 and file 0600. Never run this against actual user pairing data.

`bluetooth-mixer-engine --check` only verifies that a built-in output device exists. Do not directly start the engine: use the app/supervisor, which owns and restores the controller state.

## Architecture and compatibility

- UI communicates with a broker via stdin/stdout JSON; a separate guardian holds the exclusive file lock and controls restoration. Broker disappearance, EOF and missed heartbeats end the engine; there is no five-minute session timeout.
- HCI/ACL channels are looked up dynamically in IORegistry. `native_channel.c` uses the private Skywalk channel ABI; no guessed kernel offsets, system binary patches, SIP changes or daemon unloads.
- Receiver dynamically provisions SBC A2DP endpoints, has per-peer AVRCP volume state, and persists its own link keys atomically per local controller. More than one actual link is verified; maximum physical concurrency is not established.
- Windows 11 service negotiation failed with unused ERTM capability advertisement enabled. Disabling `ENABLE_L2CAP_ENHANCED_RETRANSMISSION_MODE` and refreshing Windows service enumeration resolved this on the tested PC. Existing SDP records were queried successfully and playback was manually confirmed.
- `sdp_server_trace.c` is a copy of the pinned upstream `src/classic/sdp_server.c` with metadata-only diagnostic logging. L2CAP trace logs signaling only. No media or link-key bytes are logged. `vendor/btstack` is an otherwise unmodified upstream subset, pinned in `vendor/versions.json`.
- Per-source decoded queues start at 30 ms. Actual starvation raises only the affected target in 15 ms steps, capped at 90 ms. Queues above a fixed 160 ms stale-data bound trim to target. A stream stopping/disconnecting restores the last playing survivor to 30 ms and trims its queue; FLUSH empties current decoded queues and partial media fragments and restores 30 ms while preserving pairing/connections/volume. The guard is larger than in 1.0 because the old 80 ms bound repeatedly clipped normal delivery bursts in the reported concurrent playback session. It does not flush source-device or controller queues or promise an end-to-end latency value.
- `outputMs` uses PortAudio output timing, not a loopback or acoustic latency measurement. Diagnostic queue counters are audio correctness data, not power/CPU benchmarks.

Mac16,1 / M4 was originally verified on macOS 27.0.1 (26A434). After upgrading to 27.2 (26B118), the user reported normal operation; the 1.0.2 lifecycle/control checks are recorded in the release validation document. The declared minimum build target 14.0 is not a compatibility certification. A future macOS update may restrict these private channels; repairs cannot be guaranteed.

## 1.0.2 same-user remote control

Use the bundled native client from iPhone/iPad Shortcuts' Run Script over SSH, as the logged-in Mac user without sudo:

```sh
"/Applications/声桥.app/Contents/MacOS/btmixctl" on
"/Applications/声桥.app/Contents/MacOS/btmixctl" off
"/Applications/声桥.app/Contents/MacOS/btmixctl" status
"/Applications/声桥.app/Contents/MacOS/btmixctl" toggle
```

The GUI owns a mode-0600 Unix socket in its mode-0700 Application Support folder. Both ends check the peer UID. The protocol accepts only the four commands, with bounded reads/client counts/timeouts. There is no new TCP listener. `on`/`off` are idempotent; successful replies wait for actual readiness or supervisor completion/restoration. A new conflicting target supersedes a pending request. JSON `ok:true` returns exit 0; failures return 1 with a code/message. `status` distinguishes requested state from actual readiness. `on`/`toggle` can launch the app through LaunchServices; `off`/`status` do not launch a closed app. An orphaned guardian lock is allowed to finish recovery before a new start.

The Mac must be awake with the same user logged into the GUI session. SSH transport, authentication, network reachability and wake-on-demand remain OS/user configuration. This release does not configure SSH, create credentials, add a daemon, or expose native App Intents actions in the Shortcuts library.

Bluetooth authorization is checked before starting the supervisor. If undetermined, the app activates and requests permission using CoreBluetooth, without scanning or connecting through it; the temporary manager is released before reception starts. If denied, the app reports the Privacy & Security setting to enable. Ad-hoc rebuilds can invalidate a previous Bluetooth grant; a one-time local approval may be needed for each changed build. `needs_bluetooth_permission` indicates that approval is still pending. This avoids killing a helper while its system permission request is delayed. The supervisor also resets its own SIGCHLD disposition so it can reap children regardless of caller signal settings, and emits bounded failure metadata for its power helper.

Controller tests cover duplicate requests, restart during restoration, cancelled startup and failure/retry. Native socket/client tests use a fake radio-free host and cover concurrency, malformed input, abandoned clients and stale sockets. Supervisor tests use fake helpers only, including inherited ignored SIGCHLD. No test in these suites opens Bluetooth or measures power/resources. See the release validation document for actual hardware checks and mobile-side verification limits.

## Licenses and privacy

Retain BTSTACK-LICENSE.txt, PORTAUDIO-LICENSE.txt, APACHE-2.0.txt, SBC-NOTICES.txt and upstream headers. BTstack's supplied license permits personal noncommercial use and requires separate handling for commercial use. Pairing files, user logs, Bluetooth addresses and audio recordings are excluded from this source bundle.

## 1.0.1 notes and regression evidence

Each connected source has a native 备注… button. The modal field stores a local alias under name.<stable Bluetooth address> in the existing app preferences. It is used immediately and after reconnect/restart; an empty value removes the alias. It never changes the Bluetooth-advertised remote name, pairing data or transport commands. Input is normalized to one line and capped at 64 composed characters. UI automation remains unavailable for this app; the user confirmed the remark button/save works and Windows stutter improved on the installed 1.0.1.

The user confirmed 1.0 FLUSH and latency recovery after stopping mixing, and reported slight Windows stutter. Audio metadata showed repeated source underruns/trimming while PortAudio reported zero output underruns. A deterministic 30-cycle / 100 ms delivery-burst simulation with the previous mixer produced 30 source underruns and 100829 discarded frames. With 1.0.1 it produced 2 initial adaptation underruns, no discarded frames and no silent samples in the final burst. This is a functional audio-queue simulation, not an end-to-end latency or resource benchmark. A new regression covers burst delivery plus reset to the low-latency target on pause/FLUSH; all original mixing tests remain passing.

## 1.0.3 product rename

Display name, menu/help/accessibility labels and app bundle filename are now 声桥. CFBundleIdentifier, executable identifiers, Application Support/Logs paths, stored preferences, controller pairing DB, Bluetooth-advertised Mac Switch Speaker name and the audio backend remain unchanged. Update SSH scripts to /Applications/声桥.app/Contents/MacOS/btmixctl. The companion native shortcut is 声桥开关: one SSH action toggles once and queries actual state, then a Show Alert action displays its result. Credentials stay inside the user-owned shortcut; this source archive contains no credentials or exported private shortcut.

## 1.0.4 remark focus fix

The custom menu button now ends tracking without animation, then schedules the remark dialog in the default run-loop mode. In the modal run-loop mode the alert explicitly becomes key and assigns/selects its editable field. This prevents opening the modal alert from inside the still-active menu tracking callback. Audio/session/control code and alias storage remain unchanged. Physical keyboard and IME behavior still require user confirmation; no UI access workaround or global keyboard hooks are used.

## 1.0.6 single-source automatic takeover

A new Classic audio peer is accepted and automatically takes over, without requiring any action on the Mac or manual disconnection of the previous source. At the new ACL request, old PCM/fragment queues are stopped and cleared and old connected ACL handles are disconnected. A previously pending peer completing after a newer request is disconnected as superseded. The latest requested peer alone may configure/start/play audio. The UI reports only that current peer.

Controller disconnect is asynchronous, so ACLs and SBC endpoint slots may briefly overlap during handover. Spare endpoints are created only if none is available for the incoming handover; this does not permit simultaneous playback. Media and stream-start callbacks reject stale ownership; old source queues stop immediately at handover. Persistent pairings, gain, remarks and SSH commands remain unchanged. The existing 30 ms initial/starvation-adaptive buffer is retained. No end-to-end latency measurement or guarantee is made.

`receiver-policy-test` uses the real receiver/BTstack and synthetic HCI events to verify B is admitted while A disconnects, A can switch back, latest pending request wins, and failed attempts can retry. No Bluetooth hardware or real pairing DB is opened. Runtime status reports connectionLimit=1 (current audio source) and switchingPolicy=newestWins.


## 1.0.7 trusted-device reconnection

“允许新设备配对” gates unknown addresses only. Known devices may reconnect and repeat SSP confirmation while it is off. Unknown ACL requests are rejected before takeover so they cannot interrupt the active known source. With the switch on, newest-device automatic takeover remains unchanged.

Successful SSP on an admitted connection adds its address to a private per-controller `.trusted` sidecar, independently of whether the peer requests bonding. Existing saved link-key addresses are migrated automatically. Link keys and their file format are unchanged. The address-only sidecar is atomic, mode 0600, and excluded from releases. A device with neither a saved key nor trust entry needs one successful pairing with the switch on. Failed SSP never adds trust. Address recognition is a convenience policy, not cryptographic proof of identity when fresh Just Works pairing is accepted.

`receiver-pairing-test <temporary-directory>` uses a fake transport with the real receiver/BTstack and checks actual 0x042c/0x042d confirmation commands, no-bond trust persistence, failed pairing and unknown-device gating. A test-only translation unit sets the fake stack state; the production engine compiles upstream hci.c directly and has no test-state hook. Physical NS2 reconnection with the switch off still requires user validation.

## Public-source rebuild note (2026-11-04)

Fresh offline app build/signature verification passed. The bundled PortAudio dylib records LC_BUILD_VERSION minos 26.0 (SDK 26.0); this takes precedence over any impression of a macOS 14 compatibility promise from the project build target. The library marker is a binary constraint, not evidence that the complete app supports macOS 26. The actual development and use environment is macOS 27 on an M4 MacBook Pro; macOS 14–26 compatibility has not been verified. Building and synthetic tests do not start the receiver.
