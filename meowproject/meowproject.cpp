#include <Windows.h>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <future>
#include <algorithm>
#include <TlHelp32.h>
#include <iostream>
#include <iomanip>
#include <sstream>

namespace Colors {
    void SetColor(int color) {
        SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), color);
    }

    const int
        RED = 12,
        GREEN = 10,
        BLUE = 9,
        PURPLE = 13,
        YELLOW = 14,
        WHITE = 15,
        CYAN = 11;
}

std::string FormatAddress(uintptr_t address) {
    std::stringstream ss;
    ss << "0x" << std::uppercase << std::setfill('0') << std::setw(16) << std::hex << address;
    return ss.str();
}

struct MemoryRegion {
    uintptr_t base;
    SIZE_T size;
    DWORD state;
    DWORD protect;
    DWORD allocProtect;
};

DWORD GetProcessIDByName(const wchar_t* processName) {
    DWORD processId = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W processEntry;
        processEntry.dwSize = sizeof(processEntry);

        if (Process32FirstW(snapshot, &processEntry)) {
            do {
                if (_wcsicmp(processEntry.szExeFile, processName) == 0) {
                    processId = processEntry.th32ProcessID;
                    break;
                }
            } while (Process32NextW(snapshot, &processEntry));
        }
        CloseHandle(snapshot);
    }
    return processId;
}

uintptr_t GetModuleBaseAddress(DWORD processId, const wchar_t* moduleName) {
    uintptr_t moduleBase = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId);
    if (snapshot != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W moduleEntry;
        moduleEntry.dwSize = sizeof(moduleEntry);

        if (Module32FirstW(snapshot, &moduleEntry)) {
            do {
                if (_wcsicmp(moduleEntry.szModule, moduleName) == 0) {
                    moduleBase = (uintptr_t)moduleEntry.modBaseAddr;
                    break;
                }
            } while (Module32NextW(snapshot, &moduleEntry));
        }
        CloseHandle(snapshot);
    }
    return moduleBase;
}

class Memory {
private:
    HANDLE processHandle;
    uintptr_t moduleBase;

    std::vector<uintptr_t> FindAllPatterns(const std::vector<BYTE>& data, const std::vector<BYTE>& pattern, uintptr_t baseAddress) {
        std::vector<uintptr_t> results;
        size_t patternLength = pattern.size();
        size_t dataLength = data.size();

        for (size_t i = 0; i <= dataLength - patternLength; i++) {
            bool found = true;
            for (size_t j = 0; j < patternLength; j++) {
                if (pattern[j] != 0x00 && pattern[j] != data[i + j]) {
                    found = false;
                    break;
                }
            }
            if (found) {
                results.push_back(baseAddress + i);
            }
        }
        return results;
    }

    uintptr_t FindPattern(const std::vector<BYTE>& data, const std::vector<BYTE>& pattern, uintptr_t baseAddress) {
        auto results = FindAllPatterns(data, pattern, baseAddress);
        return results.empty() ? 0 : results[0];
    }

public:
    Memory(HANDLE handle, uintptr_t base) : processHandle(handle), moduleBase(base) {}

    std::vector<BYTE> StringToPattern(const std::string& str) {
        std::vector<BYTE> pattern;
        for (char c : str) {
            pattern.push_back(static_cast<BYTE>(c));
        }
        return pattern;
    }

    template<typename T>
    T Read(uintptr_t address) {
        T value;
        ReadProcessMemory(processHandle, (LPCVOID)address, &value, sizeof(T), nullptr);
        return value;
    }

    std::vector<uintptr_t> AOBScanAll(const std::string& pattern, bool returnMultiple = false, int stopAtValue = 1) {
        std::vector<uintptr_t> results;
        std::vector<MemoryRegion> regions;
        std::vector<BYTE> bytePattern = StringToPattern(pattern);

        MEMORY_BASIC_INFORMATION mbi;
        uintptr_t address = 0;

        while (VirtualQueryEx(processHandle, (LPCVOID)address, &mbi, sizeof(mbi))) {
            if (mbi.State == MEM_COMMIT &&
                (mbi.Protect == PAGE_EXECUTE_READ ||
                    mbi.Protect == PAGE_EXECUTE_READWRITE ||
                    mbi.Protect == PAGE_READWRITE ||
                    mbi.Protect == PAGE_READONLY))
            {
                regions.push_back({
                    (uintptr_t)mbi.BaseAddress,
                    mbi.RegionSize,
                    mbi.State,
                    mbi.Protect,
                    mbi.AllocationProtect
                    });

                std::cout << "Scanning region at: " << FormatAddress((uintptr_t)mbi.BaseAddress)
                    << " Size: 0x" << std::hex << mbi.RegionSize
                    << " Protect: 0x" << std::hex << mbi.Protect << std::dec << std::endl;
            }
            address = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        }

        std::cout << "Total regions to scan: " << regions.size() << std::endl;

        std::vector<std::future<std::vector<uintptr_t>>> futures;
        std::mutex resultsMutex;

        for (const auto& region : regions) {
            futures.push_back(std::async(std::launch::async, [&, region]() {
                std::vector<BYTE> data(region.size);
                if (ReadProcessMemory(processHandle, (LPCVOID)region.base, data.data(), region.size, nullptr)) {
                    if (returnMultiple) {
                        return FindAllPatterns(data, bytePattern, region.base);
                    }
                    else {
                        uintptr_t result = FindPattern(data, bytePattern, region.base);
                        return result ? std::vector<uintptr_t>{result} : std::vector<uintptr_t>();
                    }
                }
                return std::vector<uintptr_t>();
                }));
        }

        for (auto& future : futures) {
            auto threadResults = future.get();
            if (!threadResults.empty()) {
                std::lock_guard<std::mutex> lock(resultsMutex);
                results.insert(results.end(), threadResults.begin(), threadResults.end());

                if (!returnMultiple || (stopAtValue > 0 && results.size() >= static_cast<size_t>(stopAtValue))) {
                    break;
                }
            }
        }

        std::sort(results.begin(), results.end());
        return results;
    }
};

