/*
 * kernel_memory.c - Xbox Memory Management
 *
 * Implements Mm* and NtAllocateVirtualMemory/NtFreeVirtualMemory/NtQueryVirtualMemory
 * using Win32 VirtualAlloc/VirtualFree/VirtualQuery.
 *
 * Xbox contiguous memory (MmAllocateContiguousMemory) is used for GPU-accessible
 * buffers. On Windows, actual GPU resources are handled by our D3D11 layer;
 * these allocations just need to return valid CPU-accessible memory.
 */

#include "kernel.h"
#include "xbox_memory_layout.h"
#include <stdio.h>
#include <stdlib.h>
#if defined(_WIN32)
/* _aligned_malloc/_aligned_free; POSIX gets them from win32_compat.h */
#include <malloc.h>
#endif

/* ============================================================================
 * Helper: Xbox protect flags → Win32 protect flags
 * ============================================================================ */

static DWORD xbox_protect_to_win32(ULONG xbox_protect)
{
    /* Xbox uses the same PAGE_* constants as Windows NT */
    switch (xbox_protect & 0xFF) {
        case 0x01: return PAGE_NOACCESS;
        case 0x02: return PAGE_READONLY;
        case 0x04: return PAGE_READWRITE;
        case 0x08: return PAGE_WRITECOPY;
        case 0x10: return PAGE_EXECUTE;
        case 0x20: return PAGE_EXECUTE_READ;
        case 0x40: return PAGE_EXECUTE_READWRITE;
        default:   return PAGE_READWRITE;
    }
}

/* ============================================================================
 * Contiguous Memory (GPU-accessible on Xbox)
 * ============================================================================ */

PVOID __stdcall xbox_MmAllocateContiguousMemory(ULONG NumberOfBytes)
{
    PVOID p = VirtualAlloc(NULL, NumberOfBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    XBOX_TRACE(XBOX_LOG_MEM, "MmAllocateContiguousMemory(%u) = %p", NumberOfBytes, p);
    return p;
}

PVOID __stdcall xbox_MmAllocateContiguousMemoryEx(
    ULONG NumberOfBytes,
    ULONG_PTR LowestAcceptableAddress,
    ULONG_PTR HighestAcceptableAddress,
    ULONG Alignment,
    ULONG Protect)
{
    /*
     * Xbox requests physically contiguous, aligned memory for GPU use.
     * We can't guarantee physical contiguity on Windows, but the game's
     * CPU-side code just needs a valid pointer. GPU resources are handled
     * separately by our D3D11 layer.
     *
     * Use _aligned_malloc for alignment, then VirtualAlloc for a fallback.
     */
    PVOID p = NULL;

    if (Alignment > 0 && (Alignment & (Alignment - 1)) == 0) {
        /* Power-of-2 alignment: use _aligned_malloc */
        p = _aligned_malloc(NumberOfBytes, Alignment);
        if (p)
            memset(p, 0, NumberOfBytes);
    }

    if (!p) {
        /* Fallback: page-aligned VirtualAlloc */
        p = VirtualAlloc(NULL, NumberOfBytes, MEM_COMMIT | MEM_RESERVE,
                         xbox_protect_to_win32(Protect));
    }

    XBOX_TRACE(XBOX_LOG_MEM, "MmAllocateContiguousMemoryEx(%u, align=%u) = %p",
        NumberOfBytes, Alignment, p);
    return p;
}

VOID __stdcall xbox_MmFreeContiguousMemory(PVOID BaseAddress)
{
    XBOX_TRACE(XBOX_LOG_MEM, "MmFreeContiguousMemory(%p)", BaseAddress);
    if (!BaseAddress)
        return;

    /*
     * Determine if this was allocated with _aligned_malloc or VirtualAlloc.
     * We use VirtualQuery to check: if it's a VirtualAlloc'd region,
     * AllocationBase will equal the pointer (page-aligned).
     */
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(BaseAddress, &mbi, sizeof(mbi)) &&
        mbi.AllocationBase == BaseAddress &&
        mbi.State == MEM_COMMIT) {
        VirtualFree(BaseAddress, 0, MEM_RELEASE);
    } else {
        _aligned_free(BaseAddress);
    }
}

