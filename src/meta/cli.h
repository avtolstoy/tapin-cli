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
#include "meta/annotations.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <meta>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// The CLI generator: every [[=Cli{...}]] member function of a facade is a subcommand, its argument struct's fields
// are --flags or positionals, and the help text comes from [[=Description{...}]]. `--help` lists the commands and
// `<command> --help` shows one of them. --help and --version belong to the [[=Program{...}]] annotation, so no command
// can shadow them.
namespace tapin::meta {

// The two words the parser answers itself, before it looks up a command.
inline constexpr std::string_view HELP_WORD{"--help"};
inline constexpr std::string_view VERSION_WORD{"--version"};

// The exit codes of every command.
enum class ExitCode {
    Ok = 0,
    Failure = 1, // The command failed, or refused to go on
    Usage = 2,   // Bad command line
};

// What a command prints: the lines main writes with std::println, and the exit code. A command never puts a newline
// into a line; text that spans lines goes through text().
class CliOutput {
public:
    template <typename... Args>
    void line(std::format_string<Args...> text, Args&&... args) {
        lines_.push_back(std::format(text, std::forward<Args>(args)...));
    }

    // Text that may span lines, one entry per line. A trailing newline ends the last line, as printing does.
    void text(std::string_view text) {
        size_t start = 0;
        while (start < text.size()) {
            const size_t end = text.find('\n', start);
            if (end == std::string_view::npos) {
                lines_.emplace_back(text.substr(start));
                return;
            }
            lines_.emplace_back(text.substr(start, end - start));
            start = end + 1;
        }
    }

    const std::vector<std::string>& lines() const {
        return lines_;
    }

