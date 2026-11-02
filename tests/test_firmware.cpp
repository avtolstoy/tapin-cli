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

#include "firmware/download.h"
#include "firmware/image.h"
#include "firmware/short_frame_patch.h"
#include "io/file.h"
#include "testing.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <utility>

using namespace tapin;
using namespace tapin::firmware;
using test::makeImageBytes;
using test::updateRecordCrc;

namespace {

constexpr size_t PAYLOAD_START{RECORD_HEADER_SIZE};
constexpr std::array<size_t, 4> CHECK_OFFSETS{0x100, 0x200, 0x300, 0x400};
constexpr size_t MN10300_BRANCH_IN_CODE{4};
constexpr size_t THUMB_BRANCH_IN_CODE{7};
constexpr uint8_t MN10300_BLS{0xC7};
constexpr uint8_t THUMB_BHI{0xD8};

// Real code shapes: the check, then a load of 8 on the path after it.
void addMn10300Check(Bytes& bytes, size_t offset) {
    constexpr std::array<uint8_t, 10> CODE{0xFA, 0xC8, 0x82, 0x00, 0xC8, 0x04, 0xCC, 0x00, 0x80, 0x08};
    std::ranges::copy(CODE, bytes.begin() + static_cast<std::ptrdiff_t>(PAYLOAD_START + offset));
}

void addThumbCheck(Bytes& bytes, size_t offset) {
    // cmp r0,#0xa ... cmp r0,#0x82 ; bne ... cmp r0,#8
    constexpr std::array<uint8_t, 10> CODE{0x0A, 0x28, 0x00, 0xBF, 0x82, 0x28, 0x0E, 0xD1, 0x08, 0x28};
    std::ranges::copy(CODE, bytes.begin() + static_cast<std::ptrdiff_t>(PAYLOAD_START + offset));
}

Bytes startProgram(Bytes bytes) {
    constexpr std::string_view START{"START PROGRAM"};
    std::ranges::copy(START, bytes.begin() + PAYLOAD_START);
    return bytes;
}

// What a patch must change and nothing more: these branch bytes, and the CRC that follows from them.
Bytes withBranches(Bytes bytes, size_t branchInCode, uint8_t branch) {
    for (size_t offset : CHECK_OFFSETS) {
        bytes[PAYLOAD_START + offset + branchInCode] = branch;
    }
    updateRecordCrc(bytes);
    return bytes;
}

} // namespace

