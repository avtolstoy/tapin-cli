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

#include "io/loop.h"

#include "io/asio.h"

#include <format>
#include <memory>

#include <exec/asio/asio_thread_pool.hpp>
#include <exec/asio/completion_token.hpp>

namespace tapin::io {

struct IoLoop::Pool {
    exec::asio::asio_thread_pool threads{1};                // The one io thread
    asio::any_io_executor executor{threads.get_executor()}; // Handed to everything that opens a socket or a port
};

IoLoop::IoLoop()
        : pool_(std::make_unique<Pool>()) {
}

IoLoop::~IoLoop() = default;

stdexec::task_scheduler IoLoop::scheduler() const {
    return stdexec::task_scheduler{pool_->threads.get_scheduler()};
}

const asio::any_io_executor& IoLoop::executor() const {
    return pool_->executor;
}

Async<void> IoLoop::sleepFor(std::chrono::milliseconds duration) {
    asio::steady_timer timer{pool_->executor, duration};
    const std::error_code error = co_await timer.async_wait(exec::asio::completion_token);
    if (error) {
        co_await fail(ErrorCode::IoFailed, std::format("timer: {}", error.message()));
    }
}

} // namespace tapin::io
