#include <Windows.h>
#include <TlHelp32.h>
#include <iostream>
#include <string>

DWORD Inject(PCWSTR libFilePath, DWORD pid) {

    auto sz = (lstrlenW(libFilePath) + 1) * sizeof(wchar_t);

    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_CREATE_THREAD |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE,
        FALSE, pid);

    if (!proc) {
        std::cout << "Error: could not open process for " << pid << "\n";
        return 1;
    }

    LPVOID libFilePathRemote =
        VirtualAllocEx(proc, nullptr, sz, MEM_COMMIT, PAGE_READWRITE);

    if (!libFilePathRemote) {
        std::cout << "Error: Could not allocate memory inside " << pid << "\n";
        return 1;
    }

    DWORD write_count = WriteProcessMemory(
        proc, libFilePathRemote, reinterpret_cast<const void*>(libFilePath), sz,
        nullptr);

    if (!write_count) {
        std::cout << "Unable to write bytes into the " << pid << " address space"
            << "\n";
        return 1;
    }

    auto k32_handle = GetModuleHandle(TEXT("Kernel32"));

    if (!k32_handle) {
        std::cout << "Unable to get handle to kernel32.dll"
            << "\n";
        return 1;
    }

    auto loadLibrary = reinterpret_cast<PTHREAD_START_ROUTINE>(
        GetProcAddress(k32_handle, "LoadLibraryW"));

    if (!loadLibrary) {
        std::cout << "Unable to find LoadLibraryW inside \"kernel32.dll\"."
            << "\n";
        return 1;
    }

    auto injectThread = CreateRemoteThread(proc, nullptr, 0, loadLibrary,
        libFilePathRemote, 0, nullptr);

    if (!injectThread) {
        std::cout << "Unable to call LoadLibraryW inside " << pid << "\n";
        return 1;
    }

    WaitForSingleObject(injectThread, INFINITE);

    if (libFilePathRemote) {
        VirtualFreeEx(proc, libFilePathRemote, 0, MEM_RELEASE);
    }

    if (injectThread) {
        CloseHandle(injectThread);
    }

    if (proc) {
        CloseHandle(proc);
    }

    return 0;
}

DWORD GetPid(const std::wstring& name) {
    std::cout << "Looking for the game PID" << "\n";

    PROCESSENTRY32 pentry{ 0 };
    pentry.dwSize = sizeof(PROCESSENTRY32);

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

    if (Process32First(snapshot, &pentry)) {
        while (Process32Next(snapshot, &pentry)) {
            if (!name.compare(pentry.szExeFile)) {
                std::cout << "Found PID for bhd.exe" << "\n";
                CloseHandle(snapshot);
                return pentry.th32ProcessID;
            }
        }
    }

    CloseHandle(snapshot);
    return 0;
}

int main() {
    int pid = GetPid(L"bhd.exe");
    if (!pid) return 0;
    std::cout << "Pid for BHD is: " << pid << "\n";
    std::cout << "Injecting DLL at path C:\\code\\bhd\\bhd\\Debug\\bhd.dll"
        << "\n";
    auto retv = Inject(L"C:\\code\\bhd\\bhd\\Debug\\bhd.dll", pid);
}