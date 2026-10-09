#pragma once

#include <atomic>
#include <string>
#include <string_view>
#include <type_traits>

#include <ArduinoJson.h>

#include "tesser/json_writer.h"
#include "tesser/request.h"
#include "tesser/value_types.h"

namespace tesser {

class Api;

// Writes the standard error body: {"error":..,"path":..,"message":..}.
void writeError(Reply& reply, Status status, std::string_view path, const char* message, bool partial = false);

namespace detail {
// Ends a reply whose body was written through `w`, turning an overflow into a
// too_large error when the reply can roll back.
void finishBody(Reply& reply, JsonWriter& w, std::string_view path);
}  // namespace detail

// Handle to a reply that outlives the request handler (see Call::defer()).
// Copyable and thread-safe: the first reply()/fail() wins, later ones are
// ignored. If the last copy is destroyed without a reply, the client gets an
// "internal" error, so a forgotten reply never hangs it.
class Pending {
public:
    Pending() = default;
    Pending(const Pending& o);
    Pending(Pending&& o) noexcept : s_(o.s_) { o.s_ = nullptr; }
    Pending& operator=(Pending o) noexcept {
        State* t = s_;
        s_ = o.s_;
        o.s_ = t;
        return *this;
    }
    ~Pending();

    explicit operator bool() const { return s_ != nullptr; }

    template <class T>
    void reply(const T& value) {
        Reply* r = take();
        if (!r) return;
        JsonWriter w(r->begin(Status::Ok));
        ValueTraits<std::remove_cv_t<std::remove_reference_t<T>>>::write(w, value);
        finish(r, w);
    }
    void reply();  // null body
    void fail(Status status, const char* message = nullptr);

private:
    friend class Call;
    struct State {
        std::atomic<int> refs{1};
        std::atomic<bool> done{false};
        Reply* reply = nullptr;
        Api* api = nullptr;
        std::string path;
    };
    explicit Pending(State* s) : s_(s) {}
    Reply* take();
    void finish(Reply* r, JsonWriter& w);
    void finish(Reply* r);

    State* s_ = nullptr;
};

// What an action taking `tesser::Call&` receives: the argument, the client,
// and the means to reply now or later.
class Call {
public:
    JsonVariantConst arg() const { return arg_; }
    const Client& client() const { return client_; }
    std::string_view path() const { return path_; }

    // Immediate result. Without reply(), fail() or defer() the result is null.
    template <class T>
    void reply(const T& value) {
        if (!reply_ || replied_ || deferred_) return;
        replied_ = true;
        JsonWriter w(reply_->begin(Status::Ok));
        ValueTraits<std::remove_cv_t<std::remove_reference_t<T>>>::write(w, value);
        detail::finishBody(*reply_, w, path_);
    }
    void fail(Status status, const char* message = nullptr) {
        status_ = status;
        message_ = message;
    }

    // Keeps the request open; complete it later through the returned handle,
    // from any task. Returns an empty handle (and the call fails with "busy")
    // when the transport can't defer or too many calls are pending.
    Pending defer();

    Call(Api& api, Reply* reply, JsonVariantConst arg, const Client& client, std::string_view path)
        : api_(api), reply_(reply), arg_(arg), client_(client), path_(path) {}

    // Used by the core.
    Status status() const { return status_; }
    const char* message() const { return message_; }
    bool replied() const { return replied_; }
    bool deferred() const { return deferred_; }

private:
    Api& api_;
    Reply* reply_;  // null when called from a patch: results are discarded
    JsonVariantConst arg_;
    const Client& client_;
    std::string_view path_;
    Status status_ = Status::Ok;
    const char* message_ = nullptr;
    bool replied_ = false;
    bool deferred_ = false;
};

}  // namespace tesser
