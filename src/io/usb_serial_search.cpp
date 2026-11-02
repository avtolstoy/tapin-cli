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

#include "io/usb_serial_search.h"

#include <array>
#include <cstddef>
#include <format>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <string_view>

#include <windows.h>
#elif !defined(__APPLE__)
#include <charconv>
#include <filesystem>
#include <fstream>
#include <optional>
#include <system_error>
#endif

namespace tapin::io {

#if defined(_WIN32)

namespace {

constexpr DWORD MAX_REGISTRY_NAME_CHARACTERS{256}; // Longest key or value name in the registry is 255 characters
constexpr const char* ACTIVE_PORTS_KEY{"HARDWARE\\DEVICEMAP\\SERIALCOMM"};

// Enum\USB keeps entries for unplugged devices, SERIALCOMM only lists ports that exist right now.
bool isActivePort(std::string_view portName) {
    HKEY key{};
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, ACTIVE_PORTS_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    bool active = false;
    for (DWORD index = 0; !active; ++index) {
        std::array<char, MAX_REGISTRY_NAME_CHARACTERS> name{};
        std::array<char, MAX_REGISTRY_NAME_CHARACTERS> value{};
        DWORD nameSize = MAX_REGISTRY_NAME_CHARACTERS;
        DWORD valueSize = MAX_REGISTRY_NAME_CHARACTERS;
        auto* valueBytes = reinterpret_cast<LPBYTE>(value.data());
        const LSTATUS status = RegEnumValueA(key, index, name.data(), &nameSize, nullptr, nullptr, valueBytes, &valueSize);
        if (status != ERROR_SUCCESS) {
            break;
        }
        active = portName == value.data();
    }
    RegCloseKey(key);
    return active;
}

} // namespace

// A USB serial device keeps its COM name in Enum\USB\VID_xxxx&PID_xxxx\<instance>\Device Parameters\PortName.
Result<std::vector<std::string>> findPortsOfUsbDevice(unsigned vendorId, unsigned productId) {
    const std::string devicePath = std::format("SYSTEM\\CurrentControlSet\\Enum\\USB\\VID_{:04X}&PID_{:04X}", vendorId, productId);
    std::vector<std::string> ports;
    HKEY device{};
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, devicePath.c_str(), 0, KEY_READ, &device) != ERROR_SUCCESS) {
        return ports;
    }
    for (DWORD index = 0;; ++index) {
        std::array<char, MAX_REGISTRY_NAME_CHARACTERS> instance{};
        DWORD instanceSize = MAX_REGISTRY_NAME_CHARACTERS;
        if (RegEnumKeyExA(device, index, instance.data(), &instanceSize, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
            break;
        }
        const std::string parametersPath = std::format("{}\\Device Parameters", instance.data());
        std::array<char, MAX_REGISTRY_NAME_CHARACTERS> portName{};
        DWORD portNameSize = MAX_REGISTRY_NAME_CHARACTERS;
        const LSTATUS status = RegGetValueA(device, parametersPath.c_str(), "PortName", RRF_RT_REG_SZ, nullptr, portName.data(), &portNameSize);
        if (status == ERROR_SUCCESS && isActivePort(portName.data())) {
            ports.push_back(std::format("\\\\.\\{}", portName.data()));
        }
    }
    RegCloseKey(device);
    return ports;
}

#elif defined(__APPLE__)

extern "C" int findUsbSerialPortMacos(unsigned vendorId, unsigned productId, unsigned skip, char* path, unsigned long pathSize);

namespace {

constexpr size_t MAX_DEVICE_PATH_CHARACTERS{1024}; // MAXPATHLEN on macOS

} // namespace

Result<std::vector<std::string>> findPortsOfUsbDevice(unsigned vendorId, unsigned productId) {
    std::vector<std::string> ports;
    while (true) {
        std::array<char, MAX_DEVICE_PATH_CHARACTERS> path{};
        const int status = findUsbSerialPortMacos(vendorId, productId, static_cast<unsigned>(ports.size()), path.data(), path.size());
        if (status < 0) {
            return fail(ErrorCode::IoFailed, std::format("IOKit serial port search failed: {}", status));
        }
        if (status == 0) {
            return ports;
        }
        ports.emplace_back(path.data());
    }
}

#else

namespace {

namespace fs = std::filesystem;

constexpr int HEX_BASE{16};

std::optional<unsigned> readHexFile(const fs::path& path) {
    std::ifstream file{path};
    std::string text;
    if (!(file >> text)) {
        return std::nullopt;
    }
    unsigned value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, HEX_BASE);
    if (error != std::errc{}) {
        return std::nullopt;
    }
    return value;
}

// /sys/class/tty/<name>/device links into the USB device tree, and the USB device is the parent that has idVendor.
bool portBelongsToUsbDevice(const fs::path& port, unsigned vendorId, unsigned productId) {
    std::error_code error;
    fs::path device = fs::canonical(port / "device", error);
    if (error) {
        return false;
    }
    while (device.has_relative_path()) {
        const std::optional<unsigned> vendor = readHexFile(device / "idVendor");
        if (vendor) {
            return *vendor == vendorId && readHexFile(device / "idProduct") == productId;
        }
        device = device.parent_path();
    }
    return false;
}

} // namespace

Result<std::vector<std::string>> findPortsOfUsbDevice(unsigned vendorId, unsigned productId) {
    std::error_code error;
    fs::directory_iterator port{"/sys/class/tty", error};
    if (error) {
        return fail(ErrorCode::IoFailed, std::format("cannot read /sys/class/tty: {}", error.message()));
    }
    std::vector<std::string> ports;
    // increment() with an error_code: the plain ++ throws, which aborts here, if a port goes away during the walk.
    for (const fs::directory_iterator end; port != end; port.increment(error)) {
        if (portBelongsToUsbDevice(port->path(), vendorId, productId)) {
            ports.push_back(std::format("/dev/{}", port->path().filename().string()));
        }
    }
    return ports;
}

#endif

} // namespace tapin::io
