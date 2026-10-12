"""Transport conformance: the same requests must give the same results over
every transport. Used by the e2e scripts against test/hardware firmware.

A transport is any object with call(op, path, query, body) -> (status, body),
where status is the transport-neutral name ("ok", "not_found", ...).
"""
import base64
import json
import os
import time
import urllib.error
import urllib.parse
import urllib.request

ABSENT = object()

LAMP = {'on': False, 'brightness': 128, 'label': 'desk'}

# (name, op, path, query, body, expected status, expected body or ABSENT)
VECTORS = [
    ('reset lamp', 'set', '/lamp', {}, LAMP, 'ok', LAMP),
    ('get subtree', 'get', '/lamp', {}, ABSENT, 'ok', LAMP),
    ('get leaf', 'get', '/lamp/brightness', {}, ABSENT, 'ok', 128),
    ('trailing slash', 'get', '/lamp/', {'keys': 'on'}, ABSENT, 'ok', {'on': False}),
    ('keys', 'get', '/lamp', {'keys': 'on,label'}, ABSENT, 'ok', {'on': False, 'label': 'desk'}),
    ('exclude', 'get', '/lamp', {'exclude': 'label'}, ABSENT, 'ok', {'on': False, 'brightness': 128}),
    ('depth', 'get', '/config', {'depth': 1}, ABSENT, 'ok',
     {'gain': 0.25, 'level': 3, 'owner': 'leo', 'version': '1.2.0', 'numbers': {}}),
    ('64-bit integers', 'get', '/config/numbers', {}, ABSENT, 'ok',
     {'big': -9007199254740993, 'huge': 18446744073709551615}),
    ('shape', 'get', '/', {}, {'lamp': {'on': None}, 'sensors': True}, 'ok',
     {'lamp': {'on': False}, 'sensors': {'temperature': 21.5, 'serial': 4242}}),
    ('schema', 'get', '/lamp', {'view': 'schema'}, ABSENT, 'ok',
     {'type': 'object', 'children': {
         'on': {'type': 'boolean', 'writable': True},
         'brightness': {'type': 'integer', 'writable': True, 'min': 0, 'max': 255, 'description': 'PWM duty'},
         'label': {'type': 'string', 'writable': True, 'maxLength': 7},
         'toggle': {'type': 'action'}}}),
    ('set leaf', 'set', '/lamp/brightness', {}, 200, 'ok', 200),
    ('read back', 'get', '/lamp/brightness', {}, ABSENT, 'ok', 200),
    ('range', 'set', '/lamp/brightness', {}, 300, 'invalid_value', ABSENT),
    ('type', 'set', '/lamp/on', {}, 1, 'invalid_value', ABSENT),
    ('string too long', 'set', '/lamp/label', {}, 'x' * 20, 'invalid_value', ABSENT),
    ('uint8 overflow', 'set', '/config/level', {}, 256, 'invalid_value', ABSENT),
    ('patch unknown key', 'set', '/lamp', {}, {'on': True, 'nope': 1}, 'not_found', ABSENT),
    ('patch atomic', 'get', '/lamp/on', {}, ABSENT, 'ok', False),
    ('read-only', 'set', '/sensors/serial', {}, 1, 'read_only', ABSENT),
    ('not found', 'get', '/nope/deeper', {}, ABSENT, 'not_found', ABSENT),
    ('keys and exclude', 'get', '/lamp', {'keys': 'on', 'exclude': 'label'}, ABSENT, 'bad_request', ABSENT),
    ('action', 'set', '/lamp/toggle', {}, ABSENT, 'ok', None),
    ('action effect', 'get', '/lamp/on', {}, ABSENT, 'ok', True),
    ('action value', 'get', '/lamp/toggle', {}, ABSENT, 'not_allowed', ABSENT),
    ('patch', 'set', '/', {}, {'lamp': {'on': False, 'brightness': 128}}, 'ok',
     {'lamp': {'on': False, 'brightness': 128}}),
]


def run(transport, label, report):
    """Runs every vector; report(name, ok, detail) records each result."""
    for name, op, path, query, body, want_status, want_body in VECTORS:
        try:
            status, got = transport.call(op, path, query, body)
        except Exception as e:  # noqa: BLE001 - report and continue
            report(f'{label}: {name}', False, repr(e))
            continue
        ok = status == want_status and (want_body is ABSENT or got == want_body)
        report(f'{label}: {name}', ok, f'got {status} {json.dumps(got)[:200]}')


def envelope_sender(request):
    """send(fields) -> (status, body) for run_files(), from a transport's
    request(op, path, **kw) that returns the reply envelope."""
    def send(fields):
        f = dict(fields)
        r = request(f.pop('op'), f.pop('path'), **f)
        return r.get('status'), r.get('body')
    return send


