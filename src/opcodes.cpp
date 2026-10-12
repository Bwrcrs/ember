// ============================================================
//  Opcodes —— 译码
//  见 opcodes.hpp 里那段很长的说明，尤其是"为什么不用掩码表"。
// ============================================================

#include "opcodes.hpp"

namespace {

// 8xy_ 家族。这个家族是唯一一个真正按低半字节分派的。
Opcode decodeLogic(std::uint16_t raw) {
    switch (raw & 0x000F) {
        case 0x0: return Opcode::LD_REG;
        case 0x1: return Opcode::OR;
        case 0x2: return Opcode::AND;
        case 0x3: return Opcode::XOR;
        case 0x4: return Opcode::ADD_REG;
        case 0x5: return Opcode::SUB;
        case 0x6: return Opcode::SHR;
        case 0x7: return Opcode::SUBN;
        case 0xE: return Opcode::SHL;
        default:  return Opcode::UNKNOWN;
    }
}

// Fx__ 家族。
//
// ★ 必须按**低字节**分派，不能按低半字节：
//   Fx15（LD DT, Vx）和 Fx55（LD [I], Vx）的低半字节都是 5。
//   这就是 opcodes.hpp 里那条"结构性事实"的落地处。
Opcode decodeMisc(std::uint16_t raw) {
    switch (raw & 0x00FF) {
        case 0x07: return Opcode::LD_VX_DT;
        case 0x0A: return Opcode::LD_VX_K;
        case 0x15: return Opcode::LD_DT_VX;
        case 0x18: return Opcode::LD_ST_VX;
        case 0x1E: return Opcode::ADD_I_VX;
        case 0x29: return Opcode::LD_F_VX;
        case 0x33: return Opcode::LD_B_VX;
        case 0x55: return Opcode::LD_I_VX;
        case 0x65: return Opcode::LD_VX_I;
        default:   return Opcode::UNKNOWN;
    }
}

}  // namespace

Decoded decode(std::uint16_t raw) {
    Decoded d;
    d.raw = raw;
    d.x = static_cast<std::uint8_t>((raw >> 8) & 0x000F);
    d.y = static_cast<std::uint8_t>((raw >> 4) & 0x000F);
    d.n = static_cast<std::uint8_t>(raw & 0x000F);
    d.nn = static_cast<std::uint8_t>(raw & 0x00FF);
    d.nnn = static_cast<std::uint16_t>(raw & 0x0FFF);

    switch (raw >> 12) {
        case 0x0:
            if (raw == 0x00E0) {
                d.op = Opcode::CLS;
            } else if (raw == 0x00EE) {
                d.op = Opcode::RET;
            } else {
                // 0nnn 是"调用机器码子程序"，现代模拟器一律当空操作。
                //
                // ★ 这里**故意**把它归成 SYS 而不是 UNKNOWN，
                //   因为这两种情况在 trace 里长得不一样，而区别很重要：
                //   全零内存译出来就是 0x0000（即 SYS 0x000），
                //   trace 里看到一屏 "SYS 0x000" 你就知道"PC 跑到没代码的地方了"，
                //   而看到 "???" 你还得再想一下 ??? 是什么。
                //
                // 【注意】SUPER-CHIP 的 00Cn / 00FB-00FF（滚动屏幕）也会
                //   落进这一支，被当成 SYS 空操作。基础 CHIP-8 不含它们，
                //   这是刻意的取舍——见 README 的"已知取舍"。
                d.op = Opcode::SYS;
            }
            break;

        case 0x1: d.op = Opcode::JP;       break;
        case 0x2: d.op = Opcode::CALL;     break;
        case 0x3: d.op = Opcode::SE_BYTE;  break;
        case 0x4: d.op = Opcode::SNE_BYTE; break;

        case 0x5:
            // ★ 5xy_ 只有 n == 0 才是"跳过"。n != 0 的话不是 CHIP-8 指令
            //   （5xy2 / 5xy3 是 XO-CHIP 的存/取寄存器区间）。
            //   如果这里偷懒不看 n，那么内存里任何一个 0x5?x? 的字节
            //   都会被当成跳过执行——**一次静默的控制流劫持**。
            d.op = (d.n == 0) ? Opcode::SE_REG : Opcode::UNKNOWN;
            break;

        case 0x6: d.op = Opcode::LD_BYTE;  break;
        case 0x7: d.op = Opcode::ADD_BYTE; break;

        case 0x8: d.op = decodeLogic(raw); break;

        case 0x9:
            // 和 5xy0 同理：只有 n == 0 才算。
            d.op = (d.n == 0) ? Opcode::SNE_REG : Opcode::UNKNOWN;
            break;

        case 0xA: d.op = Opcode::LD_ADDR; break;
        case 0xB: d.op = Opcode::JP_V0;   break;
        case 0xC: d.op = Opcode::RND;     break;
        case 0xD: d.op = Opcode::DRW;     break;

        case 0xE:
            // 同样按低字节分派，不是低半字节。
            switch (raw & 0x00FF) {
                case 0x9E: d.op = Opcode::SKP;  break;
                case 0xA1: d.op = Opcode::SKNP; break;
                default:   d.op = Opcode::UNKNOWN; break;
            }
            break;

        case 0xF: d.op = decodeMisc(raw); break;

        default:
            // 高半字节只有 0..F 十六种，case 已经列全了，
            // 走不到这里。留着是为了让编译器相信所有路径都有值。
            d.op = Opcode::UNKNOWN;
            break;
    }

    return d;
}

bool isDefined(Opcode op) {
    return op != Opcode::UNKNOWN;
}

const char* opcodeName(Opcode op) {
    switch (op) {
        case Opcode::SYS:       return "SYS";
        case Opcode::CLS:       return "CLS";
        case Opcode::RET:       return "RET";
        case Opcode::JP:        return "JP";
        case Opcode::CALL:      return "CALL";
        case Opcode::SE_BYTE:   return "SE";
        case Opcode::SNE_BYTE:  return "SNE";
        case Opcode::LD_BYTE:   return "LD";
        case Opcode::ADD_BYTE:  return "ADD";
        case Opcode::LD_REG:    return "LD";
        case Opcode::OR:        return "OR";
        case Opcode::AND:       return "AND";
        case Opcode::XOR:       return "XOR";
        case Opcode::ADD_REG:   return "ADD";
        case Opcode::SUB:       return "SUB";
        case Opcode::SHR:       return "SHR";
        case Opcode::SUBN:      return "SUBN";
        case Opcode::SHL:       return "SHL";
        case Opcode::SE_REG:    return "SE";
        case Opcode::SNE_REG:   return "SNE";
        case Opcode::LD_ADDR:   return "LD";
        case Opcode::JP_V0:     return "JP";
        case Opcode::RND:       return "RND";
        case Opcode::DRW:       return "DRW";
        case Opcode::SKP:       return "SKP";
        case Opcode::SKNP:      return "SKNP";
        case Opcode::LD_VX_DT:  return "LD";
        case Opcode::LD_VX_K:   return "LD";
        case Opcode::LD_DT_VX:  return "LD";
        case Opcode::LD_ST_VX:  return "LD";
        case Opcode::ADD_I_VX:  return "ADD";
        case Opcode::LD_F_VX:   return "LD";
        case Opcode::LD_B_VX:   return "LD";
        case Opcode::LD_I_VX:   return "LD";
        case Opcode::LD_VX_I:   return "LD";
        case Opcode::UNKNOWN:   return "???";
    }
    return "???";
}
