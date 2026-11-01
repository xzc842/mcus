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
    m.reset_space = str(j, "reset_space", "code");
    m.initial_sp = u32(j, "initial_sp");
    m.sp_space = str(j, "sp_space", "iram");

    for (const auto& e : j.value("isa_extensions", json::array()))
        m.isa_extensions.push_back(e.get<std::string>());

    for (const auto& r : j.value("regions", json::array())) {
        MemoryRegion reg;
        reg.name = str(r, "name");
        reg.space = str(r, "space", "code");
        reg.base = u32(r, "base");
        reg.size = u32(r, "size");
        reg.is_mmio = b(r, "is_mmio");
        reg.addr_mask = u32(r, "addr_mask", 0);
        m.regions.push_back(std::move(reg));
    }
    for (const auto& r : j.value("registers", json::array())) {
        RegisterDesc rd;
        rd.name = str(r, "name");
        rd.space = str(r, "space", "sfr");
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
        m.pins.push_back(std::move(pd));
    }
    for (const auto& irq : j.value("interrupts", json::array())) {
        InterruptDesc d;
        d.name = str(irq, "name");
        d.number = u32(irq, "number");
        d.vector_addr = u32(irq, "vector_addr");
        d.vector_space = str(irq, "vector_space", "code");
        d.enable_reg = u32(irq, "enable_reg");
        d.enable_space = str(irq, "enable_space", "sfr");
        d.enable_bit = static_cast<uint8_t>(u32(irq, "enable_bit"));
        d.pending_reg = u32(irq, "pending_reg");
        d.pending_space = str(irq, "pending_space", "sfr");
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
        td.counter.reg_space = str(c, "reg_space", "sfr");
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
        m.timers.push_back(std::move(td));
    }

    // arch_desc
    if (j.contains("arch_desc")) {
        const auto& ad = j.at("arch_desc");
        if (ad.contains("global_irq_enable")) {
            const auto& ge = ad.at("global_irq_enable");
            m.arch_desc.global_irq_enable.present = true;
            m.arch_desc.global_irq_enable.space = str(ge, "space", "sfr");
            m.arch_desc.global_irq_enable.reg = u32(ge, "reg", 0);
            m.arch_desc.global_irq_enable.bit = static_cast<uint8_t>(u32(ge, "bit", 0));
        }
        m.arch_desc.irq_push_bytes = static_cast<uint8_t>(u32(ad, "irq_push_bytes", 2));
        if (ad.contains("fast_spaces")) {
            for (const auto& v : ad.at("fast_spaces"))
                m.arch_desc.fast_spaces.push_back(v.get<std::string>());
        }
        m.arch_desc.reset_space = str(ad, "reset_space", "code");
        m.arch_desc.reset_vector = u32(ad, "reset_vector", 0);
        m.arch_desc.sp_space = str(ad, "sp_space", "iram");
        m.arch_desc.initial_sp = u32(ad, "initial_sp", 7);
        m.arch_desc.sp_reg = u32(ad, "sp_reg", 0x81);
        m.arch_desc.sp_reg_space = str(ad, "sp_reg_space", "sfr");
        m.arch_desc.has_pc_reg = b(ad, "has_pc_reg", false);
        m.arch_desc.pc_reg_space = str(ad, "pc_reg_space", "sfr");
        m.arch_desc.pc_reg_lo = u32(ad, "pc_reg_lo", 0);
        m.arch_desc.pc_reg_hi = u32(ad, "pc_reg_hi", 0);
    } else {
        m.arch_desc.reset_space = m.reset_space;
        m.arch_desc.reset_vector = m.reset_vector;
        m.arch_desc.sp_space = m.sp_space;
        m.arch_desc.initial_sp = m.initial_sp;
        m.arch_desc.fast_spaces = {"code", "iram", "xram"};
        m.arch_desc.irq_push_bytes = 2;
        m.arch_desc.sp_reg = 0x81;
        m.arch_desc.sp_reg_space = "sfr";
        m.arch_desc.global_irq_enable.present = true;
        m.arch_desc.global_irq_enable.space = "sfr";
        m.arch_desc.global_irq_enable.reg = 0xA8;
        m.arch_desc.global_irq_enable.bit = 7;
    }

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
        if (!in(irq.vector_space, irq.vector_addr))
            throw std::runtime_error("irq vector not in space: " + irq.name);
        if (!reg_index.count(reg_key(irq.enable_space, irq.enable_reg)))
            throw std::runtime_error("irq enable_reg missing: " + irq.name);
        if (!reg_index.count(reg_key(irq.pending_space, irq.pending_reg)))
            throw std::runtime_error("irq pending_reg missing: " + irq.name);
    }

    for (const auto& t : timers) {
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

bool Model::resolve_bit_addr(Addr bit_addr, SpaceId& space,
                             Addr& byte_addr, uint8_t& bit) const {
    if (bit_addr < 0x80) {
        space = "iram";
        byte_addr = 0x20 + (bit_addr / 8);
        bit = static_cast<uint8_t>(bit_addr % 8);
        return find_region(space, byte_addr) != nullptr;
    }
    space = "sfr";
    byte_addr = bit_addr & 0xF8;
    bit = static_cast<uint8_t>(bit_addr & 0x07);
    return find_register(space, byte_addr) != nullptr;
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