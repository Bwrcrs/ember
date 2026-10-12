// ============================================================
//  Disassembler —— 见 disassembler.hpp
// ============================================================

#include "disassembler.hpp"

#include "hexfmt.hpp"

// hex1 / hex2 / hex3 现在共用 hexfmt.hpp 里的那一份——
// 追踪器也要印同样的十六进制，各写一份迟早会出现
// 同一数字两种写法，比对时全是噪音。见 hexfmt.hpp 的说明。

std::string registerName(int index) {
    return "V" + hex1(index);
}

std::string disassemble(const Decoded& d, const Quirks& quirks) {
    switch (d.op) {
        case Opcode::SYS:  return "SYS " + hex3(d.nnn);
        case Opcode::CLS:  return "CLS";
        case Opcode::RET:  return "RET";

        case Opcode::JP:   return "JP " + hex3(d.nnn);
        case Opcode::CALL: return "CALL " + hex3(d.nnn);

        case Opcode::SE_BYTE:
            return "SE " + registerName(d.x) + ", " + hex2(d.nn);
        case Opcode::SNE_BYTE:
            return "SNE " + registerName(d.x) + ", " + hex2(d.nn);

        case Opcode::LD_BYTE:
            return "LD " + registerName(d.x) + ", " + hex2(d.nn);
        case Opcode::ADD_BYTE:
            return "ADD " + registerName(d.x) + ", " + hex2(d.nn);

        case Opcode::LD_REG:
            return "LD " + registerName(d.x) + ", " + registerName(d.y);
        case Opcode::OR:
            return "OR " + registerName(d.x) + ", " + registerName(d.y);
        case Opcode::AND:
            return "AND " + registerName(d.x) + ", " + registerName(d.y);
        case Opcode::XOR:
            return "XOR " + registerName(d.x) + ", " + registerName(d.y);
        case Opcode::ADD_REG:
            return "ADD " + registerName(d.x) + ", " + registerName(d.y);
        case Opcode::SUB:
            return "SUB " + registerName(d.x) + ", " + registerName(d.y);

        // SHR / SHL 是唯一两个印法依赖 quirks 的指令。
        // 见 disassembler.hpp 文件头那段关于"不可能往返一致"的说明。
        case Opcode::SHR:
            if (quirks.shiftVxInPlace) {
                return "SHR " + registerName(d.x);
            }
            return "SHR " + registerName(d.x) + ", " + registerName(d.y);
        case Opcode::SHL:
            if (quirks.shiftVxInPlace) {
                return "SHL " + registerName(d.x);
            }
            return "SHL " + registerName(d.x) + ", " + registerName(d.y);

        case Opcode::SUBN:
            return "SUBN " + registerName(d.x) + ", " + registerName(d.y);

        case Opcode::SE_REG:
            return "SE " + registerName(d.x) + ", " + registerName(d.y);
        case Opcode::SNE_REG:
            return "SNE " + registerName(d.x) + ", " + registerName(d.y);

        case Opcode::LD_ADDR:
            return "LD I, " + hex3(d.nnn);

        // Bnnn 的印法也随 quirk 变：加 V0 还是加 Vx。
        // 两个都印成 "JP V?, addr" 的形式，因为目标地址取决于
        // 运行时的寄存器值，静态印不出确切地址。
        case Opcode::JP_V0:
            if (quirks.jumpUsesVx) {
                return "JP " + registerName(d.x) + ", " + hex3(d.nnn);
            }
            return "JP V0, " + hex3(d.nnn);

        case Opcode::RND:
            return "RND " + registerName(d.x) + ", " + hex2(d.nn);

        case Opcode::DRW:
            // 行数是一位十六进制，不带 0x——写成 "DRW V1, V2, 5"
            // 比 "DRW V1, V2, 0x5" 更像传统汇编写法。
            return "DRW " + registerName(d.x) + ", " + registerName(d.y) +
                   ", " + hex1(d.n);

        case Opcode::SKP:  return "SKP " + registerName(d.x);
        case Opcode::SKNP: return "SKNP " + registerName(d.x);

        // 定时器这几条长得像但方向不同，印出来一眼能分清：
        //   LD V1, DT  寄存器 <- 定时器
        //   LD DT, V1  定时器 <- 寄存器
        // 这个方向搞反是常见错误，所以打印格式上刻意拉开差距。
        case Opcode::LD_VX_DT: return "LD " + registerName(d.x) + ", DT";
        case Opcode::LD_VX_K:  return "LD " + registerName(d.x) + ", K";
        case Opcode::LD_DT_VX: return "LD DT, " + registerName(d.x);
        case Opcode::LD_ST_VX: return "LD ST, " + registerName(d.x);

        case Opcode::ADD_I_VX: return "ADD I, " + registerName(d.x);
        case Opcode::LD_F_VX:  return "LD F, " + registerName(d.x);
        case Opcode::LD_B_VX:  return "LD B, " + registerName(d.x);
        case Opcode::LD_I_VX:  return "LD [I], " + registerName(d.x);
        case Opcode::LD_VX_I:  return "LD " + registerName(d.x) + ", [I]";

        case Opcode::UNKNOWN:
            // 印出**完整的 16 位**原始值，这样 trace 里能直接看到
            // "跑到什么垃圾字节上了"。
            //
            // ★ 这里原来是 hex3(d.raw & 0x0FFF)，把 0xFFFF 印成 "0xFFF"。
            //   对"未知指令"来说这是丢信息：0xFFF 本身是 Fx55 系
            //   家族的合法形状，而 0xFFFF 是完全不同的东西。
            //   看 trace 的人要的是那几个字节的**原样**。
            return "??? " + hex4(d.raw);
    }

    // 枚举列全了，走不到这里。
    return "???";
}

std::string disassemble(std::uint16_t raw, const Quirks& quirks) {
    return disassemble(decode(raw), quirks);
}
