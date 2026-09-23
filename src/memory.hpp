#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <unordered_map>
#include <optional>

class Process {
public:
    HANDLE handle = INVALID_HANDLE_VALUE;
    DWORD  pid    = 0;
    std::unordered_map<std::string, uintptr_t> modules;

    Process() = default;
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    Process(Process&& o) noexcept
        : handle(o.handle), pid(o.pid), modules(std::move(o.modules)) {
        o.handle = INVALID_HANDLE_VALUE;
    }

    Process& operator=(Process&& o) noexcept {
        if (this != &o) {
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
            handle  = o.handle;
            pid     = o.pid;
            modules = std::move(o.modules);
            o.handle = INVALID_HANDLE_VALUE;
        }
        return *this;
    }

    ~Process() {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }

    static std::optional<Process> open(const char* exeName);

    std::optional<uintptr_t> moduleBase(const char* name) const {
        auto it = modules.find(name);
        return it != modules.end() ? std::optional<uintptr_t>(it->second) : std::nullopt;
    }

    template<typename T>
    std::optional<T> read(uintptr_t addr) const {
        T buf{};
        SIZE_T n = 0;
        if (!ReadProcessMemory(handle, reinterpret_cast<LPCVOID>(addr), &buf, sizeof(T), &n)
            || n != sizeof(T))
            return std::nullopt;
        return buf;
    }

    template<typename T>
    bool write(uintptr_t addr, const T& val) const {
        SIZE_T n = 0;
        return WriteProcessMemory(handle, reinterpret_cast<LPVOID>(addr), &val, sizeof(T), &n) && n == sizeof(T);
    }

    // Bulk read into a caller-supplied buffer.  Returns bytes actually read (0 on failure).
    SIZE_T readRaw(uintptr_t addr, void* buf, SIZE_T size) const {
        SIZE_T n = 0;
        ReadProcessMemory(handle, reinterpret_cast<LPCVOID>(addr), buf, size, &n);
        return n;
    }
};

inline std::optional<Process> Process::open(const char* exeName) {
    DWORD pid = 0;
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return std::nullopt;
        PROCESSENTRY32 pe{};
        pe.dwSize = sizeof(pe);
        if (Process32First(snap, &pe)) {
            do {
                if (_stricmp(pe.szExeFile, exeName) == 0) {
                    pid = pe.th32ProcessID;
                    break;
                }
            } while (Process32Next(snap, &pe));
        }
        CloseHandle(snap);
    }
    if (pid == 0) return std::nullopt;

    HANDLE h = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h || h == INVALID_HANDLE_VALUE) return std::nullopt;

    Process p;
    p.handle = h;
    p.pid    = pid;

    {
        HANDLE msnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (msnap != INVALID_HANDLE_VALUE) {
            MODULEENTRY32 me{};
            me.dwSize = sizeof(me);
            if (Module32First(msnap, &me)) {
                do {
                    p.modules[me.szModule] = reinterpret_cast<uintptr_t>(me.modBaseAddr);
                } while (Module32Next(msnap, &me));
            }
            CloseHandle(msnap);
        }
    }

    return p;
}
