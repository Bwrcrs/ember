// ============================================================
//  Machine —— 一台（假的）CHIP-8 电脑
//
//  这里装着"一台机器"的全部状态。不多不少就是这些：
//
//      4KB 内存          memory_
//      16 个 8 位寄存器  v_      （V0..VF，VF 兼作标志位）
//      1 个 12 位地址寄存器 i_   （叫 I，专门用来指内存）
//      1 个程序计数器     pc_
//      一个 16 层栈      stack_ / sp_  （16 位一层的返回地址）
//      两个 8 位定时器    delayTimer_ / soundTimer_
//      16 个按键的当前状态 keypad_
//      一块 64x32 的屏幕  display_
//      一个随机数发生器   rng_
//      一组行为开关       quirks_
//
//  真实的 COSMAC VIP 一共就 2KB 内存、没有屏幕控制器，
//  屏幕是靠 CPU 一个像素一个像素往显存里写的。这里抽象成 Display
//  是因为"画一个点"和"画一个精灵"在概念上是两件事。
//
//  ★★ 两条最重要的契约，写在最前面 ★★
//
//  【契约一】step() 一次执行**一条**指令，并且**永远消耗一个周期**，
//            阻塞时也一样。
//
//    为什么阻塞也要消耗周期？因为定时器必须在 Fx0A 等按键期间照样走。
//    如果等待时周期停了，那么一个"等待按键时背景音乐停止"的 bug
//    就会变成"等按键时延迟定时器冻住"，而后者更难发现。
//    这是真实行为要求，不是偷懒。
//
//  【契约二】pc_ += 2 在**分发之前**就做完，任何改变控制流的指令
//            显式重写 pc_。
//
//    这一条让"某个分支忘了推进 PC"在**结构上不可能发生**。
//    那是手写模拟器最常见的一类 bug：表现出来是程序在某条指令上
//    无限循环，而你会以为是跳转指令写错了，实际上是一条普通的
//    加法指令忘了前进。把它变成不可能，比事后调试它便宜得多。
//
//  ★★ 这个文件**不碰任何 Win32** ★★
//    内核必须能在没有窗口的情况下被驱动，否则测试就只能靠人眼看屏幕。
//    这是 150 项自动化测试能存在的前提，由 build.bat 的 lint 机械保证。
// ============================================================

#pragma once

#include <array>
#include <cstdint>
#include <random>
#include <vector>

#include "display.hpp"
#include "opcodes.hpp"
#include "quirks.hpp"
#include "rom.hpp"

// step() 执行完之后，这一步是什么结果。
enum class StepStatus {
    OK,              // 正常执行，PC 已经指向下一条（或跳转指令重写过了）
    WAIT_KEY,        // Fx0A 但当前没有键按着：PC 退回，等下一次
    WAIT_DISPLAY,    // Dxyn 撞上了 displayWait 开关：这一帧已经画过了
    UNKNOWN_OPCODE   // 不是合法指令：当空操作，但**照样消耗一个周期**
};

// 刚执行的这一步的全部信息，给追踪器读。
struct StepInfo {
    std::uint64_t cycle = 0;
    std::uint16_t pc = 0;       // 这条指令是从哪儿取的
    std::uint16_t opcode = 0;   // 取指那一刻的原始字节
    StepStatus status = StepStatus::OK;
};

class Machine {
public:
    // 从 rom.hpp 转发过来，这样外界只需要认识 Machine 一个名字。
    //
    // 【注意】这里必须写 :: 前缀。因为右边那个名字和左边要声明的成员
    // 同名，不写 :: 的话编译器会把右边的 MEMORY_SIZE 解析成
    // "正在声明的那个成员自己"，于是报一个很费解的自引用错误。
    // 加 :: 明确指向全局作用域，两不相干。
    static constexpr std::uint16_t MEMORY_SIZE = ::MEMORY_SIZE;
    static constexpr std::uint16_t ROM_BASE = ::ROM_BASE;
    static constexpr std::size_t MAX_ROM_SIZE = ::MAX_ROM_SIZE;

    static constexpr int STACK_DEPTH = 16;
    static constexpr int REGISTER_COUNT = 16;
    static constexpr int KEY_COUNT = 16;

