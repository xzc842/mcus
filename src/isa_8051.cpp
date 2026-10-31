#include "mcus/instruction.h"
#include "mcus/cpu.h"
#include "mcus/log.h"
#include <memory>
#include <string>
#include <unordered_set>

namespace mcus {
namespace {

class Isa8051 : public InstructionSet {
public:
    explicit Isa8051(const Model& model) : model_(model) {
        for (const auto& e : model.isa_extensions)
            extensions_.insert(e);
    }

    bool has_ext(const std::string& name) const {
        return extensions_.count(name) > 0;
    }

    ExecResult execute(CpuRegisters& regs, MemoryBus& bus) override {
        ExecResult res;
        res.cycles = 1;

        Addr pc = regs.pc;
        uint8_t op = fetch(bus, pc);

        switch (op) {
            case 0x00: pc += 1; break;

            case 0x01: case 0x21: case 0x41: case 0x61:
            case 0x81: case 0xA1: case 0xC1: case 0xE1: {
                uint16_t target = ((pc + 2) & 0xF800) |
                                  ((op & 0xE0) << 3) |
                                  fetch(bus, pc + 1);
                pc = target;
                break;
            }

            case 0x02: {
                uint16_t a = (fetch(bus, pc + 1) << 8) | fetch(bus, pc + 2);
                pc = a;
                break;
            }

            case 0x03: {
                uint8_t a = read_acc(bus);
                a = static_cast<uint8_t>((a >> 1) | (a << 7));
                write_acc(bus, a);
                pc += 1;
                break;
            }

            case 0x04: {
                write_acc(bus, static_cast<uint8_t>(read_acc(bus) + 1));
                pc += 1;
                break;
            }

            case 0x05: {
                uint8_t addr = fetch(bus, pc + 1);
                direct_write(bus, addr,
                             static_cast<uint8_t>(direct_read(bus, addr) + 1));
                pc += 2;
                break;
            }

            case 0x06: case 0x07: {
                uint8_t ri = op & 0x01;
                uint8_t addr = reg_read(bus, ri);
                bus.write8("iram", addr,
                           static_cast<uint8_t>(bus.read8("iram", addr) + 1));
                pc += 1;
                break;
            }

            case 0x08: case 0x09: case 0x0A: case 0x0B:
            case 0x0C: case 0x0D: case 0x0E: case 0x0F: {
                uint8_t rn = op & 0x07;
                reg_write(bus, rn, static_cast<uint8_t>(reg_read(bus, rn) + 1));
                pc += 1;
                break;
            }

            case 0x10: {
                uint8_t bit_addr = fetch(bus, pc + 1);
                int8_t rel = static_cast<int8_t>(fetch(bus, pc + 2));
                bool set = bit_read(bus, bit_addr);
                if (set) {
                    bit_write(bus, bit_addr, false);
                    pc = static_cast<uint16_t>(pc + 3 + rel);
                } else {
                    pc += 3;
                }
                break;
            }

            case 0x11: case 0x31: case 0x51: case 0x71:
            case 0x91: case 0xB1: case 0xD1: case 0xF1: {
                uint16_t target = ((pc + 2) & 0xF800) |
                                  ((op & 0xE0) << 3) |
                                  fetch(bus, pc + 1);
                push(bus, static_cast<uint8_t>((pc + 2) & 0xFF));
                push(bus, static_cast<uint8_t>(((pc + 2) >> 8) & 0xFF));
                pc = target;
                break;
            }

            case 0x12: {
                uint16_t a = (fetch(bus, pc + 1) << 8) | fetch(bus, pc + 2);
                push(bus, static_cast<uint8_t>((pc + 3) & 0xFF));
                push(bus, static_cast<uint8_t>(((pc + 3) >> 8) & 0xFF));
                pc = a;
                break;
            }

            case 0x13: {
                uint8_t a = read_acc(bus);
                uint8_t cy = read_cy(bus);
                uint8_t new_cy = a & 0x01;
                a = static_cast<uint8_t>((a >> 1) | (cy << 7));
                write_cy(bus, new_cy);
                write_acc(bus, a);
                pc += 1;
                break;
            }

            case 0x14: {
                write_acc(bus, static_cast<uint8_t>(read_acc(bus) - 1));
                pc += 1;
                break;
            }

            case 0x15: {
                uint8_t addr = fetch(bus, pc + 1);
                direct_write(bus, addr,
                             static_cast<uint8_t>(direct_read(bus, addr) - 1));
                pc += 2;
                break;
            }

            case 0x16: case 0x17: {
                uint8_t ri = op & 0x01;
                uint8_t addr = reg_read(bus, ri);
                bus.write8("iram", addr,
                           static_cast<uint8_t>(bus.read8("iram", addr) - 1));
                pc += 1;
                break;
            }

            case 0x18: case 0x19: case 0x1A: case 0x1B:
            case 0x1C: case 0x1D: case 0x1E: case 0x1F: {
                uint8_t rn = op & 0x07;
                reg_write(bus, rn, static_cast<uint8_t>(reg_read(bus, rn) - 1));
                pc += 1;
                break;
            }

            case 0x20: {
                uint8_t bit_addr = fetch(bus, pc + 1);
                int8_t rel = static_cast<int8_t>(fetch(bus, pc + 2));
                if (bit_read(bus, bit_addr))
                    pc = static_cast<uint16_t>(pc + 3 + rel);
                else
                    pc += 3;
                break;
            }

            case 0x22: {
                uint8_t hi = pop(bus);
                uint8_t lo = pop(bus);
                pc = (static_cast<uint16_t>(hi) << 8) | lo;
                break;
            }

            case 0x23: {
                uint8_t a = read_acc(bus);
                a = static_cast<uint8_t>((a << 1) | (a >> 7));
                write_acc(bus, a);
                pc += 1;
                break;
            }

            case 0x24: case 0x25: {
                uint8_t src = (op == 0x24) ? fetch(bus, pc + 1)
                                            : direct_read(bus, fetch(bus, pc + 1));
                add_a(bus, src, false);
                pc += 2;
                break;
            }

            case 0x26: case 0x27: {
                uint8_t ri = op & 0x01;
                add_a(bus, bus.read8("iram", reg_read(bus, ri)), false);
                pc += 1;
                break;
            }

            case 0x28: case 0x29: case 0x2A: case 0x2B:
            case 0x2C: case 0x2D: case 0x2E: case 0x2F: {
                add_a(bus, reg_read(bus, op & 0x07), false);
                pc += 1;
                break;
            }

            case 0x30: {
                uint8_t bit_addr = fetch(bus, pc + 1);
                int8_t rel = static_cast<int8_t>(fetch(bus, pc + 2));
                if (!bit_read(bus, bit_addr))
                    pc = static_cast<uint16_t>(pc + 3 + rel);
                else
                    pc += 3;
                break;
            }

            case 0x32: {
                uint8_t hi = pop(bus);
                uint8_t lo = pop(bus);
                pc = (static_cast<uint16_t>(hi) << 8) | lo;
                bus.on_reti();
                break;
            }

            case 0x33: {
                uint8_t a = read_acc(bus);
                uint8_t cy = read_cy(bus);
                uint8_t new_cy = (a >> 7) & 0x01;
                a = static_cast<uint8_t>((a << 1) | cy);
                write_cy(bus, new_cy);
                write_acc(bus, a);
                pc += 1;
                break;
            }

            case 0x34: case 0x35: {
                uint8_t src = (op == 0x34) ? fetch(bus, pc + 1)
                                            : direct_read(bus, fetch(bus, pc + 1));
                add_a(bus, src, true);
                pc += 2;
                break;
            }

            case 0x36: case 0x37: {
                uint8_t ri = op & 0x01;
                add_a(bus, bus.read8("iram", reg_read(bus, ri)), true);
                pc += 1;
                break;
            }

            case 0x38: case 0x39: case 0x3A: case 0x3B:
            case 0x3C: case 0x3D: case 0x3E: case 0x3F: {
                add_a(bus, reg_read(bus, op & 0x07), true);
                pc += 1;
                break;
            }

            case 0x40: {
                int8_t rel = static_cast<int8_t>(fetch(bus, pc + 1));
                if (read_cy(bus))
                    pc = static_cast<uint16_t>(pc + 2 + rel);
                else
                    pc += 2;
                break;
            }

            case 0x42: {
                uint8_t addr = fetch(bus, pc + 1);
                direct_write(bus, addr,
                             static_cast<uint8_t>(direct_read(bus, addr) | read_acc(bus)));
                pc += 2;
                break;
            }

            case 0x43: {
                uint8_t addr = fetch(bus, pc + 1);
                uint8_t imm = fetch(bus, pc + 2);
                direct_write(bus, addr,
                             static_cast<uint8_t>(direct_read(bus, addr) | imm));
                pc += 3;
                break;
            }

            case 0x44: {
                uint8_t imm = fetch(bus, pc + 1);
                write_acc(bus, static_cast<uint8_t>(read_acc(bus) | imm));
                pc += 2;
                break;
            }

            case 0x45: {
                uint8_t addr = fetch(bus, pc + 1);
                write_acc(bus, static_cast<uint8_t>(read_acc(bus) | direct_read(bus, addr)));
                pc += 2;
                break;
            }

            case 0x46: case 0x47: {
                uint8_t ri = op & 0x01;
                write_acc(bus, static_cast<uint8_t>(
                    read_acc(bus) | bus.read8("iram", reg_read(bus, ri))));
                pc += 1;
                break;
            }

            case 0x48: case 0x49: case 0x4A: case 0x4B:
            case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
                write_acc(bus, static_cast<uint8_t>(
                    read_acc(bus) | reg_read(bus, op & 0x07)));
                pc += 1;
                break;
            }

            case 0x50: {
                int8_t rel = static_cast<int8_t>(fetch(bus, pc + 1));
                if (!read_cy(bus))
                    pc = static_cast<uint16_t>(pc + 2 + rel);
                else
                    pc += 2;
                break;
            }

            case 0x52: {
                uint8_t addr = fetch(bus, pc + 1);
                direct_write(bus, addr,
                             static_cast<uint8_t>(direct_read(bus, addr) & read_acc(bus)));
                pc += 2;
                break;
            }

            case 0x53: {
                uint8_t addr = fetch(bus, pc + 1);
                uint8_t imm = fetch(bus, pc + 2);
                direct_write(bus, addr,
                             static_cast<uint8_t>(direct_read(bus, addr) & imm));
                pc += 3;
                break;
            }

            case 0x54: {
                uint8_t imm = fetch(bus, pc + 1);
                write_acc(bus, static_cast<uint8_t>(read_acc(bus) & imm));
                pc += 2;
                break;
            }

            case 0x55: {
                uint8_t addr = fetch(bus, pc + 1);
                write_acc(bus, static_cast<uint8_t>(read_acc(bus) & direct_read(bus, addr)));
                pc += 2;
                break;
            }

            case 0x56: case 0x57: {
                uint8_t ri = op & 0x01;
                write_acc(bus, static_cast<uint8_t>(
                    read_acc(bus) & bus.read8("iram", reg_read(bus, ri))));
                pc += 1;
                break;
            }

            case 0x58: case 0x59: case 0x5A: case 0x5B:
            case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
                write_acc(bus, static_cast<uint8_t>(
                    read_acc(bus) & reg_read(bus, op & 0x07)));
                pc += 1;
                break;
            }

