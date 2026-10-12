// ============================================================
//  Cpu —— 35 条指令的实现
//
//  这是**唯一**会改动虚拟机状态的文件。machine.cpp 管"怎么开始运转"
//  和"每一步的骨架"，这里管"每条指令具体干什么"。
//
//  这个文件没有对应的 cpu.hpp，因为这些处理函数都是 Machine 的私有成员
//  （见 machine.hpp 里的说明）。
//
//  ★★ 贯穿整个文件的顺序规则 ★★
//
//  凡是"结果写进 Vx，同时标志位写进 VF"的指令，
//  当 x == 0xF 时两者指向**同一个寄存器**。谁后写谁赢。
//  规则是：**标志位后写**，因为硬件上标志是运算的副产物，
//  而 Vx 是主结果——VF 必须反映这次运算，不能被主结果盖掉。
//
//  正确写法永远是三步：
//      ① 把要用的操作数**全部读进局部变量**
//      ② 算
//      ③ 先写 Vx，最后写 VF
//
//  顺序错的典型症状：8xy4 用 VF 当 Vx 时（x = 0xF）结果全错，
//  而 4-flags 那个测试 ROM 就是专门抓这个的。
//
//  ★★ 第二条贯穿全文件的规则：所有地址都要掩码 ★★
//
//  I 是 12 位，但 Fx33 会写 I+2、Fx55 会碰 I+15、Dxyn 会读 I+14。
//  I = 0xFF8 时这些都越过 4096 字节的数组边界——**未定义行为**，
//  静默，而且不一定崩（数组后面可能是别的成员变量，你改坏的是自己）。
//  所以每一处内存访问都写 (i_ + k) & 0x0FFF。
// ============================================================

#include "font.hpp"
#include "machine.hpp"

// VF 的下标。它是第 16 个寄存器，兼作标志位。
// 单独起个名字是因为这个文件里它出现得太频繁，
// 而 0x0F 混在移位掩码、半字节常量里很容易看错。
constexpr int VF_INDEX = 0x0F;

StepStatus Machine::executeOpcode(const Decoded& d) {
    // 【注意】这里**故意不写 default:**。
    // 这样将来往 Opcode 里加了新指令却忘了在这里实现，
    // -Wswitch 会直接点名是哪一个。
    // 代价是编译器会抱怨"可能走到函数末尾"，所以最后有一句兜底 return。
    switch (d.op) {
        // ---- 0x0 开头 ----
        case Opcode::SYS:
            // 0nnn 是"调用机器码子程序"，现代模拟器一律忽略。
            // 什么都不做，但照样消耗一个周期（step 已经加过了）。
            return StepStatus::OK;

        case Opcode::CLS:
            opCls();
            return StepStatus::OK;

        case Opcode::RET:
            opRet();
            return StepStatus::OK;

        // ---- 跳转和调用 ----
        case Opcode::JP:
            opJp(d);
            return StepStatus::OK;

        case Opcode::CALL:
            opCall(d);
            return StepStatus::OK;

        // ---- 条件跳过 ----
        // 六个跳过类指令形状完全一样，所以统一在这里处理：
        // 各个 testXxx 只回答"跳不跳"，推进 PC 只有这一处实现。
        // 这样不会出现"某条指令的跳过写成了 pc_ += 4"这种错。
        case Opcode::SE_BYTE:
            if (testSeByte(d)) { advancePc(2); }
            return StepStatus::OK;
        case Opcode::SNE_BYTE:
            if (testSneByte(d)) { advancePc(2); }
            return StepStatus::OK;
        case Opcode::SE_REG:
            if (testSeReg(d)) { advancePc(2); }
            return StepStatus::OK;
        case Opcode::SNE_REG:
            if (testSneReg(d)) { advancePc(2); }
            return StepStatus::OK;
        case Opcode::SKP:
            if (testSkp(d)) { advancePc(2); }
            return StepStatus::OK;
        case Opcode::SKNP:
            if (testSknp(d)) { advancePc(2); }
            return StepStatus::OK;

        // ---- 赋值和运算 ----
        case Opcode::LD_BYTE:  opLdByte(d);  return StepStatus::OK;
        case Opcode::ADD_BYTE: opAddByte(d); return StepStatus::OK;
        case Opcode::LD_REG:   opLdReg(d);   return StepStatus::OK;
        case Opcode::OR:       opOr(d);      return StepStatus::OK;
        case Opcode::AND:      opAnd(d);     return StepStatus::OK;
        case Opcode::XOR:      opXor(d);     return StepStatus::OK;
        case Opcode::ADD_REG:  opAddReg(d);  return StepStatus::OK;
        case Opcode::SUB:      opSub(d);     return StepStatus::OK;
        case Opcode::SHR:      opShr(d);     return StepStatus::OK;
        case Opcode::SUBN:     opSubn(d);    return StepStatus::OK;
        case Opcode::SHL:      opShl(d);     return StepStatus::OK;

        // ---- 内存和随机数 ----
        case Opcode::LD_ADDR: opLdAddr(d); return StepStatus::OK;
        case Opcode::JP_V0:   opJpV0(d);   return StepStatus::OK;
        case Opcode::RND:     opRnd(d);    return StepStatus::OK;
        case Opcode::DRW:     return opDrw(d);

        // ---- 定时器 ----
        case Opcode::LD_VX_DT: opLdVxDt(d); return StepStatus::OK;
        case Opcode::LD_VX_K:  return opLdVxK(d);   // 可能阻塞
        case Opcode::LD_DT_VX: opLdDtVx(d); return StepStatus::OK;
        case Opcode::LD_ST_VX: opLdStVx(d); return StepStatus::OK;

        // ---- I 寄存器 ----
        case Opcode::ADD_I_VX: opAddIVx(d); return StepStatus::OK;
        case Opcode::LD_F_VX:  opLdFVx(d);  return StepStatus::OK;
        case Opcode::LD_B_VX:  opLdBVx(d);  return StepStatus::OK;
        case Opcode::LD_I_VX:  opLdIVx(d);  return StepStatus::OK;
        case Opcode::LD_VX_I:  opLdVxI(d);  return StepStatus::OK;

        // ---- 认不出来 ----
        case Opcode::UNKNOWN:
            // 当空操作，但周期照烧。
            // 不抛异常：程序跑飞之后读到垃圾字节是常态，
            // 每秒钟抛 700 次异常的模拟器没法用。
            // 想知道跑飞了没有，用 --strict 或者看 trace 里的 "???"。
            return StepStatus::UNKNOWN_OPCODE;
    }

    // 所有枚举值都已经在上面列全了，走不到这里。
    // 留着是为了让编译器相信函数一定有返回值。
    return StepStatus::OK;
}

