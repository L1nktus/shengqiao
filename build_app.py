#!/usr/bin/env python3
"""Build a self-contained, ad-hoc-signed app for this user's Apple Silicon Mac."""
import argparse
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build-dir', type=Path, default=ROOT / 'build')
parser.add_argument('--btstack-root', type=Path, default=ROOT / 'vendor/btstack')
parser.add_argument('--portaudio-root', type=Path, default=ROOT / 'vendor/portaudio')
parser.add_argument('--output', type=Path, default=ROOT.parent / '声桥.app')
args = parser.parse_args()
BUILD = args.build_dir.resolve()
BTSTACK = args.btstack_root.resolve()
PORTAUDIO = args.portaudio_root.resolve()
APP = args.output.resolve()
CONTENTS = APP / 'Contents'
RESOURCES = CONTENTS / 'Resources'
MACOS = CONTENTS / 'MacOS'
FRAMEWORKS = CONTENTS / 'Frameworks'

def run(*arguments):
    subprocess.run(arguments, check=True)

run('cmake', '-S', str(ROOT), '-B', str(BUILD), '-DBTSTACK_ROOT=' + str(BTSTACK),
    '-DPORTAUDIO_ROOT=' + str(PORTAUDIO))
run('cmake', '--build', str(BUILD), '-j', '8')
for directory in [RESOURCES, MACOS, FRAMEWORKS]:
    directory.mkdir(parents=True, exist_ok=True)
run('xcrun', 'swiftc', '-swift-version', '5', '-parse-as-library', '-target', 'arm64-apple-macosx14.0', '-O',
    '-framework', 'AppKit', str(ROOT / 'ReceivingControl.swift'), str(ROOT / 'LocalControlServer.swift'),
    str(ROOT / 'MenuBar.swift'), '-o', str(MACOS / 'BluetoothMixer'))
shutil.copy2(BUILD / 'btmixctl', MACOS / 'btmixctl')
(MACOS / 'btmixctl').chmod(0o755)
for name in ['bluetooth-mixer-engine', 'session-supervisor', 'bt-power']:
    shutil.copy2(BUILD / name, RESOURCES / name)
    (RESOURCES / name).chmod(0o755)
audio_library = FRAMEWORKS / 'libportaudio.2.dylib'
if audio_library.exists():
    audio_library.chmod(0o755)
shutil.copy2(PORTAUDIO / 'lib/libportaudio.2.dylib', audio_library)
audio_library.chmod(0o755)
engine = RESOURCES / 'bluetooth-mixer-engine'
for line in subprocess.check_output(['/usr/bin/otool', '-L', str(engine)], text=True).splitlines()[1:]:
    dependency = line.strip().split(' (compatibility version')[0]
    if Path(dependency).name.startswith('libportaudio'):
        run('/usr/bin/install_name_tool', '-change', dependency,
            '@loader_path/../Frameworks/libportaudio.2.dylib', str(engine))
run('/usr/bin/install_name_tool', '-id', '@rpath/libportaudio.2.dylib', str(FRAMEWORKS / 'libportaudio.2.dylib'))
for name in ['BTSTACK-LICENSE.txt', 'PORTAUDIO-LICENSE.txt', 'APACHE-2.0.txt', 'SBC-NOTICES.txt']:
    shutil.copy2(ROOT / name, RESOURCES / name)
info = {
    'CFBundleExecutable': 'BluetoothMixer', 'CFBundleIdentifier': 'local.codex.BluetoothMixer',
    'CFBundleName': '声桥', 'CFBundleDisplayName': '声桥',
    'CFBundlePackageType': 'APPL', 'CFBundleShortVersionString': '1.0.7',
    'CFBundleVersion': '107', 'LSUIElement': True, 'LSMinimumSystemVersion': '14.0',
    'NSBluetoothAlwaysUsageDescription': '将 Mac 内置蓝牙用作音频接收端，播放一台已连接设备的声音。',
    'NSHighResolutionCapable': True,
}
(CONTENTS / 'Info.plist').write_bytes(plistlib.dumps(info))
for file in [MACOS / 'btmixctl', FRAMEWORKS / 'libportaudio.2.dylib', *(RESOURCES / name for name in ['bluetooth-mixer-engine', 'session-supervisor', 'bt-power'])]:
    run('/usr/bin/codesign', '--force', '--sign', '-', str(file))
run('/usr/bin/codesign', '--force', '--sign', '-', '--identifier', 'local.codex.BluetoothMixer', str(APP))
run('/usr/bin/codesign', '--verify', '--deep', '--strict', str(APP))
print('APP_READY', APP)
