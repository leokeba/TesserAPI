#include "tesser/status.h"

#include <string.h>

namespace tesser {

namespace {

struct StatusInfo {
    const char* name;
    int http;
};

const StatusInfo kStatus[] = {
    {"ok", 200},
    {"bad_request", 400},
    {"not_found", 404},
    {"invalid_value", 422},
    {"read_only", 405},
    {"not_allowed", 405},
    {"unauthorized", 403},
    {"too_large", 413},
    {"busy", 503},
    {"timeout", 504},
    {"internal", 500},
};

constexpr size_t kStatusCount = sizeof(kStatus) / sizeof(kStatus[0]);

}  // namespace

const char* toString(Status s) {
    size_t i = static_cast<size_t>(s);
    return i < kStatusCount ? kStatus[i].name : "internal";
}

bool parseStatus(const char* name, Status& out) {
    if (!name) return false;
    for (size_t i = 0; i < kStatusCount; i++) {
        if (strcmp(kStatus[i].name, name) == 0) {
            out = static_cast<Status>(i);
            return true;
        }
    }
    return false;
}

namespace {
const char* const kReasons[] = {
    "",           "low_memory", "pending_limit", "queue_full", "unreachable",   "subscription_limit",
    "transfer_busy", "peer_offline", "no_answer", "idle",      "cannot_wait",   "not_authorized",
    "ceiling_unsupported", "unknown_option", "send_failed", "auth_failed", "rejected",
};
constexpr size_t kReasonCount = sizeof(kReasons) / sizeof(kReasons[0]);
}  // namespace

const char* toString(Reason r) {
    size_t i = static_cast<size_t>(r);
    return i < kReasonCount ? kReasons[i] : "";
}

bool parseReason(const char* name, Reason& out) {
    if (!name || !*name) return false;
    for (size_t i = 1; i < kReasonCount; i++) {
        if (strcmp(kReasons[i], name) == 0) {
            out = static_cast<Reason>(i);
            return true;
        }
    }
    return false;
}

int httpCode(Status s) {
    size_t i = static_cast<size_t>(s);
    return i < kStatusCount ? kStatus[i].http : 500;
}

}  // namespace tesser
