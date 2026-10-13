"""A TesserAPI UDP client (docs/DESIGN.md section 10.7), for the e2e tests.

Speaks the link protocol: fragments, acknowledgements, retransmission,
advertisements. Single-threaded: every wait pumps the socket.
"""
import json
import random
import socket
import struct
import time

HEADER = struct.Struct('<BBBBIIBBHI')  # magic, version|type, flags, index/status, nonce, id, count, -, chunk, length
MAGIC, VERSION = 0x54, 1
DATA, ACK, ADVERT = 0, 1, 2
RELIABLE, ACK_REQUEST, BROADCAST = 0x01, 0x02, 0x04
QUERY, BYE = 0x01, 0x02
COMPLETE, PARTIAL, REJECTED = 0, 1, 2
PORT = 8484


class UdpClient:
    def __init__(self, port=0, fragment=1400, nonce=None):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(('0.0.0.0', port))
        self.fragment = fragment
        self.nonce = random.getrandbits(32) if nonce is None else nonce
        self.next_id = 1
        self.next_req = 1
        self.acks = {}          # (addr, msg id) -> (status, bitmap)
        self.partials = {}      # (addr, nonce, msg id) -> [chunks, count, chunk size, length, flags]
        self.done = set()       # (addr, nonce, msg id) delivered
        self.messages = []      # (addr, envelope dict)
        self.adverts = {}       # addr -> payload dict
        self.byes = []
        self.datagrams_sent = 0

    def close(self):
        self.sock.close()

    # ---- wire ----------------------------------------------------------
    def _send(self, addr, flags, typ, index, msg_id, count=0, chunk=0, length=0, payload=b''):
        head = HEADER.pack(MAGIC, VERSION << 4 | typ, flags, index, self.nonce, msg_id, count, 0, chunk, length)
        self.sock.sendto(head + payload, addr)
        self.datagrams_sent += 1

    def _fragments(self, addr, msg_id, data, flags, mask=None):
        count = max(1, -(-len(data) // self.fragment))
        idx = [i for i in range(count) if mask is None or mask & (1 << i)]
        for i in idx:
            f = flags | (ACK_REQUEST if flags & RELIABLE and i == idx[-1] else 0)
            self._send(addr, f, DATA, i, msg_id, count, self.fragment, len(data),
                       data[i * self.fragment:(i + 1) * self.fragment])
        return count

    def pump(self, timeout=0.05):
        """Waits up to `timeout` seconds for a datagram, then handles every
        one waiting."""
        self.sock.settimeout(max(timeout, 0.0001))
        try:
            pkt, addr = self.sock.recvfrom(2048)
        except socket.timeout:
            return
        self._packet(pkt, addr)
        self.sock.setblocking(False)
        try:
            while True:
                pkt, addr = self.sock.recvfrom(2048)
                self._packet(pkt, addr)
        except BlockingIOError:
            pass

    def _packet(self, pkt, addr):
        if len(pkt) < HEADER.size:
            return
        magic, vt, flags, index, nonce, msg_id, count, _, chunk, length = HEADER.unpack_from(pkt)
        if magic != MAGIC or vt >> 4 != VERSION or nonce == self.nonce:
            return
        typ, payload = vt & 15, pkt[HEADER.size:]
        if typ == ACK:
            bitmap = struct.unpack('<Q', payload[:8])[0] if index == PARTIAL else 0
            self.acks[(addr, msg_id)] = (index, bitmap)
        elif typ == ADVERT:
            if flags & BYE:
                self.byes.append(addr)
            elif payload:
                self.adverts[addr] = json.loads(payload)
        elif typ == DATA:
            key = (addr, nonce, msg_id)
            if key in self.done:
                if flags & RELIABLE:
                    self._send(addr, 0, ACK, COMPLETE, msg_id)
                return
            p = self.partials.setdefault(key, [{}, count, chunk, length, flags])
            p[0][index] = payload
            if len(p[0]) == count:
                del self.partials[key]
                self.done.add(key)
                if flags & RELIABLE:
                    self._send(addr, 0, ACK, COMPLETE, msg_id)
                data = b''.join(p[0][i] for i in range(count))
                self.messages.append((addr, json.loads(data)))
            elif flags & RELIABLE and flags & ACK_REQUEST:
                got = sum(1 << i for i in p[0])
                self._send(addr, 0, ACK, PARTIAL, msg_id, payload=struct.pack('<Q', got))

    # ---- messages ------------------------------------------------------
    def send(self, addr, envelope, reliable=True, broadcast=False, timeout=3.0, copies=2):
        """Sends one envelope; a reliable one waits for its acknowledgement,
        a broadcast goes `copies` times (receivers apply the first)."""
        data = json.dumps(envelope, separators=(',', ':')).encode()
        msg_id = self.next_id
        self.next_id += 1
        flags = (RELIABLE if reliable and not broadcast else 0) | (BROADCAST if broadcast else 0)
        count = self._fragments(addr, msg_id, data, flags)
        if not flags & RELIABLE:
            for _ in range(copies - 1 if broadcast else 0):
                self._fragments(addr, msg_id, data, flags)
            return True
        deadline, backoff = time.time() + timeout, 0.1
        while time.time() < deadline:
            next_try = time.time() + backoff
            while time.time() < next_try:
                self.pump(next_try - time.time())
                ack = self.acks.pop((addr, msg_id), None)
                if ack and ack[0] == COMPLETE:
                    return True
                if ack and ack[0] == REJECTED:
                    return False
                if ack and ack[0] == PARTIAL:
                    missing = ((1 << count) - 1) & ~ack[1]
                    self._fragments(addr, msg_id, data, flags, missing)
            self._fragments(addr, msg_id, data, flags, ((1 << count) - 1))
            backoff = min(backoff * 2, 1.0)
        return False

    def take(self, match, timeout=3.0):
        """The first received envelope for which match(addr, env) holds."""
        deadline = time.time() + timeout
        while True:
            for i, (addr, env) in enumerate(self.messages):
                if match(addr, env):
                    return self.messages.pop(i)[1]
            if time.time() >= deadline:
                return None
            self.pump(min(0.05, deadline - time.time()))

    def request(self, addr, op, path=None, timeout=5.0, **fields):
        """A request; returns the reply envelope (None on timeout)."""
        req_id = self.next_req
        self.next_req += 1
        env = {'id': req_id, 'op': op, **({'path': path} if path is not None else {}), **fields}
        if not self.send(addr, env, timeout=min(timeout, 3.0)):
            return None
        return self.take(lambda a, e: a == addr and e.get('id') == req_id and 'status' in e, timeout)

    def query(self, addr=('255.255.255.255', PORT)):
        """Asks advertising nodes to answer with their advertisement."""
        self._send(addr, QUERY, ADVERT, 0, 0)
        self._send(addr, QUERY, ADVERT, 0, 0)


class Transport:
    """conformance.py's transport interface over a UdpClient."""

    def __init__(self, client, addr):
        self.client, self.addr = client, addr

    def call(self, op, path, query, body):
        from conformance import ABSENT
        fields = dict(query)
        if body is not ABSENT:
            fields['body'] = body
        r = self.client.request(self.addr, op, path, **fields)
        if r is None:
            raise RuntimeError('no reply')
        return r['status'], r.get('body')

    def envelope(self, fields):
        f = dict(fields)
        r = self.client.request(self.addr, f.pop('op'), f.pop('path'), **f)
        if r is None:
            raise RuntimeError('no reply')
        return r['status'], r.get('body')
