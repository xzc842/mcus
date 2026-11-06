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

#include "mcus/model.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <stdexcept>
#include <unordered_set>

namespace mcus {

using nlohmann::json;

static uint32_t u32(const json& j, const char* k, uint32_t d = 0) {
    return j.contains(k) ? j.at(k).get<uint32_t>() : d;
}
// 兼容旧模型字段名：新名字优先，旧名字作为别名。
static uint32_t u32_alias(const json& j, const char* name, const char* alias,
                          uint32_t d = 0) {
    if (j.contains(name)) return j.at(name).get<uint32_t>();
    if (j.contains(alias)) return j.at(alias).get<uint32_t>();
    return d;
}
static std::string str(const json& j, const char* k, const std::string& d = "") {
    return j.contains(k) ? j.at(k).get<std::string>() : d;
}
static bool b(const json& j, const char* k, bool d = false) {
    return j.contains(k) ? j.at(k).get<bool>() : d;
}

static RegSemantics parse_sem(const std::string& s) {
    if (s == "write_trigger") return RegSemantics::WriteTrigger;
    if (s == "read_clear") return RegSemantics::ReadClear;
    if (s == "write_one_clear") return RegSemantics::WriteOneClear;
    if (s == "read_only") return RegSemantics::ReadOnly;
    if (s == "write_only") return RegSemantics::WriteOnly;
    if (s == "fifo") return RegSemantics::Fifo;
    return RegSemantics::Plain;
}

static std::vector<RegisterAction> parse_actions(const json& reg,
                                                 const char* key) {
    std::vector<RegisterAction> out;
    for (const auto& a : reg.value(key, json::array())) {
        RegisterAction act;
        const std::string kind = str(a, "action", "set_bit");
        if (kind == "set_bit") act.kind = RegisterAction::Kind::SetBit;
        else if (kind == "clear_bit") act.kind = RegisterAction::Kind::ClearBit;
        else if (kind == "set_value") act.kind = RegisterAction::Kind::SetValue;
        else if (kind == "clear_value") act.kind = RegisterAction::Kind::ClearValue;
        else throw std::runtime_error("unknown register action: " + kind);

        act.space = str(a, "space", "");
        act.addr = u32(a, "addr");
        act.bit = static_cast<uint8_t>(u32(a, "bit", 0));
        act.value = u32(a, "value", 0);
        out.push_back(std::move(act));
    }
    return out;
}

Model Model::load_from_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) throw std::runtime_error("cannot open model: " + path);
    json j;
    try { f >> j; }
    catch (const std::exception& e) {
        throw std::runtime_error("invalid model json: " + std::string(e.what()));
    }

    Model m;
    m.name = str(j, "name");
    m.description = str(j, "description");
    m.arch = str(j, "arch", "mcus-minimal");
    m.crystal_hz = u32(j, "crystal_hz");
    m.cycles_per_instruction = std::max<uint32_t>(1, u32(j, "cycles_per_instruction", 1));
    m.reset_vector = u32(j, "reset_vector");
    // 不在核心里给某个架构的默认空间名；模型必须显式声明。
    m.reset_space = str(j, "reset_space");
    m.initial_sp = u32(j, "initial_sp");
    m.sp_space = str(j, "sp_space");

    for (const auto& e : j.value("isa_extensions", json::array()))
        m.isa_extensions.push_back(e.get<std::string>());

    for (const auto& r : j.value("regions", json::array())) {
        MemoryRegion reg;
        reg.name = str(r, "name");
        reg.space = str(r, "space");
        reg.base = u32(r, "base");
        reg.size = u32(r, "size");
        reg.is_mmio = b(r, "is_mmio");
        reg.read_only = b(r, "read_only");
        reg.addr_mask = u32(r, "addr_mask", 0);
        m.regions.push_back(std::move(reg));
    }
    for (const auto& r : j.value("registers", json::array())) {
        RegisterDesc rd;
        rd.name = str(r, "name");
        rd.space = str(r, "space");
        rd.addr = u32(r, "addr");
        rd.width = static_cast<uint8_t>(u32(r, "width", 8));
        rd.semantics = parse_sem(str(r, "semantics", "plain"));
        rd.reset_value = u32(r, "reset_value");
        rd.read_alias = str(r, "read_alias");
        rd.write_alias = str(r, "write_alias");
        for (const auto& ba : r.value("bit_aliases", json::array())) {
            rd.bit_aliases.emplace_back(
                ba.at("addr").get<Addr>(),
                static_cast<uint8_t>(ba.at("bit").get<uint32_t>()));
        }
        rd.on_write = parse_actions(r, "on_write");
        rd.on_read = parse_actions(r, "on_read");
        // action 未写 space 时默认作用于本寄存器所在空间
        for (auto& act : rd.on_write)
            if (act.space.empty()) act.space = rd.space;
        for (auto& act : rd.on_read)
            if (act.space.empty()) act.space = rd.space;
        m.registers.push_back(std::move(rd));
    }
    for (const auto& p : j.value("pins", json::array())) {
        PinDesc pd;
        pd.name = str(p, "name");
        pd.number = u32(p, "number");
        for (const auto& fn : p.value("functions", json::array()))
            pd.functions.push_back(fn.get<std::string>());
        pd.reset_function = u32(p, "reset_function");
        pd.source_space = str(p, "source_space", "");
        pd.source_reg = u32(p, "source_reg", 0);
        pd.source_bit = static_cast<uint8_t>(u32(p, "source_bit", 0));
        if (p.contains("direction_reg")) {
            pd.has_direction = true;
            pd.direction_space = str(p, "direction_space", pd.source_space);
            pd.direction_reg = u32(p, "direction_reg", 0);
            pd.direction_bit = static_cast<uint8_t>(u32(p, "direction_bit", pd.source_bit));
        }
        m.pins.push_back(std::move(pd));
    }
    for (const auto& irq : j.value("interrupts", json::array())) {
        InterruptDesc d;
        d.name = str(irq, "name");
        d.number = u32(irq, "number");
        // 默认全部同一优先级；需要抢占时在模型里显式给高优先级中断更小的 priority。
        d.priority = u32(irq, "priority", 0);
        d.vector_addr = u32(irq, "vector_addr");
        d.vector_space = str(irq, "vector_space");
        d.enable_reg = u32(irq, "enable_reg");
        d.enable_space = str(irq, "enable_space");
        d.enable_bit = static_cast<uint8_t>(u32(irq, "enable_bit"));
        d.pending_reg = u32(irq, "pending_reg");
        d.pending_space = str(irq, "pending_space");
        d.pending_bit = static_cast<uint8_t>(u32(irq, "pending_bit"));
        d.clear_pending = b(irq, "clear_pending", true);
        m.interrupts.push_back(std::move(d));
    }
    for (const auto& t : j.value("timers", json::array())) {
        TimerDesc td;
        td.name = str(t, "name");
        const auto& c = t.at("counter");
        td.counter.bits = static_cast<uint8_t>(u32(c, "bits", 16));
        td.counter.lo_reg = u32(c, "lo_reg");
        td.counter.hi_reg = u32(c, "hi_reg", 0);
        td.counter.reg_space = str(c, "reg_space");
        td.counter.reload_lo_reg = u32(c, "reload_lo_reg", td.counter.lo_reg);
        td.counter.reload_hi_reg = u32(c, "reload_hi_reg", td.counter.hi_reg);
        td.counter.up = b(c, "up", true);
        td.enable.reg = u32(t.at("enable"), "reg");
        td.enable.bit = static_cast<uint8_t>(u32(t.at("enable"), "bit"));
        td.enable.space = str(t.at("enable"), "space", td.counter.reg_space);
        td.overflow.reg = u32(t.at("overflow"), "reg");
        td.overflow.bit = static_cast<uint8_t>(u32(t.at("overflow"), "bit"));
        td.overflow.space = str(t.at("overflow"), "space", td.counter.reg_space);
        td.irq = u32(t, "irq");
        td.overflow_action = str(t, "overflow_action", "reload");
        td.prescaler = std::max<uint32_t>(1, u32(t, "prescaler", 1));
        m.timers.push_back(std::move(td));
    }

    // 引脚名 -> 索引，供外设的通道/引脚映射使用
    std::unordered_map<std::string, uint32_t> pin_index;
    for (size_t i = 0; i < m.pins.size(); ++i)
        pin_index[m.pins[i].name] = static_cast<uint32_t>(i);

    auto resolve_pin = [&](const json& v) -> uint32_t {
        if (v.is_number()) return v.get<uint32_t>();
        const std::string name = v.get<std::string>();
        auto it = pin_index.find(name);
        if (it == pin_index.end())
            throw std::runtime_error("unknown pin in peripheral: " + name);
        return it->second;
    };

    for (const auto& u : j.value("uarts", json::array())) {
        UartDesc d;
        d.name = str(u, "name");
        d.irq = u32(u, "irq");
        d.space = str(u, "space");
        d.ctrl = u32_alias(u, "ctrl", "scon");
        d.data = u32_alias(u, "data", "sbuf");
        d.tx_flag_bit = static_cast<uint8_t>(
            u32_alias(u, "tx_flag_bit", "ti_bit", 1));
        d.rx_flag_bit = static_cast<uint8_t>(
            u32_alias(u, "rx_flag_bit", "ri_bit", 0));
        d.tx_cycles = u32(u, "tx_cycles", 0);
        d.rx_fifo = std::max<uint32_t>(1, u32(u, "rx_fifo", 8));
        m.uarts.push_back(std::move(d));
    }

    for (const auto& s : j.value("spis", json::array())) {
        SpiDesc d;
        d.name = str(s, "name");
        d.irq = u32(s, "irq");
        d.space = str(s, "space");
        d.data = u32(s, "data");
        d.stat = u32(s, "stat");
        d.ctrl = u32(s, "ctrl");
        d.done_bit = static_cast<uint8_t>(
            u32_alias(s, "done_bit", "spif_bit", 4));
        d.busy_bit = static_cast<uint8_t>(u32(s, "busy_bit", 7));
        d.xfer_cycles = u32(s, "xfer_cycles", 8);
        m.spis.push_back(std::move(d));
    }

    for (const auto& p : j.value("pwms", json::array())) {
        PwmDesc d;
        d.name = str(p, "name");
        d.irq = u32(p, "irq");
        d.space = str(p, "space");
        d.ctrl = u32(p, "ctrl");
        d.data1 = u32(p, "data1");
        d.data2 = u32(p, "data2");
        d.ck_se = u32(p, "ck_se");
        d.if_bit = static_cast<uint8_t>(u32(p, "if_bit", 4));
        d.enable_bit1 = static_cast<uint8_t>(u32(p, "enable_bit1", 0));
        d.enable_bit2 = static_cast<uint8_t>(u32(p, "enable_bit2", 1));
        d.period = std::max<uint32_t>(1, u32(p, "period", 256));
        d.prescaler = std::max<uint32_t>(1, u32(p, "prescaler", 1));
        for (const auto& pp : p.value("pins", json::array())) {
            PwmPinDesc pd;
            pd.channel = static_cast<uint8_t>(u32(pp, "channel", 1));
            pd.pin = resolve_pin(pp.at("pin"));
            pd.function = str(pp, "function");
            d.pins.push_back(pd);
        }
        m.pwms.push_back(std::move(d));
    }

    for (const auto& a : j.value("adcs", json::array())) {
        AdcDesc d;
        d.name = str(a, "name");
        d.irq = u32(a, "irq");
        d.space = str(a, "space");
        d.ctrl = u32(a, "ctrl");
        d.cfg = u32(a, "cfg");
        d.data = u32(a, "data");
        d.start_bit = static_cast<uint8_t>(u32(a, "start_bit", 0));
        d.if_bit = static_cast<uint8_t>(u32(a, "if_bit", 5));
        d.busy_bit = static_cast<uint8_t>(u32(a, "busy_bit", 4));
        d.conversion_cycles = u32(a, "conversion_cycles", 96);
        d.channel_mask = u32(a, "channel_mask", 0xFFFFFFFFu);
        d.channel_shift = static_cast<uint8_t>(u32(a, "channel_shift", 0));
        for (const auto& c : a.value("channels", json::array())) {
            const uint32_t ch = u32(c, "channel");
            const uint32_t pin = resolve_pin(c.at("pin"));
            if (d.channel_pins.size() <= ch) d.channel_pins.resize(ch + 1, UINT32_MAX);
            d.channel_pins[ch] = pin;
        }
        m.adcs.push_back(std::move(d));
    }

    for (const auto& t : j.value("tkeys", json::array())) {
        TkeyDesc d;
        d.name = str(t, "name");
        d.irq = u32(t, "irq");
        d.space = str(t, "space");
        d.ctrl = u32(t, "ctrl");
        d.datl = u32(t, "datl");
        d.dath = u32(t, "dath");
        d.start_bit = static_cast<uint8_t>(u32(t, "start_bit", 0));
        d.if_bit = static_cast<uint8_t>(u32(t, "if_bit", 7));
        d.scan_cycles = u32(t, "scan_cycles", 256);
        d.data_bits = static_cast<uint8_t>(
            std::max<uint32_t>(1, std::min<uint32_t>(16, u32(t, "data_bits", 16))));
        for (const auto& c : t.value("channels", json::array()))
            d.channel_pins.push_back(resolve_pin(c));
        m.tkeys.push_back(std::move(d));
    }

    if (j.contains("gpio_irq")) {
        const auto& g = j.at("gpio_irq");
        m.gpio_irq.present = true;
        m.gpio_irq.irq = u32(g, "irq");
        m.gpio_irq.enable_space = str(g, "enable_space");
        m.gpio_irq.enable_reg = u32(g, "enable_reg");
        m.gpio_irq.enable_bit_base =
            static_cast<uint8_t>(u32(g, "enable_bit_base", 0));
        m.gpio_irq.enable_bits =
            static_cast<uint8_t>(u32(g, "enable_bits", 8));
    }

    if (j.contains("usb")) {
        const auto& u = j.at("usb");
        m.usb.present = true;
        m.usb.irq = u32(u, "irq");
        m.usb.space = str(u, "space");
        m.usb.int_fg = u32(u, "int_fg");
        m.usb.int_st = u32(u, "int_st");
        m.usb.mis_st = u32(u, "mis_st");
        m.usb.rx_len = u32(u, "rx_len");
        m.usb.int_en = u32(u, "int_en");
        m.usb.ctrl = u32(u, "ctrl");
        m.usb.dev_ad = u32(u, "dev_ad");
        m.usb.int_fg_bit = static_cast<uint8_t>(u32(u, "int_fg_bit", 1));
        m.usb.tog_ok_bit = static_cast<uint8_t>(u32(u, "tog_ok_bit", 6));
        m.usb.bus_reset_bit = static_cast<uint8_t>(u32(u, "bus_reset_bit", 0));
        m.usb.buffer_space = str(u, "buffer_space");
        m.usb.buffer_base = u32(u, "buffer_base", 0);
        m.usb.buffer_stride = std::max<uint32_t>(1, u32(u, "buffer_stride", 64));
        for (const auto& e : u.value("endpoints", json::array())) {
            UsbEndpointDesc ed;
            ed.index = static_cast<uint8_t>(u32(e, "index"));
            ed.ctrl_space = str(e, "ctrl_space", m.usb.space);
            ed.ctrl = u32(e, "ctrl");
            ed.max_packet = static_cast<uint16_t>(u32(e, "max_packet", 64));
            ed.len_reg = u32(e, "len_reg", 0);
            ed.len_space = str(e, "len_space", ed.ctrl_space);
            ed.buffer_offset = u32(e, "buffer_offset",
                                   static_cast<uint32_t>(ed.index) *
                                       m.usb.buffer_stride);
            ed.tx_buffer_offset = u32(e, "tx_buffer_offset", 0);
            m.usb.endpoints.push_back(std::move(ed));
        }
    }

    // arch_desc
    if (j.contains("arch_desc")) {
        const auto& ad = j.at("arch_desc");
        if (ad.contains("global_irq_enable")) {
            const auto& ge = ad.at("global_irq_enable");
            m.arch_desc.global_irq_enable.present = true;
            m.arch_desc.global_irq_enable.space = str(ge, "space", "");
            m.arch_desc.global_irq_enable.reg = u32(ge, "reg", 0);
            m.arch_desc.global_irq_enable.bit = static_cast<uint8_t>(u32(ge, "bit", 0));
        }
        m.arch_desc.irq_push_bytes = static_cast<uint8_t>(u32(ad, "irq_push_bytes", 0));
        m.arch_desc.stack_grows_down = b(ad, "stack_grows_down", false);
        if (ad.contains("fast_spaces")) {
            for (const auto& v : ad.at("fast_spaces"))
                m.arch_desc.fast_spaces.push_back(v.get<std::string>());
        }
        m.arch_desc.reset_space = str(ad, "reset_space", m.reset_space);
        m.arch_desc.reset_vector = u32(ad, "reset_vector", m.reset_vector);
        m.arch_desc.sp_space = str(ad, "sp_space", m.sp_space);
        m.arch_desc.initial_sp = u32(ad, "initial_sp", m.initial_sp);
        m.arch_desc.sp_reg = u32(ad, "sp_reg", 0);
        m.arch_desc.sp_reg_space = str(ad, "sp_reg_space", m.sp_space);
        m.arch_desc.has_pc_reg = b(ad, "has_pc_reg", false);
        m.arch_desc.pc_reg_space = str(ad, "pc_reg_space", "");
        m.arch_desc.pc_reg_lo = u32(ad, "pc_reg_lo", 0);
        m.arch_desc.pc_reg_hi = u32(ad, "pc_reg_hi", 0);
    } else {
        // 没有 arch_desc 时只使用模型顶层字段，不注入任何具体架构的默认。
        m.arch_desc.reset_space = m.reset_space;
        m.arch_desc.reset_vector = m.reset_vector;
        m.arch_desc.sp_space = m.sp_space;
        m.arch_desc.initial_sp = m.initial_sp;
        m.arch_desc.fast_spaces.clear();
        m.arch_desc.irq_push_bytes = 0;
        m.arch_desc.stack_grows_down = false;
        m.arch_desc.sp_reg = 0;
        m.arch_desc.sp_reg_space = m.sp_space;
        m.arch_desc.global_irq_enable.present = false;
    }

    // 顶层字段是旧写法；新模型可以只写 arch_desc，这里回填以保持兼容。
    if (m.reset_space.empty()) m.reset_space = m.arch_desc.reset_space;
    if (m.sp_space.empty()) m.sp_space = m.arch_desc.sp_space;

    for (size_t i = 0; i < m.registers.size(); ++i)
        m.reg_index[reg_key(m.registers[i].space, m.registers[i].addr)] = i;

    for (const auto& r : m.regions) {
        if (std::find(m.space_order.begin(), m.space_order.end(), r.space) ==
            m.space_order.end()) {
            m.space_order.push_back(r.space);
        }
    }

    m.validate();
    return m;
}

