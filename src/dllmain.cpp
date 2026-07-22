#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::array<std::uint8_t, 12> kHookSignature{
    0xF3, 0x0F, 0x10, 0x4A, 0x18, // movss xmm1,[rdx+18]
    0x0F, 0x2F, 0x89, 0x84, 0x32, 0x01, 0x00
};
constexpr std::array<std::uint8_t, 5> kOriginalInstruction{
    0xF3, 0x0F, 0x10, 0x4A, 0x18
};
// CSteamLeaderboardRequest_Update creation/enqueue function. The relative call
// displacement is wildcarded by kScoreUploadBlockMask.
constexpr std::array<std::uint8_t, 52> kScoreUploadBlockSignature{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x55, 0x56, 0x57,
    0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xDA, 0x48,
    0x8B, 0xE9, 0xB9, 0x40, 0x00, 0x00, 0x00, 0xE8,
    0x00, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xF0, 0x48,
    0x89, 0x44, 0x24, 0x60, 0x0F, 0x57, 0xC0, 0x0F,
    0x11, 0x00, 0x0F, 0x11, 0x40, 0x10, 0x0F, 0x11,
    0x40, 0x20, 0x0F, 0x11
};
constexpr std::string_view kScoreUploadBlockMask{
    "xxxxxxxxxxxxxxxxxxxxxxxx????xxxxxxxxxxxxxxxxxxxxxxxx"
};
constexpr std::uint8_t kScoreUploadOriginalByte = 0x48;
constexpr std::uint8_t kReturnInstruction = 0xC3;
constexpr std::uint32_t kFinePositiveOffset = 0x13274;
constexpr std::uint32_t kFineNegativeOffset = 0x13278;
constexpr float kDefaultFineWindowMs = 100.0F;
constexpr float kMinimumFineWindowMs = 30.0F;
constexpr float kMaximumFineWindowMs = 130.0F;

HMODULE g_module{};
std::uint8_t* g_hookAddress{};
std::uint8_t* g_codeCave{};
std::size_t g_codeCaveSize{};
bool g_hookInstalled{};
std::uint8_t* g_scoreUploadAddress{};
bool g_scoreUploadBlockInstalled{};
std::filesystem::path g_logPath;

struct Config {
    bool enabled{true};
    float fineWindowMs{kDefaultFineWindowMs};
};

std::string Trim(std::string value) {
    const auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

void Log(std::string_view message) {
    if (g_logPath.empty()) {
        return;
    }
    std::ofstream stream(g_logPath, std::ios::app);
    if (!stream) {
        return;
    }
    SYSTEMTIME time{};
    GetLocalTime(&time);
    stream << '[' << std::setfill('0') << std::setw(2) << time.wHour << ':'
           << std::setw(2) << time.wMinute << ':' << std::setw(2) << time.wSecond
           << "] " << message << '\n';
}

std::filesystem::path ModuleDirectory(HMODULE module) {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(module, buffer.data(),
                                             static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}

Config ReadConfig(const std::filesystem::path& path) {
    Config config;
    std::ifstream stream(path);
    if (!stream) {
        Log("config.toml not found; using defaults");
        return config;
    }

    std::string line;
    while (std::getline(stream, line)) {
        if (const auto comment = line.find('#'); comment != std::string::npos) {
            line.erase(comment);
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::string key = Lower(Trim(line.substr(0, equals)));
        std::string value = Lower(Trim(line.substr(equals + 1)));

        if (key == "enabled") {
            config.enabled = value != "false";
        } else if (key == "fine_window_ms") {
            try {
                config.fineWindowMs = std::stof(value);
            } catch (...) {
                Log("Invalid fine_window_ms; using default 100 ms");
                config.fineWindowMs = kDefaultFineWindowMs;
            }
        }
    }

    if (!std::isfinite(config.fineWindowMs) ||
        config.fineWindowMs < kMinimumFineWindowMs ||
        config.fineWindowMs > kMaximumFineWindowMs) {
        Log("fine_window_ms must be between 30 and 130; using default 100 ms");
        config.fineWindowMs = kDefaultFineWindowMs;
    }
    return config;
}

std::uint8_t* FindSignature(HMODULE image) {
    auto* const base = reinterpret_cast<std::uint8_t*>(image);
    const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return nullptr;
    }
    const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return nullptr;
    }

    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned index = 0; index < nt->FileHeader.NumberOfSections; ++index, ++section) {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
            continue;
        }
        auto* const begin = base + section->VirtualAddress;
        const std::size_t size = std::max<std::size_t>(section->Misc.VirtualSize,
                                                       section->SizeOfRawData);
        if (size < kHookSignature.size()) {
            continue;
        }
        auto* const end = begin + size;
        const auto match = std::search(begin, end,
                                       kHookSignature.begin(), kHookSignature.end());
        if (match != end) {
            return match;
        }
    }
    return nullptr;
}

