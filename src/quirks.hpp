// ============================================================
//  Quirks —— CHIP-8 里那些"两种做法都有人用"的地方
//
//  CHIP-8 是 1977 年给 COSMAC VIP 这台机器写的。后来被人移植到
//  无数平台上，每个移植者都遇到同一批"原文档没写清楚"的地方，
//  于是各自选了一种做法。几十年下来，同一个操作码在不同模拟器上
//  行为不一样，而**游戏 ROM 是按其中某一种写的**。
//
//  所以这些不是"要不要兼容"的问题，是"必须做成开关"的问题：
//  同一份代码，换一组开关，才能同时跑通不同年代的程序。
//
//  ★ 关键认识：这里没有"正确"答案，只有"和哪个年代的 ROM 一致"。
//    所以默认值的选择标准是"能跑通现在还在流通的那些游戏"，
//    不是"哪个更对"。
//
//  【设计取舍】为什么是 10 个 bool 而不是一个 enum？
//  因为这些开关是**正交**的——理论上 2^10 种组合都存在，
//  只是常用的就那么几组（见下面的预设）。用 enum 会假装
//  "只有这几种组合"，而实际上存档文件里什么组合都可能冒出来。
//
//  【设计取舍】为什么字模地址也做成 bool 而不是 uint16_t？
//  为了让 10 个开关形状统一，selftest 能用同一个循环遍历它们、
//  打印、比对。多一个"不规则"的字段就要多一处特判。
//  真正的地址由 fontBase() 算出来。
//
//  这个文件是 header-only 的：里面只有数据和几个 inline 函数，
//  没有需要单独一个编译单元的东西。
// ============================================================

#pragma once

#include <cstdint>
#include <string>

#include "error.hpp"

struct Quirks {
    // ---- 1. 8xy6 / 8xyE 右移左移：移的是谁 ----
    //   true  : Vx = Vx >> 1，Vy 被忽略        （CHIP-48 / SUPER-CHIP）
    //   false : Vx = Vy，然后 Vx >>= 1          （原始 COSMAC VIP）
    // 无论哪种，VF 都是被移出去的那一位，且 VF **最后**写。
    // 见 cpu.cpp 里 opShr/opShl 的注释：x == 0xF 时标志位要盖过结果。
    bool shiftVxInPlace = true;

    // ---- 2. Fx55 / Fx65 存/取寄存器组后，I 加不加 ----
    //   true  : I 最终停在 I + x + 1  （VIP 用循环写的，I 自己走了）
    //   false : I 不变                （CHIP-48 用临时变量，I 被保住）
    // 默认 false，因为主流游戏假设 I 不变。
    bool loadStoreIncrementsI = false;

    // ---- 3. Bnnn 跳转：加 V0 还是加 Vx ----
    //   true  : 跳到 XNN + Vx，X 是 NNN 的最高半字节  （CHIP-48 / SUPER-CHIP）
    //   false : 跳到 NNN + V0                          （原始 COSMAC VIP）
    // 注意两个都是 false 的情况也合法：就是"不加寄存器，直接跳 NNN"。
    bool jumpUsesVx = false;

    // ---- 4. 8xy1 / 8xy2 / 8xy3 逻辑运算后 VF 清不清零 ----
    //   true  : VF = 0  （VIP 把 VF 当临时寄存器用，用完就清）
    //   false : VF 不动 （CHIP-48 改了行为）
    bool vfResetOnLogic = false;

    // ---- 5. Dxyn 精灵"身体"超出屏幕底部：裁掉还是绕回来 ----
    //   true  : 超过 y = 31 的行从 y = 0 重新出现（SUPER-CHIP 1.0 的遗留行为）
    //   false : 超过的行直接丢掉                  （VIP 和 SUPER-CHIP 1.1）
    //
    // ★ 这个开关只管**精灵身体**。起点坐标是另一回事：
    //   Vx 和 Vy 永远回绕（Vx & 63、Vy & 31），横向永远裁剪，任何版本都不绕。
    //   这三件事经常被混为一谈，是画错精灵的高发原因。
    bool spriteVerticalWrap = false;

    // ---- 6. Dxyn 每帧最多画一次 ----
    //   true  : 一帧内重复执行 Dxyn 会被拖延到下一帧（VIP 在等垂直回扫）
    //   false : 想画几次画几次
    // 真实游戏的动画速度依赖这个（VIP 上的游戏是把它当"限速器"用的），
    // 但现代程序不希望被限速，所以默认 false。
    bool displayWait = false;

    // ---- 7. Fx1E（I += Vx）越界时置不置 VF ----
    //   true  : I 超过 0xFFF 时 VF = 1  （Amiga 上的移植版，有游戏依赖它）
    //   false : VF 不动                 （VIP 没碰 VF，测试 ROM 期望这样）
    bool vfOnIndexOverflow = false;

    // ---- 8. 字模放在哪 ----
    //   true  : 0x000  （原始 VIP 的内存布局）
    //   false : 0x050  （现代约定，避开零页）
    bool fontAtVipAddress = false;

    // ---- 9. Fx29 取字模地址时，Vx 取整个字节还是只取低半字节 ----
    //   true  : I = fontBase + Vx，Vx >= 0x10 就指到字模外面去了
    //   false : I = fontBase + (Vx & 0x0F)，只认低半字节（推荐）
    // 几乎所有人都要 false。做成开关是因为它确实存在于某些老实现里。
    bool fontUsesFullByte = false;

