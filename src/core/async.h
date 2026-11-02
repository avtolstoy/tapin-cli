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

#include <stdexec/execution.hpp>

#include <concepts>
#include <coroutine>
#include <exception>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace tapin {

template <typename T>
class Async;

namespace async_coroutine {

template <typename T>
inline constexpr bool isAsync = false;

template <typename T>
inline constexpr bool isAsync<Async<T>> = true;

using Scheduler = stdexec::task_scheduler;
using StopToken = stdexec::inplace_stop_token;

class PromiseBase;

// The environment an Async frame gives to the senders it awaits: its scheduler and the root's stop token.
struct Environment {
    const PromiseBase* promise;

    StopToken query(stdexec::get_stop_token_t) const noexcept;
    Scheduler query(stdexec::get_scheduler_t) const noexcept;
    Scheduler query(stdexec::get_start_scheduler_t) const noexcept;
};

// State shared by every Async frame. A frame is either a root (awaited by stdexec: connect, sync_wait,
// async_scope) or a child awaited by another Async frame. Errors and stop requests walk up this chain.
class PromiseBase {
public:
    PromiseBase() = default;
    PromiseBase(const PromiseBase&) = delete;
    PromiseBase& operator=(const PromiseBase&) = delete;

    // Called by a parent Async frame before it resumes this one.
    void startChild(PromiseBase& parent, std::coroutine_handle<> parentHandle, bool propagatesErrors) noexcept {
        scheduler_ = parent.scheduler_;
        stopToken_ = parent.stopToken_;
        continuation_ = parentHandle;
        awaiter_ = &parent;
        propagatesErrors_ = propagatesErrors;
    }

    // Called by the root awaiter with what it found in the stdexec environment.
    template <typename Foreign>
    void startRoot(std::coroutine_handle<Foreign> foreign, Scheduler scheduler, StopToken stopToken) noexcept {
        scheduler_ = std::move(scheduler);
        stopToken_ = stopToken;
        continuation_ = foreign;
        rootPromise_ = &foreign.promise();
        rootStopped_ = [](void* promise) noexcept -> std::coroutine_handle<> {
            return static_cast<Foreign*>(promise)->unhandled_stopped();
        };
    }

    // Records the error in this frame and in every frame that awaits it with propagation, then returns the
    // coroutine that receives the error as a value: a parent that awaited .result(), or the root awaiter.
    // The frames in between are never resumed; their owners destroy them.
    std::coroutine_handle<> failWith(const Error& error) noexcept {
        PromiseBase* frame = this;
        frame->storeError(error);
        while (frame->propagatesErrors_ && frame->awaiter_ != nullptr) {
            frame = frame->awaiter_;
            frame->storeError(error);
        }
        return frame->continuation_;
    }

    // A stop request is never a value: it ends every frame up to the root, which completes with set_stopped.
    std::coroutine_handle<> stopWalk() noexcept {
        PromiseBase* frame = this;
        while (frame->awaiter_ != nullptr) {
            frame = frame->awaiter_;
        }
        return frame->rootStopped_(frame->rootPromise_);
    }

    // Where control goes when the coroutine body finishes, with a value or after co_return fail(...).
    std::coroutine_handle<> completion() noexcept {
        const Error* error = storedError();
        if (error != nullptr && propagatesErrors_ && awaiter_ != nullptr) {
            return awaiter_->failWith(*error);
        }
        return continuation_;
    }

    const Scheduler& scheduler() const noexcept {
        return scheduler_;
    }

    StopToken stopToken() const noexcept {
        return stopToken_;
    }

    Environment env() const noexcept {
        return Environment{this};
    }

protected:
    ~PromiseBase() = default;

private:
    virtual void storeError(const Error& error) noexcept = 0;
    virtual const Error* storedError() const noexcept = 0;