// ============================================================
//  清屏 / 返回
// ============================================================

void Machine::opCls() {
    display_.clear();
}

void Machine::opRet() {
    // 栈只有 16 层。sp_ 已经是 0 的时候执行 RET 会绕到 15，
    // 返回一个从来没压过的地址。这是程序自己的错（多返回了一次），
    // 掩码至少让它**确定性地**错，而不是越界写栈。
    pc_ = static_cast<std::uint16_t>(pop() & 0x0FFF);
}

// ============================================================
//  跳转和调用
// ============================================================

void Machine::opJp(const Decoded& d) {
    // ★ 直接赋值，**不是** advancePc(d.nnn)。
    // 因为 CALL/JP 的目标是绝对地址，不是相对偏移。
    // 而 pc_ 此刻已经指向下一条指令了（见【契约二】），
    // 所以这里必须用赋值把它**盖掉**。
    pc_ = static_cast<std::uint16_t>(d.nnn & 0x0FFF);
}

void Machine::opCall(const Decoded& d) {
    // pc_ 此刻已经指向 CALL 的下一条指令，这正是要压的返回地址。
    push(pc_);
    pc_ = static_cast<std::uint16_t>(d.nnn & 0x0FFF);
}

// ============================================================
//  条件跳过：这六个只回答"跳不跳"，推进 PC 由 executeOpcode 统一做
// ============================================================

bool Machine::testSeByte(const Decoded& d) const {
    return v_[d.x] == d.nn;
}

bool Machine::testSneByte(const Decoded& d) const {
    return v_[d.x] != d.nn;
}

bool Machine::testSeReg(const Decoded& d) const {
    return v_[d.x] == v_[d.y];
}

bool Machine::testSneReg(const Decoded& d) const {
    return v_[d.x] != v_[d.y];
}

bool Machine::testSkp(const Decoded& d) const {
    // ★ 必须 & 0x0F。Vx 里的值不保证在 0..15 之间——
    // 程序完全可以把 V1 设成 200，然后拿它当键号。
    // 不掩码就是一次越界读 keypad_ 数组。
    return keypad_[v_[d.x] & 0x0F];
}

