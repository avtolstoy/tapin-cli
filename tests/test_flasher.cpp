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
#include "flash/flasher.h"
#include "io/loop.h"
#include "testing.h"

#include <cstdint>

using namespace tapin;
using test::FakeConsole;

namespace {

constexpr uint8_t KEPT_VC_MODE{3};
constexpr uint8_t DEFAULT_VC_MODE{2};
constexpr uint8_t NIKON_MOUNT{2};
constexpr size_t ADJUST_VC_MODE_OFFSET{1};

Result<void> flashWith(FakeConsole& fake, const firmware::Bytes& bytes, const flash::FlashOptions& options) {
    io::IoLoop io;
    auto console = CHECK(test::openConsoleOn(io, fake));
    flash::Flasher flasher{*console, io};
    const firmware::FirmwareImage image = CHECK(firmware::FirmwareImage::parse(bytes));
    const Result<void> result = io.waitFor(flasher.flashImage(image, options, std::nullopt));
    fake.stop();
    return result;
}

} // namespace

TEST_CASE("flashing keeps the adjustment data") {
    FakeConsole fake;
    EXPECT(flashWith(fake, test::makeImageBytes("F013E0", 4), {}).has_value());
    EXPECT(fake.adjust[ADJUST_VC_MODE_OFFSET] == KEPT_VC_MODE);
    EXPECT(fake.ends == 1);
}

TEST_CASE("--reset writes the defaults") {
    FakeConsole fake;
    EXPECT(flashWith(fake, test::makeImageBytes("F013E0", 4), flash::FlashOptions{.reset = true}).has_value());
    EXPECT(fake.adjust[ADJUST_VC_MODE_OFFSET] == DEFAULT_VC_MODE);
}

TEST_CASE("a new major version writes the defaults") {
    FakeConsole fake;
    EXPECT(flashWith(fake, test::makeImageBytes("F013E0", 5), {}).has_value());
    EXPECT(fake.adjust[ADJUST_VC_MODE_OFFSET] == DEFAULT_VC_MODE);
}

TEST_CASE("adjustment data that can't be read is left alone") {
    FakeConsole fake;
    fake.adjustReadFails = true;
    EXPECT(flashWith(fake, test::makeImageBytes("F013E0", 4), {}).has_value());
    EXPECT(fake.adjust[ADJUST_VC_MODE_OFFSET] == KEPT_VC_MODE);
}

TEST_CASE("a Nikon lens on a Nikon console updates like any other") {
    FakeConsole fake;
    fake.mount = NIKON_MOUNT;
    fake.model = "A025";
    EXPECT(flashWith(fake, test::makeImageBytes("A025N0", 4), {}).has_value());
    EXPECT(fake.ends == 1);
}

TEST_CASE("a Canon image is refused on a Nikon console") {
    FakeConsole fake;
    fake.mount = NIKON_MOUNT;
    fake.model = "A025";
    EXPECT(test::failedWith(flashWith(fake, test::makeImageBytes("A025E0", 4), {}), ErrorCode::WrongImage));
}

TEST_CASE("an image for another lens is refused") {
    FakeConsole fake;
    EXPECT(test::failedWith(flashWith(fake, test::makeImageBytes("F016E0", 4), {}), ErrorCode::WrongImage));
    EXPECT(fake.received.empty());
}

TEST_CASE("--force flashes an image for another lens") {
    FakeConsole fake;
    EXPECT(flashWith(fake, test::makeImageBytes("F016E0", 4), flash::FlashOptions{.force = true}).has_value());
    EXPECT(fake.ends == 1);
}

TEST_CASE("--recovery does not skip the wrong lens check on a lens that answers") {
    FakeConsole fake;
    EXPECT(test::failedWith(flashWith(fake, test::makeImageBytes("F016E0", 4), flash::FlashOptions{.recovery = true}), ErrorCode::WrongImage));
}

