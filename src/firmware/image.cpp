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

#include "firmware/image.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <utility>

#include <openssl/evp.h>

namespace tapin::firmware {

namespace {

constexpr size_t MAX_RECORD_SIZE{1024 * 1024}; // The largest real record is under 400 KB
constexpr unsigned SHA256_SIZE{32};
constexpr std::string_view SERVER_KEY_BASE{"EncryptionToolBaseKey"}; // The server key is XORed with this
constexpr int HEX_BASE{16};

constexpr uint16_t CRC_POLYNOMIAL{0x1021};
constexpr uint16_t CRC_TOP_BIT{0x8000};
constexpr int BITS_PER_BYTE{8};

constexpr size_t MOUNT_CODE_SIZE{2};
constexpr size_t VERSION_CODE_SIZE{4};

size_t readLittleEndian32(std::span<const uint8_t> bytes) {
    size_t value = 0;
    for (size_t index = 0; index < sizeof(uint32_t); ++index) {
        value |= size_t{bytes[index]} << (index * BITS_PER_BYTE);
    }
    return value;
}

} // namespace

std::string_view mountCode(Mount mount) {
    switch (mount) {
    case Mount::Canon:
        return "E0";
    case Mount::Nikon:
        return "N0";
    case Mount::Sony:
        return "S0";
    }
    return {};
}

std::string_view mountName(Mount mount) {
    switch (mount) {
    case Mount::Canon:
        return "canon";
    case Mount::Nikon:
        return "nikon";
    case Mount::Sony:
        return "sony";
    }
    return {};
}

Result<Mount> mountFromName(std::string_view name) {
    const auto sameLetter = [](char first, char second) {
        return std::tolower(static_cast<unsigned char>(first)) == std::tolower(static_cast<unsigned char>(second));
    };
    for (Mount mount : {Mount::Canon, Mount::Nikon, Mount::Sony}) {
        if (std::ranges::equal(mountName(mount), name, sameLetter)) {
            return mount;
        }
    }
    return fail(ErrorCode::InvalidArgument, std::format("unknown mount {}, expected canon, nikon or sony", name));
}

uint16_t crc16Xmodem(std::span<const uint8_t> data) {
    uint16_t crc = 0;
    for (uint8_t byte : data) {
        crc = static_cast<uint16_t>(crc ^ (byte << BITS_PER_BYTE));
        for (int bit = 0; bit < BITS_PER_BYTE; ++bit) {
            const bool high = (crc & CRC_TOP_BIT) != 0;
            crc = static_cast<uint16_t>(crc << 1);
            if (high) {
                crc = static_cast<uint16_t>(crc ^ CRC_POLYNOMIAL);
            }
        }
    }
    return crc;
}

uint16_t readLittleEndian16(std::span<const uint8_t> bytes) {
    return static_cast<uint16_t>(bytes[0] | (bytes[1] << BITS_PER_BYTE));
}

void writeLittleEndian16(std::span<uint8_t> bytes, uint16_t value) {
    bytes[0] = static_cast<uint8_t>(value & 0xFF);
    bytes[1] = static_cast<uint8_t>(value >> BITS_PER_BYTE);
}

std::string versionText(uint8_t major, uint8_t minor) {
    return std::format("{:02d}{:02d}", major, minor);
}

FirmwareImage::FirmwareImage(Bytes bytes, std::vector<FirmwareRecord> records)
        : bytes_(std::move(bytes)),
          records_(std::move(records)) {
}

Result<FirmwareImage> FirmwareImage::parse(Bytes bytes) {
    std::vector<FirmwareRecord> records;
    size_t offset = 0;
    while (offset + RECORD_HEADER_SIZE <= bytes.size()) {
        const std::span<const uint8_t> header = std::span{bytes}.subspan(offset, RECORD_HEADER_SIZE);
        const size_t size = readLittleEndian32(header.subspan(RECORD_SIZE_OFFSET));
        if (size > MAX_RECORD_SIZE) {
            return fail(ErrorCode::InvalidArgument, std::format("firmware record at 0x{:X} is too large", offset));
        }
        if (offset + RECORD_HEADER_SIZE + size > bytes.size()) {
            return fail(ErrorCode::InvalidArgument, std::format("firmware record at 0x{:X} is cut short", offset));
        }
        const std::span<const uint8_t> payload = std::span{bytes}.subspan(offset + RECORD_HEADER_SIZE, size);
        if (crc16Xmodem(payload) != readLittleEndian16(header.subspan(RECORD_CRC_OFFSET))) {
            return fail(ErrorCode::InvalidArgument, std::format("firmware record at 0x{:X} has a bad CRC", offset));
        }
        records.push_back(FirmwareRecord{
            .offset = offset,
            .payloadSize = size,
            .versionMajor = header[RECORD_VERSION_OFFSET],
            .versionMinor = header[RECORD_VERSION_OFFSET + 1],
            .device = header[RECORD_DEVICE_OFFSET],
            .area = header[RECORD_AREA_OFFSET],
        });
        offset += RECORD_HEADER_SIZE + size;
    }
    if (offset != bytes.size()) {
        return fail(ErrorCode::InvalidArgument, std::format("{} trailing bytes after the last firmware record", bytes.size() - offset));
    }
    if (records.empty()) {
        return fail(ErrorCode::InvalidArgument, "no firmware records");
    }
    return FirmwareImage{std::move(bytes), std::move(records)};
}

std::span<const uint8_t> FirmwareImage::recordBytes(const FirmwareRecord& record) const {
    return std::span{bytes_}.subspan(record.offset, RECORD_HEADER_SIZE + record.payloadSize);
}

std::span<const uint8_t> FirmwareImage::payloadOf(const FirmwareRecord& record) const {
    return std::span{bytes_}.subspan(record.offset + RECORD_HEADER_SIZE, record.payloadSize);
}

bool FirmwareImage::namesLens(std::string_view modelAndMount) const {
    const std::span<const uint8_t> payload = payloadOf(mainRecord());
    const auto* begin = reinterpret_cast<const uint8_t*>(modelAndMount.data());
    return !std::ranges::search(payload, std::span{begin, modelAndMount.size()}).empty();
}

std::string FirmwareImage::versionText() const {
    return firmware::versionText(mainRecord().versionMajor, mainRecord().versionMinor);
}

Result<std::string> FirmwareImage::sha256() const {
    std::array<unsigned char, SHA256_SIZE> digest{};
    unsigned digestSize = 0;
    if (EVP_Digest(bytes_.data(), bytes_.size(), digest.data(), &digestSize, EVP_sha256(), nullptr) != 1) {
        return fail(ErrorCode::Internal, "SHA-256 failed");
    }
    std::string text;
    for (unsigned char byte : digest) {
        text += std::format("{:02x}", byte);
    }
    return text;
}

Result<XorKey> keyFromServer(std::string_view serverKeyHex) {
    if (serverKeyHex.size() != XOR_KEY_SIZE * 2) {
        return fail(ErrorCode::InvalidArgument, "the firmware key must be 64 hex characters");
    }
    XorKey key{};
    for (size_t index = 0; index < key.size(); ++index) {
        uint8_t value{};
        const char* first = serverKeyHex.data() + index * 2;
        const auto [end, error] = std::from_chars(first, first + 2, value, HEX_BASE);
        if (error != std::errc{} || end != first + 2) {
            return fail(ErrorCode::InvalidArgument, "the firmware key is not hex");
        }
        const auto base = static_cast<uint8_t>(SERVER_KEY_BASE[index % SERVER_KEY_BASE.size()]);
        key[index] = static_cast<uint8_t>(value ^ base);
    }
    return key;
}

Result<XorKey> keyFromNameParts(std::string_view model, std::string_view mount, std::string_view version) {
    if (model.empty() || mount.size() != MOUNT_CODE_SIZE || version.size() != VERSION_CODE_SIZE) {
        return fail(ErrorCode::InvalidArgument, std::format("{}, {} and {} don't look like F013, E0 and 0400", model, mount, version));
    }
    const std::string_view withoutLeadingZero = version.starts_with('0') ? version.substr(1) : version;
    const std::string seed = std::format("{}{}{}", model, mount, withoutLeadingZero);
    XorKey key{};
    for (size_t index = 0; index < key.size(); ++index) {
        key[index] = static_cast<uint8_t>(seed[index % seed.size()]);
    }
    return key;
}

Result<XorKey> keyFromImageName(std::string_view name) {
    const size_t separator = name.rfind('_');
    if (separator == std::string_view::npos || separator <= MOUNT_CODE_SIZE) {
        return fail(ErrorCode::InvalidArgument, std::format("{} is not named like F013E0_0400, give --key", name));
    }
    const std::string_view model = name.substr(0, separator - MOUNT_CODE_SIZE);
    const std::string_view mount = name.substr(separator - MOUNT_CODE_SIZE, MOUNT_CODE_SIZE);
    return keyFromNameParts(model, mount, name.substr(separator + 1));
}

Result<FirmwareImage> decryptImage(std::span<const uint8_t> tfwf, const XorKey& key) {
    Bytes bytes(tfwf.begin(), tfwf.end());
    for (size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<uint8_t>(bytes[index] ^ key[index % key.size()]);
    }
    return FirmwareImage::parse(std::move(bytes));
}

} // namespace tapin::firmware