void SetConsoleFontSize(int fontSize) {
    CONSOLE_FONT_INFOEX cfi;
    cfi.cbSize = sizeof(cfi);
    cfi.nFont = 0;
    cfi.dwFontSize.X = 0;
    cfi.dwFontSize.Y = fontSize;
    cfi.FontFamily = FF_DONTCARE;
    cfi.FontWeight = FW_NORMAL;
    wcscpy_s(cfi.FaceName, L"Courier New");
    SetCurrentConsoleFontEx(GetStdHandle(STD_OUTPUT_HANDLE), FALSE, &cfi);
}

void SetConsoleWindowSize(int width, int height) {
    HWND console = GetConsoleWindow();
    RECT r;
    GetWindowRect(console, &r);

    MoveWindow(console, r.left, r.top, width, height, TRUE);
}


int main() {
    SetConsoleTitle(L"MEOW");
    SetConsoleFontSize(14);

    SetConsoleWindowSize(700, 350);

    Colors::SetColor(Colors::WHITE);
    std::cout << "[";
    Colors::SetColor(Colors::YELLOW);
    std::cout << "init";
    Colors::SetColor(Colors::WHITE);
    std::cout << "] Searching for Roblox process...\n";

    DWORD processId = GetProcessIDByName(L"RobloxPlayerBeta.exe");
    if (processId == 0) {
        Colors::SetColor(Colors::RED);
        std::cout << "[error] Failed to find Roblox process\n";
        Colors::SetColor(Colors::WHITE);
        system("pause");
        return 1;
    }

    Colors::SetColor(Colors::GREEN);
    std::cout << "[success] Found Roblox process ID: " << processId << std::endl;

    Colors::SetColor(Colors::WHITE);
    std::cout << "[";
    Colors::SetColor(Colors::YELLOW);
    std::cout << "init";
    Colors::SetColor(Colors::WHITE);
    std::cout << "] Acquiring process handle...\n";

    HANDLE hProcess = OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION,
        FALSE,
        processId
    );

    if (hProcess == NULL) {
        Colors::SetColor(Colors::RED);
        std::cout << "[error] Failed to open process. Error: " << GetLastError() << std::endl;
        Colors::SetColor(Colors::WHITE);
        system("pause");
        return 1;
    }

    Colors::SetColor(Colors::GREEN);
    std::cout << "[succes] Process handle acquired\n";

    Colors::SetColor(Colors::WHITE);
    std::cout << "[";
    Colors::SetColor(Colors::YELLOW);
    std::cout << "init";
    Colors::SetColor(Colors::WHITE);
    std::cout << "] Getting base address...\n";

    uintptr_t baseAddress = GetModuleBaseAddress(processId, L"RobloxPlayerBeta.exe");
    if (baseAddress == 0) {
        Colors::SetColor(Colors::RED);
        std::cout << "[error] Failed to get base address\n";
        Colors::SetColor(Colors::WHITE);
        CloseHandle(hProcess);
        system("pause");
        return 1;
    }

    Colors::SetColor(Colors::GREEN);
    std::cout << "[success] Base address: " << FormatAddress(baseAddress) << "\n\n";

    Memory mem(hProcess, baseAddress);

    Colors::SetColor(Colors::PURPLE);
    std::cout << "STARTING MEOW SCAN\n";

    Colors::SetColor(Colors::WHITE);

    auto datamodel = mem.AOBScanAll("RenderJob(EarlyRendering;", false, 1);

    if (!datamodel.empty()) {
        Colors::SetColor(Colors::GREEN);
        std::cout << "\n[found] DataModel pattern at: " << FormatAddress(datamodel[0]) << "\n";

        const uintptr_t RENDERVIEW_OFFSET = 0x1E8;
        const uintptr_t FAKE_OFFSET = 0x118;
        const uintptr_t REAL_OFFSET = 0x1A8;

        uintptr_t renderView = mem.Read<uintptr_t>(datamodel[0] + RENDERVIEW_OFFSET);
        Colors::SetColor(Colors::CYAN);
        std::cout << "[read] RenderView address: " << FormatAddress(renderView) << "\n";

        uintptr_t fakeDataModel = mem.Read<uintptr_t>(renderView + FAKE_OFFSET);
        Colors::SetColor(Colors::CYAN);
        std::cout << "[read] FakeDataModel address: " << FormatAddress(fakeDataModel) << "\n";

        uintptr_t realDataModel = mem.Read<uintptr_t>(fakeDataModel + REAL_OFFSET);
        Colors::SetColor(Colors::CYAN);
        std::cout << "[read] RealDataModel address: " << FormatAddress(realDataModel) << "\n";

        Colors::SetColor(Colors::GREEN);
        std::cout << "\n[success] MEOW scan completed successfully!\n";
    }
    else {
        Colors::SetColor(Colors::RED);
        std::cout << "[read] Failed to find DataModel pattern\n";
    }

    Colors::SetColor(Colors::PURPLE);
    std::cout << "\nMEOW COMPLETE\n";
    Colors::SetColor(Colors::WHITE);
    std::cout << "Press Enter to exit...";
    std::cin.get();

    CloseHandle(hProcess);
    return 0;
}