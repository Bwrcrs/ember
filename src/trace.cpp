// ============================================================
//  Trace —— 见 trace.hpp
//
//  这个文件只读机器状态（除了那一句推进机器的 clock.stepAndTick），
//  不改任何东西。追踪器一旦会改状态，它观察到的就不是
//  程序本来的行为了——那是"海森堡 bug"，最难查的一类。
// ============================================================

#include "trace.hpp"

#include "disassembler.hpp"
#include "hexfmt.hpp"

namespace {

// 三种非正常状态的后缀标记。
// 用 ASCII 而不是中文：追踪输出要拿去做 diff，
// 纯 ASCII 的对齐和可比性都更好。
const char* WAIT_KEY_TAG = "[wait-key]";
const char* WAIT_DISPLAY_TAG = "[wait-display]";
const char* UNKNOWN_TAG = "[unknown]";

// 反汇编列补到这么宽。20 列够放最长的
// "LD [I], VF"（10 字符）和 "DRW V1, V2, 15"（14 字符）。
constexpr std::size_t DISASM_WIDTH = 20;

}  // namespace

void Tracer::snapshot(const Machine& machine) {
    for (int k = 0; k < Machine::REGISTER_COUNT; ++k) {
        vBefore_[static_cast<std::size_t>(k)] = machine.v(k);
    }
    iBefore_ = machine.i();
    dtBefore_ = machine.delayTimer();
    stBefore_ = machine.soundTimer();
}

std::string Tracer::formatDeltas(const Machine& machine) const {
    std::string out;

    // 寄存器：只印变了的。
    // 顺序固定从 V0 到 VF，不按"变化先后"——固定的顺序
    // 才能让黄金文件可比对。
    for (int k = 0; k < Machine::REGISTER_COUNT; ++k) {
        const std::uint8_t now = machine.v(k);
        if (now == vBefore_[static_cast<std::size_t>(k)]) {
            continue;
        }
        if (!out.empty()) {
            out += ' ';
        }
        out += registerName(k) + "=" + hex2bare(now);
    }

    // I 寄存器。
    if (machine.i() != iBefore_) {
        if (!out.empty()) {
            out += ' ';
        }
        out += "I=" + hex3(machine.i()).substr(2);  // 去掉 0x，保持列宽紧凑
    }

    // 两个定时器。它们只在 TICK 时变，所以这里出现变化
    // 通常紧跟在 TICK 行后面，能对上。
    if (machine.delayTimer() != dtBefore_) {
        if (!out.empty()) {
            out += ' ';
        }
        out += "DT=" + hex2bare(machine.delayTimer());
    }
    if (machine.soundTimer() != stBefore_) {
        if (!out.empty()) {
            out += ' ';
        }
        out += "ST=" + hex2bare(machine.soundTimer());
    }

    return out;
}

std::string Tracer::step(Machine& machine, Clock& clock) {
    // ---- 1. 执行前先记下状态 ----
    // 必须在 step 之前。执行完再记的话，读到的就是"结果"，
    // 而结果和结果相减永远是零，deltas 永远是空的——
    // 而且这个 bug 不会报错，只会让追踪输出看起来"很干净"。
    snapshot(machine);

    // ---- 2. 取指前把原始字节存下来 ----
    // 要在 step 之前读。因为一条指令可以改掉自己的字节
    // （自修改代码），step 之后读到的可能是被改过的新值。
    const std::uint16_t pc = machine.pc();
    const std::uint16_t opcode = machine.peekOpcode();

    // ---- 3. 执行 ----
    // 走 Clock 而不是 Machine::step，理由见 trace.hpp 文件头：
    // 定时器的滴答时机只能有一个实现。
    ticked_ = clock.stepAndTick(machine);
    const StepStatus status = machine.lastStep().status;

    // ---- 4. 拼行 ----
    std::string line;
    line += padLeft(dec(machine.cycle()), 6);
    line += "  ";
    line += hex3(pc);
    line += "  ";
    line += hex4(opcode);
    line += "  ";
    line += padRight(disassemble(opcode, machine.quirks()), DISASM_WIDTH);

    if (showDeltas_) {
        line += formatDeltas(machine);
    }

    // 后缀标记去掉行尾多余空格，让黄金文件干净些。
    const char* tag = nullptr;
    switch (status) {
        case StepStatus::WAIT_KEY:      tag = WAIT_KEY_TAG;     break;
        case StepStatus::WAIT_DISPLAY:  tag = WAIT_DISPLAY_TAG; break;
        case StepStatus::UNKNOWN_OPCODE: tag = UNKNOWN_TAG;     break;
        case StepStatus::OK:            tag = nullptr;          break;
    }

    if (tag != nullptr) {
        // 先把行尾空格削掉再贴标记，否则标记前面会有一串
        // 由 padRight 留下的空格，看着像排版坏了。
        while (!line.empty() && line.back() == ' ') {
            line.pop_back();
        }
        line += "  ";
        line += tag;
    } else {
        while (!line.empty() && line.back() == ' ') {
            line.pop_back();
        }
    }

    return line;
}
