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

#if !defined(_WIN32)

#include "console/console.h"
#include "firmware/image.h"
#include "io/loop.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace tapin::test {

using namespace std::chrono_literals;

// Quick enough for a test, with room for a sanitizer build on a busy machine.
inline const console::ConsoleOptions FAST_OPTIONS{
    .answerLimit = 500ms,
    .quickLimit = 500ms,
    .settleTime = 1ms,
    .updateStartLimit = 500ms,
    .blockLimit = 1s,
};

// A TAP-in Console and lens on the other end of a pty, so tests run the real serial code. It runs on its own thread:
// set it up before start(), and read its results after stop(). Something it did not expect fails the test.
class FakeConsole {
public:
    FakeConsole();
    ~FakeConsole();

    FakeConsole(const FakeConsole&) = delete;
    FakeConsole& operator=(const FakeConsole&) = delete;

    void start();
    void stop();

    const std::string& portName() const {
        return portName_;
    }

    // Set up before start().
    std::string model{"F013"};
    uint8_t mount{1};
    uint8_t firmwareMajor{4};
    bool mounted{true};
    bool connected{false};
    bool stale{false};            // A lens went on or off, and the console has not been through F9 and F8
    bool lensDead{false};         // The lens fails requests even after F9 and F8
    bool deadStartsUpdate{false}; // The application is dead, but the bootloader still takes an update
    bool overcurrent{false};      // Lens info fails with overcurrent
    bool adjustReadFails{false};
    bool adjustWriteFails{false};
    bool adjustWriteIgnored{false};           // Says OK to a write and keeps the old data
    bool lensRecoversAfterUpdate{false};      // A lens that failed everything works once it has new firmware
    bool keepsOldFirmware{false};             // Takes an update and still reports the old version
    std::optional<uint8_t> connectRejectCode; // F8 fails with this code, and the lens connects anyway
    int nakCount{0};                          // How many blocks to NAK
    int nakFromBlock{-1};                     // NAKs every block from this one on, counted over all records
    int interruptAfterBlocks{-1};             // Raises SIGINT after this many blocks
    firmware::Bytes adjust;

    // Read after stop().
    std::vector<uint8_t> commands;
    std::vector<std::array<uint8_t, 2>> updateRequests; // Device and area of each update request
    std::vector<uint8_t> firstBlockNumbers;             // The block number each record started with
    firmware::Bytes received;
    int cancels{0};
    int ends{0};

private:
    void serve(std::stop_token stop);
    void receive(uint8_t byte);
    void receiveBlockByte(uint8_t byte);
    void answer(const std::vector<uint8_t>& raw);
    void sendBytes(const firmware::Bytes& bytes);
    void startUpdate(const console::Packet& request);

    int master_{-1};                    // Our end of the pty
    int slave_{-1};                     // Kept open so the port does not hang up between opens
    std::string portName_;              // The port the code under test opens
    std::jthread thread_;               // Runs serve()
    bool updating_{false};              // Between the update request and its end or cancel
    firmware::Bytes incoming_;          // Bytes of the packet or block being received
    int blocks_{0};                     // Blocks taken so far
    bool awaitingFirstBlock_{false};    // The next block is the first of its record
    std::vector<std::string> problems_; // What it did not expect, reported from the destructor on the test's thread
};

// Starts the fake and opens the real Console on it.
Result<std::unique_ptr<console::Console>> openConsoleOn(io::IoLoop& io, FakeConsole& fake);

} // namespace tapin::test

#endif
