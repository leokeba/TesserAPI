"""End-to-end test of the HTTP transport against test/hardware firmware.

Usage:
  python3 test/e2e_http.py http://<board-ip>/api

Runs the conformance vectors, then HTTP-specific checks. Standard library
only. Prints "E2E pass=<n> fail=<n>".
"""
import json
import statistics
import sys
import threading

import conformance
from conformance import ABSENT, Http


def main():
    base = sys.argv[1].rstrip('/')
    h = Http(base)
    results = []

    def check(name, cond, detail=''):
        results.append(bool(cond))
        print(('PASS ' if cond else 'FAIL ') + name + ('' if cond else ': ' + str(detail)), flush=True)

    conformance.run(h, 'http', check)

    # Status codes.
    for path, method, body, query, code in [
        ('/nope', 'GET', ABSENT, None, 404),
        ('/lamp/brightness', 'PUT', 999, None, 422),
        ('/sensors/serial', 'PUT', 1, None, 405),
        ('/lamp', 'GET', ABSENT, {'depth': 'abc'}, 400),
        ('/lamp', 'GET', ABSENT, {'view': 'tree'}, 400),
        ('/lamp', 'POST', b'{"on":', None, 400),
    ]:
        got, data, _, _ = h.raw(method, path, query, body)
        check(f'status {method} {path} {query or ""} -> {code}', got == code, (got, data[:120]))

    # PUT and PATCH map to set like POST.
    got, data, _, _ = h.raw('PATCH', '/lamp', None, {'brightness': 9})
    check('PATCH patches', got == 200 and json.loads(data) == {'brightness': 9}, data)
    got, data, _, _ = h.raw('PUT', '/lamp/brightness', None, 128)
    check('PUT sets', got == 200 and json.loads(data) == 128, data)

    # A shape in a GET body (curl/clients) and in ?shape= (browsers).
    got, data, _, _ = h.raw('GET', '/', None, {'lamp': {'brightness': None}})
    check('GET body shape', got == 200 and json.loads(data) == {'lamp': {'brightness': 128}}, data)
    got, data, _, _ = h.raw('GET', '/', {'shape': '{"lamp":{"on":1}}'})
    check('invalid shape value', got == 400, data)
    got, data, _, _ = h.raw('GET', '/', {'shape': '{"lamp":true}'}, {'lamp': True})
    check('shape given twice', got == 400, data)

    # Small responses carry Content-Length; large ones are chunked.
    got, data, headers, _ = h.raw('GET', '/lamp')
    check('small response has Content-Length', 'Content-Length' in headers and
          'chunked' not in headers.get('Transfer-Encoding', ''), headers)
    got, data, headers, _ = h.raw('GET', '/', {'view': 'schema'})
    schema = json.loads(data)
    check('large response is chunked', got == 200 and len(data) > 512 and
          headers.get('Transfer-Encoding') == 'chunked', (len(data), headers))
    check('large response parses', schema['children']['net']['children']['remote'] ==
          {'type': 'action', 'arg': 'any', 'deferred': True}, schema)

    # Outside the API prefix.
    got, data, _, _ = Http(base + 'x').raw('GET', '/')
    check('prefix boundary (/apix)', got == 404, (got, data[:80]))

    # Body limit (Config::maxRequestBody = 4096).
    got, data, _, _ = h.raw('POST', '/lamp/label', None, b'"' + b'x' * 5000 + b'"')
    check('body too large -> 413', got == 413, (got, data[:80]))

    # CORS (enabled in the test firmware).
    got, data, headers, _ = h.raw('OPTIONS', '/lamp')
    check('CORS preflight', got == 204 and headers.get('Access-Control-Allow-Origin') == '*', (got, headers))
    got, data, headers, _ = h.raw('GET', '/lamp')
    check('CORS header on responses', headers.get('Access-Control-Allow-Origin') == '*', headers)

    # Deferred action: the handler parks until the reply arrives.
    got, data, _, ms = h.raw('POST', '/system/later')
    check('deferred reply', got == 200 and json.loads(data) == 'done' and ms >= 180, (got, data, ms))

    # Concurrent clients.
    errors = []

    def worker(i):
        hh = Http(base)
        for j in range(10):
            code, data, _, _ = hh.raw('GET', '/lamp', {'keys': 'brightness'})
            if code != 200 or json.loads(data) != {'brightness': 128}:
                errors.append((i, j, code, data))

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(4)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    check('4 clients x 10 requests', not errors, errors[:3])

    times = [h.raw('GET', '/lamp')[3] for _ in range(20)]
    print(f'     GET /lamp latency: median {statistics.median(times):.1f} ms, max {max(times):.1f} ms')
    got, data, _, _ = h.raw('GET', '/system', {'keys': 'heap,minHeap'})
    print(f'     heap: {data.decode()}')

    print(f'E2E pass={sum(results)} fail={len(results) - sum(results)}')
    sys.exit(0 if all(results) else 1)


if __name__ == '__main__':
    main()
