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

#include "console/console.h"
#include "core/async.h"
#include "core/version.h"
#include "flash/flasher.h"
#include "io/loop.h"
#include "io/terminal.h"
#include "meta/annotations.h"
#include "meta/cli.h"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tapin::cli {

// clang-format would pull each annotation back onto the line of what it annotates.
// clang-format off
struct UpdateArgs {
    [[= meta::Description{"Lens model (e.g. F012, F013); read from lens by default"}]]
    std::optional<std::string> model;

    [[= meta::Description{"Serial port; auto-detect the console by default"}]]
    std::optional<std::string> port;

    [[= meta::Description{"Skip confirmation prompts"}]]
    bool yes{};

    [[= meta::Description{"Continue if the lens does not respond"}]]
    bool recovery{};

    [[= meta::Description{"Write default adjustment data"}]]
    bool reset{};

    [[= meta::Description{"Hexdump all traffic"}]]
    bool debug{};
};

struct FixArgs {
    [[= meta::Description{"Lens model (e.g. F012, F013); read from lens by default"}]]
    std::optional<std::string> model;

    [[= meta::Description{"Serial port; auto-detect the console by default"}]]
    std::optional<std::string> port;

    [[= meta::Description{"Skip confirmation prompts"}]]
    bool yes{};

    [[= meta::Description{"Allow images not tested on a camera"}]]
    bool experimental{};

    [[= meta::Description{"Continue if the lens does not respond"}]]
    bool recovery{};

    [[= meta::Description{"Write default adjustment data"}]]
    bool reset{};

    [[= meta::Description{"Hexdump all traffic"}]]
    bool debug{};
};

struct DownloadArgs {
    [[= meta::Positional{}]]
    [[= meta::Description{"Lens model (e.g. F012, F013)"}]]
    std::string model;

    [[= meta::Description{"Lens mount: canon, nikon or sony; canon by default"}]]
    std::optional<std::string> mount;

    [[= meta::Description{"Output file"}]]
    std::optional<std::string> output;
};

struct PatchArgs {
    [[= meta::Positional{}]]
    [[= meta::Description{"Lens model (e.g. F012, F013)"}]]
    std::string model;

    [[= meta::Description{"Output file"}]]
    std::optional<std::string> output;
};

struct FlashArgs {
    [[= meta::Positional{}]]
    [[= meta::Description{"A .tfwf or a decrypted .dec image"}]]
    std::string image;

    [[= meta::Description{"Key for a .tfwf: 64 hex characters from Tamron's lensinfo XML; from the file name by default"}]]
    std::optional<std::string> key;

    [[= meta::Description{"Serial port; auto-detect the console by default"}]]
    std::optional<std::string> port;

    [[= meta::Description{"Skip confirmation prompts"}]]
    bool yes{};

    [[= meta::Description{"Flash an image made for another lens"}]]
    bool force{};

    [[= meta::Description{"Continue if the lens does not respond"}]]
    bool recovery{};

    [[= meta::Description{"Write default adjustment data"}]]
    bool reset{};

    [[= meta::Description{"Hexdump all traffic"}]]
    bool debug{};
};

struct InfoArgs {
    [[= meta::Description{"Serial port; auto-detect the console by default"}]]
    std::optional<std::string> port;

    [[= meta::Description{"Hexdump all traffic"}]]
    bool debug{};
};

struct AdjustFileArgs {
    [[= meta::Positional{}]]
    [[= meta::Description{"The file"}]]
    std::string file;

    [[= meta::Description{"Serial port; auto-detect the console by default"}]]
    std::optional<std::string> port;

    [[= meta::Description{"Hexdump all traffic"}]]
    bool debug{};
};

struct DecryptArgs {
    [[= meta::Positional{}]]
    [[= meta::Description{"A .tfwf file"}]]
    std::string input;

    [[= meta::Positional{}]]
    [[= meta::Description{"The .dec file to write"}]]
    std::string output;

    [[= meta::Description{"64 hex characters from Tamron's lensinfo XML; from the file name by default"}]]
    std::optional<std::string> key;

    [[= meta::Description{"Lens model, with --mount and --version instead of --key"}]]
    std::optional<std::string> model;

    [[= meta::Description{"Lens mount: canon, nikon or sony"}]]
    std::optional<std::string> mount;

    [[= meta::Description{"Four digits (e.g. 0400)"}]]
    std::optional<std::string> version;
};

class [[= meta::Program{"tapin-cli", VERSION, "CLI for the Tamron TAP-in Console.", "https://github.com/avtolstoy/tapin-cli"}]] Commands {
public:
    explicit Commands(io::IoLoop& io, console::ConsoleOptions consoleOptions = {});

    [[= meta::Cli{"update"}]]
    [[= meta::Description{"Flash the latest stock Tamron firmware to the lens on the console"}]]
    Async<meta::CliOutput> updateLens(UpdateArgs args);

    [[= meta::Cli{"fix"}]]
    [[= meta::Description{"Flash patched firmware so older Canon bodies can drive the lens"}]]
    Async<meta::CliOutput> fixLens(FixArgs args);

    [[= meta::Cli{"info"}]]
    [[= meta::Description{"Show the console and the lens"}]]
    Async<meta::CliOutput> showInfo(InfoArgs args);

    [[= meta::Cli{"download"}]]
    [[= meta::Description{"Save the latest stock firmware to a file, no flashing"}]]
    Async<meta::CliOutput> downloadImage(DownloadArgs args);

    [[= meta::Cli{"patch"}]]
    [[= meta::Description{"Save patched firmware to a file, no flashing"}]]
    Async<meta::CliOutput> writePatchedImage(PatchArgs args);

    [[= meta::Cli{"flash"}]]
    [[= meta::Description{"Flash an image file"}]]
    Async<meta::CliOutput> flashFile(FlashArgs args);

    [[= meta::Cli{"get-adj"}]]
    [[= meta::Description{"Save adjustment data to a file"}]]
    Async<meta::CliOutput> saveAdjust(AdjustFileArgs args);

    [[= meta::Cli{"set-adj"}]]
    [[= meta::Description{"Write adjustment data from a file"}]]
    Async<meta::CliOutput> loadAdjust(AdjustFileArgs args);

    [[= meta::Cli{"decrypt"}]]
    [[= meta::Description{"Decrypt a .tfwf file"}]]
    Async<meta::CliOutput> decryptFile(DecryptArgs args);
    // clang-format on

private:
    struct InstallRequest {
        flash::UpdateRequest update;     // Which firmware, and how to flash it
        std::optional<std::string> port; // Serial port, or nothing to find the console
        bool yes;                        // Skip the confirmation
        bool debug;                      // Hexdump all traffic
    };

    Result<std::unique_ptr<console::Console>> openConsole(const std::optional<std::string>& port, bool debug);
    Async<meta::CliOutput> installFirmware(InstallRequest request);
    Async<bool> confirmFlash(std::string prompt);

    io::IoLoop& io_;                         // Owned by the caller, runs every command
    console::ConsoleOptions consoleOptions_; // Timing of the console
    io::Terminal terminal_;                  // Answers confirmation prompts
};

// The CLI's mode of main: run the command on the io loop, print its lines and return its exit code.
int runCommand(std::span<const std::string_view> args);

// The command line without argv[0].
std::vector<std::string_view> argumentsOf(int argc, char** argv);

} // namespace tapin::cli
