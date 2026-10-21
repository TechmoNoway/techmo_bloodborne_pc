/* Out-of-process minidump writer. Invoked only by win32_diagnostics.c. */
#include <windows.h>
#include <dbghelp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "engine/runtime/win32_diagnostics.h"

int main(int argc, char **argv) {
    if (argc != 5) return 2;
    HANDLE mapping = (HANDLE)(uintptr_t)strtoull(argv[1], NULL, 10);
    HANDLE ready = (HANDLE)(uintptr_t)strtoull(argv[2], NULL, 10);
    HANDLE requested = (HANDLE)(uintptr_t)strtoull(argv[3], NULL, 10);
    HANDLE finished = (HANDLE)(uintptr_t)strtoull(argv[4], NULL, 10);
    BbCrashRequest *request = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(BbCrashRequest));
    if (!request) return 3;
    if (request->protocol != BB_CRASH_PROTOCOL || request->structure_size != sizeof(BbCrashRequest)) {
        request->startup_error = ERROR_REVISION_MISMATCH;
        SetEvent(ready);
        return 3;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE,
                                 FALSE, request->process_id);
    if (!process) {
        request->startup_error = (LONG)GetLastError();
        SetEvent(ready);
        return 4;
    }
    request->startup_error = 0;
    SetEvent(ready);
    HANDLE waits[] = {requested, process};
    if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) return 0;
    wchar_t path[2100];
    _snwprintf(path, 2100, L"%ls\\crash.dmp", request->directory);
    HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    BOOL success = FALSE;
    DWORD error = GetLastError();
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION exception = {
            request->thread_id, (EXCEPTION_POINTERS *)(uintptr_t)request->exception_pointers, TRUE};
        success = MiniDumpWriteDump(process, request->process_id, file,
            MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules,
            &exception, NULL, NULL);
        error = success ? 0 : GetLastError();
        if (success && !FlushFileBuffers(file)) { success = FALSE; error = GetLastError(); }
        CloseHandle(file);
        if (!success) DeleteFileW(path);
    }
    _snwprintf(path, 2100, L"%ls\\minidump.json", request->directory);
    FILE *status = _wfopen(path, L"w");
    if (status) {
        int written = fprintf(status, "{\"success\":%s,\"error_code\":%lu,\"type\":\"normal_with_threads\"}\n",
                 success ? "true" : "false", error);
        if (fclose(status) || written < 0) request->status_error = ERROR_WRITE_FAULT;
    } else {
        request->status_error = ERROR_OPEN_FAILED;
    }
    request->dump_error = (LONG)error;
    request->dump_success = success;
    MemoryBarrier();
    SetEvent(finished);
    CloseHandle(process);
    UnmapViewOfFile(request);
    return success ? 0 : 5;
}
