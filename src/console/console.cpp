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

#include "console/console.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <format>
#include <print>
#include <string>
#include <string_view>
#include <utility>

namespace tapin::console {

namespace {

constexpr int REQUEST_ATTEMPTS{3}; // Serial over USB rarely drops a packet, but the lens can be busy
constexpr int SINGLE_ATTEMPT{1};
constexpr int LENS_READ_ATTEMPTS{3};
constexpr int CONNECT_ATTEMPTS{2};
constexpr int VERSION_READ_ATTEMPTS{5}; // A lens that just rebooted takes a few seconds to answer
constexpr int MAX_FAILED_BLOCKS{5};
constexpr int END_ATTEMPTS{5};
constexpr auto CONNECT_RETRY = 500ms;
constexpr uint8_t MAX_SEQUENCE{255};
constexpr size_t PROGRESS_EVERY_BYTES{64 * 1024}; // Often enough to see it moving
constexpr uint8_t ERROR_BIT_OVERCURRENT{0x20};    // Only this bit means overcurrent
constexpr uint8_t ERROR_ALREADY_CONNECTED{0x10};
constexpr uint8_t REPLY_OK{0x01};
constexpr uint8_t ADJUST_WRITE_OK{0x00};
constexpr std::array<uint8_t, 1> CONNECT_DATA{0x00};
constexpr std::array<uint8_t, 1> XMODEM_CANCEL{static_cast<uint8_t>(XmodemControl::Cancel)};
constexpr std::array<uint8_t, 1> XMODEM_EOT{static_cast<uint8_t>(XmodemControl::EndOfTransmission)};
constexpr std::string_view REPLUG_HINT{"Unplug the console from USB, re-seat the lens, plug the console back in and try again."};

bool isOvercurrent(uint8_t code) {
    return (code & ERROR_BIT_OVERCURRENT) != 0;
}

Error rejection(Command command, uint8_t code) {
    if (isOvercurrent(code)) {
        return Error{ErrorCode::Overcurrent, std::format("console reports overcurrent (0x{:02X}), remove the lens and check the contacts", code)};
    }
    const auto commandByte = static_cast<uint8_t>(command);
    return Error{ErrorCode::IoFailed, std::format("console rejected command 0x{:02X} with code 0x{:02X}", commandByte, code)};
}

bool isXmodemControl(uint8_t byte) {
    switch (static_cast<XmodemControl>(byte)) {
    case XmodemControl::CrcStart:
    case XmodemControl::Ack:
    case XmodemControl::Nak:
    case XmodemControl::Cancel:
    case XmodemControl::EndOfTransmission:
        return true;
    }
    return false;
}

uint8_t rejectCodeOf(const Packet& packet) {
    return packet.data.empty() ? uint8_t{0} : packet.data.front();
}

} // namespace

Console::Console(io::IoLoop& io, std::unique_ptr<io::SerialLink> link, const ConsoleOptions& options)
        : io_(io),
          link_(std::move(link)),
          options_(options) {
}

Result<std::unique_ptr<Console>> Console::open(io::IoLoop& io, const std::string& portName, const ConsoleOptions& options) {
    auto link = CHECK(io::SerialLink::open(io, portName));
    link->discardInput();
    return std::unique_ptr<Console>{new Console{io, std::move(link), options}};
}

Console::RequestSettings Console::defaultRequest() const {
    return RequestSettings{options_.answerLimit, REQUEST_ATTEMPTS};
}

uint8_t Console::nextSequence() {
    seq_ = static_cast<uint8_t>(seq_ % MAX_SEQUENCE + 1);
    return seq_;
}

Async<void> Console::sendBytes(std::span<const uint8_t> data) {
    if (options_.debug) {
        std::println(stderr, "-> {}", hexText(data));
    }
    co_await link_->write(data);
}

Async<std::optional<uint8_t>> Console::readByteBefore(Deadline deadline) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) {
        co_return std::nullopt;
    }
    co_return co_await link_->readByte(remaining);
}

