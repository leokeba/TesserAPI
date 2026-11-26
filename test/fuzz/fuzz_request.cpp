// Request envelopes on a line, as a serial, Bluetooth or WebSocket client
// sends them: parseEnvelope() and what follows from it, the op and its
// ceiling ("set@user"), the path walk (objects, keyed and plain lists,
// arrays, '*' fan-out), the query members (depth, keys, exclude, view,
// shape, at...) and the reply. The input is fed as is (several lines, a
// partial one), to two clients of one tree: one with physical access, one
// whose level comes from its token ({"op":"auth"}). Every line written
// back must be JSON.
#include "fuzz_common.h"

#include "wildcard_cases.h"

namespace {

struct Client {
    cases::Lines lines;
    tesser::LineTransport t;
    explicit Client(tesser::Api& api) : t(api, [this](const char* d, size_t n) { lines.add(d, n); }) {}
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    wildcard_cases::Fixture f;
    f.api.authenticate([](std::string_view token, const tesser::Client&) {
        return token == "admin" ? tesser::Access::Admin
               : token == "user" ? tesser::Access::User
                                 : tesser::Access::Public;
    });
    const char* text = reinterpret_cast<const char*>(data);
    {
        Client trusted(f.api);
        Client remote(f.api);
        remote.t.setAuthenticated(false);
        trusted.t.feed(text, size);
        trusted.t.feed('\n');
        remote.t.feed(text, size);
        remote.t.feed('\n');
        // Deferred actions ("later") answered while both are connected.
        for (size_t i = 0; i < f.pendings.size(); i++) {
            if (i % 2) {
                f.pendings[i].reply();
            } else {
                f.pendings[i].fail(tesser::Status::Internal, "failed");
            }
        }
        f.pendings.clear();
        fuzz::checkLines(trusted.lines.out);
        fuzz::checkLines(remote.lines.out);
    }
    return 0;
}
