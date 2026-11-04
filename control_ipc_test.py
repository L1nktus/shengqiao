"""Exercise the real native socket/client against a fake radio-free Swift host."""
import concurrent.futures
import json
import os
from pathlib import Path
import socket
import stat
import subprocess
import sys
import tempfile
import time

host,client=(str(Path(p).resolve()) for p in sys.argv[1:3])
with tempfile.TemporaryDirectory(prefix='btm-') as folder:
    address=str(Path(folder)/'control.sock')
    environment=dict(os.environ,BTMIX_TEST_SOCKET=address)
    def launch():
        process=subprocess.Popen([host,address],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        assert process.stdout.readline().strip()=='READY'
        return process
    def call(command,ok=True):
        result=subprocess.run([client,command],env=environment,capture_output=True,text=True,timeout=5)
        reply=json.loads(result.stdout)
        assert reply['ok']==ok,(result.returncode,reply)
        assert (result.returncode==0)==ok
        return reply
    process=launch()
    try:
        assert stat.S_IMODE(Path(address).stat().st_mode)==0o600
        assert call('status')['state']=='off'
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            replies=list(pool.map(call,['on']*4))
        assert all(r['state']=='on' and r['starts']==1 for r in replies)
        assert call('off')['state']=='off'
        assert call('off')['stops']==1
        assert call('on')['starts']==2
        first=subprocess.Popen([client,'off'],env=environment,stdout=subprocess.PIPE,text=True)
        time.sleep(.04)
        second=call('on')
        old=json.loads(first.communicate(timeout=5)[0])
        assert not old['ok'] and old['code']=='superseded'
        assert second['state']=='on' and second['starts']==3
        before=call('status')
        for message in [b'on\nstatus\n',b'x'*600+b'\n']:
            with socket.socket(socket.AF_UNIX) as connection:
                connection.connect(address);connection.sendall(message)
                assert not json.loads(connection.recv(8192))['ok']
        assert call('status')['starts']==before['starts']
        call('unsupported',False)
        assert call('toggle')['state']=='off'
        assert call('toggle')['state']=='on'
        # A disappearing client must not deliver SIGPIPE to the menu app.
        with socket.socket(socket.AF_UNIX) as connection:
            connection.connect(address);connection.sendall(b'status\n')
        time.sleep(.04)
        assert call('status')['state']=='on'
        process.terminate();process.wait(timeout=3)
        process=launch()
        assert call('status')['state']=='off'
        print('PASS: real native client/socket, duplicate on/off, transition race, toggle, invalid/oversized requests, abandoned client, owner permissions and stale socket recovery; no Bluetooth APIs used')
    finally:
        process.terminate();process.wait(timeout=3)
        if process.returncode not in [0,-15]:
            print('TEST_HOST_EXIT',process.returncode,process.stderr.read(),file=sys.stderr)
