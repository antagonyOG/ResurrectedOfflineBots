#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace
{
    constexpr DWORD kProcessAccess =
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE;

    std::filesystem::path g_Root;
    std::filesystem::path g_DllPath;
    std::filesystem::path g_StatusPath;
    DWORD g_Pid = 0;
    uintptr_t g_RemoteDll = 0;

    void WriteStatus(const std::string& text)
    {
        std::ofstream output(g_StatusPath, std::ios::trunc);
        output << text << "\r\n";
    }

    void PrintStatus(const std::string& text)
    {
        std::cout << text << std::endl;
        WriteStatus(text);
    }

    DWORD FindGameProcess()
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return 0;

        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        DWORD result = 0;
        if (Process32FirstW(snapshot, &entry))
        {
            do
            {
                const std::wstring name(entry.szExeFile);
                const bool match =
                    _wcsicmp(name.c_str(), L"SummerCamp-Win64-Shipping.exe") == 0 ||
                    _wcsicmp(name.c_str(), L"SummercampShipping.exe") == 0 ||
                    (name.find(L"SummerCamp-Win64-Shipping") != std::wstring::npos &&
                     name.find(L"OfflineBots") == std::wstring::npos);
                if (match)
                {
                    result = entry.th32ProcessID;
                    break;
                }
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        return result;
    }

    uintptr_t FindModule(DWORD pid, const std::wstring& wanted)
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snapshot == INVALID_HANDLE_VALUE)
            return 0;

        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        uintptr_t result = 0;
        if (Module32FirstW(snapshot, &entry))
        {
            do
            {
                if (_wcsicmp(entry.szModule, wanted.c_str()) == 0)
                {
                    result = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
                    break;
                }
            } while (Module32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        return result;
    }

    bool Inject(DWORD pid)
    {
        const std::wstring dllName = g_DllPath.filename().wstring();
        if (const auto loaded = FindModule(pid, dllName))
        {
            g_RemoteDll = loaded;
            return true;
        }

        HANDLE process = OpenProcess(kProcessAccess, FALSE, pid);
        if (!process)
            return false;

        const std::wstring path = g_DllPath.wstring();
        const SIZE_T bytes = (path.size() + 1) * sizeof(wchar_t);
        void* remote = VirtualAllocEx(
            process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!remote)
        {
            CloseHandle(process);
            return false;
        }

        SIZE_T written = 0;
        const bool wrote = WriteProcessMemory(
            process, remote, path.c_str(), bytes, &written) != FALSE;
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        auto loadLibrary = kernel
            ? GetProcAddress(kernel, "LoadLibraryW")
            : nullptr;
        HANDLE thread = wrote && loadLibrary
            ? CreateRemoteThread(
                process,
                nullptr,
                0,
                reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibrary),
                remote,
                0,
                nullptr)
            : nullptr;

        if (thread)
        {
            WaitForSingleObject(thread, INFINITE);
            CloseHandle(thread);
        }
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);

        if (!thread)
            return false;

        for (int i = 0; i < 50; ++i)
        {
            if (const auto loaded = FindModule(pid, dllName))
            {
                g_RemoteDll = loaded;
                return true;
            }
            Sleep(100);
        }
        return false;
    }

    uintptr_t ExportRva(const char* name)
    {
        HMODULE local = LoadLibraryExW(
            g_DllPath.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
        if (!local)
            return 0;
        FARPROC address = GetProcAddress(local, name);
        const uintptr_t rva = address
            ? reinterpret_cast<uintptr_t>(address) -
                reinterpret_cast<uintptr_t>(local)
            : 0;
        FreeLibrary(local);
        return rva;
    }

    DWORD CallExport(const char* name)
    {
        const uintptr_t rva = ExportRva(name);
        if (!rva || !g_RemoteDll || !g_Pid)
            return 0;

        HANDLE process = OpenProcess(
            PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION,
            FALSE,
            g_Pid);
        if (!process)
            return 0;

        HANDLE thread = CreateRemoteThread(
            process,
            nullptr,
            0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(g_RemoteDll + rva),
            nullptr,
            0,
            nullptr);
        if (!thread)
        {
            CloseHandle(process);
            return 0;
        }

        WaitForSingleObject(thread, INFINITE);
        DWORD result = 0;
        GetExitCodeThread(thread, &result);
        CloseHandle(thread);
        CloseHandle(process);
        return result;
    }

    bool KeyPressedEdge(int virtualKey, bool& wasDown)
    {
        const bool down = (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
        const bool pressed = down && !wasDown;
        wasDown = down;
        return pressed;
    }
}

int wmain()
{
    wchar_t modulePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    g_Root = std::filesystem::path(modulePath).parent_path();
    g_DllPath = g_Root / L"ResurrectedOfflineBots.dll";
    g_StatusPath = g_Root / L"lite-launcher-status.txt";

    std::cout << "ResurrectedOfflineBots Lite - Sandbox AI\n"
              << "---------------------------------------\n"
              << "1. Start Resurrected through Steam.\n"
              << "2. Open Offline Play -> Sandbox.\n"
              << "3. Choose any map and let the world/UI finish loading.\n"
              << "4. Keep this window open while you play.\n"
              << "5. Press F1 to request AI Jason.\n"
              << "6. Press F3 to request an AI counselor.\n\n";

    if (GetFileAttributesW(g_DllPath.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        PrintStatus("ERROR: ResurrectedOfflineBots.dll is missing beside the EXE");
        return 1;
    }

    g_Pid = FindGameProcess();
    if (!g_Pid || !Inject(g_Pid))
    {
        PrintStatus("ERROR: start Resurrected and fully load Sandbox before running this EXE");
        return 1;
    }

    PrintStatus("OK: Lite AI attached; F1 = AI Jason, F3 = AI counselor");
    bool f1WasDown = false;
    bool f3WasDown = false;

    for (;;)
    {
        if (KeyPressedEdge(VK_F1, f1WasDown))
            PrintStatus(CallExport("ROB_QueueJason")
                ? "OK: F1 Jason request queued; wait for him to appear before pressing F3"
                : "ERROR: F1 Jason request rejected/not ready; see backend log");

        if (KeyPressedEdge(VK_F3, f3WasDown))
            PrintStatus(CallExport("ROB_QueueCounselor")
                ? "OK: F3 counselor request queued"
                : "WAITING: Jason is not active yet; do not press F1 again, wait and retry F3");

        const DWORD currentPid = FindGameProcess();
        if (!currentPid)
        {
            PrintStatus("WAITING: Resurrected is closed");
            g_Pid = 0;
            g_RemoteDll = 0;
        }
        else if (currentPid != g_Pid)
        {
            g_Pid = currentPid;
            g_RemoteDll = 0;
            if (Inject(g_Pid))
                PrintStatus("OK: Lite AI reattached; F1 = AI Jason, F3 = AI counselor");
        }

        Sleep(50);
    }
}
