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

#include "cli/commands.h"
#include "fake_console.h"
#include "io/file.h"
#include "testing.h"

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

using namespace tapin;
using cli::Commands;

TEST_CASE("the parser takes fix with its flags") {
    constexpr std::array<std::string_view, 4> WORDS{"fix", "--model", "F013", "--yes"};
    EXPECT_VALUE(invocation, meta::parseCli<Commands>(WORDS));
    EXPECT(invocation.calls<^^Commands::fixLens>());
}

TEST_CASE("the parser refuses an unknown command") {
    constexpr std::array<std::string_view, 1> WORDS{"nope"};
    EXPECT_FALSE(meta::parseCli<Commands>(WORDS).has_value());
}

TEST_CASE("the parser wants the image for flash") {
    constexpr std::array<std::string_view, 1> WORDS{"flash"};
    EXPECT_FALSE(meta::parseCli<Commands>(WORDS).has_value());
}

TEST_CASE("--help lists every command and none of their options") {
    const std::string help = meta::cliHelpText<Commands>();
    for (std::string_view command : {"update", "fix", "download", "patch", "flash", "info", "get-adj", "set-adj", "decrypt"}) {
        EXPECT(help.contains(command));
    }
    EXPECT(help.contains("--version"));
    EXPECT(help.contains("https://github.com/avtolstoy/tamron-tapin-cli"));
    EXPECT_FALSE(help.contains("--model"));
}

TEST_CASE("--help after a command asks for that command's help") {
    constexpr std::array<std::string_view, 2> WORDS{"flash", "--help"};
    EXPECT_VALUE(invocation, meta::parseCli<Commands>(WORDS));
    EXPECT(invocation.kind == meta::CliInvocation<Commands>::Kind::CommandHelp);
    EXPECT(invocation.calls<^^Commands::flashFile>());
}

TEST_CASE("--help after a command needs none of its arguments") {
    constexpr std::array<std::string_view, 3> WORDS{"decrypt", "only-one-file", "--help"};
    EXPECT_VALUE(invocation, meta::parseCli<Commands>(WORDS));
    EXPECT(invocation.kind == meta::CliInvocation<Commands>::Kind::CommandHelp);
}

TEST_CASE("--help after an unknown command is still an unknown command") {
    constexpr std::array<std::string_view, 2> WORDS{"nope", "--help"};
    EXPECT_FALSE(meta::parseCli<Commands>(WORDS).has_value());
}

TEST_CASE("a command's help shows its usage, arguments and options") {
    constexpr std::array<std::string_view, 2> WORDS{"flash", "--help"};
    EXPECT_VALUE(invocation, meta::parseCli<Commands>(WORDS));
    const std::string help = meta::cliCommandHelpText<Commands>(invocation.index);
    EXPECT(help.contains("usage: tapin-cli flash <image> [options]"));
    EXPECT(help.contains("<image>"));
    EXPECT(help.contains("--key <key>"));
    EXPECT(help.contains("--yes  "));
    EXPECT(help.contains("--help"));
    EXPECT_FALSE(help.contains("--experimental"));
}

TEST_CASE("a usage error points at the right help") {
    constexpr std::array<std::string_view, 1> KNOWN{"flash"};
    constexpr std::array<std::string_view, 1> UNKNOWN{"nope"};
    EXPECT(meta::cliUsageHint<Commands>(KNOWN).contains("tapin-cli flash --help"));
    EXPECT(meta::cliUsageHint<Commands>(UNKNOWN).contains("tapin-cli --help"));
}

TEST_CASE("--version names the tool") {
    constexpr std::array<std::string_view, 1> WORDS{"--version"};
    EXPECT_VALUE(invocation, meta::parseCli<Commands>(WORDS));
    EXPECT(invocation.kind == meta::CliInvocation<Commands>::Kind::Version);
}

TEST_CASE("runCommand exit codes") {
    constexpr std::array<std::string_view, 1> VERSION{"--version"};
    constexpr std::array<std::string_view, 1> UNKNOWN{"nope"};
    constexpr std::array<std::string_view, 3> MISSING_FILE{"decrypt", "/nonexistent/in.tfwf", "/nonexistent/out.dec"};
    EXPECT(cli::runCommand(VERSION) == static_cast<int>(meta::ExitCode::Ok));
    EXPECT(cli::runCommand(UNKNOWN) == static_cast<int>(meta::ExitCode::Usage));
    EXPECT(cli::runCommand(MISSING_FILE) == static_cast<int>(meta::ExitCode::Failure));
}

TEST_CASE("decrypt wants all of --model, --mount and --version or none") {
    const test::TempDir directory;
    const std::string input = (directory.path / "F013E0_0400.tfwf").string();
    EXPECT(io::writeWholeFile(input, test::makeImageBytes("F013E0", 4)).has_value());
    io::IoLoop io;
    Commands commands{io};
    cli::DecryptArgs args;
    args.input = input;
    args.output = (directory.path / "out.dec").string();
    args.model = "F013";
    EXPECT(test::failedSaying(io.waitFor(commands.decryptFile(std::move(args))), "go together"));
}