bool Machine::testSknp(const Decoded& d) const {
    return !keypad_[v_[d.x] & 0x0F];
}

// ============================================================
//  赋值和运算
// ============================================================

void Machine::opLdByte(const Decoded& d) {
    v_[d.x] = d.nn;
}

void Machine::opAddByte(const Decoded& d) {
    // ★ 7xnn 加立即数**不碰 VF**。
    // 这一点和 8xy4（寄存器相加）不一样，而两者长得像，
    // 是新手最常搞混的一对。加法溢出就被丢掉，没有进位记录。
    v_[d.x] = static_cast<std::uint8_t>(v_[d.x] + d.nn);
}

void Machine::opLdReg(const Decoded& d) {
    v_[d.x] = v_[d.y];
}

// 8xy1 / 8xy2 / 8xy3 三个逻辑运算共享同一个"要不要清 VF"的开关。
// 三处各写一次同样的判断，但都指向 quirks.hpp 里 vfResetOnLogic 的说明。

void Machine::opOr(const Decoded& d) {
    v_[d.x] = static_cast<std::uint8_t>(v_[d.x] | v_[d.y]);
    if (quirks_.vfResetOnLogic) {
        v_[VF_INDEX] = 0;
    }
}

void Machine::opAnd(const Decoded& d) {
    v_[d.x] = static_cast<std::uint8_t>(v_[d.x] & v_[d.y]);
    if (quirks_.vfResetOnLogic) {
        v_[VF_INDEX] = 0;
    }
}

// ★ 上面 opAnd 里的两句话，顺序**换不换都一样**——这是能证明的，
//   不是"大概没人会注意"。
//
//   x == 0xF 时目标寄存器和被清的都是 VF：
//     正确：VF = VF & Vy，然后 VF = 0        最终 VF = 0
//     调换：VF = 0，然后 VF = 0 & Vy = 0     最终 VF = 0
//   而 0 & 任何数恒等于 0，正好就是重置想要的那个值。
//   所以两种写法对**任何操作数、任何 quirks 设置**都等价。
//
//   对比一下同族的另两条，它们的顺序是**可观测**的：
//     OR ：0 | Vy = Vy   —— Vy != 0 时和 0 不同
//     XOR：0 ^ Vy = Vy   —— Vy != 0 时和 0 不同
//   所以那两条的顺序被测试钉住了（见 run_tests.ps1 的 flags-vip），
//   而 AND 这条钉不住。
//
//   记录这件事的原因是它反直觉：一个看起来完全对称的三兄弟里，
//   只有一条的写法是无所谓的。要是哪天有人"统一风格"把三条都
//   调换过来，测试只会对 OR 和 XOR 报警，AND 那条会静悄悄过去——
//   而那是**正确的**，不该去补一个测不出东西的测试。
//   tests/prove_tests_can_fail.py 把 AND 单独标成
//   "equivalent mutant"，就是留了这个案底。

void Machine::opXor(const Decoded& d) {
    v_[d.x] = static_cast<std::uint8_t>(v_[d.x] ^ v_[d.y]);
    if (quirks_.vfResetOnLogic) {
        v_[VF_INDEX] = 0;
    }
}

void Machine::opAddReg(const Decoded& d) {
    // 三步顺序（见文件头）：先读操作数，再算，最后写。
    const int a = v_[d.x];
    const int b = v_[d.y];
    const int sum = a + b;

    v_[d.x] = static_cast<std::uint8_t>(sum & 0xFF);
    // ★ VF 最后写。x == 0xF 时这句会盖掉上面刚写进 V[0xF] 的结果，
    //   而这是**对的**：VF 必须反映"有没有进位"。
    //
    //   ★ 这一行的顺序是**被测试钉住的**，不是靠自觉。
    //     把两句对调，flags ROM 的第 3 行会从 VF=01 变成 VF=02，
    //     run_tests.ps1 里"8xy4 x=0xF -> VF holds the carry"立刻变红。
    //
    //     注意这个 ROM 的取值是**特意挑的**：VF=0xFF + V1=0x03 得 0x102，
    //     低字节 0x02 ≠ 进位 1，两种写法才分得开。
    //     原来 V1 是 0x02，和是 0x101，低字节 0x01 **恰好等于**进位，
    //     两种顺序给出同一个结果——测试全绿，但什么也没验。
    //     换操作数之前先确认这个巧合没被重新引入。
    v_[VF_INDEX] = (sum > 0xFF) ? 1 : 0;
}

