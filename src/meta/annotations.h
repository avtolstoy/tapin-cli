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

#include <cstddef>
#include <meta>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// The annotations a domain facade puts on its member functions and argument fields, and the reflection queries
// the CLI generator shares. Every text annotation stores a static string created in its
// consteval constructor, so the annotation value can be read at compile time.
namespace tapin::meta {

// A CLI subcommand.
struct Cli {
    const char* command;

    consteval Cli(std::string_view word)
            : command(std::define_static_string(word)) {
    }
};

struct Description {
    const char* text;

    consteval Description(std::string_view textValue)
            : text(std::define_static_string(textValue)) {
    }
};

// The program a CLI facade belongs to: meta/cli.h answers --help and --version from it, before it looks up a
// command, so no facade can shadow them.
struct Program {
    const char* name;
    const char* version;
    const char* summary;  // One line at the top of --help
    const char* homepage; // Where to go for more, at the bottom of --help

    consteval Program(std::string_view programName, std::string_view programVersion, std::string_view programSummary, std::string_view programHomepage)
            : name(std::define_static_string(programName)),
              version(std::define_static_string(programVersion)),
              summary(std::define_static_string(programSummary)),
              homepage(std::define_static_string(programHomepage)) {
    }
};

// A CLI argument taken by position instead of as --name.
struct Positional {
    consteval Positional() = default;
};

consteval bool hasAnnotation(std::meta::info declaration, std::meta::info annotation) {
    for (auto value : std::meta::annotations_of(declaration)) {
        if (std::meta::remove_const(std::meta::type_of(value)) == annotation) {
            return true;
        }
    }
    return false;
}

template <std::meta::info Member, typename Annotation>
consteval Annotation annotationValue() {
    static constexpr auto values = std::define_static_array(std::meta::annotations_of(Member));
    template for (constexpr auto value : values) {
        if constexpr (std::meta::remove_const(std::meta::type_of(value)) == ^^Annotation) {
            return std::meta::extract<Annotation>(value);
        }
    }
    std::unreachable();
}

consteval std::string_view descriptionOf(std::meta::info member) {
    for (auto value : std::meta::annotations_of(member)) {
        if (std::meta::remove_const(std::meta::type_of(value)) == ^^Description) {
            return std::string_view{std::meta::extract<Description>(value).text};
        }
    }
    return {};
}

// The member functions of Api that carry Annotation, in declaration order. Every generator walks this list.
template <typename Api, typename Annotation>
consteval std::span<const std::meta::info> annotatedFunctions() {
    std::vector<std::meta::info> functions;
    for (auto member : std::meta::members_of(^^Api, std::meta::access_context::unchecked())) {
        if (std::meta::is_function(member) && hasAnnotation(member, ^^Annotation)) {
            functions.push_back(member);
        }
    }
    return std::define_static_array(functions);
}

template <std::meta::info Function>
using ArgsOf = typename[:std::meta::remove_cvref(std::meta::type_of(std::meta::parameters_of(Function)[0])):];

template <std::meta::info Function>
using ReturnOf = typename[:std::meta::return_type_of(Function):];

namespace annotation_checks {

template <typename Api>
consteval std::string duplicateCommandError() {
    static constexpr auto functions = annotatedFunctions<Api, Cli>();
    std::vector<std::string_view> commands;
    std::vector<std::string_view> names;
    template for (constexpr auto function : functions) {
        commands.push_back(annotationValue<function, Cli>().command);
        names.push_back(std::meta::identifier_of(function));
    }
    for (size_t first = 0; first < commands.size(); ++first) {
        for (size_t second = first + 1; second < commands.size(); ++second) {
            if (commands[first] == commands[second]) {
                return "duplicate command '" + std::string{commands[first]} + "' on " + std::string{names[first]} + " and " + std::string{names[second]};
            }
        }
    }
    return {};
}

template <typename Api>
consteval std::string signatureError() {
    for (auto function : annotatedFunctions<Api, Cli>()) {
        const std::string name{std::meta::identifier_of(function)};
        if (std::meta::parameters_of(function).size() != 1) {
            return name + " must take exactly one argument struct";
        }
        if (!std::meta::is_class_type(std::meta::remove_cvref(std::meta::type_of(std::meta::parameters_of(function)[0])))) {
            return name + " must take a struct argument";
        }
    }
    return {};
}

} // namespace annotation_checks

// Compile-time checks on a CLI facade: no two functions share a command, and every one takes one argument struct.
template <typename Api>
consteval bool checkApi() {
    static_assert(annotation_checks::duplicateCommandError<Api>().empty(), annotation_checks::duplicateCommandError<Api>());
    static_assert(annotation_checks::signatureError<Api>().empty(), annotation_checks::signatureError<Api>());
    return true;
}

} // namespace tapin::meta
