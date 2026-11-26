#!/usr/bin/env python3
"""Seed corpora for the fuzz harnesses, from the host tests' fixtures.

    python3 test/fuzz/seeds.py <out-dir>

writes <out-dir>/{json,request,transfer,http}/ from the string literals in
test/*.h (adjacent literals joined, C escapes undone), plus a few
multi-envelope transfers the tests build at run time.
"""
import hashlib
import json
import pathlib
import re
import sys

TEST = pathlib.Path(__file__).resolve().parent.parent
LITERAL = re.compile(r'"((?:\\.|[^"\\\n])*)"')
ESCAPES = {"n": "\n", "r": "\r", "t": "\t", "0": "\0", "\\": "\\", '"': '"', "'": "'"}


def unescape(s):
    out, i = [], 0
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            n = s[i + 1]
            if n == "x":
                m = re.match(r"[0-9a-fA-F]{1,2}", s[i + 2:])
                out.append(chr(int(m.group(0), 16)))
                i += 2 + len(m.group(0))
                continue
            out.append(ESCAPES.get(n, n))
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def literals(text):
    """Each string literal, adjacent ones ("a" "b", across lines) joined."""
    pos, current, end = 0, None, -1
    for m in LITERAL.finditer(text):
        if current is not None and text[end:m.start()].strip() == "":
            current += unescape(m.group(1))
        else:
            if current is not None:
                yield current
            current = unescape(m.group(1))
        end = m.end()
    if current is not None:
        yield current


def is_json(s):
    try:
        json.loads(s)
        return True
    except ValueError:
        return False


def main():
    out = pathlib.Path(sys.argv[1])
    sets = {k: set() for k in ("json", "request", "transfer", "http")}
    for header in sorted(TEST.glob("*.h")):
        for s in literals(header.read_text(encoding="utf-8")):
            if is_json(s):
                sets["json"].add("\x00" + s)  # first byte: nesting 0 → the harness's lowest
                sets["json"].add("\x0a" + s)
            if '"op"' in s:
                kind = "transfer" if re.search(r'"op":"(upload|download)', s) else "request"
                sets[kind].add(s)
            elif s.startswith("/"):
                sets["request"].add(json.dumps({"op": "get", "path": s}))
                sets["request"].add(json.dumps({"op": "set", "path": s, "body": 1}))
            if "%" in s or "://" in s or "filename" in s:
                sets["http"].add(s)
    upload = '{{"id":{i},"op":"upload","path":"/sys/firmware","offset":{o},"data":"{d}"{x}}}'
    sets["transfer"].add("\n".join([
        upload.format(i=1, o=0, d="AAECAw==", x=',"size":8,"filename":"a.bin"'),
        upload.format(i=2, o=4, d="BAUGBw==", x=""),
    ]))
    sets["transfer"].add("\n".join([
        upload.format(i=1, o=0, d="e30=", x=',"size":2'),
    ]).replace("/sys/firmware", "/sys/config"))
    sets["transfer"].add('{"id":1,"op":"download","path":"/sys/coredump","length":3,"crc32":true}\n'
                         '{"id":2,"op":"download","path":"/sys/coredump","offset":3}')
    sets["request"].add('{"op":"auth","token":"user"}\n{"op":"set@user","path":"/lamps/*/on","body":true}')
    sets["request"].add('{"op":"get","path":"/nets/home","depth":1,"keys":"ssid"}')
    sets["http"].update(["a%20b+c%zz%4", "fw%C3%A9.bin\0dev.local:8080\0http://DEV.local:8080",
                         "x\0[fe80::1]:80\0http://[fe80::1]"])
    for kind, seeds in sets.items():
        d = out / kind
        d.mkdir(parents=True, exist_ok=True)
        for s in seeds:
            b = s.encode("utf-8", "surrogatepass")
            (d / hashlib.sha1(b).hexdigest()).write_bytes(b)
        print(f"{kind}: {len(seeds)} seeds")


if __name__ == "__main__":
    main()