def run_files(send, label, report, size=12000):
    """File transfers in upload/download envelopes (docs/DESIGN.md section
    10.1.1) against the firmware's /files/blob. send(fields) -> (status, body)
    sends one envelope (its id is the transport's business)."""
    blob = os.urandom(size)
    b64 = lambda b: base64.b64encode(b).decode()  # noqa: E731

    def check(name, ok, detail):
        report(f'{label}: {name}', bool(ok), detail)
        return ok

    t0 = time.time()
    status, body = send({'op': 'upload', 'path': '/files/blob', 'size': size, 'filename': 'x.bin'})
    if not check('upload opens', status == 'ok' and body.get('received') == 0 and body.get('chunk', 0) > 0,
                 (status, body)):
        return
    chunk = body['chunk']
    at = 0
    while at < size:
        part = blob[at:at + chunk]
        status, body = send({'op': 'upload', 'path': '/files/blob', 'offset': at, 'data': b64(part)})
        at += len(part)
        if status != 'ok' or (at < size and body != {'received': at}):
            break
    rate = size / 1024 / (time.time() - t0)
    check(f'upload {size} bytes in {chunk}-byte chunks ({rate:.1f} KB/s)',
          status == 'ok' and body == {'bytes': size}, (at, status, body))

    def download(length=None, again=False):
        got = b''
        first = None
        for _ in range(size // 100 + 10):
            fields = {'op': 'download', 'path': '/files/blob', 'offset': len(got)}
            if length:
                fields['length'] = length
            status, body = send(fields)
            if status != 'ok' or body.get('offset') != len(got):
                return None, first, (status, body)
            first = first or body
            data = base64.b64decode(body['data'])
            if again:
                # The same chunk again, as after a lost reply.
                status, repeat = send(fields)
                if status != 'ok' or base64.b64decode(repeat['data']) != data:
                    return None, first, ('repeat', status, repeat)
            got += data
            if body.get('end'):
                return got, first, None
        return None, first, 'no end'

    t0 = time.time()
    got, first, err = download()
    rate = size / 1024 / (time.time() - t0)
    check(f'download {size} bytes ({rate:.1f} KB/s)', got == blob and first.get('size') == size and
          first.get('filename') == 'blob.bin' and first.get('chunk', 0) > 0, err or (first and {
              k: v for k, v in first.items() if k != 'data'}))
    got, _, err = download(length=1000, again=True)
    check('download in 1000-byte chunks, each asked twice', got == blob, err)
    # The last chunk once more, after the end.
    tail = size % 1000 or 1000
    status, body = send({'op': 'download', 'path': '/files/blob', 'offset': size - tail})
    check('last chunk again after the end', status == 'ok' and body.get('end') is True and
          base64.b64decode(body['data']) == blob[-tail:], (status, body))

    # Uploads: a chunk sent again is acknowledged, a gap refused.
    small = os.urandom(10)
    status, body = send({'op': 'upload', 'path': '/files/blob', 'size': 10, 'data': b64(small[:6])})
    status2, body2 = send({'op': 'upload', 'path': '/files/blob', 'offset': 3, 'data': b64(small[3:6])})
    status3, body3 = send({'op': 'upload', 'path': '/files/blob', 'offset': 8, 'data': b64(small[8:])})
    status4, body4 = send({'op': 'upload', 'path': '/files/blob', 'offset': 6, 'data': b64(small[6:])})
    check('upload: repeated chunk, gap', (status, body, status2, body2, status3, status4, body4) ==
          ('ok', {'received': 6, 'chunk': chunk}, 'ok', {'received': 6}, 'bad_request', 'ok', {'bytes': 10}),
          (status, body, status2, body2, status3, body3, status4, body4))
    got, _, err = download()
    check('upload readable', got == small, err)

    # An upload under the client's name, and a list element's file node. It
    # rewrites notes.txt as the firmware starts it, so the listing other
    # scripts expect is unchanged.
    status, body = send({'op': 'upload', 'path': '/files/upload', 'size': 5, 'filename': 'notes.txt',
                         'data': b64(b'hello')})
    check('upload with a filename', status == 'ok' and body == 'notes.txt', (status, body))
    status, body = send({'op': 'download', 'path': '/files/entries/notes.txt/content'})
    check('download from a list element', status == 'ok' and base64.b64decode(body['data']) == b'hello' and
          body.get('filename') == 'notes.txt', (status, body))

    status, body = send({'op': 'download', 'path': '/lamp/on'})
    check('not a file', status == 'not_allowed', (status, body))
    status, body = send({'op': 'download', 'path': '/files/blob', 'offset': 3})
    check('no download in progress', status == 'bad_request', (status, body))


class Http:
    def __init__(self, base, timeout=10):
        self.base = base.rstrip('/')
        self.timeout = timeout

    def raw(self, method, path, query=None, body=ABSENT, headers=None):
        url = self.base + path
        if query:
            url += '?' + urllib.parse.urlencode(query)
        data = None if body is ABSENT else (body if isinstance(body, bytes) else json.dumps(body).encode())
        req = urllib.request.Request(url, data=data, method=method, headers=headers or {})
        t0 = time.time()
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as r:
                return r.status, r.read(), dict(r.headers), (time.time() - t0) * 1000
        except urllib.error.HTTPError as e:
            return e.code, e.read(), dict(e.headers), (time.time() - t0) * 1000

    def call(self, op, path, query, body):
        q = dict(query)
        if op == 'get':
            if body is not ABSENT:
                q['shape'] = json.dumps(body)
            code, data, _, _ = self.raw('GET', path, q)
        else:
            code, data, _, _ = self.raw('POST', path, q, body)
        parsed = json.loads(data) if data else None
        if code == 200:
            return 'ok', parsed
        return parsed.get('error', str(code)), parsed


class NowtpRelay:
    """Requests to board B relayed by board A's /net/remote action."""

    def __init__(self, via, mac):
        self.via = via  # a transport to board A
        self.mac = mac

    def call(self, op, path, query, body):
        arg = {'mac': self.mac, 'op': op, 'path': path, **query}
        if body is not ABSENT:
            arg['body'] = body
        status, reply = self.via.call('set', '/net/remote', {}, arg)
        if status != 'ok':
            raise RuntimeError(f'relay failed: {status} {reply}')
        return reply['status'], reply['body']

    def envelope(self, fields):
        """Any envelope (upload, download): run_files()'s send()."""
        status, reply = self.via.call('set', '/net/remote', {}, {'mac': self.mac, **fields})
        if status != 'ok':
            raise RuntimeError(f'relay failed: {status} {reply}')
        return reply['status'], reply['body']
