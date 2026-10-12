// ============================================================
//  Error —— Ember 里的"出错"这件事
//
//  和 Nova / Acorn 一样：用异常，不用错误码。
//
//  分两个类型，因为两者的"用户"不同：
//
//    EmberError  一般的错。文件打不开、ROM 太大、参数不对。
//                调用方处理不了，只能打印出来然后退出。
//
//    AsmError    汇编器报的错，比上面多带一个行列号。
//                因为汇编源码是人写的，写错了要指出来是**哪一行的哪个字符**。
//                见下面 formatAsmError 里的箭头。
//
//  【设计取舍】为什么 AsmError 不直接塞进 EmberError 当成员？
//  因为那样每个 catch (EmberError&) 的地方都要判断"有没有位置信息"，
//  而实际上只有汇编那一段代码需要位置。分成两个类型，
//  普通代码只管 catch 基类，汇编器 catch 派生类，各取所需。
//
//  退出码约定（照抄 Acorn）：
//    0 = 成功
//    1 = 用户给的东西是错的（汇编报错、ROM 太大、--strict 违规）
//    2 = 用法/环境不对（文件不存在、参数不认识、子命令拼错）
//  区分 1 和 2 的意义：脚本看到 2 才知道"重试同样的输入也没用"。
// ============================================================

#pragma once

#include <stdexcept>
#include <string>

class EmberError : public std::runtime_error {
public:
    explicit EmberError(const std::string& message)
        : std::runtime_error(message) {}
};

class AsmError : public EmberError {
public:
    AsmError(int line, int column, const std::string& message)
        : EmberError(message), line_(line), column_(column) {}

    int line() const { return line_; }
    int column() const { return column_; }

private:
    int line_;
    int column_;
};

// 把 AsmError 渲染成三行：位置、出错的那行源码、指向那一列的箭头。
//
//     [汇编错误] 第 3 行 第 12 列: 不认识的指令 "LDV"
//         LD V1, LDV
//                    ^
//
// source 传整份源码（按 \n 切开用），error 是捕获到的异常。
// 这个函数在 error.cpp 里，因为要遍历源码，不算一行两行。
std::string formatAsmError(const std::string& source, const AsmError& error);
