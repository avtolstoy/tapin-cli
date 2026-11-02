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

namespace tapin::io {

namespace {

constexpr size_t MAX_BODY_SIZE{16 * 1024 * 1024}; // The largest real firmware image is under 1 MB
constexpr int STOP_PARSING{-1};                   // What an llhttp callback returns to end the parse

} // namespace

HttpResponse::HttpResponse() {
    llhttp_settings_init(&settings_);
    settings_.on_body = appendBody;
    settings_.on_message_complete = markComplete;
    llhttp_init(&parser_, HTTP_RESPONSE, &settings_);
    parser_.data = this;
}

int HttpResponse::appendBody(llhttp_t* parser, const char* data, size_t size) {
    auto* response = static_cast<HttpResponse*>(parser->data);
    if (response->body_.size() + size > MAX_BODY_SIZE) {
        response->tooLarge_ = true;
        return STOP_PARSING;
    }
    response->body_.append(data, size);
    return 0;
}

int HttpResponse::markComplete(llhttp_t* parser) {
    static_cast<HttpResponse*>(parser->data)->complete_ = true;
    return 0;
}

Result<void> HttpResponse::feed(std::span<const char> bytes) {
    if (llhttp_execute(&parser_, bytes.data(), bytes.size()) != HPE_OK) {
        return fail(ErrorCode::IoFailed, tooLarge_ ? "reply too large" : "bad HTTP reply");
    }
    return {};
}

Result<void> HttpResponse::finishAtClose() {
    if (llhttp_finish(&parser_) != HPE_OK) {
        return fail(ErrorCode::IoFailed, "connection ended early");
    }
    return {};
}

} // namespace tapin::io
