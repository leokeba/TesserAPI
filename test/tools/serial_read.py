"""Reads several serial ports at once, prefixing lines with labels.

Usage:
  uv run --with pyserial python test/tools/serial_read.py SECONDS [--reset] [--until REGEX] PORT:LABEL...

--reset pulses RTS with DTR low (works for CP2102/CH9102 auto-reset and
native USB). --until stops reading a port once a line matches. Reopens ports
on USB CDC errors.
"""
import re
import sys
import threading
import time

import serial


def main():
    args = sys.argv[1:]
    secs = float(args.pop(0))
    reset = '--reset' in args
    until = None
    if '--until' in args:
        i = args.index('--until')
        until = re.compile(args[i + 1])
        del args[i:i + 2]
    ports = [a.split(':', 1) for a in args if not a.startswith('--')]
    done = {}
    lock = threading.Lock()

    def run(port, label):
        s = serial.Serial(port, 115200, timeout=0.2)
        if reset:
            s.dtr = False
            s.rts = True
            time.sleep(0.1)
            s.rts = False
        end = time.time() + secs
        buf = b''
        while time.time() < end and not done.get(label):
            try:
                buf += s.read(4096)
            except Exception:
                try:
                    s.close()
                except Exception:
                    pass
                time.sleep(0.5)
                try:
                    s = serial.Serial(port, 115200, timeout=0.2)
                except Exception:
                    time.sleep(0.5)
                continue
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                text = line.decode('utf-8', 'replace').rstrip('\r')
                with lock:
                    print(f'[{label}] {text}', flush=True)
                if until and until.search(text):
                    done[label] = True

    threads = [threading.Thread(target=run, args=p) for p in ports]
    for t in threads:
        t.start()
    for t in threads:
        t.join()


if __name__ == '__main__':
    main()
