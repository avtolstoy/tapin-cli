#!/bin/sh
# Copyright 2026 Andrey Tolstoy <avtolstoy@gmail.com>
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Runs a built binary the way a user does, on a machine that has no toolchain and no console. It covers what the tests
# can't: that the binary starts at all there, finds the OS trust store, resolves names and talks to Tamron's server.
# Plain sh, because Alpine has no bash.
set -u

case "$1" in
    /*) binary="$1" ;;
    *) binary="$PWD/$1" ;;
esac
work_dir="$(mktemp -d)"
trap 'rm -rf "$work_dir"' EXIT
failed=0

check() {
    description="$1"
    if [ "$2" = "$3" ]; then
        echo "ok: $description"
    else
        echo "FAILED: $description: expected $3, got $2"
        failed=1
    fi
}

# Downloads and patches one lens. A different hash only warns, Tamron may have released a new version.
check_patch() {
    model="$1"
    known_sha256="$2"
    output="$(cd "$work_dir" && "$binary" patch "$model" 2>&1)"
    status="$?"
    echo "$output"
    check "patch $model exits 0" "$status" 0
    case "$output" in
        *"$known_sha256"*) echo "ok: patch $model gives the known hash" ;;
        *) echo "warning: patch $model does not give the known hash, did Tamron release a new version?" ;;
    esac
}

"$binary" --version
check "--version exits 0" "$?" 0

"$binary" --help >/dev/null
check "--help exits 0" "$?" 0

"$binary" flash --help >/dev/null
check "a command's --help exits 0" "$?" 0

"$binary" nope >/dev/null 2>&1
check "an unknown command exits 2" "$?" 2

"$binary" flash /nonexistent.dec </dev/null >/dev/null 2>&1
check "a missing file exits 1" "$?" 1

check_patch F012 7a355e2e213ec59b9d15bc53f7913b0aca6408408478ba844461bf7865e66b7e
check_patch F013 a9ee5fe339819d7076ec2c08dafe16fa07b8d912445cb1a882a48228bcf453ba
check_patch F016 8fc699853db6e8171f813bc061036ac4415b684d96a2ed651592e393e1d7a039

# Cortex-M lenses. Nobody has flashed these, the patch still has to apply.
check_patch A022 aa8b95ebf1a92a189c1c2dddfc830bcfc47779bd25491352e35e578eb8b11582
check_patch A025 2d908360d0737c61a893d0fbd3930b7be218fdec253d183bbebd9a6f7aec0ebf
check_patch A032 72eac6da3a6279a3bac6ea4f29f78276232b644efed031085d1767c813df711c
check_patch A034 9b90986b4234f4f8eb311c8d3d4422bd134194c501812e954f6e25c92c9c2b8d
check_patch A035 264b6e97264d1fbc38cad29d7605eceb35d5679a88df7e2f812723a0ff7f2d4d
check_patch A037 8e2619b47cefe33f2e5b232e6dac9bbb4cb3cf340cff36c1da995919f98b61e7
check_patch A041 9f2b29b48010b137a5fb4c3e6036011c4315f644295fb01cae2b476d5b177dbf
check_patch B023 6170ce0087725938f7afce60d4aabb047c1b70d2e68b476990abb6ecb3b3a3aa
check_patch B028 4fd4e0cb6daab5024b1a1bb5fe3c48cd8e8eb2939a97c27232210e625e0af6f4
check_patch F045 b0208fdf0f7b4be05a468075d6b282cfcf3d655d50269e5a790352d63ca80e5c

# F017 has no such check, so there is nothing to patch.
(cd "$work_dir" && "$binary" patch F017 >/dev/null 2>&1)
check "patch F017 refuses" "$?" 1

exit "$failed"
