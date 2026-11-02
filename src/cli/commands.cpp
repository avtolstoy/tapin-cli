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

#include "console/wire.h"
#include "firmware/download.h"
#include "firmware/image.h"
#include "firmware/short_frame_patch.h"
#include "flash/flasher.h"
#include "io/file.h"
#include "io/https.h"
#include "io/usb_serial_search.h"

#include <array>
#include <cstdio>
#include <filesystem>
#include <format>
#include <print>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace tapin::cli {

namespace {

using firmware::Mount;

constexpr std::string_view ENCRYPTED_IMAGE_EXTENSION{".tfwf"};

int printError(const Error& error) {
    std::println(stderr, "error: {}", error.message);
    return static_cast<int>(meta::ExitCode::Failure);
}

int printOutput(const Result<meta::CliOutput>& outcome) {
    if (!outcome) {
        return printError(outcome.error());
    }
    for (const std::string& line : outcome->lines()) {
        std::println("{}", line);
    }
    return static_cast<int>(outcome->exitCode);
}

meta::CliOutput abortedOutput() {
    meta::CliOutput output;
    output.line("aborted");
    output.exitCode = meta::ExitCode::Failure;
    return output;
}

// The key given on the command line, or the one in the file name.
Result<firmware::XorKey> keyForImageFile(const std::filesystem::path& file, const std::optional<std::string>& serverKey) {
    if (serverKey) {
        return firmware::keyFromServer(*serverKey);
    }
    return firmware::keyFromImageName(file.stem().string());
}

// A .tfwf is encrypted, anything else is the image as it is. The record CRCs tell a right key from a wrong one.
Result<firmware::FirmwareImage> readImageFile(const std::string& path, const std::optional<std::string>& serverKey) {
    firmware::Bytes bytes = CHECK(io::readWholeFile(path));
    const std::filesystem::path file{path};
    if (file.extension() != ENCRYPTED_IMAGE_EXTENSION) {
        return firmware::FirmwareImage::parse(std::move(bytes));
    }
    const firmware::XorKey key = CHECK(keyForImageFile(file, serverKey));
    Result<firmware::FirmwareImage> image = firmware::decryptImage(bytes, key);
    if (!image && !serverKey) {
        return fail(ErrorCode::InvalidArgument, std::format("the key from the name of {} does not decrypt it, give --key", path));
    }
    return image;
}

// Started by double clicking in Explorer: this process is alone on its console, and the window closes when it exits.
bool startedByDoubleClick() {
#if defined(_WIN32)
    std::array<DWORD, 2> processes{};
    return GetConsoleProcessList(processes.data(), static_cast<DWORD>(processes.size())) == 1;
#else
    return false;
#endif
}

} // namespace

Commands::Commands(io::IoLoop& io, console::ConsoleOptions consoleOptions)
        : io_(io),
          consoleOptions_(consoleOptions) {
}

Result<std::unique_ptr<console::Console>> Commands::openConsole(const std::optional<std::string>& port, bool debug) {
    std::string portName;
    if (port) {
        portName = *port;
    } else {
        const std::vector<std::string> found = CHECK(io::findPortsOfUsbDevice(console::USB_VENDOR_ID, console::USB_PRODUCT_ID));
        if (found.empty()) {
            return fail(
                ErrorCode::NotFound,
                std::format("TAP-in Console not found (USB {:04X}:{:04X}). Is it plugged in?", console::USB_VENDOR_ID, console::USB_PRODUCT_ID)
            );
        }
        if (found.size() > 1) {
            std::string names;
            for (const std::string& name : found) {
                names += names.empty() ? name : std::format(", {}", name);
            }
            return fail(ErrorCode::InvalidArgument, std::format("found {} consoles, pick one with --port: {}", found.size(), names));
        }
        portName = found.front();
    }
    std::println(stderr, "using {}", portName);
    console::ConsoleOptions options = consoleOptions_;
    options.debug = debug;
    return console::Console::open(io_, portName, options);
}

Async<bool> Commands::confirmFlash(std::string prompt) {
    if (!io::Terminal::isInteractive()) {
        std::println(stderr, "refusing to go on without --yes, this is not a terminal");
        co_return false;
    }
    co_return co_await terminal_.confirm(std::move(prompt));
}

