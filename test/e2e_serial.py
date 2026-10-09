"""End-to-end test of the serial transport against test/hardware firmware.

Usage:
  uv run --with pyserial python test/e2e_serial.py PORT [--reset]

With --reset, resets the board and waits for "SERVING" (the on-target cases
run first, about 5 s). Sends envelopes over the UART and checks the replies;
log lines interleaved on the port are ignored. Prints "E2E pass=<n> fail=<n>".
"""
import json
import sys
import time

import serial

import conformance


class Board:
    def __init__(self, port, reset):
        # Open with DTR/RTS deasserted: asserting them resets boards with an
        # auto-reset circuit, which would restart the firmware on every open.
        self.s = serial.Serial()
        self.s.port = port
        self.s.baudrate = 115200
        self.s.timeout = 0.1
        self.s.dtr = False
        self.s.rts = False
        self.s.open()
        self.buf = b''
        self.next_id = 1
        if reset:
            self.s.dtr = False
            self.s.rts = True
            time.sleep(0.1)
            self.s.rts = False
            self.wait_line(lambda l: l.startswith('SERVING'), 60)
        time.sleep(0.2)
        self.s.reset_input_buffer()
        # Terminate whatever noise the UART picked up during reset.
        self.s.write(b'\n')

    def lines(self, timeout):
        end = time.time() + timeout
        while time.time() < end:
            self.buf += self.s.read(max(1, self.s.in_waiting))
            while b'\n' in self.buf:
                line, self.buf = self.buf.split(b'\n', 1)
                yield line.decode('utf-8', 'replace').rstrip('\r')

    def wait_line(self, pred, timeout):
        for line in self.lines(timeout):
            if pred(line):
                return line
        raise TimeoutError('no matching line')

    def send_raw(self, text):
        self.s.write(text.encode() + b'\n')

    def call(self, op, path, query, body):
        """Conformance transport interface (see conformance.py)."""
        kw = dict(query)
        if body is not conformance.ABSENT:
            kw['body'] = body
        r = self.request(op, path, **kw)
        return r['status'], r.get('body')

    def request(self, op, path, timeout=2.0, **kw):
        rid = self.next_id
        self.next_id += 1
        env = {'id': rid, 'op': op, 'path': path, **kw}
        t0 = time.time()
        self.send_raw(json.dumps(env))
        for line in self.lines(timeout):
            # Log output from other tasks can share the line (UART0 is also
            # the console): parse from the first '{'.
            start = line.find('{"')
            if start < 0:
                continue
            try:
                msg = json.loads(line[start:])
            except ValueError:
                continue
            if msg.get('id') == rid:
                msg['_ms'] = (time.time() - t0) * 1000
                return msg
        return {'status': 'no reply'}


def main():
    port = sys.argv[1]
    b = Board(port, '--reset' in sys.argv)
    results = []

    def check(name, cond, detail=''):
        results.append(cond)
        print(('PASS ' if cond else 'FAIL ') + name + ('' if cond else ': ' + str(detail)), flush=True)

    r = b.request('get', '/lamp')
    check('get lamp', r['status'] == 'ok' and r['body'] == {'on': False, 'brightness': 128, 'label': 'desk'}, r)
    print(f"     round trip {r['_ms']:.1f} ms")

    r = b.request('set', '/lamp/brightness', body=42)
    check('set brightness', r == {**r, 'status': 'ok', 'body': 42}, r)
    r = b.request('get', '/lamp/brightness')
    check('read back', r.get('body') == 42, r)

    r = b.request('set', '/lamp/brightness', body=999)
    check('range rejected', r['status'] == 'invalid_value' and r['body']['path'] == '/lamp/brightness', r)

    r = b.request('set', '/lamp', body={'on': True, 'label': 'shelf'})
    check('patch', r['status'] == 'ok' and r['body'] == {'on': True, 'label': 'shelf'}, r)

    r = b.request('set', '/lamp/toggle')
    check('action', r['status'] == 'ok' and r['body'] is None, r)
    check('action effect', b.request('get', '/lamp/on').get('body') is False)

    r = b.request('get', '/', body={'sensors': {'serial': None}, 'system': {'idf': True}})
    check('shape', r['status'] == 'ok' and r['body']['sensors'] == {'serial': 4242}
          and r['body']['system']['idf'].startswith('v'), r)

    r = b.request('get', '/system', view='schema', depth=1)
    check('schema', r['status'] == 'ok' and r['body']['children']['later'] ==
          {'type': 'action', 'arg': 'any', 'deferred': True}, r)

    r = b.request('set', '/system/later', timeout=3)
    check('deferred reply', r['status'] == 'ok' and r['body'] == 'done' and r['_ms'] >= 180, r)

    r = b.request('get', '/nope')
    check('not found', r['status'] == 'not_found', r)

    b.send_raw('{"id": 99, "op": "get"')  # malformed
    line = b.wait_line(lambda l: '{"status"' in l, 2)
    check('malformed', json.loads(line[line.find('{"status"'):])['status'] == 'bad_request', line)

    # Burst: replies must come back complete and in order.
    for i in range(20):
        b.send_raw(json.dumps({'id': 1000 + i, 'op': 'get', 'path': '/sensors'}))
    ids = []
    for line in b.lines(3):
        start = line.find('{"id":10')
        if start >= 0:
            ids.append(json.loads(line[start:])['id'])
            if len(ids) == 20:
                break
    check('burst of 20', ids == list(range(1000, 1020)), ids)

    r = b.request('set', '/system/stackFree')
    check('uart task stack', r['status'] == 'ok' and r['body'] > 1024, r)
    print(f"     uart task stack free: {r.get('body')} bytes")
    heap = b.request('get', '/system/heap')['body']
    min_heap = b.request('get', '/system/minHeap')['body']
    print(f'     heap free: {heap} bytes, minimum since boot: {min_heap} bytes')

    conformance.run(b, 'serial', check)

    # Subscriptions: the serial line is one client.
    r = b.request('sub', '/lamp', keys='brightness')
    check('sub snapshot', r['status'] == 'ok' and r['body'] == {'brightness': 128}, r)
    b.request('set', '/lamp/brightness', body=12)
    note = None
    for line in b.lines(2):
        start = line.find('{"op":"change"')
        if start >= 0:
            note = json.loads(line[start:])
            break
    check('change notification', note == {'op': 'change', 'path': '/lamp', 'body': {'brightness': 12}}, note)
    r = b.request('unsub', '/lamp')
    check('unsub', r['status'] == 'ok' and r['body'] == 1, r)
    b.request('set', '/lamp/brightness', body=128)

    print(f'E2E pass={sum(results)} fail={len(results) - sum(results)}')
    sys.exit(0 if all(results) else 1)


if __name__ == '__main__':
    main()
