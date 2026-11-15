"""Exercise the actual CLI prompts and requests against an isolated IPC stub."""
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time

binary = sys.argv[1]


def run_case(arguments, input_text, destination, permissions='', confirm=True,
             invalid=False):
    with tempfile.TemporaryDirectory(prefix='apm-app-cli-') as home:
        root = Path(home) / 'APMEmulator/data/apm'
        security = root / '.security'
        security.mkdir(parents=True)
        (security / 'session.bin').write_text(
            f'token:fixture\nexpires:{int(time.time()) + 300}\nhmac:fixture\n')
        endpoint = root / 'apmd.socket'
        requests = []
        failures = []
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
            server.bind(str(endpoint))
            server.listen(2)
            server.settimeout(10)

            def respond():
                try:
                    for _ in range(0 if invalid else 2 if confirm else 1):
                        conn, _ = server.accept()
                        with conn:
                            conn.settimeout(10)
                            data = b''
                            while b'\n\n' not in data:
                                part = conn.recv(65536)
                                if not part:
                                    raise RuntimeError('Incomplete request')
                                data += part
                            fields = dict(line.split(':', 1) for line in
                                          data.decode().strip().split('\n'))
                            requests.append(fields)
                            conn.sendall(b'status:ok\nmessage:fixture app plan\n'
                                         b'apprepo:1\npackages:microg\nplan:fixture-plan\n\n')
                except Exception as error:
                    failures.append(error)

            worker = threading.Thread(target=respond)
            worker.start()
            result = subprocess.run([binary, *arguments], input=input_text,
                                    capture_output=True, text=True,
                                    env=dict(os.environ, HOME=home), timeout=10)
            worker.join(timeout=11)
            if failures:
                raise failures[0]
            assert not worker.is_alive(), 'IPC test did not finish'
            assert result.returncode == (1 if invalid else 0), (result.stdout, result.stderr)
            if invalid:
                assert not requests
                return
            assert requests[0]['type'] == 'APP_INSTALL'
            assert requests[0]['package'] == 'microg'
            assert requests[0]['session'] == 'fixture'
            assert requests[0]['simulate'] == '1'
            assert requests[0]['destination'] == destination
            assert requests[0].get('permissions', '').replace('\\n', '\n') == permissions
            assert 'fixture app plan' in result.stdout
            if confirm:
                assert len(requests) == 2
                assert requests[1]['expected_plan'] == 'fixture-plan'
                assert requests[1]['destination'] == destination
                assert 'simulate' not in requests[1]
                assert requests[1].get('permissions', '') == requests[0].get('permissions', '')
            else:
                assert len(requests) == 1
                assert 'Installation aborted' in result.stdout


run_case(['app-install', 'microg'], 'y\n', 'app')
run_case(['app-install', 'microg'], 'n\n', 'app', confirm=False)
run_case(['app-install', 'microg', '--install-as-system'], '\ny\n', 'app')
run_case(['app-install', 'microg', '--install-as-system'], '2\n\ny\n', 'priv-app')
run_case(['app-install', 'microg', '--install-as-system'],
         '2\nandroid.permission.GET_ACCOUNTS, android.permission.INTERNET\ny\n',
         'priv-app', 'android.permission.GET_ACCOUNTS\nandroid.permission.INTERNET')
run_case(['app-install', 'microg', '--install-as-system'], 'unexpected\n', 'app', invalid=True)
run_case(['app-install', 'microg', '--unknown'], '', 'app', invalid=True)
print('Seven CLI destination, permissions, confirmation and invalid-option checks passed.')
