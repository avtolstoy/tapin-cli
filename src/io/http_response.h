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

#include "core/error.h"

#include <cstddef>
#include <span>
#include <string>
#include <utility>

#include <llhttp.h>

namespace tapin::io {

// One HTTP/1.1 reply, fed in as it arrives. llhttp keeps pointers into this object, so it does not move.
class HttpResponse {
public:
    HttpResponse();

    HttpResponse(const HttpResponse&) = delete;
    HttpResponse& operator=(const HttpResponse&) = delete;

    Result<void> feed(std::span<const char> bytes);

    // The connection closed cleanly. That ends a reply with no Content-Length, and only llhttp_finish tells llhttp. A
    // reply that was cut short is an error.
    Result<void> finishAtClose();

    bool complete() const {
        return complete_;
    }

    unsigned status() const {
        return parser_.status_code;
    }

    std::string takeBody() {
        return std::move(body_);
    }

private:
    static int appendBody(llhttp_t* parser, const char* data, size_t size);
    static int markComplete(llhttp_t* parser);

    llhttp_settings_t settings_{}; // llhttp keeps a pointer to this
    llhttp_t parser_{};            // Its data member points back at this object
    std::string body_;             // What llhttp has delivered so far
    bool complete_{};              // llhttp saw the end of the message
    bool tooLarge_{};              // The server sent more than any firmware image can be
};

} // namespace tapin::io
