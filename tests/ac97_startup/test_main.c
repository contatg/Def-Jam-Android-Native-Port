/* Force the real startup interleaving at the page-protection boundary. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>

static BOOL WINAPI observed_protect(LPVOID, SIZE_T, DWORD, PDWORD);
#define VirtualProtect observed_protect
#ifndef AC97_FIXTURE_SOURCE
#define AC97_FIXTURE_SOURCE "../../src/hooks/ac97_bm.c"
#endif
#include AC97_FIXTURE_SOURCE
#undef VirtualProtect

static int calls, failed_call, failures;
static BOOL WINAPI observed_protect(LPVOID native, SIZE_T size, DWORD protection, PDWORD old)
{
    static const unsigned char read_cl[] = {0x8a, 0x08}; /* mov cl, byte ptr [rax] */
    uint32_t base = calls++ ? AC97_BASE + 0x2000u : AC97_BASE;
    uint64_t expected = 0x123400u | (base == AC97_BASE ? 0x51u : 0xa5u);
    CONTEXT context = {0};
    (void)native;
    if (size != AC97_PAGE || protection != PAGE_NOACCESS) failures++;
    /* A polling thread may fault immediately after protection takes effect,
     * before VirtualProtect returns to the initializing thread. */
    if (!ac97_bm_owns_address(base + 0x10b)) failures++;
    else {
        context.Rip = (DWORD64)(uintptr_t)read_cl;
        context.Rax = base + 0x10b;
        context.Rcx = 0x123400;
        if (!ac97_bm_handle_mmio(&context, base + 0x10b)
            || context.Rcx != expected
            || context.Rip != (DWORD64)(uintptr_t)(read_cl + 2)) failures++;
    }
    *old = PAGE_READWRITE;
    if (calls == failed_call) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    return TRUE;
}

int main(void)
{
    unsigned char *memory = VirtualAlloc(NULL, 0x3000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!memory) return 2;
    memory[0x10b] = 0x51; memory[0x210b] = 0xa5;
    for (failed_call = 0; failed_call <= 2; failed_call++) {
        s_page_count = 0; memset(s_pages, 0, sizeof(s_pages)); calls = 0;
        ac97_bm_init((ptrdiff_t)((uintptr_t)memory - (uintptr_t)AC97_BASE));
        if (calls != 2 || s_page_count != (failed_call ? 1 : 2)) failures++;
        if (!!ac97_bm_owns_address(AC97_BASE + 0x10b) != (failed_call != 1)) failures++;
        if (!!ac97_bm_owns_address(AC97_BASE + 0x210b) != (failed_call != 2)) failures++;
        ac97_bm_init((ptrdiff_t)((uintptr_t)memory - (uintptr_t)AC97_BASE));
        if (calls != 2) failures++;
    }
    VirtualFree(memory, 0, MEM_RELEASE);
    if (failures) fprintf(stderr, "%d publication/read/rollback checks failed\n", failures);
    return failures ? 1 : 0;
}