Async<meta::CliOutput> Commands::installFirmware(InstallRequest request) {
    auto console = co_await openConsole(request.port, request.debug);
    auto https = co_await io::HttpsClient::create(io_);
    flash::Flasher flasher{*console, io_};
    const flash::PreparedUpdate update = co_await flasher.prepareUpdate(*https, request.update);

    const bool patched = request.update.kind == flash::FirmwareKind::Patched;
    const std::string_view action = patched ? "Flash the patched firmware." : "Flash stock Tamron firmware.";
    if (!request.yes && !co_await confirmFlash(std::format("{} Don't touch the lens or the USB cable until it finishes.", action))) {
        co_return abortedOutput();
    }
    co_await flasher.flashImage(update.image, request.update.options, update.lens);

    meta::CliOutput output;
    output.line("");
    if (patched) {
        output.line("Done. Put the lens on the camera and check the aperture and AF.");
        output.line("`tapin-cli update` goes back to stock Tamron firmware.");
    } else {
        output.line("Done. The lens has stock Tamron firmware.");
    }
    co_return std::move(output);
}

Async<meta::CliOutput> Commands::fixLens(FixArgs args) {
    InstallRequest request{
        .update = {
            .kind = flash::FirmwareKind::Patched,
            .model = std::move(args.model),
            .experimental = args.experimental,
            .options = {.recovery = args.recovery, .reset = args.reset},
        },
        .port = std::move(args.port),
        .yes = args.yes,
        .debug = args.debug,
    };
    co_return co_await installFirmware(std::move(request));
}

Async<meta::CliOutput> Commands::updateLens(UpdateArgs args) {
    InstallRequest request{
        .update = {
            .kind = flash::FirmwareKind::Stock,
            .model = std::move(args.model),
            .experimental = false,
            .options = {.recovery = args.recovery, .reset = args.reset},
        },
        .port = std::move(args.port),
        .yes = args.yes,
        .debug = args.debug,
    };
    co_return co_await installFirmware(std::move(request));
}

Async<meta::CliOutput> Commands::downloadImage(DownloadArgs args) {
    Mount mount = Mount::Canon;
    if (args.mount) {
        mount = co_await firmware::mountFromName(*args.mount);
    }
    auto https = co_await io::HttpsClient::create(io_);
    const firmware::DownloadedImage downloaded = co_await firmware::downloadLatestImage(*https, args.model, mount);
    const std::string path = args.output ? *args.output : std::format("{}.dec", downloaded.name);
    co_await io::writeWholeFile(path, downloaded.image.bytes());
    meta::CliOutput output;
    output.line("wrote {}, sha256 {}", path, co_await downloaded.image.sha256());
    co_return std::move(output);
}

Async<meta::CliOutput> Commands::writePatchedImage(PatchArgs args) {
    auto https = co_await io::HttpsClient::create(io_);
    const firmware::DownloadedImage stock = co_await firmware::downloadLatestImage(*https, args.model, Mount::Canon);
    const firmware::PatchedImage patched = co_await firmware::patchShortFrameChecks(stock.image);
    const std::string path = args.output ? *args.output : std::format("{}_patched.dec", stock.name);
    co_await io::writeWholeFile(path, patched.image.bytes());
    meta::CliOutput output;
    output.line("wrote {}, sha256 {}", path, patched.sha256);
    output.line("status: {}", patched.status);
    co_return std::move(output);
}

Async<meta::CliOutput> Commands::flashFile(FlashArgs args) {
    const firmware::FirmwareImage image = co_await readImageFile(args.image, args.key);
    std::println("{}: firmware {}, {} record(s)", args.image, image.versionText(), image.records().size());
    if (!args.yes && !co_await confirmFlash(std::format("Flash {} to the lens.", args.image))) {
        co_return abortedOutput();
    }
    auto console = co_await openConsole(args.port, args.debug);
    flash::Flasher flasher{*console, io_};
    const flash::FlashOptions options{.force = args.force, .recovery = args.recovery, .reset = args.reset};
    co_await flasher.flashImage(image, options, std::nullopt);
    co_return meta::CliOutput{};
}

