// File transfers over a line: "upload" and "download" envelopes
// (docs/DESIGN.md section 10.1.1) through FileSession, to the host tests'
// file nodes (file_cases::Fixture: an upload with a size limit and an
// accepted extension, a download, a text file). An upload's "data" is read
// from the envelope's text rather than its document (isUploadEnvelope(),
// uploadFilter()), so the raw scan of the text is fuzzed too. The input is
// fed as is, as several envelopes; every line written back must be JSON.
//
// The base64 decoder gets the input too: whatever it accepts must encode
// back to the same bytes, and crc32OfBase64() must agree with crc32() of
// the decoded bytes.
#include "fuzz_common.h"

#include "file_cases.h"
#include "tesser/file_session.h"

namespace {

void base64Properties(std::string_view text) {
    std::vector<uint8_t> bytes;
    bool decoded = tesser::detail::base64Decode(text, bytes);
    uint32_t crc = 0;
    bool summed = tesser::detail::crc32OfBase64(text, crc);
    if (decoded != summed) fuzz::fail("base64Decode and crc32OfBase64 disagree on", std::string(text));
    if (!decoded) return;
    if (crc != tesser::detail::crc32(bytes.data(), bytes.size(), 0)) fuzz::fail("crc32 of base64 differs", std::string(text));
    std::string again;
    tesser::detail::base64Encode(bytes.data(), bytes.size(), again);
    std::vector<uint8_t> back;
    if (!tesser::detail::base64Decode(again, back) || back != bytes) fuzz::fail("base64 round trip", std::string(text));
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string_view text(reinterpret_cast<const char*>(data), size);
    base64Properties(text);
    file_cases::Fixture f;
    cases::Lines lines;
    {
        tesser::LineTransport t(f.api, [&](const char* d, size_t n) { lines.add(d, n); });
        t.feed(text.data(), text.size());
        t.feed('\n');
    }
    fuzz::checkLines(lines.out);
    return 0;
}
