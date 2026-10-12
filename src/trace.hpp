// ============================================================
//  Trace —— 每执行一条指令，印一行
//
//  ★ 这是整个项目里最有用的调试工具，理由比"好比对"更锋利：
//
//    黄金文件会在**行为开始出错的那一条指令**上分叉，
//    而且那一行的反汇编列就写着"这条指令本来该干什么"。
//    屏幕转储只能告诉你"上游某个地方错了"，然后你就得
//    开始二分查找。追踪输出直接把出错的那一条指给你。
//
//  输出格式（定宽，因为要当黄金文件比对）：
//
//      cycle          pc      opcode   disassembly          deltas
//      ─────────────────────────────────────────────────────────────
//        112        0x208    0x6012   LD V0, 0x12          V0=12
//        113        0x20a    0x6105   LD V1, 0x05          V1=05
//        114        0x20c    0x8014   ADD V0, V1           V0=17 VF=0
//      -- TICK 60Hz --
//
//  规则：
//    · deltas 只印**变了**的寄存器，没变的不印，这样一行才看得完
//    · PC 不印在 deltas 里——下一行的 pc 列就写着它跳到哪了，
//      重复一遍只是噪音
//    · 阻塞的指令带一个后缀标记（见下面 STATUS 那几个常量），
//      否则"同一条指令连着出现 12 次"看起来像是死循环
//    · TICK 行单独一行，位置精确——定时器只在指令边界走，
//      所以"第几条指令之后 tick"是确定值，可以钉死
//
//  为了让 TICK 行印在对的位置，追踪器**不自己调 Machine::step**，
//  而是调 Clock::stepAndTick——这样"什么时候该 tick"和实际执行
//  用的是同一个实现，不会两处各自取整导致 TICK 行错开一条。
//  见 clock.hpp 里的说明。
// ============================================================

#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "clock.hpp"
#include "machine.hpp"
#include "quirks.hpp"

class Tracer {
public:
    // showDeltas 关掉时只印 cycle / pc / opcode / 反汇编，
    // 适合只想看控制流的场合。默认开，因为寄存器变化才是
    // 一半以上的 bug 所在。
    explicit Tracer(bool showDeltas = true) : showDeltas_(showDeltas) {}

    // 执行一步并返回**一行**输出（不带换行符）。
    //
    // 【注意】这个方法内部会真的推进机器。命名成 step 而不是
    // "格式化某一步"，就是为了让"调它一次 = 机器走一步"这件事
    // 在调用处一眼可见——追踪器如果把"观察"和"推进"分开，
    // 迟早会有人调错顺序，得到一份和实际执行不符的追踪。
    //
    // 【为什么要 Clock 而不是直接 Machine】见文件头。
    // 一句话：TICK 的位置必须由唯一那个计时实现说了算。
    std::string step(Machine& machine, Clock& clock);

    // 刚才那一步是否触发了一次定时器滴答。
    //
    // 做成"查一次"而不是"step 返回两行"，是为了保住
    // "一次 step = 一行"这个契约——调用方拿到行之后先打印，
    // 再问一句"要不要补一行 TICK"，顺序天然是对的。
    bool ticked() const { return ticked_; }

    // 定时器滴答时单独一行。由驱动方在 step 返回后按需调用。
    static std::string tickLine() { return "-- TICK 60Hz --"; }

private:
    void snapshot(const Machine& machine);
    std::string formatDeltas(const Machine& machine) const;

    bool showDeltas_;
    bool ticked_ = false;

    // 执行前的状态，用来算差量。
    std::array<std::uint8_t, Machine::REGISTER_COUNT> vBefore_{};
    std::uint16_t iBefore_ = 0;
    std::uint8_t dtBefore_ = 0;
    std::uint8_t stBefore_ = 0;
};
