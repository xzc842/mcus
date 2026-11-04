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

// src/firmware.cpp
#include "mcus/firmware.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace mcus {

namespace {

constexpr uint32_t kMaxImageSize = 16u * 1024u * 1024u;

struct Segment {
    Addr addr = 0;
    std::vector<uint8_t> bytes;
};

std::string lower_extension(const std::string& path) {
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool is_space(char c) {
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

std::string trim(const std::string& s) {
    size_t begin = 0;
    while (begin < s.size() && is_space(s[begin])) ++begin;
    size_t end = s.size();
    while (end > begin && is_space(s[end - 1])) --end;
    return s.substr(begin, end - begin);
}

[[noreturn]] void fail(const std::string& origin, size_t line,
                       const std::string& message) {
    throw std::runtime_error(origin + ": Intel HEX 第 " +
                             std::to_string(line) + " 行: " + message);
}

} // namespace

FirmwareFormat detect_firmware_format(const std::string& path) {
    const std::string ext = lower_extension(path);
    if (ext == ".hex" || ext == ".ihx") return FirmwareFormat::IntelHex;
    if (ext == ".bin") return FirmwareFormat::Raw;

    // 未知扩展名：按内容嗅探，首个非空白字符是 ':' 就当 Intel HEX
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        throw std::runtime_error("cannot open firmware: " + path);
    }
    char c = 0;
    while (f.get(c)) {
        if (!is_space(c)) {
            return c == ':' ? FirmwareFormat::IntelHex : FirmwareFormat::Raw;
        }
    }
    return FirmwareFormat::Raw;
}

FirmwareImage parse_intel_hex(const std::string& text,
                              const std::string& origin) {
    std::vector<Segment> segments;
    uint32_t base = 0;
    bool saw_data = false;
    bool saw_eof = false;
    size_t line_no = 0;

    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        std::string line = text.substr(
            pos, end == std::string::npos ? std::string::npos : end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        ++line_no;

        line = trim(line);
        if (!line.empty()) {
            if (line[0] != ':') fail(origin, line_no, "记录必须以 ':' 开头");
            if (line.size() < 11 || (line.size() - 1) % 2 != 0) {
                fail(origin, line_no, "记录长度不合法");
            }

            std::vector<uint8_t> bytes;
            bytes.reserve((line.size() - 1) / 2);
            for (size_t i = 1; i < line.size(); i += 2) {
                int hi = hex_digit(line[i]);
                int lo = hex_digit(line[i + 1]);
                if (hi < 0 || lo < 0) {
                    fail(origin, line_no, "包含非十六进制字符");
                }
                bytes.push_back(static_cast<uint8_t>((hi << 4) | lo));
            }

            const uint8_t count = bytes[0];
            if (bytes.size() != static_cast<size_t>(count) + 5) {
                fail(origin, line_no, "字节数与 count 字段不一致");
            }

            uint8_t sum = 0;
            for (uint8_t b : bytes) sum = static_cast<uint8_t>(sum + b);
            if (sum != 0) fail(origin, line_no, "校验和错误");

            const uint32_t addr =
                (static_cast<uint32_t>(bytes[1]) << 8) | bytes[2];
            const uint8_t type = bytes[3];
            const uint8_t* data = bytes.data() + 4;

            switch (type) {
                case 0x00: {
                    Segment seg;
                    seg.addr = base + addr;
                    seg.bytes.assign(data, data + count);
                    segments.push_back(std::move(seg));
                    saw_data = true;
                    break;
                }
                case 0x01:
                    saw_eof = true;
                    break;
                case 0x02:
                    if (count != 2) {
                        fail(origin, line_no, "扩展段地址记录长度必须为 2");
                    }
                    base = ((static_cast<uint32_t>(data[0]) << 8) | data[1]) << 4;
                    break;
                case 0x04:
                    if (count != 2) {
                        fail(origin, line_no, "扩展线性地址记录长度必须为 2");
                    }
                    base = ((static_cast<uint32_t>(data[0]) << 8) | data[1]) << 16;
                    break;
                case 0x03:
                case 0x05:
                    break;
                default:
                    fail(origin, line_no,
                         "不支持的记录类型 " + std::to_string(type));
            }
        }

        if (saw_eof) break;
        if (end == std::string::npos) break;
        pos = end + 1;
    }

    if (!saw_data || segments.empty()) {
        throw std::runtime_error(origin + ": Intel HEX 中没有数据记录");
    }

    Addr min_addr = segments.front().addr;
    Addr max_addr = segments.front().addr;
    for (const auto& seg : segments) {
        min_addr = std::min(min_addr, seg.addr);
        max_addr = std::max(max_addr, seg.addr +
                                          static_cast<Addr>(seg.bytes.size()));
    }
    if (max_addr <= min_addr || (max_addr - min_addr) > kMaxImageSize) {
        throw std::runtime_error(origin + ": 固件地址范围不合法");
    }

    FirmwareImage image;
    image.load_addr = min_addr;
    image.data.assign(static_cast<size_t>(max_addr - min_addr), 0);
    for (const auto& seg : segments) {
        const size_t off = static_cast<size_t>(seg.addr - min_addr);
        if (off + seg.bytes.size() > image.data.size()) {
            throw std::runtime_error(origin + ": 固件记录越界");
        }
        std::copy(seg.bytes.begin(), seg.bytes.end(),
                  image.data.begin() + static_cast<std::ptrdiff_t>(off));
    }
    return image;
}

FirmwareImage load_firmware(const std::string& path) {
    const FirmwareFormat format = detect_firmware_format(path);

    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        throw std::runtime_error("cannot open firmware: " + path);
    }

    if (format == FirmwareFormat::Raw) {
        FirmwareImage image;
        image.load_addr = 0;
        image.data.assign(std::istreambuf_iterator<char>(f),
                          std::istreambuf_iterator<char>());
        return image;
    }

    std::ostringstream ss;
    ss << f.rdbuf();
    return parse_intel_hex(ss.str(), path);
}

} // namespace mcus