/* ============================================================================
 * System Memory
 * ============================================================================ */

PVOID __stdcall xbox_MmAllocateSystemMemory(ULONG NumberOfBytes, ULONG Protect)
{
    PVOID p = VirtualAlloc(NULL, NumberOfBytes, MEM_COMMIT | MEM_RESERVE,
                           xbox_protect_to_win32(Protect));
    XBOX_TRACE(XBOX_LOG_MEM, "MmAllocateSystemMemory(%u) = %p", NumberOfBytes, p);
    return p;
}

VOID __stdcall xbox_MmFreeSystemMemory(PVOID BaseAddress, ULONG NumberOfBytes)
{
    XBOX_TRACE(XBOX_LOG_MEM, "MmFreeSystemMemory(%p, %u)", BaseAddress, NumberOfBytes);
    if (BaseAddress)
        VirtualFree(BaseAddress, 0, MEM_RELEASE);
}

/* ============================================================================
 * Memory Query & Protection
 * ============================================================================ */

NTSTATUS __stdcall xbox_MmQueryStatistics(PXBOX_MM_STATISTICS MemoryStatistics)
{
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);

    if (!MemoryStatistics)
        return STATUS_INVALID_PARAMETER;

    if (!GlobalMemoryStatusEx(&ms))
        return STATUS_UNSUCCESSFUL;

    memset(MemoryStatistics, 0, sizeof(XBOX_MM_STATISTICS));
    MemoryStatistics->Length = sizeof(XBOX_MM_STATISTICS);

    /* The console's 64 MB, less what is actually in use.
     *
     * This used to report the host's free memory, which is always more than
     * 64 MB, and so fell back to a flat half: 32 MB. Titles size their own
     * heaps from this. Def Jam: Fight for NY takes AvailablePages minus a fixed
     * 14 MB reserve as its main arena, so it got 18 MB. That is roughly half
     * what a console gives it, and the front end ran the arena dry: the
     * allocation for the loader movie's 587 KB decompressed archive came back
     * NULL, the title decompressed it onto address 0, and the movie never
     * advanced a frame.
     *
     * In use: a kernel footprint (ponytail: a fixed 2 MB, roughly a retail
     * kernel and its pools), the title's image (SizeOfImage from its own XBE
     * header, read once while it is still intact), and what the runtime's two
     * allocators have handed out. The runtime's main-thread stack reservation is
     * its own artefact and is not counted. */
    {
        enum { PAGE = 4096, TOTAL = 64 * 1024 * 1024, KERNEL = 2 * 1024 * 1024 };
        static uint32_t image_size;
        static int logged;
        const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
        uint64_t used;

        if (!image_size) {
            uint32_t base = *(const uint32_t *)(mem + 0x10000u + 0x104u);
            if (base == 0x10000u)
                image_size = *(const uint32_t *)(mem + 0x10000u + 0x10Cu);
        }
        used = (uint64_t)KERNEL + image_size
             + xbox_HeapLiveBytes() + xbox_ContiguousLiveBytes();
        MemoryStatistics->TotalPhysicalPages = TOTAL / PAGE;               /* 16384 */
        MemoryStatistics->AvailablePages =
            used >= TOTAL ? 0 : (ULONG)((TOTAL - used) / PAGE);
        if (logged++ < 4)
            fprintf(stderr, "  [MM] MmQueryStatistics: %lu of %lu pages available "
                            "(image %u KB, heap %u KB, contiguous %u KB)\n",
                    (unsigned long)MemoryStatistics->AvailablePages,
                    (unsigned long)MemoryStatistics->TotalPhysicalPages,
                    image_size / 1024, xbox_HeapLiveBytes() / 1024,
                    xbox_ContiguousLiveBytes() / 1024);
    }
    (void)ms;

    return STATUS_SUCCESS;
}

