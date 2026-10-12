// ============================================================
//  Error —— 把带行列号的错误渲染成能看的文本
//  见 error.hpp 的说明。
// ============================================================

#include "error.hpp"

namespace {

// 从源码里抠出第 line 行（从 1 开始数）。
// 越界就返回空串——错误信息少一行源码，总比崩了强。
std::string extractLine(const std::string& source, int line) {
    int current = 1;
    std::size_t start = 0;

    while (start <= source.size()) {
        std::size_t end = source.find('\n', start);
        if (end == std::string::npos) {
            end = source.size();
        }

        if (current == line) {
            std::string text = source.substr(start, end - start);
            // 行尾的 \r 是 CRLF 留下的，会算进列宽导致箭头对不准
            if (!text.empty() && text.back() == '\r') {
                text.pop_back();
            }
            return text;
        }

        if (end == source.size()) {
            break;
        }
        start = end + 1;
        ++current;
    }

    return "";
}

}  // namespace

std::string formatAsmError(const std::string& source, const AsmError& error) {
    std::string out = "[汇编错误] 第 " + std::to_string(error.line()) +
                      " 行 第 " + std::to_string(error.column()) +
                      " 列: " + error.what() + "\n";

    std::string text = extractLine(source, error.line());
    if (text.empty()) {
        return out;
    }

    out += "    " + text + "\n";
    out += "    ";

    // 箭头的列号是 1 起算的，字符串下标是 0 起算的。
    // 如果列号落在了行尾之后（比如整行被吃掉了），把箭头夹回行尾，
    // 不然会打出一串空格把箭头推到屏幕外面去。
    int caretAt = error.column() - 1;
    if (caretAt < 0) {
        caretAt = 0;
    }
    if (caretAt > static_cast<int>(text.size())) {
        caretAt = static_cast<int>(text.size());
    }

    // 【注意】这里**不能**考虑中文对齐的问题。
    // 汇编源码里可能出现中文注释，一个汉字占两个字符宽度，
    // 但 std::string 里只算一个字节（UTF-8 是三个字节，更糟）。
    // 想把箭头对准中文行需要知道显示宽度，代价太大。
    // 折中：列号按字节算，注释行的箭头可能会偏。
    // 真正的解法是"错误不落在注释里"，而这一点由汇编器保证
    // ——报错时绝不停在注释中间。
    out.append(static_cast<std::size_t>(caretAt), ' ');
    out += "^\n";

    return out;
}
