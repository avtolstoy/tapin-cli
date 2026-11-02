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

// Plain C and built with Apple Clang: IOKit and CoreFoundation headers are not written for GCC.
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/serial/IOSerialKeys.h>
#include <stdbool.h>

static bool readParentNumber(io_object_t service, CFStringRef key, long* value) {
    const IOOptionBits searchParents = kIORegistryIterateRecursively | kIORegistryIterateParents;
    CFTypeRef property = IORegistryEntrySearchCFProperty(service, kIOServicePlane, key, kCFAllocatorDefault, searchParents);
    if (property == NULL) {
        return false;
    }
    bool isNumber = CFGetTypeID(property) == CFNumberGetTypeID();
    isNumber = isNumber && CFNumberGetValue((CFNumberRef)property, kCFNumberLongType, value);
    CFRelease(property);
    return isNumber;
}

// Finds the port after skipping that many matches. Returns 1 and the /dev/cu.* path if found, 0 if not, and a negative
// IOKit status on failure.
int findUsbSerialPortMacos(unsigned vendorId, unsigned productId, unsigned skip, char* path, unsigned long pathSize) {
    io_iterator_t services = 0;
    kern_return_t status = IOServiceGetMatchingServices(MACH_PORT_NULL, IOServiceMatching(kIOSerialBSDServiceValue), &services);
    if (status != KERN_SUCCESS) {
        return -(int)status;
    }
    int found = 0;
    io_object_t service = 0;
    while (found == 0 && (service = IOIteratorNext(services)) != 0) {
        long vendor = 0;
        long product = 0;
        bool matches = readParentNumber(service, CFSTR("idVendor"), &vendor);
        matches = matches && readParentNumber(service, CFSTR("idProduct"), &product);
        matches = matches && (unsigned long)vendor == vendorId && (unsigned long)product == productId;
        if (matches && skip > 0) {
            --skip;
        } else if (matches) {
            CFTypeRef device = IORegistryEntryCreateCFProperty(service, CFSTR(kIOCalloutDeviceKey), kCFAllocatorDefault, 0);
            if (device != NULL) {
                if (CFStringGetCString((CFStringRef)device, path, (CFIndex)pathSize, kCFStringEncodingUTF8)) {
                    found = 1;
                }
                CFRelease(device);
            }
        }
        IOObjectRelease(service);
    }
    IOObjectRelease(services);
    return found;
}