            case 0x60: {
                int8_t rel = static_cast<int8_t>(fetch(bus, pc + 1));
                if (read_acc(bus) == 0)
                    pc = static_cast<uint16_t>(pc + 2 + rel);
                else
                    pc += 2;
                break;
            }

            case 0x62: {
                uint8_t addr = fetch(bus, pc + 1);
                direct_write(bus, addr,
                             static_cast<uint8_t>(direct_read(bus, addr) ^ read_acc(bus)));
                pc += 2;
                break;
            }

            case 0x63: {
                uint8_t addr = fetch(bus, pc + 1);
                uint8_t imm = fetch(bus, pc + 2);
                direct_write(bus, addr,
                             static_cast<uint8_t>(direct_read(bus, addr) ^ imm));
                pc += 3;
                break;
            }

            case 0x64: {
                uint8_t imm = fetch(bus, pc + 1);
                write_acc(bus, static_cast<uint8_t>(read_acc(bus) ^ imm));
                pc += 2;
                break;
            }

            case 0x65: {
                uint8_t addr = fetch(bus, pc + 1);
                write_acc(bus, static_cast<uint8_t>(read_acc(bus) ^ direct_read(bus, addr)));
                pc += 2;
                break;
            }

            case 0x66: case 0x67: {
                uint8_t ri = op & 0x01;
                write_acc(bus, static_cast<uint8_t>(
                    read_acc(bus) ^ bus.read8("iram", reg_read(bus, ri))));
                pc += 1;
                break;
            }

