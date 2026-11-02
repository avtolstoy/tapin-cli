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

#include "core/error.h"
#include "firmware/image.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace tapin::console {

inline constexpr size_t ADJUST_DATA_SIZE{35};

// The reply to a basic info request, from the lens and from the adapter.
namespace basic_info {

inline constexpr size_t SIZE{85};
inline constexpr size_t MODEL_OFFSET{3};
inline constexpr size_t MODEL_SIZE{16};
inline constexpr size_t SERIAL_OFFSET{19};
inline constexpr size_t SERIAL_SIZE{16};
inline constexpr size_t MAIN_FIRMWARE_OFFSET{35}; // On the adapter, its only firmware
inline constexpr size_t ALT_MAIN_FIRMWARE_OFFSET{37};
inline constexpr size_t SUB_FIRMWARE_OFFSET{39};
inline constexpr size_t ALT_SUB_FIRMWARE_OFFSET{41};
inline constexpr size_t HARDWARE_OFFSET{47};
inline constexpr size_t ADAPTER_MOUNT_OFFSET{47};
inline constexpr size_t ADAPTER_HARDWARE_OFFSET{48};
inline constexpr size_t CONNECTION_OFFSET{49};
inline constexpr size_t ADJUST_FLAGS_OFFSET{51};
inline constexpr size_t FOCUS_LIMIT_MODES_OFFSET{55}; // The high 4 bits
inline constexpr size_t FOCUS_LIMIT_INIT_OFFSET{65};

} // namespace basic_info

// The lens on the console, from its basic info.
struct LensInfo {
    std::string model; // Like F013
    std::string serialNumber;
    std::array<uint8_t, 2> mainFirmware{};
    std::array<uint8_t, 2> altMainFirmware{};
    std::array<uint8_t, 2> subFirmware{};
    std::array<uint8_t, 2> altSubFirmware{};
    std::array<uint8_t, 2> hardware{};
    std::array<uint8_t, 2> connection{};
    uint8_t adjustFlags{};
    std::array<uint8_t, 4> focusLimitInit{};
    std::array<uint8_t, 2> focusLimitModes{};

    static Result<LensInfo> parse(std::span<const uint8_t> data);

    // The first 4 characters, so a renamed test build like F013P1 still counts as F013.
    std::string_view baseModel() const;
    std::string firmwareText() const;

    // The record for a lens with no adjustments.
    firmware::Bytes defaultAdjustData() const;

    std::string describe() const;
};

// The console itself, from its basic info.
struct AdapterInfo {
    std::string firmware;
    std::optional<firmware::Mount> mount; // Nothing when it reports one this tool doesn't know
    uint8_t hardware{};
    std::array<uint8_t, 2> connection{};

    static Result<AdapterInfo> parse(std::span<const uint8_t> data);

    std::string describe() const;
};

} // namespace tapin::console
