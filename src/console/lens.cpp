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

#include "console/lens.h"

#include <algorithm>
#include <format>
#include <utility>

namespace tapin::console {

namespace {

constexpr size_t BASE_MODEL_SIZE{4};

constexpr uint8_t FOCUS_LIMIT_MODE_1_BIT{1};
constexpr uint8_t FOCUS_LIMIT_MODE_2_BIT{2};
constexpr int FOCUS_LIMIT_MODES_SHIFT{4};
constexpr uint8_t FOCUS_LIMIT_MODE_SET{2};
constexpr uint8_t FOCUS_LIMIT_MODE_CLEAR{1};

// Where things are in the adjustment data.
constexpr size_t ADJUST_SENSITIVITY_OFFSET{0};
constexpr size_t ADJUST_VC_MODE_OFFSET{1};
constexpr size_t ADJUST_FOCUS_LIMIT_1_OFFSET{3}; // Two bytes of init, then the mode
constexpr size_t ADJUST_FOCUS_LIMIT_1_MODE_OFFSET{5};
constexpr size_t ADJUST_FOCUS_LIMIT_2_OFFSET{7}; // Two bytes of init, then the mode
constexpr size_t ADJUST_FOCUS_LIMIT_2_MODE_OFFSET{9};
constexpr uint8_t DEFAULT_SENSITIVITY{2};
constexpr uint8_t DEFAULT_VC_MODE{2};

constexpr std::array<std::pair<uint8_t, std::string_view>, 4> ADJUST_FLAG_NAMES{{{1, "focus"}, {2, "ftm"}, {4, "flimit"}, {8, "vc"}}};

constexpr uint8_t ADAPTER_MOUNT_CANON{1};
constexpr uint8_t ADAPTER_MOUNT_NIKON{2};
constexpr uint8_t ADAPTER_MOUNT_SONY{3};

std::string textField(std::span<const uint8_t> data) {
    const auto end = std::ranges::find(data, uint8_t{0});
    return std::string(data.begin(), end);
}

std::string versionText(const std::array<uint8_t, 2>& version) {
    return firmware::versionText(version[0], version[1]);
}

std::array<uint8_t, 2> pairAt(std::span<const uint8_t> data, size_t offset) {
    return {data[offset], data[offset + 1]};
}

uint8_t focusLimitMode(int modeBits, uint8_t bit) {
    return (modeBits & bit) != 0 ? FOCUS_LIMIT_MODE_SET : FOCUS_LIMIT_MODE_CLEAR;
}

std::optional<firmware::Mount> mountFromAdapterByte(uint8_t value) {
    switch (value) {
    case ADAPTER_MOUNT_CANON:
        return firmware::Mount::Canon;
    case ADAPTER_MOUNT_NIKON:
        return firmware::Mount::Nikon;
    case ADAPTER_MOUNT_SONY:
        return firmware::Mount::Sony;
    default:
        return std::nullopt;
    }
}

} // namespace

Result<LensInfo> LensInfo::parse(std::span<const uint8_t> data) {
    if (data.size() != basic_info::SIZE) {
        return fail(ErrorCode::IoFailed, std::format("lens info is {} bytes, expected {}", data.size(), basic_info::SIZE));
    }
    using namespace basic_info;
    const int modeBits = data[FOCUS_LIMIT_MODES_OFFSET] >> FOCUS_LIMIT_MODES_SHIFT;
    LensInfo info;
    info.model = textField(data.subspan(MODEL_OFFSET, MODEL_SIZE));
    info.serialNumber = textField(data.subspan(SERIAL_OFFSET, SERIAL_SIZE));
    info.mainFirmware = pairAt(data, MAIN_FIRMWARE_OFFSET);
    info.altMainFirmware = pairAt(data, ALT_MAIN_FIRMWARE_OFFSET);
    info.subFirmware = pairAt(data, SUB_FIRMWARE_OFFSET);
    info.altSubFirmware = pairAt(data, ALT_SUB_FIRMWARE_OFFSET);
    info.hardware = pairAt(data, HARDWARE_OFFSET);
    info.connection = pairAt(data, CONNECTION_OFFSET);
    info.adjustFlags = data[ADJUST_FLAGS_OFFSET];
    std::ranges::copy(data.subspan(FOCUS_LIMIT_INIT_OFFSET, info.focusLimitInit.size()), info.focusLimitInit.begin());
    info.focusLimitModes = {focusLimitMode(modeBits, FOCUS_LIMIT_MODE_1_BIT), focusLimitMode(modeBits, FOCUS_LIMIT_MODE_2_BIT)};
    return info;
}

std::string_view LensInfo::baseModel() const {
    return std::string_view{model}.substr(0, BASE_MODEL_SIZE);
}

std::string LensInfo::firmwareText() const {
    return versionText(mainFirmware);
}

firmware::Bytes LensInfo::defaultAdjustData() const {
    firmware::Bytes data(ADJUST_DATA_SIZE);
    data[ADJUST_SENSITIVITY_OFFSET] = DEFAULT_SENSITIVITY;
    data[ADJUST_VC_MODE_OFFSET] = DEFAULT_VC_MODE;
    data[ADJUST_FOCUS_LIMIT_1_OFFSET] = focusLimitInit[0];
    data[ADJUST_FOCUS_LIMIT_1_OFFSET + 1] = focusLimitInit[1];
    data[ADJUST_FOCUS_LIMIT_1_MODE_OFFSET] = focusLimitModes[0];
    data[ADJUST_FOCUS_LIMIT_2_OFFSET] = focusLimitInit[2];
    data[ADJUST_FOCUS_LIMIT_2_OFFSET + 1] = focusLimitInit[3];
    data[ADJUST_FOCUS_LIMIT_2_MODE_OFFSET] = focusLimitModes[1];
    return data;
}

std::string LensInfo::describe() const {
    std::string adjustable;
    for (const auto& [bit, name] : ADJUST_FLAG_NAMES) {
        if ((adjustFlags & bit) != 0) {
            adjustable += adjustable.empty() ? std::string{name} : std::format(", {}", name);
        }
    }
    std::string text;
    text += std::format("  model:   {}\n", model);
    text += std::format("  serial:  {}\n", serialNumber);
    text += std::format("  main fw: {}  (alt {})\n", firmwareText(), versionText(altMainFirmware));
    text += std::format("  sub fw:  {}  (alt {})\n", versionText(subFirmware), versionText(altSubFirmware));
    text += std::format("  hw:      {:02d}.{:02d}\n", hardware[0], hardware[1]);
    text += std::format("  conn:    {:02d}.{:02d}\n", connection[0], connection[1]);
    text += std::format("  adjust:  {}", adjustable.empty() ? "none" : adjustable);
    return text;
}

Result<AdapterInfo> AdapterInfo::parse(std::span<const uint8_t> data) {
    if (data.size() != basic_info::SIZE) {
        return fail(ErrorCode::IoFailed, std::format("adapter info is {} bytes, expected {}", data.size(), basic_info::SIZE));
    }
    AdapterInfo info;
    info.firmware = versionText(pairAt(data, basic_info::MAIN_FIRMWARE_OFFSET));
    info.mount = mountFromAdapterByte(data[basic_info::ADAPTER_MOUNT_OFFSET]);
    info.hardware = data[basic_info::ADAPTER_HARDWARE_OFFSET];
    info.connection = pairAt(data, basic_info::CONNECTION_OFFSET);
    return info;
}

std::string AdapterInfo::describe() const {
    std::string text;
    text += std::format("  adapter fw: {}\n", firmware);
    text += std::format("  mount:      {}\n", mount ? firmware::mountName(*mount) : "(none)");
    text += std::format("  hw:         {:02d}\n", hardware);
    text += std::format("  conn:       {:02d}.{:02d}", connection[0], connection[1]);
    return text;
}

} // namespace tapin::console
