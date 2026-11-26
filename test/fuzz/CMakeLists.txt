# libFuzzer harnesses (test/fuzz/README.md): clang only, with ASan and
# UBSan, the library instrumented for coverage. Off by default
# (-DTESSER_FUZZ=ON).
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "TESSER_FUZZ needs clang (libFuzzer); set CMAKE_CXX_COMPILER=clang++")
endif()

set(TESSER_FUZZ_FLAGS -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=all
                      -fno-omit-frame-pointer -g)
target_compile_options(tesser_core PRIVATE ${TESSER_FUZZ_FLAGS})

foreach(name json request transfer http)
    add_executable(fuzz_${name} fuzz_${name}.cpp)
    target_link_libraries(fuzz_${name} PRIVATE tesser_core)
    target_include_directories(fuzz_${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    set_target_properties(fuzz_${name} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    target_compile_options(fuzz_${name} PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti
                           -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g)
    target_link_options(fuzz_${name} PRIVATE -fsanitize=fuzzer,address,undefined)
endforeach()