Async<meta::CliOutput> Commands::showInfo(InfoArgs args) {
    auto console = co_await openConsole(args.port, args.debug);
    const console::AdapterInfo adapter = co_await console->readAdapterInfo();
    const console::LensInfo lens = co_await console->connectLens();
    meta::CliOutput output;
    output.line("adapter:");
    output.text(adapter.describe());
    output.line("lens:");
    output.text(lens.describe());
    co_return std::move(output);
}

Async<meta::CliOutput> Commands::saveAdjust(AdjustFileArgs args) {
    auto console = co_await openConsole(args.port, args.debug);
    co_await console->connectLens();
    const firmware::Bytes data = co_await console->readAdjustData();
    co_await io::writeWholeFile(args.file, data);
    meta::CliOutput output;
    output.line("wrote {} ({} bytes)", args.file, data.size());
    co_return std::move(output);
}

Async<meta::CliOutput> Commands::loadAdjust(AdjustFileArgs args) {
    const firmware::Bytes data = co_await io::readWholeFile(args.file);
    auto console = co_await openConsole(args.port, args.debug);
    co_await console->connectLens();
    co_await console->writeAdjustData(data);
    meta::CliOutput output;
    output.line("wrote {} bytes from {}", data.size(), args.file);
    co_return std::move(output);
}

Async<meta::CliOutput> Commands::decryptFile(DecryptArgs args) {
    const firmware::Bytes tfwf = co_await io::readWholeFile(args.input);
    firmware::XorKey key{};
    const int nameParts = int{args.model.has_value()} + int{args.mount.has_value()} + int{args.version.has_value()};
    if (nameParts > 0 && nameParts < 3) {
        co_return fail(ErrorCode::InvalidArgument, "--model, --mount and --version go together");
    }
    if (nameParts == 3) {
        const Mount mount = co_await firmware::mountFromName(*args.mount);
        key = co_await firmware::keyFromNameParts(*args.model, firmware::mountCode(mount), *args.version);
    } else {
        key = co_await keyForImageFile(args.input, args.key);
    }
    const firmware::FirmwareImage image = co_await firmware::decryptImage(tfwf, key);
    co_await io::writeWholeFile(args.output, image.bytes());
    meta::CliOutput output;
    output.line("wrote {} ({} record(s), {} bytes)", args.output, image.records().size(), image.bytes().size());
    co_return std::move(output);
}

std::vector<std::string_view> argumentsOf(int argc, char** argv) {
    if (argc <= 1) {
        return {};
    }
    return std::vector<std::string_view>(argv + 1, argv + argc);
}

int runCommand(std::span<const std::string_view> args) {
    // Progress and prompts show up at once, on a pipe too. Unbuffered because Windows has no line buffering.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    if (args.empty() && startedByDoubleClick()) {
        std::print("{}\nThis is a command line tool: run it from a terminal.\nPress Enter to close...", meta::cliHelpText<Commands>());
        (void)std::getchar(); // Any input closes the window, including none
        return static_cast<int>(meta::ExitCode::Usage);
    }
    auto invocation = meta::parseCli<Commands>(args);
    if (!invocation) {
        std::println(stderr, "error: {}\n{}", invocation.error().message, meta::cliUsageHint<Commands>(args));
        return static_cast<int>(meta::ExitCode::Usage);
    }
    switch (invocation->kind) {
    case meta::CliInvocation<Commands>::Kind::Help:
        std::print("{}", meta::cliHelpText<Commands>());
        return static_cast<int>(meta::ExitCode::Ok);
    case meta::CliInvocation<Commands>::Kind::CommandHelp:
        std::print("{}", meta::cliCommandHelpText<Commands>(invocation->index));
        return static_cast<int>(meta::ExitCode::Ok);
    case meta::CliInvocation<Commands>::Kind::Version:
        std::println("{}", meta::cliVersionText<Commands>());
        return static_cast<int>(meta::ExitCode::Ok);
    case meta::CliInvocation<Commands>::Kind::Run:
        break;
    }
    io::IoLoop io;
    Commands commands{io};
    return printOutput(io.waitFor(meta::runInvocation(commands, *invocation)));
}

} // namespace tapin::cli
