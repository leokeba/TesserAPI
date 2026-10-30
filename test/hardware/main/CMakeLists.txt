# test/ holds check.h and the shared cases (and the gitignored secrets.h).
idf_component_register(SRCS "main.cpp" "network.cpp" INCLUDE_DIRS "../..")
# A test's frame on the main task's 16 KB: at most 7.8 KB in use (esp32, -Og),
# so a test with its fixtures on the stack stops the build, not the board.
# Fixtures add up: one to a test. Measured on esp32 only.
if(IDF_TARGET STREQUAL "esp32")
    target_compile_options(${COMPONENT_LIB} PRIVATE -Werror=frame-larger-than=10240)
endif()