    Scheduler scheduler_{stdexec::inline_scheduler{}};        // Where the frame resumes after a sender; from the parent or root
    StopToken stopToken_;                                     // From the root's stdexec environment; empty when the root cannot be stopped
    std::coroutine_handle<> continuation_;                    // Resumed when this frame completes with a value or an error
    PromiseBase* awaiter_ = nullptr;                          // The Async frame awaiting this one; null for a root
    bool propagatesErrors_ = false;                           // The awaiting frame wants the value, so an error ends it too
    void* rootPromise_ = nullptr;                             // Root only: the stdexec promise awaiting this frame
    std::coroutine_handle<> (*rootStopped_)(void*) = nullptr; // Root only: that promise's unhandled_stopped
};

inline StopToken Environment::query(stdexec::get_stop_token_t) const noexcept {
    return promise->stopToken();
}

inline Scheduler Environment::query(stdexec::get_scheduler_t) const noexcept {
    return promise->scheduler();
}

inline Scheduler Environment::query(stdexec::get_start_scheduler_t) const noexcept {
    return promise->scheduler();
}

struct FinalAwaiter {
    static bool await_ready() noexcept {
        return false;
    }

    template <typename Promise>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> handle) noexcept {
        return handle.promise().completion();
    }

    static void await_resume() noexcept {
    }
};

// co_await on a Result: its value, or the coroutine ends with its error.
template <typename T>
struct ResultAwaiter {
    Result<T> result;
    PromiseBase& promise;

    bool await_ready() const noexcept {
        return result.has_value();
    }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<>) noexcept {
        return promise.failWith(result.error());
    }

    T await_resume() {
        return std::move(result).value();
    }
};

// co_await on a child Async. Propagate: yields the value, and an error ends the parent too. Otherwise (.result()):
// yields the child's Result.
template <typename T, bool Propagate>
class ChildAwaiter {
public:
    ChildAwaiter(Async<T> child, PromiseBase& parent) noexcept
            : child_(std::move(child)),
              parent_(parent) {
    }

    static bool await_ready() noexcept {
        return false;
    }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> parentHandle) noexcept {
        child_.handle_.promise().startChild(parent_, parentHandle, Propagate);
        return child_.handle_;
    }

    auto await_resume() {
        Result<T> result = child_.handle_.promise().takeResult();
        if constexpr (Propagate) {
            return std::move(result).value();
        } else {
            return result;
        }
    }

private:
    Async<T> child_;
    PromiseBase& parent_;
};

// What co_await yields for one value completion: nothing, the value, or a std::tuple of the values (Asio operations
// through completion_token complete with (error_code, result), read with structured bindings).
template <typename... Values>
struct AwaitedValue {
    using type = std::tuple<std::decay_t<Values>...>;
};

template <>
struct AwaitedValue<> {
    using type = void;
};

template <typename Value>
struct AwaitedValue<Value> {
    using type = std::decay_t<Value>;
};

// A sender without value completions (just_error, just_stopped) never resumes the coroutine with a value.
template <typename... Signatures>
struct OneSignature {
    static_assert(sizeof...(Signatures) == 1, "Async awaits senders with exactly one value completion");
    using type = typename Signatures...[0] ::type;
};

template <>
struct OneSignature<> {
    using type = void;
};

template <typename Sender>
using SenderValue = typename stdexec::value_types_of_t<Sender, Environment, AwaitedValue, OneSignature>::type;

// co_await on a stdexec sender. The completion moves back to the frame's scheduler before the coroutine resumes.
// set_error(Error) and a Result value holding an error end the coroutine with that error; set_stopped ends the
// chain up to the root. Other error types are a bug in the sender: they terminate.
template <typename Sender>
class SenderAwaiter {
    using Value = SenderValue<Sender>;
    using Stored = std::conditional_t<std::is_void_v<Value>, std::monostate, Value>;
    using Rescheduled = decltype(stdexec::continues_on(std::declval<Sender>(), std::declval<Scheduler>()));

    struct Receiver {
        using receiver_concept = stdexec::receiver_t;

        SenderAwaiter* awaiter;

        template <typename... Values>
        void set_value(Values&&... values) noexcept {
            awaiter->value_.emplace(std::forward<Values>(values)...);
            if constexpr (isResult<Value>) {
                if (!awaiter->value_->has_value()) {
                    awaiter->promise_.failWith(awaiter->value_->error()).resume();
                    return;
                }
            }
            awaiter->handle_.resume();
        }