    // quirks 默认是现代约定（能跑通现在流通的游戏，见 quirks.hpp）。
    // seed 默认固定，这样 Cxnn 随机指令的输出可复现——
    // 测试的黄金文件依赖这一点。想要真随机就传 0 当种子走时钟，
    // 但那样 trace 就没法比对了，所以默认必须是固定的。
    explicit Machine(Quirks quirks = Quirks::modern(),
                     std::uint32_t seed = 0x00C0FFEEu);

    // ---- 生命周期 ----

    // 清空一切，重新装上字模，PC 回到 0x200。
    // 【注意】字模是在**这里**装的，不是构造函数的 loadRom 里。
    // 因为 reset 之后必须能再跑一次，字模得跟着回来。
    void reset();

    // 把 ROM 装到 0x200。不动寄存器，不清屏。
    // 这样测试可以"先装 ROM 再 poke 内存"，顺序自由。
    void loadRom(const Rom& rom);

    // 在任意地址塞一个字节。给测试和 --poke 参数用。
    void poke(std::uint16_t address, std::uint8_t value);

    // ---- 执行 ----

    // 执行一条指令，消耗一个周期。见文件头【契约一】【契约二】。
    // 这个函数**不抛异常**——跑飞的程序读到垃圾指令是常态，不是异常。
    StepStatus step();

    const StepInfo& lastStep() const { return lastStep_; }

    // 60Hz 的一次滴答：两个定时器各减一。
    // 由外面的 Clock 按 700:60 的比例调用，内核自己不知道墙钟时间。
    void tickTimers();

    // 声音定时器还在走吗？平台层拿这个决定要不要响。
    bool soundActive() const { return soundTimer_ > 0; }

    // ---- 读状态（调试器、追踪器、测试用）----

    std::uint8_t v(int index) const { return v_[static_cast<std::size_t>(index)]; }
    std::uint16_t i() const { return i_; }
    std::uint16_t pc() const { return pc_; }
    std::uint8_t sp() const { return sp_; }
    std::uint8_t delayTimer() const { return delayTimer_; }
    std::uint8_t soundTimer() const { return soundTimer_; }
    std::uint16_t stackAt(int index) const {
        return stack_[static_cast<std::size_t>(index)];
    }
    std::uint8_t memory(std::uint16_t address) const {
        return memory_[address & 0x0FFF];
    }
    bool keyDown(int key) const {
        return keypad_[static_cast<std::size_t>(key)];
    }
    const Display& display() const { return display_; }
    const Quirks& quirks() const { return quirks_; }
    std::uint64_t cycle() const { return cycle_; }

    // 取出 PC 处那条指令的原始 16 位。
    //
    // ★ 为什么要有这个函数（而不是从 lastStep 里读）：
    //   因为**自修改代码**。一条指令可以把下一条指令的字节改掉，
    //   而追踪器需要在执行**之前**拿到"当时"的字节。
    //   如果等 step() 完再从内存里读，读到的可能是被自己改过的新字节。
    //   取指顺序：先 peekOpcode() 存档，再 step()。
    std::uint16_t peekOpcode() const {
        return static_cast<std::uint16_t>(
            (memory_[pc_] << 8) | memory_[(pc_ + 1) & 0x0FFF]);
    }

    // ---- 输入 ----

    void setKey(int key, bool down) {
        keypad_[static_cast<std::size_t>(key)] = down;
    }
    void clearKeys() { keypad_.fill(false); }

private:
    // ---- 执行一条已经译好码的指令 ----
    //
    // 【设计取舍】为什么 cpu.cpp 没有对应的 cpu.hpp？
    // 因为这三十几个处理函数是 Machine 的**私有成员**：既能直接改
    // 寄存器文件，又不用对外开放 setV/setI 这类写入接口。
    // 如果做成"公开的 setter + 自由函数"，等于把"谁都能弄坏寄存器状态"
    // 这个能力散给所有调用方，只为了省下这几十行声明。
    // 而且下面这一段 private 声明，本身就是一份完整的指令集索引——
    // 想知道"这台机器实现了哪些指令"，看这里就够了。
    StepStatus executeOpcode(const Decoded& d);

    // 0x0 开头
    void opCls();
    void opRet();

