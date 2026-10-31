#pragma once
#include "types.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>

namespace mcus {

struct MemoryRegion {
    std::string name;
    SpaceId space;
    Addr base = 0;
    uint32_t size = 0;
    bool is_mmio = false;
    Addr addr_mask = 0;
    bool contains(Addr addr) const {
        return addr >= base && (addr - base) < size;
    }
};

struct RegisterDesc {
    std::string name;
    SpaceId space;
    Addr addr = 0;
    uint8_t width = 32;
    RegSemantics semantics = RegSemantics::Plain;
    uint32_t reset_value = 0;
    std::string read_alias;
    std::string write_alias;
    std::vector<std::pair<Addr, uint8_t>> bit_aliases;
};

struct PinDesc {
    std::string name;
    uint32_t number = 0;
    std::vector<std::string> functions;
    uint32_t reset_function = 0;
    SpaceId source_space;
    Addr source_reg = 0;
    uint8_t source_bit = 0;
};

struct InterruptDesc {
    std::string name;
    uint32_t number = 0;
    Addr vector_addr = 0;
    SpaceId vector_space = "code";
    Addr enable_reg = 0;
    SpaceId enable_space = "sfr";
    uint8_t enable_bit = 0;
    Addr pending_reg = 0;
    SpaceId pending_space = "sfr";
    uint8_t pending_bit = 0;
};

struct CounterDesc {
    uint8_t bits = 16;
    Addr lo_reg = 0;
    Addr hi_reg = 0;
    SpaceId reg_space = "sfr";
    Addr reload_lo_reg = 0;
    Addr reload_hi_reg = 0;
    bool up = true;
};

struct TimerDesc {
    std::string name;
    CounterDesc counter;
    struct CtrlBit { Addr reg; uint8_t bit; } enable;
    struct FlagBit { Addr reg; uint8_t bit; } overflow;
    uint32_t irq = 0;
    std::string overflow_action = "reload";
};

struct Model {
    std::string name;
    std::string description;
    std::string arch = "mcus-minimal";
    uint32_t crystal_hz = 0;
    uint32_t cycles_per_instruction = 1;
    Addr reset_vector = 0;
    SpaceId reset_space = "code";
    Addr initial_sp = 0;
    SpaceId sp_space = "iram";

    std::vector<MemoryRegion> regions;
    std::vector<RegisterDesc> registers;
    std::vector<PinDesc> pins;
    std::vector<InterruptDesc> interrupts;
    std::vector<TimerDesc> timers;
    std::vector<std::string> isa_extensions;
    std::vector<SpaceId> space_order;

    std::unordered_map<std::string, size_t> reg_index;

    static Model load_from_file(const std::string& path);
    void validate() const;

    const MemoryRegion* find_region(const SpaceId& space, Addr addr) const;
    const RegisterDesc* find_register(const SpaceId& space, Addr addr) const;
    const RegisterDesc* find_register_by_name(const std::string& name) const;
    const InterruptDesc* find_interrupt(uint32_t number) const;
    bool resolve_bit_addr(Addr bit_addr, SpaceId& space,
                          Addr& byte_addr, uint8_t& bit) const;

    size_t mmio_area_size() const;
    size_t pin_area_size() const;
    size_t irq_bitmap_size() const;

    int space_index(const SpaceId& space) const {
        for (size_t i = 0; i < space_order.size(); ++i)
            if (space_order[i] == space) return static_cast<int>(i);
        return -1;
    }

    static std::string reg_key(const SpaceId& space, Addr addr) {
        return space + ":" + std::to_string(addr);
    }
};

} // namespace mcus