        void set_error(Error error) noexcept {
            awaiter->promise_.failWith(error).resume();
        }

        template <typename Other>
        void set_error(Other&&) noexcept {
            std::terminate();
        }

        void set_stopped() noexcept {
            awaiter->promise_.stopWalk().resume();
        }

        Environment get_env() const noexcept {
            return awaiter->promise_.env();
        }
    };

public:
    SenderAwaiter(Sender&& sender, PromiseBase& promise)
            : promise_(promise),
              operation_(stdexec::connect(stdexec::continues_on(std::move(sender), promise.scheduler()), Receiver{this})) {
    }

    static bool await_ready() noexcept {
        return false;
    }

    // Nothing may touch the awaiter after start(): the receiver can resume the coroutine before start() returns.
    void await_suspend(std::coroutine_handle<> handle) noexcept {
        handle_ = handle;
        stdexec::start(operation_);
    }

    auto await_resume() {
        if constexpr (std::is_void_v<Value>) {
            return;
        } else if constexpr (isResult<Value>) {
            return std::move(*value_).value();
        } else {
            return std::move(*value_);
        }
    }

private:
    PromiseBase& promise_;           // The awaiting frame; errors and stops start their walk here
    std::coroutine_handle<> handle_; // The awaiting coroutine, resumed with the value
    std::optional<Stored> value_;    // Filled by set_value before the coroutine resumes
    stdexec::connect_result_t<Rescheduled, Receiver> operation_;
};

template <typename T>
class Promise;

// Everything except how the coroutine body returns, shared by Promise<T> and Promise<void>.
template <typename T>
class PromiseCommon : public PromiseBase {
public:
    Async<T> get_return_object() noexcept;

    static std::suspend_always initial_suspend() noexcept {
        return {};
    }

    static FinalAwaiter final_suspend() noexcept {
        return {};
    }

    [[noreturn]]
    static void unhandled_exception() noexcept {
        std::terminate();
    }

    template <typename U>
    ResultAwaiter<U> await_transform(Result<U> result) noexcept {
        return ResultAwaiter<U>{std::move(result), *this};
    }

    ResultAwaiter<std::monostate> await_transform(std::unexpected<Error> error) noexcept {
        return ResultAwaiter<std::monostate>{std::move(error), *this};
    }

    template <typename U>
    ChildAwaiter<U, true> await_transform(Async<U>&& child) noexcept {
        return ChildAwaiter<U, true>{std::move(child), *this};
    }

    template <typename Wrapped>
    requires requires { typename Wrapped::ResultOfTag; }
    ChildAwaiter<typename Wrapped::Value, false> await_transform(Wrapped&& wrapped) noexcept {
        return ChildAwaiter<typename Wrapped::Value, false>{std::move(wrapped.async), *this};
    }

    template <stdexec::sender_in<Environment> Sender>
    requires(!isAsync<std::remove_cvref_t<Sender>>) SenderAwaiter<std::remove_cvref_t<Sender>> await_transform(Sender&& sender) {
        return SenderAwaiter<std::remove_cvref_t<Sender>>{std::forward<Sender>(sender), *this};
    }

    Environment get_env() const noexcept {
        return env();
    }

    Result<T> takeResult() noexcept {
        return std::move(*result_);
    }

protected:
    std::optional<Result<T>> result_; // Set by co_return or by an error walk; read once by the awaiter

private:
    void storeError(const Error& error) noexcept override {
        result_.emplace(std::unexpected<Error>{error});
    }

    const Error* storedError() const noexcept override {
        if (!result_ || result_->has_value()) {
            return nullptr;
        }
        return &result_->error();
    }
};

template <typename T>
class Promise : public PromiseCommon<T> {
public:
    template <typename Value>
    requires std::convertible_to<Value, Result<T>> void return_value(Value&& value) {
        this->result_.emplace(std::forward<Value>(value));
    }
};

// A coroutine promise cannot have both return_void and return_value, so an Async<void> fails early with
// co_await fail(...) instead of co_return fail(...).
template <>
class Promise<void> : public PromiseCommon<void> {
public:
    void return_void() noexcept {
        result_.emplace();
    }
};

