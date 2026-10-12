// ============================================================
//  Utf8Path —— 用 UTF-8 写的路径去开文件
//
//  这是个**可移植性补丁**，不是平台层。它只做一件事：
//  把"UTF-8 编码的路径字符串"变成能真正打开的文件。
//
//  【为什么需要这个文件】
//  Windows 上的 fopen 用的是**系统 ANSI 代码页**（这台机器上是 GBK），
//  而程序内部一路传的都是 UTF-8。于是路径里有中文时：
//
//      路径 "D:\我的ROM\游戏.ch8" 在内存里是 UTF-8
//      fopen 拿这串字节按 GBK 去解码 -> 解出一堆乱码 -> 文件不存在
//
//  而且它不一定报错！GBK 解出来的乱码如果碰巧是个合法的短文件名，
//  你可能打开到**完全不相干的文件**。这个 bug 查起来能让人怀疑人生。
//
//  解法是走宽字符：UTF-8 -> UTF-16（MultiByteToWideChar）-> _wfopen。
//  Windows 的宽字符 API 内部用 UTF-16，能正确表示任何路径。
//
//  【为什么它在核心层而不是 platform_win32 里】
//  因为 ROM 装载和汇编输出都必须能在**无窗口版本**里工作——
//  测试跑的就是无窗口版本，而测试里的 ROM 路径也可能有中文。
//  如果这个功能放在 platform_win32 里，headless 构建就会缺掉它。
//
//  所以它自带 #else 分支：非 Windows 上直接用普通 fopen
//  （Linux/macOS 的 fopen 本来就吃 UTF-8，不需要转换）。
//  因为这个 #else 分支的存在，它可以在核心里出现，
//  而 platform_win32 不行——那个文件在没有 Win32 的情况下
//  整个是空的。build.bat 的 lint 把这一点写成了规则。
// ============================================================

#pragma once

#include <cstdint>
#include <string>
#include <vector>

// 读一个文件的全部字节。
// 【重要】一定是以**二进制**模式打开的。文本模式会把 \r\n 翻译掉，
// 还会把字节 0x1A（EOF）当成文件结束——ROM 里出现 0x1A 是常事，
// 那样读进来的 ROM 会被悄悄截断。
//
// 打不开就抛 EmberError，消息里带上路径。
std::vector<std::uint8_t> readFileBytes(const std::string& utf8Path);

// 把全部字节写进一个文件（覆盖）。
// 汇编器的输出用这个。同样是二进制模式。
void writeFileBytes(const std::string& utf8Path,
                    const std::vector<std::uint8_t>& bytes);
