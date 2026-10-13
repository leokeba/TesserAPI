"""Two-board end-to-end test of routes and streamed sets (docs/DESIGN.md
section 14.3), with the test/hardware firmware on both boards, on the same
LAN as this machine and the same ESP-NOW channel.

Usage:
  python3 test/e2e_routes.py <ip-A> <ip-B>

Each board mounts the other under /both over UDP and NowTP: one node with a
route each, UDP first. A streams /demo/seq to B (/net/routes/stream) while B's
UDP transport is switched off and on again: A must fail over to NowTP and come
back, and B must never apply an older value after a newer one. Prints
"E2E pass=<n> fail=<n>".
"""
import json
import sys
import time

from conformance import ABSENT, Http


def main():
    ip_a, ip_b = sys.argv[1], sys.argv[2]
    a, b = Http(f'http://{ip_a}/api'), Http(f'http://{ip_b}/api')
    results = []

    def check(name, cond, detail=''):
        results.append(bool(cond))
        print(('PASS ' if cond else 'FAIL ') + name + ('' if cond else ': ' + str(detail)), flush=True)

    get = lambda board, path: board.call('get', path, {}, ABSENT)[1]  # noqa: E731

    def state(board, node):
        return json.loads(get(board, '/net/routes/state')).get(node, {})

    def wait(cond, seconds):
        deadline = time.time() + seconds
        while time.time() < deadline:
            if cond():
                return True
            time.sleep(0.1)
        return cond()

    for board in (a, b):
        board.call('set', '/net/udp/enabled', {}, True)
    mac_a, mac_b = get(a, '/net/mac').replace(':', ''), get(b, '/net/mac').replace(':', '')
    name_a, name_b = 'tesser-' + mac_a[-4:], 'tesser-' + mac_b[-4:]

    # ---- one node per board, a route per transport -----------------------
    check('A has B over both routes', wait(lambda: state(a, name_b).get('routes') == 2, 12), state(a, name_b))
    check('B has A over both routes', wait(lambda: state(b, name_a).get('routes') == 2, 12), state(b, name_a))
    both = json.loads(a.raw('GET', '/both', {'view': 'schema', 'depth': 1})[1])['children']
    stub = both.get(name_b, {})
    check('stub: route and device', stub.get('route') == 'udp' and stub.get('id') == mac_b, stub)
    check('A uses UDP to B', wait(lambda: state(a, name_b).get('route') == 'udp', 6), state(a, name_b))

    # ---- a stream over UDP -----------------------------------------------
    _, r = a.call('set', '/net/routes/stream', {}, {'count': 200, 'node': name_b})
    check('stream accepted', r.get('refused') == 0, r)
    check('stream lands over UDP', wait(lambda: get(b, '/demo/seq') == r['last'], 3), (get(b, '/demo/seq'), r))
    # Counted from here: a board A that restarted counts from 1 again.
    backwards0 = get(b, '/demo/seqBackwards')
    st = state(a, name_b)
    check('queued sets merged', st.get('merged', 0) > 0 and st.get('failed') == 0, st)
    print(f"     UDP: rtt {st.get('rttMs')} ms, {st.get('sent')} sets sent, {st.get('merged')} merged")

    # ---- B leaves the LAN while A streams: NowTP takes over ---------------
    last, routes_seen, while_off = 0, [], set()
    t0 = time.time()
    for i in range(120):
        if i == 30:
            b.call('set', '/net/udp/enabled', {}, False)  # a goodbye: failover at once
        if i == 80:
            b.call('set', '/net/udp/enabled', {}, True)  # a query and an advertisement: back
        _, r = a.call('set', '/net/routes/stream', {}, {'count': 3, 'node': name_b})
        last = r['last']
        if r.get('refused'):
            print(f'     refused at {i}: {r}')
        route = state(a, name_b).get('route')
        if 32 <= i < 80:
            while_off.add(route)
        if not routes_seen or routes_seen[-1] != route:
            routes_seen.append(route)
            udp = get(a, '/net/udp/stats')
            print(f'     {time.time() - t0:5.1f} s (step {i}): {route}; A udp sent {udp["sent"]} '
                  f'failed {udp["failed"]} retransmits {udp["retransmits"]}', flush=True)
        time.sleep(0.03)
    elapsed = time.time() - t0
    check('stream lands across both switches', wait(lambda: get(b, '/demo/seq') == last, 5),
          (get(b, '/demo/seq'), last))
    check('never an older value after a newer one', get(b, '/demo/seqBackwards') == backwards0,
          get(b, '/demo/seqBackwards'))
    # A lossy Wi-Fi may fail a UDP delivery (3 s without an acknowledgement)
    # while B's UDP is on: that switches to NowTP too, until B's next
    # advertisement. But with B's UDP off, A must stay on NowTP.
    check('NowTP while B is off the LAN', while_off == {'nowtp'}, while_off)
    check('UDP again at the end', wait(lambda: state(a, name_b).get('route') == 'udp', 8), state(a, name_b))
    st = state(a, name_b)
    check('nothing failed', st.get('failed') == 0 and st.get('online'), st)
    print(f'     {120 * 3} sets in {elapsed:.1f} s through the switches; routes seen: {routes_seen}')

    # ---- forwarded requests follow the route too ---------------------------
    b.call('set', '/net/udp/enabled', {}, False)
    wait(lambda: state(a, name_b).get('route') == 'nowtp', 3)
    code, data, _, _ = a.raw('GET', f'/both/{name_b}/demo/seq')
    check('a forwarded get over NowTP', code == 200 and json.loads(data) == last, (code, data))
    b.call('set', '/net/udp/enabled', {}, True)
    check('back to UDP', wait(lambda: state(a, name_b).get('route') == 'udp', 8), state(a, name_b))

    passed = sum(results)
    print(f'E2E pass={passed} fail={len(results) - passed}')
    return 0 if passed == len(results) else 1


if __name__ == '__main__':
    sys.exit(main())