Async<std::optional<Packet>> Console::readPacketBefore(Deadline deadline) {
    firmware::Bytes buffer;
    while (true) {
        const std::optional<uint8_t> byte = co_await readByteBefore(deadline);
        if (!byte) {
            co_return std::nullopt;
        }
        buffer.push_back(*byte);
        while (!buffer.empty() && buffer.front() != PACKET_START) {
            buffer.erase(buffer.begin());
        }
        if (buffer.size() < PACKET_HEADER_SIZE) {
            continue;
        }
        const std::optional<size_t> size = packetSize(buffer);
        if (!size) {
            buffer.erase(buffer.begin());
            continue;
        }
        if (buffer.size() >= *size) {
            std::optional<Packet> packet = parsePacket(buffer);
            if (packet) {
                if (options_.debug) {
                    std::println(stderr, "<- {}", hexText(packet->raw));
                }
                co_return std::move(packet);
            }
            buffer.erase(buffer.begin());
        }
    }
}

Async<Console::Answer> Console::sendRequest(Target target, Command command, std::span<const uint8_t> data, RequestSettings settings) {
    const uint8_t seq = nextSequence();
    const firmware::Bytes request = buildPacket(target, command, data, seq);
    link_->discardInput();
    for (int attempt = 0; attempt < settings.attempts; ++attempt) {
        co_await sendBytes(request);
        const Deadline deadline = std::chrono::steady_clock::now() + settings.limit;
        while (true) {
            std::optional<Packet> packet = co_await readPacketBefore(deadline);
            if (!packet) {
                break;
            }
            // Sequence 0 is how the console answers some errors, anything else is stale.
            if (packet->seq != seq && packet->seq != 0) {
                continue;
            }
            if (packet->command == static_cast<uint8_t>(Command::Error)) {
                co_return Answer{AnswerKind::Rejected, {}, rejectCodeOf(*packet)};
            }
            if (packet->target == static_cast<uint8_t>(target) && packet->command == static_cast<uint8_t>(command)) {
                co_return Answer{AnswerKind::Reply, std::move(*packet), 0};
            }
        }
    }
    co_return Answer{};
}

Async<firmware::Bytes> Console::requestData(Target target, Command command, std::span<const uint8_t> data) {
    Answer answer = co_await sendRequest(target, command, data, defaultRequest());
    switch (answer.kind) {
    case AnswerKind::Reply:
        co_return std::move(answer.packet.data);
    case AnswerKind::Rejected:
        co_return std::unexpected{rejection(command, answer.rejectCode)};
    case AnswerKind::NoAnswer:
        break;
    }
    co_return fail(ErrorCode::IoFailed, std::format("console did not answer command 0x{:02X}", static_cast<uint8_t>(command)));
}

Async<AdapterInfo> Console::readAdapterInfo() {
    const firmware::Bytes data = co_await requestData(Target::Adapter, Command::BasicInfo, {});
    co_return co_await AdapterInfo::parse(data);
}

Async<bool> Console::isLensMounted() {
    // F7, F8 and F9 go to the adapter.
    const firmware::Bytes data = co_await requestData(Target::Adapter, Command::MountStatus, {});
    co_return !data.empty() && data.front() == REPLY_OK;
}

// Only overcurrent stops here. The lens read that follows tells whether anything else mattered.
Async<void> Console::sendConnect(RequestSettings settings) {
    const Answer answer = co_await sendRequest(Target::Adapter, Command::Connect, CONNECT_DATA, settings);
    if (answer.kind == AnswerKind::Rejected && isOvercurrent(answer.rejectCode)) {
        co_await std::unexpected{rejection(Command::Connect, answer.rejectCode)};
    }
}

Async<bool> Console::reconnectLens() {
    const RequestSettings quick{options_.quickLimit, SINGLE_ATTEMPT};
    co_await sendRequest(Target::Adapter, Command::Disconnect, {}, quick);
    co_await io_.sleepFor(options_.settleTime);
    for (int attempt = 0; attempt < CONNECT_ATTEMPTS; ++attempt) {
        const Answer answer = co_await sendRequest(Target::Adapter, Command::Connect, CONNECT_DATA, quick);
        const firmware::Bytes& reply = answer.packet.data;
        const bool connected = answer.kind == AnswerKind::Reply && !reply.empty() && reply.front() == REPLY_OK;
        if (connected || (answer.kind == AnswerKind::Rejected && answer.rejectCode == ERROR_ALREADY_CONNECTED)) {
            co_await io_.sleepFor(options_.settleTime);
            co_return true;
        }
        co_await io_.sleepFor(std::min<std::chrono::milliseconds>(CONNECT_RETRY, options_.settleTime));
    }
    co_return false;
}