    ExitCode exitCode{ExitCode::Ok};

private:
    std::vector<std::string> lines_; // What gets printed, one entry per line
};

namespace cli_parser {

// A member that may be left out: a positional the command may omit, or an optional flag value.
template <typename T>
inline constexpr bool isOptionalField = false;

template <typename T>
inline constexpr bool isOptionalField<std::optional<T>> = true;

struct FieldInfo {
    std::string_view name;
    std::string_view description;
    bool positional;
    bool boolean;
    bool optional;
};

template <typename Args>
consteval auto fieldInfos() {
    static constexpr auto members = std::define_static_array(std::meta::nonstatic_data_members_of(^^Args, std::meta::access_context::unchecked()));
    std::array<FieldInfo, members.size()> out{};
    [[maybe_unused]] size_t index{};
    template for (constexpr auto member : members) {
        using Field = typename[:std::meta::type_of(member):];
        out[index] = FieldInfo{
            .name = std::meta::identifier_of(member),
            .description = descriptionOf(member),
            .positional = hasAnnotation(member, ^^Positional),
            .boolean = std::is_same_v<Field, bool>,
            .optional = isOptionalField<Field>,
        };
        ++index;
    }
    return out;
}

template <typename T>
Result<T> parseCliValue(std::string_view text) {
    if constexpr (isOptionalField<T>) {
        using Inner = typename T::value_type;
        auto inner = CHECK(parseCliValue<Inner>(text));
        return T{std::move(inner)};
    } else if constexpr (std::is_same_v<T, std::string>) {
        return std::string{text};
    } else if constexpr (std::is_same_v<T, bool>) {
        if (text == "true") {
            return true;
        }
        if (text == "false") {
            return false;
        }
        return fail(ErrorCode::InvalidArgument, std::format("expected true or false, got {}", text));
    } else {
        static_assert(!std::is_same_v<T, T>, "unsupported cli field type");
    }
}

template <typename Args>
const FieldInfo* findFlag(std::string_view flag) {
    static constexpr auto fields = fieldInfos<Args>();
    const auto found = std::find_if(fields.begin(), fields.end(), [flag](const FieldInfo& field) { return !field.positional && field.name == flag; });
    return found == fields.end() ? nullptr : &*found;
}

using CliOptions = std::vector<std::pair<std::string_view, std::string_view>>;

// Fills Args from --name value pairs and positionals, in field order.
template <typename Args>
Result<void> assignArgs(Args& parsed, const CliOptions& options, std::span<const std::string_view> positionals) {
    static constexpr auto members = std::define_static_array(std::meta::nonstatic_data_members_of(^^Args, std::meta::access_context::unchecked()));
    size_t positionalIndex{};
    template for (constexpr auto member : members) {
        constexpr auto name = std::meta::identifier_of(member);
        using Field = typename[:std::meta::type_of(member):];
        std::optional<std::string_view> value;
        if constexpr (hasAnnotation(member, ^^Positional)) {
            if (positionalIndex < positionals.size()) {
                value = positionals[positionalIndex++];
            } else if constexpr (!isOptionalField<Field>) {
                return fail(ErrorCode::InvalidArgument, std::format("missing value for {}", name));
            }
        } else {
            const auto found = std::find_if(options.begin(), options.end(), [name](const auto& option) { return option.first == name; });
            if (found == options.end()) {
                continue;
            }
            value = found->second;
        }
        if (value) {
            parsed.[:member:] = CHECK(parseCliValue<Field>(*value));
        }
    }
    if (positionalIndex < positionals.size()) {
        return fail(ErrorCode::InvalidArgument, std::format("unexpected argument: {}", positionals[positionalIndex]));
    }
    return {};
}

template <typename Args>
Result<Args> parseArgs(std::span<const std::string_view> words) {
    CliOptions options;
    std::vector<std::string_view> positionals;
    for (size_t index = 0; index < words.size(); ++index) {
        const std::string_view word = words[index];
        if (!word.starts_with("--")) {
            positionals.push_back(word);
            continue;
        }
        const std::string_view flag = word.substr(2);
        const FieldInfo* field = findFlag<Args>(flag);
        if (field == nullptr) {
            return fail(ErrorCode::InvalidArgument, std::format("unknown option --{}", flag));
        }
        if (field->boolean) {
            options.emplace_back(flag, "true");
            continue;
        }
        if (index + 1 >= words.size()) {
            return fail(ErrorCode::InvalidArgument, std::format("missing value for --{}", flag));
        }
        options.emplace_back(flag, words[++index]);
    }
    Args parsed{};
    CHECK(assignArgs(parsed, options, positionals));
    return parsed;
}

// The commands, in the order of annotatedFunctions: index N of this table is alternative N of CliArguments. It is
// built outside any coroutine, where GCC 16 constant-evaluates the expansion statement.
template <typename Api>
std::vector<Cli> commandTable() {
    static_assert(checkApi<Api>());
    static constexpr auto functions = annotatedFunctions<Api, Cli>();
    std::vector<Cli> table;
    table.reserve(functions.size());
    template for (constexpr auto function : functions) {
        table.push_back(annotationValue<function, Cli>());
    }
    return table;
}

struct HelpRow {
    std::string name;
    std::string_view description;
};

// One section of help: the names in a column, the descriptions lined up after it.
inline void appendHelpSection(std::string& out, std::string_view heading, const std::vector<HelpRow>& rows) {
    if (rows.empty()) {
        return;
    }
    size_t width{};
    for (const HelpRow& row : rows) {
        width = std::max(width, row.name.size());
    }
    out += std::format("\n{}:\n", heading);
    for (const HelpRow& row : rows) {
        out += std::format("  {:<{}}  {}\n", row.name, width, row.description);
    }
}

template <typename Args>
std::string commandHelpText(std::string_view program, std::string_view command, std::string_view description) {
    static constexpr auto fields = fieldInfos<Args>();
    std::string usage = std::format("usage: {} {}", program, command);
    std::vector<HelpRow> arguments;
    std::vector<HelpRow> options;
    for (const FieldInfo& field : fields) {
        if (field.positional) {
            std::string name = std::format("<{}>", field.name);
            usage += field.optional ? std::format(" [{}]", name) : std::format(" {}", name);
            arguments.push_back(HelpRow{std::move(name), field.description});
        } else if (field.boolean) {
            options.push_back(HelpRow{std::format("--{}", field.name), field.description});
        } else {
            options.push_back(HelpRow{std::format("--{} <{}>", field.name, field.name), field.description});
        }
    }
    options.push_back(HelpRow{std::string{HELP_WORD}, "Show this help"});
    std::string out = std::format("{}\n\n{} [options]\n", description, usage);
    appendHelpSection(out, "arguments", arguments);
    appendHelpSection(out, "options", options);
    return out;
}

// The facade's CLI functions, in table order. A variable template so a coroutine can index it: GCC 16 rejects a
// consteval call inside a coroutine body.
template <typename Api>
inline constexpr auto CLI_FUNCTIONS = annotatedFunctions<Api, Cli>();

// The argument structs of every command, in table order: the alternatives of CliArguments. A variable template,
// because a consteval call in an alias would be evaluated in a runtime context.
template <typename Api>
consteval auto cliArgumentTypes() {
    static constexpr auto functions = annotatedFunctions<Api, Cli>();
    static_assert(!functions.empty(), "a CLI facade needs at least one command");
    std::array<std::meta::info, functions.size()> types{};
    size_t index{};
    template for (constexpr auto function : functions) {
        types[index++] = std::meta::type_of(std::meta::parameters_of(function)[0]);
    }
    return types;
}

template <typename Api>
inline constexpr auto CLI_ARGUMENT_TYPES = cliArgumentTypes<Api>();

// The table position of one command, so an invocation can be asked about a function by type.
template <typename Api, std::meta::info Function>
consteval size_t cliFunctionIndex() {
    static constexpr auto functions = CLI_FUNCTIONS<Api>;
    for (size_t index = 0; index < functions.size(); ++index) {
        if (functions[index] == Function) {
            return index;
        }
    }
    std::unreachable();
}

template <typename Api, typename Sequence>
struct CliArgumentsByIndex;

template <typename Api, size_t... Index>
struct CliArgumentsByIndex<Api, std::index_sequence<Index...>> {
    using type = std::variant<typename[:CLI_ARGUMENT_TYPES<Api>[Index]:]...>;
};

} // namespace cli_parser

// The arguments of the chosen command, one alternative per command in the order of the table.
template <typename Api>
using CliArguments = typename cli_parser::CliArgumentsByIndex<Api, std::make_index_sequence<cli_parser::CLI_ARGUMENT_TYPES<Api>.size()>>::type;

// What a command line asks for. Nothing has run yet: help and the version are answered from the program, and a
// command's arguments are already parsed but its method is called by runInvocation.
template <typename Api>
struct CliInvocation {
    enum class Kind {
        Run,
        Help,
        CommandHelp,
        Version,
    };

