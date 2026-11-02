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

#include "firmware/image.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

// The TAP-in Console wire format.
//
//   0F <seq> <target> 00 <len_lo> <len_hi> <cmd> [data...] <crc_lo> <crc_hi> F0
//
// len is the data size + 1. The CRC covers everything before it. After an update request the firmware goes over
// XMODEM-1K with a CRC, except that block numbers wrap at 255:
//
//   02 <blk> <~blk> <1024 bytes, padded with 1A> <crc_hi> <crc_lo>
namespace tapin::console {

inline constexpr unsigned USB_VENDOR_ID{0x2CD1};
inline constexpr unsigned USB_PRODUCT_ID{0x0001};

inline constexpr uint8_t PACKET_START{0x0F};
inline constexpr size_t PACKET_HEADER_SIZE{7};
inline constexpr size_t XMODEM_BLOCK_DATA_SIZE{1024};
inline constexpr size_t XMODEM_BLOCK_SIZE{1029}; // The start byte, two block number bytes, the data, two CRC bytes

enum class Target : uint8_t {
    Lens = 0,
    Adapter = 1,
};

enum class Command : uint8_t {
    MountStatus = 0xF7,
    Connect = 0xF8,
    Disconnect = 0xF9,
    BasicInfo = 0xFA,
    SetAdjust = 0xFB,
    GetAdjust = 0xFC,
    Update = 0xFD,
    Error = 0xFF,
};

// The single control bytes of XMODEM. The console sends C when it is ready, and the rest go as usual.
enum class XmodemControl : uint8_t {
    CrcStart = 0x43, // C
    Ack = 0x06,
    Nak = 0x15,
    Cancel = 0x18,
    EndOfTransmission = 0x04,
};

struct Packet {
    uint8_t seq{};
    uint8_t target{};
    uint8_t command{};
    firmware::Bytes data;
    firmware::Bytes raw;
};

// Bytes as text for people: 0F 01 F7
std::string hexText(std::span<const uint8_t> data);

firmware::Bytes buildPacket(Target target, Command command, std::span<const uint8_t> data, uint8_t seq);
firmware::Bytes buildXmodemBlock(std::span<const uint8_t> chunk, size_t blockNumber);

// The full size of a packet from its header, or nothing if its length field is not valid.
std::optional<size_t> packetSize(std::span<const uint8_t> header);

// One complete packet, or nothing if it is not valid.
std::optional<Packet> parsePacket(std::span<const uint8_t> raw);

} // namespace tapin::console
