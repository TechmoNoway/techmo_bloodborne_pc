/* SPDX-License-Identifier: GPL-3.0-or-later
 * Real directory search over disposable fixture saves; production realloc failure paths. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <direct.h>
static const char *fixture_root;
static size_t allocations, fail_at;
static void *owned;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL save line %d: %s\n",__LINE__,#x); ExitProcess(1); } } while (0)
static void *checked_realloc(void *p,size_t size) {
    if (++allocations==fail_at) return NULL;
    void *next=realloc(p,size);
    if (next) owned=next;
    return next;
}
static void checked_free(void *p) { if (p==owned) owned=NULL; free(p); }
#define realloc checked_realloc
#define free checked_free
#include "../engine/runtime/runtime_savedata.c"
#undef realloc
#undef free
const char *runtime_file_user_dir(void) { return fixture_root; }
uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *nid) {
    (void)table; (void)count; (void)nid; CHECK(0); return 0;
}
int runtime_file_mount(const char *guest,const char *host) {
    (void)guest; (void)host; CHECK(0); return -1;
}
void runtime_file_unmount(const char *guest) { (void)guest; CHECK(0); }

int main(int argc,char **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    CHECK(argc==2); fixture_root=argv[1];
    char path[900];
    const char *parts[]={"savedata","savedata/1","savedata/1/CUSA03173"};
    for (unsigned i=0;i<3;++i) { snprintf(path,sizeof(path),"%s/%s",fixture_root,parts[i]); CHECK(!_mkdir(path)); }
    for (unsigned i=0;i<40;++i) { snprintf(path,sizeof(path),"%s/savedata/1/CUSA03173/save%02u",fixture_root,i); CHECK(!_mkdir(path)); }
    initialized=1; strcpy(title_id,"CUSA03173");
    SearchCond cond={.user=1};
    DirName names[3]; SearchResult result={.names=names,.names_capacity=3};
    for (fail_at=1;fail_at<=3;++fail_at) {
        allocations=0; result.hits=0x1234; result.set_count=0x5678;
        CHECK(save_search(&cond,&result)==ERR_INTERNAL);
        CHECK(!owned && result.hits==0x1234 && result.set_count==0x5678);
    }
    fail_at=0; allocations=0;
    CHECK(save_search(&cond,&result)==0);
    CHECK(result.hits==40 && result.set_count==3 && !owned && allocations==3);
    for (unsigned i=0;i<3;++i) CHECK(!strncmp(names[i].data,"save",4));
    snprintf(path,sizeof(path),"%s/savedata/1/CUSA03173/save39",fixture_root);
    CHECK(GetFileAttributesA(path)!=INVALID_FILE_ATTRIBUTES);
    puts("PASS: save search first/growth allocation failures release ownership, preserve outputs and retain all fixture directories");
    return 0;
}