Async<LensInfo> Console::connectLens() {
    if (!co_await isLensMounted()) {
        co_return fail(ErrorCode::LensNotReady, "no lens on the console");
    }
    co_await sendConnect(defaultRequest());
    for (int attempt = 0; attempt < LENS_READ_ATTEMPTS; ++attempt) {
        Answer answer = co_await sendRequest(Target::Lens, Command::BasicInfo, {}, defaultRequest());
        if (answer.kind == AnswerKind::Rejected && isOvercurrent(answer.rejectCode)) {
            co_return std::unexpected{rejection(Command::BasicInfo, answer.rejectCode)};
        }
        if (answer.kind == AnswerKind::Reply) {
            LensInfo info = co_await LensInfo::parse(answer.packet.data);
            if (!info.model.empty()) {
                co_return std::move(info);
            }
        }
        if (attempt + 1 < LENS_READ_ATTEMPTS) {
            std::println(stderr, "lens is not answering, reconnecting it...");
            co_await reconnectLens();
        }
    }
    co_return fail(ErrorCode::LensNotReady, std::format("lens is not answering. {}", REPLUG_HINT));
}

Async<firmware::Bytes> Console::readAdjustData() {
    firmware::Bytes data = co_await requestData(Target::Lens, Command::GetAdjust, {});
    if (data.size() != ADJUST_DATA_SIZE) {
        co_return fail(ErrorCode::IoFailed, std::format("adjustment data is {} bytes, expected {}", data.size(), ADJUST_DATA_SIZE));
    }
    co_return std::move(data);
}

Async<void> Console::writeAdjustData(std::span<const uint8_t> data) {
    if (data.size() != ADJUST_DATA_SIZE) {
        co_await fail(ErrorCode::InvalidArgument, std::format("adjustment data is {} bytes, expected {}", data.size(), ADJUST_DATA_SIZE));
    }
    const firmware::Bytes reply = co_await requestData(Target::Lens, Command::SetAdjust, data);
    if (reply.empty() || reply.front() != ADJUST_WRITE_OK) {
        co_await fail(ErrorCode::IoFailed, "the lens rejected the adjustment data");
    }
}

// An error packet during an update comes in place of a control byte. Only overcurrent ends the update, anything else
// counts as a NAK.
Async<std::optional<uint8_t>> Console::readErrorCodeBefore(Deadline deadline) {
    firmware::Bytes buffer{PACKET_START};
    std::optional<size_t> size;
    while (!size || buffer.size() < *size) {
        const std::optional<uint8_t> byte = co_await readByteBefore(deadline);
        if (!byte) {
            co_return std::nullopt;
        }
        buffer.push_back(*byte);
        if (!size && buffer.size() == PACKET_HEADER_SIZE) {
            size = packetSize(buffer);
            if (!size) {
                co_return std::nullopt;
            }
        }
    }
    const std::optional<Packet> packet = parsePacket(buffer);
    if (!packet || packet->command != static_cast<uint8_t>(Command::Error)) {
        co_return std::nullopt;
    }
    co_return rejectCodeOf(*packet);
}

Async<std::optional<XmodemControl>> Console::readXmodemControl(std::chrono::milliseconds limit) {
    const Deadline deadline = std::chrono::steady_clock::now() + limit;
    while (true) {
        const std::optional<uint8_t> byte = co_await readByteBefore(deadline);
        if (!byte) {
            co_return std::nullopt;
        }
        if (isXmodemControl(*byte)) {
            co_return static_cast<XmodemControl>(*byte);
        }
        if (*byte != PACKET_START) {
            continue;
        }
        const std::optional<uint8_t> code = co_await readErrorCodeBefore(deadline);
        if (!code) {
            continue;
        }
        if (isOvercurrent(*code)) {
            co_return std::unexpected{rejection(Command::Update, *code)};
        }
        co_return XmodemControl::Nak;
    }
}

Async<bool> Console::requestUpdate(const firmware::FirmwareRecord& record) {
    const std::array<uint8_t, 2> data{record.device, record.area};
    link_->discardInput();
    co_await sendBytes(buildPacket(Target::Lens, Command::Update, data, nextSequence()));
    co_return co_await readXmodemControl(options_.updateStartLimit) == XmodemControl::CrcStart;
}

