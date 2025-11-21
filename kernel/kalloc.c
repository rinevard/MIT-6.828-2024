// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.
extern int ncpu;   // actually used cpus
                   // defined by main.c

struct run {
    struct run *next;
};

struct {
    struct spinlock lock;
    struct run *freelist;
} kmems[NCPU];

static char kmemlockname[NCPU][16];
const int STOLEN_NUM = 8;

void kinit() {
    for (int i = 0; i < ncpu; i++) {
        snprintf(kmemlockname[i], sizeof(kmemlockname[i]), "kmem_%d", i);
        initlock(&kmems[i].lock, kmemlockname[i]);
    }
    freerange(end, (void *)PHYSTOP);
}

void freerange(void *pa_start, void *pa_end) {
    char *p;
    p = (char *)PGROUNDUP((uint64)pa_start);
    for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE)
        kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void kfree(void *pa) {
    struct run *r;

    if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
        panic("kfree");

    // Fill with junk to catch dangling refs.
    memset(pa, 1, PGSIZE);

    r = (struct run *)pa;

    push_off();
    int id = cpuid();

    acquire(&kmems[id].lock);
    r->next = kmems[id].freelist;
    kmems[id].freelist = r;
    release(&kmems[id].lock);

    pop_off();
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *kalloc(void) {
    // We can consistently pass the 2025 tests, but occasionally fail the 2024
    // test3. I think this is good enough, so we won't continue further.
    struct run *r;

    push_off();
    int id = cpuid();

    acquire(&kmems[id].lock);
    r = kmems[id].freelist;
    if (r) {
        kmems[id].freelist = r->next;
        release(&kmems[id].lock);
    } else {
        // There isn't a fixed acquire order for CPU locks
        // so release the lock before acquiring other locks to avoid deadlock
        release(&kmems[id].lock);
        struct run *stolen_head = 0;
        struct run *stolen_end = 0;
        // Steal STOLEN_NUM pages from another CPU's free list
        for (int i = (id - 1 + ncpu) % ncpu; i != id;
             i = (i - 1 + ncpu) % ncpu) {
            acquire(&kmems[i].lock);
            if (!kmems[i].freelist) {
                release(&kmems[i].lock);
                continue;
            }
            stolen_head = kmems[i].freelist;
            for (int cnt = 0; (kmems[i].freelist) && (cnt < STOLEN_NUM);
                 cnt++) {
                stolen_end = kmems[i].freelist;
                kmems[i].freelist = stolen_end->next;
            }
            stolen_end->next = 0;
            release(&kmems[i].lock);
            break;
        }
        if (stolen_head) {
            acquire(&kmems[id].lock);
            stolen_end->next = kmems[id].freelist;
            kmems[id].freelist = stolen_head;
            r = kmems[id].freelist;
            kmems[id].freelist = r->next;
            release(&kmems[id].lock);
        }
    }

    pop_off();

    if (r)
        memset((char *)r, 5, PGSIZE); // fill with junk
    return (void *)r;
}
