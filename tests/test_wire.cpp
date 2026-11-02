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
#include "testing.h"

#include <array>
#include <cstdint>
#include <optional>

using namespace tapin;
using namespace tapin::console;

TEST_CASE("a mount status request has the exact bytes the console expects") {
    const firmware::Bytes expected{0x0F, 0x01, 0x01, 0x00, 0x01, 0x00, 0xF7, 0xD0, 0xCD, 0xF0};
    EXPECT(buildPacket(Target::Adapter, Command::MountStatus, {}, 1) == expected);
}

TEST_CASE("bytes print as hex for people") {
    EXPECT(hexText(firmware::Bytes{0x0F, 0x01}) == "0F 01");
}

TEST_CASE("a packet survives a round trip") {
    const firmware::Bytes data{1, 2};
    const std::optional<Packet> packet = parsePacket(buildPacket(Target::Lens, Command::SetAdjust, data, 7));
    EXPECT(packet.has_value());
    if (packet) {
        EXPECT(packet->seq == 7);
        EXPECT(packet->command == 0xFB);
        EXPECT(packet->data == data);
    }
}

TEST_CASE("a packet with a bad CRC is rejected") {
    firmware::Bytes raw = buildPacket(Target::Lens, Command::SetAdjust, firmware::Bytes{1, 2}, 7);
    raw[7] ^= 1;
    EXPECT_FALSE(parsePacket(raw).has_value());
}

TEST_CASE("block numbers wrap at 255, not 256") {
    EXPECT(buildXmodemBlock({}, 254)[1] == 254);
    EXPECT(buildXmodemBlock({}, 255)[1] == 0);
    EXPECT(buildXmodemBlock({}, 255)[2] == 255);
    EXPECT(buildXmodemBlock({}, 256)[1] == 1);
}

TEST_CASE("a short block is padded to 1024 bytes") {
    const firmware::Bytes block = buildXmodemBlock(firmware::Bytes{'a', 'b'}, 1);
    EXPECT(block.size() == XMODEM_BLOCK_SIZE);
    EXPECT(block[3] == 'a');
    EXPECT(block[5] == 0x1A);
}
