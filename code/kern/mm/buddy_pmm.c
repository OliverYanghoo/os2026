#include <pmm.h>
#include <list.h>
#include <buddy_pmm.h>
#include <stdio.h>
#include <string.h>

//lab2 function3-1 杨昊冉
/* Buddy system allocator.
 *
 * Free memory is kept in MAX_ORDER+1 doubly linked lists, one per order;
 * order k holds blocks of 2^k pages. Every free block head carries
 * PG_property and records the real block size in property. An allocated
 * block head keeps its real size in property but drops PG_property, so
 * free_pages can recover the block size without trusting the caller.
 * Each list is sorted by address, hence the first entry of a list is
 * always its lowest-address block. */

#define MAX_ORDER 14

static free_area_t free_areas[MAX_ORDER + 1];
static size_t total_free = 0;

#define free_list_of(k) (free_areas[k].free_list)
#define nr_free_of(k) (free_areas[k].nr_free)

static void buddy_check(void);
static void buddy_free_pages(struct Page *base, size_t n);

static void
buddy_init(void) {
    for (size_t k = 0; k <= MAX_ORDER; k++) {
        list_init(&free_list_of(k));
        nr_free_of(k) = 0;
    }
    total_free = 0;
}

/* Insert a free block head into its order list, keeping address order. */
static void
buddy_insert(list_entry_t *list, struct Page *base) {
    list_entry_t *next = list_next(list);
    while (next != list && le2page(next, page_link) < base) {
        next = list_next(next);
    }
    list_add_before(next, &base->page_link);
}

/* Smallest k with 2^k >= n. */
static size_t
ceil_order(size_t n) {
    assert(n > 0);
    size_t k = 0;
    while ((1UL << k) < n) {
        k++;
    }
    return k;
}

static size_t
count_blocks(size_t k) {
    size_t count = 0;
    for (list_entry_t *le = list_next(&free_list_of(k));
         le != &free_list_of(k); le = list_next(le)) {
        count++;
    }
    return count;
}

static void
buddy_init_memmap(struct Page *base, size_t n) {
    assert(n > 0);
    for (struct Page *p = base; p != base + n; p++) {
        assert(PageReserved(p));
        p->flags = 0;
        p->property = 0;
        set_page_ref(p, 0);
    }
    /* Carve [base, base+n) into address-aligned power-of-two blocks and
     * hand each one to the free path, so blocks registered by several
     * init_memmap calls coalesce exactly like freed memory does.
     * Alignment is tested on the global page index, which equals physical
     * alignment because nbase is a multiple of 2^MAX_ORDER. */
    while (n > 0) {
        size_t idx = base - pages;
        size_t k = MAX_ORDER;
        while (k > 0 && ((1UL << k) > n || (idx & ((1UL << k) - 1)) != 0)) {
            k--;
        }
        base->property = 1UL << k;
        ClearPageProperty(base);
        buddy_free_pages(base, 1UL << k);
        n -= 1UL << k;
        base += 1UL << k;
    }
}

static struct Page *
buddy_alloc_pages(size_t n) {
    assert(n > 0);
    /* Reject anything beyond the top order before shifting. */
    if (n > (1UL << MAX_ORDER) || n > total_free) {
        return NULL;
    }
    size_t k = ceil_order(n);
    if ((1UL << k) > total_free) {
        return NULL;
    }
    /* Serve from the lowest order that still has a block. */
    size_t j = k;
    while (j <= MAX_ORDER && list_empty(&free_list_of(j))) {
        j++;
    }
    if (j > MAX_ORDER) {
        return NULL;
    }
    struct Page *block = le2page(list_next(&free_list_of(j)), page_link);
    list_del(&block->page_link);
    nr_free_of(j) -= 1UL << j;
    /* Split down to order k, always keeping the lower half and putting
     * the upper half back into the next lower order list. */
    while (j > k) {
        j--;
        struct Page *upper = block + (1UL << j);
        upper->property = 1UL << j;
        SetPageProperty(upper);
        buddy_insert(&free_list_of(j), upper);
        nr_free_of(j) += 1UL << j;
    }
    block->property = 1UL << k;
    ClearPageProperty(block);
    total_free -= 1UL << k;
    return block;
}

