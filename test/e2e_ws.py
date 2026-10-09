"""End-to-end test of WebSocket and subscriptions against test/hardware firmware.

Usage:
  python3 test/e2e_ws.py <board-ip>

Runs the conformance vectors over WebSocket, then subscriptions: change
notifications (from HTTP writes, watch() sampling and explicit changed()),
events, filters, intervals, unsubscribe and cleanup on disconnect. Standard
library only. Prints "E2E pass=<n> fail=<n>".
"""
import base64
import json
import os
import socket
import struct
import sys
import time

import conformance
from conformance import ABSENT, Http


class WebSocket:
    """Just enough RFC 6455 for text frames."""

    def __init__(self, host, path='/ws', port=80, timeout=5):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall((f'GET {path} HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\n'
                           f'Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n'
                           'Sec-WebSocket-Version: 13\r\n\r\n').encode())
        head = b''
        while b'\r\n\r\n' not in head:
            chunk = self.sock.recv(1024)
            if not chunk:
                raise ConnectionError('handshake failed')
            head += chunk
        if b' 101 ' not in head.split(b'\r\n')[0]:
            raise ConnectionError(head.decode(errors='replace'))
        self.buf = head.split(b'\r\n\r\n', 1)[1]
        self.next_id = 1
        self.notifications = []

    def send(self, text):
        data = text.encode()
        mask = os.urandom(4)
        header = bytes([0x81])
        n = len(data)
        if n < 126:
            header += bytes([0x80 | n])
        elif n < 65536:
            header += bytes([0x80 | 126]) + struct.pack('>H', n)
        else:
            header += bytes([0x80 | 127]) + struct.pack('>Q', n)
        self.sock.sendall(header + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def _read(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError('closed')
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def recv(self, timeout=3):
        self.sock.settimeout(timeout)
        b0, b1 = self._read(2)
        n = b1 & 0x7f
        if n == 126:
            n = struct.unpack('>H', self._read(2))[0]
        elif n == 127:
            n = struct.unpack('>Q', self._read(8))[0]
        payload = self._read(n)
        return b0 & 0x0f, payload

    def message(self, timeout=3):
        end = time.time() + timeout
        while time.time() < end:
            op, payload = self.recv(max(0.05, end - time.time()))
            if op == 1:
                return json.loads(payload)
        raise TimeoutError

    def request(self, op, path, timeout=3, **kw):
        rid = self.next_id
        self.next_id += 1
        self.send(json.dumps({'id': rid, 'op': op, 'path': path, **kw}))
        end = time.time() + timeout
        while time.time() < end:
            msg = self.message(end - time.time())
            if msg.get('id') == rid:
                return msg
            self.notifications.append(msg)
        raise TimeoutError(f'no reply to {op} {path}')

    def wait_notification(self, pred, timeout=3):
        for i, m in enumerate(self.notifications):
            if pred(m):
                return self.notifications.pop(i)
        end = time.time() + timeout
        while time.time() < end:
            try:
                msg = self.message(end - time.time())
            except (TimeoutError, socket.timeout):
                break
            if pred(msg):
                return msg
            self.notifications.append(msg)
        return None

    def call(self, op, path, query, body):
        kw = dict(query)
        if body is not ABSENT:
            kw['body'] = body
        r = self.request(op, path, **kw)
        return r['status'], r.get('body')

    def close(self):
        try:
            self.sock.sendall(bytes([0x88, 0x80]) + os.urandom(4))
        except OSError:
            pass
        self.sock.close()


def main():
    host = sys.argv[1]
    http = Http(f'http://{host}/api')
    results = []

    def check(name, cond, detail=''):
        results.append(bool(cond))
        print(('PASS ' if cond else 'FAIL ') + name + ('' if cond else ': ' + str(detail)), flush=True)

    def subs():
        return http.call('get', '/system/subscriptions', {}, ABSENT)[1]

    base_subs = subs()
    ws = WebSocket(host)
    conformance.run(ws, 'ws', check)

    # Change notifications for writes made over another transport.
    r = ws.request('sub', '/lamp', keys='on,brightness')
    check('sub snapshot', r['status'] == 'ok' and r['body'] == {'on': False, 'brightness': 128}, r)
    check('subscription registered', subs() == base_subs + 1)
    http.call('set', '/lamp/brightness', {}, 77)
    n = ws.wait_notification(lambda m: m.get('op') == 'change')
    check('change from HTTP write', n == {'op': 'change', 'path': '/lamp', 'body': {'brightness': 77}}, n)
    http.call('set', '/lamp/label', {}, 'zz')  # filtered out by keys
    http.call('set', '/lamp', {}, {'on': True, 'brightness': 128, 'label': 'desk'})
    n = ws.wait_notification(lambda m: m.get('op') == 'change')
    check('patch notification, filtered', n and n['body'] == {'on': True, 'brightness': 128}, n)
    r = ws.request('unsub', '/lamp')
    check('unsub', r['body'] == 1, r)
    http.call('set', '/lamp/on', {}, False)
    check('nothing after unsub', ws.wait_notification(lambda m: m.get('op') == 'change', 0.5) is None)

    # watch(): the counter changes without anyone calling changed().
    http.call('set', '/demo/running', {}, True)
    r = ws.request('sub', '/demo', keys='counter', interval=300)
    n1 = ws.wait_notification(lambda m: m.get('op') == 'change')
    n2 = ws.wait_notification(lambda m: m.get('op') == 'change')
    t0 = time.time()
    n3 = ws.wait_notification(lambda m: m.get('op') == 'change')
    gap = time.time() - t0
    ok = n1 and n2 and n3 and n2['body']['counter'] > n1['body']['counter']
    check('watch() notifications', ok, (n1, n2, n3))
    check(f'interval respected ({gap * 1000:.0f} ms)', 0.2 <= gap <= 0.6, gap)
    ws.request('unsub', '/demo')

    # changed(): ticks are reported explicitly once per second.
    r = ws.request('sub', '/demo/ticks')
    n = ws.wait_notification(lambda m: m.get('path') == '/demo/ticks', 2.5)
    check('explicit changed()', n and isinstance(n['body'], int) and n['body'] > r['body'], (r, n))
    ws.request('unsub', '/demo/ticks')
    http.call('set', '/demo/running', {}, False)
    ws.notifications.clear()

    # Events, with and without "events": false.
    ws.request('sub', '/demo', keys='fired', events=True)
    http.call('set', '/demo/fire', {}, {'n': 1})
    n = ws.wait_notification(lambda m: m.get('op') == 'event')
    check('event', n == {'op': 'event', 'path': '/demo/fired', 'body': {'n': 1}}, n)
    ws.request('unsub', '/demo')
    ws.request('sub', '/demo', events=False)
    http.call('set', '/demo/fire', {}, 2)
    check('events: false', ws.wait_notification(lambda m: m.get('op') == 'event', 0.5) is None)

    # Subscriptions die with the connection.
    check('subscriptions while connected', subs() == base_subs + 1, subs())
    ws.close()
    time.sleep(0.5)
    check('dropped on disconnect', subs() == base_subs, subs())

    # Several clients at once.
    clients = [WebSocket(host) for _ in range(3)]
    for c in clients:
        c.request('sub', '/lamp/brightness')
    http.call('set', '/lamp/brightness', {}, 5)
    got = [c.wait_notification(lambda m: m.get('op') == 'change') for c in clients]
    check('3 clients notified', all(g and g['body'] == 5 for g in got), got)
    http.call('set', '/lamp/brightness', {}, 128)
    for c in clients:
        c.close()
    time.sleep(0.5)
    check('all dropped', subs() == base_subs, subs())

    # Plain HTTP can't subscribe.
    code, data, _, _ = http.raw('GET', '/lamp')
    print(f'E2E pass={sum(results)} fail={len(results) - sum(results)}')
    sys.exit(0 if all(results) else 1)


if __name__ == '__main__':
    main()
