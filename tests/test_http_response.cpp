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

#include "io/http_response.h"
#include "testing.h"

#include <span>
#include <string>
#include <string_view>

using namespace tapin;
using io::HttpResponse;

namespace {

std::span<const char> bytesOf(std::string_view text) {
    return std::span{text.data(), text.size()};
}

} // namespace

TEST_CASE("a reply with Content-Length completes without the connection closing") {
    HttpResponse response;
    EXPECT(response.feed(bytesOf("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc")).has_value());
    EXPECT(response.complete());
    EXPECT(response.status() == 200);
    EXPECT(response.takeBody() == "abc");
}

TEST_CASE("a reply can arrive in pieces") {
    HttpResponse response;
    EXPECT(response.feed(bytesOf("HTTP/1.1 200 OK\r\nContent-Le")).has_value());
    EXPECT(response.feed(bytesOf("ngth: 3\r\n\r\nab")).has_value());
    EXPECT_FALSE(response.complete());
    EXPECT(response.feed(bytesOf("c")).has_value());
    EXPECT(response.complete());
    EXPECT(response.takeBody() == "abc");
}

TEST_CASE("a chunked reply is put back together") {
    HttpResponse response;
    EXPECT(response.feed(bytesOf("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n")).has_value());
    EXPECT(response.complete());
    EXPECT(response.takeBody() == "abc");
}

TEST_CASE("a reply with no length ends when the connection closes") {
    HttpResponse response;
    EXPECT(response.feed(bytesOf("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nabc")).has_value());
    EXPECT_FALSE(response.complete());
    EXPECT(response.finishAtClose().has_value());
    EXPECT(response.complete());
    EXPECT(response.takeBody() == "abc");
}

TEST_CASE("a reply cut short is an error when the connection closes") {
    HttpResponse response;
    EXPECT(response.feed(bytesOf("HTTP/1.1 200 OK\r\nContent-Length: 9\r\n\r\nabc")).has_value());
    EXPECT(test::failedSaying(response.finishAtClose(), "ended early"));
    EXPECT_FALSE(response.complete());
}

TEST_CASE("a reply that is not HTTP is an error") {
    HttpResponse response;
    EXPECT_FALSE(response.feed(bytesOf("not http at all\r\n\r\n")).has_value());
}

TEST_CASE("a reply over the size limit is an error") {
    HttpResponse response;
    EXPECT(response.feed(bytesOf("HTTP/1.1 200 OK\r\n\r\n")).has_value());
    const std::string megabyte(1024 * 1024, 'x');
    Result<void> fed;
    for (int count = 0; count < 17 && fed; ++count) {
        fed = response.feed(megabyte);
    }
    EXPECT(test::failedSaying(fed, "too large"));
}
