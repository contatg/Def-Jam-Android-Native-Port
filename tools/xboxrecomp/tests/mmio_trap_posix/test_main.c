/*
 * Trapped device registers on a POSIX host.
 *
 * Two halves. The decoder half runs everywhere: fixed A64 encodings in, the
 * decoded access out. The live half runs on arm64 and x86-64: it closes one 4 KB
 * "device" page in the middle of ordinary memory and checks that
 *
 *   - loads and stores to it reach the callbacks with the right offset,
 *     size and value, and never touch the memory underneath;
 *   - its neighbours in the same host page still read and write as memory
 *     (on a 16 KB host they fault too, and are completed for them);
 *   - a second device in the same host page gets its own callbacks;
 *   - removing a device gives the page back.
 */
#include "mmio_trap.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

static int g_failed;

#define CHECK(cond) do { \
    if (!(cond)) { g_failed++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static void test_decoder(void)
{
    mmio_a64_access a;

    CHECK(mmio_a64_decode(0xB9400020u, &a) && a.is_load && a.size == 4
          && a.rt == 0 && a.rn == 1 && !a.writeback && a.rt2 < 0);   /* ldr w0,[x1] */
    CHECK(mmio_a64_decode(0xB9000462u, &a) && !a.is_load && a.size == 4
          && a.rt == 2 && a.rn == 3);                                /* str w2,[x3,#4] */
    CHECK(mmio_a64_decode(0x39400020u, &a) && a.is_load && a.size == 1
          && !a.sign_extend);                                        /* ldrb w0,[x1] */
    CHECK(mmio_a64_decode(0x79000020u, &a) && !a.is_load && a.size == 2); /* strh w0,[x1] */
    CHECK(mmio_a64_decode(0xB9800020u, &a) && a.is_load && a.size == 4
          && a.sign_extend && a.dest_64);                            /* ldrsw x0,[x1] */
    CHECK(mmio_a64_decode(0x39C00020u, &a) && a.is_load && a.size == 1
          && a.sign_extend && !a.dest_64);                           /* ldrsb w0,[x1] */
    CHECK(mmio_a64_decode(0xF9400020u, &a) && a.is_load && a.size == 8);  /* ldr x0,[x1] */
    CHECK(mmio_a64_decode(0xB8404420u, &a) && a.is_load && a.writeback
          && a.wb_delta == 4);                                       /* ldr w0,[x1],#4 */
    CHECK(mmio_a64_decode(0xB81FCC20u, &a) && !a.is_load && a.writeback
          && a.wb_delta == -4);                                      /* str w0,[x1,#-4]! */
    CHECK(mmio_a64_decode(0xF8626820u, &a) && a.is_load && a.size == 8
          && !a.writeback);                                          /* ldr x0,[x1,x2] */
    CHECK(mmio_a64_decode(0xB85FC020u, &a) && a.is_load && a.size == 4);  /* ldur w0,[x1,#-4] */
    CHECK(mmio_a64_decode(0x29000440u, &a) && !a.is_load && a.size == 4
          && a.rt == 0 && a.rt2 == 1 && a.rn == 2 && !a.writeback);  /* stp w0,w1,[x2] */
    CHECK(mmio_a64_decode(0xA8C10440u, &a) && a.is_load && a.size == 8
          && a.rt2 == 1 && a.writeback && a.wb_delta == 16);         /* ldp x0,x1,[x2],#16 */

    CHECK(!mmio_a64_decode(0x3DC00020u, &a));   /* ldr q0,[x1]: SIMD */
    CHECK(!mmio_a64_decode(0x885F7C20u, &a));   /* ldxr w0,[x1]: exclusive */
    CHECK(!mmio_a64_decode(0xF9800020u, &a));   /* prfm */
    CHECK(!mmio_a64_decode(0x8B020020u, &a));   /* add x0,x1,x2 */
}

#if defined(__aarch64__) || defined(__x86_64__)

typedef struct {
    uint32_t reg[1024];
    int reads, writes;
    uint32_t last_off;
    int last_size;
    uint64_t last_val;
} device;

static uint64_t dev_read(void *p, uint32_t off, int size)
{
    device *d = p;
    uint64_t v = 0;
    d->reads++; d->last_off = off; d->last_size = size;
    memcpy(&v, (uint8_t *)d->reg + off, (size_t)size);
    return v;
}

static void dev_write(void *p, uint32_t off, uint64_t val, int size)
{
    device *d = p;
    d->writes++; d->last_off = off; d->last_size = size; d->last_val = val;
    memcpy((uint8_t *)d->reg + off, &val, (size_t)size);
}

/* The handler changes the device behind the compiler's back, so the test
 * looks at it through volatile and plants values the same way. */
static void plant(volatile device *d, uint32_t off, uint32_t v)
{
    d->reg[off / 4] = v;
}

static void test_live(void)
{
    static volatile device a, b;
    size_t host = (size_t)sysconf(_SC_PAGESIZE);
    uint8_t *mem = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANON, -1, 0);
    volatile uint32_t *before, *reg, *after;
    volatile uint8_t *reg8;
    volatile uint16_t *reg16;
    volatile int32_t *sreg;
    uint64_t neighbours;
    uint32_t i;

    CHECK(mem != MAP_FAILED);
    if (mem == MAP_FAILED) return;
    for (i = 0; i < 0x10000; i += 4)
        *(uint32_t *)(mem + i) = 0xA5000000u | i;

    before = (volatile uint32_t *)(mem + 0x0FFC);
    reg    = (volatile uint32_t *)(mem + 0x1000);
    reg8   = (volatile uint8_t  *)(mem + 0x1000);
    reg16  = (volatile uint16_t *)(mem + 0x1000);
    sreg   = (volatile int32_t  *)(mem + 0x1000);
    after  = (volatile uint32_t *)(mem + 0x2000);

    CHECK(mmio_trap_add(mem + 0x1000, 0x1000, (void *)&a, dev_read, dev_write) == 0);

    /* The device sees the access; the memory under it does not. */
    reg[4] = 0x12345678u;
    CHECK(a.writes == 1 && a.last_off == 16 && a.last_size == 4
          && a.last_val == 0x12345678u);
    CHECK(reg[4] == 0x12345678u && a.reads == 1);
    reg8[0x21] = 0xEE;
    CHECK(a.writes == 2 && a.last_off == 0x21 && a.last_size == 1 && a.last_val == 0xEE);
    plant(&a, 0x40, 0xFFFF8001u);
    CHECK(reg16[0x40 / 2] == 0x8001u && a.last_size == 2);
    CHECK(reg8[0x40] == 0x01u && a.last_size == 1);
    CHECK((int64_t)sreg[0x40 / 4] == -32767);          /* sign-extending load */
    plant(&a, 0xFFC, 0xCAFEF00Du);
    CHECK(reg[0xFFC / 4] == 0xCAFEF00Du && a.last_off == 0xFFC);

    /* Neighbours are memory, whether or not they share the host page. */
    neighbours = mmio_trap_neighbour_count();
    CHECK(*before == (0xA5000000u | 0x0FFC));
    *before = 0x11111111u;
    CHECK(*before == 0x11111111u);
    CHECK(*after == (0xA5000000u | 0x2000));
    *after = 0x22222222u;
    CHECK(*after == 0x22222222u);
    CHECK(a.reads == 5 && a.writes == 2);
    if (host > 4096)
        CHECK(mmio_trap_neighbour_count() == neighbours + 6);
    else
        CHECK(mmio_trap_neighbour_count() == 0);

#if defined(__aarch64__)
    /* Forms a compiler rarely picks for a register access. */
    {
        uint32_t *p = (uint32_t *)(mem + 0x1100), v, lo = 0xAAAA0001u, hi = 0xBBBB0002u;
        uint32_t *q = (uint32_t *)(mem + 0x1200), r0, r1;
        plant(&a, 0x100, 0x0BADC0DEu);
        __asm__ volatile("ldr %w[v], [%[p]], #4" : [v] "=&r"(v), [p] "+r"(p) : : "memory");
        CHECK(v == 0x0BADC0DEu && p == (uint32_t *)(mem + 0x1104));
        __asm__ volatile("stp %w[lo], %w[hi], [%[q]]" : : [lo] "r"(lo), [hi] "r"(hi), [q] "r"(q) : "memory");
        CHECK(a.reg[0x200 / 4] == lo && a.reg[0x204 / 4] == hi && a.last_off == 0x204);
        __asm__ volatile("ldp %w[r0], %w[r1], [%[q]]" : [r0] "=&r"(r0), [r1] "=&r"(r1) : [q] "r"(q) : "memory");
        CHECK(r0 == lo && r1 == hi);
        __asm__ volatile("str wzr, [%[q]]" : : [q] "r"(q) : "memory");
        CHECK(a.reg[0x200 / 4] == 0);
    }
#endif

#if defined(__x86_64__)
    /* Register reads folded into arithmetic, as GCC emits them. The OHCI
     * interrupt routine's `and ecx, [rax+rsi]` (23 0C 30) stopped the game
     * at boot on Linux. Each is checked for its result and its flags. */
    {
        volatile uint32_t *p = (volatile uint32_t *)(mem + 0x1300);
        uint32_t v;
        uint8_t zf, cf, lt;

        plant(&a, 0x300, 0x000000F0u);
        v = 0x0000003Cu;                                       /* and r32, r/m32 */
        __asm__ volatile("andl (%[p]), %[v]; setz %[zf]"
                         : [v] "+r"(v), [zf] "=r"(zf) : [p] "r"(p) : "cc", "memory");
        CHECK(v == 0x30u && !zf && a.last_off == 0x300 && a.last_size == 4);
        v = 0x0000000Fu;
        __asm__ volatile("andl (%[p]), %[v]; setz %[zf]"
                         : [v] "+r"(v), [zf] "=r"(zf) : [p] "r"(p) : "cc", "memory");
        CHECK(v == 0 && zf);
        v = 0x00000100u;                                       /* sub r32, r/m32 */
        __asm__ volatile("subl (%[p]), %[v]; setc %[cf]"
                         : [v] "+r"(v), [cf] "=r"(cf) : [p] "r"(p) : "cc", "memory");
        CHECK(v == 0x10u && !cf);
        v = 0x00000001u;                                       /* cmp r32, r/m32 */
        __asm__ volatile("cmpl (%[p]), %[v]; setl %[lt]"
                         : [lt] "=r"(lt) : [v] "r"(v), [p] "r"(p) : "cc", "memory");
        CHECK(lt);
        __asm__ volatile("cmpl $0xF0, (%[p]); setz %[zf]"     /* cmp r/m32, imm */
                         : [zf] "=r"(zf) : [p] "r"(p) : "cc", "memory");
        CHECK(zf && a.writes == 2);
        __asm__ volatile("testl $0x80, (%[p]); setz %[zf]"    /* test r/m32, imm32 */
                         : [zf] "=r"(zf) : [p] "r"(p) : "cc", "memory");
        CHECK(!zf);
        __asm__ volatile("orl $0x0F, (%[p])" : : [p] "r"(p) : "cc", "memory");
        CHECK(a.reg[0x300 / 4] == 0xFFu && a.writes == 3);    /* or r/m32, imm8 */
        __asm__ volatile("andl $0xFFFFFF0F, (%[p])" : : [p] "r"(p) : "cc", "memory");
        CHECK(a.reg[0x300 / 4] == 0x0Fu && a.writes == 4);    /* and r/m32, imm32 */
    }
#endif

    /* A second device in the same 16 KB host page. */
    CHECK(mmio_trap_add(mem + 0x3000, 0x1000, (void *)&b, dev_read, dev_write) == 0);
    *(volatile uint32_t *)(mem + 0x3008) = 0x33333333u;
    CHECK(b.writes == 1 && b.last_off == 8 && b.reg[2] == 0x33333333u);
    i = a.writes;
    reg[0] = 7;
    CHECK(a.writes == (int)i + 1 && b.writes == 1);
    CHECK(*after == 0x22222222u);

    /* Removing one leaves the other trapped and gives the page back. */
    CHECK(mmio_trap_remove(mem + 0x1000) == 0);
    i = a.reads;
    CHECK(reg[4] == (0xA5000000u | 0x1010) && a.reads == (int)i);
    *(volatile uint32_t *)(mem + 0x3008) = 0x44444444u;
    CHECK(b.writes == 2 && b.reg[2] == 0x44444444u);
    CHECK(mmio_trap_remove(mem + 0x3000) == 0);
    CHECK(*(volatile uint32_t *)(mem + 0x3008) == (0xA5000000u | 0x3008));
    CHECK(*before == 0x11111111u && *after == 0x22222222u);

    printf("live: host page %zu, %llu neighbour accesses completed\n",
           host, (unsigned long long)mmio_trap_neighbour_count());
    munmap(mem, 0x10000);
}

#else
static void test_live(void) { printf("live: skipped on this architecture\n"); }
#endif

int main(void)
{
    test_decoder();
    test_live();
    if (g_failed) {
        fprintf(stderr, "%d check(s) failed\n", g_failed);
        return 1;
    }
    printf("mmio_trap_posix: ok\n");
    return 0;
}
