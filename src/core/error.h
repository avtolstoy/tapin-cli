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

#include <expected>
#include <string>
#include <utility>

namespace tapin {

enum class ErrorCode {
    InvalidArgument,
    NotFound,
    LensNotReady,
    Overcurrent,
    WrongImage,
    NoPatchSites,
    Interrupted,
    IoFailed,
    Internal,
};

struct Error {
    ErrorCode code;
    std::string message;
};

template <typename T>
using Result = std::expected<T, Error>;

template <typename T>
inline constexpr bool isResult = false;

template <typename T>
inline constexpr bool isResult<Result<T>> = true;

inline std::unexpected<Error> fail(ErrorCode code, std::string message) {
    return std::unexpected<Error>{Error{code, std::move(message)}};
}

} // namespace tapin

// CHECK(expr) evaluates a Result: it yields the value, or returns the error from the enclosing function.
// Synchronous functions only. GCC 16 crashes on statement expressions that contain
// co_await or co_return, so coroutines use co_await on the Result instead (core/async.h). snitch is built
// without its shorthand macros, so its CHECK does not clash; tests use EXPECT (tests/testing.h).
#define TAPIN_CHECK_CONCAT_IMPL(first, second) first##second
#define TAPIN_CHECK_CONCAT(first, second) TAPIN_CHECK_CONCAT_IMPL(first, second)
#define TAPIN_CHECK_NAMED(name, ...)                                         \
    __extension__({                                                          \
        auto name = (__VA_ARGS__);                                           \
        if (!name) {                                                         \
            return std::unexpected<::tapin::Error>{std::move(name).error()}; \
        }                                                                    \
        std::move(name).value();                                             \
    })
#define CHECK(...) TAPIN_CHECK_NAMED(TAPIN_CHECK_CONCAT(tapinCheck, __COUNTER__), __VA_ARGS__)
