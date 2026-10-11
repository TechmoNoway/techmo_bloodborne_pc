#ifdef _WIN32
#include "win32_diagnostics.h"
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

static BbCrashRequest *request;
static HANDLE requested, finished, fault_file = INVALID_HANDLE_VALUE;
static volatile LONG reported;
static int reporter_ready;

void bb_diagnostics_init(void) {
    wchar_t directory[2048], executable[2048], command[2300], fault_path[2100];
    DWORD length = GetEnvironmentVariableW(L"BB_SESSION_DIR", directory, 2048);
    if (!length || length >= 2048) return;
    _snwprintf(fault_path, 2100, L"%ls\\fault.json", directory);
    fault_file = CreateFileW(fault_path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL);
    if (!GetModuleFileNameW(NULL, executable, 2048)) return;
    wchar_t *slash = wcsrchr(executable, L'\\');
    if (!slash) return;
    slash[1] = 0;
    if (wcslen(executable) + wcslen(L"bb-crash-reporter.exe") >= 2048) return;
    wcscat(executable, L"bb-crash-reporter.exe");
    if (GetFileAttributesW(executable) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "Diagnostics: crash reporter missing; textual fault record only.\n");
        return;
    }
    SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE,
                                       0, sizeof(BbCrashRequest), NULL);
    HANDLE ready = CreateEventW(&security, TRUE, FALSE, NULL);
    requested = CreateEventW(&security, TRUE, FALSE, NULL);
    finished = CreateEventW(&security, TRUE, FALSE, NULL);
    if (!mapping || !ready || !requested || !finished) goto cleanup;
    request = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(BbCrashRequest));
    if (!request) goto cleanup;
    request->process_id = GetCurrentProcessId();
    wcscpy(request->directory, directory);
    _snwprintf(command, 2300, L"\"%ls\" %llu %llu %llu %llu", executable,
               (unsigned long long)(uintptr_t)mapping, (unsigned long long)(uintptr_t)ready,
               (unsigned long long)(uintptr_t)requested, (unsigned long long)(uintptr_t)finished);
    /* Explicit inheritance keeps stdout, sockets and game file handles out of the helper. */
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &bytes);
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = HeapAlloc(GetProcessHeap(), 0, bytes);
    if (!attributes || !InitializeProcThreadAttributeList(attributes, 1, 0, &bytes)) {
        if (attributes) HeapFree(GetProcessHeap(), 0, attributes);
        goto cleanup;
    }
    HANDLE inherited[] = {mapping, ready, requested, finished};
    STARTUPINFOEXW startup = {0};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process = {0};
    if (UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                  inherited, sizeof(inherited), NULL, NULL) &&
        CreateProcessW(executable, command, NULL, NULL, TRUE,
                       CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
                       &startup.StartupInfo, &process)) {
        reporter_ready = WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0;
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
    DeleteProcThreadAttributeList(attributes);
    HeapFree(GetProcessHeap(), 0, attributes);
cleanup:
    if (ready) CloseHandle(ready);
    if (mapping) CloseHandle(mapping);
    fprintf(stderr, "Diagnostics: external minidump reporter %s.\n", reporter_ready ? "ready" : "unavailable");
}

void bb_diagnostics_fault(EXCEPTION_POINTERS *exception, int guest) {
    if (InterlockedExchange(&reported, 1)) return;
    EXCEPTION_RECORD *record = exception->ExceptionRecord;
    char text[512];
    const int is_access = record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2;
    DWORD access = is_access ? (DWORD)record->ExceptionInformation[0] : 0;
    uint64_t address = is_access ? record->ExceptionInformation[1] : 0;
    int length = snprintf(text, sizeof(text),
        "{\"exception_code\":\"0x%08lx\",\"instruction_address\":\"0x%llx\","
        "\"thread_id\":%lu,\"process_id\":%lu,\"origin\":\"%s\","
        "\"access_kind\":\"%s\",\"access_address\":\"0x%llx\"}\n",
        record->ExceptionCode, (unsigned long long)exception->ContextRecord->Rip,
        GetCurrentThreadId(), GetCurrentProcessId(), guest ? "guest" : "host",
        is_access ? (access == 1 ? "write" : access == 8 ? "execute" : "read") : "none",
        (unsigned long long)address);
    if (fault_file != INVALID_HANDLE_VALUE && length > 0 && length < (int)sizeof(text)) {
        DWORD written;
        WriteFile(fault_file, text, (DWORD)length, &written, NULL);
        FlushFileBuffers(fault_file);
    }
    if (!reporter_ready || !request) return;
    request->thread_id = GetCurrentThreadId();
    request->exception_code = record->ExceptionCode;
    request->exception_pointers = (uint64_t)(uintptr_t)exception;
    request->instruction_address = exception->ContextRecord->Rip;
    request->access_kind = access;
    request->access_address = address;
    MemoryBarrier();
    SetEvent(requested);
    /* Keep the faulting context alive for the remote dumper, with a bounded wait. */
    WaitForSingleObject(finished, 8000);
}
#endif
