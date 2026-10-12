// ============================================================
//  Clock —— 见 clock.hpp
//
//  提醒：这个文件里**没有任何**计时调用。
//  没有 std::chrono、没有 timeGetTime、没有 QueryPerformanceCounter。
//  时间是从参数进来的。这是刻意的，也是测试能复现的前提。
//  量时间的地方在 platform_win32.cpp，那里才是唯一读钟的地方。
// ============================================================

#include "clock.hpp"

#include "error.hpp"
#include "machine.hpp"

Clock::Clock(int instructionsPerSecond, int timerHz) {
    if (instructionsPerSecond <= 0) {
        throw EmberError("每秒指令数必须是正数，给的是 " +
                         std::to_string(instructionsPerSecond));
    }
    if (timerHz <= 0) {
        throw EmberError("定时器频率必须是正数，给的是 " +
                         std::to_string(timerHz));
    }

    ips_ = instructionsPerSecond;
    hz_ = timerHz;
}

bool Clock::stepAndTick(Machine& machine) {
    machine.step();
    ++total_;

    // 每跑一条指令，就给定时器记上 hz 点"欠账"；
    // 攒够 ips 点就是整整一秒的定时器时间，该 tick 了。
    //
    // ★ 这里是**整数**累加，不是 double 累加 hz_/ips_ 再和 1.0 比。
    //
    //   原来写的是 double，700/60 上看不出问题（tick 落在第
    //   12、24、35... 条之后），但在 60/600 这个"精确 10:1"上露馅：
    //   0.1 在二进制里是无限循环小数，累加十次得到的是
    //   0.9999999999999999，够不到 1.0，于是第一次 tick 拖到第 11
    //   条指令之后，之后一路错位：11、21、31、41...
    //   长期**平均**比例仍然对（每 10 条一次），但**相位**永久
    //   偏了一条指令——而黄金文件要钉住的正是相位。
    //   表现是 --steps 40 只 tick 了 3 次而不是 4 次，
    //   而 clock.hpp 里白纸黑字写着 600/60 是精确的 10:1。
    //
    //   换成整数之后 60/600 恰好十次攒够，tick 落在第 10、20、30...
    //   条之后；而且 tick 位置不再取决于编译器的浮点舍入，
    //   黄金文件在任何符合标准的实现上都一样。
    timerCredit_ += hz_;
    bool ticked = false;

    // 用 while 而不是 if：比例大于 1 的时候
    // （比如 --hz 120 --ips 60）一次要 tick 好几下。
    while (timerCredit_ >= ips_) {
        machine.tickTimers();
        timerCredit_ -= ips_;
        ticked = true;
    }

    return ticked;
}

void Clock::runSteps(Machine& machine, long long count) {
    for (long long k = 0; k < count; ++k) {
        stepAndTick(machine);
    }
}

void Clock::advanceInstructions(Machine& machine, long long count) {
    if (count <= 0) {
        return;
    }
    runSteps(machine, count);
}

long long Clock::advanceByMs(Machine& machine, double elapsedMs) {
    // 先把明显不合理的值挡掉。
    // 负数（时钟回拨、量错）直接当 0。
    if (!(elapsedMs > 0.0)) {
        return 0;
    }

    // 见 clock.hpp 里那段关于死亡螺旋的说明。
    if (elapsedMs > MAX_CATCHUP_MS) {
        elapsedMs = MAX_CATCHUP_MS;
    }

    // 这一帧该跑多少条 = 毫秒数 / 1000 * 每秒指令数。
    // 加上上次剩下的零头，然后取整。
    //
    // ★ 这一路**必须**是 double：elapsedMs 是量出来的真实时间，
    //   本来就是小数。见 clock.hpp 关于"欠着的零头不能丢"的说明。
    //   只有 tick 累加器需要精确，所以那边是整数、这边是小数。
    instructionCredit_ += elapsedMs / 1000.0 * static_cast<double>(ips_);

    // ★ 用 floor 取整，不是四舍五入。
    //   因为"欠着"的零头留在 instructionCredit_ 里，
    //   下一帧会补上。四舍五入的话，0.6 会被算成 1 条，
    //   但同时零头也留着，等于多跑了——速度会偏快。
    const long long due = static_cast<long long>(instructionCredit_);
    if (due <= 0) {
        // 这一帧连一条指令都不够。
        // 【注意】这里**不能**清掉 instructionCredit_，
        //   否则高速刷新（每帧 1ms）时 0.7 永远攒不到 1，
        //   模拟器就彻底不动了——这是"帧率越高跑得越慢"的经典 bug。
        return 0;
    }

    instructionCredit_ -= static_cast<double>(due);
    runSteps(machine, due);
    return due;
}
