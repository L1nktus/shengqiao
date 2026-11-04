"""Lifecycle tests use fake helpers only. Never touches a Bluetooth API."""
import json
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile
import time

SUPERVISOR = Path(sys.argv[1]).resolve()

def wait_until(predicate, timeout=8):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if predicate():
            return
        time.sleep(0.05)
    raise AssertionError('deadline expired')

with tempfile.TemporaryDirectory(prefix='mixer-supervisor-') as temporary:
    root = Path(temporary)
    resources = root / 'fake-resources'
    resources.mkdir()
    helper = resources / 'bt-power'
    helper.write_text('''#!/usr/bin/python3
from pathlib import Path
import sys
p=Path(__file__).with_name('radio-state')
if sys.argv[1]=='state': print(p.read_text())
else:
 p.write_text('1' if sys.argv[1]=='on' else '0')
 print('POWER_CONFIRMED '+p.read_text())
''')
    engine = resources / 'bluetooth-mixer-engine'
    engine.write_text('''#!/usr/bin/python3
from pathlib import Path
import os,sys
Path(__file__).with_name('engine-started').write_text(str(os.getpid()))
print('{"event":"ready"}',flush=True)
for line in sys.stdin:
 if line.strip()=='CRASH': os._exit(9)
 if line.strip()=='STOP': break
 with Path(__file__).with_name('commands').open('a') as f: f.write(line)
''')
    helper.chmod(0o755); engine.chmod(0o755)
    radio = resources / 'radio-state'
    for scenario, original in [('normal_on', '1'), ('normal_off', '0'), ('engine_crash', '1'), ('supervisor_crash', '1'), ('ignored_sigchld', '1')]:
        radio.write_text(original)
        (resources / 'engine-started').unlink(missing_ok=True)
        process = subprocess.Popen([str(SUPERVISOR), '--run', str(resources), str(root / scenario)],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   preexec_fn=(lambda: signal.signal(signal.SIGCHLD,signal.SIG_IGN)) if scenario=='ignored_sigchld' else None)
        wait_until(lambda: (resources / 'engine-started').exists())
        assert radio.read_text() == '0'
        if scenario == 'engine_crash':
            process.stdin.write(b'CRASH\n'); process.stdin.flush()
        elif scenario == 'supervisor_crash':
            process.kill()
        else:
            process.stdin.write(b'MASTER 51\nGAIN 200\n'); process.stdin.flush()
            time.sleep(0.2)
            process.stdin.write(b'STOP\n'); process.stdin.flush()
        process.wait(timeout=10)
        wait_until(lambda: radio.read_text() == original)
        process.stdin.close(); process.stdout.close()
        print('PASS:', scenario, 'original power state restored', flush=True)
    forwarded = (resources / 'commands').read_text()
    assert 'MASTER 51' in forwarded and 'GAIN 200' in forwarded
    print('PASS: live controls forwarded; tests used fake helpers exclusively', flush=True)
