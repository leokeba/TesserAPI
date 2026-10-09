// Host test runner: ./tesser_tests [filter]
#include "core_cases.h"
#include "datagram_cases.h"

int main(int argc, char** argv) { return check::run(argc > 1 ? argv[1] : nullptr) ? 1 : 0; }