            case 0x68: case 0x69: case 0x6A: case 0x6B:
            case 0x6C: case 0x6D: case 0x6E: case 0x6F: {
                write_acc(bus, static_cast<uint8_t>(
                    read_acc(bus) ^ reg_read(bus, op & 0x07)));
                pc += 1;
                break;
            }

            case 0x70: {
                int8_t rel = static_cast<int8_t>(fetch(bus, pc + 1));
                if (read_acc(bus) != 0)
                    pc = static_cast<uint16_t>(pc + 2 + rel);
                else
                    pc += 2;
                break;
            }

            case 0x72: {
                uint8_t bit_addr = fetch(bus, pc + 1);
                write_cy(bus, read_cy(bus) || bit_read(bus, bit_addr));
                pc += 2;
                break;
            }

            case 0x73: {
                uint16_t dptr = (read_dph(bus) << 8) | read_dpl(bus);
                pc = static_cast<uint16_t>(read_acc(bus) + dptr);
                break;
            }

            case 0x74: {
                write_acc(bus, fetch(bus, pc + 1));
                pc += 2;
                break;
            }

            case 0x75: {
                uint8_t addr = fetch(bus, pc + 1);
                uint8_t imm = fetch(bus, pc + 2);
                direct_write(bus, addr, imm);
                pc += 3;
                break;
            }

