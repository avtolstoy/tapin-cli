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

#include "console/lens.h"
#include "console/wire.h"
#include "core/async.h"
#include "core/error.h"
#include "firmware/image.h"
#include "io/interrupt_watch.h"
#include "io/loop.h"
#include "io/serial_link.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace tapin::console {

using namespace std::chrono_literals;

struct ConsoleOptions {
    bool debug{false};                                // Hexdump all traffic to stderr
    std::chrono::milliseconds answerLimit = 5s;       // Waiting for an answer to a packet
    std::chrono::milliseconds quickLimit = 2s;        // The reconnect steps, so a dead lens can't block for a minute
    std::chrono::milliseconds settleTime = 1s;        // The lens needs this after F9 and after F8
    std::chrono::milliseconds updateStartLimit = 20s; // The console takes a while to enter update mode
    std::chrono::milliseconds blockLimit = 10s;       // Writing a block to flash can take seconds
};

// The TAP-in Console on a USB serial port, and the lens on it.
class Console {
public:
    static Result<std::unique_ptr<Console>> open(io::IoLoop& io, const std::string& portName, const ConsoleOptions& options);

    Console(const Console&) = delete;
    Console& operator=(const Console&) = delete;

    Async<AdapterInfo> readAdapterInfo();

    // Connects and reads the lens, including right after a lens swap. The console keeps its connected state when a
    // lens goes on or off while it stays powered: F8 then fails with 0x10 and the lens fails everything with 0x00,
    // until F9 and F8.
    Async<LensInfo> connectLens();

    Async<firmware::Bytes> readAdjustData();
    Async<void> writeAdjustData(std::span<const uint8_t> data);

    // Sends the image and reconnects the lens. Ctrl-C through the watch cancels it between blocks.
    Async<void> transferImage(const firmware::FirmwareImage& image, const io::InterruptWatch& interrupt);

    // Reads the lens after an update, which can take a few tries.
    Async<LensInfo> readLensAfterUpdate();

private:
    Console(io::IoLoop& io, std::unique_ptr<io::SerialLink> link, const ConsoleOptions& options);

    using Deadline = std::chrono::steady_clock::time_point;

    enum class AnswerKind {
        Reply,
        Rejected,
        NoAnswer,
    };

    struct Answer {
        AnswerKind kind{AnswerKind::NoAnswer}; // Which of the two below is valid
        Packet packet;                         // For Reply
        uint8_t rejectCode{};                  // For Rejected
    };

    struct RequestSettings {
        std::chrono::milliseconds limit; // Per attempt
        int attempts;                    // How many times to send the request
    };

    RequestSettings defaultRequest() const;
    uint8_t nextSequence();
    Async<void> sendBytes(std::span<const uint8_t> data);
    Async<std::optional<uint8_t>> readByteBefore(Deadline deadline);
    Async<std::optional<Packet>> readPacketBefore(Deadline deadline);
    Async<Answer> sendRequest(Target target, Command command, std::span<const uint8_t> data, RequestSettings settings);
    Async<firmware::Bytes> requestData(Target target, Command command, std::span<const uint8_t> data);
    Async<bool> reconnectLens();
    Async<void> sendConnect(RequestSettings settings);
    Async<bool> isLensMounted();

    Async<std::optional<XmodemControl>> readXmodemControl(std::chrono::milliseconds limit);
    Async<std::optional<uint8_t>> readErrorCodeBefore(Deadline deadline);
    Async<bool> requestUpdate(const firmware::FirmwareRecord& record);
    Async<void> sendXmodemBlocks(std::span<const uint8_t> data, const io::InterruptWatch& interrupt);
    Async<void> sendEndOfTransmission();
    Async<void> transferRecord(const firmware::FirmwareImage& image, const firmware::FirmwareRecord& record, const io::InterruptWatch& interrupt);

    io::IoLoop& io_;                       // Owned by the caller, outlives the console
    std::unique_ptr<io::SerialLink> link_; // The port
    ConsoleOptions options_;               // Timing and debug output
    uint8_t seq_{0};                       // Sequence number of the last request, never 0 after the first
};

} // namespace tapin::console
