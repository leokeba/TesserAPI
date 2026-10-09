"""Persistence across a reboot, against test/hardware firmware.

Usage:
  uv run --with pyserial python test/e2e_persist.py http://<board-ip>/api <serial-port>

Writes /settings/name over HTTP, waits for the debounced save, resets the
board through the serial port's RTS line, and reads the value back.
Prints "E2E pass=<n> fail=<n>".
"""
import random
import sys
import time

import serial

from conformance import ABSENT, Http


def main():
    h = Http(sys.argv[1], timeout=3)
    port = sys.argv[2]
    results = []

    def check(name, cond, detail=''):
        results.append(bool(cond))
        print(('PASS ' if cond else 'FAIL ') + name + ('' if cond else ': ' + str(detail)), flush=True)

    boots = h.call('get', '/settings/boots', {}, ABSENT)[1]
    name = f'n{random.randint(0, 99999)}'
    check('write', h.call('set', '/settings/name', {}, name) == ('ok', name))
    check('boots is read-only', h.call('set', '/settings/boots', {}, 0)[0] == 'read_only')
    time.sleep(1.5)  # 500 ms debounce + poll period

    s = serial.Serial(port, 115200)
    s.dtr = False
    s.rts = True
    time.sleep(0.1)
    s.rts = False
    s.close()

    end = time.time() + 30
    got = None
    while time.time() < end:
        try:
            got = h.call('get', '/settings', {}, ABSENT)
            break
        except OSError:
            time.sleep(0.5)
    check('back after reboot', got is not None, got)
    if got:
        status, settings = got
        check('name survived the reboot', settings['name'] == name, settings)
        check('boot counter advanced', settings['boots'] > boots, (boots, settings))

    print(f'E2E pass={sum(results)} fail={len(results) - sum(results)}')
    sys.exit(0 if all(results) else 1)


if __name__ == '__main__':
    main()