std::uint8_t* FindUniqueMaskedSignature(HMODULE image,
                                        const std::uint8_t* signature,
                                        std::string_view mask) {
    if (!image || !signature || mask.empty()) {
        return nullptr;
    }

    auto* const base = reinterpret_cast<std::uint8_t*>(image);
    const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return nullptr;
    }
    const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return nullptr;
    }

    std::uint8_t* result{};
    std::size_t matches{};
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned index = 0; index < nt->FileHeader.NumberOfSections; ++index, ++section) {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
            continue;
        }
        auto* const begin = base + section->VirtualAddress;
        const std::size_t size = std::max<std::size_t>(section->Misc.VirtualSize,
                                                       section->SizeOfRawData);
        if (size < mask.size()) {
            continue;
        }

        for (std::size_t offset = 0; offset <= size - mask.size(); ++offset) {
            bool matched = true;
            for (std::size_t byte = 0; byte < mask.size(); ++byte) {
                if (mask[byte] != '?' && begin[offset + byte] != signature[byte]) {
                    matched = false;
                    break;
                }
            }
            if (matched) {
                result = begin + offset;
                if (++matches > 1) {
                    return nullptr;
                }
            }
        }
    }
    return matches == 1 ? result : nullptr;
}

bool WriteExecutableByte(std::uint8_t* address, std::uint8_t value) {
    DWORD oldProtection{};
    if (!VirtualProtect(address, 1, PAGE_EXECUTE_READWRITE, &oldProtection)) {
        return false;
    }
    *address = value;
    FlushInstructionCache(GetCurrentProcess(), address, 1);
    DWORD ignored{};
    VirtualProtect(address, 1, oldProtection, &ignored);
    return true;
}

bool InstallScoreUploadBlock() {
    HMODULE executable = GetModuleHandleW(nullptr);
    if (!executable) {
        Log("Score upload block: GetModuleHandleW(nullptr) failed");
        return false;
    }

    static_assert(kScoreUploadBlockSignature.size() == kScoreUploadBlockMask.size());
    g_scoreUploadAddress = FindUniqueMaskedSignature(
        executable, kScoreUploadBlockSignature.data(), kScoreUploadBlockMask);
    if (!g_scoreUploadAddress) {
        Log("Score upload block signature was not found uniquely; leaving uploads unchanged");
        return false;
    }
    if (*g_scoreUploadAddress != kScoreUploadOriginalByte) {
        Log("Score upload block site does not contain the expected original byte");
        g_scoreUploadAddress = nullptr;
        return false;
    }
    if (!WriteExecutableByte(g_scoreUploadAddress, kReturnInstruction)) {
        Log("Score upload block: VirtualProtect failed");
        g_scoreUploadAddress = nullptr;
        return false;
    }

    g_scoreUploadBlockInstalled = true;
    std::ostringstream message;
    message << "Blocked leaderboard score uploads at RVA 0x" << std::hex
            << (g_scoreUploadAddress - reinterpret_cast<std::uint8_t*>(executable));
    Log(message.str());
    return true;
}

bool RemoveScoreUploadBlock() {
    if (!g_scoreUploadBlockInstalled || !g_scoreUploadAddress) {
        return true;
    }
    if (!WriteExecutableByte(g_scoreUploadAddress, kScoreUploadOriginalByte)) {
        return false;
    }
    g_scoreUploadAddress = nullptr;
    g_scoreUploadBlockInstalled = false;
    Log("Score upload block removed");
    return true;
}

bool FitsRelativeJump(const std::uint8_t* instruction,
                      const std::uint8_t* destination) {
    const auto difference = reinterpret_cast<std::intptr_t>(destination) -
                            (reinterpret_cast<std::intptr_t>(instruction) + 5);
    return difference >= std::numeric_limits<std::int32_t>::min() &&
           difference <= std::numeric_limits<std::int32_t>::max();
}

