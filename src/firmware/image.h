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

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tapin::firmware {

using Bytes = std::vector<uint8_t>;

inline constexpr size_t XOR_KEY_SIZE{32};
using XorKey = std::array<uint8_t, XOR_KEY_SIZE>;

// Where things are in the 1024 byte header of an image record, little endian.
inline constexpr size_t RECORD_HEADER_SIZE{1024};
inline constexpr size_t RECORD_SIZE_OFFSET{64};
inline constexpr size_t RECORD_VERSION_OFFSET{68};
inline constexpr size_t RECORD_CRC_OFFSET{70};
inline constexpr size_t RECORD_DEVICE_OFFSET{72};
inline constexpr size_t RECORD_AREA_OFFSET{73};

// The lens mount of a console.
enum class Mount {
    Canon,
    Nikon,
    Sony,
};

// E0, N0 or S0: how Tamron's file names say it.
std::string_view mountCode(Mount mount);

// canon, nikon or sony: how people say it.
std::string_view mountName(Mount mount);
Result<Mount> mountFromName(std::string_view name);

// CRC-16/XMODEM: polynomial 0x1021, initial value 0, not reflected. TAP-in packets, firmware blocks and image headers
// all use it.
uint16_t crc16Xmodem(std::span<const uint8_t> data);

uint16_t readLittleEndian16(std::span<const uint8_t> bytes);
void writeLittleEndian16(std::span<uint8_t> bytes, uint16_t value);

// How a firmware version prints: 4 and 0 is 0400.
std::string versionText(uint8_t major, uint8_t minor);

// One part of an image: a 1024 byte header and a payload.
struct FirmwareRecord {
    size_t offset; // Where the header starts in the image
    size_t payloadSize;
    uint8_t versionMajor;
    uint8_t versionMinor;
    uint8_t device; // 0 is the main processor
    uint8_t area;
};

// A decrypted image, checked by its header CRCs. A lens image has one or two records.
class FirmwareImage {
public:
    static Result<FirmwareImage> parse(Bytes bytes);

    const Bytes& bytes() const {
        return bytes_;
    }

    const std::vector<FirmwareRecord>& records() const {
        return records_;
    }

    const FirmwareRecord& mainRecord() const {
        return records_.front();
    }

    std::span<const uint8_t> recordBytes(const FirmwareRecord& record) const;
    std::span<const uint8_t> payloadOf(const FirmwareRecord& record) const;

    // Whether this image names a lens model and mount, like F013E0.
    bool namesLens(std::string_view modelAndMount) const;

    std::string versionText() const;
    Result<std::string> sha256() const;

private:
    FirmwareImage(Bytes bytes, std::vector<FirmwareRecord> records);

    Bytes bytes_;                         // Headers and payloads, as sent to the lens
    std::vector<FirmwareRecord> records_; // Never empty
};

// The key from the 64 hex characters in Tamron's lensinfo XML.
Result<XorKey> keyFromServer(std::string_view serverKeyHex);

// The key from the parts of a file name: F013, E0 and 0400. Most files use their own version, a few need the server key.
Result<XorKey> keyFromNameParts(std::string_view model, std::string_view mount, std::string_view version);

// The same from Tamron's file name without its extension, like F013E0_0400.
Result<XorKey> keyFromImageName(std::string_view name);

Result<FirmwareImage> decryptImage(std::span<const uint8_t> tfwf, const XorKey& key);

} // namespace tapin::firmware
