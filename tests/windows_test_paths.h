#pragma once
#ifdef _WIN32
#include <windows.h>
#include <assert.h>
#include <fcntl.h>
#include <io.h>
#include <stdlib.h>

static inline int test_temp_file(char path[MAX_PATH]) {
    char directory[MAX_PATH];
    DWORD length = GetTempPathA(MAX_PATH, directory);
    assert(length && length < MAX_PATH);
    assert(GetTempFileNameA(directory, "tbb", 0, path));
    return _open(path, _O_RDWR | _O_BINARY);
}
static inline void test_temp_directory(char path[MAX_PATH]) {
    int descriptor = test_temp_file(path);
    assert(descriptor >= 0);
    _close(descriptor);
    assert(DeleteFileA(path));
    assert(CreateDirectoryA(path, NULL));
}
static inline int test_setenv(const char *name, const char *value, int overwrite) {
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value);
}
#define setenv test_setenv
#endif
