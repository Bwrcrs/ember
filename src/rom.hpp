// ============================================================
//  Rom —— 一段要装进机器里的程序
//
//  ROM 就是一段字节。没有文件头、没有校验和、没有元数据。
//  整个格式就是"机器码，从第一个字节开始"。
//
//  【为什么常量定义在这里而不是 machine.hpp】
//  因为 machine.hpp 要 #include rom.hpp（loadRom 收一个 Rom），
//  反过来的话就是循环包含。而这些常量既描述"内存多大"，
//  也描述"ROM 能多大、从哪儿开始装"——放在 ROM 这一侧同样合理，
//  而且依赖方向是干净的单向。
//  真正需要给外界看的名字（Machine::MEMORY_SIZE 之类）在
//  machine.hpp 里再转发一次。
//
//  【装载地址 0x200 是怎么来的】
//  COSMAC VIP 上的 CHIP-8 解释器自己占用了内存最低的 512 字节
//  （0x000-0x1FF），所以程序只能从 0x200 开始放。
//  这个数字一直沿用到今天：**所有** CHIP-8 ROM 都假设自己被装在 0x200。
//  其中 0x000-0x04F 后来被约定用来放那 80 字节字模。
// ============================================================

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// 机器总内存 4KB。
inline constexpr std::uint16_t MEMORY_SIZE = 0x1000;

// 所有 ROM 的装载地址。
inline constexpr std::uint16_t ROM_BASE = 0x200;

// 一段 ROM 最大能有多大：0x1000 - 0x200 = 3584 字节。
// 超过这个数的文件一定是拿错了（比如把别的格式当 ROM 传来），
// 所以宁可直接报错，也不要截断——截断会得到一个"能跑但行为诡异"的程序，
// 那种 bug 比开不了机难查一百倍。
inline constexpr std::size_t MAX_ROM_SIZE = MEMORY_SIZE - ROM_BASE;

class Rom {
public:
    Rom() = default;

    // 从文件读。文件不存在、是空文件、或者超过 3584 字节都抛 EmberError。
    static Rom fromFile(const std::string& utf8Path);

    // 从内存里的字节造。空的话抛 EmberError，太大的话也抛。
    // 测试和汇编器走这条路。
    static Rom fromBytes(const std::vector<std::uint8_t>& bytes);

    const std::vector<std::uint8_t>& bytes() const { return bytes_; }
    std::size_t size() const { return bytes_.size(); }
    bool empty() const { return bytes_.empty(); }

    // 这段 ROM 占用的地址范围，给错误消息和调试用。
    // 末端是**开区间**（最后一个字节的地址 + 1）。
    std::uint16_t loadAddress() const { return ROM_BASE; }
    std::uint16_t endAddress() const {
        return static_cast<std::uint16_t>(ROM_BASE + bytes_.size());
    }

private:
    std::vector<std::uint8_t> bytes_;
};