std::uint8_t* AllocateNear(std::uint8_t* target, std::size_t size) {
    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    const auto granularity = static_cast<std::uintptr_t>(systemInfo.dwAllocationGranularity);
    const auto targetAddress = reinterpret_cast<std::uintptr_t>(target);
    const auto alignedTarget = targetAddress & ~(granularity - 1);
    constexpr std::uintptr_t kMaximumDistance = 0x70000000ULL;

    for (std::uintptr_t distance = granularity;
         distance <= kMaximumDistance;
         distance += granularity) {
        const std::array<std::uintptr_t, 2> candidates{
            alignedTarget >= distance ? alignedTarget - distance : 0,
            alignedTarget + distance
        };
        for (const auto candidate : candidates) {
            if (candidate < 0x10000) {
                continue;
            }
            MEMORY_BASIC_INFORMATION information{};
            if (VirtualQuery(reinterpret_cast<void*>(candidate), &information,
                             sizeof(information)) == 0 ||
                information.State != MEM_FREE) {
                continue;
            }
            auto* allocation = static_cast<std::uint8_t*>(VirtualAlloc(
                reinterpret_cast<void*>(candidate), size,
                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            if (allocation && FitsRelativeJump(target, allocation)) {
                return allocation;
            }
            if (allocation) {
                VirtualFree(allocation, 0, MEM_RELEASE);
            }
        }
    }
    return nullptr;
}

void Append(std::vector<std::uint8_t>& code,
            std::initializer_list<std::uint8_t> bytes) {
    code.insert(code.end(), bytes.begin(), bytes.end());
}

void WriteI32(std::vector<std::uint8_t>& code, std::size_t offset,
              std::int32_t value) {
    std::memcpy(code.data() + offset, &value, sizeof(value));
}

std::optional<std::int32_t> Relative32(const std::uint8_t* instructionEnd,
                                       const std::uint8_t* destination) {
    const auto difference = reinterpret_cast<std::intptr_t>(destination) -
                            reinterpret_cast<std::intptr_t>(instructionEnd);
    if (difference < std::numeric_limits<std::int32_t>::min() ||
        difference > std::numeric_limits<std::int32_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(difference);
}

bool InstallHook(float fineWindowMs) {
    HMODULE executable = GetModuleHandleW(nullptr);
    if (!executable) {
        Log("GetModuleHandleW(nullptr) failed");
        return false;
    }

    g_hookAddress = FindSignature(executable);
    if (!g_hookAddress) {
        Log("Hook signature not found; this game version is unsupported");
        return false;
    }
    if (!std::equal(kOriginalInstruction.begin(), kOriginalInstruction.end(),
                    g_hookAddress)) {
        Log("Hook site does not contain the expected original instruction");
        return false;
    }

    constexpr std::size_t kAllocationSize = 256;
    g_codeCave = AllocateNear(g_hookAddress, kAllocationSize);
    if (!g_codeCave) {
        Log("Could not allocate executable memory within rel32 range");
        return false;
    }
    g_codeCaveSize = kAllocationSize;

    std::vector<std::uint8_t> code;
    code.reserve(64);

    // movss xmm1,[rip+positive]
    Append(code, {0xF3, 0x0F, 0x10, 0x0D, 0, 0, 0, 0});
    const std::size_t positiveDisplacement = 4;
    // movss [rcx+13274],xmm1
    Append(code, {0xF3, 0x0F, 0x11, 0x89, 0x74, 0x32, 0x01, 0x00});
    // movss xmm1,[rip+negative]
    const std::size_t negativeLoadStart = code.size();
    Append(code, {0xF3, 0x0F, 0x10, 0x0D, 0, 0, 0, 0});
    const std::size_t negativeDisplacement = negativeLoadStart + 4;
    // movss [rcx+13278],xmm1
    Append(code, {0xF3, 0x0F, 0x11, 0x89, 0x78, 0x32, 0x01, 0x00});
    // Replay overwritten instruction: movss xmm1,[rdx+18]
    code.insert(code.end(), kOriginalInstruction.begin(), kOriginalInstruction.end());
    // jmp hook+5
    const std::size_t returnJumpStart = code.size();
    Append(code, {0xE9, 0, 0, 0, 0});
    while ((code.size() & 3U) != 0) {
        code.push_back(0x90);
    }

    const std::size_t positiveOffset = code.size();
    const float positiveSeconds = fineWindowMs / 1000.0F;
    const auto* positiveBytes = reinterpret_cast<const std::uint8_t*>(&positiveSeconds);
    code.insert(code.end(), positiveBytes, positiveBytes + sizeof(float));

    const std::size_t negativeOffset = code.size();
    const float negativeSeconds = -positiveSeconds;
    const auto* negativeBytes = reinterpret_cast<const std::uint8_t*>(&negativeSeconds);
    code.insert(code.end(), negativeBytes, negativeBytes + sizeof(float));

    const auto positiveRel = Relative32(g_codeCave + 8,
                                        g_codeCave + positiveOffset);
    const auto negativeRel = Relative32(g_codeCave + negativeLoadStart + 8,
                                        g_codeCave + negativeOffset);
    const auto returnRel = Relative32(g_codeCave + returnJumpStart + 5,
                                      g_hookAddress + kOriginalInstruction.size());
    const auto hookRel = Relative32(g_hookAddress + 5, g_codeCave);
    if (!positiveRel || !negativeRel || !returnRel || !hookRel) {
        Log("A generated relative displacement was out of range");
        VirtualFree(g_codeCave, 0, MEM_RELEASE);
        g_codeCave = nullptr;
        return false;
    }

    WriteI32(code, positiveDisplacement, *positiveRel);
    WriteI32(code, negativeDisplacement, *negativeRel);
    WriteI32(code, returnJumpStart + 1, *returnRel);
    std::memcpy(g_codeCave, code.data(), code.size());

    DWORD oldCaveProtection{};
    if (!VirtualProtect(g_codeCave, g_codeCaveSize, PAGE_EXECUTE_READ,
                        &oldCaveProtection)) {
        Log("VirtualProtect on code cave failed");
        VirtualFree(g_codeCave, 0, MEM_RELEASE);
        g_codeCave = nullptr;
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), g_codeCave, code.size());

    std::array<std::uint8_t, 5> patch{0xE9, 0, 0, 0, 0};
    std::memcpy(patch.data() + 1, &*hookRel, sizeof(std::int32_t));
    DWORD oldProtection{};
    if (!VirtualProtect(g_hookAddress, patch.size(), PAGE_EXECUTE_READWRITE,
                        &oldProtection)) {
        Log("VirtualProtect on hook site failed");
        VirtualFree(g_codeCave, 0, MEM_RELEASE);
        g_codeCave = nullptr;
        return false;
    }
    std::memcpy(g_hookAddress, patch.data(), patch.size());
    FlushInstructionCache(GetCurrentProcess(), g_hookAddress, patch.size());
    DWORD ignored{};
    VirtualProtect(g_hookAddress, patch.size(), oldProtection, &ignored);

    g_hookInstalled = true;
    std::ostringstream message;
    message << "Installed FINE window +/- " << fineWindowMs
            << " ms at RVA 0x" << std::hex
            << (g_hookAddress - reinterpret_cast<std::uint8_t*>(executable));
    Log(message.str());
    return true;
}

bool RemoveHook() {
    if (!g_hookInstalled || !g_hookAddress) {
        return true;
    }
    DWORD oldProtection{};
    if (!VirtualProtect(g_hookAddress, kOriginalInstruction.size(),
                        PAGE_EXECUTE_READWRITE, &oldProtection)) {
        return false;
    }
    std::memcpy(g_hookAddress, kOriginalInstruction.data(),
                kOriginalInstruction.size());
    FlushInstructionCache(GetCurrentProcess(), g_hookAddress,
                          kOriginalInstruction.size());
    DWORD ignored{};
    VirtualProtect(g_hookAddress, kOriginalInstruction.size(), oldProtection, &ignored);
    if (g_codeCave) {
        VirtualFree(g_codeCave, 0, MEM_RELEASE);
    }
    g_codeCave = nullptr;
    g_hookAddress = nullptr;
    g_hookInstalled = false;
    Log("Hook removed");
    return true;
}

DWORD WINAPI Initialize(void*) {
    const auto directory = ModuleDirectory(g_module);
    g_logPath = directory / L"DivaFineWindow.log";
    std::ofstream(g_logPath, std::ios::trunc) << "DivaFineWindow startup\n";

    const Config config = ReadConfig(directory / L"config.toml");
    if (!config.enabled) {
        Log("Mod disabled by config.toml");
        return 0;
    }
    InstallHook(config.fineWindowMs);
    InstallScoreUploadBlock();
    return 0;
}

} // namespace

extern "C" __declspec(dllexport) BOOL WINAPI DisableDivaFineWindow() {
    const bool scoreUploadRestored = RemoveScoreUploadBlock();
    const bool fineWindowRestored = RemoveHook();
    return scoreUploadRestored && fineWindowRestored ? TRUE : FALSE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
        if (HANDLE thread = CreateThread(nullptr, 0, Initialize, nullptr, 0, nullptr)) {
            CloseHandle(thread);
        }
    }
    return TRUE;
}