            case 0x76: case 0x77: {
                uint8_t ri = op & 0x01;
                uint8_t imm = fetch(bus, pc + 1);
                bus.write8("iram", reg_read(bus, ri), imm);
                pc += 2;
                break;
            }

            case 0x78: case 0x79: case 0x7A: case 0x7B:
            case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
                reg_write(bus, op & 0x07, fetch(bus, pc + 1));
                pc += 2;
                break;
            }

            case 0x80: {
                int8_t rel = static_cast<int8_t>(fetch(bus, pc + 1));
                pc = static_cast<uint16_t>(pc + 2 + rel);
                break;
            }

            case 0x82: {
                uint8_t bit_addr = fetch(bus, pc + 1);
                write_cy(bus, read_cy(bus) && bit_read(bus, bit_addr));
                pc += 2;
                break;
            }

            case 0x83: {
                Addr a = static_cast<Addr>(pc + 1 + read_acc(bus));
                write_acc(bus, bus.read8("code", a));
                pc += 1;
                break;
            }

            case 0x84: {
                uint8_t a = read_acc(bus);
                uint8_t b = bus.read8("sfr", 0xF0);
                if (b == 0) {
                    write_cy(bus, 1);
                    write_ov(bus, 1);
                } else {
                    write_acc(bus, static_cast<uint8_t>(a / b));
                    bus.write8("sfr", 0xF0, static_cast<uint8_t>(a % b));
                    write_cy(bus, 0);
                    write_ov(bus, 0);
                }
                pc += 1;
                break;
            }

            case 0x85: {
                uint8_t src = fetch(bus, pc + 1);
                uint8_t dst = fetch(bus, pc + 2);
                direct_write(bus, dst, direct_read(bus, src));
                pc += 3;
                break;
            }

            case 0x86: case 0x87: {
                uint8_t ri = op & 0x01;
                uint8_t dst = fetch(bus, pc + 1);
                direct_write(bus, dst, bus.read8("iram", reg_read(bus, ri)));
                pc += 2;
                break;
            }

            case 0x88: case 0x89: case 0x8A: case 0x8B:
            case 0x8C: case 0x8D: case 0x8E: case 0x8F: {
                uint8_t dst = fetch(bus, pc + 1);
                direct_write(bus, dst, reg_read(bus, op & 0x07));
                pc += 2;
                break;
            }

            case 0x90: {
                write_dph(bus, fetch(bus, pc + 1));
                write_dpl(bus, fetch(bus, pc + 2));
                pc += 3;
                break;
            }

            case 0x92: {
                uint8_t bit_addr = fetch(bus, pc + 1);
                bit_write(bus, bit_addr, read_cy(bus));
                pc += 2;
                break;
            }

            case 0x93: {
                uint16_t dptr = (read_dph(bus) << 8) | read_dpl(bus);
                Addr a = static_cast<Addr>(dptr + read_acc(bus));
                write_acc(bus, bus.read8("code", a));
                pc += 1;
                break;
            }

            case 0x94: case 0x95: {
                uint8_t src = (op == 0x94) ? fetch(bus, pc + 1)
                                            : direct_read(bus, fetch(bus, pc + 1));
                subb_a(bus, src);
                pc += 2;
                break;
            }

            case 0x96: case 0x97: {
                uint8_t ri = op & 0x01;
                subb_a(bus, bus.read8("iram", reg_read(bus, ri)));
                pc += 1;
                break;
            }

            case 0x98: case 0x99: case 0x9A: case 0x9B:
            case 0x9C: case 0x9D: case 0x9E: case 0x9F: {
                subb_a(bus, reg_read(bus, op & 0x07));
                pc += 1;
                break;
            }

            case 0xA0: {
                uint8_t bit_addr = fetch(bus, pc + 1);
                write_cy(bus, read_cy(bus) || !bit_read(bus, bit_addr));
                pc += 2;
                break;
            }

