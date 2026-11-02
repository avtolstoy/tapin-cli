// Copyright 2026 Andrey Tolstoy <avtolstoy@gmail.com>
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include "console/lens.h"
#include "core/error.h"
#include "firmware/image.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <random>
#include <span>
#include <string_view>
#include <system_error>
#include <vector>

#include <snitch/snitch.hpp>

// snitch is built without its shorthand macros because CHECK (core/error.h) propagates errors. Tests use these names.
#define TEST_CASE(...) SNITCH_TEST_CASE(__VA_ARGS__)
#define EXPECT(...) SNITCH_REQUIRE_IMPL("EXPECT", true, (void)0, __VA_ARGS__)
#define EXPECT_FALSE(...) SNITCH_REQUIRE_IMPL("EXPECT_FALSE", false, (void)0, __VA_ARGS__)

// Setup that has to work: names the value of a Result, or fails the test with the error message and ends it. Only in a
// test body, which returns void.
#define EXPECT_VALUE(variable, ...)                          \
    auto variable##Result = (__VA_ARGS__);                   \
    if (!variable##Result) {                                 \
        SNITCH_FAIL_CHECK(variable##Result.error().message); \
        return;                                              \
    }                                                        \
    auto& variable = *variable##Result

namespace tapin::test {

template <typename T>
bool failedWith(const Result<T>& result, ErrorCode code) {
    return !result && result.error().code == code;
}

template <typename T>
bool failedSaying(const Result<T>& result, std::string_view text) {
    return !result && result.error().message.contains(text);
}

inline void updateRecordCrc(firmware::Bytes& bytes) {
    const uint16_t crc = firmware::crc16Xmodem(std::span{bytes}.subspan(firmware::RECORD_HEADER_SIZE));
    firmware::writeLittleEndian16(std::span{bytes}.subspan(firmware::RECORD_CRC_OFFSET), crc);
}

// A valid image with one record, whose payload has the marker near its start.
inline firmware::Bytes makeImageBytes(std::string_view marker, uint8_t versionMajor, size_t payloadSize = 4 * 1024) {
    constexpr size_t MARKER_OFFSET{16}; // Past where a real payload starts its code or its vector table
    firmware::Bytes bytes(firmware::RECORD_HEADER_SIZE + payloadSize);
    for (size_t index = 0; index < marker.size(); ++index) {
        bytes[firmware::RECORD_HEADER_SIZE + MARKER_OFFSET + index] = static_cast<uint8_t>(marker[index]);
    }
    for (size_t index = 0; index < sizeof(uint32_t); ++index) {
        bytes[firmware::RECORD_SIZE_OFFSET + index] = static_cast<uint8_t>(payloadSize >> (index * 8));
    }
    bytes[firmware::RECORD_VERSION_OFFSET] = versionMajor;
    updateRecordCrc(bytes);
    return bytes;
}

// Two records, the way most Cortex-M images are: the main processor, then device 1.
inline firmware::Bytes makeTwoRecordImageBytes(std::string_view marker, uint8_t versionMajor) {
    constexpr uint8_t SUB_DEVICE{1};
    constexpr uint8_t SUB_VERSION_MAJOR{1};
    firmware::Bytes bytes = makeImageBytes(marker, versionMajor);
    firmware::Bytes sub = makeImageBytes("", SUB_VERSION_MAJOR);
    sub[firmware::RECORD_DEVICE_OFFSET] = SUB_DEVICE;
    bytes.insert(bytes.end(), sub.begin(), sub.end());
    return bytes;
}

inline firmware::Bytes encryptImage(firmware::Bytes bytes, const firmware::XorKey& key) {
    for (size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<uint8_t>(bytes[index] ^ key[index % key.size()]);
    }
    return bytes;
}

// The basic info a lens replies with.
inline firmware::Bytes makeLensInfoBytes(std::string_view model, uint8_t firmwareMajor) {
    using namespace console::basic_info;
    constexpr std::string_view SERIAL{"TESTSERIAL"};
    constexpr uint8_t ADJUST_FOCUS_FTM_VC{0x0B};
    firmware::Bytes info(SIZE);
    std::ranges::copy(model, info.begin() + MODEL_OFFSET);
    std::ranges::copy(SERIAL, info.begin() + SERIAL_OFFSET);
    info[MAIN_FIRMWARE_OFFSET] = firmwareMajor;
    info[ADJUST_FLAGS_OFFSET] = ADJUST_FOCUS_FTM_VC;
    return info;
}

// The basic info an adapter replies with. mount is the byte the console reports: 1 Canon, 2 Nikon, 3 Sony.
inline firmware::Bytes makeAdapterInfoBytes(uint8_t mount) {
    using namespace console::basic_info;
    constexpr uint8_t FIRMWARE_MAJOR{3};
    constexpr uint8_t HARDWARE{1};
    firmware::Bytes info(SIZE);
    info[MAIN_FIRMWARE_OFFSET] = FIRMWARE_MAJOR;
    info[ADAPTER_MOUNT_OFFSET] = mount;
    info[ADAPTER_HARDWARE_OFFSET] = HARDWARE;
    return info;
}

// A private directory under the system temp dir, removed with everything in it at scope exit.
struct TempDir {
    std::filesystem::path path;

    TempDir() {
        std::random_device random;
        path = std::filesystem::temp_directory_path() / std::format("tapin-test-{}", random());
        std::error_code error;
        std::filesystem::create_directories(path, error);
    }

    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

} // namespace tapin::test
