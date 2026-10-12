// ============================================================
//  Utf8Path —— 见 utf8path.hpp
//
//  这是整个核心里唯一允许出现 <windows.h> 的文件之一
//  （另一个是 platform_win32，但它整个包在 #ifdef EMBER_GUI 里）。
//  理由是这里同时有 POSIX 的 #else 分支，所以核心在任何平台上都能编过。
//  build.bat 的 lint 明确放行这个文件名。
// ============================================================

#include "utf8path.hpp"

#include <cstdio>

#include "error.hpp"

#ifdef _WIN32
// <windows.h> 会把 TRUE / FALSE / NULL / min / max 定义成宏，
// 污染后面所有的代码。所以它必须**排在所有项目头文件之后**，
// 并且只在这一个 .cpp 里出现，绝不进任何 .hpp——
// 一旦进了头文件，所有 #include 它的地方都会被污染。
// 这是 Nova 和 Acorn 都踩过的同一个坑。
#include <windows.h>
#endif

namespace {

#ifdef _WIN32
// UTF-8 的 std::string -> UTF-16 的 std::wstring。
// 空字符串会在调用前被挡住，所以这里不用处理。
std::wstring toWide(const std::string& utf8) {
    const int needed = ::MultiByteToWideChar(
        CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);

    if (needed <= 0) {
        throw EmberError("路径不是合法的 UTF-8：" + utf8);
    }

    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                          static_cast<int>(utf8.size()), &wide[0], needed);
    return wide;
}
#endif

// 以二进制模式打开。
// Windows 走 _wfopen，其他平台走普通 fopen。
std::FILE* openBinary(const std::string& utf8Path, const char* mode) {
#ifdef _WIN32
    const std::wstring widePath = toWide(utf8Path);
    const std::wstring wideMode(mode, mode + std::char_traits<char>::length(mode));
    return ::_wfopen(widePath.c_str(), wideMode.c_str());
#else
    // POSIX 的 fopen 本来就按字节处理路径，UTF-8 直接能用。
    return std::fopen(utf8Path.c_str(), mode);
#endif
}

}  // namespace

std::vector<std::uint8_t> readFileBytes(const std::string& utf8Path) {
    if (utf8Path.empty()) {
        throw EmberError("路径是空的");
    }

    std::FILE* file = openBinary(utf8Path, "rb");
    if (file == nullptr) {
        throw EmberError("打不开文件：" + utf8Path);
    }

    // 先跳到末尾问出文件大小，再跳回来。
    // 这样能一次分配够内存，不用边读边扩容。
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        throw EmberError("读不了文件长度：" + utf8Path);
    }

    const long size = std::ftell(file);
    if (size < 0) {
        std::fclose(file);
        throw EmberError("读不了文件长度：" + utf8Path);
    }

    std::rewind(file);

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));

    // size 为 0 时不能调 fread——传一个空 vector 的 data() 是合法的，
    // 但请求读 0 个字节的行为在有些实现上会返回非 0，反而误报失败。
    if (size > 0) {
        const std::size_t got =
            std::fread(bytes.data(), 1, bytes.size(), file);
        if (got != bytes.size()) {
            std::fclose(file);
            throw EmberError("文件读到一半出错了：" + utf8Path);
        }
    }

    std::fclose(file);
    return bytes;
}

void writeFileBytes(const std::string& utf8Path,
                    const std::vector<std::uint8_t>& bytes) {
    if (utf8Path.empty()) {
        throw EmberError("路径是空的");
    }

    std::FILE* file = openBinary(utf8Path, "wb");
    if (file == nullptr) {
        throw EmberError("写不了文件：" + utf8Path);
    }

    if (!bytes.empty()) {
        const std::size_t put =
            std::fwrite(bytes.data(), 1, bytes.size(), file);
        if (put != bytes.size()) {
            std::fclose(file);
            throw EmberError("文件写到一半出错了：" + utf8Path);
        }
    }

    std::fclose(file);
}