// Awaiting an Async from stdexec (connect, sync_wait, async_scope) makes it the root of a chain. The root takes
// the scheduler and stop token from the stdexec environment and yields Result<T>; a stop completes the stdexec
// side with set_stopped.
template <typename T>
class RootAwaiter {
public:
    explicit RootAwaiter(std::coroutine_handle<Promise<T>> handle) noexcept
            : handle_(handle) {
    }

    RootAwaiter(RootAwaiter&& other) noexcept
            : handle_(std::exchange(other.handle_, {})) {
    }

    RootAwaiter& operator=(RootAwaiter&&) = delete;

    ~RootAwaiter() {
        if (handle_) {
            handle_.destroy();
        }
    }

    static bool await_ready() noexcept {
        return false;
    }

    template <typename Foreign>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Foreign> foreign) noexcept {
        auto env = stdexec::get_env(foreign.promise());
        using ForeignToken = stdexec::stop_token_of_t<decltype(env)>;
        static_assert(std::same_as<ForeignToken, StopToken> || stdexec::unstoppable_token<ForeignToken>, "An Async root needs an inplace_stop_token or no stop token in its environment");
        Scheduler scheduler{stdexec::inline_scheduler{}};
        if constexpr (requires { stdexec::get_start_scheduler(env); }) {
            scheduler = Scheduler{stdexec::get_start_scheduler(env)};
        } else if constexpr (requires { stdexec::get_scheduler(env); }) {
            scheduler = Scheduler{stdexec::get_scheduler(env)};
        }
        StopToken stopToken;
        if constexpr (std::same_as<ForeignToken, StopToken>) {
            stopToken = stdexec::get_stop_token(env);
        }
        handle_.promise().startRoot(foreign, std::move(scheduler), stopToken);
        return handle_;
    }

    Result<T> await_resume() {
        return handle_.promise().takeResult();
    }

private:
    std::coroutine_handle<Promise<T>> handle_;
};

} // namespace async_coroutine

template <typename T>
inline constexpr bool isAsync = async_coroutine::isAsync<T>;

// A lazily started coroutine for everything asynchronous in tapin. co_await reads like await in JavaScript:
// awaiting a Result, a child Async or a stdexec sender yields the value, and an error ends this coroutine with that
// error, up to the nearest caller that awaits .result() instead. Started from stdexec (connect, sync_wait,
// async_scope) it completes with set_value(Result<T>), or set_stopped after a stop request.
template <typename T>
class [[nodiscard]] Async {
public:
    using promise_type = async_coroutine::Promise<T>;

    // The awaitable returned by result(): awaiting it yields Result<T> instead of propagating the error.
    struct ResultOf {
        using ResultOfTag = void;
        using Value = T;

        Async async;
    };

    Async(Async&& other) noexcept
            : handle_(std::exchange(other.handle_, {})) {
    }

    Async& operator=(Async&& other) noexcept {
        if (this != &other) {
            destroy();
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }

    Async(const Async&) = delete;
    Async& operator=(const Async&) = delete;

    ~Async() {
        destroy();
    }

    ResultOf result() && noexcept {
        return ResultOf{std::move(*this)};
    }

    async_coroutine::RootAwaiter<T> operator co_await() && noexcept {
        return async_coroutine::RootAwaiter<T>{std::exchange(handle_, {})};
    }

private:
    template <typename, bool>
    friend class async_coroutine::ChildAwaiter;
    friend class async_coroutine::PromiseCommon<T>;

    explicit Async(std::coroutine_handle<promise_type> handle) noexcept
            : handle_(handle) {
    }

    void destroy() noexcept {
        if (handle_) {
            std::exchange(handle_, {}).destroy();
        }
    }

    std::coroutine_handle<promise_type> handle_;
};

namespace async_coroutine {

template <typename T>
Async<T> PromiseCommon<T>::get_return_object() noexcept {
    auto& promise = static_cast<Promise<T>&>(*this);
    return Async<T>{std::coroutine_handle<Promise<T>>::from_promise(promise)};
}

} // namespace async_coroutine

} // namespace tapin
