// Shared by the libFuzzer harnesses (test/fuzz/README.md). Each harness is
// its own program, so this header holds the one copy of the heap-limit
// implementation the host test headers expect (tesser/testing/heap_limit.h).
#pragma once

#define TESSER_HEAP_LIMIT_IMPLEMENTATION
#include "tesser/testing/heap_limit.h"

#include <stdio.h>
#include <stdlib.h>

#include <string>
#include <vector>

#include <ArduinoJson.h>

#include "tesser/envelope.h"
#include "tesser/json_writer.h"

namespace fuzz {

// Aborts (a crash libFuzzer keeps) with what broke.
[[noreturn]] inline void fail(const char* what, const std::string& detail) {
    fprintf(stderr, "FUZZ CHECK FAILED: %s\n%s\n", what, detail.c_str());
    abort();
}

// Every line a node writes to a client must be one JSON value: replies,
// errors and notifications alike, whatever the client sent.
inline void checkLines(const std::vector<std::string>& lines) {
    for (const std::string& line : lines) {
        JsonDocument doc;
        DeserializationError err = tesser::parseJson(doc, line, tesser::kWrittenNesting);
        if (err) fail("a line written is not JSON", line);
    }
}

}  // namespace fuzz
