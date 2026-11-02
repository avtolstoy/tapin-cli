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

// Plain C and built with Apple Clang: Security and CoreFoundation headers are not written for GCC.
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

// Calls back with the DER bytes of every root macOS trusts. Returns 0, or the Security framework status.
int forEachSystemRootMacos(void (*callback)(const unsigned char* bytes, long size, void* store), void* store) {
    CFArrayRef anchors = NULL;
    OSStatus status = SecTrustCopyAnchorCertificates(&anchors);
    if (status != errSecSuccess) {
        return (int)status;
    }
    for (CFIndex index = 0; index < CFArrayGetCount(anchors); ++index) {
        SecCertificateRef certificate = (SecCertificateRef)CFArrayGetValueAtIndex(anchors, index);
        CFDataRef der = SecCertificateCopyData(certificate);
        if (der != NULL) {
            callback(CFDataGetBytePtr(der), CFDataGetLength(der), store);
            CFRelease(der);
        }
    }
    CFRelease(anchors);
    return 0;
}
