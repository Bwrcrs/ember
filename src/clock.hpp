// ============================================================
//  Clock —— 让 700Hz 的 CPU 和 60Hz 的定时器并存
//
//  ★ 这个类最重要的设计是它**不知道墙钟时间**。
//
//    它只知道两条指令之间隔了多少"比例"，不知道真实世界过了多久。
//    真实时间是由调用方量出来、然后当作参数传进来的。
//
//    为什么这么设计？因为如果 Clock 自己去读系统时钟，
//    那么"跑 40 条指令后的状态"就变成了不可复现的东西——
//    取决于你机器那天有多忙。而测试要的是**确定性**。
//    把时间变成参数之后，同一段程序跑出来的结果永远一样，
//    黄金文件才能比对。
//
//  两条路径，共用同一个累加器：
//
//    advanceByMs(ms)      实时路径。窗口版每帧调用，传"这一帧过了几毫秒"。
//                         该跑几条就跑几条（小数进位保留，不漂移）。
//
//    advanceInstructions(n) 确定性路径。跑**正好 n 条**。
//                           trace / screen 走这条。
//
//  ★ 为什么定时器只在**指令边界**走？
//    因为如果允许"一条指令执行到一半时定时器变了"，
//    那么 Fx07（读延迟定时器）读到什么就取决于那条指令内部
//    正好处在什么位置——完全不可测。
//    钉在指令边界上之后，"跑 N 条指令后延迟定时器是几"是确定值。
//
//  ★ 默认 700/60 的比例是 11.667，不是整数。所以 tick 落在
//    第 12、24、35、47、59... 条指令之后（实测值，不是手算的）。
//    小数进位保留着，所以长期不漂移。
//    测试想要好算的比例就传 --ips 600 --hz 60，那是精确的 10:1，
//    tick 正好落在第 10、20、30... 条指令之后。
//
//  ★ 累加器是**整数**，这一点是承重的，不是风格问题。
//    见 clock.cpp 里 stepAndTick 的注释：用 double 累加 hz_/ips_
//    会让 0.1 累加十次得到 0.9999999999999999，够不到 1.0，
//    于是"精确 10:1"变成 11、21、31...——平均比例没错，
//    相位永久偏一条，而黄金文件钉的正是相位。
//    整数累加还有个好处：tick 位置不依赖编译器的浮点舍入，
//    黄金文件在任何符合标准的实现上都一样。
// ============================================================

#pragma once

#include <cstdint>

class Machine;

class Clock {
public:
    // 700 指令/秒是社区公认的数值，也是让游戏手感对的那个数。
    // 60Hz 是屏幕刷新率，从 1977 年到现在没变过。
    static constexpr int DEFAULT_IPS = 700;
    static constexpr int DEFAULT_TIMER_HZ = 60;

    // 一帧最多补多少毫秒。见下面 advanceByMs 的说明。
    static constexpr double MAX_CATCHUP_MS = 100.0;

    Clock() = default;

    explicit Clock(int instructionsPerSecond, int timerHz = DEFAULT_TIMER_HZ);

    // ---- 实时路径 ----
    // 跑掉"这么多毫秒该跑的指令数"，小数部分留到下次。
    // 返回这次实际执行的指令条数。
    //
    // ★ elapsedMs 会被**钳制**在 MAX_CATCHUP_MS 以内。
    //   不钳的话：拖动窗口十秒钟，elapsedMs 可能就是 10000，
    //   于是要一次性补 7000 条指令——窗口在这期间不响应任何消息，
    //   看起来就是"卡死了"。用户松开鼠标之后它"追上了"，
    //   但这个过程会反复发生，体验极差。这叫死亡螺旋。
    //   钳制之后，落后的时间被**丢掉**而不是补回来。
    //   对模拟器来说这是对的：模拟器要的是"现在看起来对"，
    //   不是"把过去十秒的帧全部补播一遍"。
    long long advanceByMs(Machine& machine, double elapsedMs);

    // ---- 确定性路径 ----
    // 跑正好 count 条指令，中途该 tick 定时器就 tick。
    // 测试走这条，所以同一条命令的输出永远一样。
    void advanceInstructions(Machine& machine, long long count);

    int instructionsPerSecond() const { return static_cast<int>(ips_); }
    int timerHz() const { return static_cast<int>(hz_); }
    long long totalInstructions() const { return total_; }

    // 跑**一条**指令，并按比例决定要不要 tick 定时器。
    // 返回 true 表示这一步之后正好发生了一次 60Hz 滴答。
    //
    // 这个函数是公开的，因为追踪器要按步调用它、好把
    // "-- TICK 60Hz --" 这样的分隔行印在**正确的位置**上。
    // runSteps 内部也用它，所以"什么时候该 tick"只有一份实现——
    // 如果追踪器自己再算一遍比例，两处的取整误差迟早会让
    // 追踪输出和实际执行错开一条指令，那种 bug 极难查。
    bool stepAndTick(Machine& machine);

private:
    // 两条路径最终都汇到这里。
    void runSteps(Machine& machine, long long count);

    // 指令数和定时器频率都是**整数**。
    // 它们来自命令行（--ips / --hz），本来就是整数；
    // 而且 tick 的判定要精确到"正好第几条指令"，
    // 用整数才有那个精确度。见上面那段说明。
    long long ips_ = DEFAULT_IPS;
    long long hz_ = DEFAULT_TIMER_HZ;

    // 欠这个机器几条指令。小数部分留在这里，所以不漂移。
    // 这一路**必须**是小数：它是从真实的毫秒数换算来的。
    double instructionCredit_ = 0.0;

    // 距离下一次 tick 还差多少。单位是"千分之一个 ips_"，
    // 或者说：攒够 ips_ 就是一次 tick。
    //
    // ★ 整数。理由见上面那段，以及 clock.cpp 里 stepAndTick 的注释。
    long long timerCredit_ = 0;

    long long total_ = 0;
};