void Machine::opSub(const Decoded& d) {
    const int a = v_[d.x];
    const int b = v_[d.y];

    v_[d.x] = static_cast<std::uint8_t>((a - b) & 0xFF);
    // ★ 借位的方向容易记反：VF = 1 表示"**没有**借位"，即 a >= b。
    //   直觉上"减法产生了标志位"会让人以为 1 代表借位，正好相反。
    v_[VF_INDEX] = (a >= b) ? 1 : 0;
}

void Machine::opShr(const Decoded& d) {
    // ★ 移的是 Vx 还是 Vy 由行为开关决定：
    //   shiftVxInPlace = true  -> Vx = Vx >> 1，Vy 被忽略（现代约定）
    //   shiftVxInPlace = false -> Vx = Vy，然后 Vx >>= 1（原始 VIP）
    // 看 quirks.hpp 的说明。两组测试 ROM 都用 x == y 的写法，
    // 所以它们对这个开关不敏感——但真游戏敏感。
    const std::uint8_t source =
        quirks_.shiftVxInPlace ? v_[d.x] : v_[d.y];

    // 先取出要被移出去的那一位（最低位），再移位。
    // 反过来的话取到的就是移位后的值了。
    const std::uint8_t carry = static_cast<std::uint8_t>(source & 0x01);

    v_[d.x] = static_cast<std::uint8_t>(source >> 1);
    v_[VF_INDEX] = carry;
}

void Machine::opSubn(const Decoded& d) {
    // ★ SUBN 是"反着减"：Vx = Vy - Vx。
    //   8xy5 是 Vx - Vy，8xy7 是 Vy - Vx，两条指令长得几乎一样，
    //   而 SUB/SUBN 两个名字也几乎一样。这里是抄错的高发区。
    const int a = v_[d.x];
    const int b = v_[d.y];

    v_[d.x] = static_cast<std::uint8_t>((b - a) & 0xFF);
    // 同样：1 表示"没有借位"，且比较方向跟着反过来。
    v_[VF_INDEX] = (b >= a) ? 1 : 0;
}

void Machine::opShl(const Decoded& d) {
    const std::uint8_t source =
        quirks_.shiftVxInPlace ? v_[d.x] : v_[d.y];

    // 左移移出去的是**最高位**（第 7 位），和右移相反。
    const std::uint8_t carry = static_cast<std::uint8_t>((source >> 7) & 0x01);

    // ★ 移位后必须 & 0xFF。std::uint8_t 参与运算时会先提升成 int，
    //   source << 1 的结果是 int（可能到 510），截断回 uint8_t 才对。
    //   显式写 & 0xFF 是为了让"我要的是低 8 位"这个意图可见。
    v_[d.x] = static_cast<std::uint8_t>((source << 1) & 0xFF);
    v_[VF_INDEX] = carry;
}

// ============================================================
//  内存和随机数
// ============================================================

void Machine::opLdAddr(const Decoded& d) {
    i_ = static_cast<std::uint16_t>(d.nnn & 0x0FFF);
}

void Machine::opJpV0(const Decoded& d) {
    // Bnnn 的两种解释：
    //   jumpUsesVx = false -> 跳到 nnn + V0   （原始 VIP）
    //   jumpUsesVx = true  -> 跳到 xnn + Vx   （CHIP-48 / SUPER-CHIP）
    //   其中 x 是 nnn 的最高半字节（也就是 d.x 字段）。
    // 无论哪种，加完都要掩码回 12 位——
    // 0xFFF + 0xFF 已经超出地址空间了。
    const std::uint16_t offset =
        quirks_.jumpUsesVx ? v_[d.x] : v_[0];
    pc_ = static_cast<std::uint16_t>((d.nnn + offset) & 0x0FFF);
}

void Machine::opRnd(const Decoded& d) {
    // ★ 用 mt19937 直接取低 8 位，**不用** std::uniform_int_distribution。
    //   因为后者"怎么把随机数映射到区间"是实现定义的，
    //   换个 libstdc++ 版本就可能给出不同的序列，
    //   于是 trace 的黄金文件会因为换了编译器而对不上。
    //   mt19937 产生的原始序列由标准规定死，固定种子在任何
    //   符合标准的实现上都一样。
    const std::uint8_t roll = static_cast<std::uint8_t>(rng_() & 0xFF);
    v_[d.x] = static_cast<std::uint8_t>(roll & d.nn);
}

