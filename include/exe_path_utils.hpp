#pragma once

#include <filesystem>
#include <system_error>
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <vector>
#elif defined(__linux__)
#include <array>
#include <unistd.h>
#endif

namespace exe_path_utils {

namespace fs = std::filesystem;

inline fs::path get_executable_path() {
#if defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');

    for (;;) {
        const DWORD size = static_cast<DWORD>(buffer.size());
        const DWORD len = ::GetModuleFileNameW(nullptr, buffer.data(), size);

        if (len == 0) {
            throw std::system_error(static_cast<int>(::GetLastError()),
                                    std::system_category(),
                                    "GetModuleFileNameW failed");
        }

        if (len < size) {
            buffer.resize(len);
            return fs::weakly_canonical(fs::path(buffer));
        }

        buffer.resize(buffer.size() * 2);
    }

#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);

    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        throw std::runtime_error("_NSGetExecutablePath failed");
    }

    return fs::weakly_canonical(fs::path(buffer.data()));

#elif defined(__linux__)
    std::array<char, 4096> buffer{};
    const ssize_t len =
        ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);

    if (len <= 0) {
        throw std::system_error(errno, std::generic_category(),
                                "readlink(/proc/self/exe) failed");
    }

    buffer[static_cast<std::size_t>(len)] = '\0';
    return fs::weakly_canonical(fs::path(buffer.data()));

#else
#error Unsupported platform: implement get_executable_path() for this OS
#endif
}

inline fs::path get_executable_dir() {
    return get_executable_path().parent_path();
}

inline fs::path get_vec_extension_path() {
#if defined(_WIN32)
    return get_executable_dir() / "vec0.dll";
#elif defined(__APPLE__)
    return get_executable_dir() / "vec0.dylib";
#else
    return get_executable_dir() / "vec0.so";
#endif
}
} // namespace exe_path_utils
