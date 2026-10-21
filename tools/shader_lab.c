/* CPU-only shader cache rebuild. Separate process: never executes the game. */
#include <stdio.h>
#include <stdlib.h>
#include <crtdbg.h>
#include <windows.h>
extern int bbgpu_rebuild_shader_cache(const char *directory);
/* The shared library references this entry; the CPU-only helper cannot restart a game. */
void runtime_restart(void) { abort(); }
int main(int argc, char **argv) {
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc != 2) { fputs("Usage: bb-shader-lab CACHE_DIRECTORY\n", stderr); return 2; }
    return bbgpu_rebuild_shader_cache(argv[1]);
}
