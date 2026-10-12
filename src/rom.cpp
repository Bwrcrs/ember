// ============================================================
//  Rom —— 装载和校验
//  见 rom.hpp 的说明。
// ============================================================

#include "rom.hpp"

#include "error.hpp"
#include "utf8path.hpp"

namespace {

// 两个入口共用的校验。检查顺序有讲究：
// 先看空不空，再看大不大——因为"空文件"和"太大"是两种完全不同的用户错误，
// 报出来的消息也该不一样。
void validate(std::size_t size, const std::string& what) {
    if (size == 0) {
        throw EmberError(what + "是空的（0 字节）。ROM 至少要有一个字节。");
    }

    if (size > MAX_ROM_SIZE) {
        throw EmberError(what + "有 " + std::to_string(size) +
                         " 字节，超过了上限 " + std::to_string(MAX_ROM_SIZE) +
                         " 字节（0x1000 的内存减去 0x200 的装载地址）。"
                         "这多半不是一份 CHIP-8 ROM。");
    }
}

}  // namespace

Rom Rom::fromFile(const std::string& utf8Path) {
    // readFileBytes 内部是二进制模式，所以 0x1A 不会被当成文件结束。
    std::vector<std::uint8_t> bytes = readFileBytes(utf8Path);

    validate(bytes.size(), "文件 " + utf8Path);

    Rom rom;
    rom.bytes_ = std::move(bytes);
    return rom;
}

Rom Rom::fromBytes(const std::vector<std::uint8_t>& bytes) {
    validate(bytes.size(), "这段 ROM");

    Rom rom;
    rom.bytes_ = bytes;
    return rom;
}