    // ---- 10. Fx0A 等按键：等"按下"还是等"松开" ----
    //   true  : 等到按键**松开**才算数（VIP 的锁存行为）
    //   false : 一按下就返回（现代约定）
    bool fx0aWaitsForRelease = false;

    // ---- 字模地址（由开关 8 算出来，不是独立字段） ----
    std::uint16_t fontBase() const {
        return fontAtVipAddress ? 0x0000 : 0x0050;
    }

    // ---- 预设 ----
    // 默认：能跑通现在还在流通的游戏。
    static Quirks modern() { return Quirks{}; }

    // 原始 COSMAC VIP。给 5-quirks 测试 ROM 的 target 1 用。
    static Quirks vip() {
        Quirks q;
        q.shiftVxInPlace = false;
        q.loadStoreIncrementsI = true;
        q.jumpUsesVx = false;
        q.vfResetOnLogic = true;
        q.spriteVerticalWrap = false;
        q.displayWait = true;
        q.vfOnIndexOverflow = false;
        // ★ 这一行原来是 false，和上面 fontAtVipAddress 那条注释
        //   （"true: 0x000 原始 VIP 的内存布局"）直接矛盾——
        //   于是 vip 预设的字模落在 0x050，也就是"现代约定"。
        //   两处都写着"VIP"，一处说是，一处说否，只能有一个对。
        //
        //   对的是 true：VIP 的 CHIP-8 解释器占着 0x000-0x1FF，
        //   字模就在解释器自己那一段的开头；0x050 是后来
        //   CHIP-48 为了避开零页才挪的。
        //
        //   【这个 bug 是怎么露出来的】不是测试抓的（测试那时
        //   全绿），是 selftest --font 打印"VIP 预设装在 0x050"
        //   那一行，看着别扭才回头查的。
        //   一个能看的输出比一个只能变红变绿的断言更容易发现
        //   "两句话互相矛盾"这种问题。
        q.fontAtVipAddress = true;
        q.fontUsesFullByte = false;
        q.fx0aWaitsForRelease = false;
        return q;
    }

    // SUPER-CHIP 的两组。阶段 8 才会真正用到，
    // 现在先留着，这样 --quirks 的名字列表从头就是完整的，
    // 不会等做到阶段 8 才发现名字得改。
    static Quirks schipModern();
    static Quirks schipLegacy();

    // ---- 按名字取预设 ----
    // 认不出来就抛 EmberError，消息里带上所有合法的名字。
    // 这样用户拼错时不用去翻文档。
    static Quirks fromName(const std::string& name);

    // ---- 自检用：把 10 个开关按统一形状暴露出来 ----
    // 有了这两个函数，selftest 可以写一个循环遍历所有开关，
    // 而不用为每个开关写一行。加第 11 个开关时只改这里。
    static constexpr int FLAG_COUNT = 10;
    static const char* flagName(int index);
    bool flagValue(int index) const;

    // 合法预设名字的列表，给错误消息和 --help 用。
    static std::string presetNames();
};

// ---- 下面几个是 inline，放在头文件里 ----

inline Quirks Quirks::schipModern() {
    Quirks q;
    q.shiftVxInPlace = true;
    q.loadStoreIncrementsI = false;
    q.jumpUsesVx = true;
    q.vfResetOnLogic = false;
    q.spriteVerticalWrap = false;
    q.displayWait = false;
    q.vfOnIndexOverflow = false;
    q.fontAtVipAddress = false;
    q.fontUsesFullByte = false;
    q.fx0aWaitsForRelease = false;
    return q;
}

inline Quirks Quirks::schipLegacy() {
    Quirks q = Quirks::schipModern();
    // 1.0 版的区别只在这两处
    q.spriteVerticalWrap = true;
    q.displayWait = true;
    return q;
}

inline Quirks Quirks::fromName(const std::string& name) {
    if (name == "modern") {
        return Quirks::modern();
    }
    if (name == "vip") {
        return Quirks::vip();
    }
    if (name == "schip" || name == "schip-modern") {
        return Quirks::schipModern();
    }
    if (name == "schip-legacy") {
        return Quirks::schipLegacy();
    }

    throw EmberError("不认识的行为开关预设 \"" + name + "\"，可用的有：" +
                     presetNames());
}

inline const char* Quirks::flagName(int index) {
    switch (index) {
        case 0: return "shiftVxInPlace";
        case 1: return "loadStoreIncrementsI";
        case 2: return "jumpUsesVx";
        case 3: return "vfResetOnLogic";
        case 4: return "spriteVerticalWrap";
        case 5: return "displayWait";
        case 6: return "vfOnIndexOverflow";
        case 7: return "fontAtVipAddress";
        case 8: return "fontUsesFullByte";
        case 9: return "fx0aWaitsForRelease";
        default: return "?";
    }
}

inline bool Quirks::flagValue(int index) const {
    switch (index) {
        case 0: return shiftVxInPlace;
        case 1: return loadStoreIncrementsI;
        case 2: return jumpUsesVx;
        case 3: return vfResetOnLogic;
        case 4: return spriteVerticalWrap;
        case 5: return displayWait;
        case 6: return vfOnIndexOverflow;
        case 7: return fontAtVipAddress;
        case 8: return fontUsesFullByte;
        case 9: return fx0aWaitsForRelease;
        default: return false;
    }
}

inline std::string Quirks::presetNames() {
    return "modern, vip, schip, schip-legacy";
}
