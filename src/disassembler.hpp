// ============================================================
//  Disassembler —— 把 16 位数字变回人能读的文本
//
//  这个文件的输出格式**必须和汇编器认的格式一致**。
//  selftest 会把全部 65536 个值走一遍"译码 -> 反汇编 -> 汇编 -> 译码"，
//  任何一个不一致都会在那一步暴露出来。
//
//  ★ 有一处**不可能**往返一致，而且是有原因的：
//
//    8xy6 / 8xyE 在 shiftVxInPlace = true 时，Vy 被完全忽略。
//    这时反汇编出来的是 "SHR V3"（不带 Vy，因为说了也没用），
//    汇编回去得到 0x8336（y 默认填 3），而原始可能是 0x8366。
//
//    两个值的**行为完全相同**，所以这不是 bug，是"同一个东西的
//    两种写法"。selftest 对这个家族只比对 Opcode 和 x，
//    不比对完整的 16 位。这一点写进了 selftest 的注释里，
//    而不是想办法绕过去。
//
//  地址一律印成 0xNNN（三位十六进制），字节印成 0xNN（两位）。
//  寄存器印成 V0..VF，用大写十六进制数字。
// ============================================================

#pragma once

#include <cstdint>
#include <string>

#include "opcodes.hpp"
#include "quirks.hpp"

// 寄存器名："V0" ... "VF"。
std::string registerName(int index);

// 把已经译好码的指令变成文本。
//
// quirks 参数只影响 SHR / SHL 两个的印法（见文件头的说明）。
// 其他指令的印法和 quirks 无关，所以反汇编一个 ROM 时
// 传哪组开关都不影响绝大多数行。
std::string disassemble(const Decoded& d, const Quirks& quirks);

// 直接给 16 位也行，内部会先 decode。
std::string disassemble(std::uint16_t raw, const Quirks& quirks);
