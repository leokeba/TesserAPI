// Host test runner: ./tesser_tests [filter]
#include "core_cases.h"
#include "datagram_cases.h"
#include "subscription_cases.h"
#include "persistence_cases.h"
#include "list_cases.h"
#include "remote_cases.h"
#include "client_cases.h"
#include "access_cases.h"
#include "stream_cases.h"
#include "housekeeping_cases.h"
#include "action_cases.h"
#include "array_cases.h"
#include "keyed_cases.h"

int main(int argc, char** argv) { return check::run(argc > 1 ? argv[1] : nullptr) ? 1 : 0; }