static void
buddy_free_pages(struct Page *base, size_t n) {
    assert(n > 0);
    /* The head page records the real allocated block size. */
    size_t size = base->property;
    size_t span = npage - nbase;
    assert(size >= n && (size & (size - 1)) == 0);
    assert((size_t)(base - pages) + size <= span);
    for (struct Page *p = base; p != base + size; p++) {
        assert(!PageReserved(p) && !PageProperty(p));
        p->flags = 0;
        p->property = 0;
        set_page_ref(p, 0);
    }
    size_t k = 0;
    while ((1UL << (k + 1)) <= size) {
        k++;
    }
    assert(k <= MAX_ORDER); /* a recorded size above the top order is corrupt */
    size_t idx = base - pages;
    total_free += size;
    /* Merge with the buddy while the buddy is a free block head of the
     * very same order; the top order has no buddy list to grow into. */
    for (;;) {
        if (k >= MAX_ORDER) {
            break;
        }
        size_t buddy = idx ^ (1UL << k);
        struct Page *b = NULL;
        if (buddy < span && buddy + (1UL << k) <= span) {
            b = pages + buddy;
        }
        if (b != NULL && !PageReserved(b) && PageProperty(b) &&
            b->property == (1UL << k)) {
            list_del(&b->page_link);
            nr_free_of(k) -= 1UL << k;
            if (b < base) {
                base = b;
            }
            idx = base - pages;
            k++;
        } else {
            break;
        }
    }
    base->property = 1UL << k;
    SetPageProperty(base);
    buddy_insert(&free_list_of(k), base);
    nr_free_of(k) += 1UL << k;
}

static size_t
buddy_nr_free_pages(void) {
    return total_free;
}

const struct pmm_manager buddy_pmm_manager = {
    .name = "buddy_pmm_manager",
    .init = buddy_init,
    .init_memmap = buddy_init_memmap,
    .alloc_pages = buddy_alloc_pages,
    .free_pages = buddy_free_pages,
    .nr_free_pages = buddy_nr_free_pages,
    .check = buddy_check,
};

//lab2 function3-2 杨昊冉
/* Self check that runs on whatever region page_init handed over. It only
 * uses the allocator itself plus a private bitmap, and it restores the
 * initial free layout before returning, so the boot can continue. */
#define CHECK_MAP_PAGES ((PHYSICAL_MEMORY_END - DRAM_BASE) / PGSIZE)
#define CHECK_MAP_BYTES ((CHECK_MAP_PAGES + 7) / 8)
static uint8_t check_map[CHECK_MAP_BYTES];
static struct Page *check_slots[CHECK_MAP_PAGES];

/* Every free block must be a properly sized, properly aligned, uniquely
 * linked head; the per-order page counts must add up to the total. */
static void
buddy_audit(void) {
    memset(check_map, 0, CHECK_MAP_BYTES);
    size_t total = 0;
    for (size_t k = 0; k <= MAX_ORDER; k++) {
        list_entry_t *prev_le = &free_list_of(k);
        list_entry_t *le;
        size_t pages_in_order = 0;
        struct Page *prev = NULL;
        while ((le = list_next(prev_le)) != &free_list_of(k)) {
            assert(list_prev(le) == prev_le);
            struct Page *p = le2page(le, page_link);
            assert(PageProperty(p) && p->property == (1UL << k));
            size_t idx = p - pages;
            assert((idx & ((1UL << k) - 1)) == 0);
            assert(prev == NULL || prev < p);
            for (size_t i = 0; i < (1UL << k); i++) {
                size_t pi = idx + i;
                assert(pi < CHECK_MAP_PAGES);
                assert(!((check_map[pi / 8] >> (pi % 8)) & 1));
                check_map[pi / 8] |= 1 << (pi % 8);
            }
            pages_in_order += 1UL << k;
            prev = p;
            prev_le = le;
        }
        assert(pages_in_order == nr_free_of(k));
        total += pages_in_order;
    }
    assert(total == total_free);
}

