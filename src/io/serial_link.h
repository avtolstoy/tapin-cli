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
#include "core/error.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace tapin::io {

class IoLoop;

// One USB serial port at 115200 8N1. Exclusive, with DTR and RTS turned off.
class SerialLink {
public:
    static Result<std::unique_ptr<SerialLink>> open(IoLoop& io, const std::string& portName);
    ~SerialLink();

    SerialLink(const SerialLink&) = delete;
    SerialLink& operator=(const SerialLink&) = delete;

    Async<void> write(std::span<const uint8_t> data);

    // The next byte, or nothing if none arrived in time.
    Async<std::optional<uint8_t>> readByte(std::chrono::milliseconds limit);

    // Drops everything received so far.
    void discardInput();

private:
    struct Impl;
    explicit SerialLink(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_; // The port and bytes read but not handed out yet
};

} // namespace tapin::io
