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
#include "firmware/image.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Bodies ask for lens details with command 0x80. Old ones can send a short frame of 9 bytes, and the firmware only
// treats a frame as short when the body id is exactly 0x82, so it swallows the next commands. The check exists 4 times,
// 2 receiving and 2 sending. The patch makes all of them accept every id up to 0x82. That fixes the EOS 3, though nobody
// has captured what an EOS 3 actually sends.
namespace tapin::firmware {

// A patched image and what is known about it.
struct PatchedImage {
    FirmwareImage image;
    std::string sha256;
    std::string status; // For people: which bodies it was tried on
    bool tested;        // Someone has flashed this one and used it on a camera
};

// Where the checks are in a main processor payload, found by the shape of the code around them. No offsets, so this
// also works on firmware that did not exist when it was written.
std::vector<size_t> findFrameLengthChecks(std::span<const uint8_t> payload);

// Fails with NoPatchSites unless there are exactly 4 checks.
Result<PatchedImage> patchShortFrameChecks(const FirmwareImage& stock);

// Which bodies the patched form of a stock image was tried on, or nothing for an image nobody has tried.
std::optional<std::string_view> testedStatus(std::string_view stockSha256);

} // namespace tapin::firmware
