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

#include <string>

#include <exec/single_thread_context.hpp>

namespace tapin::io {

// The prompt runs on its own thread, the io thread never blocks on stdin.
class Terminal {
public:
    // Whether stdin can answer a prompt at all.
    static bool isInteractive();

    // Prints the prompt and asks for the word YES.
    Async<bool> confirm(std::string prompt);

private:
    exec::single_thread_context context_; // The thread that reads stdin
};

} // namespace tapin::io
