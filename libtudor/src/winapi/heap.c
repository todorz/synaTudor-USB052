#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <malloc.h>
#include <unistd.h>
#include <sys/mman.h>
#include "internal.h"

#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define PAGE_WRITECOPY 0x08
#define PAGE_EXECUTE 0x10
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_EXECUTE_WRITECOPY 0x80

static DWORD prot_to_page_protect(int prot) {
    bool r = prot & PROT_READ, w = prot & PROT_WRITE, x = prot & PROT_EXEC;
    if(x) return w ? PAGE_EXECUTE_READWRITE : (r ? PAGE_EXECUTE_READ : PAGE_EXECUTE);
    return w ? PAGE_READWRITE : (r ? PAGE_READONLY : PAGE_NOACCESS);
}

static int page_protect_to_prot(DWORD page_protect) {
    switch(page_protect & 0xff) {
        case PAGE_READONLY: case PAGE_WRITECOPY: return PROT_READ;
        case PAGE_READWRITE: return PROT_READ | PROT_WRITE;
        case PAGE_EXECUTE: return PROT_EXEC;
        case PAGE_EXECUTE_READ: return PROT_READ | PROT_EXEC;
        case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY: return PROT_READ | PROT_WRITE | PROT_EXEC;
        default: return PROT_NONE;
    }
}

//Reads the current protection of the page containing addr out of /proc/self/maps,
//since mprotect() offers no way to query the existing protection before overwriting it
//(and callers commonly save/restore protection in pairs via the old-protection out-param).
static int query_page_prot(void *addr) {
    FILE *f = fopen("/proc/self/maps", "r");
    if(!f) return PROT_READ | PROT_WRITE;

    uintptr_t target = (uintptr_t) addr;
    int prot = PROT_READ | PROT_WRITE;

    char line[512];
    while(fgets(line, sizeof(line), f)) {
        uintptr_t start, end;
        char perms[8];
        if(sscanf(line, "%lx-%lx %7s", &start, &end, perms) == 3 && target >= start && target < end) {
            prot = 0;
            if(perms[0] == 'r') prot |= PROT_READ;
            if(perms[1] == 'w') prot |= PROT_WRITE;
            if(perms[2] == 'x') prot |= PROT_EXEC;
            break;
        }
    }

    fclose(f);
    return prot;
}

#define PROC_HEAP_HANDLE ((HANDLE) (uintptr_t) 0x50524f4348454150) /* PROCHEAP */

#define HEAP_GENERATE_EXCEPTIONS 0x00000004
#define HEAP_NO_SERIALIZE 0x00000001
#define HEAP_ZERO_MEMORY 0x00000008

__winfnc HANDLE GetProcessHeap() { return PROC_HEAP_HANDLE; }
WINAPI(GetProcessHeap)

__winfnc void *HeapAlloc(HANDLE heap, DWORD flags, SIZE_T size) {
    if(heap != PROC_HEAP_HANDLE) {
        log_warn("HeapAlloc called with invalid heap handle");
        winerr_set();
        return NULL;
    }

    //Allocate the memory
    void *mem = malloc(size);
    if(mem) {
        if(flags & HEAP_ZERO_MEMORY) memset(mem, 0, size);
        return mem;
    }

    //There was an error allocating the memory
    if(flags & HEAP_GENERATE_EXCEPTIONS) {
        perror("Error allocating memory for HeapAlloc");
        log_error("HeapAlloc: HEAP_GENERATE_EXCEPTIONS flag set and memory allocation failed!");
        abort();
    }

    winerr_set_errno();
    return NULL;
}
WINAPI(HeapAlloc)

//The driver's own AddRef/Release-style object lifecycles don't match this project's fake stand-ins
//closely enough for a real free() here to be safe (see driver.c's fake quine objects): first found
//as a straight double-free abort (the same pointer reaching HeapFree twice - fixed by tracking freed
//pointers and no-opping repeats), but that only masked a deeper problem, confirmed live afterward -
//the *first* free() genuinely succeeds and glibc's tcache "safe linking" then writes an obfuscated
//freelist pointer (`address >> 12`, matched exactly live) into the freed chunk's own first 8 bytes.
//Something else (ctx->field_0x100's real object, via its own field_0x8) still holds a live reference
//to that same memory and reads it later, expecting it to remain the object this project set up -
//a genuine use-after-free once anything actually frees it. So HeapFree is now a permanent no-op:
//nothing this project hands the driver via HeapAlloc is ever actually released for the life of the
//process. Trades unbounded leaking (fine for a short CLI test run) for never corrupting memory
//something else still depends on. See FINDINGS.md.
__winfnc BOOL HeapFree(HANDLE heap, DWORD flags, void *mem) {
    if(heap != PROC_HEAP_HANDLE) {
        log_warn("HeapAlloc called with invalid heap handle");
        winerr_set();
        return FALSE;
    }

    return TRUE;
}
WINAPI(HeapFree)

__winfnc SIZE_T HeapSize(HANDLE heap, DWORD flags, const void *mem) {
    if(heap != PROC_HEAP_HANDLE) {
        log_warn("HeapSize called with invalid heap handle");
        winerr_set();
        return (SIZE_T) -1;
    }

    return malloc_usable_size((void*) mem);
}
WINAPI(HeapSize)

__winfnc BOOL VirtualProtect(void *addr, SIZE_T size, DWORD new_protect, DWORD *old_protect) {
    if(old_protect) *old_protect = prot_to_page_protect(query_page_prot(addr));

    long page_size = sysconf(_SC_PAGESIZE);
    uintptr_t page_addr = (uintptr_t) addr & ~((uintptr_t) page_size - 1);
    size_t total_size = ((uintptr_t) addr + size - page_addr + page_size - 1) & ~((uintptr_t) page_size - 1);

    if(mprotect((void*) page_addr, total_size, page_protect_to_prot(new_protect))) {
        winerr_set_errno();
        return FALSE;
    }

    return TRUE;
}
WINAPI(VirtualProtect)

__winfnc void *LocalFree(void *mem) {
    free(mem);
    return NULL;
}
WINAPI(LocalFree)