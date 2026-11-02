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

#include "io/https.h"

#include "io/asio.h"
#include "io/http_response.h"
#include "io/loop.h"
#include "io/system_roots.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <format>
#include <span>
#include <string_view>
#include <utility>

#include <asio/ssl.hpp>
#include <exec/asio/completion_token.hpp>

#if defined(__GLIBC__)
#include <nss.h>
#endif

namespace tapin::io {

namespace {

using namespace std::chrono_literals;

constexpr std::string_view HTTPS_PORT{"443"}; // Numeric: a service name would go through NSS
constexpr auto NETWORK_LIMIT = 60s;           // For each step: resolve, connect, TLS, write, one read
constexpr size_t READ_CHUNK_SIZE{16384};      // One TLS record
constexpr unsigned HTTP_OK{200};
constexpr unsigned HTTP_NOT_FOUND{404};

// A static glibc binary must not load NSS modules: they bring the host's libc into the process and crash it. The files
// and dns sources are built into libc. This has to run before the first name lookup.
Result<void> useBuiltInNameLookup() {
#if defined(__GLIBC__)
    if (__nss_configure_lookup("hosts", "files dns") != 0) {
        return fail(ErrorCode::Internal, "cannot limit name lookup to files and dns");
    }
#endif
    return {};
}

} // namespace

struct HttpsClient::Impl {
    explicit Impl(const asio::any_io_executor& executorValue)
            : executor(executorValue) {
    }

    asio::any_io_executor executor;                         // The io thread
    asio::ssl::context tls{asio::ssl::context::tls_client}; // Verifies against the OS trust store
};

HttpsClient::HttpsClient(std::unique_ptr<Impl> impl)
        : impl_(std::move(impl)) {
}

HttpsClient::~HttpsClient() = default;

Result<std::unique_ptr<HttpsClient>> HttpsClient::create(IoLoop& io) {
    CHECK(useBuiltInNameLookup());
    auto impl = std::make_unique<Impl>(io.executor());
    std::error_code error;
    impl->tls.set_verify_mode(asio::ssl::verify_peer, error);
    if (error) {
        return fail(ErrorCode::Internal, std::format("TLS setup: {}", error.message()));
    }
    CHECK(addSystemRoots(impl->tls.native_handle()));
    return std::unique_ptr<HttpsClient>{new HttpsClient{std::move(impl)}};
}

Async<std::string> HttpsClient::get(std::string host, std::string path) {
    const auto deadline = asio::cancel_after(NETWORK_LIMIT, exec::asio::completion_token);
    asio::ip::tcp::resolver resolver{impl_->executor};
    auto [resolveError, endpoints] = co_await resolver.async_resolve(host, HTTPS_PORT, deadline);
    if (resolveError) {
        co_return fail(ErrorCode::IoFailed, std::format("cannot resolve {}: {}", host, resolveError.message()));
    }
    asio::ssl::stream<asio::ip::tcp::socket> stream{impl_->executor, impl_->tls};
    if (SSL_set_tlsext_host_name(stream.native_handle(), host.c_str()) != 1) {
        co_return fail(ErrorCode::Internal, std::format("cannot set the TLS host name {}", host));
    }
    stream.set_verify_callback(asio::ssl::host_name_verification(host));
    auto [connectError, endpoint] = co_await asio::async_connect(stream.lowest_layer(), endpoints, deadline);
    if (connectError) {
        co_return fail(ErrorCode::IoFailed, std::format("cannot connect to {}: {}", host, connectError.message()));
    }
    const std::error_code handshakeError = co_await stream.async_handshake(asio::ssl::stream_base::client, deadline);
    if (handshakeError) {
        co_return fail(ErrorCode::IoFailed, std::format("TLS to {}: {}", host, handshakeError.message()));
    }
    const std::string request = std::format(
        "GET {} HTTP/1.1\r\n"
        "Host: {}\r\n"
        "User-Agent: tapin-cli\r\n"
        "Connection: close\r\n"
        "\r\n",
        path,
        host
    );
    auto [writeError, written] = co_await asio::async_write(stream, asio::buffer(request), deadline);
    if (writeError) {
        co_return fail(ErrorCode::IoFailed, std::format("write to {}: {}", host, writeError.message()));
    }

    HttpResponse response;
    std::array<char, READ_CHUNK_SIZE> chunk{};
    while (!response.complete()) {
        auto [readError, size] = co_await stream.async_read_some(asio::buffer(chunk), deadline);
        co_await response.feed(std::span{chunk}.first(size));
        if (readError == asio::error::eof) {
            co_await response.finishAtClose();
            break;
        }
        if (readError && !response.complete()) {
            co_return fail(ErrorCode::IoFailed, std::format("read from {}: {}", host, readError.message()));
        }
    }
    if (response.status() == HTTP_NOT_FOUND) {
        co_return fail(ErrorCode::NotFound, std::format("https://{}{} does not exist", host, path));
    }
    if (response.status() != HTTP_OK) {
        co_return fail(ErrorCode::IoFailed, std::format("https://{}{}: HTTP {}", host, path, response.status()));
    }
    co_return response.takeBody();
}

} // namespace tapin::io
