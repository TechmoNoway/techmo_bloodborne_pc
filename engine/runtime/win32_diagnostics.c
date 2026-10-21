#ifdef _WIN32
#include "win32_diagnostics.h"
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

static BbCrashRequest *request;
static HANDLE requested, finished, reporter_process;
static HANDLE fault_file = INVALID_HANDLE_VALUE, status_file = INVALID_HANDLE_VALUE;
static volatile LONG reported, reporting_thread;
static int reporter_ready;
static DWORD fault_open_error, startup_error;
static const char *startup_stage = "not_initialized";

/* A crashed thread can own stderr's CRT lock. Do not take it while requesting a dump. */
static void diagnostic_error(const char *operation, DWORD error) {
    char text[256];
    int length = snprintf(text, sizeof(text), "Diagnostics: %s (Windows %lu).\n", operation, error);
    if (length > 0 && length < (int)sizeof(text)) {
        DWORD written;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, (DWORD)length, &written, NULL);
    }
}

/* Preopened files keep the reporting path independent of path lookup and allocation. */
static DWORD write_record(HANDLE file, const char *text, DWORD length) {
    if (file == INVALID_HANDLE_VALUE) return ERROR_INVALID_HANDLE;
    LARGE_INTEGER beginning = {0};
    DWORD written = 0;
    if (!SetFilePointerEx(file, beginning, NULL, FILE_BEGIN) ||
        !WriteFile(file, text, length, &written, NULL)) return GetLastError();
    if (written != length) return ERROR_WRITE_FAULT;
    if (!SetEndOfFile(file) || !FlushFileBuffers(file)) return GetLastError();
    return ERROR_SUCCESS;
}

static void write_status(const char *phase, DWORD fault_error, const char *dump_state, DWORD waited) {
    char text[1024];
    int length = snprintf(text, sizeof(text),
        "{\"schema_version\":1,\"protocol\":%u,\"reporter_ready\":%s,"
        "\"startup_stage\":\"%s\",\"startup_error\":%lu,\"fault_open_error\":%lu,"
        "\"phase\":\"%s\",\"fault_write_error\":%lu,\"dump_state\":\"%s\","
        "\"wait_result\":%lu,\"dump_success\":%s,\"dump_error\":%ld,\"status_error\":%ld}\n",
        BB_CRASH_PROTOCOL, reporter_ready ? "true" : "false", startup_stage, startup_error,
        fault_open_error, phase, fault_error, dump_state, waited,
        request && request->dump_success ? "true" : "false",
        request ? request->dump_error : 0L, request ? request->status_error : 0L);
    if (length > 0 && length < (int)sizeof(text)) {
        DWORD error = write_record(status_file, text, (DWORD)length);
        if (error) diagnostic_error("status write failed", error);
    }
}