            case 0xA2: {
                uint8_t bit_addr = fetch(bus, pc + 1);
                write_cy(bus, read_cy(bus) && !bit_read(bus, bit_addr));
                pc += 2;
                break;
            }

            case 0xA3: {
                uint16_t dptr = (read_dph(bus) << 8) | read_dpl(bus);
                dptr = static_cast<uint16_t>(dptr + 1);
                write_dph(bus, static_cast<uint8_t>(dptr >> 8));
                write_dpl(bus, static_cast<uint8_t>(dptr & 0xFF));
                pc += 1;
                break;
            }

            case 0xA4: {
                uint16_t r = static_cast<uint16_t>(read_acc(bus)) *
                             bus.read8("sfr", 0xF0);
                write_acc(bus, static_cast<uint8_t>(r & 0xFF));
                bus.write8("sfr", 0xF0, static_cast<uint8_t>(r >> 8));
                write_cy(bus, 0);
                write_ov(bus, (r > 0xFF) ? 1 : 0);
                pc += 1;
                break;
            }

            case 0xA5: {
                if (!has_ext("wch_dptr1_movx")) {
                    res.halted = true;
                    break;
                }
                uint8_t xbus = bus.read8("sfr", 0xA2);
                uint8_t saved_dps = xbus & 0x01;
                bus.write8("sfr", 0xA2, static_cast<uint8_t>(xbus | 0x01));
                uint16_t dptr = (read_dph(bus) << 8) | read_dpl(bus);
                bus.write8("xram", dptr, read_acc(bus));
                dptr = static_cast<uint16_t>(dptr + 1);
                write_dph(bus, static_cast<uint8_t>(dptr >> 8));
                write_dpl(bus, static_cast<uint8_t>(dptr & 0xFF));
                bus.write8("sfr", 0xA2,
                           static_cast<uint8_t>((xbus & ~0x01) | saved_dps));
                pc += 1;
                break;
            }

            case 0xA6: case 0xA7: {
                uint8_t ri = op & 0x01;
                uint8_t src = fetch(bus, pc + 1);
                bus.write8("iram", reg_read(bus, ri), direct_read(bus, src));
                pc += 2;
                break;
            }

            case 0xA8: case 0xA9: case 0xAA: case 0xAB:
            case 0xAC: case 0xAD: case 0xAE: case 0xAF: {
                reg_write(bus, op & 0x07, direct_read(bus, fetch(bus, pc + 1)));
                pc += 2;
                break;
            }

            case 0xB0: {
                uint8_t bit_addr = fetch(bus, pc + 1);
                write_cy(bus, read_cy(bus) && !bit_read(bus, bit_addr));
                pc += 2;
                break;
            }

            case 0xB2: {
                uint8_t bit_addr = fetch(bus, pc + 1);
                bit_write(bus, bit_addr, !bit_read(bus, bit_addr));
                pc += 2;
                break;
            }

            case 0xB3: write_cy(bus, !read_cy(bus)); pc += 1; break;

            case 0xB4: {
                uint8_t imm = fetch(bus, pc + 1);
                int8_t rel = static_cast<int8_t>(fetch(bus, pc + 2));
                uint8_t a = read_acc(bus);
                write_cy(bus, a < imm ? 1 : 0);
                if (a != imm)
                    pc = static_cast<uint16_t>(pc + 3 + rel);
                else
                    pc += 3;
                break;
            }

            case 0xB5: {
                uint8_t addr = fetch(bus, pc + 1);
                int8_t rel = static_cast<int8_t>(fetch(bus, pc + 2));
                uint8_t a = read_acc(bus);
                uint8_t d = direct_read(bus, addr);
                write_cy(bus, a < d ? 1 : 0);
                if (a != d)
                    pc = static_cast<uint16_t>(pc + 3 + rel);
                else
                    pc += 3;
                break;
            }

            case 0xB6: case 0xB7: {
                uint8_t ri = op & 0x01;
                uint8_t imm = fetch(bus, pc + 1);
                int8_t rel = static_cast<int8_t>(fetch(bus, pc + 2));
                uint8_t v = bus.read8("iram", reg_read(bus, ri));
                write_cy(bus, v < imm ? 1 : 0);
                if (v != imm)
                    pc = static_cast<uint16_t>(pc + 3 + rel);
                else
                    pc += 3;
                break;
            }

            case 0xB8: case 0xB9: case 0xBA: case 0xBB:
            case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
                uint8_t rn = op & 0x07;
                uint8_t imm = fetch(bus, pc + 1);
                int8_t rel = static_cast<int8_t>(fetch(bus, pc + 2));
                uint8_t v = reg_read(bus, rn);
                write_cy(bus, v < imm ? 1 : 0);
                if (v != imm)
                    pc = static_cast<uint16_t>(pc + 3 + rel);
                else
                    pc += 3;
                break;
            }

