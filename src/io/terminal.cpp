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

#include "io/terminal.h"

#include <cstdio>
#include <iostream>
#include <print>
#include <string>
#include <utility>

#include <stdexec/execution.hpp>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace tapin::io {

bool Terminal::isInteractive() {
#if defined(_WIN32)
    return _isatty(_fileno(stdin)) != 0;
#else
    return ::isatty(STDIN_FILENO) != 0;
#endif
}

Async<bool> Terminal::confirm(std::string prompt) {
    const auto readAnswer = [&prompt] {
        std::print("{} Type YES to continue: ", prompt);
        std::string line;
        std::getline(std::cin, line);
        return line == "YES" || line == "YES\r";
    };
    auto answer = stdexec::starts_on(context_.get_scheduler(), stdexec::just() | stdexec::then(readAnswer));
    co_return co_await std::move(answer);
}

} // namespace tapin::io
