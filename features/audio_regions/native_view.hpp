#pragma once

#include <windows.h>
#include <array>
#include <cstdint>
#include <cstring>

namespace regions {
struct NativeViewport {
    double first;
    double end;
};

template<class T>
bool readLocalImage(std::uintptr_t address, T& value) {
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
                             &value, sizeof(value), nullptr) != FALSE;
}

// MEpeaks uses these normalized endpoints for drawing even when its optional
// CoolSB scrollbar is hidden. They are private and must be verified per build.
// Each fingerprint includes the wheel handler's two RIP-relative loads of
// the endpoints followed by its calculation of the visible span.
inline std::uintptr_t findNativeViewport() {
#ifdef _WIN64
    auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"reaper_explorer.dll"));
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!base || !readLocalImage(base,dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < int(sizeof(dos)) || dos.e_lfanew > 4096 ||
        !readLocalImage(base+dos.e_lfanew,nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.OptionalHeader.SizeOfImage != 0x12c000) return 0;
    struct Build {
        DWORD timestamp;
        std::uintptr_t wheelLoads;
        std::array<unsigned char,4> displacement;
    };
    constexpr Build builds[] = {
        {0x6ac2c85e,0x92a2d,{0x5b,0x86,0x07,0x00}}, // Windows x64 7.82
        {0x6ae1f676,0x92e25,{0x63,0x82,0x07,0x00}}, // Windows x64 7.84
    };
    for (const auto& build : builds) {
        if (nt.FileHeader.TimeDateStamp != build.timestamp) continue;
        std::array<unsigned char,25> actual{};
        std::array<unsigned char,25> expected{
            0xf2,0x0f,0x10,0x05,0,0,0,0, 0xf2,0x0f,0x10,0x15,0,0,0,0,
            0xf2,0x0f,0x5c,0xd0, 0xf2,0x41,0x0f,0x59,0xd5
        };
        std::memcpy(expected.data()+4,build.displacement.data(),4);
        std::memcpy(expected.data()+12,build.displacement.data(),4);
        if (readLocalImage(base+build.wheelLoads,actual) && actual == expected)
            return base+0x10b090;
    }
#endif
    return 0;
}
}
