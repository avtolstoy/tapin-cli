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

#if !defined(_WIN32)

#include "fake_console.h"

#include "console/wire.h"
#include "testing.h"

#include <array>
#include <format>
#include <poll.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

namespace tapin::test {

namespace {

using console::Command;
using console::Target;
using console::XmodemControl;

constexpr int POLL_MILLISECONDS{5};
constexpr size_t PTY_NAME_SIZE{128};
constexpr size_t READ_CHUNK_SIZE{4096};
constexpr uint8_t REPLY_OK{1};
constexpr uint8_t ERROR_NOT_READY{0x00};
constexpr uint8_t ERROR_ALREADY_CONNECTED{0x10};
constexpr uint8_t ERROR_OVERCURRENT{0x20};
constexpr size_t XMODEM_HEADER_SIZE{3};
constexpr size_t XMODEM_CRC_SIZE{2};
constexpr size_t UPDATE_REQUEST_SIZE{2};

constexpr uint8_t controlByte(XmodemControl control) {
    return static_cast<uint8_t>(control);
}

} // namespace

FakeConsole::FakeConsole() {
    std::array<char, PTY_NAME_SIZE> name{};
    if (::openpty(&master_, &slave_, name.data(), nullptr, nullptr) != 0) {
        problems_.emplace_back("openpty failed");
        return;
    }
    termios settings{};
    ::tcgetattr(slave_, &settings);
    ::cfmakeraw(&settings);
    ::tcsetattr(slave_, TCSANOW, &settings);
    portName_ = name.data();
    adjust = firmware::Bytes{2, 3, 0, 0, 0, 1, 0, 0, 0, 1};
    adjust.resize(console::ADJUST_DATA_SIZE);
}

FakeConsole::~FakeConsole() {
    stop();
    ::close(slave_);
    ::close(master_);
    for (const std::string& problem : problems_) {
        SNITCH_FAIL_CHECK(problem);
    }
}

void FakeConsole::start() {
    thread_ = std::jthread{[this](std::stop_token stop) { serve(stop); }};
}

void FakeConsole::stop() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

void FakeConsole::serve(std::stop_token stop) {
    std::array<uint8_t, READ_CHUNK_SIZE> chunk{};
    while (!stop.stop_requested()) {
        pollfd descriptor{master_, POLLIN, 0};
        if (::poll(&descriptor, 1, POLL_MILLISECONDS) <= 0) {
            continue;
        }
        const ssize_t size = ::read(master_, chunk.data(), chunk.size());
        for (ssize_t index = 0; index < size; ++index) {
            receive(chunk[static_cast<size_t>(index)]);
        }
    }
}

void FakeConsole::sendBytes(const firmware::Bytes& bytes) {
    if (::write(master_, bytes.data(), bytes.size()) < 0) {
        problems_.emplace_back("write to the pty failed");
    }
}

void FakeConsole::receiveBlockByte(uint8_t byte) {
    incoming_.push_back(byte);
    if (incoming_.size() < console::XMODEM_BLOCK_SIZE) {
        return;
    }
    const firmware::Bytes block = std::move(incoming_);
    incoming_.clear();
    if (awaitingFirstBlock_) {
        firstBlockNumbers.push_back(block[1]);
        awaitingFirstBlock_ = false;
    }
    const bool nakThisOne = nakCount > 0 || (nakFromBlock >= 0 && blocks_ >= nakFromBlock);
    if (nakThisOne) {
        nakCount = nakCount > 0 ? nakCount - 1 : 0;
        sendBytes({controlByte(XmodemControl::Nak)});
        return;
    }
    received.insert(received.end(), block.begin() + XMODEM_HEADER_SIZE, block.end() - XMODEM_CRC_SIZE);
    ++blocks_;
    if (blocks_ == interruptAfterBlocks) {
        ::raise(SIGINT);
    }
    sendBytes({controlByte(XmodemControl::Ack)});
}

void FakeConsole::receive(uint8_t byte) {
    if (incoming_.empty() && byte == controlByte(XmodemControl::Cancel)) {
        ++cancels;
        updating_ = false;
        return;
    }
    if (updating_ && incoming_.empty() && byte == controlByte(XmodemControl::EndOfTransmission)) {
        ++ends;
        updating_ = false;
        if (received.size() > firmware::RECORD_VERSION_OFFSET && !keepsOldFirmware) {
            firmwareMajor = received[firmware::RECORD_VERSION_OFFSET];
        }
        if (lensRecoversAfterUpdate) {
            lensDead = false;
            deadStartsUpdate = false;
        }
        sendBytes({controlByte(XmodemControl::Ack)});
        return;
    }
    if (updating_) {
        receiveBlockByte(byte);
        return;
    }
    incoming_.push_back(byte);
    if (incoming_.size() < console::PACKET_HEADER_SIZE) {
        return;
    }
    const std::optional<size_t> size = console::packetSize(incoming_);
    if (size && incoming_.size() >= *size) {
        const firmware::Bytes raw = std::move(incoming_);
        incoming_.clear();
        answer(raw);
    }
}

void FakeConsole::startUpdate(const console::Packet& request) {
    if (request.data.size() == UPDATE_REQUEST_SIZE) {
        updateRequests.push_back({request.data[0], request.data[1]});
    } else {
        problems_.emplace_back("an update request without a device and an area");
    }
    updating_ = true;
    awaitingFirstBlock_ = true;
    sendBytes({controlByte(XmodemControl::CrcStart)});
}

void FakeConsole::answer(const std::vector<uint8_t>& raw) {
    const std::optional<console::Packet> packet = console::parsePacket(raw);
    if (!packet) {
        problems_.emplace_back("got a packet that does not parse");
        return;
    }
    commands.push_back(packet->command);
    const auto target = static_cast<Target>(packet->target);
    const auto command = static_cast<Command>(packet->command);
    const auto reply = [&](const firmware::Bytes& data) {
        sendBytes(console::buildPacket(target, command, data, packet->seq));
    };
    const auto reject = [&](uint8_t code) {
        sendBytes(console::buildPacket(target, Command::Error, firmware::Bytes{code}, packet->seq));
    };

    if (command == Command::MountStatus) {
        reply({static_cast<uint8_t>(mounted ? REPLY_OK : 0)});
    } else if (command == Command::Connect) {
        if (connectRejectCode) {
            connected = true;
            stale = false;
            reject(*connectRejectCode);
        } else if (connected) {
            reject(ERROR_ALREADY_CONNECTED);
        } else {
            connected = true;
            stale = false;
            reply({REPLY_OK});
        }
    } else if (command == Command::Disconnect) {
        connected = false;
        reply({REPLY_OK});
    } else if (command == Command::BasicInfo && target == Target::Adapter) {
        reply(makeAdapterInfoBytes(mount));
    } else if (command == Command::Update && deadStartsUpdate) {
        startUpdate(*packet);
    } else if (stale || lensDead || !connected) {
        reject(ERROR_NOT_READY);
    } else if (command == Command::BasicInfo) {
        if (overcurrent) {
            reject(ERROR_OVERCURRENT);
            return;
        }
        reply(makeLensInfoBytes(model, firmwareMajor));
    } else if (command == Command::GetAdjust) {
        if (adjustReadFails) {
            reject(ERROR_NOT_READY);
            return;
        }
        reply(adjust);
    } else if (command == Command::SetAdjust) {
        if (adjustWriteFails) {
            reject(ERROR_NOT_READY);
            return;
        }
        if (!adjustWriteIgnored) {
            adjust = packet->data;
        }
        reply({0});
    } else if (command == Command::Update) {
        startUpdate(*packet);
    } else {
        problems_.push_back(std::format("got an unexpected command 0x{:02X}", packet->command));
    }
}

Result<std::unique_ptr<console::Console>> openConsoleOn(io::IoLoop& io, FakeConsole& fake) {
    fake.start();
    return console::Console::open(io, fake.portName(), FAST_OPTIONS);
}

} // namespace tapin::test

#endif
