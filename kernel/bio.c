// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define BUCKET_SIZE 13

struct {
    struct spinlock
        steal_lock; // steal lock, only one process is stealing at a time
    struct buf buf[NBUF];
    struct buf head;
} bcache;

struct {
    struct spinlock lock;
    struct buf head;
} buckets[BUCKET_SIZE];

uint hash(uint blockno) { return blockno % BUCKET_SIZE; }

void binit(void) {
    struct buf *b;

    initlock(&bcache.steal_lock, "bcache");
    for (int i = 0; i < BUCKET_SIZE; i++) {
        initlock(&buckets[i].lock, "bcache.bucket");
    }

    // Put all buffers into bucket[0] initially
    struct buf *cur;
    cur = &buckets[0].head;
    for (b = bcache.buf; b < bcache.buf + NBUF; b++) {
        cur->next = b;
        b->prev = cur;
        initsleeplock(&b->lock, "buffer");
        cur = b;
    }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf *bget(uint dev, uint blockno) {
    struct buf *b;

    uint dest = hash(blockno);

    acquire(&buckets[dest].lock);

    // Is the block already cached?
    for (b = buckets[dest].head.next; b; b = b->next) {
        if (b->dev == dev && b->blockno == blockno) {
            b->refcnt++;
            release(&buckets[dest].lock);
            acquiresleep(&b->lock);
            return b;
        }
    }

    // Not cached.
    release(&buckets[dest].lock);

    // steal from another bucket
    for (int i = 0; i < BUCKET_SIZE; i++) {
        acquire(&buckets[i].lock);
        // iterate through bucket to find a free buf
        for (b = buckets[i].head.next; b; b = b->next) {
            if (b->refcnt == 0) {
                // remove b from orig bucket
                b->prev->next = b->next;
                if (b->next) {
                    b->next->prev = b->prev;
                }
                b->prev = 0;
                b->next = 0;
                release(&buckets[i].lock);
                // put b into dest bucket
                acquire(&buckets[dest].lock);
                b->prev = &buckets[dest].head;
                b->next = buckets[dest].head.next;
                b->prev->next = b;
                if (b->next) {
                    b->next->prev = b;
                }
                // double check, other process might have stolen before we steal
                for (struct buf *check = buckets[dest].head.next; check;
                     check = check->next) {
                    if (check->dev == dev && check->blockno == blockno) {
                        check->refcnt++;
                        release(&buckets[dest].lock);
                        acquiresleep(&check->lock);
                        return check;
                    }
                }
                b->dev = dev;
                b->blockno = blockno;
                b->valid = 0;
                b->refcnt = 1;
                release(&buckets[dest].lock);
                acquiresleep(&b->lock);
                return b;
            }
        }
        release(&buckets[i].lock);
    }
    panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf *bread(uint dev, uint blockno) {
    struct buf *b;

    b = bget(dev, blockno);
    if (!b->valid) {
        virtio_disk_rw(b, 0);
        b->valid = 1;
    }
    return b;
}

// Write b's contents to disk.  Must be locked.
void bwrite(struct buf *b) {
    if (!holdingsleep(&b->lock))
        panic("bwrite");
    virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void brelse(struct buf *b) {
    if (!holdingsleep(&b->lock))
        panic("brelse");

    releasesleep(&b->lock);

    uint idx = hash(b->blockno);
    acquire(&buckets[idx].lock);
    b->refcnt--;
    release(&buckets[idx].lock);
}

void bpin(struct buf *b) {
    uint idx = hash(b->blockno);
    acquire(&buckets[idx].lock);
    b->refcnt++;
    release(&buckets[idx].lock);
}

void bunpin(struct buf *b) {
    uint idx = hash(b->blockno);
    acquire(&buckets[idx].lock);
    b->refcnt--;
    release(&buckets[idx].lock);
}
