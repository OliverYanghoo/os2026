/* Test the production allocator, including its unchanged framework checks.
 * Only boot/UART and DTB getters are fixtures. The bitmap is an independent
 * oracle, never used by the implementation under test. */
#include "../kern/mm/best_fit_pmm.c"
#include <string.h>
#include <test_lab2.h>

#define ARENA_PAGES 64
static struct Page arena[ARENA_PAGES];
enum { RESERVED, FREE, ALLOCATED };
static unsigned char state[ARENA_PAGES];

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

void __noreturn __panic(const char *file, int line, const char *fmt, ...) {
    cprintf("BEST_FIT_FAIL at %s:%d: ", file, line);
    va_list ap;
    va_start(ap, fmt);
    vcprintf(fmt, ap);
    va_end(ap);
    cprintf("\n");
    finish();
}

uint64_t get_memory_base(void) { return DRAM_BASE; }
uint64_t get_memory_size(void) { return 128 * 1024 * 1024; }

/* Every free run must have exactly one head, in the address-sorted list.
 * Check both link directions and bound traversal to detect list cycles. */
static void audit(void) {
    size_t total = 0, blocks = 0;
    list_entry_t *le = list_next(&free_list), *prev = &free_list;
    for (size_t i = 0; i < ARENA_PAGES; i++) {
        struct Page *p = &arena[i];
        if (state[i] == RESERVED) {
            assert(PageReserved(p));
            continue;
        }
        assert(!PageReserved(p) && page_ref(p) == 0);
        if (state[i] == ALLOCATED) {
            assert(!PageProperty(p) && p->property == 0);
            continue;
        }
        total++;
        if (i == 0 || state[i - 1] != FREE) {
            size_t length = 1;
            while (i + length < ARENA_PAGES && state[i + length] == FREE)
                length++;
            assert(PageProperty(p) && p->property == length);
            assert(le == &p->page_link && list_prev(le) == prev);
            prev = le;
            le = list_next(le);
            assert(++blocks <= ARENA_PAGES);
        } else {
            assert(!PageProperty(p) && p->property == 0);
        }
    }
    assert(le == &free_list && list_prev(&free_list) == prev);
    assert(nr_free == total && nr_free_pages() == total);
}

static void reset_arena(void) {
    pages = arena;
    npage = nbase + ARENA_PAGES;
    pmm_manager->init();
    for (size_t i = 0; i < ARENA_PAGES; i++) {
        arena[i].flags = 0;
        SetPageReserved(&arena[i]);
        arena[i].property = 17;
        set_page_ref(&arena[i], 9);
        list_init(&arena[i].page_link);
        state[i] = RESERVED;
    }
    audit();
}

static void register_range(size_t start, size_t n) {
    assert(start + n <= ARENA_PAGES);
    for (size_t i = start; i < start + n; i++) {
        assert(state[i] == RESERVED);
        state[i] = FREE;
    }
    pmm_manager->init_memmap(&arena[start], n);
    audit();
}

static struct Page *checked_alloc(size_t n) {
    size_t best = ARENA_PAGES, best_size = ARENA_PAGES + 1;
    for (size_t i = 0; i < ARENA_PAGES;) {
        if (state[i] != FREE) {
            i++;
            continue;
        }
        size_t start = i;
        while (i < ARENA_PAGES && state[i] == FREE)
            i++;
        size_t length = i - start;
        if (length >= n && length < best_size) {
            best = start;
            best_size = length;
        }
    }
    struct Page *p = alloc_pages(n);
    assert(p == (best == ARENA_PAGES ? NULL : &arena[best]));
    if (p != NULL) {
        for (size_t i = best; i < best + n; i++)
            state[i] = ALLOCATED;
    }
    audit();
    return p;
}

static void checked_free(size_t start, size_t n) {
    assert(start + n <= ARENA_PAGES);
    for (size_t i = start; i < start + n; i++) {
        assert(state[i] == ALLOCATED);
        state[i] = FREE;
        set_page_ref(&arena[i], 3);  /* free must reset every page's ref. */
    }
    free_pages(&arena[start], n);
    audit();
}

int kern_init(void) __attribute__((noreturn));
int kern_init(void) {
    extern char edata[], end[];
    memset(edata, 0, end - edata);
    /* Exercise real pmm_init selection/page_init and the original .check. */
    pmm_init();
    assert(pmm_manager == &best_fit_pmm_manager);
    size_t before = nr_free_pages();
    pmm_manager->check();
    assert(nr_free_pages() == before);
    test_page_init();
    test_best_fit();
    cprintf("PASS: pmm_init selection and unchanged framework checks\n");

    reset_arena();
    assert(checked_alloc(1) == NULL);
    cprintf("PASS: empty initialization\n");

    register_range(20, 4);
    register_range(4, 4);
    register_range(12, 4);
    register_range(8, 4);   /* merge both sides on registration */
    register_range(16, 4);  /* bridge the remaining two blocks */
    assert(arena[4].property == 20 && nr_free_pages() == 20);
    cprintf("PASS: unordered registration, metadata reset and coalescing\n");

    reset_arena();
    register_range(0, 8);
    register_range(12, 5);
    register_range(24, 3);
    register_range(40, 3);
    assert(checked_alloc(2) == &arena[24]);
    assert(arena[26].property == 1 && PageProperty(&arena[26]));
    assert(checked_alloc(3) == &arena[40]);
    assert(checked_alloc(5) == &arena[12]);
    assert(checked_alloc(8) == &arena[0]);
    assert(checked_alloc(2) == NULL);
    cprintf("PASS: smallest fit, low-address tie, split and exact fit\n");

    reset_arena();
    register_range(0, 4);
    register_range(12, 4);
    assert(checked_alloc(6) == NULL && nr_free_pages() == 8);
    cprintf("PASS: fragmented allocation failure preserves state\n");

    reset_arena();
    register_range(0, 12);
    assert(checked_alloc(12) == arena);
    checked_free(0, 2);
    checked_free(10, 2); /* tail insert must terminate */
    checked_free(4, 2);  /* middle insert, noncontiguous neighbors */
    checked_free(2, 2);  /* merge predecessor and successor */
    checked_free(6, 4);  /* restore whole block by merging both sides */
    assert(arena[0].property == 12 && nr_free_pages() == 12);
    assert(checked_alloc(12) == arena);
    checked_free(0, 6);
    checked_free(6, 6);  /* tail insert and merge predecessor only */
    assert(checked_alloc(12) == arena);
    checked_free(6, 6);
    checked_free(0, 6);  /* insert at head and merge successor only */
    cprintf("PASS: head/middle/tail free and two-sided coalescing\n");

    reset_arena();
    register_range(0, ARENA_PAGES);
    unsigned int seed = 2412351;
    for (size_t step = 0; step < 512; step++) {
        seed = seed * 1664525U + 1013904223U;
        size_t start = (seed >> 16) % ARENA_PAGES;
        if ((seed & 1) && state[start] == ALLOCATED) {
            size_t n = 1;
            while (n < 5 && start + n < ARENA_PAGES &&
                   state[start + n] == ALLOCATED)
                n++;
            checked_free(start, n);
        } else {
            checked_alloc(1 + ((seed >> 8) % 10));
        }
    }
    for (size_t i = 0; i < ARENA_PAGES; i++) {
        if (state[i] == ALLOCATED)
            checked_free(i, 1);
    }
    assert(nr_free_pages() == ARENA_PAGES && arena[0].property == ARENA_PAGES);
    cprintf("PASS: 512 deterministic operations against independent bitmap oracle\n");
    cprintf("BEST_FIT_PASS\n");
    finish();
}
