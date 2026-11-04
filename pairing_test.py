"""Synthetic authentication/storage regression. Never uses the user's pairing DB."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

build=Path(sys.argv[1]).resolve()
def store(folder,operation):
    subprocess.run([str(build/'pair-store-test'),str(folder),operation],check=True)
with tempfile.TemporaryDirectory(prefix='shengqiao-trust-test-') as tmp:
    root=Path(tmp)
    subprocess.run([str(build/'receiver-pairing-test'),str(root/'receiver')],check=True)
    old=root/'legacy'
    store(old,'write')
    # Remove only the synthetic sidecar to emulate the previous release.
    next(old.glob('*.trusted')).unlink()
    store(old,'read');store(old,'trusted')
    store(old,'delete');store(old,'empty');store(old,'trusted')
    no_bond=root/'no-bond'
    store(no_bond,'trust-only');store(no_bond,'trusted');store(no_bond,'empty')
    sidecar=next(no_bond.glob('*.trusted'))
    assert sidecar.stat().st_size==14  # 8-byte header plus one address, no key.
    assert sidecar.stat().st_mode&0o777==0o600
    assert no_bond.stat().st_mode&0o777==0o700
    store(no_bond,'trust-only');assert sidecar.stat().st_size==14
    sidecar.write_bytes(b'corrupt synthetic data')
    store(no_bond,'untrusted');assert sidecar.read_bytes()==b'corrupt synthetic data'
    symlink=root/'symlink';symlink.mkdir()
    victim=root/'victim';victim.write_bytes(b'do not touch')
    (symlink/'pairings-010203040506.bin.trusted').symlink_to(victim)
    store(symlink,'untrusted');assert victim.read_bytes()==b'do not touch'
    print('PASS: legacy migration, no-key trust across processes, retained trust after key deletion, deduplication, private permissions, corrupt/symlink fail-closed; no real pairing data used.')
