"""Read-only short connections are silent unless --debug is enabled."""
import json
from pathlib import Path
import subprocess
import tempfile
import time
from integration import Peer, DAEMON

for debug in (False, True):
    with tempfile.TemporaryDirectory(prefix='lotus-log-policy-') as directory, tempfile.TemporaryFile() as log:
        path = Path(directory) / 'control.sock'
        process = subprocess.Popen([DAEMON, '--mock', '--socket', str(path)] +
                                   (['--debug'] if debug else []), stderr=log)
        try:
            for _ in range(100):
                if path.exists(): break
                time.sleep(.01)
            for _ in range(3):
                peer = Peer(path)
                assert peer.request('state.get')['ok']
                peer.close()
            time.sleep(.15)
        finally:
            process.terminate(); process.wait(timeout=3)
        log.seek(0)
        events = [json.loads(line) for line in log if line.strip()]
        connections = [e for e in events if e['event'] in ('connected', 'disconnected')]
        assert len(connections) == (6 if debug else 0), connections
        assert all(e['level'] == 'DEBUG' for e in connections)
        assert any(e['event'] == 'ready' and e['level'] == 'INFO' for e in events)
print('Default quiet short connections and opt-in DEBUG logging passed')
