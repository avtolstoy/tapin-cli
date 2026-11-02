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

#include "flash/flasher.h"

#include "firmware/download.h"
#include "firmware/short_frame_patch.h"

#include <algorithm>
#include <format>
#include <print>
#include <string_view>
#include <utility>

namespace tapin::flash {

using firmware::Mount;

Async<std::optional<console::LensInfo>> Flasher::readLens(const FlashOptions& options) {
    Result<console::LensInfo> lens = co_await console_.connectLens().result();
    if (lens) {
        co_return std::move(*lens);
    }
    // Overcurrent can be a real electrical fault, so --recovery does not go through it.
    if (!options.recovery || lens.error().code == ErrorCode::Overcurrent) {
        co_return std::unexpected{lens.error()};
    }
    std::println("warning: {} Going on because of --recovery.", lens.error().message);
    co_return std::nullopt;
}

Async<PreparedUpdate> Flasher::prepareUpdate(io::HttpsClient& https, UpdateRequest request) {
    std::optional<console::LensInfo> lens = co_await readLens(request.options);
    if (!lens && !request.model) {
        co_return fail(ErrorCode::InvalidArgument, "cannot read the lens, say which one with --model");
    }
    const std::string model = request.model ? firmware::canonicalModelName(*request.model) : std::string{lens->baseModel()};
    if (lens && lens->baseModel() != model) {
        co_return fail(ErrorCode::WrongImage, std::format("the lens on the console is {}, not {}", lens->model, model));
    }
    // The console knows its mount with or without a lens on it.
    const std::optional<Mount> mount = (co_await console_.readAdapterInfo()).mount;
    if (!mount) {
        co_return fail(ErrorCode::IoFailed, "the console does not report a mount");
    }
    const bool patched = request.kind == FirmwareKind::Patched;
    if (patched && *mount != Mount::Canon) {
        co_return fail(ErrorCode::WrongImage, std::format("the patch is for Canon EF, this console has a {} mount", firmware::mountName(*mount)));
    }

    std::println("getting the latest {}{} firmware from Tamron...", model, firmware::mountCode(*mount));
    firmware::DownloadedImage downloaded = co_await firmware::downloadLatestImage(https, model, *mount);
    firmware::FirmwareImage image = std::move(downloaded.image);
    std::println("firmware {}, sha256 {}", downloaded.version, co_await image.sha256());
    if (patched) {
        firmware::PatchedImage patchedImage = co_await firmware::patchShortFrameChecks(image);
        if (!patchedImage.tested && !request.experimental) {
            co_return fail(ErrorCode::InvalidArgument, "nobody has tested this image on a camera. --experimental goes ahead anyway, `tapin-cli update` goes back");
        }
        std::println("patched, sha256 {}", patchedImage.sha256);
        std::println("status: {}", patchedImage.status);
        image = std::move(patchedImage.image);
    }
    if (lens && lens->firmwareText() != image.versionText()) {
        std::println("this changes the lens firmware from {} to {}", lens->firmwareText(), image.versionText());
    } else if (lens) {
        // A patched image reports the same version as stock, so being on this version says nothing.
        std::println("the lens already reports {}, this flashes it again", lens->firmwareText());
    }
    co_return PreparedUpdate{std::move(image), std::move(lens)};
}

// A lens that answers can always be checked. --recovery only helps when it can't.
Async<void> Flasher::checkImageFitsLens(const firmware::FirmwareImage& image, const console::LensInfo& lens, const FlashOptions& options) {
    const console::AdapterInfo adapter = co_await console_.readAdapterInfo();
    std::string problem = "the console does not report a mount, so this image can't be checked against the lens";
    if (adapter.mount) {
        const std::string marker = std::format("{}{}", lens.baseModel(), firmware::mountCode(*adapter.mount));
        if (image.namesLens(marker)) {
            co_return;
        }
        problem = std::format("image does not contain {}, it is not for this lens", marker);
    }
    if (!options.force) {
        co_await fail(ErrorCode::WrongImage, std::format("{} (`tapin-cli flash --force` overrides)", problem));
    }
    std::println("warning: {}", problem);
}

Async<std::optional<firmware::Bytes>> Flasher::readAdjustBeforeFlash() {
    Result<firmware::Bytes> data = co_await console_.readAdjustData().result();
    if (!data) {
        std::println("warning: cannot read adjustment data: {}", data.error().message);
        co_return std::nullopt;
    }
    co_return std::move(*data);
}

Async<void> Flasher::writeAndCheckAdjust(std::span<const uint8_t> data) {
    co_await console_.writeAdjustData(data);
    const firmware::Bytes readBack = co_await console_.readAdjustData();
    if (!std::ranges::equal(readBack, data)) {
        co_await fail(ErrorCode::IoFailed, "it reads back different");
    }
}

// An update leaves the lens needing this record. Writing defaults would lose the VC mode and AF adjustment, so keep
// them unless the major version changes, the format might change with it.
Async<void> Flasher::writeAdjustAfterFlash(
    const firmware::FirmwareImage& image,
    const std::optional<console::LensInfo>& before,
    const console::LensInfo& after,
    const std::optional<firmware::Bytes>& saved,
    const FlashOptions& options
) {
    if (!saved && !options.reset) {
        std::println("warning: adjustment data left as it is, it could not be read before the update");
        co_return;
    }
    const bool sameMajor = before && before->mainFirmware[0] == image.mainRecord().versionMajor;
    const bool keep = saved && sameMajor && !options.reset;
    if (!keep) {
        std::println("writing default adjustment data");
    }
    const firmware::Bytes data = keep ? *saved : after.defaultAdjustData();
    const Result<void> written = co_await writeAndCheckAdjust(data).result();
    if (!written) {
        const std::string oldData = saved ? console::hexText(*saved) : "unknown";
        co_await fail(ErrorCode::IoFailed, std::format("the firmware went over, but the adjustment data did not: {}. The old data was: {}", written.error().message, oldData));
    }
    if (keep) {
        std::println("adjustment data restored");
    }
}

Async<void> Flasher::flashImage(const firmware::FirmwareImage& image, const FlashOptions& options, std::optional<console::LensInfo> lens) {
    // For the whole flash: killed after the transfer, the lens would never get its adjustment data back.
    const io::InterruptWatch interrupt{io_};
    if (!lens) {
        lens = co_await readLens(options);
    }
    std::optional<firmware::Bytes> savedAdjust;
    if (lens) {
        co_await checkImageFitsLens(image, *lens, options);
        std::println("lens: {} {}, firmware {}", lens->model, lens->serialNumber, lens->firmwareText());
        savedAdjust = co_await readAdjustBeforeFlash();
    }

    co_await console_.transferImage(image, interrupt);

    // A failure even with --recovery: nothing shows the lens took the firmware.
    Result<console::LensInfo> after = co_await console_.readLensAfterUpdate().result();
    if (!after) {
        co_await fail(after.error().code, std::format("the firmware went over, but {}", after.error().message));
    }
    // The major version only. It can't tell a patched image from stock: they report the same version, and the lens has
    // no command to read firmware back.
    if (after->mainFirmware[0] != image.mainRecord().versionMajor) {
        co_await fail(ErrorCode::IoFailed, std::format("lens reports firmware {}, expected {}", after->firmwareText(), image.versionText()));
    }
    std::println("after update:\n{}", after->describe());
    co_await writeAdjustAfterFlash(image, lens, *after, savedAdjust, options);
}

} // namespace tapin::flash