void Model::validate() const {
    if (reset_space.empty() || sp_space.empty())
        throw std::runtime_error("reset_space/sp_space must be set");
    if (arch_desc.reset_space.empty() || arch_desc.sp_space.empty())
        throw std::runtime_error("arch_desc.reset_space/sp_space must be set");
    if (arch_desc.global_irq_enable.present &&
        arch_desc.global_irq_enable.space.empty())
        throw std::runtime_error("global_irq_enable.space must be set");
    if (arch_desc.sp_reg != 0 && arch_desc.sp_reg_space.empty())
        throw std::runtime_error("sp_reg_space must be set when sp_reg is set");
    if (arch_desc.has_pc_reg && arch_desc.pc_reg_space.empty())
        throw std::runtime_error("pc_reg_space must be set when has_pc_reg is set");

    for (size_t i = 0; i < regions.size(); ++i) {
        if (regions[i].size == 0)
            throw std::runtime_error("region size zero: " + regions[i].name);
        for (size_t j = i + 1; j < regions.size(); ++j) {
            if (regions[i].space != regions[j].space) continue;
            Addr a0 = regions[i].base, a1 = a0 + regions[i].size;
            Addr b0 = regions[j].base, b1 = b0 + regions[j].size;
            if (a0 < b1 && b0 < a1)
                throw std::runtime_error("regions overlap: " + regions[i].name +
                                         " / " + regions[j].name);
        }
    }
    auto in = [&](const SpaceId& sp, Addr a) {
        return find_region(sp, a) != nullptr;
    };
    if (!in(reset_space, reset_vector))
        throw std::runtime_error("reset_vector not in reset_space");
    if (!in(sp_space, initial_sp))
        throw std::runtime_error("initial_sp not in sp_space");

    std::unordered_map<std::string, std::vector<const RegisterDesc*>> by_addr;
    for (const auto& rd : registers) {
        if (rd.width != 8 && rd.width != 16 && rd.width != 32)
            throw std::runtime_error("invalid register width: " + rd.name);
        if (rd.space.empty())
            throw std::runtime_error("register space empty: " + rd.name);
        if (!in(rd.space, rd.addr))
            throw std::runtime_error("register not in space: " + rd.name);
        by_addr[reg_key(rd.space, rd.addr)].push_back(&rd);
    }
    for (const auto& [key, list] : by_addr) {
        if (list.size() == 1) continue;
        for (const auto* rd : list) {
            bool linked = false;
            for (const auto* other : list) {
                if (other == rd) continue;
                if (rd->read_alias == other->name ||
                    rd->write_alias == other->name ||
                    other->read_alias == rd->name ||
                    other->write_alias == rd->name) {
                    linked = true;
                    break;
                }
            }
            if (!linked)
                throw std::runtime_error("duplicate register without alias: " +
                                         rd->name);
        }
    }

    std::unordered_set<uint32_t> pins_seen;
    for (const auto& p : pins) {
        if (!pins_seen.insert(p.number).second)
            throw std::runtime_error("duplicate pin: " + p.name);
        if (p.functions.empty() || p.reset_function >= p.functions.size())
            throw std::runtime_error("invalid pin functions: " + p.name);
    }

    std::unordered_set<uint32_t> irq_seen;
    for (const auto& irq : interrupts) {
        if (!irq_seen.insert(irq.number).second)
            throw std::runtime_error("duplicate irq: " + irq.name);
        if (irq.vector_space.empty() || irq.enable_space.empty() ||
            irq.pending_space.empty())
            throw std::runtime_error("irq space empty: " + irq.name);
        if (!in(irq.vector_space, irq.vector_addr))
            throw std::runtime_error("irq vector not in space: " + irq.name);
        if (!reg_index.count(reg_key(irq.enable_space, irq.enable_reg)))
            throw std::runtime_error("irq enable_reg missing: " + irq.name);
        if (!reg_index.count(reg_key(irq.pending_space, irq.pending_reg)))
            throw std::runtime_error("irq pending_reg missing: " + irq.name);
    }

    for (const auto& t : timers) {
        if (t.counter.reg_space.empty())
            throw std::runtime_error("timer reg_space empty: " + t.name);
        if (!reg_index.count(reg_key(t.counter.reg_space, t.counter.lo_reg)))
            throw std::runtime_error("timer lo_reg missing: " + t.name);
        if (t.counter.bits > 8 &&
            !reg_index.count(reg_key(t.counter.reg_space, t.counter.hi_reg)))
            throw std::runtime_error("timer hi_reg missing: " + t.name);
        if (!reg_index.count(reg_key(t.counter.reg_space, t.counter.reload_lo_reg)))
            throw std::runtime_error("timer reload_lo missing: " + t.name);
        if (t.irq != 0 && !irq_seen.count(t.irq))
            throw std::runtime_error("timer irq missing: " + t.name);
    }

    auto has_reg = [&](const SpaceId& sp, Addr a) {
        return reg_index.count(reg_key(sp, a)) != 0;
    };
    auto check_reg = [&](const SpaceId& sp, Addr a, const std::string& what) {
        if (!has_reg(sp, a))
            throw std::runtime_error("peripheral register missing: " + what);
    };
    auto check_irq = [&](uint32_t n, const std::string& what) {
        if (n != 0 && !irq_seen.count(n))
            throw std::runtime_error("peripheral irq missing: " + what);
    };

    for (const auto& p : pins) {
        if (p.has_direction &&
            !has_reg(p.direction_space, p.direction_reg))
            throw std::runtime_error("pin direction_reg missing: " + p.name);
        if (!p.source_space.empty() &&
            !has_reg(p.source_space, p.source_reg))
            throw std::runtime_error("pin source_reg missing: " + p.name);
    }

    for (const auto& sp : arch_desc.fast_spaces) {
        bool found = false;
        for (const auto& r : regions)
            if (r.space == sp) { found = true; break; }
        if (!found)
            throw std::runtime_error("fast_space not in regions: " + sp);
    }
    if (arch_desc.global_irq_enable.present &&
        !has_reg(arch_desc.global_irq_enable.space,
                 arch_desc.global_irq_enable.reg))
        throw std::runtime_error("global_irq_enable register missing");
    if (arch_desc.sp_reg != 0 &&
        !find_region(arch_desc.sp_reg_space, arch_desc.sp_reg))
        throw std::runtime_error("sp_reg not in space");
    if (arch_desc.has_pc_reg &&
        (!find_region(arch_desc.pc_reg_space, arch_desc.pc_reg_lo) ||
         !find_region(arch_desc.pc_reg_space, arch_desc.pc_reg_hi)))
        throw std::runtime_error("pc_reg not in space");

    for (const auto& u : uarts) {
        check_reg(u.space, u.ctrl, u.name + ".ctrl");
        check_reg(u.space, u.data, u.name + ".data");
        check_irq(u.irq, u.name);
    }
    for (const auto& s : spis) {
        check_reg(s.space, s.data, s.name + ".data");
        check_reg(s.space, s.stat, s.name + ".stat");
        check_reg(s.space, s.ctrl, s.name + ".ctrl");
        check_irq(s.irq, s.name);
    }
    for (const auto& p : pwms) {
        check_reg(p.space, p.ctrl, p.name + ".ctrl");
        check_reg(p.space, p.data1, p.name + ".data1");
        check_reg(p.space, p.data2, p.name + ".data2");
        check_reg(p.space, p.ck_se, p.name + ".ck_se");
        check_irq(p.irq, p.name);
        for (const auto& pp : p.pins)
            if (pp.pin >= pins.size())
                throw std::runtime_error("pwm pin index invalid: " + p.name);
    }
    for (const auto& a : adcs) {
        check_reg(a.space, a.ctrl, a.name + ".ctrl");
        check_reg(a.space, a.cfg, a.name + ".cfg");
        check_reg(a.space, a.data, a.name + ".data");
        check_irq(a.irq, a.name);
        for (uint32_t pin : a.channel_pins)
            if (pin != UINT32_MAX && pin >= pins.size())
                throw std::runtime_error("adc pin index invalid: " + a.name);
    }
    for (const auto& t : tkeys) {
        check_reg(t.space, t.ctrl, t.name + ".ctrl");
        check_reg(t.space, t.datl, t.name + ".datl");
        check_reg(t.space, t.dath, t.name + ".dath");
        check_irq(t.irq, t.name);
        for (uint32_t pin : t.channel_pins)
            if (pin >= pins.size())
                throw std::runtime_error("tkey pin index invalid: " + t.name);
    }
    if (gpio_irq.present) {
        check_reg(gpio_irq.enable_space, gpio_irq.enable_reg, "gpio_irq.enable");
        check_irq(gpio_irq.irq, "gpio_irq");
    }
    if (usb.present) {
        check_reg(usb.space, usb.int_fg, "usb.int_fg");
        check_reg(usb.space, usb.int_st, "usb.int_st");
        check_reg(usb.space, usb.mis_st, "usb.mis_st");
        check_reg(usb.space, usb.rx_len, "usb.rx_len");
        check_reg(usb.space, usb.int_en, "usb.int_en");
        check_reg(usb.space, usb.ctrl, "usb.ctrl");
        check_reg(usb.space, usb.dev_ad, "usb.dev_ad");
        check_irq(usb.irq, "usb");
        uint64_t buf_end = usb.buffer_base;
        for (const auto& e : usb.endpoints) {
            check_reg(e.ctrl_space, e.ctrl, "usb.ep.ctrl");
            if (e.len_reg != 0)
                check_reg(e.len_space, e.len_reg, "usb.ep.len");
            const uint64_t end = static_cast<uint64_t>(usb.buffer_base) +
                                 e.buffer_offset + e.max_packet;
            if (end > buf_end) buf_end = end;
        }
        if (buf_end > usb.buffer_base) {
            const MemoryRegion* br =
                find_region(usb.buffer_space, usb.buffer_base);
            if (!br || !br->contains(static_cast<Addr>(buf_end - 1)))
                throw std::runtime_error("usb endpoint buffers out of region");
        }
    }
}

const MemoryRegion* Model::find_region(const SpaceId& space, Addr addr) const {
    for (const auto& r : regions)
        if (r.space == space && r.contains(addr)) return &r;
    return nullptr;
}

const RegisterDesc* Model::find_register(const SpaceId& space, Addr addr) const {
    auto it = reg_index.find(reg_key(space, addr));
    if (it == reg_index.end()) return nullptr;
    return &registers[it->second];
}

const RegisterDesc* Model::find_register_by_name(const std::string& name) const {
    for (const auto& rd : registers)
        if (rd.name == name) return &rd;
    return nullptr;
}

const InterruptDesc* Model::find_interrupt(uint32_t number) const {
    for (const auto& irq : interrupts) if (irq.number == number) return &irq;
    return nullptr;
}

size_t Model::mmio_area_size() const {
    size_t total = 0;
    for (const auto& rd : registers) {
        size_t bytes = (rd.width + 7) / 8;
        total += (bytes + 3) & ~size_t(3);
    }
    return total;
}

size_t Model::pin_area_size() const { return pins.size() * 12; }

size_t Model::irq_bitmap_size() const {
    if (interrupts.empty()) return 4;
    return ((interrupts.size() + 31) / 32) * 4;
}

} // namespace mcus