StepStatus Machine::opDrw(const Decoded& d) {
    // displayWait：这个开关打开时，一帧内只允许画一次。
    // 原始 VIP 在上画精灵会等到垂直回扫，效果就是给动画限速。
    //
    // 【注意】这个判断必须在**清 VF 之前**。
    // 因为返回 WAIT_DISPLAY 意味着"这条指令这次没执行"，
    // 而没执行就不该动任何状态——包括不该把 VF 清掉。
    // 放在后面的话，每帧的第一次会清 VF，第二次不清，
    // VF 的值就变得依赖"这一帧画过几次"，完全没法预测。
    if (quirks_.displayWait && drewThisFrame_) {
        return StepStatus::WAIT_DISPLAY;
    }

    // ---- 第一步：先把坐标读进局部变量 ----
    //
    // ★★ 这是整个文件最容易写错的地方 ★★
    //
    // 下面马上要把 VF 清零。如果 x == 0xF，那么 v_[0xF] 既是
    // "横坐标来源"又是"碰撞标志"——先清零再读坐标的话，
    // 坐标就变成 0 了，精灵会诡异地画到左上角。
    //
    // 所以顺序必须是：先读坐标 → 再清 VF → 再画。
    // 4-flags 那个测试 ROM 就是专门抓这类顺序错误的。
    //
    // 【注意这里**不做** & 63 / & 31】起点回绕是屏幕坐标系的事，
    // Display::drawSprite 自己做。这里读进局部变量只是为了让
    // 下面清 VF 的时候坐标已经安全落地了。
    const int x = v_[d.x];
    const int y = v_[d.y];

    v_[VF_INDEX] = 0;

    // ---- 第二步：把精灵数据从内存里抄出来 ----
    //
    // ★ 为什么要抄一份，不直接把 &memory_[i_] 传进去？
    //   因为 Dxyn 会读 I + row，row 最大到 14。
    //   I = 0xFF8 时 I + 14 = 0x1006，越过 4096 字节的数组边界了。
    //   抄一份顺便做掩码，越界的部分读到的是内存绕回来的字节
    //   （也就是 0x000 开始的字模区），**确定性地错**而不是 UB。
    std::uint8_t sprite[15] = {};
    for (int row = 0; row < d.n; ++row) {
        sprite[row] = memory_[(i_ + static_cast<std::uint16_t>(row)) & 0x0FFF];
    }

    // ---- 第三步：画 ----
    if (display_.drawSprite(x, y, sprite, d.n, quirks_.spriteVerticalWrap)) {
        v_[VF_INDEX] = 1;   // 有像素从亮变灭，记碰撞
    }

    drewThisFrame_ = true;
    return StepStatus::OK;
}

// ============================================================
//  定时器
// ============================================================

void Machine::opLdVxDt(const Decoded& d) {
    v_[d.x] = delayTimer_;
}

StepStatus Machine::opLdVxK(const Decoded& d) {
    // Fx0A 是唯一一条**阻塞**指令：等一个按键，然后把它放进 Vx。
    //
    // ★ 关键设计：这里**不死循环等待**，而是返回 WAIT_KEY。
    //   step() 会把 PC 退回去，下一轮重新执行这条指令。
    //
    //   如果在这里 while 循环等按键会怎样？
    //     - 窗口卡死（消息循环再也跑不到）
    //     - 定时器停摆（tickTimers 再也调不到）
    //     - 声音定时器归零之后无法停止，蜂鸣器一直响
    //   把"怎么办"交给调用方，是内核能被无头驱动的关键。
    //
    //   代价是"等待"这件事变成了"每条指令重试一次"，
    //   每次都烧一个周期——而这正是我们要的：定时器要继续走。

    if (!quirks_.fx0aWaitsForRelease) {
        // 现代约定：一按下就返回。
        //
        // 【明确下来的行为】同时按住多个键时返回**编号最小**的那个。
        // 文档没规定这件事，所以必须自己定死一个，否则测试写不出
        // 确定的期望值。见 machine.cpp 的 lowestKeyDown()。
        const int key = lowestKeyDown();
        if (key < 0) {
            return StepStatus::WAIT_KEY;
        }
        v_[d.x] = static_cast<std::uint8_t>(key);
        return StepStatus::OK;
    }

    // 原始 VIP 的锁存行为：等到按下，记住它，然后**等松开**才算数。
    // 这样做的好处是"一次按键只被消费一次"，避免按住不放时
    // 后续的 Fx0A 立刻又返回同一个键。
    if (pendingKey_ < 0) {
        const int key = lowestKeyDown();
        if (key < 0) {
            return StepStatus::WAIT_KEY;
        }
        pendingKey_ = key;      // 记住了，但还要等松开
        return StepStatus::WAIT_KEY;
    }

    if (lowestKeyDown() >= 0) {
        return StepStatus::WAIT_KEY;   // 还没松开，继续等
    }

    v_[d.x] = static_cast<std::uint8_t>(pendingKey_);
    pendingKey_ = -1;
    return StepStatus::OK;
}

