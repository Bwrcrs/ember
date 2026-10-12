// ============================================================
//  HexFmt —— 十六进制和定宽对齐的小工具
//
//  反汇编器、追踪器、main 的输出都要印十六进制数字，
//  而且要印得**定宽对齐**——因为这些东西会被当成黄金文件比对，
//  对不齐的话 diff 出来一片红，看不出真正变了哪一行。
//
//  抽成一个 header-only 的小文件，而不是各写一份：
//  各写一份的话，反汇编器印 "0x0a"、追踪器印 "0A"，
//  同一个数字两种写法，比对时全是噪音。
//
//  ★ 一律**大写**十六进制。
//
//    这条一开始写成了小写（理由是"C++ 字面量习惯"，那是个
//    和这里无关的理由），后来改掉了——因为寄存器名是靠这个
//    生成的：小写会印出 "Vf"，而这个寄存器是**标志寄存器 VF**，
//    它在整个 CHIP-8 里是个特殊角色，必须一眼能从 V0..VE 里
//    认出来。"Vf" 和 "V1" 在等宽字体下几乎一样显眼，
//    而 "VF" 跳出来。
//
//    顺带的好处：反汇编列表用大写十六进制是几十年的惯例，
//    读起来更像汇编。
// ============================================================

#pragma once

#include <cstdint>
#include <string>

inline constexpr const char* HEX_DIGITS = "0123456789ABCDEF";

// 一位十六进制，不带前缀。寄存器号和 DRW 的行数用。
inline std::string hex1(int value) {
    std::string out;
    out += HEX_DIGITS[value & 0x0F];
    return out;
}

// 两位十六进制，带 0x 前缀。立即数用。
inline std::string hex2(std::uint8_t value) {
    std::string out = "0x";
    out += HEX_DIGITS[(value >> 4) & 0x0F];
    out += HEX_DIGITS[value & 0x0F];
    return out;
}

// 三位十六进制，带 0x 前缀。地址用（12 位地址空间）。
inline std::string hex3(std::uint16_t value) {
    std::string out = "0x";
    const int v = value & 0x0FFF;
    out += HEX_DIGITS[(v >> 8) & 0x0F];
    out += HEX_DIGITS[(v >> 4) & 0x0F];
    out += HEX_DIGITS[v & 0x0F];
    return out;
}

// 四位十六进制，带 0x 前缀。整条指令的原始值用。
inline std::string hex4(std::uint16_t value) {
    std::string out = "0x";
    out += HEX_DIGITS[(value >> 12) & 0x0F];
    out += HEX_DIGITS[(value >> 8) & 0x0F];
    out += HEX_DIGITS[(value >> 4) & 0x0F];
    out += HEX_DIGITS[value & 0x0F];
    return out;
}

// 两位十六进制，**不带前缀**。字节转储用。
inline std::string hex2bare(std::uint8_t value) {
    std::string out;
    out += HEX_DIGITS[(value >> 4) & 0x0F];
    out += HEX_DIGITS[value & 0x0F];
    return out;
}

// 左边补空格到 width 列。数字右对齐用。
inline std::string padLeft(const std::string& text, std::size_t width) {
    if (text.size() >= width) {
        return text;
    }
    return std::string(width - text.size(), ' ') + text;
}

// 右边补空格到 width 列。文本左对齐用。
inline std::string padRight(const std::string& text, std::size_t width) {
    if (text.size() >= width) {
        return text;
    }
    return text + std::string(width - text.size(), ' ');
}

// 无符号十进制的快捷写法。std::to_string 对每种整型都重载了，
// 但调用处经常要的是"一个 unsigned long long 的十进制"，
// 直接写会到处都是 static_cast。
inline std::string dec(unsigned long long value) {
    return std::to_string(value);
}
