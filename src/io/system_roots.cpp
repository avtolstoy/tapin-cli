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

#if defined(_WIN32)
// Before OpenSSL: wincrypt.h defines names like X509_NAME that OpenSSL undefines again.
#include <windows.h>

#include <wincrypt.h>
#endif

#include "io/system_roots.h"

#include <array>
#include <cstdlib>

#include <openssl/ssl.h>
#include <openssl/x509.h>

#if defined(__APPLE__)
extern "C" int forEachSystemRootMacos(void (*callback)(const unsigned char* bytes, long size, void* store), void* store);
#endif

namespace tapin::io {

namespace {

#if defined(__APPLE__)

void addRootToStore(const unsigned char* bytes, long size, void* store) {
    X509* parsed = d2i_X509(nullptr, &bytes, size);
    if (parsed != nullptr) {
        X509_STORE_add_cert(static_cast<X509_STORE*>(store), parsed);
        X509_free(parsed);
    }
}

#elif !defined(_WIN32)

// OpenSSL only looks under its own build prefix, which means nothing on the machine that got this binary. So this looks
// where each distro keeps its bundle.
constexpr std::array<const char*, 6> CA_BUNDLES{
    "/etc/ssl/certs/ca-certificates.crt",                // Debian, Ubuntu, Arch, Gentoo
    "/etc/pki/tls/certs/ca-bundle.crt",                  // Fedora, RHEL
    "/etc/ssl/ca-bundle.pem",                            // openSUSE
    "/etc/pki/tls/cacert.pem",                           // OpenELEC
    "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", // RHEL and CentOS
    "/etc/ssl/cert.pem",                                 // Alpine
};

#endif

} // namespace

#if defined(_WIN32)

// Only sees roots already installed. Windows downloads others on demand, for programs that use its own TLS.
Result<void> addSystemRoots(ssl_ctx_st* context) {
    HCERTSTORE store = CertOpenSystemStoreA(0, "ROOT");
    if (store == nullptr) {
        return fail(ErrorCode::IoFailed, "cannot open the Windows ROOT store");
    }
    PCCERT_CONTEXT certificate = nullptr;
    while ((certificate = CertEnumCertificatesInStore(store, certificate)) != nullptr) {
        const unsigned char* bytes = certificate->pbCertEncoded;
        X509* parsed = d2i_X509(nullptr, &bytes, static_cast<long>(certificate->cbCertEncoded));
        if (parsed != nullptr) {
            X509_STORE_add_cert(SSL_CTX_get_cert_store(context), parsed);
            X509_free(parsed);
        }
    }
    CertCloseStore(store, 0);
    return {};
}

#elif defined(__APPLE__)

Result<void> addSystemRoots(ssl_ctx_st* context) {
    if (forEachSystemRootMacos(addRootToStore, SSL_CTX_get_cert_store(context)) != 0) {
        return fail(ErrorCode::IoFailed, "cannot read the macOS root certificates");
    }
    return {};
}

#else

Result<void> addSystemRoots(ssl_ctx_st* context) {
    const char* configured = std::getenv("SSL_CERT_FILE");
    if (configured != nullptr && SSL_CTX_load_verify_locations(context, configured, nullptr) == 1) {
        return {};
    }
    for (const char* bundle : CA_BUNDLES) {
        if (SSL_CTX_load_verify_locations(context, bundle, nullptr) == 1) {
            return {};
        }
    }
    return fail(ErrorCode::IoFailed, "no CA certificates found, install the ca-certificates package");
}

#endif

} // namespace tapin::io
