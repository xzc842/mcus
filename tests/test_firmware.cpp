// Copyright 2026 XueZicheng <xuezicheng842@outlook.com>
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

#include "mcus/firmware.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace mcus;

int main() {
    // 两段数据，中间空洞填 0
    {
        const std::string hex =
            ":03000000010203F7\n"
            ":02010000AABB98\n"
            ":00000001FF\n";
        FirmwareImage img = parse_intel_hex(hex, "test");
        assert(img.load_addr == 0);
        assert(img.data.size() == 0x102);
        assert(img.data[0] == 1 && img.data[1] == 2 && img.data[2] == 3);
        assert(img.data[0x100] == 0xAA && img.data[0x101] == 0xBB);
        assert(img.data[3] == 0);   // 空洞
    }

    // 扩展线性地址：0x08000000
    {
        const std::string hex =
            ":020000040800F2\n"
            ":0100000011EE\n"
            ":00000001FF\n";
        FirmwareImage img = parse_intel_hex(hex, "test");
        assert(img.load_addr == 0x08000000);
        assert(img.data.size() == 1 && img.data[0] == 0x11);
    }

    // 校验和错误
    {
        bool threw = false;
        try {
            parse_intel_hex(":03000000010203F8\n:00000001FF\n", "test");
        } catch (const std::runtime_error&) {
            threw = true;
        }
        assert(threw);
    }

    // 裸二进制
    {
        const char* path = "/tmp/mcus_firmware_test.bin";
        {
            std::ofstream f(path, std::ios::binary);
            f.put('\x01');
            f.put('\x02');
        }
        FirmwareImage img = load_firmware(path);
        assert(img.load_addr == 0);
        assert(img.data.size() == 2 && img.data[0] == 1 && img.data[1] == 2);
        std::remove(path);
    }

    std::cout << "test_firmware passed\n";
    return 0;
}