TEST_CASE("--recovery flashes a lens that does not answer, and it works if the lens comes back") {
    FakeConsole fake;
    fake.connected = true;
    fake.stale = true;
    fake.lensDead = true;
    fake.deadStartsUpdate = true;
    fake.lensRecoversAfterUpdate = true;
    EXPECT(flashWith(fake, test::makeImageBytes("F013E0", 4), flash::FlashOptions{.recovery = true}).has_value());
    EXPECT(fake.ends == 1);
    EXPECT(fake.adjust[ADJUST_VC_MODE_OFFSET] == KEPT_VC_MODE);
}

TEST_CASE("--recovery with --reset writes the defaults once the lens comes back") {
    FakeConsole fake;
    fake.connected = true;
    fake.stale = true;
    fake.lensDead = true;
    fake.deadStartsUpdate = true;
    fake.lensRecoversAfterUpdate = true;
    const flash::FlashOptions options{.recovery = true, .reset = true};
    EXPECT(flashWith(fake, test::makeImageBytes("F013E0", 4), options).has_value());
    EXPECT(fake.adjust[ADJUST_VC_MODE_OFFSET] == DEFAULT_VC_MODE);
}

TEST_CASE("--recovery is not a success if the lens stays dead after the flash") {
    FakeConsole fake;
    fake.connected = true;
    fake.stale = true;
    fake.lensDead = true;
    fake.deadStartsUpdate = true;
    const Result<void> result = flashWith(fake, test::makeImageBytes("F013E0", 4), flash::FlashOptions{.recovery = true});
    EXPECT(test::failedSaying(result, "went over"));
    EXPECT(fake.ends == 1);
}

TEST_CASE("a lens reporting the wrong major version after a flash is a failure, and adjustment data is not written") {
    FakeConsole fake;
    fake.keepsOldFirmware = true;
    const Result<void> result = flashWith(fake, test::makeImageBytes("F013E0", 5), {});
    EXPECT(test::failedSaying(result, "lens reports firmware 0400, expected 0500"));
    EXPECT(fake.adjust[ADJUST_VC_MODE_OFFSET] == KEPT_VC_MODE);
}

TEST_CASE("a two record image flashes, and the main record's version is what gets checked") {
    FakeConsole fake;
    const firmware::Bytes bytes = test::makeTwoRecordImageBytes("F013E0", 4);
    EXPECT(flashWith(fake, bytes, {}).has_value());
    EXPECT(fake.received == bytes);
    EXPECT(fake.ends == 2);
    EXPECT(fake.adjust[ADJUST_VC_MODE_OFFSET] == KEPT_VC_MODE);
}

TEST_CASE("adjustment data the lens refuses is a failure that shows the old data") {
    FakeConsole fake;
    fake.adjustWriteFails = true;
    const Result<void> result = flashWith(fake, test::makeImageBytes("F013E0", 4), {});
    EXPECT(test::failedSaying(result, "The old data was: 02 03 00"));
    EXPECT(fake.ends == 1);
}

TEST_CASE("adjustment data that reads back different is a failure") {
    FakeConsole fake;
    fake.adjustWriteIgnored = true;
    const Result<void> result = flashWith(fake, test::makeImageBytes("F013E0", 4), flash::FlashOptions{.reset = true});
    EXPECT(test::failedSaying(result, "reads back different"));
}

TEST_CASE("--recovery does not flash through overcurrent") {
    FakeConsole fake;
    fake.overcurrent = true;
    fake.deadStartsUpdate = true;
    EXPECT(test::failedWith(flashWith(fake, test::makeImageBytes("F013E0", 4), flash::FlashOptions{.recovery = true}), ErrorCode::Overcurrent));
    EXPECT(fake.received.empty());
}

TEST_CASE("a lens without --recovery that does not answer stops before flashing") {
    FakeConsole fake;
    fake.connected = true;
    fake.stale = true;
    fake.lensDead = true;
    EXPECT_FALSE(flashWith(fake, test::makeImageBytes("F013E0", 4), {}).has_value());
    EXPECT(fake.received.empty());
}

#endif