    // Whether the parser chose this function, by type rather than by its words.
    template <std::meta::info Function>
    bool calls() const {
        return index == cli_parser::cliFunctionIndex<Api, Function>();
    }

    Kind kind{Kind::Run};
    size_t index{};                // The chosen command's table position, for Run and CommandHelp
    CliArguments<Api> arguments{}; // The chosen command's parsed arguments; the first command's for help and version
};

// The program's name and version, from its Program annotation.
template <typename Api>
consteval Program programOf() {
    static_assert(hasAnnotation(^^Api, ^^Program), "a CLI facade carries [[=meta::Program{name, version}]]");
    return annotationValue<^^Api, Program>();
}

// What --help prints: the commands, one line each.
template <typename Api>
std::string cliHelpText() {
    static_assert(checkApi<Api>());
    static constexpr Program program = programOf<Api>();
    static constexpr auto functions = annotatedFunctions<Api, Cli>();
    std::vector<cli_parser::HelpRow> commands;
    template for (constexpr auto function : functions) {
        constexpr Cli command = annotationValue<function, Cli>();
        constexpr std::string_view description = descriptionOf(function);
        commands.push_back(cli_parser::HelpRow{std::string{command.command}, description});
    }
    std::string out = std::format("{}\n\nusage: {} <command> [options]\n", program.summary, program.name);
    cli_parser::appendHelpSection(out, "commands", commands);
    const std::vector<cli_parser::HelpRow> options{
        {std::string{HELP_WORD}, "Show this help"},
        {std::string{VERSION_WORD}, "Show the version"},
    };
    cli_parser::appendHelpSection(out, "options", options);
    out += std::format("\nRun {} <command> {} for its options.\n{}\n", program.name, HELP_WORD, program.homepage);
    return out;
}

// What <command> --help prints. index is the command's table position.
template <typename Api>
std::string cliCommandHelpText(size_t index) {
    static constexpr Program program = programOf<Api>();
    static constexpr auto functions = annotatedFunctions<Api, Cli>();
    std::string out;
    size_t position{};
    template for (constexpr auto function : functions) {
        if (position == index) {
            constexpr Cli command = annotationValue<function, Cli>();
            constexpr std::string_view description = descriptionOf(function);
            out = cli_parser::commandHelpText<ArgsOf<function>>(program.name, command.command, description);
        }
        ++position;
    }
    return out;
}

// One line that says where to read more, for after a usage error: the command's help if the line names a command.
template <typename Api>
std::string cliUsageHint(std::span<const std::string_view> args) {
    static constexpr Program program = programOf<Api>();
    const auto table = cli_parser::commandTable<Api>();
    const bool namesCommand = !args.empty() && std::ranges::any_of(table, [&args](const Cli& command) { return args.front() == command.command; });
    if (namesCommand) {
        return std::format("Run {} {} {} for its options.", program.name, args.front(), HELP_WORD);
    }
    return std::format("Run {} {} for the commands.", program.name, HELP_WORD);
}

template <typename Api>
std::string cliVersionText() {
    static constexpr Program program = programOf<Api>();
    return std::format("{} {}", program.name, program.version);
}

namespace cli_parser {

// Parses the chosen command's arguments into the alternative that holds them.
template <typename Api, size_t Index = 0>
Result<void> parseChosenArguments(size_t chosen, CliInvocation<Api>& invocation, std::span<const std::string_view> words) {
    using Args = typename[:CLI_ARGUMENT_TYPES<Api>[Index]:];
    if (chosen == Index) {
        auto parsed = CHECK(parseArgs<Args>(words));
        invocation.arguments.template emplace<Index>(std::move(parsed));
        return {};
    }
    if constexpr (Index + 1 < CLI_ARGUMENT_TYPES<Api>.size()) {
        return parseChosenArguments<Api, Index + 1>(chosen, invocation, words);
    } else {
        return fail(ErrorCode::Internal, "command index out of range");
    }
}

Async<CliOutput> failedOutput(ErrorCode code, std::string message);

// Runs the method of the alternative the parser filled. This is the only place a command is called, and it is not a
// coroutine itself: GCC 16 rejects a consteval call inside a coroutine body. A CLI command returns Async<CliOutput>;
// its lines are what main prints, and an error ends the command.
template <typename Api, size_t Index = 0>
Async<CliOutput> runCliInvocation(Api& api, CliInvocation<Api>& invocation) {
    if (invocation.arguments.index() == Index) {
        constexpr auto function = CLI_FUNCTIONS<Api>[Index];
        using Return = ReturnOf<function>;
        static_assert(isAsync<Return> && std::is_same_v<typename Return::ResultOf::Value, CliOutput>, "a CLI command returns Async<CliOutput>");
        return api.[:function:](std::get<Index>(invocation.arguments));
    }
    if constexpr (Index + 1 < CLI_FUNCTIONS<Api>.size()) {
        return runCliInvocation<Api, Index + 1>(api, invocation);
    } else {
        return failedOutput(ErrorCode::Internal, "command index out of range");
    }
}

inline Async<CliOutput> failedOutput(ErrorCode code, std::string message) {
    co_return fail(code, std::move(message));
}

} // namespace cli_parser

// What the command line names, with its arguments parsed and nothing run. --help first asks for the list of commands,
// and --help anywhere after a command asks for that command's help. --version has to be the first word, so a stray one
// is a usage error like any other unknown argument.
template <typename Api>
Result<CliInvocation<Api>> parseCli(std::span<const std::string_view> args) {
    static_assert(checkApi<Api>());
    if (args.empty() || args.front() == HELP_WORD) {
        return CliInvocation<Api>{.kind = CliInvocation<Api>::Kind::Help};
    }
    if (args.front() == VERSION_WORD) {
        return CliInvocation<Api>{.kind = CliInvocation<Api>::Kind::Version};
    }
    const auto table = cli_parser::commandTable<Api>();
    const auto chosen = std::ranges::find_if(table, [&args](const Cli& command) { return args.front() == command.command; });
    if (chosen == table.end()) {
        return fail(ErrorCode::InvalidArgument, std::format("unknown command: {}", args.front()));
    }
    const auto index = static_cast<size_t>(chosen - table.begin());
    if (std::ranges::find(args, HELP_WORD) != args.end()) {
        return CliInvocation<Api>{.kind = CliInvocation<Api>::Kind::CommandHelp, .index = index};
    }
    CliInvocation<Api> invocation{.kind = CliInvocation<Api>::Kind::Run, .index = index};
    CHECK(cli_parser::parseChosenArguments<Api>(index, invocation, args.subspan(1)));
    return invocation;
}

// Runs the command the parser chose: the step after parseCli, for a caller that answers help and the version itself.
template <typename Api>
Async<CliOutput> runInvocation(Api& api, CliInvocation<Api>& invocation) {
    return cli_parser::runCliInvocation(api, invocation);
}

} // namespace tapin::meta
