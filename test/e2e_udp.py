"""Two-board end-to-end test of the UDP transport (test/hardware firmware on
both boards, on the same LAN as this machine).

Usage:
  python3 test/e2e_udp.py <ip-A> <ip-B> [broadcast address, default a.b.c.255]

Talks to both boards over UDP directly (test/udp_client.py) and through
their HTTP APIs: discovery, the conformance vectors and file transfers over
UDP, subscriptions, tokens and the peer level, broadcast writes, a gateway
forwarding over UDP, and switching the transport off and on at run time.
Prints "E2E pass=<n> fail=<n>".
"""
import json
import os
import statistics
import sys
import time

import conformance
from conformance import ABSENT, Http
from udp_client import PORT, Transport, UdpClient

TOKEN = {'Authorization': 'Bearer test-token'}


def main():
    ip_a, ip_b = sys.argv[1], sys.argv[2]
    bcast = sys.argv[3] if len(sys.argv) > 3 else ip_a.rsplit('.', 1)[0] + '.255'
    a, b = Http(f'http://{ip_a}/api'), Http(f'http://{ip_b}/api')
    A, B = (ip_a, PORT), (ip_b, PORT)
    results = []

    def check(name, cond, detail=''):
        results.append(bool(cond))
        print(('PASS ' if cond else 'FAIL ') + name + ('' if cond else ': ' + str(detail)), flush=True)

    get = lambda board, path: board.call('get', path, {}, ABSENT)[1]  # noqa: E731
    for board in (a, b):
        board.call('set', '/net/udp/enabled', {}, True)
        board.call('set', '/net/udp/peerAccess', {}, 'public')
    client = UdpClient()

    # ---- discovery ------------------------------------------------------
    client.query((bcast, PORT))
    deadline = time.time() + 3
    while time.time() < deadline and not (A in client.adverts and B in client.adverts):
        client.pump(0.1)
    for label, board, addr in (('A', a, A), ('B', b, B)):
        ad = client.adverts.get(addr)
        mac = get(board, '/net/mac').replace(':', '')
        hash_ = json.loads(board.raw('GET', '/', {'view': 'hash'}, ABSENT, TOKEN)[1])
        check(f'{label} answers a query with its advertisement',
              ad and ad.get('id') == mac and ad.get('name') == 'tesser-' + mac[-4:] and ad.get('schema') == hash_,
              (ad, mac, hash_))
    name_a, name_b = 'tesser-' + get(a, '/net/mac').replace(':', '')[-4:], \
        'tesser-' + get(b, '/net/mac').replace(':', '')[-4:]
    lan_a = json.loads(a.raw('GET', '/lan', {'view': 'schema', 'depth': 1})[1])['children']
    lan_b = json.loads(b.raw('GET', '/lan', {'view': 'schema', 'depth': 1})[1])['children']
    check('A mounted B under /lan', name_b in lan_a and lan_a[name_b].get('online', True), lan_a)
    check('B mounted A under /lan', name_a in lan_b and lan_b[name_a].get('online', True), lan_b)

    # ---- requests -------------------------------------------------------
    conformance.run(Transport(client, A), 'udp A', check)
    conformance.run(Transport(client, B), 'udp B', check)
    conformance.run_files(Transport(client, A).envelope, 'udp A files', check, size=20000)
    # A reply of many datagrams: the whole schema, as HTTP gives it.
    r = client.request(A, 'get', '/', view='schema')
    http_schema = json.loads(a.raw('GET', '/', {'view': 'schema'})[1])
    size = len(json.dumps(http_schema))
    check(f'schema over UDP ({size} bytes) equals HTTP\'s', r and r['status'] == 'ok' and r['body'] == http_schema,
          r and r['status'])
    times = []
    for _ in range(30):
        t0 = time.time()
        r = client.request(A, 'get', '/lamp/on')
        times.append((time.time() - t0) * 1000)
    check(f'GET over UDP: median {statistics.median(times):.1f} ms, max {max(times):.1f} ms',
          r and r['status'] == 'ok')

    # ---- subscriptions --------------------------------------------------
    r = client.request(B, 'sub', '/lamp', keys='brightness')
    check('subscribe over UDP', r and r['status'] == 'ok' and r['body'] == {'brightness': 128}, r)
    b.call('set', '/lamp/brightness', {}, 99)
    note = client.take(lambda addr, e: addr == B and e.get('op') == 'change')
    check('change notification over UDP', note == {'op': 'change', 'path': '/lamp', 'body': {'brightness': 99}}, note)
    r = client.request(B, 'unsub', '/lamp')
    check('unsubscribe', r and r['status'] == 'ok' and r['body'] == 1, r)
    b.call('set', '/lamp/brightness', {}, 128)

    # ---- access ---------------------------------------------------------
    secret = get(a, '/secure/secret')
    r = client.request(A, 'set', '/secure/secret', body=secret)
    check('a public peer can\'t write /secure', r and r['status'] == 'unauthorized', r)
    r = client.request(A, 'auth', token='test-token')
    check('auth with the token: admin', r and r['body'] == {'access': 'admin'}, r)
    r = client.request(A, 'set', '/secure/secret', body=secret)
    check('an admin peer writes /secure', r and r['status'] == 'ok', r)
    r = client.request(A, 'auth', token='')
    check('signing out: public', r and r['body'] == {'access': 'public'}, r)
    a.call('set', '/net/udp/peerAccess', {}, 'user')
    r = client.request(A, 'set', '/secure/secret', body=secret)
    check('peerAccess user: authenticated without a token', r and r['status'] == 'ok', r)
    a.call('set', '/net/udp/peerAccess', {}, 'public')

    # ---- broadcast writes -----------------------------------------------
    client.messages.clear()
    # Three copies: broadcasts from this machine to the AP are lost in bursts.
    client.send((bcast, PORT), {'op': 'set', 'path': '/lamp/label', 'body': 'bcast'}, broadcast=True, copies=3)
    end = time.time() + 0.5
    while time.time() < end:
        client.pump(end - time.time())
    replies = [e for _, e in client.messages if 'status' in e]
    check('a broadcast set from this machine reaches both boards',
          get(a, '/lamp/label') == 'bcast' and get(b, '/lamp/label') == 'bcast',
          (get(a, '/lamp/label'), get(b, '/lamp/label')))
    check('nobody answers it', not replies, replies)
    status, r = a.call('set', '/net/udp/broadcast', {}, {'path': '/lamp/label', 'value': 'fromA'})
    time.sleep(0.3)
    check('A\'s broadcast set lands on B, not on A',
          status == 'ok' and get(b, '/lamp/label') == 'fromA' and get(a, '/lamp/label') == 'bcast',
          (status, r, get(a, '/lamp/label'), get(b, '/lamp/label')))
    status, r = a.call('set', '/net/udp/broadcast', {}, {'path': '/lamp/label', 'value': 'desk', 'to': bcast,
                                                         'replies': True})
    check('replies collected from a directed broadcast', status == 'ok' and r == [{'from': f'{ip_b}:{PORT}',
                                                                                   'status': 'ok'}], (status, r))
    a.call('set', '/lamp/label', {}, 'desk')
    check('labels restored', get(a, '/lamp/label') == 'desk' and get(b, '/lamp/label') == 'desk')

    # ---- gateway --------------------------------------------------------
    via = Http(f'http://{ip_a}/api/lan/{name_b}')
    conformance.run(via, 'gateway A->B over UDP', check)
    remote, direct = [], []
    for _ in range(15):
        remote.append(via.raw('GET', '/lamp/on')[3])
        direct.append(a.raw('GET', '/lamp/on')[3])
    check(f'GET through the gateway: median {statistics.median(remote):.0f} ms '
          f'(A itself {statistics.median(direct):.0f} ms)', True)
    blob = os.urandom(30000)
    t0 = time.time()
    code, data, _, _ = via.raw('PUT', '/files/blob', None, blob, TOKEN)
    up = len(blob) / 1024 / (time.time() - t0)
    check(f'relayed HTTP upload over UDP ({up:.1f} KB/s)', code == 200 and json.loads(data) == {'bytes': len(blob)},
          (code, data))
    t0 = time.time()
    code, data, _, _ = via.raw('GET', '/files/blob', None, ABSENT, TOKEN)
    down = len(blob) / 1024 / (time.time() - t0)
    check(f'relayed HTTP download over UDP ({down:.1f} KB/s)', code == 200 and data == blob, (code, len(data)))

    # ---- on and off at run time ------------------------------------------
    def online_on_a():
        s = json.loads(a.raw('GET', '/lan', {'view': 'schema', 'depth': 1, 'keys': name_b})[1])
        return s['children'][name_b].get('online', True)

    b.call('set', '/net/udp/enabled', {}, False)
    time.sleep(0.5)
    check('B switched off: offline on A at once (goodbye)', not online_on_a() and get(b, '/net/udp/enabled') is False)
    b.call('set', '/net/udp/enabled', {}, True)
    time.sleep(0.5)
    check('B switched on: online on A again', online_on_a())
    # Switched off by a request over UDP itself; its reply may not get out.
    client.request(B, 'set', '/net/udp/enabled', timeout=1.5, body=False)
    time.sleep(0.3)
    check('switched off over UDP', get(b, '/net/udp/enabled') is False)
    b.call('set', '/net/udp/enabled', {}, True)
    r = client.request(B, 'get', '/lamp/on')
    check('back on: answers over UDP', r and r['status'] == 'ok', r)
    stats_a, stats_b = get(a, '/net/udp/stats'), get(b, '/net/udp/stats')
    print(f'     A {stats_a}\n     B {stats_b}')
    client.close()

    passed = sum(results)
    print(f'E2E pass={passed} fail={len(results) - passed}')
    sys.exit(0 if passed == len(results) else 1)


if __name__ == '__main__':
    main()