Async<void> Console::sendXmodemBlocks(std::span<const uint8_t> data, const io::InterruptWatch& interrupt) {
    int failedBlocks = 0;
    size_t blockNumber = 0;
    for (size_t position = 0; position < data.size(); position += XMODEM_BLOCK_DATA_SIZE) {
        if (interrupt.interrupted()) {
            co_await fail(ErrorCode::Interrupted, "interrupted");
        }
        ++blockNumber;
        const size_t chunkSize = std::min(XMODEM_BLOCK_DATA_SIZE, data.size() - position);
        const firmware::Bytes block = buildXmodemBlock(data.subspan(position, chunkSize), blockNumber);
        while (true) {
            // Every earlier block was already answered, so anything left is stale and could look like this block's ACK.
            link_->discardInput();
            co_await sendBytes(block);
            const std::optional<XmodemControl> reply = co_await readXmodemControl(options_.blockLimit);
            if (reply == XmodemControl::Ack) {
                failedBlocks = 0;
                break;
            }
            if (reply == XmodemControl::Cancel) {
                co_await fail(ErrorCode::IoFailed, "console cancelled the update");
            }
            ++failedBlocks;
            if (failedBlocks > MAX_FAILED_BLOCKS) {
                co_await fail(ErrorCode::IoFailed, "too many failed blocks");
            }
        }
        const size_t sent = position + chunkSize;
        if (sent == data.size() || sent % PROGRESS_EVERY_BYTES < XMODEM_BLOCK_DATA_SIZE) {
            std::println("  sent {}/{} bytes", sent, data.size());
        }
    }
}

Async<void> Console::sendEndOfTransmission() {
    for (int attempt = 0; attempt < END_ATTEMPTS; ++attempt) {
        link_->discardInput();
        co_await sendBytes(XMODEM_EOT);
        const std::optional<XmodemControl> reply = co_await readXmodemControl(options_.blockLimit);
        if (reply == XmodemControl::Ack) {
            co_return;
        }
        if (reply == XmodemControl::Cancel) {
            co_await fail(ErrorCode::IoFailed, "console cancelled the update at the end");
        }
    }
    co_await fail(ErrorCode::IoFailed, "no ACK at the end of the update");
}

Async<void> Console::transferRecord(const firmware::FirmwareImage& image, const firmware::FirmwareRecord& record, const io::InterruptWatch& interrupt) {
    bool started = co_await requestUpdate(record);
    if (!started) {
        // The stale state from connectLens(), when --recovery went on without a readable lens.
        co_await reconnectLens();
        started = co_await requestUpdate(record);
    }
    if (!started) {
        co_await sendBytes(XMODEM_CANCEL);
        co_await fail(ErrorCode::LensNotReady, std::format("console did not start the update. {}", REPLUG_HINT));
    }
    Result<void> sent = co_await sendXmodemBlocks(image.recordBytes(record), interrupt).result();
    if (sent) {
        sent = co_await sendEndOfTransmission().result();
    }
    if (!sent) {
        co_await sendBytes(XMODEM_CANCEL);
        if (!co_await reconnectLens()) {
            std::println("warning: lens did not reconnect. {}", REPLUG_HINT);
        }
        co_await std::unexpected{sent.error()};
    }
    if (!co_await reconnectLens()) {
        co_await fail(ErrorCode::LensNotReady, std::format("lens did not reconnect after the update. {}", REPLUG_HINT));
    }
}

Async<void> Console::transferImage(const firmware::FirmwareImage& image, const io::InterruptWatch& interrupt) {
    for (const firmware::FirmwareRecord& record : image.records()) {
        std::println("sending device {} area {}, {} bytes", record.device, record.area, image.recordBytes(record).size());
        co_await transferRecord(image, record, interrupt);
    }
}

Async<LensInfo> Console::readLensAfterUpdate() {
    for (int attempt = 0; attempt < VERSION_READ_ATTEMPTS; ++attempt) {
        co_await io_.sleepFor(options_.settleTime);
        Answer answer = co_await sendRequest(Target::Lens, Command::BasicInfo, {}, defaultRequest());
        if (answer.kind == AnswerKind::Reply) {
            Result<LensInfo> info = LensInfo::parse(answer.packet.data);
            if (info) {
                co_return std::move(*info);
            }
        }
    }
    co_return fail(ErrorCode::LensNotReady, std::format("cannot read the lens after the update. {}", REPLUG_HINT));
}

} // namespace tapin::console
