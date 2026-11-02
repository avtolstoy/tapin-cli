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

#include "console/wire.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>

namespace tapin::console {

namespace {

constexpr size_t PACKET_SEQ_OFFSET{1};
constexpr size_t PACKET_TARGET_OFFSET{2};
constexpr uint8_t PACKET_RESERVED{0}; // The byte after the target, always 0
constexpr size_t PACKET_LENGTH_OFFSET{4};
constexpr size_t PACKET_COMMAND_OFFSET{6};
constexpr uint8_t PACKET_END{0xF0};
constexpr size_t PACKET_OVERHEAD{9};     // Everything around the data except the command, which the length counts
constexpr size_t PACKET_TRAILER_SIZE{3}; // Two CRC bytes and the end byte
constexpr size_t MAX_PACKET_LENGTH{2048};

constexpr uint8_t XMODEM_STX{0x02};
constexpr uint8_t XMODEM_PADDING{0x1A};
constexpr size_t XMODEM_HEADER_SIZE{3};     // Start, number, inverted number
constexpr size_t BLOCK_NUMBER_MODULUS{255}; // Not XMODEM's 256: the console counts blocks this way

} // namespace

std::string hexText(std::span<const uint8_t> data) {
    std::string text;
    for (uint8_t byte : data) {
        text += text.empty() ? std::format("{:02X}", byte) : std::format(" {:02X}", byte);
    }
    return text;
}

firmware::Bytes buildPacket(Target target, Command command, std::span<const uint8_t> data, uint8_t seq) {
    const size_t length = data.size() + 1;
    firmware::Bytes packet{
        PACKET_START,
        seq,
        static_cast<uint8_t>(target),
        PACKET_RESERVED,
        static_cast<uint8_t>(length & 0xFF),
        static_cast<uint8_t>(length >> 8),
        static_cast<uint8_t>(command),
    };
    packet.insert(packet.end(), data.begin(), data.end());
    const uint16_t crc = firmware::crc16Xmodem(packet);
    packet.resize(packet.size() + sizeof(crc));
    firmware::writeLittleEndian16(std::span{packet}.last(sizeof(crc)), crc);
    packet.push_back(PACKET_END);
    return packet;
}

firmware::Bytes buildXmodemBlock(std::span<const uint8_t> chunk, size_t blockNumber) {
    const auto number = static_cast<uint8_t>(blockNumber % BLOCK_NUMBER_MODULUS);
    firmware::Bytes block(XMODEM_HEADER_SIZE + XMODEM_BLOCK_DATA_SIZE, XMODEM_PADDING);
    block[0] = XMODEM_STX;
    block[1] = number;
    block[2] = static_cast<uint8_t>(~number);
    std::ranges::copy(chunk, block.begin() + XMODEM_HEADER_SIZE);
    const uint16_t crc = firmware::crc16Xmodem(std::span{block}.subspan(XMODEM_HEADER_SIZE));
    block.push_back(static_cast<uint8_t>(crc >> 8));
    block.push_back(static_cast<uint8_t>(crc & 0xFF));
    return block;
}

std::optional<size_t> packetSize(std::span<const uint8_t> header) {
    const size_t length = firmware::readLittleEndian16(header.subspan(PACKET_LENGTH_OFFSET));
    if (length == 0 || length > MAX_PACKET_LENGTH) {
        return std::nullopt;
    }
    return length + PACKET_OVERHEAD;
}

std::optional<Packet> parsePacket(std::span<const uint8_t> raw) {
    if (raw.size() < PACKET_HEADER_SIZE || raw[0] != PACKET_START) {
        return std::nullopt;
    }
    const std::optional<size_t> size = packetSize(raw);
    if (!size || raw.size() < *size) {
        return std::nullopt;
    }
    const std::span<const uint8_t> packet = raw.first(*size);
    const size_t crcOffset = *size - PACKET_TRAILER_SIZE;
    const uint16_t expected = firmware::readLittleEndian16(packet.subspan(crcOffset));
    if (packet.back() != PACKET_END || firmware::crc16Xmodem(packet.first(crcOffset)) != expected) {
        return std::nullopt;
    }
    const std::span<const uint8_t> data = packet.subspan(PACKET_HEADER_SIZE, crcOffset - PACKET_HEADER_SIZE);
    return Packet{
        .seq = packet[PACKET_SEQ_OFFSET],
        .target = packet[PACKET_TARGET_OFFSET],
        .command = packet[PACKET_COMMAND_OFFSET],
        .data = firmware::Bytes(data.begin(), data.end()),
        .raw = firmware::Bytes(packet.begin(), packet.end()),
    };
}

} // namespace tapin::console
