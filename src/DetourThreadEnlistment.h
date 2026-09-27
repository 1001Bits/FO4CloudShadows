// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <Windows.h>
#include <TlHelp32.h>
#include <detours.h>
#include <vector>

namespace FO4CS
{
    // Collect handles before suspension so vector allocation and enumeration
    // cannot wait on a heap lock held by an already-suspended thread.
    class DetourThreadEnlistment
    {
    public:
        ~DetourThreadEnlistment()
        {
            for (HANDLE thread : threads_)
                CloseHandle(thread);
        }

        DetourThreadEnlistment(const DetourThreadEnlistment&) = delete;
        DetourThreadEnlistment& operator=(const DetourThreadEnlistment&) = delete;
        DetourThreadEnlistment() = default;

        LONG EnlistProcessThreads() noexcept
        {
            LONG error = DetourUpdateThread(GetCurrentThread());
            if (error != NO_ERROR)
                return error;
            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snapshot == INVALID_HANDLE_VALUE)
                return static_cast<LONG>(GetLastError());
            THREADENTRY32 entry{};
            entry.dwSize = sizeof(entry);
            if (!Thread32First(snapshot, &entry)) {
                error = static_cast<LONG>(GetLastError());
                CloseHandle(snapshot);
                return error;
            }
            const DWORD processId = GetCurrentProcessId();
            const DWORD currentThreadId = GetCurrentThreadId();
            do {
                if (entry.th32OwnerProcessID != processId ||
                    entry.th32ThreadID == currentThreadId)
                    continue;
                HANDLE thread = OpenThread(
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                        THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION,
                    FALSE, entry.th32ThreadID);
                if (!thread) {
                    const DWORD openError = GetLastError();
                    if (openError == ERROR_INVALID_PARAMETER)
                        continue;
                    error = static_cast<LONG>(openError);
                    break;
                }
                threads_.push_back(thread);
            } while (Thread32Next(snapshot, &entry));
            if (error == NO_ERROR) {
                const DWORD enumerationError = GetLastError();
                if (enumerationError != ERROR_NO_MORE_FILES)
                    error = static_cast<LONG>(enumerationError);
            }
            CloseHandle(snapshot);
            if (error != NO_ERROR)
                return error;
            for (HANDLE thread : threads_) {
                error = DetourUpdateThread(thread);
                if (error != NO_ERROR)
                    return error;
            }
            return error;
        }

    private:
        std::vector<HANDLE> threads_;
    };
}
