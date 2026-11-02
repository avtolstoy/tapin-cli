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

#include "core/async.h"
#include "firmware/image.h"
#include "io/https.h"

#include <string>
#include <string_view>

namespace tapin::firmware {

// What Tamron's lensinfo XML says about the latest firmware of a lens.
struct LatestFirmware {
    std::string version;   // Four digits, like 0400
    std::string serverKey; // 64 hex characters
};

Result<LatestFirmware> parseLensInfo(std::string_view xml);

// The latest firmware of a lens, as Tamron names it. An image's own header can have an older minor version than this.
struct DownloadedImage {
    FirmwareImage image;
    std::string version; // Tamron's version, like 0400
    std::string name;    // Tamron's file name without its extension, like F013E0_0400
};

// F013 for f013: how Tamron writes a model.
std::string canonicalModelName(std::string_view model);

// The latest firmware of a lens from Tamron's server, decrypted. model is like F013.
Async<DownloadedImage> downloadLatestImage(io::HttpsClient& https, std::string model, Mount mount);

} // namespace tapin::firmware
