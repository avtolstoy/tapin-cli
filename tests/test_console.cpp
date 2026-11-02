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

#if !defined(_WIN32)

#include "console/console.h"
#include "fake_console.h"
#include "firmware/image.h"
#include "io/interrupt_watch.h"
#include "io/loop.h"
#include "testing.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

using namespace tapin;
using namespace tapin::console;
using test::FakeConsole;
using test::openConsoleOn;
using namespace std::chrono_literals;

namespace {

constexpr size_t LARGE_IMAGE_PAYLOAD{200 * 1024}; // Enough blocks that Ctrl-C lands in the middle

bool sawCommand(const FakeConsole& fake, Command command) {
    return std::ranges::find(fake.commands, static_cast<uint8_t>(command)) != fake.commands.end();
}

} // namespace

TEST_CASE("a lens swap recovers without a replug") {
    FakeConsole fake;
    fake.connected = true;
    fake.stale = true;
    io::IoLoop io;
    EXPECT_VALUE(console, openConsoleOn(io, fake));
    const Result<LensInfo> lens = io.waitFor(console->connectLens());
    fake.stop();
    EXPECT(lens.has_value());
    EXPECT(lens.transform([](const LensInfo& info) { return info.model; }) == std::string{"F013"});
    EXPECT(sawCommand(fake, Command::Disconnect));
}

TEST_CASE("a lens that never answers asks for a replug") {
    FakeConsole fake;
    fake.connected = true;
    fake.stale = true;
    fake.lensDead = true;
    io::IoLoop io;
    EXPECT_VALUE(console, openConsoleOn(io, fake));
    const Result<LensInfo> lens = io.waitFor(console->connectLens());
    EXPECT(test::failedWith(lens, ErrorCode::LensNotReady));
    EXPECT(test::failedSaying(lens, "Unplug"));
}

TEST_CASE("no lens on the console is its own error") {
    FakeConsole fake;
    fake.mounted = false;
    io::IoLoop io;
    EXPECT_VALUE(console, openConsoleOn(io, fake));
    EXPECT(test::failedSaying(io.waitFor(console->connectLens()), "no lens"));
}

TEST_CASE("overcurrent stops at once, without reconnecting") {
    FakeConsole fake;
    fake.overcurrent = true;
    io::IoLoop io;
    EXPECT_VALUE(console, openConsoleOn(io, fake));
    const Result<LensInfo> lens = io.waitFor(console->connectLens());
    fake.stop();
    EXPECT(test::failedWith(lens, ErrorCode::Overcurrent));
    EXPECT_FALSE(sawCommand(fake, Command::Disconnect));
}

TEST_CASE("an F8 rejection that is not overcurrent does not stop the lens read") {
    FakeConsole fake;
    fake.connectRejectCode = uint8_t{0x00};
    io::IoLoop io;
    EXPECT_VALUE(console, openConsoleOn(io, fake));
    EXPECT(io.waitFor(console->connectLens()).has_value());
}

TEST_CASE("overcurrent on F8 stops at once") {
    FakeConsole fake;
    fake.connectRejectCode = uint8_t{0x20};
    io::IoLoop io;
    EXPECT_VALUE(console, openConsoleOn(io, fake));
    EXPECT(test::failedWith(io.waitFor(console->connectLens()), ErrorCode::Overcurrent));
}

TEST_CASE("an image goes over intact") {
    FakeConsole fake;
    fake.connected = true;
    io::IoLoop io;
    EXPECT_VALUE(console, openConsoleOn(io, fake));
    EXPECT_VALUE(image, firmware::FirmwareImage::parse(test::makeImageBytes("F013E0", 4)));
    const io::InterruptWatch interrupt{io};
    const Result<void> sent = io.waitFor(console->transferImage(image, interrupt));
    fake.stop();
    EXPECT(sent.has_value());
    EXPECT(fake.received == image.bytes());
    EXPECT(fake.ends == 1);
}

