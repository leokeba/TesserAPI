"""Two-board end-to-end test of the NowTP transport (test/hardware firmware on
both boards, same Wi-Fi network so they share a channel).

Usage:
  python3 test/e2e_nowtp.py http://<ip-A>/api http://<ip-B>/api

Board A relays requests to board B over ESP-NOW through its /net/remote
action; board B's HTTP API is used to check the effects independently. Runs
the conformance vectors in both directions. Prints "E2E pass=<n> fail=<n>".
"""
import json
import statistics
import sys
import time

import conformance
from conformance import Http, NowtpRelay


def main():
    a, b = Http(sys.argv[1]), Http(sys.argv[2])
    results = []

    def check(name, cond, detail=''):
        results.append(bool(cond))
        print(('PASS ' if cond else 'FAIL ') + name + ('' if cond else ': ' + str(detail)), flush=True)

    mac_a = a.call('get', '/net/mac', {}, conformance.ABSENT)[1]
    mac_b = b.call('get', '/net/mac', {}, conformance.ABSENT)[1]
    print(f'     A={mac_a} B={mac_b}')
    peers = a.call('get', '/net/peers', {}, conformance.ABSENT)[1]
    check('A discovered B', any(p['mac'] == mac_b for p in peers), peers)

    a_to_b = NowtpRelay(a, mac_b)
    b_to_a = NowtpRelay(b, mac_a)
    conformance.run(a_to_b, 'nowtp A->B', check)
    conformance.run(b_to_a, 'nowtp B->A', check)

    # Effects are real: a write over ESP-NOW shows up in B's own HTTP API.
    a_to_b.call('set', '/lamp/label', {}, 'radio')
    check('write visible on B', b.call('get', '/lamp/label', {}, conformance.ABSENT) == ('ok', 'radio'))
    a_to_b.call('set', '/lamp/label', {}, 'desk')

    # A response spanning many ESP-NOW frames.
    status, schema = a_to_b.call('get', '/', {'view': 'schema'}, conformance.ABSENT)
    size = len(json.dumps(schema, separators=(',', ':')))
    check(f'fragmented response ({size} bytes)', status == 'ok' and size > 1000 and
          schema['children']['net']['children']['remote']['type'] == 'action', (status, size))

    # Unknown peer: the client call times out.
    _, reply = a.call('set', '/net/remote', {}, {'mac': '02:00:00:00:00:01', 'op': 'get', 'path': '/',
                                                 'timeoutMs': 600})
    check('unknown peer times out', reply['status'] == 'timeout' and 550 <= reply['ms'] <= 900, reply)

    # Latency as measured on board A (request out, response in).
    times = []
    for _ in range(20):
        _, reply = a.call('set', '/net/remote', {}, {'mac': mac_b, 'op': 'get', 'path': '/lamp'})
        times.append(reply['ms'])
    check('20 round trips ok', len(times) == 20)
    print(f'     NowTP round trip A->B->A: median {statistics.median(times)} ms, max {max(times)} ms')

    # Subscriptions over NowTP: A subscribes to B; writes on B (over HTTP)
    # reach A as change notifications, events too.
    def notifications():
        return a.call('get', '/net/notifications', {}, conformance.ABSENT)[1]

    def wait_count(n, timeout=3):
        end = time.time() + timeout
        while time.time() < end:
            got = notifications()
            if got['count'] >= n:
                return got
            time.sleep(0.05)
        return notifications()

    subs_b = b.call('get', '/system/subscriptions', {}, conformance.ABSENT)[1]
    start = notifications()['count']
    status, snap = a_to_b.call('sub', '/lamp', {'keys': 'brightness'}, conformance.ABSENT)
    check('sub over NowTP', status == 'ok' and snap == {'brightness': 128}, (status, snap))
    check('B holds the subscription',
          b.call('get', '/system/subscriptions', {}, conformance.ABSENT)[1] == subs_b + 1)
    t0 = time.time()
    b.call('set', '/lamp/brightness', {}, 66)
    got = wait_count(start + 1)
    latency = (time.time() - t0) * 1000
    check('change reaches A', got['last'] == {'op': 'change', 'path': '/lamp', 'body': {'brightness': 66}}, got)
    print(f'     B write (HTTP) -> notification on A: {latency:.0f} ms, including both HTTP calls')
    b.call('set', '/lamp/brightness', {}, 128)
    wait_count(start + 2)
    status, removed = a_to_b.call('unsub', '/lamp', {}, conformance.ABSENT)
    check('unsub over NowTP', status == 'ok' and removed == 1, (status, removed))

    a_to_b.call('sub', '/demo', {'keys': 'fired'}, conformance.ABSENT)
    before = notifications()['count']
    b.call('set', '/demo/fire', {}, 'hello')
    got = wait_count(before + 1)
    check('event reaches A', got['last'] == {'op': 'event', 'path': '/demo/fired', 'body': 'hello'}, got)
    a_to_b.call('unsub', '/demo', {}, conformance.ABSENT)
    check('B back to its subscriptions',
          b.call('get', '/system/subscriptions', {}, conformance.ABSENT)[1] == subs_b)

    # B trusts no NowTP peer: writes to /secure are refused, reads are fine.
    status, body = a_to_b.call('set', '/secure/secret', {}, 9)
    check('untrusted peer -> unauthorized', status == 'unauthorized', (status, body))
    status, body = a_to_b.call('get', '/secure/secret', {}, conformance.ABSENT)
    check('untrusted peer can read', status == 'ok', (status, body))

    stats = b.call('get', '/net/endpoint', {}, conformance.ABSENT)[1]
    print(f'     B endpoint: {stats}')
    check('B dropped nothing', stats['dropped'] == 0, stats)

    print(f'E2E pass={sum(results)} fail={len(results) - sum(results)}')
    sys.exit(0 if all(results) else 1)


if __name__ == '__main__':
    main()
