"""End-to-end test of the HTTP transport against test/hardware firmware.

Usage:
  python3 test/e2e_http.py http://<board-ip>/api

Runs the conformance vectors, then HTTP-specific checks. Standard library
only. Prints "E2E pass=<n> fail=<n>".
"""
import http.client
import json
import os
import statistics
import sys
import threading
import time
import urllib.parse

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

    # Access control: writes under /secure need the bearer token.
    got, data, _, _ = h.raw('PUT', '/secure/secret', None, 5)
    check('write without token -> 403', got == 403 and json.loads(data)['error'] == 'unauthorized', (got, data))
    got, data, _, _ = h.raw('PUT', '/secure/secret', None, 5, {'Authorization': 'Bearer wrong'})
    check('wrong token -> 403', got == 403, got)
    got, data, _, _ = h.raw('PUT', '/secure/secret', None, 5, {'Authorization': 'Bearer test-token'})
    check('bearer token -> 200', got == 200 and json.loads(data) == 5, (got, data))
    got, data, _, _ = h.raw('PUT', '/secure/secret', {'token': 'test-token'}, 6)
    check('?token= -> 200', got == 200, got)
    got, data, _, _ = h.raw('POST', '/', None, {'lamp': {'on': True}, 'secure': {'secret': 1}})
    check('patch touching /secure -> 403, nothing applied', got == 403 and
          h.call('get', '/lamp/on', {}, ABSENT) == ('ok', False), (got, data))
    check('reads stay open', h.call('get', '/secure/secret', {}, ABSENT) == ('ok', 6))

    # File nodes: raw bytes both ways, past maxRequestBody.
    blob = os.urandom(20000)
    got, data, _, ms = h.raw('PUT', '/files/blob', None, blob, {'Content-Type': 'application/octet-stream'})
    check(f'file upload ({ms:.0f} ms)', got == 200 and json.loads(data) == {'bytes': 20000}, (got, data[:80]))
    got, data, headers, ms = h.raw('GET', '/files/blob')
    check(f'file download ({ms:.0f} ms)', got == 200 and data == blob and headers.get('Content-Length') == '20000' and
          headers.get('Content-Type') == 'application/octet-stream' and
          'filename="blob.bin"' in headers.get('Content-Disposition', ''), (got, len(data), headers))
    for i in range(3):  # the body is drained, so the client always reads the 413
        got, data, _, _ = h.raw('PUT', '/files/blob', None, os.urandom(40000))
        check(f'file too large -> 413 ({i + 1}/3)', got == 413, (got, data[:80]))
    got, data, _, _ = h.raw('GET', '/files/blob', {'view': 'schema'})
    check('file schema', got == 200 and json.loads(data) == {
        'type': 'file', 'readable': True, 'writable': True, 'maxSize': 32768,
        'contentType': 'application/octet-stream', 'accept': '.bin'}, data)
    r = h.call('get', '/files', {}, ABSENT)
    # Only the directory listing: blob, backup and upload are file nodes.
    check('file left out of values', r[0] == 'ok' and list(r[1]) == ['entries'], r)
    # A directory listing: file nodes in a read-only keyed list's elements,
    # uploads named by ?filename=. Idempotent: the directory lives in RAM.
    got, data, _, _ = h.raw('PUT', '/files/entries/notes.txt/content', None, b'edited')
    check('element file upload', got == 200 and json.loads(data) == 'notes.txt', (got, data))
    got, data, headers, _ = h.raw('GET', '/files/entries/notes.txt/content')
    check('element file download', got == 200 and data == b'edited' and
          headers.get('Content-Disposition') == 'attachment; filename="notes.txt"', (got, data, headers))
    name = 'r\u00e9 "x".txt'
    got, data, _, _ = h.raw('PUT', '/files/upload', {'filename': name}, b'new bytes')
    check('upload with ?filename=', got == 200 and json.loads(data) == name, (got, data))
    got, data, headers, _ = h.raw('GET', '/files/entries/' + urllib.parse.quote(name) + '/content')
    check('download of a non-ASCII name', got == 200 and data == b'new bytes' and
          headers.get('Content-Disposition') ==
          'attachment; filename="r__ _x_.txt"; filename*=UTF-8\'\'r%C3%A9%20%22x%22.txt', (got, data, headers))
    got, data, _, _ = h.raw('PUT', '/files/upload', None, b'x')
    check('upload without a name -> 422', got == 422 and b'bad file name' in data, (got, data))
    r = h.call('get', '/files/entries', {}, ABSENT)
    check('listing updated', r[0] == 'ok' and {'name': name, 'size': 9} in r[1], r)
    # The backup: the persisted state, Admin-only, restored leniently.
    auth = {'Authorization': 'Bearer test-token'}
    got, data, _, _ = h.raw('GET', '/files/backup')
    check('backup needs the token', got == 403, got)
    # Only values that differ from their defaults are in it (DESIGN.md §12).
    name = h.call('get', '/settings/name', {}, ABSENT)[1]
    h.call('set', '/settings/name', {}, 'in-backup')
    got, data, headers, _ = h.raw('GET', '/files/backup', None, ABSENT, auth)
    backup = json.loads(data) if got == 200 else {}
    check('backup download', got == 200 and backup.get('settings', {}).get('name') == 'in-backup' and
          headers.get('Content-Type') == 'application/json', (got, data[:120]))
    got, data, _, _ = h.raw('PUT', '/files/backup', None,
                            json.dumps({'settings': {'name': 'restored', 'gone': 1}}).encode(), auth)
    check('backup restore', got == 200 and json.loads(data) == {'skipped': ['/settings/gone']} and
          h.call('get', '/settings/name', {}, ABSENT)[1] == 'restored', (got, data))
    h.call('set', '/settings/name', {}, name)

    # Deferred action: the handler parks until the reply arrives.
    got, data, _, ms = h.raw('POST', '/system/later')
    check('deferred reply', got == 200 and json.loads(data) == 'done' and ms >= 180, (got, data, ms))

    # Streamed subscriptions: one notification per line on a long GET.
    url = urllib.parse.urlsplit(base)
    subs0 = h.call('get', '/system/subscriptions', {}, ABSENT)[1]

    def stream(query, path='/'):
        c = http.client.HTTPConnection(url.hostname, url.port or 80, timeout=30)
        c.request('GET', url.path + path + '?' + query)
        return c, c.getresponse()

    c1, r1 = stream('sub&interval=60000&changes=false&snapshot=false')
    check('stream opens', r1.status == 200 and r1.getheader('Content-Type') == 'application/x-ndjson' and
          r1.getheader('Transfer-Encoding') == 'chunked' and r1.getheader('Access-Control-Allow-Origin') == '*',
          (r1.status, r1.getheaders()))
    h.call('set', '/demo/fire', {}, 5)
    line = r1.readline()
    check('stream carries events', json.loads(line) == {'op': 'event', 'path': '/demo/fired', 'body': 5}, line)
    c2, r2 = stream('sub=1&snapshot=false')
    c3, r3 = stream('sub&snapshot=false')
    body3 = r3.read()
    check('third stream -> 503 busy', r3.status == 503 and json.loads(body3)['error'] == 'busy', (r3.status, body3))
    c3.close()
    got, data, _, _ = h.raw('GET', '/lamp', {'sub': 'maybe'})
    check('bad sub flag -> 400', got == 400, (got, data))
    got, data, _, _ = h.raw('POST', '/lamp', {'sub': ''}, {'on': False})
    check('sub with POST -> 400', got == 400, (got, data))
    c2.close()
    t0 = time.time()
    line = r1.readline()  # nothing else happens: the keep-alive
    check(f'stream keep-alive ({time.time() - t0:.0f} s)', line == b'\n' and time.time() - t0 < 26, line)
    c1.close()
    time.sleep(0.5)
    check('closed streams drop their subscriptions',
          h.call('get', '/system/subscriptions', {}, ABSENT)[1] == subs0, subs0)
    c5, r5 = stream('sub&snapshot=false', '/nope')
    body5 = r5.read()
    check('refused stream -> ordinary error', r5.status == 404 and r5.getheader('Content-Type') == 'application/json',
          (r5.status, body5))
    c5.close()

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