void bb_diagnostics_init(void) {
    wchar_t directory[2048], executable[2048], command[2300], path[2100];
    HANDLE mapping = NULL, ready = NULL;
    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee(&guarantee);
    DWORD length = GetEnvironmentVariableW(L"BB_SESSION_DIR", directory, 2048);
    if (!length || length >= 2048) {
        fprintf(stderr, "Diagnostics: no usable BB_SESSION_DIR; crash artifacts unavailable.\n");
        return;
    }
    _snwprintf(path, 2100, L"%ls\\diagnostics.json", directory);
    status_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, NULL);
    if (status_file == INVALID_HANDLE_VALUE)
        fprintf(stderr, "Diagnostics: cannot open diagnostics.json (Windows %lu).\n", GetLastError());
    _snwprintf(path, 2100, L"%ls\\fault.json", directory);
    fault_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL);
    if (fault_file == INVALID_HANDLE_VALUE) {
        fault_open_error = GetLastError();
        fprintf(stderr, "Diagnostics: cannot open fault.json (Windows %lu).\n", fault_open_error);
    }
    startup_stage = "reporter_path";
    length = GetModuleFileNameW(NULL, executable, 2048);
    if (!length || length >= 2048) { startup_error = GetLastError(); goto cleanup; }
    wchar_t *slash = wcsrchr(executable, L'\\');
    if (!slash) { startup_error = ERROR_BAD_PATHNAME; goto cleanup; }
    slash[1] = 0;
    if (wcslen(executable) + wcslen(L"bb-crash-reporter.exe") >= 2048) {
        startup_error = ERROR_FILENAME_EXCED_RANGE; goto cleanup;
    }
    wcscat(executable, L"bb-crash-reporter.exe");
    if (GetFileAttributesW(executable) == INVALID_FILE_ATTRIBUTES) {
        startup_error = GetLastError(); goto cleanup;
    }
    startup_stage = "shared_handles";
    SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
    mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE,
                                0, sizeof(BbCrashRequest), NULL);
    if (!mapping) { startup_error = GetLastError(); goto cleanup; }
    ready = CreateEventW(&security, TRUE, FALSE, NULL);
    if (!ready) { startup_error = GetLastError(); goto cleanup; }
    requested = CreateEventW(&security, TRUE, FALSE, NULL);
    if (!requested) { startup_error = GetLastError(); goto cleanup; }
    finished = CreateEventW(&security, TRUE, FALSE, NULL);
    if (!finished) { startup_error = GetLastError(); goto cleanup; }
    request = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(BbCrashRequest));
    if (!request) { startup_error = GetLastError(); goto cleanup; }
    request->protocol = BB_CRASH_PROTOCOL;
    request->structure_size = sizeof(BbCrashRequest);
    request->startup_error = ERROR_NOT_READY;
    request->process_id = GetCurrentProcessId();
    wcscpy(request->directory, directory);
    _snwprintf(command, 2300, L"\"%ls\" %llu %llu %llu %llu", executable,
               (unsigned long long)(uintptr_t)mapping, (unsigned long long)(uintptr_t)ready,
               (unsigned long long)(uintptr_t)requested, (unsigned long long)(uintptr_t)finished);
    /* Only these four handles are inherited: never game files, logs or sockets. */
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &bytes);
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = HeapAlloc(GetProcessHeap(), 0, bytes);
    startup_stage = "handle_list";
    if (!attributes) { startup_error = ERROR_NOT_ENOUGH_MEMORY; goto cleanup; }
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes)) {
        startup_error = GetLastError();
        HeapFree(GetProcessHeap(), 0, attributes);
        goto cleanup;
    }
    HANDLE inherited[] = {mapping, ready, requested, finished};
    STARTUPINFOEXW startup = {0};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process = {0};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherited, sizeof(inherited), NULL, NULL)) {
        startup_error = GetLastError();
    } else {
        startup_stage = "create_process";
        if (!CreateProcessW(executable, command, NULL, NULL, TRUE,
                            CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
                            &startup.StartupInfo, &process)) {
            startup_error = GetLastError();
        } else {
            CloseHandle(process.hThread);
            reporter_process = process.hProcess;
            startup_stage = "handshake";
            HANDLE waits[] = {ready, reporter_process};
            DWORD waited = WaitForMultipleObjects(2, waits, FALSE, 5000);
            if (waited == WAIT_OBJECT_0) {
                MemoryBarrier();
                startup_error = (DWORD)request->startup_error;
                reporter_ready = startup_error == 0;
            } else if (waited == WAIT_OBJECT_0 + 1) {
                startup_stage = "reporter_exited_before_ready";
                if (!GetExitCodeProcess(reporter_process, &startup_error)) startup_error = GetLastError();
                if (!startup_error) startup_error = ERROR_PROCESS_ABORTED;
            } else {
                startup_error = waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
                /* Only the helper we just created, never another application. */
                TerminateProcess(reporter_process, startup_error);
            }
        }
    }
    DeleteProcThreadAttributeList(attributes);
    HeapFree(GetProcessHeap(), 0, attributes);
cleanup:
    if (ready) CloseHandle(ready);
    if (mapping) CloseHandle(mapping);
    if (reporter_ready) startup_stage = "ready";
    write_status("initialized", fault_open_error, "not_requested", 0);
    fprintf(stderr, "Diagnostics: external minidump reporter %s; stage=%s Windows=%lu.\n",
            reporter_ready ? "ready" : "unavailable", startup_stage, startup_error);
}

