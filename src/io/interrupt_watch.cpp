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

#include "io/interrupt_watch.h"

#include "io/asio.h"
#include "io/loop.h"

#include <atomic>
#include <csignal>
#include <memory>

namespace tapin::io {

struct InterruptWatch::Impl {
    explicit Impl(const asio::any_io_executor& executor)
            : signals(executor, SIGINT) {
    }

    asio::signal_set signals;                                                            // Registered for this object's lifetime
    std::shared_ptr<std::atomic<bool>> flag{std::make_shared<std::atomic<bool>>(false)}; // Outlives the signal handler
};

InterruptWatch::InterruptWatch(IoLoop& io)
        : impl_(std::make_unique<Impl>(io.executor())) {
    const auto markInterrupted = [flag = impl_->flag](const std::error_code& error, int) {
        if (!error) {
            flag->store(true);
        }
    };
    impl_->signals.async_wait(markInterrupted);
}

InterruptWatch::~InterruptWatch() = default;

bool InterruptWatch::interrupted() const {
    return impl_->flag->load();
}

} // namespace tapin::io
