/*
 * Copyright 2026 XueZicheng <xuezicheng842@outlook.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// include/mcus/firmware.h
#pragma once

#include "types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mcus {

enum class FirmwareFormat {
    Raw,       // 裸二进制，按文件原样加载
    IntelHex,  // Intel HEX（.hex / .ihx）
};

// 连续固件镜像：load_addr 是镜像第一个字节在地址空间中的地址。
// Raw 固件的 load_addr 为 0，表示相对复位区域基址加载。
struct FirmwareImage {
    Addr load_addr = 0;
    std::vector<uint8_t> data;
};

// 按扩展名判断固件格式；未知扩展名时检测首个非空白字符是否为 ':'
FirmwareFormat detect_firmware_format(const std::string& path);

// 加载固件为连续镜像：
// - Raw：原样返回，load_addr = 0
// - IntelHex：返回最小数据地址和对应的连续镜像
FirmwareImage load_firmware(const std::string& path);

// 解析 Intel HEX 文本（供测试和外部复用）
FirmwareImage parse_intel_hex(const std::string& text,
                              const std::string& origin);

} // namespace mcus