    // 跳转和调用
    void opJp(const Decoded& d);
    void opCall(const Decoded& d);

    // 条件跳过。这些函数返回"是否跳过"，由调用方统一推进 PC——
    // 这样"跳过"只有一处实现，不会每条指令各写一遍 pc_ += 2。
    bool testSeByte(const Decoded& d) const;
    bool testSneByte(const Decoded& d) const;
    bool testSeReg(const Decoded& d) const;
    bool testSneReg(const Decoded& d) const;
    bool testSkp(const Decoded& d) const;
    bool testSknp(const Decoded& d) const;

    // 赋值和运算
    void opLdByte(const Decoded& d);
    void opAddByte(const Decoded& d);
    void opLdReg(const Decoded& d);
    void opOr(const Decoded& d);
    void opAnd(const Decoded& d);
    void opXor(const Decoded& d);
    void opAddReg(const Decoded& d);
    void opSub(const Decoded& d);
    void opShr(const Decoded& d);
    void opSubn(const Decoded& d);
    void opShl(const Decoded& d);

    // 内存和随机数
    void opLdAddr(const Decoded& d);
    void opJpV0(const Decoded& d);
    void opRnd(const Decoded& d);
    // Dxyn 是唯一一条可能"这次不执行"的普通指令（displayWait），
    // 所以它要返回状态而不是 void。
    StepStatus opDrw(const Decoded& d);

    // 定时器
    void opLdVxDt(const Decoded& d);
    StepStatus opLdVxK(const Decoded& d);
    void opLdDtVx(const Decoded& d);
    void opLdStVx(const Decoded& d);

    // I 寄存器相关
    void opAddIVx(const Decoded& d);
    void opLdFVx(const Decoded& d);
    void opLdBVx(const Decoded& d);
    void opLdIVx(const Decoded& d);
    void opLdVxI(const Decoded& d);

    // ---- 小工具 ----

    // PC 前进 amount 字节，并且**永远掩码到 12 位**。
    // 在 0xFFE 处跳过时 PC 会变成 0x1000，掩码之后回到 0x000。
    // 不掩的话就越界了——读的是数组外面的内存，静默且不一定崩。
    void advancePc(std::uint16_t amount) {
        pc_ = static_cast<std::uint16_t>((pc_ + amount) & 0x0FFF);
    }

    void push(std::uint16_t address);
    std::uint16_t pop();

    // 按下着的键里**编号最小**的那个。没有键按下时返回 -1。
    //
    // 【一个必须明确的行为】同时按住多个键时，Fx0A 返回哪个？
    // 文档没说。这里定成"编号最小的那个"，并且写死成唯一定义。
    // 不定死的话，测试就没法写出确定的期望值。
    int lowestKeyDown() const;

    // ---- 状态 ----
    std::array<std::uint8_t, MEMORY_SIZE> memory_{};
    std::array<std::uint8_t, REGISTER_COUNT> v_{};
    std::uint16_t i_ = 0;
    std::uint16_t pc_ = ROM_BASE;
    std::array<std::uint16_t, STACK_DEPTH> stack_{};
    std::uint8_t sp_ = 0;
    std::uint8_t delayTimer_ = 0;
    std::uint8_t soundTimer_ = 0;
    std::array<bool, KEY_COUNT> keypad_{};
    Display display_;
    std::mt19937 rng_;

    // 记住种子，因为 reset() 要重新播一次。
    // 不重播的话，同一个程序跑两遍会得到不同的 Cxnn 结果——
    // 那样 trace 的黄金文件就永远对不上。
    std::uint32_t seed_ = 0;

    Quirks quirks_;
    std::uint64_t cycle_ = 0;
    StepInfo lastStep_;

    // displayWait 用：这一帧画过了吗？
    // 由 tickTimers 清除，也就是"每 1/60 秒放行一次"。
    bool drewThisFrame_ = false;

    // fx0aWaitsForRelease 用：已经按下、正在等松开的那个键。
    // -1 表示"还没按到任何键"。
    // 【注意】只在这个 quirk 打开时才会被用到，
    // 但它必须是一个成员变量而不是局部变量——因为"等松开"这件事
    // 要跨好几次 step() 调用才能完成，状态得有地方存。
    int pendingKey_ = -1;
};