void Machine::opLdDtVx(const Decoded& d) {
    delayTimer_ = v_[d.x];
}

void Machine::opLdStVx(const Decoded& d) {
    // 声音定时器只是个倒计时，响不响由平台层根据 soundActive() 决定。
    // 内核不知道"声音"是什么——它只管这个数字在不在走。
    soundTimer_ = v_[d.x];
}

// ============================================================
//  I 寄存器相关
// ============================================================

void Machine::opAddIVx(const Decoded& d) {
    // 三步顺序：先读操作数。
    // x == 0xF 时下面写 VF 会盖掉 V[0xF]，但那个值已经读进 local 了。
    const std::uint16_t operand = v_[d.x];
    const std::uint16_t sum = static_cast<std::uint16_t>(i_ + operand);

    if (quirks_.vfOnIndexOverflow) {
        // Amiga 上的移植版会在这里置标志，有游戏依赖它。
        // 原始 VIP 和测试 ROM 都期望 VF 不动，所以默认关闭。
        v_[VF_INDEX] = (sum > 0x0FFF) ? 1 : 0;
    }

    i_ = static_cast<std::uint16_t>(sum & 0x0FFF);
}

void Machine::opLdFVx(const Decoded& d) {
    // ★ 只取低半字节。Vx 里可能是个大于 15 的值
    //   （程序把 Vx 当普通变量用之后忘了清零就调 Fx29），
    //   不掩码的话算出来的地址会指到字模外面去。
    //
    //   fontUsesFullByte 那个开关就是"不掩码"的老实现，
    //   留着是为了能复现那种行为，不是推荐做法。
    const std::uint8_t digit = quirks_.fontUsesFullByte
                                   ? v_[d.x]
                                   : static_cast<std::uint8_t>(v_[d.x] & 0x0F);

    i_ = static_cast<std::uint16_t>(
        (quirks_.fontBase() + FONT_GLYPH_BYTES * digit) & 0x0FFF);
}

void Machine::opLdBVx(const Decoded& d) {
    // Fx33：把 Vx 的十进制值拆成三位，写进 [I]、[I+1]、[I+2]。
    // 百位、十位、个位，高位在前（大端）。
    //
    // ★ 经典错误：十位写成 v / 10 而不是 (v / 10) % 10。
    //   255 会变成 2、25、5 —— 后两位都错了，而且看起来"挺像"，
    //   扫一眼不容易发现。测试要用 255、137、100、99、9、0 覆盖。
    const std::uint8_t value = v_[d.x];

    memory_[(i_ + 0) & 0x0FFF] = static_cast<std::uint8_t>(value / 100);
    memory_[(i_ + 1) & 0x0FFF] =
        static_cast<std::uint8_t>((value / 10) % 10);
    memory_[(i_ + 2) & 0x0FFF] = static_cast<std::uint8_t>(value % 10);
}

void Machine::opLdIVx(const Decoded& d) {
    // Fx55：把 V0..Vx 共 x+1 个寄存器写进内存，从 I 开始。
    //
    // 【注意】循环上界是 d.x（原始索引），不是被改过的值。
    //   因为按 quirk 递增 I 的时候，用的是 d.x 而不是 Vx 的内容。
    for (int k = 0; k <= d.x; ++k) {
        memory_[(i_ + static_cast<std::uint16_t>(k)) & 0x0FFF] =
            v_[static_cast<std::size_t>(k)];
    }

    if (quirks_.loadStoreIncrementsI) {
        i_ = static_cast<std::uint16_t>((i_ + d.x + 1) & 0x0FFF);
    }
}

void Machine::opLdVxI(const Decoded& d) {
    for (int k = 0; k <= d.x; ++k) {
        v_[static_cast<std::size_t>(k)] =
            memory_[(i_ + static_cast<std::uint16_t>(k)) & 0x0FFF];
    }

    if (quirks_.loadStoreIncrementsI) {
        i_ = static_cast<std::uint16_t>((i_ + d.x + 1) & 0x0FFF);
    }
}