PVOID __stdcall xbox_MmMapIoSpace(ULONG_PTR PhysicalAddress, ULONG NumberOfBytes, ULONG Protect)
{
    /* GPU register access - handled by our D3D11 layer. Return a dummy buffer. */
    PVOID p = VirtualAlloc(NULL, NumberOfBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    XBOX_TRACE(XBOX_LOG_MEM, "MmMapIoSpace(0x%08X, %u) = %p (stub)", (ULONG)PhysicalAddress, NumberOfBytes, p);
    return p;
}

VOID __stdcall xbox_MmUnmapIoSpace(PVOID BaseAddress, ULONG NumberOfBytes)
{
    XBOX_TRACE(XBOX_LOG_MEM, "MmUnmapIoSpace(%p, %u)", BaseAddress, NumberOfBytes);
    if (BaseAddress)
        VirtualFree(BaseAddress, 0, MEM_RELEASE);
}

/* Physical-to-virtual, remembered from the translations we handed out.
 *
 * A hardware model is given physical addresses and has to write guest memory,
 * so it needs to go back the other way, and it cannot do that by arithmetic.
 * MmGetPhysicalAddress below subtracts the contiguous base inside that window
 * and is the identity outside it, which makes a physical address ambiguous:
 * 0x3CC934 is both the physical form of the contiguous address 0x803CC934 and
 * the identity form of the ordinary address 0x3CC934. Guessing costs real
 * time -- the OHCI model guessed "contiguous", the title's USB driver had
 * passed the address of an ordinary static buffer, and so every device
 * descriptor was written 0x80000000 bytes away from the driver that was
 * waiting to read it. Enumeration failed with every byte moved and every
 * condition code correct.
 *
 * There is no ambiguity in what we actually handed out, though. Any physical
 * address a model sees came from this call, so recording each translation as
 * it is made gives an exact inverse. Whole pages, because a driver translates
 * a buffer once and then puts buffer+offset in its descriptors.
 */
typedef struct { uint32_t pa_page, va_page; } PhysicalMapping;
static PhysicalMapping *g_phys_map;
static LONG g_phys_map_capacity;
static LONG g_phys_map_count;
/* Bumped whenever an answer changes, so a model that caches translations (the
 * APU looks one up for every sample it reads) knows when to drop them. */
static volatile LONG g_phys_map_gen;
static CRITICAL_SECTION g_phys_map_lock;
static INIT_ONCE g_phys_map_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK phys_map_init(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&g_phys_map_lock);
    return TRUE;
}
static void phys_map_lock(void)
{
    InitOnceExecuteOnce(&g_phys_map_once, phys_map_init, NULL, NULL);
    EnterCriticalSection(&g_phys_map_lock);
}

uint32_t xbox_PhysMapGeneration(void)
{
    return (uint32_t)InterlockedCompareExchange(&g_phys_map_gen, 0, 0);
}

static void phys_map_record(uint32_t va, uint32_t pa)
{
    uint32_t va_page = va & ~0xFFFu;
    uint32_t pa_page = pa & ~0xFFFu;
    LONG n, i;
    phys_map_lock();
    n = g_phys_map_count;

    for (i = 0; i < n; i++)
        if (g_phys_map[i].pa_page == pa_page) {
            if (g_phys_map[i].va_page != va_page) {
                g_phys_map[i].va_page = va_page;
                InterlockedIncrement(&g_phys_map_gen);
            }
            LeaveCriticalSection(&g_phys_map_lock);
            return;
        }
    if (n == g_phys_map_capacity) {
        LONG capacity = g_phys_map_capacity ? g_phys_map_capacity * 2 : 512;
        PhysicalMapping *grown = (PhysicalMapping *)realloc(g_phys_map,
                                                (size_t)capacity * sizeof *grown);
        if (!grown) {
            fprintf(stderr, "[MM] physical mapping allocation failed (%ld entries)\n", capacity);
            LeaveCriticalSection(&g_phys_map_lock);
            return;
        }
        g_phys_map = grown;
        g_phys_map_capacity = capacity;
    }
    g_phys_map[n].pa_page = pa_page;
    g_phys_map[n].va_page = va_page;
    g_phys_map_count = n + 1;
    InterlockedIncrement(&g_phys_map_gen);
    LeaveCriticalSection(&g_phys_map_lock);
}

/* The guest address for a physical one, or 0 when we never handed it out.
 * Callers fall back to their own rule in that case rather than being handed
 * a guess from here. */
