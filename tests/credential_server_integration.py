"""Legacy WSS protocol integration; requires Python websockets and openssl.

xmake b credential_server_fixture
python3 tests/credential_server_integration.py --clients 2000
All databases, certificates and logs are isolated in a temporary directory.
"""
import argparse
import asyncio
import json
import os
from pathlib import Path
import resource
import signal
import socket
import sqlite3
import ssl
import statistics
import subprocess
import tempfile
import time

import websockets


class Server:
    def __init__(self, binary, directory, **settings):
        self.binary = binary
        self.directory = directory
        directory.mkdir(parents=True, exist_ok=True)
        self.settings = settings
        self.generation = 0
        self.proc = None
        subprocess.run([
            'openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
            '-keyout', str(directory / 'api.crossdesk.cn.key'),
            '-out', str(directory / 'api.crossdesk.cn_bundle.crt'),
            '-days', '1', '-subj', '/CN=localhost',
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.ssl = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        self.ssl.check_hostname = False
        self.ssl.verify_mode = ssl.CERT_NONE  # Isolated, self-signed loopback fixture.

    async def start(self):
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            self.port = sock.getsockname()[1]
        self.generation += 1
        self.log = self.directory / f'stdout-{self.generation}.log'
        self.stream = self.log.open('w')
        env = os.environ.copy()
        env.update(CROSSDESK_ICE_SERVERS='[]', ADMIN_USERNAME='', ADMIN_PASSWORD='')
        env.update(CROSSDESK_MAX_CONNECTIONS='8192', CROSSDESK_AUTH_WORKERS='2',
                   CROSSDESK_AUTH_QUEUE_CAPACITY='4096',
                   CROSSDESK_AUTH_SOURCE_CAPACITY='256',
                   CROSSDESK_AUTH_SOURCE_PER_MINUTE='60',
                   CROSSDESK_AUTH_TIMEOUT_SECONDS='600')
        env.update({key: str(value) for key, value in self.settings.items()})
        self.proc = subprocess.Popen(
            [self.binary, str(self.port), str(self.directory)], env=env,
            stdout=self.stream, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 15
        while 'Signal server listening' not in self.log.read_text():
            assert self.proc.poll() is None, self.log.read_text()[-5000:]
            assert time.monotonic() < deadline, 'startup timeout'
            await asyncio.sleep(.05)

    async def connect(self):
        return await websockets.connect(
            f'wss://127.0.0.1:{self.port}', ssl=self.ssl,
            open_timeout=30, close_timeout=3, ping_interval=3, ping_timeout=30,
            proxy=None)

    async def stop(self):
        if self.proc is None:
            return
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                await asyncio.wait_for(asyncio.to_thread(self.proc.wait), 12)
            except asyncio.TimeoutError:
                self.proc.kill()
                await asyncio.to_thread(self.proc.wait)
                raise AssertionError('shutdown did not drain pending authentication')
        self.stream.close()
        assert self.proc.returncode == 0, self.log.read_text()[-5000:]
        self.proc = None


async def login(ws, identity='', timeout=90):
    # No retry_after, new capability, or retry logic: old login request/response.
    await ws.send(json.dumps({'type': 'login', 'user_id': identity}))
    while True:
        result = json.loads(await asyncio.wait_for(ws.recv(), timeout))
        if result.get('type') == 'login':
            return result


async def source_wait(binary, output):
    server = Server(binary, output / 'source-wait', CROSSDESK_AUTH_WORKERS=1,
                    CROSSDESK_AUTH_QUEUE_CAPACITY=1,
                    CROSSDESK_AUTH_SOURCE_CAPACITY=1,
                    CROSSDESK_AUTH_SOURCE_PER_MINUTE=1,
                    CROSSDESK_AUTH_TIMEOUT_SECONDS=75)
    sockets = []
    try:
        await server.start()
        first = await server.connect(); sockets.append(first)
        assert (await login(first))['status'] == 'success'
        # A disconnected waiter must relinquish the one queue slot.
        abandoned = await server.connect(); sockets.append(abandoned)
        await abandoned.send(json.dumps({'type': 'login', 'user_id': ''}))
        await asyncio.sleep(.3)
        await abandoned.close()
        # The client close can finish before websocketpp finishes TLS shutdown.
        # Wait for the server's close callback, which queues cancellation.
        cleanup_deadline = time.monotonic() + 8
        while 'connection [2|] closed' not in server.log.read_text():
            assert time.monotonic() < cleanup_deadline, 'disconnected waiter retained'
            await asyncio.sleep(.05)
        waiting = await server.connect(); sockets.append(waiting)
        begun = time.monotonic()
        response = asyncio.create_task(login(waiting))
        await asyncio.sleep(.4)
        overflow = await server.connect(); sockets.append(overflow)
        failure = await login(overflow)
        assert failure['status'] == 'fail' and failure['reason'] == 'Credential service busy'
        await overflow.close()
        await waiting.send('{"type":"login","user_id":""}')
        await asyncio.sleep(20)
        assert not response.done(), ('legacy waiter failed early', response.result())
        pong = await waiting.ping()
        await asyncio.wait_for(pong, 3)
        result = await response
        elapsed = time.monotonic() - begun
        assert result['status'] == 'success' and 45 < elapsed < 75, (result, elapsed)
        duplicate = json.loads(await asyncio.wait_for(waiting.recv(), 3))
        assert duplicate['reason'] == 'Already authenticated', duplicate
        print(json.dumps({'case': 'source_wait_legacy_no_retry', 'result': 'PASS',
                          'wait_seconds': round(elapsed, 2),
                          'heartbeat_after_20s': 'PASS',
                          'disconnect_releases_slot': 'PASS',
                          'deferred_login_order': 'PASS',
                          'queue_full_does_not_consume_quota': 'PASS'}), flush=True)
    finally:
        await asyncio.gather(*(ws.close() for ws in sockets), return_exceptions=True)
        await server.stop()


async def expiry(binary, output):
    server = Server(binary, output / 'expiry', CROSSDESK_AUTH_SOURCE_PER_MINUTE=1,
                    CROSSDESK_AUTH_TIMEOUT_SECONDS=15)
    sockets = []
    try:
        await server.start()
        first = await server.connect(); sockets.append(first)
        assert (await login(first))['status'] == 'success'
        waiting = await server.connect(); sockets.append(waiting)
        start = time.monotonic()
        try:
            result = await login(waiting, timeout=23)
            assert result['status'] == 'fail' and result['reason'] == 'Credential service busy'
        except websockets.ConnectionClosed as error:
            assert error.rcvd and error.rcvd.code == 1008
        elapsed = time.monotonic() - start
        assert 14 <= elapsed < 23, elapsed
        print(json.dumps({'case': 'bounded_wait_expiry', 'result': 'PASS',
                          'seconds': round(elapsed, 2)}), flush=True)
    finally:
        await asyncio.gather(*(ws.close() for ws in sockets), return_exceptions=True)
        await server.stop()


async def burst(binary, output, count):
    server = Server(binary, output / 'burst', CROSSDESK_MAX_CONNECTIONS=8192,
                    CROSSDESK_AUTH_WORKERS=2, CROSSDESK_AUTH_QUEUE_CAPACITY=4096,
                    CROSSDESK_AUTH_SOURCE_CAPACITY=4096,
                    CROSSDESK_AUTH_SOURCE_PER_MINUTE=6000,
                    CROSSDESK_AUTH_TIMEOUT_SECONDS=120)
    sockets = []
    try:
        await server.start()
        seed = await server.connect()
        credentials = (await login(seed))['user_id']
        seed_id, password = credentials.split('@', 1)
        await seed.close(); await server.stop()
        # Isolated test identities reuse one test digest to avoid timing fixture
        # creation. Every login still performs the real Argon2id verification.
        with sqlite3.connect(server.directory / 'test.db') as db:
            salt, digest = db.execute(
                'SELECT password_salt,password_hash FROM devices WHERE device_id=?',
                (seed_id,)).fetchone()
            db.executemany(
                'INSERT INTO devices(device_id,password_salt,password_hash,retention_started_at) '
                'VALUES(?,?,?,?)',
                [(str(600000000 + i), salt, digest, int(time.time())) for i in range(count)])
        for phase in ['initial_burst', 'restart_burst']:
            await server.start()
            started = time.monotonic()
            latencies = []
            # Bound simultaneous TCP/TLS handshakes, not pending logins. macOS
            # defaults to a 128-entry listen backlog; overwhelming it tests the
            # OS accept queue instead of the credential recovery path.
            handshakes = asyncio.Semaphore(64)
            async def client(index):
                async with handshakes:
                    ws = await server.connect()
                sockets.append(ws)
                result = await login(ws, f'{600000000 + index}@{password}', timeout=125)
                assert result['status'] == 'success', result
                latencies.append(time.monotonic() - started)
            await asyncio.gather(*(client(i) for i in range(count)))
            await asyncio.sleep(5)
            # Verify that authenticated connections also serve ordinary signaling.
            await asyncio.gather(*(ws.send('{"type":"ping"}') for ws in sockets))
            replies = await asyncio.gather(*(asyncio.wait_for(ws.recv(), 10) for ws in sockets))
            assert all(json.loads(msg)['type'] == 'pong' for msg in replies)
            await server.stop()  # Disconnect every client as a real restart does.
            text = server.log.read_text()
            assert f'authenticated={count}' in text, 'online snapshot missing'
            assert 'authentication_timeouts_total=0' in text
            print(json.dumps({'case': phase, 'result': 'PASS', 'clients': count,
                              'p50_login_seconds': round(statistics.median(latencies), 2),
                              'p95_login_seconds': round(sorted(latencies)[int(count*.95)-1], 2),
                              'all_online_seconds': round(max(latencies), 2),
                              'concurrent_handshakes': 64,
                              'all_signaling_pings': 'PASS'}), flush=True)
            await asyncio.gather(*(ws.close() for ws in sockets), return_exceptions=True)
            sockets = []
    finally:
        await asyncio.gather(*(ws.close() for ws in sockets), return_exceptions=True)
        await server.stop()


async def deferred_backlog(binary, output):
    server = Server(binary, output / 'deferred-backlog',
                    CROSSDESK_AUTH_SOURCE_PER_MINUTE=1)
    sockets = []
    try:
        await server.start()
        seed = await server.connect(); sockets.append(seed)
        assert (await login(seed))['status'] == 'success'
        for _ in range(9):
            ws = await server.connect(); sockets.append(ws)
            await ws.send('{"type":"login","user_id":""}')
            await asyncio.sleep(.05)
            # Below each connection's cap, but collectively above the global
            # deferred-message capacity behind pending authentication.
            for _ in range(120):
                await ws.send('{"type":"login","user_id":""}')
            await asyncio.sleep(.05)
        overloaded = sockets[-1]
        await asyncio.wait_for(overloaded.wait_closed(), 8)
        assert overloaded.close_code == 1013, overloaded.close_code
        pong = await sockets[1].ping()
        await asyncio.wait_for(pong, 3)
        await asyncio.gather(*(ws.close() for ws in sockets[1:]))
        # Allow server TLS teardown / periodic closing-state cleanup to finish.
        await asyncio.sleep(5)
        fresh = await server.connect(); sockets.append(fresh)
        for _ in range(121):
            await fresh.send('{"type":"login","user_id":""}')
        await asyncio.sleep(1)
        pong = await fresh.ping()
        await asyncio.wait_for(pong, 3)
        print(json.dumps({'case': 'bounded_deferred_backlog', 'result': 'PASS',
                          'unrelated_waiter_alive': 'PASS',
                          'cleanup_releases_capacity': 'PASS'}), flush=True)
    finally:
        await asyncio.gather(*(ws.close() for ws in sockets), return_exceptions=True)
        await server.stop()


async def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--fixture', help='Fixture binary; auto-detects a unique build')
    parser.add_argument('--clients', type=int, default=128)
    parser.add_argument('--case', choices=['all', 'limits', 'burst'], default='all')
    args = parser.parse_args()
    assert 1 <= args.clients <= 4000
    if not args.fixture:
        fixtures = list(Path('build').glob('*/*/*/credential_server_fixture'))
        if len(fixtures) != 1:
            parser.error('pass --fixture with the built credential_server_fixture path')
        args.fixture = str(fixtures[0])
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    resource.setrlimit(resource.RLIMIT_NOFILE, (min(32768, hard) if hard != resource.RLIM_INFINITY else 32768, hard))
    output = Path(tempfile.mkdtemp(prefix='crossdesk-auth-integration-'))
    print('Integration logs: ' + str(output), flush=True)
    binary = str(Path(args.fixture).resolve())
    cases = []
    if args.case != 'burst':
        cases += [source_wait(binary, output), expiry(binary, output),
                  deferred_backlog(binary, output)]
    if args.case != 'limits':
        cases += [burst(binary, output, args.clients)]
    results = await asyncio.gather(*cases, return_exceptions=True)
    for result in results:
        if isinstance(result, BaseException):
            raise result


if __name__ == '__main__':
    asyncio.run(main())
