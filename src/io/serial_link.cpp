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

#include "io/serial_link.h"

#include "io/asio.h"
#include "io/loop.h"

#include <array>
#include <cstddef>
#include <format>
#include <memory>
#include <string_view>
#include <system_error>
#include <utility>

#include <exec/asio/completion_token.hpp>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <termios.h>
#endif

namespace tapin::io {

namespace {

using namespace std::chrono_literals;

constexpr unsigned BAUD_RATE{115200};
constexpr unsigned CHARACTER_SIZE{8};
constexpr size_t READ_CHUNK_SIZE{2048}; // More than the largest reply from the console
constexpr auto WRITE_LIMIT = 2s;        // A write only gets stuck if the console went away

template <typename Option>
Result<void> setPortOption(asio::serial_port& port, const Option& option, std::string_view portName) {
    std::error_code error;
    port.set_option(option, error);
    if (error) {
        return fail(ErrorCode::IoFailed, std::format("cannot set up {}: {}", portName, error.message()));
    }
    return {};
}

} // namespace

struct SerialLink::Impl {
    explicit Impl(const asio::any_io_executor& executor)
            : port(executor) {
    }

    asio::serial_port port;                       // The console
    std::array<uint8_t, READ_CHUNK_SIZE> chunk{}; // Last bytes read from the port
    size_t chunkSize{};                           // How many of them are valid
    size_t chunkPosition{};                       // The next one to hand out
};

SerialLink::SerialLink(std::unique_ptr<Impl> impl)
        : impl_(std::move(impl)) {
}

SerialLink::~SerialLink() = default;

Result<std::unique_ptr<SerialLink>> SerialLink::open(IoLoop& io, const std::string& portName) {
    auto impl = std::make_unique<Impl>(io.executor());
    std::error_code error;
    impl->port.open(portName, error);
    if (error == std::errc::permission_denied) {
        return fail(ErrorCode::IoFailed, std::format("cannot open {}: {}. On Linux, see Linux permissions in the README.", portName, error.message()));
    }
    if (error) {
        return fail(ErrorCode::IoFailed, std::format("cannot open {}: {}. Is the TAP-in Utility or another tapin-cli still running?", portName, error.message()));
    }
    using Options = asio::serial_port_base;
    CHECK(setPortOption(impl->port, Options::baud_rate(BAUD_RATE), portName));
    CHECK(setPortOption(impl->port, Options::character_size(CHARACTER_SIZE), portName));
    CHECK(setPortOption(impl->port, Options::parity(Options::parity::none), portName));
    CHECK(setPortOption(impl->port, Options::stop_bits(Options::stop_bits::one), portName));
    CHECK(setPortOption(impl->port, Options::flow_control(Options::flow_control::none), portName));
#if !defined(_WIN32)
    // Windows ports are already exclusive, and Asio opens them with DTR and RTS off.
    const int descriptor = impl->port.native_handle();
    if (::ioctl(descriptor, TIOCEXCL) != 0) {
        return fail(ErrorCode::IoFailed, std::format("cannot get {} exclusively", portName));
    }
    int modemLines = TIOCM_DTR | TIOCM_RTS;
    (void)::ioctl(descriptor, TIOCMBIC, &modemLines); // A port without modem lines has nothing to clear
#endif
    return std::unique_ptr<SerialLink>{new SerialLink{std::move(impl)}};
}

Async<void> SerialLink::write(std::span<const uint8_t> data) {
    const auto deadline = asio::cancel_after(WRITE_LIMIT, exec::asio::completion_token);
    auto [error, written] = co_await asio::async_write(impl_->port, asio::buffer(data.data(), data.size()), deadline);
    if (error) {
        co_await fail(ErrorCode::IoFailed, std::format("serial write: {}", error.message()));
    }
}

Async<std::optional<uint8_t>> SerialLink::readByte(std::chrono::milliseconds limit) {
    while (impl_->chunkPosition == impl_->chunkSize) {
        const auto deadline = asio::cancel_after(limit, exec::asio::completion_token);
        auto [error, size] = co_await impl_->port.async_read_some(asio::buffer(impl_->chunk), deadline);
        if (error == asio::error::operation_aborted) {
            co_return std::nullopt;
        }
        if (error) {
            co_return fail(ErrorCode::IoFailed, std::format("serial read: {}", error.message()));
        }
        impl_->chunkSize = size;
        impl_->chunkPosition = 0;
    }
    co_return impl_->chunk[impl_->chunkPosition++];
}

void SerialLink::discardInput() {
    impl_->chunkSize = 0;
    impl_->chunkPosition = 0;
    // A byte this misses is stale, and callers drop those by their sequence number or by the block they wait for.
#if defined(_WIN32)
    (void)PurgeComm(impl_->port.native_handle(), PURGE_RXCLEAR);
#else
    (void)::tcflush(impl_->port.native_handle(), TCIFLUSH);
#endif
}

} // namespace tapin::io
