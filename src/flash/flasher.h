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

#include "console/console.h"
#include "core/async.h"
#include "firmware/image.h"
#include "io/https.h"
#include "io/interrupt_watch.h"
#include "io/loop.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace tapin::flash {

struct FlashOptions {
    bool force{false};    // Flash an image made for another lens
    bool recovery{false}; // Go on when the lens does not answer, but not through overcurrent
    bool reset{false};    // Write default adjustment data instead of keeping it
};

enum class FirmwareKind {
    Stock,
    Patched,
};

// Which firmware a lens should get from Tamron's server.
struct UpdateRequest {
    FirmwareKind kind{FirmwareKind::Stock};
    std::optional<std::string> model; // Read from the lens when not given
    bool experimental{false};         // Allow a patched image nobody has tested on a camera
    FlashOptions options;
};

// An image ready to flash, and the lens it was chosen for.
struct PreparedUpdate {
    firmware::FirmwareImage image;
    std::optional<console::LensInfo> lens; // Nothing when --recovery went on without a readable lens
};

// One firmware update: choosing the image, the checks before it, the transfer, and the adjustment data around it.
class Flasher {
public:
    Flasher(console::Console& console, io::IoLoop& io)
            : console_(console),
              io_(io) {
    }

    Flasher(const Flasher&) = delete;
    Flasher& operator=(const Flasher&) = delete;

    // Reads the lens and the console, downloads the latest firmware and patches it if asked. Nothing is flashed yet.
    Async<PreparedUpdate> prepareUpdate(io::HttpsClient& https, UpdateRequest request);

    // lens is what the caller already read, or nothing to read it here.
    Async<void> flashImage(const firmware::FirmwareImage& image, const FlashOptions& options, std::optional<console::LensInfo> lens);

private:
    Async<std::optional<console::LensInfo>> readLens(const FlashOptions& options);
    Async<void> checkImageFitsLens(const firmware::FirmwareImage& image, const console::LensInfo& lens, const FlashOptions& options);
    Async<std::optional<firmware::Bytes>> readAdjustBeforeFlash();
    Async<void> writeAndCheckAdjust(std::span<const uint8_t> data);
    Async<void> writeAdjustAfterFlash(
        const firmware::FirmwareImage& image,
        const std::optional<console::LensInfo>& before,
        const console::LensInfo& after,
        const std::optional<firmware::Bytes>& saved,
        const FlashOptions& options
    );

    console::Console& console_; // Owned by the caller, open for the whole flash
    io::IoLoop& io_;            // Owned by the caller, for the Ctrl-C watch
};

} // namespace tapin::flash
