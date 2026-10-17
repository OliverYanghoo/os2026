/* Test the buddy allocator against a shadow model that predicts the exact
 * block addresses, plus a bitmap audit of the free lists. Only boot/UART
 * and DTB getters are fixtures. */
#include "../kern/mm/buddy_pmm.c"
#include <string.h>
#include <test_lab2.h>

#define ARENA_PAGES 20000
static struct Page arena[ARENA_PAGES];
enum { RESERVED, FREE, ALLOCATED };
static unsigned char state[ARENA_PAGES];
static unsigned char seen[ARENA_PAGES];

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
    cprintf("BUDDY_FAIL at %s:%d: ", file, line);
    va_list ap;
    va_start(ap, fmt);
    vcprintf(fmt, ap);
    va_end(ap);
    cprintf("\n");
    finish();
}

uint64_t get_memory_base(void) { return DRAM_BASE; }
uint64_t get_memory_size(void) { return 128 * 1024 * 1024; }

/* Shadow model: per order, the block head indices in ascending address
 * order, mirroring the allocator's sorted free lists. */
#define MODEL_POOL_SIZE 40010
static uint32_t model_pool[MODEL_POOL_SIZE];
static size_t model_off[MAX_ORDER + 1];
static size_t model_cnt[MAX_ORDER + 1];

static void
model_layout(void) {
    size_t off = 0;
    for (size_t k = 0; k <= MAX_ORDER; k++) {
        model_off[k] = off;
        off += ARENA_PAGES / (1UL << k) + 1;
    }
    assert(off <= MODEL_POOL_SIZE);
}

static void
model_insert(size_t idx, size_t k) {
    uint32_t *a = model_pool + model_off[k];
    size_t i = model_cnt[k];
    while (i > 0 && a[i - 1] > idx) {
        a[i] = a[i - 1];
        i--;
    }
    a[i] = idx;
    model_cnt[k]++;
}

static void
model_remove(size_t idx, size_t k) {
    uint32_t *a = model_pool + model_off[k];
    size_t i = 0;
    while (i < model_cnt[k] && a[i] != idx) {
        i++;
    }
    assert(i < model_cnt[k]);
    for (size_t j = i; j + 1 < model_cnt[k]; j++) {
        a[j] = a[j + 1];
    }
    model_cnt[k]--;
}

static int
model_contains(size_t idx, size_t k) {
    uint32_t *a = model_pool + model_off[k];
    for (size_t i = 0; i < model_cnt[k]; i++) {
        if (a[i] == idx) {
            return 1;
        }
    }
    return 0;
}

static size_t
model_total(void) {
    size_t total = 0;
    for (size_t k = 0; k <= MAX_ORDER; k++) {
        total += model_cnt[k] << k;
    }
    return total;
}

static void model_free(size_t idx, size_t size);

/* Greedy aligned decomposition of [base, base+n), same as the allocator:
 * every carved block goes through the merge path. */
static void
model_decompose(size_t base, size_t n) {
    while (n > 0) {
        size_t k = MAX_ORDER;
        while (k > 0 && ((1UL << k) > n || (base & ((1UL << k) - 1)) != 0)) {
            k--;
        }
        model_free(base, 1UL << k);
        base += 1UL << k;
        n -= 1UL << k;
    }
}

static size_t
model_alloc(size_t n) {
    size_t k = 0;
    while ((1UL << k) < n) {
        k++;
    }
    if (k > MAX_ORDER || (1UL << k) > model_total()) {
        return (size_t)-1;
    }
    size_t j = k;
    while (j <= MAX_ORDER && model_cnt[j] == 0) {
        j++;
    }
    if (j > MAX_ORDER) {
        return (size_t)-1;
    }
    size_t idx = model_pool[model_off[j]];
    model_remove(idx, j);
    while (j > k) {
        j--;
        model_insert(idx + (1UL << j), j);
    }
    return idx;
}

static void
model_free(size_t idx, size_t size) {
    size_t k = 0;
    while ((1UL << (k + 1)) <= size) {
        k++;
    }
    for (;;) {
        if (k >= MAX_ORDER) {
            break;
        }
        size_t buddy = idx ^ (1UL << k);
        if (buddy < ARENA_PAGES && buddy + (1UL << k) <= ARENA_PAGES &&
            model_contains(buddy, k)) {
            model_remove(buddy, k);
            if (buddy < idx) {
                idx = buddy;
            }
            k++;
        } else {
            break;
        }
    }
    model_insert(idx, k);
}

