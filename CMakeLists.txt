# Copyright 2026 Andrey Tolstoy <avtolstoy@gmail.com>
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required(VERSION 3.31)

# Before project(), so CMake detects the compiler's implicit libraries for this link. macOS can't be fully static.
# The conda libstdc++.a calls pthread through weak references, and -static leaves them at 0.
set(TAPIN_LINUX_LINK_FLAGS
    -static
    -Wl,-u,pthread_create
    -Wl,-u,pthread_join
    -Wl,-u,pthread_detach
    -Wl,-u,pthread_cond_wait
    -Wl,-u,pthread_cond_signal
    -Wl,-u,pthread_cond_broadcast
    -Wl,-u,pthread_cond_timedwait
    -Wl,-u,pthread_cond_clockwait
    -Wl,-u,pthread_cond_destroy
    -Wl,-u,pthread_mutex_lock
    -Wl,-u,pthread_mutex_unlock
    -Wl,-u,pthread_mutex_trylock
    -Wl,-u,pthread_mutex_clocklock
    -Wl,-u,pthread_once
    -Wl,-u,pthread_key_create
    -Wl,-u,pthread_getspecific
    -Wl,-u,pthread_setspecific
    -Wl,-u,pthread_self
    -Wl,-u,pthread_equal
)
option(TAPIN_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer, not static" OFF)
if(TAPIN_SANITIZE)
    set(CMAKE_EXE_LINKER_FLAGS_INIT "")
elseif(CMAKE_HOST_WIN32)
    set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
elseif(CMAKE_HOST_APPLE)
    set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libstdc++ -static-libgcc")
else()
    list(JOIN TAPIN_LINUX_LINK_FLAGS " " CMAKE_EXE_LINKER_FLAGS_INIT)
endif()

project(tapin_cli VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

option(TAPIN_BUILD_TESTS "Build the snitch test binary" ON)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/core/version.h.in ${CMAKE_BINARY_DIR}/generated/core/version.h @ONLY)

include(ExternalProject)
include(FetchContent)

FetchContent_Declare(
    stdexec
    URL https://github.com/NVIDIA/stdexec/archive/05807d69ba8d58377ac046d333f3a758b1931f6b.tar.gz
    URL_HASH SHA256=ce022f553472eb03c7f05e6a587dcb5ee809bd308f380317dd8308ca40e54960
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR do-not-add
    SYSTEM
)
FetchContent_MakeAvailable(stdexec)

FetchContent_Declare(
    asio
    URL https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-38-0.tar.gz
    URL_HASH SHA256=5cf78ede456fd13b95fe692711f4f689be26c826f1f5541c7a1df3aa32bd9dbd
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM
)
FetchContent_MakeAvailable(asio)

set(LLHTTP_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(LLHTTP_BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    llhttp
    URL https://github.com/nodejs/llhttp/archive/refs/tags/release/v9.4.3.tar.gz
    URL_HASH SHA256=1eb813c7437b31a87496a1cd3ed79f00746720f5e7e29c79b42c02cb69f36c39
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM
)
FetchContent_MakeAvailable(llhttp)

FetchContent_Declare(
    pugixml
    URL https://github.com/zeux/pugixml/releases/download/v1.15/pugixml-1.15.tar.gz
    URL_HASH SHA256=655ade57fa703fb421c2eb9a0113b5064bddb145d415dd1f88c79353d90d511a
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM
)
set(PUGIXML_NO_EXCEPTIONS ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(pugixml)

set(SNITCH_WITH_EXCEPTIONS OFF CACHE BOOL "" FORCE)
set(SNITCH_WITH_SHORTHAND_MACROS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    snitch
    URL https://github.com/snitch-org/snitch/archive/refs/tags/v1.3.2.tar.gz
    URL_HASH SHA256=5b3a0af7e5637f02ac95652f2238b89eb227b4881bf4e5ffc6935f184ff11626
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM
)
FetchContent_MakeAvailable(snitch)

# Built from source on every OS: one pinned version, static, nothing loaded at run time, no config file read.
if(WIN32)
    set(OPENSSL_TARGET mingw64)
elseif(APPLE)
    set(OPENSSL_TARGET darwin64-arm64-cc)
else()
    set(OPENSSL_TARGET linux-x86_64)
endif()
set(OPENSSL_DIR ${CMAKE_BINARY_DIR}/openssl)
ExternalProject_Add(
    openssl_build
    URL https://github.com/openssl/openssl/releases/download/openssl-3.5.9/openssl-3.5.9.tar.gz
    URL_HASH SHA256=603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    PREFIX ${CMAKE_BINARY_DIR}/openssl-build
    BUILD_IN_SOURCE TRUE
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env CC=${CMAKE_C_COMPILER} perl Configure ${OPENSSL_TARGET} --prefix=${OPENSSL_DIR} --libdir=lib no-shared no-dso no-module no-tests no-docs no-apps no-engine no-legacy no-comp no-autoload-config
    BUILD_COMMAND make -j8 build_libs
    INSTALL_COMMAND make install_dev
    BUILD_BYPRODUCTS ${OPENSSL_DIR}/lib/libssl.a ${OPENSSL_DIR}/lib/libcrypto.a
)
file(MAKE_DIRECTORY ${OPENSSL_DIR}/include)

set(STDEXEC_ASIO_USES_STANDALONE 1)
set(STDEXEC_ASIO_USES_BOOST 0)
configure_file(${stdexec_SOURCE_DIR}/include/exec/asio/asio_config.hpp.in ${CMAKE_BINARY_DIR}/generated/exec/asio/asio_config.hpp @ONLY)

add_library(tapin_flags INTERFACE)
target_compile_options(
    tapin_flags
    INTERFACE
        $<$<COMPILE_LANGUAGE:CXX>:-Wall>
        $<$<COMPILE_LANGUAGE:CXX>:-Wextra>
        $<$<COMPILE_LANGUAGE:CXX>:-Wpedantic>
        $<$<COMPILE_LANGUAGE:CXX>:-Werror>
        $<$<COMPILE_LANGUAGE:CXX>:-Wconversion>
        $<$<COMPILE_LANGUAGE:CXX>:-Wsign-conversion>
        $<$<COMPILE_LANGUAGE:CXX>:-Wshadow>
        $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
        $<$<COMPILE_LANGUAGE:CXX>:-freflection>
        $<$<BOOL:${TAPIN_SANITIZE}>:-fsanitize=address,undefined>
        $<$<BOOL:${TAPIN_SANITIZE}>:-fno-omit-frame-pointer>
)
target_link_options(tapin_flags INTERFACE $<$<BOOL:${TAPIN_SANITIZE}>:-fsanitize=address,undefined>)

add_library(
    tapin_lib STATIC
    src/cli/commands.cpp
    src/console/console.cpp
    src/console/lens.cpp
    src/console/wire.cpp
    src/firmware/download.cpp
    src/firmware/image.cpp
    src/firmware/short_frame_patch.cpp
    src/flash/flasher.cpp
    src/io/file.cpp
    src/io/http_response.cpp
    src/io/https.cpp
    src/io/interrupt_watch.cpp
    src/io/loop.cpp
    src/io/serial_link.cpp
    src/io/system_roots.cpp
    src/io/terminal.cpp
    src/io/usb_serial_search.cpp
)
add_dependencies(tapin_lib openssl_build)
target_include_directories(tapin_lib PUBLIC src ${CMAKE_BINARY_DIR}/generated)
target_include_directories(tapin_lib SYSTEM PUBLIC ${stdexec_SOURCE_DIR}/include ${asio_SOURCE_DIR}/asio/include ${OPENSSL_DIR}/include ${CMAKE_BINARY_DIR}/generated)
target_compile_definitions(tapin_lib PUBLIC ASIO_STANDALONE ASIO_NO_EXCEPTIONS ASIO_NO_DEPRECATED)
target_link_libraries(tapin_lib PUBLIC llhttp_static pugixml::pugixml ${OPENSSL_DIR}/lib/libssl.a ${OPENSSL_DIR}/lib/libcrypto.a tapin_flags)

if(WIN32)
    target_compile_definitions(tapin_lib PUBLIC _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN)
    target_link_libraries(tapin_lib PUBLIC ws2_32 mswsock advapi32 crypt32 gdi32)
elseif(APPLE)
    # kqueue doesn't work on serial ports.
    target_compile_definitions(tapin_lib PUBLIC ASIO_DISABLE_KQUEUE)
    target_sources(tapin_lib PRIVATE src/io/usb_serial_search_macos.c src/io/system_roots_macos.c)
    target_link_libraries(tapin_lib PUBLIC "-framework IOKit" "-framework CoreFoundation" "-framework Security")
endif()

add_executable(tapin_cli src/main.cpp)
set_target_properties(tapin_cli PROPERTIES OUTPUT_NAME tapin-cli)
target_link_libraries(tapin_cli PRIVATE tapin_lib)

if(TAPIN_BUILD_TESTS)
    enable_testing()
    add_executable(tapin_tests
        tests/fake_console.cpp
        tests/test_cli.cpp
        tests/test_console.cpp
        tests/test_firmware.cpp
        tests/test_flasher.cpp
        tests/test_http_response.cpp
        tests/test_lens.cpp
        tests/test_wire.cpp
    )
    target_include_directories(tapin_tests PRIVATE tests)
    target_link_libraries(tapin_tests PRIVATE tapin_lib snitch::snitch)
    if(NOT WIN32 AND NOT APPLE)
        target_link_libraries(tapin_tests PRIVATE util)
    endif()
    add_test(NAME tapin_tests COMMAND tapin_tests)
    set_tests_properties(tapin_tests PROPERTIES TIMEOUT 180)
endif()
