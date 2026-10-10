"""Two-board gateway test: board A mounts board B's tree as /peer and serves
it over HTTP and WebSocket, forwarding over ESP-NOW (test/hardware firmware
on both, same Wi-Fi network).

Usage:
  python3 test/e2e_gateway.py <ip-A> <ip-B>

Runs the conformance vectors through A's /peer prefix, then checks error
paths, the mirror, change notifications and events relayed to a WebSocket
client on A, B's discovery mount under /peers, and subscriptions forwarded
through it. Prints "E2E pass=<n> fail=<n>".
"""
import json
import statistics
import sys
import time

import conformance
from conformance import ABSENT, Http
from e2e_ws import WebSocket


class Prefixed:
    """A transport whose paths all live under a prefix."""

    def __init__(self, transport, prefix):
        self.t = transport
        self.prefix = prefix

    def call(self, op, path, query, body):
        return self.t.call(op, self.prefix + path, query, body)


def main():
    ip_a, ip_b = sys.argv[1], sys.argv[2]
    a, b = Http(f'http://{ip_a}/api', timeout=15), Http(f'http://{ip_b}/api')
    results = []

    def check(name, cond, detail=''):
        results.append(bool(cond))
        print(('PASS ' if cond else 'FAIL ') + name + ('' if cond else ': ' + str(detail)), flush=True)

    mac_b = b.call('get', '/net/mac', {}, ABSENT)[1]
    status, body = a.call('set', '/net/mount', {}, {'mac': mac_b, 'mirror': True, 'interval': 100})
    check('mount B as /peer', status == 'ok' or (body or {}).get('message') == 'already mounted', (status, body))

    # Mounting changed A's schema: A re-advertises its new hash on its own,
    # and B sees it in A's discovery metadata.
    mac_a = a.call('get', '/net/mac', {}, ABSENT)[1]
    hash_a = json.loads(a.raw('GET', '/', {'view': 'hash'}, ABSENT, {'Authorization': 'Bearer test-token'})[1])
    seen = None
    for _ in range(30):  # 1 s debounce, then the next announcement
        peers = b.call('get', '/net/peers', {}, ABSENT)[1] or []
        seen = next((p.get('tesser', {}).get('schema') for p in peers if p['mac'] == mac_a), None)
        if seen == hash_a:
            break
        time.sleep(0.5)
    check('A re-advertised after the mount', seen == hash_a, (seen, hash_a))

    conformance.run(Prefixed(a, '/peer'), 'gateway', check)

    # Error paths come back under the gateway's path.
    code, data, _, _ = a.raw('GET', '/peer/nope/x')
    check('remote 404 mapped', code == 404 and json.loads(data)['path'] == '/peer/nope', (code, data))
    code, data, _, _ = a.raw('PUT', '/peer/lamp/brightness', None, 999)
    check('remote 422 mapped', code == 422 and json.loads(data)['path'] == '/peer/lamp/brightness', (code, data))

    # Writes through the gateway land on B.
    a.call('set', '/peer/lamp/label', {}, 'gw')
    check('write lands on B', b.call('get', '/lamp/label', {}, ABSENT) == ('ok', 'gw'))
    a.call('set', '/peer/lamp/label', {}, 'desk')

    # The schema shows a mirrored remote node; its own schema is B's.
    _, schema = a.call('get', '/', {'view': 'schema', 'depth': 1}, ABSENT)
    stub = dict(schema['children']['peer'])
    advertised = stub.pop('schema', '')
    check('schema: remote', stub == {'type': 'remote', 'mirror': True} and len(advertised) == 8, schema)
    _, remote_schema = a.call('get', '/peer/lamp/brightness', {'view': 'schema'}, ABSENT)
    check('schema: forwarded', remote_schema.get('max') == 255, remote_schema)

    # The mirror: B's state appears in A's own tree.
    b.call('set', '/lamp/brightness', {}, 33)
    time.sleep(0.6)
    _, top = a.call('get', '/', {'keys': 'peer'}, ABSENT)
    check('mirror in parent read', (top.get('peer') or {}).get('lamp', {}).get('brightness') == 33, top)

    # Subscribers on A get B's changes and events.
    ws = WebSocket(ip_a)
    r = ws.request('sub', '/', keys='peer', interval=0)
    check('sub on gateway', r['status'] == 'ok', r)
    t0 = time.time()
    b.call('set', '/lamp/brightness', {}, 77)
    n = ws.wait_notification(lambda m: m.get('op') == 'change', 3)
    latency = (time.time() - t0) * 1000
    check('change relayed', n is not None and n['body']['peer']['lamp']['brightness'] == 77, n)
    print(f'     B write -> WebSocket on A: {latency:.0f} ms (B HTTP call included)')
    b.call('set', '/demo/fire', {}, 'ping')
    n = ws.wait_notification(lambda m: m.get('op') == 'event', 3)
    check('event relayed', n == {'op': 'event', 'path': '/peer/demo/fired', 'body': 'ping'}, n)
    ws.close()
    b.call('set', '/lamp/brightness', {}, 128)

    # Discovery: A mounted B under /peers, named after its NowTP name.
    name_b = 'tesser-' + mac_b.replace(':', '')[-4:]
    stub, peers = None, None
    for _ in range(20):
        _, peers = a.call('get', '/peers', {'view': 'schema', 'depth': 1}, ABSENT)
        stub = (peers.get('children') or {}).get(name_b)
        if stub:
            break
        time.sleep(0.5)
    check('B discovered under /peers', stub == {'type': 'remote', 'schema': advertised}, peers)
    base = f'/peers/{name_b}'
    check('read through discovered B', a.call('get', base + '/lamp/label', {}, ABSENT) == ('ok', 'desk'))

    # Forwarded subscriptions: two WebSocket clients on A share one upstream
    # subscription to B, each with its own filter.
    ws1, ws2 = WebSocket(ip_a), WebSocket(ip_a)
    t0 = time.time()
    r1 = ws1.request('sub', base + '/lamp', keys='brightness', timeout=5)
    first_ms = (time.time() - t0) * 1000
    check('forwarded sub snapshot', r1.get('status') == 'ok' and r1.get('body') == {'brightness': 128}, r1)
    t0 = time.time()
    r2 = ws2.request('sub', base + '/lamp')
    shared_ms = (time.time() - t0) * 1000
    check('shared sub snapshot', r2.get('status') == 'ok' and r2['body'].get('label') == 'desk', r2)
    print(f'     forwarded sub: first {first_ms:.0f} ms (upstream), second {shared_ms:.0f} ms (from the copy)')
    b.call('set', '/lamp/brightness', {}, 55)
    n1 = ws1.wait_notification(lambda m: m.get('op') == 'change', 3)
    n2 = ws2.wait_notification(lambda m: m.get('op') == 'change', 3)
    check('forwarded change, filtered', n1 == {'op': 'change', 'path': base + '/lamp', 'body': {'brightness': 55}}, n1)
    check('forwarded change, shared', n2 == {'op': 'change', 'path': base + '/lamp', 'body': {'brightness': 55}}, n2)
    b.call('set', '/lamp/label', {}, 'fwd')
    n1 = ws1.wait_notification(lambda m: m.get('op') == 'change', 1)
    n2 = ws2.wait_notification(lambda m: m.get('op') == 'change', 3)
    check('keys filter applied', n1 is None, n1)
    check('other keys still relayed', n2 is not None and n2['body'] == {'label': 'fwd'}, n2)
    r = ws1.request('unsub', base + '/lamp')
    check('forwarded unsub', r.get('status') == 'ok' and r.get('body') == 1, r)
    ws1.close()
    ws2.close()
    b.call('set', '/lamp/label', {}, 'desk')
    b.call('set', '/lamp/brightness', {}, 128)

    # Forwarded GET latency, end to end over HTTP + ESP-NOW.
    times = [a.raw('GET', '/peer/lamp')[3] for _ in range(10)]
    print(f'     GET /peer/lamp via gateway: median {statistics.median(times):.0f} ms')

    print(f'E2E pass={sum(results)} fail={len(results) - sum(results)}')
    sys.exit(0 if all(results) else 1)


if __name__ == '__main__':
    main()