TEST_CASE("a two record image goes over in file order, reconnecting after each record") {
    FakeConsole fake;
    fake.connected = true;
    io::IoLoop io;
    EXPECT_VALUE(console, openConsoleOn(io, fake));
    EXPECT_VALUE(image, firmware::FirmwareImage::parse(test::makeTwoRecordImageBytes("A025E0", 2)));
    EXPECT(image.records().size() == 2uz);
    const io::InterruptWatch interrupt{io};
    const Result<void> sent = io.waitFor(console->transferImage(image, interrupt));
    fake.stop();
    EXPECT(sent.has_value());
    EXPECT(fake.received == image.bytes());
    EXPECT(fake.ends == 2);
    const std::vector<std::array<uint8_t, 2>> expectedRequests{{0, 0}, {1, 0}};
    EXPECT(fake.updateRequests == expectedRequests);
    const std::vector<uint8_t> expectedFirstBlocks{1, 1};
    EXPECT(fake.firstBlockNumbers == expectedFirstBlocks);
    const auto byteOf = [](Command command) {
        return static_cast<uint8_t>(command);
    };
    const std::vector<uint8_t> expectedCommands{
        byteOf(Command::Update),
        byteOf(Command::Disconnect),
        byteOf(Command::Connect),
        byteOf(Command::Update),
        byteOf(Command::Disconnect),
        byteOf(Command::Connect),
    };
    EXPECT(fake.commands == expectedCommands);
}

TEST_CASE("a failure on the second record cancels it and reports the failure") {
    constexpr int BLOCKS_IN_FIRST_RECORD{5};
    FakeConsole fake;
    fake.connected = true;
    fake.nakFromBlock = BLOCKS_IN_FIRST_RECORD;
    io::IoLoop io;
    EXPECT_VALUE(console, openConsoleOn(io, fake));
    EXPECT_VALUE(image, firmware::FirmwareImage::parse(test::makeTwoRecordImageBytes("A025E0", 2)));
    const io::InterruptWatch interrupt{io};
    const Result<void> sent = io.waitFor(console->transferImage(image, interrupt));
    fake.stop();
    EXPECT_FALSE(sent.has_value());
    EXPECT(fake.ends == 1);
    EXPECT(fake.cancels == 1);
    EXPECT(fake.updateRequests.size() == 2uz);
}

TEST_CASE("a NAKed block is sent again") {
    FakeConsole fake;
    fake.connected = true;
    fake.nakCount = 2;
    io::IoLoop io;
    EXPECT_VALUE(console, openConsoleOn(io, fake));
    EXPECT_VALUE(image, firmware::FirmwareImage::parse(test::makeImageBytes("F013E0", 4)));
    const io::InterruptWatch interrupt{io};
    const Result<void> sent = io.waitFor(console->transferImage(image, interrupt));
    fake.stop();
    EXPECT(sent.has_value());
    EXPECT(fake.received == image.bytes());
}

TEST_CASE("too many NAKs cancel the update") {
    FakeConsole fake;
    fake.connected = true;
    fake.nakCount = 100;
    io::IoLoop io;
    EXPECT_VALUE(console, openConsoleOn(io, fake));
    EXPECT_VALUE(image, firmware::FirmwareImage::parse(test::makeImageBytes("F013E0", 4)));
    const io::InterruptWatch interrupt{io};
    const Result<void> sent = io.waitFor(console->transferImage(image, interrupt));
    fake.stop();
    EXPECT_FALSE(sent.has_value());
    EXPECT(fake.cancels == 1);
}

TEST_CASE("Ctrl-C cancels the update and reconnects the lens") {
    FakeConsole fake;
    fake.connected = true;
    fake.interruptAfterBlocks = 1;
    io::IoLoop io;
    EXPECT_VALUE(console, openConsoleOn(io, fake));
    EXPECT_VALUE(image, firmware::FirmwareImage::parse(test::makeImageBytes("F013E0", 4, LARGE_IMAGE_PAYLOAD)));
    const io::InterruptWatch interrupt{io};
    const Result<void> sent = io.waitFor(console->transferImage(image, interrupt));
    fake.stop();
    EXPECT(test::failedWith(sent, ErrorCode::Interrupted));
    EXPECT(fake.cancels == 1);
    EXPECT(sawCommand(fake, Command::Disconnect));
}

TEST_CASE("an update request recovers from a lens swap") {
    FakeConsole fake;
    fake.connected = true;
    fake.stale = true;
    io::IoLoop io;
    EXPECT_VALUE(console, openConsoleOn(io, fake));
    EXPECT_VALUE(image, firmware::FirmwareImage::parse(test::makeImageBytes("F013E0", 4)));
    const io::InterruptWatch interrupt{io};
    const Result<void> sent = io.waitFor(console->transferImage(image, interrupt));
    fake.stop();
    EXPECT(sent.has_value());
    EXPECT(fake.ends == 1);
}

#endif