            case 0xC0: {
                uint8_t addr = fetch(bus, pc + 1);
                push(bus, direct_read(bus, addr));
                pc += 2;
                break;
            }

            case 0xC2: {
                uint8_t bit_addr = fetch(bus, pc + 1);
                bit_write(bus, bit_addr, false);
                pc += 2;
                break;
            }

            case 0xC3: write_cy(bus, 0); pc += 1; break;

            case 0xC4: {
                uint8_t a = read_acc(bus);
                a = static_cast<uint8_t>((a << 4) | (a >> 4));
                write_acc(bus, a);
                pc += 1;
                break;
            }

            case 0xC5: {
                uint8_t addr = fetch(bus, pc + 1);
                uint8_t a = read_acc(bus);
                uint8_t d = direct_read(bus, addr);
                direct_write(bus, addr, a);
                write_acc(bus, d);
                pc += 2;
                break;
            }

            case 0xC6: case 0xC7: {
                uint8_t ri = op & 0x01;
                uint8_t addr = reg_read(bus, ri);
                uint8_t a = read_acc(bus);
                uint8_t d = bus.read8("iram", addr);
                bus.write8("iram", addr, a);
                write_acc(bus, d);
                pc += 1;
                break;
            }

            case 0xC8: case 0xC9: case 0xCA: case 0xCB:
            case 0xCC: case 0xCD: case 0xCE: case 0xCF: {
                uint8_t rn = op & 0x07;
                uint8_t a = read_acc(bus);
                uint8_t d = reg_read(bus, rn);
                reg_write(bus, rn, a);
                write_acc(bus, d);
                pc += 1;
                break;
            }

            case 0xD0: {
                uint8_t addr = fetch(bus, pc + 1);
                direct_write(bus, addr, pop(bus));
                pc += 2;
                break;
            }

            case 0xD2: {
                uint8_t bit_addr = fetch(bus, pc + 1);
                bit_write(bus, bit_addr, true);
                pc += 2;
                break;
            }

            case 0xD3: write_cy(bus, 1); pc += 1; break;

            case 0xD4: {
                uint8_t a = read_acc(bus);
                uint8_t cy = read_cy(bus) ? 1 : 0;
                bool ac = read_ac(bus);
                bool new_ac = false;
                if ((a & 0x0F) > 0x09 || ac) {
                    uint8_t lo = static_cast<uint8_t>(a + 0x06);
                    new_ac = ((lo & 0xF0) != (a & 0xF0));
                    a = lo;
                }
                if ((a & 0xF0) > 0x90 || cy) {
                    a = static_cast<uint8_t>(a + 0x60);
                    cy = 1;
                }
                write_cy(bus, cy);
                write_ac(bus, new_ac);
                write_acc(bus, a);
                pc += 1;
                break;
            }

            case 0xD5: {
                uint8_t addr = fetch(bus, pc + 1);
                int8_t rel = static_cast<int8_t>(fetch(bus, pc + 2));
                uint8_t v = static_cast<uint8_t>(direct_read(bus, addr) - 1);
                direct_write(bus, addr, v);
                if (v != 0)
                    pc = static_cast<uint16_t>(pc + 3 + rel);
                else
                    pc += 3;
                break;
            }

            case 0xD6: case 0xD7: {
                uint8_t ri = op & 0x01;
                uint8_t addr = reg_read(bus, ri);
                uint8_t a = read_acc(bus);
                uint8_t d = bus.read8("iram", addr);
                uint8_t new_a = static_cast<uint8_t>((a & 0xF0) | (d & 0x0F));
                uint8_t new_d = static_cast<uint8_t>((d & 0xF0) | (a & 0x0F));
                bus.write8("iram", addr, new_d);
                write_acc(bus, new_a);
                pc += 1;
                break;
            }

            case 0xD8: case 0xD9: case 0xDA: case 0xDB:
            case 0xDC: case 0xDD: case 0xDE: case 0xDF: {
                uint8_t rn = op & 0x07;
                int8_t rel = static_cast<int8_t>(fetch(bus, pc + 1));
                uint8_t v = static_cast<uint8_t>(reg_read(bus, rn) - 1);
                reg_write(bus, rn, v);
                if (v != 0)
                    pc = static_cast<uint16_t>(pc + 2 + rel);
                else
                    pc += 2;
                break;
            }

