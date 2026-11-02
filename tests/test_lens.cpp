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

#include "console/lens.h"
#include "testing.h"

using namespace tapin;
using namespace tapin::console;

TEST_CASE("lens info gives the model and the firmware") {
    EXPECT_VALUE(lens, LensInfo::parse(test::makeLensInfoBytes("F013P1", 4)));
    EXPECT(lens.model == "F013P1");
    EXPECT(lens.baseModel() == "F013");
    EXPECT(lens.serialNumber == "TESTSERIAL");
    EXPECT(lens.firmwareText() == "0400");
}

TEST_CASE("lens info of the wrong size is an error") {
    EXPECT_FALSE(LensInfo::parse(firmware::Bytes(10)).has_value());
}

TEST_CASE("lens info names what can be adjusted") {
    EXPECT_VALUE(lens, LensInfo::parse(test::makeLensInfoBytes("F013", 4)));
    EXPECT(lens.describe().contains("adjust:  focus, ftm, vc"));
}

TEST_CASE("default adjustment data takes the focus limits from the lens") {
    firmware::Bytes info = test::makeLensInfoBytes("F013", 4);
    info[basic_info::FOCUS_LIMIT_INIT_OFFSET] = 0x11;
    info[basic_info::FOCUS_LIMIT_INIT_OFFSET + 3] = 0x44;
    info[basic_info::FOCUS_LIMIT_MODES_OFFSET] = 0x10;
    EXPECT_VALUE(lens, LensInfo::parse(info));
    const firmware::Bytes data = lens.defaultAdjustData();
    EXPECT(data.size() == ADJUST_DATA_SIZE);
    EXPECT(data[3] == 0x11);
    EXPECT(data[8] == 0x44);
    EXPECT(data[5] == 2);
    EXPECT(data[9] == 1);
}

TEST_CASE("adapter info gives the mount") {
    EXPECT_VALUE(canon, AdapterInfo::parse(test::makeAdapterInfoBytes(1)));
    EXPECT(canon.mount == firmware::Mount::Canon);
    EXPECT(canon.firmware == "0300");
    EXPECT_VALUE(nikon, AdapterInfo::parse(test::makeAdapterInfoBytes(2)));
    EXPECT(nikon.mount == firmware::Mount::Nikon);
}

TEST_CASE("an adapter with a mount nobody knows has none") {
    EXPECT_VALUE(adapter, AdapterInfo::parse(test::makeAdapterInfoBytes(9)));
    EXPECT_FALSE(adapter.mount.has_value());
    EXPECT(adapter.describe().contains("(none)"));
}