static void report_exception(EXCEPTION_POINTERS *exception, int guest, const char *reason) {
    if (InterlockedCompareExchange(&reported, 1, 0)) {
        const ULONGLONG deadline = GetTickCount64() + 9000;
        while (InterlockedCompareExchange(&reported, 0, 0) == 1 &&
               (DWORD)InterlockedCompareExchange(&reporting_thread, 0, 0) != GetCurrentThreadId() &&
               GetTickCount64() < deadline) Sleep(1);
        return;
    }
    InterlockedExchange(&reporting_thread, (LONG)GetCurrentThreadId());
    EXCEPTION_RECORD *record = exception->ExceptionRecord;
    CONTEXT *context = exception->ContextRecord;
    char text[3072], escaped[1024], instruction[33] = {0};
    size_t used = 0;
    for (const unsigned char *p = (const unsigned char *)(reason ? reason : "");
         *p && used + 7 < sizeof(escaped); ++p) {
        if (*p == '"' || *p == '\\') { escaped[used++] = '\\'; escaped[used++] = (char)*p; }
        else if (*p < 0x20 || *p >= 0x80)
            used += (size_t)snprintf(escaped + used, sizeof(escaped) - used, "\\u%04x", *p);
        else escaped[used++] = (char)*p;
    }
    escaped[used] = 0;
    unsigned char bytes[16];
    SIZE_T count = 0;
    if (ReadProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)context->Rip, bytes, sizeof(bytes), &count)) {
        static const char hex[] = "0123456789abcdef";
        for (SIZE_T i = 0; i < count; ++i) {
            instruction[i * 2] = hex[bytes[i] >> 4];
            instruction[i * 2 + 1] = hex[bytes[i] & 15];
        }
    }
    const int is_access = (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
                          record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) && record->NumberParameters >= 2;
    DWORD access = is_access ? (DWORD)record->ExceptionInformation[0] : 0;
    uint64_t address = is_access ? record->ExceptionInformation[1] : 0;
    int length = snprintf(text, sizeof(text),
        "{\"exception_code\":\"0x%08lx\",\"instruction_address\":\"0x%llx\","
        "\"thread_id\":%lu,\"process_id\":%lu,\"origin\":\"%s\","
        "\"access_kind\":\"%s\",\"access_address\":\"0x%llx\",\"reason\":\"%s\","
        "\"instruction_bytes\":\"%s\",\"registers\":{"
        "\"rax\":\"0x%llx\",\"rbx\":\"0x%llx\",\"rcx\":\"0x%llx\",\"rdx\":\"0x%llx\","
        "\"rsi\":\"0x%llx\",\"rdi\":\"0x%llx\",\"rbp\":\"0x%llx\",\"rsp\":\"0x%llx\","
        "\"r8\":\"0x%llx\",\"r9\":\"0x%llx\",\"r10\":\"0x%llx\",\"r11\":\"0x%llx\","
        "\"r12\":\"0x%llx\",\"r13\":\"0x%llx\",\"r14\":\"0x%llx\",\"r15\":\"0x%llx\"}}\n",
        record->ExceptionCode, context->Rip, GetCurrentThreadId(), GetCurrentProcessId(),
        guest ? "guest" : "host", is_access ? (access == 1 ? "write" : access == 8 ? "execute" : "read") : "none",
        (unsigned long long)address, escaped, instruction,
        context->Rax, context->Rbx, context->Rcx, context->Rdx, context->Rsi, context->Rdi,
        context->Rbp, context->Rsp, context->R8, context->R9, context->R10, context->R11,
        context->R12, context->R13, context->R14, context->R15);
    DWORD fault_error = length > 0 && length < (int)sizeof(text)
        ? write_record(fault_file, text, (DWORD)length) : ERROR_INSUFFICIENT_BUFFER;
    if (fault_error) diagnostic_error("fault record write failed", fault_error);
    write_status("reporting", fault_error, "pending", 0);
    if (!reporter_ready || !request) {
        write_status("complete", fault_error, "reporter_unavailable", 0);
        InterlockedExchange(&reported, 2);
        return;
    }
    request->thread_id = GetCurrentThreadId();
    request->exception_code = record->ExceptionCode;
    request->exception_pointers = (uint64_t)(uintptr_t)exception;
    request->instruction_address = context->Rip;
    request->access_kind = access;
    request->access_address = address;
    MemoryBarrier();
    if (!SetEvent(requested)) {
        write_status("complete", fault_error, "signal_failed", GetLastError());
    } else {
        HANDLE waits[] = {finished, reporter_process};
        DWORD waited = WaitForMultipleObjects(2, waits, FALSE, 8000);
        MemoryBarrier();
        const char *state = waited == WAIT_OBJECT_0 ? (request->dump_success ? "written" : "failed")
            : waited == WAIT_OBJECT_0 + 1 ? "reporter_exited" : waited == WAIT_TIMEOUT ? "timeout" : "wait_failed";
        write_status("complete", fault_error, state, waited == WAIT_FAILED ? GetLastError() : waited);
        if (waited != WAIT_OBJECT_0 || !request->dump_success)
            diagnostic_error(state, request->dump_error ? (DWORD)request->dump_error : waited);
    }
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