/* Every list must match the model exactly, every free page must sit in
 * exactly one aligned block, and every allocated page must be outside. */
static void audit(void) {
    memset(seen, 0, sizeof(seen));
    size_t total = 0;
    for (size_t k = 0; k <= MAX_ORDER; k++) {
        list_entry_t *prev_le = &free_list_of(k);
        list_entry_t *le;
        size_t i = 0;
        while ((le = list_next(prev_le)) != &free_list_of(k)) {
            assert(list_prev(le) == prev_le);
            struct Page *p = le2page(le, page_link);
            size_t idx = p - arena;
            assert(idx == model_pool[model_off[k] + i]);
            assert(PageProperty(p) && p->property == (1UL << k));
            assert((idx & ((1UL << k) - 1)) == 0);
            for (size_t j = 0; j < (1UL << k); j++) {
                assert(state[idx + j] == FREE);
                assert(!seen[idx + j]);
                seen[idx + j] = 1;
            }
            total += 1UL << k;
            i++;
            prev_le = le;
        }
        assert(i == model_cnt[k]);
    }
    assert(total == nr_free_pages() && total == total_free);
    for (size_t i = 0; i < ARENA_PAGES; i++) {
        assert((state[i] == FREE) == (seen[i] != 0));
    }
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
    for (size_t k = 0; k <= MAX_ORDER; k++) {
        model_cnt[k] = 0;
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
    model_decompose(start, n);
    audit();
}

static struct Page *checked_alloc(size_t n) {
    size_t pred = model_alloc(n);
    struct Page *p = alloc_pages(n);
    assert(p == (pred == (size_t)-1 ? NULL : &arena[pred]));
    if (p != NULL) {
        size_t k = 0;
        while ((1UL << k) < n) {
            k++;
        }
        for (size_t i = pred; i < pred + (1UL << k); i++) {
            assert(state[i] == FREE);
            state[i] = ALLOCATED;
        }
        assert(p->property == (1UL << k) && !PageProperty(p));
    }
    audit();
    return p;
}

static void checked_free(size_t start, size_t n) {
    size_t k = 0;
    while ((1UL << k) < n) {
        k++;
    }
    for (size_t i = start; i < start + (1UL << k); i++) {
        assert(state[i] == ALLOCATED);
        state[i] = FREE;
        set_page_ref(&arena[i], 3); /* free must reset every page's ref. */
    }
    model_free(start, 1UL << k);
    free_pages(&arena[start], n);
    audit();
}

int kern_init(void) __attribute__((noreturn));
int kern_init(void) {
    extern char edata[], end[];
    memset(edata, 0, end - edata);
    /* Exercise real pmm_init selection, page_init and the buddy check. */
    pmm_init();
    assert(pmm_manager == &buddy_pmm_manager);
    size_t before = nr_free_pages();
    pmm_manager->check();
    assert(nr_free_pages() == before);
    test_page_init();
    cprintf("PASS: pmm_init selection, page_init and buddy self check\n");

    model_layout();
    reset_arena();
    assert(checked_alloc(1) == NULL);
    cprintf("PASS: empty initialization\n");

    /* Scrambled registration must coalesce through the free path. */
    reset_arena();
    register_range(20, 4);
    register_range(4, 4);
    register_range(12, 4);
    register_range(8, 4);   /* merges with [12,16) into order 3 */
    register_range(16, 4);  /* merges with [20,24) into order 3 */
    assert(checked_alloc(8) == &arena[8]);
    assert(checked_alloc(8) == &arena[16]);
    assert(checked_alloc(2) == &arena[4]);  /* split the lone order-2 block */
    assert(checked_alloc(2) == &arena[6]);
    assert(checked_alloc(1) == NULL);
    assert(nr_free_pages() == 0);
    checked_free(4, 2);
    checked_free(6, 2);
    checked_free(8, 8);
    checked_free(16, 8);
    assert(count_blocks(2) == 1 && count_blocks(3) == 2);
    assert(nr_free_pages() == 20);
    cprintf("PASS: scrambled registration, coalescing and split\n");

    /* Adjacent but non-buddy blocks must stay apart. */
    reset_arena();
    register_range(0, 8);
    register_range(16, 8);
    assert(checked_alloc(16) == NULL);
    assert(checked_alloc(8) == &arena[0]);
    assert(checked_alloc(8) == &arena[16]);
    assert(checked_alloc(1) == NULL);
    cprintf("PASS: neighbours do not merge, only buddies do\n");

    /* Buddy merge cascade up and split cascade down. */
    reset_arena();
    register_range(0, 32);
    assert(checked_alloc(32) == arena);
    checked_free(0, 32);
    assert(count_blocks(5) == 1 && arena[0].property == 32);
    assert(checked_alloc(16) == arena);
    assert(checked_alloc(16) == &arena[16]);
    checked_free(0, 16);   /* buddy still allocated: no merge */
    assert(count_blocks(4) == 1 && count_blocks(5) == 0);
    checked_free(16, 16);  /* now the pair merges back to order 5 */
    assert(count_blocks(5) == 1 && arena[0].property == 32);
    assert(nr_free_pages() == 32);
    cprintf("PASS: buddy merge cascade and split cascade\n");

    /* Splitting always keeps the lower half, so singletons come back in
     * ascending address order. */
    reset_arena();
    register_range(0, 64);
    assert(checked_alloc(64) == arena);
    checked_free(0, 64);
    for (size_t i = 0; i < 64; i++) {
        assert(checked_alloc(1) == &arena[i]);
    }
    assert(checked_alloc(1) == NULL);
    for (size_t i = 64; i > 0; i--) {
        checked_free(i - 1, 1);
    }
    assert(count_blocks(6) == 1 && arena[0].property == 64);
    assert(nr_free_pages() == 64);
    cprintf("PASS: deterministic lower-half splitting and full merge\n");

    /* Exhaust the whole arena, then restore it in reverse order. */
    reset_arena();
    register_range(0, ARENA_PAGES);
    {
        static struct Page *taken[ARENA_PAGES];
        size_t count = 0;
        while (count < ARENA_PAGES) {
            struct Page *p = alloc_pages(1);
            if (p == NULL) {
                break;
            }
            size_t idx = p - arena;
            assert(state[idx] == FREE);
            state[idx] = ALLOCATED;
            taken[count++] = p;
        }
        assert(count == ARENA_PAGES && nr_free_pages() == 0);
        assert(alloc_pages(1) == NULL);
        for (size_t i = count; i > 0; i--) {
            size_t idx = taken[i - 1] - arena;
            checked_free(idx, 1);
        }
        assert(nr_free_pages() == ARENA_PAGES);
        assert(count_blocks(14) == 1 && count_blocks(11) == 1);
        assert(count_blocks(10) == 1 && count_blocks(9) == 1 && count_blocks(5) == 1);
        cprintf("PASS: arena exhaustion and full restoration\n");
    }

    /* Random workload: every step is predicted by the shadow model. */
    reset_arena();
    register_range(0, ARENA_PAGES);
    {
        size_t live_base[64], live_n[64];
        for (size_t i = 0; i < 64; i++) {
            live_base[i] = (size_t)-1;
            live_n[i] = 0;
        }
        unsigned int seed = 2412351;
        for (size_t step = 0; step < 512; step++) {
            seed = seed * 1664525U + 1013904223U;
            size_t slot = (seed >> 8) % 64;
            if (live_base[slot] != (size_t)-1) {
                checked_free(live_base[slot], live_n[slot]);
                live_base[slot] = (size_t)-1;
            }
            if (!(seed & 1)) {
                size_t n = 1 + ((seed >> 16) % 3000);
                if ((seed & 0x60) == 0) {
                    n = 1 + ((seed >> 16) % (ARENA_PAGES - 1));
                }
                struct Page *p = checked_alloc(n);
                if (p != NULL) {
                    live_base[slot] = p - arena;
                    live_n[slot] = n;
                }
            }
        }
        for (size_t i = 0; i < 64; i++) {
            if (live_base[i] != (size_t)-1) {
                checked_free(live_base[i], live_n[i]);
            }
        }
        assert(nr_free_pages() == ARENA_PAGES);
        assert(count_blocks(14) == 1 && count_blocks(11) == 1);
        assert(count_blocks(10) == 1 && count_blocks(9) == 1 && count_blocks(5) == 1);
        cprintf("PASS: 512 deterministic operations against the shadow model\n");
    }
    cprintf("BUDDY_PASS\n");
    finish();
}