uint32_t xbox_PhysicalToVirtual(uint32_t pa)
{
    uint32_t pa_page = pa & ~0xFFFu;
    LONG n, i;
    phys_map_lock();
    n = g_phys_map_count;

    for (i = 0; i < n; i++)
        if (g_phys_map[i].pa_page == pa_page) {
            uint32_t va = g_phys_map[i].va_page | (pa & 0xFFFu);
            LeaveCriticalSection(&g_phys_map_lock);
            return va;
        }
    LeaveCriticalSection(&g_phys_map_lock);
    return 0;
}

ULONG_PTR __stdcall xbox_MmGetPhysicalAddress(PVOID BaseAddress)
{
    /*
     * The contiguous arena is the virtual window onto physical RAM, so an
     * address inside it is its physical offset plus XBOX_CONTIG_BASE. Anything
     * outside passes through unchanged.
     *
     * The only implementation. bridge_MmGetPhysicalAddress calls this rather
     * than repeating the arithmetic: the two used to disagree, with the bridge
     * translating and this returning its argument unchanged as a placeholder,
     * so the answer a title got depended on which dispatch path it took. A
     * title writes the result into a pushbuffer, the NV2A reads an address
     * with bit 31 set, and the corruption surfaces as wrong geometry with
     * nothing naming this function.
     */
    uint32_t va = (uint32_t)(uintptr_t)BaseAddress;
    uint32_t pa = (va >= XBOX_CONTIG_BASE &&
                   (uint64_t)va < (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE)
                ? va - XBOX_CONTIG_BASE : va;
    phys_map_record(va, pa);
    return (ULONG_PTR)pa;
}

VOID __stdcall xbox_MmPersistContiguousMemory(PVOID BaseAddress, ULONG NumberOfBytes, BOOLEAN Persist)
{
    /* Xbox: mark memory to survive soft-reboot. No-op on Windows. */
    XBOX_TRACE(XBOX_LOG_MEM, "MmPersistContiguousMemory(%p, %u, %d) - stub", BaseAddress, NumberOfBytes, Persist);
}

ULONG __stdcall xbox_MmQueryAddressProtect(PVOID VirtualAddress)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(VirtualAddress, &mbi, sizeof(mbi))) {
        /* Return the Xbox-equivalent protection */
        return mbi.Protect;
    }
    return PAGE_NOACCESS;
}

VOID __stdcall xbox_MmSetAddressProtect(PVOID BaseAddress, ULONG NumberOfBytes, ULONG NewProtect)
{
    DWORD old_protect;
    VirtualProtect(BaseAddress, NumberOfBytes, xbox_protect_to_win32(NewProtect), &old_protect);
    XBOX_TRACE(XBOX_LOG_MEM, "MmSetAddressProtect(%p, %u, 0x%X)", BaseAddress, NumberOfBytes, NewProtect);
}

ULONG __stdcall xbox_MmQueryAllocationSize(PVOID BaseAddress)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(BaseAddress, &mbi, sizeof(mbi))) {
        return (ULONG)mbi.RegionSize;
    }
    return 0;
}

