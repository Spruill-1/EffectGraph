#pragma once

// UTF-16 <-> UTF-8 conversion. Header-only so the broker, which does not link
// the engine, can use it too. Invalid input is replaced (U+FFFD), never
// truncated byte by byte.

#include <string>
#include <string_view>

namespace ShaderLab
{
    inline std::string WideToUtf8(std::wstring_view wide)
    {
        if (wide.empty()) return {};
        const int length = ::WideCharToMultiByte(CP_UTF8, 0,
            wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        if (length <= 0) return {};
        std::string out(static_cast<size_t>(length), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0,
            wide.data(), static_cast<int>(wide.size()), out.data(), length, nullptr, nullptr);
        return out;
    }

    inline std::wstring Utf8ToWide(std::string_view utf8)
    {
        if (utf8.empty()) return {};
        const int length = ::MultiByteToWideChar(CP_UTF8, 0,
            utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        if (length <= 0) return {};
        std::wstring out(static_cast<size_t>(length), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0,
            utf8.data(), static_cast<int>(utf8.size()), out.data(), length);
        return out;
    }
}
