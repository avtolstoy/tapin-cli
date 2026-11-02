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

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <format>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include <pugixml.hpp>

namespace tapin::firmware {

namespace {

constexpr std::string_view CDN_HOST{"tamron.cdngc.net"};
constexpr size_t VERSION_FIELD{3}; // Date, internal model, name, version, then lens settings; the key is last

// It ends up in a URL and in a file name.
Result<void> checkModelName(std::string_view model) {
    const auto isNameCharacter = [](char character) {
        return std::isalnum(static_cast<unsigned char>(character)) != 0;
    };
    if (model.empty() || !std::ranges::all_of(model, isNameCharacter)) {
        return fail(ErrorCode::InvalidArgument, std::format("{} is not a lens model, expected something like F013", model));
    }
    return {};
}

} // namespace

Result<LatestFirmware> parseLensInfo(std::string_view xml) {
    pugi::xml_document document;
    if (!document.load_buffer(xml.data(), xml.size())) {
        return fail(ErrorCode::InvalidArgument, "lensinfo is not valid XML");
    }
    const std::string_view text{document.child("lensinfo").child_value()};
    std::vector<std::string_view> fields;
    for (const auto field : text | std::views::split(',')) {
        fields.emplace_back(field);
    }
    if (fields.size() <= VERSION_FIELD + 1) {
        return fail(ErrorCode::InvalidArgument, "lensinfo has too few fields");
    }
    return LatestFirmware{std::string{fields[VERSION_FIELD]}, std::string{fields.back()}};
}

std::string canonicalModelName(std::string_view model) {
    std::string name{model};
    std::ranges::transform(name, name.begin(), [](unsigned char character) { return static_cast<char>(std::toupper(character)); });
    return name;
}

Async<DownloadedImage> downloadLatestImage(io::HttpsClient& https, std::string model, Mount mount) {
    co_await checkModelName(model);
    const std::string canonicalModel = canonicalModelName(model);
    const std::string_view code = mountCode(mount);
    const std::string lensInfo = co_await https.get(std::string{CDN_HOST}, std::format("/tapin/lens/lensinfo_{}{}.xml", canonicalModel, code));
    LatestFirmware latest = co_await parseLensInfo(lensInfo);
    const XorKey key = co_await keyFromServer(latest.serverKey);
    std::string name = std::format("{}{}_{}", canonicalModel, code, latest.version);
    const std::string tfwf = co_await https.get(std::string{CDN_HOST}, std::format("/tapin/lens/{}.tfwf", name));
    const auto* bytes = reinterpret_cast<const uint8_t*>(tfwf.data());
    FirmwareImage image = co_await decryptImage(std::span{bytes, tfwf.size()}, key);
    co_return DownloadedImage{std::move(image), std::move(latest.version), std::move(name)};
}

} // namespace tapin::firmware
