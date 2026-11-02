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

#include <memory>
#include <string>

namespace tapin::io {

class IoLoop;

// HTTPS GET against the trust store of the OS.
class HttpsClient {
public:
    static Result<std::unique_ptr<HttpsClient>> create(IoLoop& io);
    ~HttpsClient();

    HttpsClient(const HttpsClient&) = delete;
    HttpsClient& operator=(const HttpsClient&) = delete;

    // The body of a 200 reply. Any other status is an error that names it.
    Async<std::string> get(std::string host, std::string path);

private:
    struct Impl;
    explicit HttpsClient(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_; // The executor and the TLS context
};

} // namespace tapin::io