            case 0xE0: {
                uint16_t dptr = (read_dph(bus) << 8) | read_dpl(bus);
                write_acc(bus, bus.read8("xram", dptr));
                pc += 1;
                break;
            }

            case 0xE2: case 0xE3: {
                uint8_t ri = op & 0x01;
                uint8_t p2 = bus.read8("sfr", 0xA0);
                uint16_t addr = (static_cast<uint16_t>(p2) << 8) | reg_read(bus, ri);
                write_acc(bus, bus.read8("xram", addr));
                pc += 1;
                break;
            }

            case 0xE4: write_acc(bus, 0); pc += 1; break;

            case 0xE5: {
                uint8_t addr = fetch(bus, pc + 1);
                write_acc(bus, direct_read(bus, addr));
                pc += 2;
                break;
            }

            case 0xE6: case 0xE7: {
                uint8_t ri = op & 0x01;
                write_acc(bus, bus.read8("iram", reg_read(bus, ri)));
                pc += 1;
                break;
            }

            case 0xE8: case 0xE9: case 0xEA: case 0xEB:
            case 0xEC: case 0xED: case 0xEE: case 0xEF: {
                write_acc(bus, reg_read(bus, op & 0x07));
                pc += 1;
                break;
            }

            case 0xF0: {
                uint16_t dptr = (read_dph(bus) << 8) | read_dpl(bus);
                bus.write8("xram", dptr, read_acc(bus));
                pc += 1;
                break;
            }

            case 0xF2: case 0xF3: {
                uint8_t ri = op & 0x01;
                uint8_t p2 = bus.read8("sfr", 0xA0);
                uint16_t addr = (static_cast<uint16_t>(p2) << 8) | reg_read(bus, ri);
                bus.write8("xram", addr, read_acc(bus));
                pc += 1;
                break;
            }

            case 0xF4: {
                write_acc(bus, static_cast<uint8_t>(~read_acc(bus)));
                pc += 1;
                break;
            }

            case 0xF5: {
                uint8_t addr = fetch(bus, pc + 1);
                direct_write(bus, addr, read_acc(bus));
                pc += 2;
                break;
            }

            case 0xF6: case 0xF7: {
                uint8_t ri = op & 0x01;
                bus.write8("iram", reg_read(bus, ri), read_acc(bus));
                pc += 1;
                break;
            }

            case 0xF8: case 0xF9: case 0xFA: case 0xFB:
            case 0xFC: case 0xFD: case 0xFE: case 0xFF: {
                reg_write(bus, op & 0x07, read_acc(bus));
                pc += 1;
                break;
            }

            default:
                MCUS_WARN("8051 opcode unimplemented: 0x" + std::to_string(op));
                res.halted = true;
                break;
        }

        regs.pc = pc;
        return res;
    }