static void
buddy_check(void) {
    size_t init_free = total_free;
    size_t init_blocks[MAX_ORDER + 1];
    for (size_t k = 0; k <= MAX_ORDER; k++) {
        init_blocks[k] = count_blocks(k);
    }
    buddy_audit();

    /* Sizes and alignment: n pages must come back as one aligned block
     * of 2^ceil(log2 n) pages, and freeing it must undo the split. */
    for (size_t n = 1; n <= 32; n++) {
        size_t k = ceil_order(n);
        struct Page *p = buddy_alloc_pages(n);
        assert(p != NULL);
        assert(((size_t)(p - pages) & ((1UL << k) - 1)) == 0);
        assert(p->property == (1UL << k) && !PageProperty(p));
        buddy_free_pages(p, n);
        buddy_audit();
        assert(total_free == init_free);
    }

    /* Exhaustion: hand out every free page, then merge all of it back.
     * Free order must not matter, so run the restore twice, once in
     * allocation order and once reversed. */
    for (int pass = 0; pass < 2; pass++) {
        size_t total = total_free;
        size_t count = 0;
        while (count < CHECK_MAP_PAGES) {
            struct Page *p = buddy_alloc_pages(1);
            if (p == NULL) {
                break;
            }
            check_slots[count++] = p;
        }
        assert(count == total && total_free == 0);
        assert(buddy_alloc_pages(1) == NULL);

        if (pass == 0) {
            /* Pick sixteen pages with no buddy relation between any two
             * of them, free exactly those, and check that a 16-page
             * request still fails: only buddies may merge. */
            size_t chosen[16], nc = 0;
            for (size_t i = 0; i < count && nc < 16; i++) {
                size_t idx = check_slots[i] - pages;
                int ok = 1;
                for (size_t j = 0; j < nc; j++) {
                    size_t d = idx ^ chosen[j];
                    if ((d & (d - 1)) == 0) {
                        ok = 0; /* power-of-two gap: possible buddy */
                        break;
                    }
                }
                if (ok) {
                    chosen[nc++] = idx;
                }
            }
            assert(nc == 16);
            for (size_t i = 0; i < 16; i++) {
                buddy_free_pages(pages + chosen[i], 1);
            }
            assert(total_free == 16 && count_blocks(0) == 16);
            assert(buddy_alloc_pages(16) == NULL);
            struct Page *singles[16];
            for (size_t i = 0; i < 16; i++) {
                struct Page *p = buddy_alloc_pages(1);
                assert(p != NULL);
                if (i > 0) {
                    assert(p > singles[i - 1]); /* lowest address first */
                }
                singles[i] = p;
            }
            assert(total_free == 0);
            for (size_t i = 0; i < 16; i++) {
                size_t j = 0;
                while (j < 16 && (size_t)(singles[j] - pages) != chosen[i]) {
                    j++;
                }
                assert(j < 16); /* exactly the freed pages came back */
            }
            for (size_t i = 0; i < 16; i++) {
                buddy_free_pages(singles[i], 1);
            }
            assert(total_free == 16 && count_blocks(0) == 16);
            for (size_t i = 0; i < count; i++) {
                size_t idx = check_slots[i] - pages;
                int skip = 0;
                for (size_t j = 0; j < 16; j++) {
                    if (chosen[j] == idx) {
                        skip = 1;
                        break;
                    }
                }
                if (!skip) {
                    buddy_free_pages(check_slots[i], 1);
                }
            }
        } else {
            for (size_t i = count; i > 0; i--) {
                buddy_free_pages(check_slots[i - 1], 1);
            }
        }
        buddy_audit();
        for (size_t k = 0; k <= MAX_ORDER; k++) {
            assert(count_blocks(k) == init_blocks[k]);
        }
        assert(total_free == init_free);
    }

    /* Deterministic random workload against the audit, with at most 32
     * live blocks at any time. */
    struct Page *live_base[32];
    size_t live_n[32];
    for (size_t i = 0; i < 32; i++) {
        live_base[i] = NULL;
        live_n[i] = 0;
    }
    unsigned int seed = 2412351;
    for (size_t step = 0; step < 128; step++) {
        seed = seed * 1664525U + 1013904223U;
        size_t slot = (seed >> 8) % 32;
        if (live_base[slot] != NULL) {
            buddy_free_pages(live_base[slot], live_n[slot]);
            live_base[slot] = NULL;
        }
        if (!(seed & 1)) {
            size_t n = 1 + ((seed >> 16) % 64);
            struct Page *p = buddy_alloc_pages(n);
            if (p != NULL) {
                live_base[slot] = p;
                live_n[slot] = n;
            }
        }
        buddy_audit();
    }
    for (size_t i = 0; i < 32; i++) {
        if (live_base[i] != NULL) {
            buddy_free_pages(live_base[i], live_n[i]);
            live_base[i] = NULL;
        }
    }
    buddy_audit();
    for (size_t k = 0; k <= MAX_ORDER; k++) {
        assert(count_blocks(k) == init_blocks[k]);
    }
    assert(total_free == init_free);
    cprintf("buddy check: PASS\n");
}
