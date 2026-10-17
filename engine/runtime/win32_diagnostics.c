#ifdef _WIN32
#include "win32_diagnostics.h"
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

static BbCrashRequest *request;
static HANDLE requested, finished, fault_file = INVALID_HANDLE_VALUE;
static volatile LONG reported;
static volatile LONG reporting_thread;
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

static void report_exception(EXCEPTION_POINTERS *exception, int guest, const char *reason) {
    if (InterlockedCompareExchange(&reported, 1, 0)) {
        /* A concurrent assertion must not _Exit before the first dump completes.
         * Reentrant faults on the reporting thread cannot wait for themselves. */
        const ULONGLONG deadline = GetTickCount64() + 9000;
        while (InterlockedCompareExchange(&reported, 0, 0) == 1 &&
               (DWORD)InterlockedCompareExchange(&reporting_thread, 0, 0) != GetCurrentThreadId() &&
               GetTickCount64() < deadline) Sleep(1);
        return;
    }
    InterlockedExchange(&reporting_thread, (LONG)GetCurrentThreadId());
    EXCEPTION_RECORD *record = exception->ExceptionRecord;
    char text[2048], escaped[1024];
    size_t used = 0;
    /* JSON escaping without allocation; the full assertion remains in the GPU log. */
    for (const unsigned char *p = (const unsigned char *)(reason ? reason : "");
         *p && used + 7 < sizeof(escaped); ++p) {
        if (*p == '"' || *p == '\\') { escaped[used++] = '\\'; escaped[used++] = (char)*p; }
        else if (*p < 0x20 || *p >= 0x80) {
            used += (size_t)snprintf(escaped + used, sizeof(escaped) - used, "\\u%04x", *p);
        } else escaped[used++] = (char)*p;
    }
    escaped[used] = 0;
    const int is_access = record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2;
    DWORD access = is_access ? (DWORD)record->ExceptionInformation[0] : 0;
    uint64_t address = is_access ? record->ExceptionInformation[1] : 0;
    int length = snprintf(text, sizeof(text),
        "{\"exception_code\":\"0x%08lx\",\"instruction_address\":\"0x%llx\","
        "\"thread_id\":%lu,\"process_id\":%lu,\"origin\":\"%s\","
        "\"access_kind\":\"%s\",\"access_address\":\"0x%llx\",\"reason\":\"%s\"}\n",
        record->ExceptionCode, (unsigned long long)exception->ContextRecord->Rip,
        GetCurrentThreadId(), GetCurrentProcessId(), guest ? "guest" : "host",
        is_access ? (access == 1 ? "write" : access == 8 ? "execute" : "read") : "none",
        (unsigned long long)address, escaped);
    if (fault_file != INVALID_HANDLE_VALUE && length > 0 && length < (int)sizeof(text)) {
        DWORD written;
        WriteFile(fault_file, text, (DWORD)length, &written, NULL);
        FlushFileBuffers(fault_file);
    }
    if (!reporter_ready || !request) {
        InterlockedExchange(&reported, 2);
        return;
    }
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
    InterlockedExchange(&reported, 2);
}

void bb_diagnostics_fault(EXCEPTION_POINTERS *exception, int guest) {
    report_exception(exception, guest, NULL);
}

void bb_diagnostics_fatal(const char *reason) {
    CONTEXT context = {0};
    RtlCaptureContext(&context);
    EXCEPTION_RECORD record = {0};
    record.ExceptionCode = BB_RENDERER_ASSERT_EXCEPTION;
    record.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    record.ExceptionAddress = (void *)(uintptr_t)context.Rip;
    EXCEPTION_POINTERS exception = {&record, &context};
    report_exception(&exception, 0, reason ? reason : "GPU library assertion failed");
}
#endif
