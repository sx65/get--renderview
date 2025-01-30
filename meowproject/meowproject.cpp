#include <Windows.h>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <algorithm>
#include <TlHelp32.h>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <chrono>

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
        const auto startTime = std::chrono::high_resolution_clock::now();

        std::vector<BYTE> bytePattern = StringToPattern(pattern);
        const size_t patternSize = bytePattern.size();
        if (patternSize == 0) return results;

        const BYTE firstByte = bytePattern[0];

        MEMORY_BASIC_INFORMATION mbi;
        uintptr_t address = moduleBase; 

        Colors::SetColor(Colors::YELLOW);
        std::cout << "[scan]";
        Colors::SetColor(Colors::WHITE);
        std::cout << " Starting memory scan...\n";


        while (VirtualQueryEx(processHandle, (LPCVOID)address, &mbi, sizeof(mbi))) {
            if (mbi.State != MEM_COMMIT ||
                !(mbi.Protect == PAGE_EXECUTE_READ ||
                    mbi.Protect == PAGE_EXECUTE_READWRITE ||
                    mbi.Protect == PAGE_READWRITE ||
                    mbi.Protect == PAGE_READONLY)) {
                address = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
                continue;
            }

            std::vector<BYTE> buffer(mbi.RegionSize);
            SIZE_T bytesRead;

            if (!ReadProcessMemory(processHandle, (LPCVOID)mbi.BaseAddress, buffer.data(), mbi.RegionSize, &bytesRead)) {
                address = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
                continue;
            }

            for (size_t i = 0; i <= bytesRead - patternSize; i++) {
                if (buffer[i] != firstByte) continue;

                bool found = true;
                for (size_t j = 1; j < patternSize; j++) {
                    if (bytePattern[j] != 0x00 && bytePattern[j] != buffer[i + j]) {
                        found = false;
                        break;
                    }
                }

                if (found) {
                    results.push_back((uintptr_t)mbi.BaseAddress + i);
                    if (!returnMultiple || (stopAtValue > 0 && results.size() >= static_cast<size_t>(stopAtValue))) {
                        goto SCAN_COMPLETE;
                    }
                }
            }

            address = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        }

    SCAN_COMPLETE:
        const auto endTime = std::chrono::high_resolution_clock::now();
        const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);

        Colors::SetColor(Colors::GREEN);
        std::cout << "[time]";
        Colors::SetColor(Colors::WHITE);
        std::cout << " Scan completed in " << duration.count() / 1000.0 << "ms ("
            << std::fixed << std::setprecision(3) << (duration.count() / 1000000.0) << "s)\n";


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

    Colors::SetColor(Colors::YELLOW);
    std::cout << "[init]";
    Colors::SetColor(Colors::WHITE);
    std::cout << " Searching for Roblox process...\n";

    DWORD processId = GetProcessIDByName(L"RobloxPlayerBeta.exe");
    if (processId == 0) {
        Colors::SetColor(Colors::RED);
        std::cout << "[error]";
        Colors::SetColor(Colors::WHITE);
        std::cout << " Failed to find Roblox process\n";
        system("pause");
        return 1;
    }

    Colors::SetColor(Colors::GREEN);
    std::cout << "[success]";
    Colors::SetColor(Colors::WHITE);
    std::cout << " Found Roblox process ID: " << processId << std::endl;

    Colors::SetColor(Colors::YELLOW);
    std::cout << "[init]";
    Colors::SetColor(Colors::WHITE);
    std::cout << " Acquiring process handle...\n";

    HANDLE hProcess = OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION,
        FALSE,
        processId
    );

    if (hProcess == NULL) {
        Colors::SetColor(Colors::RED);
        std::cout << "[error]";
        Colors::SetColor(Colors::WHITE);
        std::cout << " Failed to open process\n";
        system("pause");
        return 1;
    }

    Colors::SetColor(Colors::GREEN);
    std::cout << "[success]";
    Colors::SetColor(Colors::WHITE);
    std::cout << " Process handle acquired\n";

    Colors::SetColor(Colors::YELLOW);
    std::cout << "[init]";
    Colors::SetColor(Colors::WHITE);
    std::cout << " Getting base address...\n";

    uintptr_t baseAddress = GetModuleBaseAddress(processId, L"RobloxPlayerBeta.exe");
    if (baseAddress == 0) {
        Colors::SetColor(Colors::RED);
        std::cout << "[error]";
        Colors::SetColor(Colors::WHITE);
        std::cout << " Failed to get base address\n";
        CloseHandle(hProcess);
        system("pause");
        return 1;
    }

    Colors::SetColor(Colors::GREEN);
    std::cout << "[success]";
    Colors::SetColor(Colors::WHITE);
    std::cout << " Base address: " << FormatAddress(baseAddress) << "\n\n";

    Memory mem(hProcess, baseAddress);

    Colors::SetColor(Colors::PURPLE);
    std::cout << "[MEOW]";
    Colors::SetColor(Colors::WHITE);
    std::cout << " Starting scan...\n";

    const auto totalStartTime = std::chrono::high_resolution_clock::now();
    auto datamodel = mem.AOBScanAll("RenderJob(EarlyRendering;", false, 1);

    if (!datamodel.empty()) {
        Colors::SetColor(Colors::GREEN);
        std::cout << "\n[found]";
        Colors::SetColor(Colors::WHITE);
        std::cout << " DataModel pattern at: " << FormatAddress(datamodel[0]) << "\n";

        const uintptr_t RENDERVIEW_OFFSET = 0x1E8;
        const uintptr_t FAKE_OFFSET = 0x118;
        const uintptr_t REAL_OFFSET = 0x1A8;

        uintptr_t renderView = mem.Read<uintptr_t>(datamodel[0] + RENDERVIEW_OFFSET);
        Colors::SetColor(Colors::CYAN);
        std::cout << "[read]";
        Colors::SetColor(Colors::WHITE);
        std::cout << " RenderView address: " << FormatAddress(renderView) << "\n";

        uintptr_t fakeDataModel = mem.Read<uintptr_t>(renderView + FAKE_OFFSET);
        Colors::SetColor(Colors::CYAN);
        std::cout << "[read]";
        Colors::SetColor(Colors::WHITE);
        std::cout << " FakeDataModel address: " << FormatAddress(fakeDataModel) << "\n";

        uintptr_t realDataModel = mem.Read<uintptr_t>(fakeDataModel + REAL_OFFSET);
        Colors::SetColor(Colors::CYAN);
        std::cout << "[read]";
        Colors::SetColor(Colors::WHITE);
        std::cout << " RealDataModel address: " << FormatAddress(realDataModel) << "\n";

        const auto totalEndTime = std::chrono::high_resolution_clock::now();
        const auto totalDuration = std::chrono::duration_cast<std::chrono::microseconds>(totalEndTime - totalStartTime);

        Colors::SetColor(Colors::GREEN);
        std::cout << "\n[success]";
        Colors::SetColor(Colors::WHITE);
        std::cout << " Scan completed successfully!\n";

        Colors::SetColor(Colors::YELLOW);
        std::cout << "[time]";
        Colors::SetColor(Colors::WHITE);
        std::cout << " Operation took " << totalDuration.count() / 1000.0 << "ms ("
            << std::fixed << std::setprecision(3) << (totalDuration.count() / 1000000.0) << "s)\n";
    }
    else {
        Colors::SetColor(Colors::RED);
        std::cout << "[error]";
        Colors::SetColor(Colors::WHITE);
        std::cout << " Failed to find DataModel pattern\n";
    }

    Colors::SetColor(Colors::PURPLE);
    std::cout << "\n[MEOW]";
    Colors::SetColor(Colors::WHITE);
    std::cout << " Scan complete!\n";

    std::cout << "\nPress Enter to exit...";
    std::cin.get();

    CloseHandle(hProcess);
    return 0;
}