private:
    const Model& model_;
    std::unordered_set<std::string> extensions_;

    uint8_t fetch(MemoryBus& bus, Addr addr) {
        return bus.read8("code", addr);
    }

    void push(MemoryBus& bus, uint8_t v) {
        uint8_t sp = static_cast<uint8_t>(bus.read8("sfr", 0x81) + 1);
        bus.write8("sfr", 0x81, sp);
        bus.write8("iram", sp, v);
        MCUS_DEBUG("push iram[0x" + std::to_string(sp) +
                   "] = 0x" + std::to_string(v));
    }

    uint8_t pop(MemoryBus& bus) {
        uint8_t sp = bus.read8("sfr", 0x81);
        uint8_t v = bus.read8("iram", sp);
        bus.write8("sfr", 0x81, static_cast<uint8_t>(sp - 1));
        MCUS_DEBUG("pop iram[0x" + std::to_string(sp) +
                   "] = 0x" + std::to_string(v));
        return v;
    }

    uint8_t read_acc(MemoryBus& bus) { return bus.read8("sfr", 0xE0); }

    void write_acc(MemoryBus& bus, uint8_t v) {
        bus.write8("sfr", 0xE0, v);
        uint8_t p = 0;
        for (int i = 0; i < 8; ++i) p ^= ((v >> i) & 1);
        uint8_t psw = bus.read8("sfr", 0xD0);
        psw = static_cast<uint8_t>((psw & 0xFE) | p);
        bus.write8("sfr", 0xD0, psw);
    }

    uint8_t read_dpl(MemoryBus& bus) { return bus.read8("sfr", 0x82); }
    uint8_t read_dph(MemoryBus& bus) { return bus.read8("sfr", 0x83); }
    void write_dpl(MemoryBus& bus, uint8_t v) { bus.write8("sfr", 0x82, v); }
    void write_dph(MemoryBus& bus, uint8_t v) { bus.write8("sfr", 0x83, v); }

    bool read_cy(MemoryBus& bus) { return (bus.read8("sfr", 0xD0) & 0x80) != 0; }

    void write_cy(MemoryBus& bus, bool v) {
        uint8_t psw = bus.read8("sfr", 0xD0);
        psw = static_cast<uint8_t>((psw & 0x7F) | (v ? 0x80 : 0));
        bus.write8("sfr", 0xD0, psw);
    }

    bool read_ac(MemoryBus& bus) { return (bus.read8("sfr", 0xD0) & 0x40) != 0; }

    void write_ac(MemoryBus& bus, bool v) {
        uint8_t psw = bus.read8("sfr", 0xD0);
        psw = static_cast<uint8_t>((psw & 0xBF) | (v ? 0x40 : 0));
        bus.write8("sfr", 0xD0, psw);
    }

    void write_ov(MemoryBus& bus, int v) {
        uint8_t psw = bus.read8("sfr", 0xD0);
        psw = static_cast<uint8_t>((psw & 0xFB) | (v ? 0x04 : 0));
        bus.write8("sfr", 0xD0, psw);
    }

    uint8_t reg_bank_base(MemoryBus& bus) {
        uint8_t psw = bus.read8("sfr", 0xD0);
        return static_cast<uint8_t>((psw & 0x18) >> 3) * 8;
    }

    uint8_t reg_read(MemoryBus& bus, uint8_t rn) {
        return bus.read8("iram", static_cast<Addr>(reg_bank_base(bus) + rn));
    }

    void reg_write(MemoryBus& bus, uint8_t rn, uint8_t v) {
        bus.write8("iram", static_cast<Addr>(reg_bank_base(bus) + rn), v);
    }

    uint8_t direct_read(MemoryBus& bus, uint8_t addr) {
        if (addr < 0x80) return bus.read8("iram", addr);
        return bus.read8("sfr", addr);
    }

    void direct_write(MemoryBus& bus, uint8_t addr, uint8_t v) {
        if (addr < 0x80) bus.write8("iram", addr, v);
        else bus.write8("sfr", addr, v);
    }

    bool bit_read(MemoryBus& bus, uint8_t bit_addr) {
        SpaceId space; Addr byte_addr; uint8_t bit;
        if (!model_.resolve_bit_addr(bit_addr, space, byte_addr, bit))
            return false;
        return ((bus.read8(space, byte_addr) >> bit) & 1) != 0;
    }

    void bit_write(MemoryBus& bus, uint8_t bit_addr, bool v) {
        SpaceId space; Addr byte_addr; uint8_t bit;
        if (!model_.resolve_bit_addr(bit_addr, space, byte_addr, bit))
            return;
        uint8_t cur = bus.read8(space, byte_addr);
        if (v) cur |= static_cast<uint8_t>(1u << bit);
        else cur &= static_cast<uint8_t>(~(1u << bit));
        bus.write8(space, byte_addr, cur);
    }

    void add_a(MemoryBus& bus, uint8_t src, bool with_carry) {
        uint8_t a = read_acc(bus);
        uint8_t c = with_carry ? (read_cy(bus) ? 1 : 0) : 0;
        uint16_t r = static_cast<uint16_t>(a) + src + c;
        uint8_t result = static_cast<uint8_t>(r & 0xFF);
        write_cy(bus, (r > 0xFF) ? 1 : 0);
        write_ac(bus, (((a & 0x0F) + (src & 0x0F) + c) > 0x0F) ? 1 : 0);
        write_ov(bus, ((~(a ^ src) & (a ^ result) & 0x80) != 0) ? 1 : 0);
        write_acc(bus, result);
    }

    void subb_a(MemoryBus& bus, uint8_t src) {
        uint8_t a = read_acc(bus);
        uint8_t c = read_cy(bus) ? 1 : 0;
        int r = static_cast<int>(a) - src - c;
        uint8_t result = static_cast<uint8_t>(r & 0xFF);
        write_cy(bus, (r < 0) ? 1 : 0);
        write_ac(bus, (((a & 0x0F) - (src & 0x0F) - c) < 0) ? 1 : 0);
        write_ov(bus, ((((a ^ src) & (a ^ result)) & 0x80) != 0) ? 1 : 0);
        write_acc(bus, result);
    }
};

} // namespace

void register_isa_8051() {
    IsaRegistry::instance().register_isa(
        "8051",
        [](const Model& m) -> std::unique_ptr<InstructionSet> {
            return std::make_unique<Isa8051>(m);
        });
}

} // namespace mcus