TEST_CASE("decrypt uses the key from the file name") {
    const test::TempDir directory;
    const firmware::Bytes plain = test::makeImageBytes("F013E0", 4);
    EXPECT_VALUE(key, firmware::keyFromImageName("F013E0_0400"));
    const std::string input = (directory.path / "F013E0_0400.tfwf").string();
    const std::string output = (directory.path / "out.dec").string();
    EXPECT(io::writeWholeFile(input, test::encryptImage(plain, key)).has_value());
    io::IoLoop io;
    Commands commands{io};
    cli::DecryptArgs args;
    args.input = input;
    args.output = output;
    EXPECT(io.waitFor(commands.decryptFile(std::move(args))).has_value());
    EXPECT_VALUE(written, io::readWholeFile(output));
    EXPECT(written == plain);
}

#if !defined(_WIN32)

namespace {

constexpr uint8_t NIKON_MOUNT{2};
constexpr std::string_view F013_SERVER_KEY{"035E52413C4040595F28645E5C29725543550D5548762B5346494032595E5D11"};

Result<std::string> writeEncryptedImage(const test::TempDir& directory, std::string_view name, const firmware::Bytes& plain) {
    const firmware::XorKey key = CHECK(firmware::keyFromImageName("F013E0_0400"));
    const std::string path = (directory.path / name).string();
    CHECK(io::writeWholeFile(path, test::encryptImage(plain, key)));
    return path;
}

Result<meta::CliOutput> flashFileOnFake(test::FakeConsole& fake, std::string image, std::optional<std::string> key) {
    fake.start();
    io::IoLoop io;
    Commands commands{io, test::FAST_OPTIONS};
    cli::FlashArgs args;
    args.image = std::move(image);
    args.key = std::move(key);
    args.port = fake.portName();
    args.yes = true;
    Result<meta::CliOutput> output = io.waitFor(commands.flashFile(std::move(args)));
    fake.stop();
    return output;
}

} // namespace

TEST_CASE("flash decrypts a .tfwf with the key from its name") {
    const test::TempDir directory;
    const firmware::Bytes plain = test::makeImageBytes("F013E0", 4);
    EXPECT_VALUE(path, writeEncryptedImage(directory, "F013E0_0400.tfwf", plain));
    test::FakeConsole fake;
    EXPECT(flashFileOnFake(fake, path, std::nullopt).has_value());
    EXPECT(fake.received == plain);
}

TEST_CASE("flash decrypts a renamed .tfwf with --key") {
    const test::TempDir directory;
    const firmware::Bytes plain = test::makeImageBytes("F013E0", 4);
    EXPECT_VALUE(path, writeEncryptedImage(directory, "lens.tfwf", plain));
    test::FakeConsole fake;
    EXPECT(flashFileOnFake(fake, path, std::string{F013_SERVER_KEY}).has_value());
    EXPECT(fake.received == plain);
}

TEST_CASE("flash asks for --key and sends nothing when the name gives the wrong key") {
    const test::TempDir directory;
    EXPECT_VALUE(path, writeEncryptedImage(directory, "F013E0_0500.tfwf", test::makeImageBytes("F013E0", 4)));
    test::FakeConsole fake;
    EXPECT(test::failedSaying(flashFileOnFake(fake, path, std::nullopt), "--key"));
    EXPECT(fake.received.empty());
}

TEST_CASE("fix refuses a console that is not Canon before it downloads anything") {
    test::FakeConsole fake;
    fake.mount = NIKON_MOUNT;
    fake.model = "A025";
    fake.start();
    io::IoLoop io;
    Commands commands{io, test::FAST_OPTIONS};
    cli::FixArgs args;
    args.port = fake.portName();
    args.yes = true;
    EXPECT(test::failedWith(io.waitFor(commands.fixLens(std::move(args))), ErrorCode::WrongImage));
}

TEST_CASE("update refuses a model that is not the lens on the console") {
    test::FakeConsole fake;
    fake.start();
    io::IoLoop io;
    Commands commands{io, test::FAST_OPTIONS};
    cli::UpdateArgs args;
    args.model = "F016";
    args.port = fake.portName();
    args.yes = true;
    EXPECT(test::failedWith(io.waitFor(commands.updateLens(std::move(args))), ErrorCode::WrongImage));
}

TEST_CASE("info shows the lens") {
    test::FakeConsole fake;
    fake.start();
    io::IoLoop io;
    Commands commands{io, test::FAST_OPTIONS};
    cli::InfoArgs args;
    args.port = fake.portName();
    EXPECT_VALUE(output, io.waitFor(commands.showInfo(std::move(args))));
    const bool sawModel = std::ranges::any_of(output.lines(), [](const std::string& line) { return line.contains("F013"); });
    EXPECT(sawModel);
}

// Two fakes: Console::open takes the port exclusively, and a pty stays exclusive for as long as the fake holds it open.
TEST_CASE("get-adj and set-adj carry the adjustment data through a file") {
    constexpr uint8_t DIFFERENT_VC_MODE{9};
    const test::TempDir directory;
    const std::string file = (directory.path / "adj.bin").string();
    test::FakeConsole source;
    test::FakeConsole destination;
    destination.adjust[1] = DIFFERENT_VC_MODE;
    source.start();
    destination.start();
    io::IoLoop io;
    Commands commands{io, test::FAST_OPTIONS};
    cli::AdjustFileArgs saveArgs;
    saveArgs.file = file;
    saveArgs.port = source.portName();
    EXPECT(io.waitFor(commands.saveAdjust(std::move(saveArgs))).has_value());
    cli::AdjustFileArgs loadArgs;
    loadArgs.file = file;
    loadArgs.port = destination.portName();
    EXPECT(io.waitFor(commands.loadAdjust(std::move(loadArgs))).has_value());
    source.stop();
    destination.stop();
    EXPECT(destination.adjust == source.adjust);
}

#endif
