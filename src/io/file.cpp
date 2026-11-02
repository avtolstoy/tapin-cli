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

#include "io/file.h"

#include <format>
#include <fstream>
#include <iterator>

namespace tapin::io {

Result<std::vector<uint8_t>> readWholeFile(const std::string& path) {
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        return fail(ErrorCode::NotFound, std::format("cannot read {}", path));
    }
    std::vector<uint8_t> bytes(std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{});
    if (file.bad()) {
        return fail(ErrorCode::IoFailed, std::format("cannot read all of {}", path));
    }
    return bytes;
}

Result<void> writeWholeFile(const std::string& path, std::span<const uint8_t> data) {
    std::ofstream file{path, std::ios::binary};
    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    file.close();
    if (!file) {
        return fail(ErrorCode::IoFailed, std::format("cannot write {}", path));
    }
    return {};
}

} // namespace tapin::io
