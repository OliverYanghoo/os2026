/* Isolate the real static page_init, without implementing Best-Fit or changing
 * the supplied tests. DTB getters and the allocator callback are test fixtures. */
#include "../kern/mm/pmm.c"
#include <test_lab2.h>

#ifndef CASE_ID
#define CASE_ID 0
#endif

static size_t callback_count, free_count;
static struct Page *registered_base;
static size_t registered_count;
static volatile unsigned char *guard;

void cons_putc(int c) {
    volatile unsigned char *uart = (volatile unsigned char *)0x10000000;
    while (!(uart[5] & 0x20))
        ;
    uart[0] = c;
}

static void __noreturn finish(void) {
    while (1)
        ;
}

static const char *expected_panic(void) {
    switch (CASE_ID) {
    case 4:
    case 5:
        return "page_init: invalid device-tree memory range";
    case 6:
        return "page_init: device-tree memory range overflows";
    case 7:
        return "page_init: no manageable DRAM pages";
    case 8:
    case 9:
        return "page_init: kernel is outside the usable memory range";
    case 10:
        return "page_init: insufficient memory for page descriptors";
    default:
        return NULL;
    }
}

void __noreturn __panic(const char *file, int line, const char *fmt, ...) {
    const char *expected = expected_panic();
    if (expected != NULL && strcmp(expected, fmt) == 0 &&
        callback_count == 0 && *guard == 0x5a) {
        cprintf("PAGE_INIT_CASE_%d_PASS (expected panic: %s)\n", CASE_ID, fmt);
    } else {
        cprintf("PAGE_INIT_CASE_%d_FAIL at %s:%d: ", CASE_ID, file, line);
        va_list ap;
        va_start(ap, fmt);
        vcprintf(fmt, ap);
        va_end(ap);
        cprintf("\n");
    }
    finish();
}

uint64_t get_memory_base(void) {
    switch (CASE_ID) {
    case 5: return 0;
    case 6: return (uint64_t)-1 - 1023;
    case 7: return DRAM_BASE - 0x100000;
    case 8: return KERNEL_BEGIN_PADDR + PGSIZE;
    case 12: return DRAM_BASE + PGSIZE;
    default: return DRAM_BASE;
    }
}

uint64_t get_memory_size(void) {
    extern char end[];
    uintptr_t array_pa = ROUNDUP((uintptr_t)end - PHYSICAL_MEMORY_OFFSET, PGSIZE);
    switch (CASE_ID) {
    case 1: return 8 * 1024 * 1024;
    case 2: return 256 * 1024 * 1024;
    case 3: return 128 * 1024 * 1024 - 37;
    case 4: return 0;
    case 6:
    case 7: return PGSIZE;
    case 9: return KERNEL_BEGIN_PADDR - DRAM_BASE + PGSIZE;
    case 10: return array_pa + PGSIZE - DRAM_BASE;
    case 11: {
        /* Find the smallest end page that fits its own descriptor array. */
        size_t count = (array_pa - DRAM_BASE) / PGSIZE;
        while (1) {
            size_t required = (array_pa - DRAM_BASE +
                               count * sizeof(struct Page) + PGSIZE - 1) / PGSIZE;
            if (required == count) {
                return count * PGSIZE;
            }
            count = required;
        }
    }
    case 12: return 128 * 1024 * 1024 - PGSIZE;
    default: return 128 * 1024 * 1024;
    }
}

static void probe_init_memmap(struct Page *base, size_t n) {
    assert(callback_count == 0 && n > 0);
    assert(base >= pages && base + n <= pages + npage - nbase);
    /* Every descriptor must be initialized before the callback runs. */
    for (size_t i = 0; i < npage - nbase; i++) {
        struct Page *p = &pages[i];
        assert(p->flags == (1UL << PG_reserved));
        assert(p->property == 0 && page_ref(p) == 0);
        assert(list_next(&p->page_link) == &p->page_link);
        assert(list_prev(&p->page_link) == &p->page_link);
    }
    callback_count++;
    registered_base = base;
    registered_count = free_count = n;
    for (size_t i = 0; i < n; i++) {
        ClearPageReserved(&base[i]);
    }
    base->property = n;
    SetPageProperty(base);
}

static size_t probe_nr_free_pages(void) {
    return free_count;
}

static const struct pmm_manager probe_manager = {
    .name = "page_init_test_probe",
    .init_memmap = probe_init_memmap,
    .nr_free_pages = probe_nr_free_pages,
};

int kern_init(void) __attribute__((noreturn));
int kern_init(void) {
    extern char edata[], end[];
    memset(edata, 0, end - edata);
    pmm_manager = &probe_manager;
    uintptr_t array_va = ROUNDUP((uintptr_t)end, PGSIZE);
    guard = (volatile unsigned char *)array_va;
    *guard = 0x5a;
    uintptr_t managed_end = 0;
    uintptr_t free_begin = 0;
    volatile unsigned char *tail_guard = NULL;
    if (expected_panic() == NULL) {
        uint64_t raw_end = get_memory_base() + get_memory_size();
        managed_end = ROUNDDOWN(raw_end > PHYSICAL_MEMORY_END ?
                                (uint64_t)PHYSICAL_MEMORY_END : raw_end, PGSIZE);
        size_t expected_count = (managed_end - DRAM_BASE) / PGSIZE;
        uintptr_t descriptors_end = array_va - PHYSICAL_MEMORY_OFFSET +
                                    expected_count * sizeof(struct Page);
        free_begin = ROUNDUP(descriptors_end, PGSIZE);
        if (descriptors_end < managed_end) {
            tail_guard = (volatile unsigned char *)(descriptors_end + PHYSICAL_MEMORY_OFFSET);
            *tail_guard = 0xa5;
        }
    }
    page_init();
    assert(expected_panic() == NULL);
    assert(va_pa_offset == PHYSICAL_MEMORY_OFFSET);
    assert(npage == managed_end / PGSIZE);
    assert(npage - nbase == (managed_end - DRAM_BASE) / PGSIZE);
    assert((uintptr_t)pages == array_va);
    if (tail_guard != NULL) {
        assert(*tail_guard == 0xa5);
    }
    if (free_begin < managed_end) {
        assert(callback_count == 1);
        assert(page2pa(registered_base) == free_begin);
        assert(registered_count == (managed_end - free_begin) / PGSIZE);
    } else {
        assert(callback_count == 0 && free_count == 0);
    }
    for (size_t i = 0; i < npage - nbase; i++) {
        struct Page *p = &pages[i];
        assert(pa2page(page2pa(p)) == p);
        assert(PageReserved(p) == (page2pa(p) < free_begin));
    }
    /* This supplied test assumes DTB memory does not exceed the framework cap
     * and at least one free page exists; other cases use independent checks. */
    if (CASE_ID == 0 || CASE_ID == 1 || CASE_ID == 3 || CASE_ID == 12) {
        test_page_init();
    }
    cprintf("PAGE_INIT_CASE_%d_PASS\n", CASE_ID);
    finish();
}
