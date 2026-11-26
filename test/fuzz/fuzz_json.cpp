// tesser::parseJson, the library's one JSON parser, on any bytes: every
// request body, envelope and stored record goes through it. The first byte
// picks the nesting limit (as maxDepth does on a node) and whether the
// upload filter applies; the rest is the text. What parses must serialize
// back (JsonWriter::variant) to text that parses again.
#include "fuzz_common.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1) return 0;
    uint8_t nesting = data[0] & 0x3f;  // up to 63: within JsonWriter::kMaxNesting
    bool filtered = data[0] & 0x40;
    std::string_view text(reinterpret_cast<const char*>(data + 1), size - 1);
    JsonDocument doc;
    DeserializationError err =
        tesser::parseJson(doc, text, nesting, filtered ? tesser::uploadFilter() : JsonVariantConst());
    if (err) return 0;
    std::string out;
    tesser::StringSink sink(out);
    tesser::JsonWriter w(sink);
    w.variant(doc.as<JsonVariantConst>());
    if (!w.ok()) fuzz::fail("JsonWriter refused a parsed document", std::string(text));
    JsonDocument again;
    if (tesser::parseJson(again, out, nesting)) fuzz::fail("serialized document does not parse", out);
    return 0;
}
