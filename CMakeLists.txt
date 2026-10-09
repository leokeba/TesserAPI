# ESP-IDF component when built by idf.py, host library + tests otherwise.
set(TESSER_CORE_SRCS
    src/tesser/status.cpp
    src/tesser/platform.cpp
    src/tesser/json_writer.cpp
    src/tesser/node.cpp
    src/tesser/call.cpp
    src/tesser/api.cpp
    src/tesser/envelope.cpp
    src/tesser/line_transport.cpp
    src/tesser/datagram.cpp
    src/tesser/subscription.cpp
    src/tesser/persistence.cpp
    src/tesser/storage.cpp
    src/tesser/remote.cpp)

if(ESP_PLATFORM)
    # The UART driver moved out of "driver" in ESP-IDF 5.3.
    if(IDF_VERSION_MAJOR GREATER 5 OR (IDF_VERSION_MAJOR EQUAL 5 AND IDF_VERSION_MINOR GREATER_EQUAL 3))
        set(TESSER_UART_REQ esp_driver_uart)
    else()
        set(TESSER_UART_REQ driver)
    endif()
    set(TESSER_SRCS ${TESSER_CORE_SRCS}
        src/tesser/transports/uart_transport.cpp
        src/tesser/transports/http_server.cpp)
    set(TESSER_REQS ${TESSER_UART_REQ} esp_http_server)
    # The NowTP transport is built when a NowTP component is part of the build.
    idf_build_get_property(TESSER_BUILD_COMPONENTS BUILD_COMPONENTS)
    set(TESSER_NOWTP "")
    foreach(c ${TESSER_BUILD_COMPONENTS})
        if(c MATCHES "(^|__)nowtp$" OR c STREQUAL "NowTP")
            set(TESSER_NOWTP ${c})
        endif()
    endforeach()
    if(TESSER_NOWTP)
        list(APPEND TESSER_SRCS src/tesser/transports/nowtp_transport.cpp)
    endif()
    idf_component_register(
        SRCS ${TESSER_SRCS}
        INCLUDE_DIRS "src"
        REQUIRES ${TESSER_REQS}
        PRIV_REQUIRES log esp_timer lwip nvs_flash)
    if(TESSER_NOWTP)
        # Requirements are resolved before the component list is known, so
        # NowTP is linked here rather than listed in REQUIRES.
        idf_component_get_property(TESSER_NOWTP_LIB ${TESSER_NOWTP} COMPONENT_LIB)
        target_link_libraries(${COMPONENT_LIB} PUBLIC ${TESSER_NOWTP_LIB})
        target_compile_definitions(${COMPONENT_LIB} PUBLIC TESSER_HAVE_NOWTP=1)
    endif()
    return()
endif()

cmake_minimum_required(VERSION 3.16)
project(tesserapi CXX)

option(TESSER_BUILD_TESTS "Build host-side tests" ON)
option(TESSER_SANITIZE "Build tests with ASan/UBSan" ON)

include(FetchContent)
FetchContent_Declare(arduinojson
    URL https://github.com/bblanchon/ArduinoJson/archive/refs/tags/v7.4.3.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
FetchContent_GetProperties(arduinojson)
if(NOT arduinojson_POPULATED)
    FetchContent_Populate(arduinojson)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(tesser_core ${TESSER_CORE_SRCS})
target_include_directories(tesser_core PUBLIC src)
target_include_directories(tesser_core SYSTEM PUBLIC ${arduinojson_SOURCE_DIR}/src)
set_target_properties(tesser_core PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(tesser_core PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror -fno-exceptions -fno-rtti)

if(TESSER_BUILD_TESTS)
    enable_testing()
    add_executable(tesser_tests test/test_main.cpp)
    target_link_libraries(tesser_tests PRIVATE tesser_core)
    target_include_directories(tesser_tests PRIVATE test)
    set_target_properties(tesser_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    target_compile_options(tesser_tests PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
    if(TESSER_SANITIZE)
        foreach(t tesser_core tesser_tests)
            target_compile_options(${t} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -g)
            target_link_options(${t} PRIVATE -fsanitize=address,undefined)
        endforeach()
    endif()
    add_test(NAME tesser_tests COMMAND tesser_tests)
endif()
