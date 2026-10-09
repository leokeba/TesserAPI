"""Transport conformance: the same requests must give the same results over
every transport. Used by the e2e scripts against test/hardware firmware.

A transport is any object with call(op, path, query, body) -> (status, body),
where status is the transport-neutral name ("ok", "not_found", ...).
"""
import json
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