PVOID __stdcall xbox_MmClaimGpuInstanceMemory(ULONG NumberOfBytes, PULONG NumberOfPaddingBytes)
{
    /* GPU instance memory - handled by D3D11 layer */
    if (NumberOfPaddingBytes)
        *NumberOfPaddingBytes = 0;
    PVOID p = VirtualAlloc(NULL, NumberOfBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    XBOX_TRACE(XBOX_LOG_MEM, "MmClaimGpuInstanceMemory(%u) = %p (stub)", NumberOfBytes, p);
    return p;
}

VOID __stdcall xbox_MmLockUnlockBufferPages(PVOID BaseAddress, ULONG NumberOfBytes, BOOLEAN UnlockPages)
{
    /* Page locking is not meaningful in user mode. No-op. */
    XBOX_TRACE(XBOX_LOG_MEM, "MmLockUnlockBufferPages(%p, %u, %d) - stub", BaseAddress, NumberOfBytes, UnlockPages);
}

VOID __stdcall xbox_MmLockUnlockPhysicalPage(ULONG_PTR PhysicalAddress, BOOLEAN UnlockPage)
{
    XBOX_TRACE(XBOX_LOG_MEM, "MmLockUnlockPhysicalPage(0x%08X, %d) - stub", (ULONG)PhysicalAddress, UnlockPage);
}

/* ============================================================================
 * Kernel Stack
 * ============================================================================ */

PVOID __stdcall xbox_MmCreateKernelStack(ULONG NumberOfBytes, BOOLEAN DebuggerThread)
{
    /* Allocate a stack-like region. Return the TOP of the stack (high address). */
    PVOID base = VirtualAlloc(NULL, NumberOfBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!base)
        return NULL;

    /* Xbox convention: return pointer to top of stack */
    PVOID stack_top = (PUCHAR)base + NumberOfBytes;
    XBOX_TRACE(XBOX_LOG_MEM, "MmCreateKernelStack(%u) = %p (base=%p)", NumberOfBytes, stack_top, base);
    return stack_top;
}

VOID __stdcall xbox_MmDeleteKernelStack(PVOID StackBase, PVOID StackLimit)
{
    /* StackLimit is the low address (base of VirtualAlloc), StackBase is the high address */
    XBOX_TRACE(XBOX_LOG_MEM, "MmDeleteKernelStack(base=%p, limit=%p)", StackBase, StackLimit);
    if (StackLimit)
        VirtualFree(StackLimit, 0, MEM_RELEASE);
}

/* ============================================================================
 * Virtual Memory (Nt API)
 * ============================================================================ */

NTSTATUS __stdcall xbox_NtAllocateVirtualMemory(
    PVOID* BaseAddress,
    ULONG_PTR ZeroBits,
    PSIZE_T RegionSize,
    ULONG AllocationType,
    ULONG Protect)
{
    if (!BaseAddress || !RegionSize)
        return STATUS_INVALID_PARAMETER;

    PVOID result = VirtualAlloc(*BaseAddress, *RegionSize,
                                AllocationType, xbox_protect_to_win32(Protect));
    if (!result) {
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_MEM,
            "NtAllocateVirtualMemory failed: base=%p size=%u type=0x%X err=%u",
            *BaseAddress, (ULONG)*RegionSize, AllocationType, GetLastError());
        return STATUS_NO_MEMORY;
    }

    *BaseAddress = result;
    XBOX_TRACE(XBOX_LOG_MEM, "NtAllocateVirtualMemory(%p, %u) = %p",
        *BaseAddress, (ULONG)*RegionSize, result);
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtFreeVirtualMemory(
    PVOID* BaseAddress,
    PSIZE_T RegionSize,
    ULONG FreeType)
{
    if (!BaseAddress || !*BaseAddress)
        return STATUS_INVALID_PARAMETER;

    SIZE_T size = (FreeType & MEM_RELEASE) ? 0 : (RegionSize ? *RegionSize : 0);

    if (!VirtualFree(*BaseAddress, size, FreeType)) {
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_MEM,
            "NtFreeVirtualMemory failed: base=%p type=0x%X err=%u",
            *BaseAddress, FreeType, GetLastError());
        return STATUS_UNSUCCESSFUL;
    }

    XBOX_TRACE(XBOX_LOG_MEM, "NtFreeVirtualMemory(%p, 0x%X)", *BaseAddress, FreeType);

    if (FreeType & MEM_RELEASE)
        *BaseAddress = NULL;

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtQueryVirtualMemory(
    PVOID BaseAddress,
    PVOID MemoryInformation,
    ULONG MemoryInformationLength,
    PULONG ReturnLength)
{
    MEMORY_BASIC_INFORMATION mbi;

    if (!VirtualQuery(BaseAddress, &mbi, sizeof(mbi)))
        return STATUS_INVALID_PARAMETER;

    /*
     * Xbox NtQueryVirtualMemory returns a MEMORY_BASIC_INFORMATION-like struct.
     * Copy what fits into the caller's buffer.
     */
    ULONG copy_size = (MemoryInformationLength < sizeof(mbi)) ? MemoryInformationLength : (ULONG)sizeof(mbi);
    memcpy(MemoryInformation, &mbi, copy_size);

    if (ReturnLength)
        *ReturnLength = copy_size;

    return STATUS_SUCCESS;
}
