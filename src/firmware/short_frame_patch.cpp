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

#include "firmware/short_frame_patch.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <string>
#include <utility>

namespace tapin::firmware {

namespace {

constexpr size_t CHECK_COUNT{4};
constexpr std::string_view NOT_TESTED_STATUS{"NOT TESTED on a real lens"};

// MN10300 payloads begin with this, Cortex-M ones with the vector table.
constexpr std::string_view MN10300_START{"START PROGRAM"};

// MN10300: cmp 0x82,d0 ; beq +4, then the 8 byte path.
constexpr std::array<uint8_t, 6> MN10300_CHECK{0xFA, 0xC8, 0x82, 0x00, 0xC8, 0x04};
constexpr size_t MN10300_BRANCH_OFFSET{4};
constexpr uint8_t MN10300_BRANCH_IF_LOWER_OR_SAME{0xC7}; // bls, replaces beq
constexpr size_t MN10300_PATH_WINDOW{16};                // The load of the frame length is this close after the check
constexpr std::array<std::array<uint8_t, 2>, 3> MN10300_LOADS_OF_8{{{0x80, 0x08}, {0xA0, 0x08}, {0xA8, 0x08}}};

// Cortex-M, Thumb halfwords: cmp rX,#0x82 ; bne, then the 8 byte path.
constexpr uint16_t THUMB_REGISTER_FREE_MASK{0xF8FF};
constexpr uint16_t THUMB_CMP_0X82{0x2882};
constexpr uint16_t THUMB_CMP_8{0x2808};
constexpr uint16_t THUMB_MOVS_8{0x2008};
constexpr uint16_t THUMB_CMP_0X0A{0x280A};
constexpr uint8_t THUMB_BNE{0xD1};
constexpr uint8_t THUMB_BHI{0xD8}; // Replaces bne, same branch target
constexpr size_t THUMB_HALFWORD_SIZE{2};
constexpr size_t THUMB_INSTRUCTIONS_SIZE{4}; // The cmp and the branch
constexpr size_t THUMB_BRANCH_HIGH_BYTE_OFFSET{3};
constexpr size_t THUMB_PATH_WINDOW{0x14};    // The 8 byte path right after the check
constexpr size_t THUMB_COMMAND_WINDOW{0x30}; // The test for the 0x0A second byte right before it

struct TestedImage {
    std::string_view stockSha256;
    std::string_view status;
};

constexpr std::array<TestedImage, 3> TESTED_IMAGES{{
    {"e8f78c88b560159be54ec492edd6852ee98d4920048c7d7b67ca458e5759ea24", "SP 35mm f/1.8 VC, 0400: works on the EOS 3 and the EOS 30V"},
    {"ff46a40b6ee363b4cb983340a2c61652c81e49a4ca35d74756e7af82d484e096", "SP 45mm f/1.8 VC, 0400: works on the EOS 3 and the EOS 30V"},
    {"602c94fa17a88e3d8fc1795099d7b1d539464241430e2173fc4af68cd8beb954", "SP 85mm f/1.8 VC, 0500: works on the EOS 3 and the EOS 30V"},
}};

bool isMn10300(std::span<const uint8_t> payload) {
    return payload.size() >= MN10300_START.size() && std::equal(MN10300_START.begin(), MN10300_START.end(), payload.begin());
}

bool mn10300LoadsFrameLength(std::span<const uint8_t> payload, size_t check) {
    const size_t start = check + MN10300_CHECK.size();
    const size_t end = std::min(start + MN10300_PATH_WINDOW, payload.size());
    const std::span<const uint8_t> path = payload.subspan(start, end - start);
    return std::ranges::any_of(MN10300_LOADS_OF_8, [&](const auto& load) { return !std::ranges::search(path, load).empty(); });
}

std::vector<size_t> findMn10300Checks(std::span<const uint8_t> payload) {
    std::vector<size_t> checks;
    std::span<const uint8_t> rest = payload;
    while (true) {
        const auto found = std::ranges::search(rest, MN10300_CHECK);
        if (found.empty()) {
            return checks;
        }
        const auto offset = static_cast<size_t>(found.data() - payload.data());
        if (mn10300LoadsFrameLength(payload, offset)) {
            checks.push_back(offset);
        }
        rest = payload.subspan(offset + 1);
    }
}

bool thumbWindowHas(std::span<const uint8_t> payload, size_t start, size_t end, std::initializer_list<uint16_t> wanted) {
    for (size_t offset = start; offset + 1 < end; offset += THUMB_HALFWORD_SIZE) {
        const auto masked = static_cast<uint16_t>(readLittleEndian16(payload.subspan(offset)) & THUMB_REGISTER_FREE_MASK);
        if (std::ranges::find(wanted, masked) != wanted.end()) {
            return true;
        }
    }
    return false;
}

std::vector<size_t> findCortexMChecks(std::span<const uint8_t> payload) {
    std::vector<size_t> checks;
    for (size_t offset = 0; offset + THUMB_PATH_WINDOW <= payload.size(); offset += THUMB_HALFWORD_SIZE) {
        const bool isCompare = (readLittleEndian16(payload.subspan(offset)) & THUMB_REGISTER_FREE_MASK) == THUMB_CMP_0X82;
        if (!isCompare || payload[offset + THUMB_BRANCH_HIGH_BYTE_OFFSET] != THUMB_BNE) {
            continue;
        }
        const size_t commandStart = offset > THUMB_COMMAND_WINDOW ? offset - THUMB_COMMAND_WINDOW : 0;
        const bool afterCommandTest = thumbWindowHas(payload, commandStart, offset, {THUMB_CMP_0X0A});
        const size_t pathStart = offset + THUMB_INSTRUCTIONS_SIZE;
        const bool beforeFrameLength = thumbWindowHas(payload, pathStart, offset + THUMB_PATH_WINDOW, {THUMB_CMP_8, THUMB_MOVS_8});
        if (afterCommandTest && beforeFrameLength) {
            checks.push_back(offset);
        }
    }
    return checks;
}

} // namespace

std::vector<size_t> findFrameLengthChecks(std::span<const uint8_t> payload) {
    return isMn10300(payload) ? findMn10300Checks(payload) : findCortexMChecks(payload);
}

Result<PatchedImage> patchShortFrameChecks(const FirmwareImage& stock) {
    const FirmwareRecord& main = stock.mainRecord();
    const std::span<const uint8_t> payload = stock.payloadOf(main);
    const std::vector<size_t> checks = findFrameLengthChecks(payload);
    if (checks.size() != CHECK_COUNT) {
        return fail(ErrorCode::NoPatchSites, std::format("found {} frame length checks in this image, expected {}, so it can't be patched", checks.size(), CHECK_COUNT));
    }
    const bool mn10300 = isMn10300(payload);
    Bytes bytes = stock.bytes();
    const size_t payloadStart = main.offset + RECORD_HEADER_SIZE;
    for (size_t check : checks) {
        if (mn10300) {
            bytes[payloadStart + check + MN10300_BRANCH_OFFSET] = MN10300_BRANCH_IF_LOWER_OR_SAME;
        } else {
            bytes[payloadStart + check + THUMB_BRANCH_HIGH_BYTE_OFFSET] = THUMB_BHI;
        }
    }
    const uint16_t crc = crc16Xmodem(std::span{bytes}.subspan(payloadStart, main.payloadSize));
    writeLittleEndian16(std::span{bytes}.subspan(main.offset + RECORD_CRC_OFFSET), crc);

    const std::string stockSha256 = CHECK(stock.sha256());
    const std::optional<std::string_view> status = testedStatus(stockSha256);
    FirmwareImage image = CHECK(FirmwareImage::parse(std::move(bytes)));
    std::string sha256 = CHECK(image.sha256());
    return PatchedImage{
        .image = std::move(image),
        .sha256 = std::move(sha256),
        .status = std::string{status.value_or(NOT_TESTED_STATUS)},
        .tested = status.has_value(),
    };
}

std::optional<std::string_view> testedStatus(std::string_view stockSha256) {
    const auto* found = std::ranges::find(TESTED_IMAGES, stockSha256, &TestedImage::stockSha256);
    if (found == TESTED_IMAGES.end()) {
        return std::nullopt;
    }
    return found->status;
}

} // namespace tapin::firmware