TEST_CASE("crc16Xmodem gives the catalogue's check value") {
    constexpr std::array<uint8_t, 9> TEXT{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    EXPECT(crc16Xmodem(TEXT) == 0x31C3);
}

TEST_CASE("a mount has a name for people and a code for file names") {
    EXPECT(mountCode(Mount::Nikon) == "N0");
    EXPECT(mountFromName("sony") == Mount::Sony);
    EXPECT(mountFromName("Canon") == Mount::Canon);
    EXPECT(test::failedWith(mountFromName("E0"), ErrorCode::InvalidArgument));
}

TEST_CASE("the key from a file name repeats the name") {
    EXPECT_VALUE(key, keyFromNameParts("F013", "E0", "0400"));
    EXPECT(key[0] == 'F');
    EXPECT(key[8] == '0');
    EXPECT(key[9] == 'F');
}

TEST_CASE("the key from a version above 9 keeps all its digits") {
    EXPECT_VALUE(key, keyFromNameParts("F013", "E0", "1000"));
    EXPECT(key[6] == '1');
    EXPECT(key[10] == 'F');
}

TEST_CASE("empty name parts have no key") {
    EXPECT(test::failedWith(keyFromNameParts("", "", ""), ErrorCode::InvalidArgument));
}

TEST_CASE("the key from an image name matches the key from its parts") {
    EXPECT_VALUE(fromName, keyFromImageName("F013E0_0400"));
    EXPECT_VALUE(fromParts, keyFromNameParts("F013", "E0", "0400"));
    EXPECT(fromName == fromParts);
}

TEST_CASE("a name not like Tamron's has no key") {
    EXPECT_FALSE(keyFromImageName("lens").has_value());
    EXPECT_FALSE(keyFromImageName("_0400").has_value());
    EXPECT_FALSE(keyFromImageName("F013E0_04").has_value());
}

TEST_CASE("an image with a bad CRC is rejected") {
    Bytes bytes = makeImageBytes("F013E0", 4);
    bytes[PAYLOAD_START] ^= 1;
    EXPECT_FALSE(FirmwareImage::parse(bytes).has_value());
}

TEST_CASE("an image with trailing bytes is rejected") {
    Bytes bytes = makeImageBytes("F013E0", 4);
    bytes.push_back(0);
    EXPECT_FALSE(FirmwareImage::parse(bytes).has_value());
}

TEST_CASE("decrypting undoes the XOR") {
    const Bytes plain = makeImageBytes("F013E0", 4);
    EXPECT_VALUE(key, keyFromNameParts("F013", "E0", "0400"));
    EXPECT_VALUE(image, decryptImage(test::encryptImage(plain, key), key));
    EXPECT(image.bytes() == plain);
}

TEST_CASE("MN10300 checks change from beq to bls, and only them and the CRC change") {
    Bytes bytes = startProgram(makeImageBytes("F013E0", 4));
    for (size_t offset : CHECK_OFFSETS) {
        addMn10300Check(bytes, offset);
    }
    updateRecordCrc(bytes);
    EXPECT_VALUE(stock, FirmwareImage::parse(bytes));
    EXPECT_VALUE(patched, patchShortFrameChecks(stock));
    EXPECT(patched.image.bytes() == withBranches(bytes, MN10300_BRANCH_IN_CODE, MN10300_BLS));
    EXPECT_FALSE(patched.tested);
    EXPECT(patched.status.contains("NOT TESTED"));
}

TEST_CASE("Cortex-M checks change from bne to bhi, and only them and the CRC change") {
    Bytes bytes = makeImageBytes("A025E0", 2);
    for (size_t offset : CHECK_OFFSETS) {
        addThumbCheck(bytes, offset);
    }
    updateRecordCrc(bytes);
    EXPECT_VALUE(stock, FirmwareImage::parse(bytes));
    EXPECT_VALUE(patched, patchShortFrameChecks(stock));
    EXPECT(patched.image.bytes() == withBranches(bytes, THUMB_BRANCH_IN_CODE, THUMB_BHI));
}

TEST_CASE("an image without exactly 4 checks can't be patched") {
    Bytes bytes = makeImageBytes("F017E0", 4);
    for (size_t offset : std::span{CHECK_OFFSETS}.first(3)) {
        addThumbCheck(bytes, offset);
    }
    updateRecordCrc(bytes);
    EXPECT_VALUE(stock, FirmwareImage::parse(bytes));
    EXPECT(test::failedWith(patchShortFrameChecks(stock), ErrorCode::NoPatchSites));
}

TEST_CASE("a check with no frame length after it does not count") {
    Bytes bytes = makeImageBytes("A025E0", 2);
    addThumbCheck(bytes, CHECK_OFFSETS[0]);
    bytes[PAYLOAD_START + CHECK_OFFSETS[0] + 8] = 0x00;
    EXPECT(findFrameLengthChecks(std::span<const uint8_t>{bytes}.subspan(PAYLOAD_START)).empty());
}

TEST_CASE("lensinfo gives the version and the key") {
    constexpr std::string_view XML{
        "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<lensinfo>20160418115300,A013,SP 45mm F/1.8 Di VC USD,0400,true,false,true,true,1,3,"
        "45,45,45,45,45,45,45,45,30,-30,0,1,1,1,1,1,"
        "035E52413C4040595F28645E5C29725543550D5548762B5346494032595E5D11</lensinfo>"
    };
    EXPECT_VALUE(latest, parseLensInfo(XML));
    EXPECT(latest.version == "0400");
    EXPECT(latest.serverKey.size() == 64);
}

TEST_CASE("lensinfo that is not what we expect is an error") {
    EXPECT_FALSE(parseLensInfo("not xml <<<").has_value());
    EXPECT_FALSE(parseLensInfo("<lensinfo>a,b</lensinfo>").has_value());
}

TEST_CASE("the server key of F013 decodes to the file name key") {
    EXPECT_VALUE(fromServer, keyFromServer("035E52413C4040595F28645E5C29725543550D5548762B5346494032595E5D11"));
    EXPECT_VALUE(fromName, keyFromNameParts("F013", "E0", "0400"));
    EXPECT(fromServer == fromName);
}

TEST_CASE("a model name is written the way Tamron does") {
    EXPECT(canonicalModelName("f013") == "F013");
}

TEST_CASE("the known stock images are marked as tested") {
    EXPECT(testedStatus("ff46a40b6ee363b4cb983340a2c61652c81e49a4ca35d74756e7af82d484e096").has_value());
    EXPECT_FALSE(testedStatus("0000").has_value());
}

// TAPIN_TEST_IMAGES is a directory of decrypted Tamron images.
TEST_CASE("real images patch to the expected hashes") {
    const char* directory = std::getenv("TAPIN_TEST_IMAGES");
    if (directory == nullptr) {
        SNITCH_SKIP_CHECK("TAPIN_TEST_IMAGES is not set");
        return;
    }
    struct Expected {
        std::string_view name;
        std::string_view patchedSha256;
    };
    constexpr std::array<Expected, 3> EXPECTED{{
        {"F012E0_0400", "7a355e2e213ec59b9d15bc53f7913b0aca6408408478ba844461bf7865e66b7e"},
        {"F013E0_0400", "a9ee5fe339819d7076ec2c08dafe16fa07b8d912445cb1a882a48228bcf453ba"},
        {"F016E0_0500", "8fc699853db6e8171f813bc061036ac4415b684d96a2ed651592e393e1d7a039"},
    }};
    for (const Expected& expected : EXPECTED) {
        EXPECT_VALUE(bytes, io::readWholeFile((std::filesystem::path{directory} / std::format("{}.dec", expected.name)).string()));
        EXPECT_VALUE(stock, FirmwareImage::parse(std::move(bytes)));
        EXPECT_VALUE(patched, patchShortFrameChecks(stock));
        EXPECT(patched.sha256 == expected.patchedSha256);
        EXPECT(patched.tested);
    }
    std::error_code error;
    std::filesystem::directory_iterator entry{directory, error};
    for (const std::filesystem::directory_iterator end; entry != end; entry.increment(error)) {
        const std::string name = entry->path().filename().string();
        if (entry->path().extension() != ".dec" || name.contains("N0_")) {
            continue;
        }
        EXPECT_VALUE(bytes, io::readWholeFile(entry->path().string()));
        EXPECT_VALUE(image, FirmwareImage::parse(std::move(bytes)));
        const size_t checks = findFrameLengthChecks(image.payloadOf(image.mainRecord())).size();
        EXPECT(checks == (name.starts_with("F017") ? 0uz : 4uz));
    }
}
