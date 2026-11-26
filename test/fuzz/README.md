# Fuzz harnesses

libFuzzer harnesses, under ASan and UBSan, for what a remote client controls:

| Harness | Input | Checks |
|---|---|---|
| `fuzz_json` | first byte: nesting limit (low 6 bits) and the upload filter (bit 6); the rest: text for `tesser::parseJson` | what parses serializes back (`JsonWriter::variant`) to JSON that parses |
| `fuzz_request` | lines fed to two `LineTransport` clients of `wildcard_cases::Fixture` (lists, keyed lists, arrays, actions, a `User`-only node), one trusted, one with a token check (`{"op":"auth"}`) | every line written back is JSON |
| `fuzz_transfer` | lines fed to a `LineTransport` on `file_cases::Fixture` (upload with size limit and extension, download, text file); the whole input also to the base64 decoder | every line written back is JSON; `base64Decode` and `crc32OfBase64` agree; decode/encode round trip |
| `fuzz_http` | a query value, a Host and an Origin, separated by NUL bytes, for `tesser/http_checks.h` | `urlDecode`'s length; `contentDisposition` gives one quoted ASCII name; `*` and the server's own origin are allowed |

The HTTP query parser itself is ESP-IDF's (`httpd_query_key_value`) and isn't built on the host; what TesserAPI does with a query value after it (percent-decoding, `?filename=`) is in `fuzz_http`, and what an envelope's query members do is in `fuzz_request`.

## Running

clang with libFuzzer is needed (any clang ≥ 14; the official LLVM release tarball works without installing). The lock-order checks (`TESSER_LOCK_CHECK`) stay on unless `NDEBUG` is set.

```sh
cmake -S . -B build-fuzz -DTESSER_FUZZ=ON -DTESSER_BUILD_TESTS=OFF \
      -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_FLAGS=-O1
cmake --build build-fuzz -j
python3 test/fuzz/seeds.py build-fuzz/seeds          # seeds from the host tests' string literals
mkdir -p build-fuzz/corpus/request
build-fuzz/test/fuzz/fuzz_request -max_len=4096 -max_total_time=1200 \
      build-fuzz/corpus/request build-fuzz/seeds/request
```

A crash leaves `crash-<sha1>` in the current directory; `build-fuzz/test/fuzz/fuzz_request crash-<sha1>` replays it. Each crash found becomes a host test case beside the code it concerns, with its fix.

CI (`.github/workflows/ci.yml`, job `fuzz-smoke`) builds these on every push and runs each harness for 60 s on the seeds: a harness that stops building, or a shallow crash, fails the push.
