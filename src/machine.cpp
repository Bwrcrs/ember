// ============================================================
//  Machine —— 生命周期、取指、定时器
//  见 machine.hpp 的说明（尤其是那两条契约）。
//
//  这个文件负责"一台机器怎么开始运转"和"每一步的骨架"；
//  每条指令具体干什么在 cpu.cpp 里。
// ============================================================

#include "machine.hpp"

#include "font.hpp"

Machine::Machine(Quirks quirks, std::uint32_t seed)
    : rng_(seed), seed_(seed), quirks_(quirks) {
    // 构造函数只管把随机数种子和行为开关存好，
    // 真正的"开机"在 reset() 里。这样"开机"这个动作只有一个实现。
    reset();
}

void Machine::reset() {
    memory_.fill(0);
    v_.fill(0);
    i_ = 0;
    pc_ = ROM_BASE;
    stack_.fill(0);
    sp_ = 0;
    delayTimer_ = 0;
    soundTimer_ = 0;
    keypad_.fill(false);
    display_.clear();
    cycle_ = 0;
    drewThisFrame_ = false;
    pendingKey_ = -1;

    // ★ 随机数发生器重新播种。
    // 不重播的话，跑第二遍同一份 ROM 会得到不同的 Cxnn 结果，
    // 于是"同一个程序两次运行行为不同"——测试的黄金文件全废。
    rng_.seed(seed_);

    // 装上字模。放在 reset 而不是 loadRom，因为
    // ① 字模和 ROM 互不相干（一个在 0x000/0x050，一个在 0x200，不会撞）
    // ② reset 之后必须能再跑一次，字模得跟着回来
    const std::uint16_t base = quirks_.fontBase();
    for (int k = 0; k < FONT_TOTAL_BYTES; ++k) {
        memory_[static_cast<std::size_t>(base) + k] = FONT[static_cast<std::size_t>(k)];
    }

    lastStep_ = StepInfo{};
}

void Machine::loadRom(const Rom& rom) {
    const std::vector<std::uint8_t>& bytes = rom.bytes();

    for (std::size_t k = 0; k < bytes.size(); ++k) {
        memory_[ROM_BASE + k] = bytes[k];
    }

    // 清掉这一步的记录，免得追踪器读到上一个程序的残留。
    lastStep_ = StepInfo{};
}

void Machine::poke(std::uint16_t address, std::uint8_t value) {
    memory_[address & 0x0FFF] = value;
}

StepStatus Machine::step() {
    // ---- 1. 取指 ----
    // ★ 顺序很重要：先把 PC 和原始字节存进 lastStep_，
    //   因为接下来这条指令可能改掉自己的字节（自修改代码），
    //   也可能改掉 PC。执行完再读就读不到"当时"的样子了。
    lastStep_.pc = pc_;
    lastStep_.opcode = peekOpcode();
    ++cycle_;
    lastStep_.cycle = cycle_;

    // ---- 2. 先推进 PC，再分发 ----
    // 见 machine.hpp【契约二】：这样"忘了推进 PC"就不可能发生。
    // 任何改变控制流的指令在这一步之后显式重写 pc_。
    advancePc(2);

    // ---- 3. 译码并执行 ----
    const Decoded d = decode(lastStep_.opcode);
    StepStatus status = executeOpcode(d);

    // ---- 4. 阻塞的指令把 PC 退回去 ----
    // Fx0A 没等到键、Dxyn 撞上 displayWait，这两种情况都表示
    // "这条指令还没真正完成，下一轮重新执行它"。
    // 周期已经烧掉了（见【契约一】），这是有意的。
    if (status == StepStatus::WAIT_KEY ||
        status == StepStatus::WAIT_DISPLAY) {
        pc_ = lastStep_.pc;
    }

    lastStep_.status = status;
    return status;
}

void Machine::tickTimers() {
    // ★ 必须先判断 > 0 再减。
    // 直接 --delayTimer_ 的话，0 会绕回 255，定时器就**永生**了：
    // 延迟永远走不完，声音一直响，而且看起来像是"定时器没生效"。
    // 这种 bug 会让人去怀疑时钟，而问题其实在这一行。
    if (delayTimer_ > 0) {
        --delayTimer_;
    }
    if (soundTimer_ > 0) {
        --soundTimer_;
    }

    // 一帧结束了，displayWait 放行一次。
    drewThisFrame_ = false;
}

void Machine::push(std::uint16_t address) {
    stack_[sp_] = address;
    // 掩码到 4 位。栈只有 16 层，sp_ 越过 15 时会绕回 0
    // ——会覆盖掉最早压的那一层，这是错的，但**确定性地错**。
    // 不掩的话 sp_ 会变成 16，下次写栈就越界，
    // 那是未定义行为，可能崩、可能不崩，反而更难查。
    sp_ = static_cast<std::uint8_t>((sp_ + 1) & 0x0F);
}

std::uint16_t Machine::pop() {
    sp_ = static_cast<std::uint8_t>((sp_ - 1) & 0x0F);
    return stack_[sp_];
}

int Machine::lowestKeyDown() const {
    for (int key = 0; key < KEY_COUNT; ++key) {
        if (keypad_[static_cast<std::size_t>(key)]) {
            return key;
        }
    }
    return -1;
}
