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

#include <chrono>
#include <memory>
#include <utility>

#include <stdexec/execution.hpp>

namespace asio {
class any_io_executor;
}

namespace tapin::io {

// Owns the one Asio io thread. Only the io module sees Asio, everything else holds an IoLoop&.
class IoLoop {
public:
    IoLoop();
    ~IoLoop();

    IoLoop(const IoLoop&) = delete;
    IoLoop& operator=(const IoLoop&) = delete;

    stdexec::task_scheduler scheduler() const;

    // For the io module: what its sockets, ports and timers run on.
    const asio::any_io_executor& executor() const;

    // Lets a coroutine on the loop wait without blocking it.
    Async<void> sleepFor(std::chrono::milliseconds duration);

    // Waits for one coroutine on the loop. Main and tests wait like this.
    template <typename T>
    Result<T> waitFor(Async<T> work) {
        auto sender = stdexec::starts_on(scheduler(), std::move(work));
        auto result = stdexec::sync_wait(std::move(sender));
        if (!result) {
            return fail(ErrorCode::Internal, "the command stopped");
        }
        return std::move(std::get<0>(*result));
    }

private:
    struct Pool;
    std::unique_ptr<Pool> pool_; // The thread and its executor
};

} // namespace tapin::io
