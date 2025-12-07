# Lab 7 Lock

这个 lab 让我们熟悉对锁的使用。除了 2024 的题外我还顺便做了下 2025 新增的读写锁（2025 年用 Read-write lock 换掉了 Buffer cache），让我们依次来看看这几道题。

## Memory allocator

在 xv6 里，多个进程同时尝试新增 / 释放内存时会在 kalloc.c 的 kmem 上产生激烈的锁竞争，这道题让我们给每个 CPU 设置一个自己的空闲块链表来缓解竞争问题。比较 tricky 的地方是当一个 CPU 的空闲块链表为空时，它要去别的 CPU 那里偷空闲块。

我们先来看看测试代码在做什么吧，先来看看 test2：

```c
void test2() {
    int free0 = countfree();
    int free1;
    int n = (PHYSTOP - KERNBASE) / PGSIZE;
    printf("start test2\n");
    printf("total free number of pages: %d (out of %d)\n", free0, n);
    if (n - free0 > 1000) {
        printf("test2 FAILED: cannot allocate enough memory");
        exit(1);
    }
    for (int i = 0; i < 50; i++) {
        free1 = countfree();
        if (i % 10 == 9)
            printf(".");
        if (free1 != free0) {
            printf("test2 FAIL: losing pages %d %d\n", free0, free1);
            exit(1);
        }
    }
    printf("\ntest2 OK\n");
}
```

这里的 countfree 函数在 sbrk 尽可能多的内存，计算一共新增了多少页，然后返回新增的页数。可以看出 test2 就是在检查分配 / 释放内存时有没有丢页。

test1 则更复杂一些。它把多进程 sbrk 前后锁自旋的总次数存在 m 和 n 中并计算 n - m。如果我们的实现较好，锁就会自旋较少，就能通过 if (n - m < 10) 然后通过测试：

```c
void test1(void) {
    int n, m;

    printf("start test1\n");
    m = ntas(0);
    // 创建 NCHILD 个子进程并让他们不断 sbrk
    for (int i = 0; i < NCHILD; i++) {
        // ...
    }
    int status = 0;
    for (int i = 0; i < NCHILD; i++) {
        wait(&status);
    }
    n = ntas(1);
    if (n - m < 10)
        printf("test1 OK\n");
    else
        printf("test1 FAIL\n");
}
```

这里的 ntas 很神奇，我花了蛮长时间才弄明白它是怎么获取锁的自旋次数的。感兴趣的话可以看看，不感兴趣的话记住它最终借助了 spinlock.c 的 statslock 来获取锁的自旋总次数就行。

首先 ntas 调用了 statistics，看向 statistics.c，它在读取 statistics 文件：

```c
int statistics(void *buf, int sz) {
    int fd, i, n;

    fd = open("statistics", O_RDONLY);
    if (fd < 0) {
        fprintf(2, "stats: open failed\n");
        exit(1);
    }
    for (i = 0; i < sz;) {
        if ((n = read(fd, buf + i, sz - i)) < 0) {
            break;
        }
        i += n;
    }
    close(fd);
    return i;
}
```

那 statistics 文件在哪呢？statistics 文件在 init.c 里被初始化。我们在内核的 stats.c 的 statsinit 里初始化了 STATS，把对它的读设置为调用 statsread，并在 statsread 里调用 spinlock.c 里的 statslock. 注意这里有内核的 stats.c 和用户的 stats.c，我们说的是前者。

总之 statistics 函数会读取 statistics，对 statistics 的读会调用 statsread，statsread 在 statistics 被读完后会调用 statslock 来更新内容。

我们当然会问为什么要绕这么大一圈，直接读spinlock.c里的东西不行吗？这是因为用户和内核有隔离。statistics 是用户态的函数，不能也不应该直接读内核的 spinlock.c 里的数据。

![](images/lab7-lock/user_kernel.png)

test3 则是 test1 和 test2 的整合高清重制豪华加强版。我的代码能通过 2023 年和 2025 年版本的完整测试，但不能稳定通过 2024 版本的 test3（大部分时间过不了，运气好能过）。

2023 年的版本没有这种整合测试，2025 年的版本写的是 n-m < (NCHILD4-1)*10000，感觉 2025 比 2024 的测试更合理，那就认为我的代码已经满足要求了吧。

然后我们上代码。先放在初始化的部分和 kfree：

```c
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
```

代码思路很清晰，每个 CPU 一个自己的空闲链表，kfree 把释放掉的块加到当前 CPU 的链表上。

我还试了下在 kinit 结尾打印出每个 CPU 的空闲块链表长度，然后发现所有的空闲块都在一个 CPU 那里。原本我还以为 freerange 的过程中每个 CPU 都会跑一跑 kfree，然后让空闲块均匀分布在各个 CPU 上呢。

然后我们看看 kalloc：

```c
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
```

这里比较 trickky 的就是 else 里窃取别的 CPU 的块的部分。首先如我的注释所言，各个 CPU 的 kmem lock 没有固定的获取顺序，所以我们要先释放自己的 kmem lock 再开偷，保证同一时间只拿着一个 CPU 的 kmem lock，不然有死锁风险。

然后我们从当前 CPU 开始往后遍历，找到合适的 CPU 以后就抓它的锁然后偷 STOLEN_NUM 个块暂存在 stolen_head 这个链表里，偷完以后释放它的锁并抓自己的锁再把链表接到自己头上。

这样一来窃取和新增块都是原子性的，不会有并发问题。

我测出来设置 STOLEN_NUM 为 1 或 8 时效果最好，更大反而效果欠佳，挺神奇的。

## Buffer cache

文件系统的缓存层也有一个较激烈的锁竞争，我们可以通过把缓存链表拆分成若干个小的链表，给每个链表设置单独的锁来缓解竞争。

那么怎么拆分链表呢？我们知道每个 block 有 blockno，因此我们可以把 block 放到编号为 hash(blockno) 的链表中。基本思路就是这样，接下来看代码吧，首先是初始化部分：

```c
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
```

这里主要是对链表做了下拆分。这里的 steal_lock 和接下来的 bget 相关，主要是为了防止下面这种情况：

1. 进程 A 和进程 B 都在请求 blockno，我们令 dest = hash(blockno)
2. 进程 A 和 B 都没在 buckets[dest] 找到块，开始窃取。
3. 进程 B 比 A 先窃取到了一个空闲块，把它放进了 buckets[dest] 并填入了数据。
4. 进程 A 也找到了一个空闲块并把它放入 buckets[dest] 并填入数据。
5. 这会导致两个 buf 有相同的 blockno，这不是我们想要的。

这里的 steal_lock 保证同一时间只有一个进程在试图窃取别的 bucket 的 buf. 配合在窃取前再次检查一遍 bucket[dest] 就能避免上述情况的发生。

我们现在来看看 bget：

```c
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

    // --------------------下面是窃取代码--------------------
    // Not cached.
    release(&buckets[dest].lock);

    acquire(&bcache.steal_lock);
    acquire(&buckets[dest].lock);

    // double check, other process might have stolen before we steal
    for (b = buckets[dest].head.next; b; b = b->next) {
        if (b->dev == dev && b->blockno == blockno) {
            b->refcnt++;
            release(&buckets[dest].lock);
            release(&bcache.steal_lock);
            acquiresleep(&b->lock);
            return b;
        }
    }
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
                b->dev = dev;
                b->blockno = blockno;
                b->valid = 0;
                b->refcnt = 1;
                acquiresleep(&b->lock);
                release(&buckets[dest].lock);
                release(&bcache.steal_lock);
                return b;
            }
        }
        release(&buckets[i].lock);
    }
    panic("bget: no buffers");
}
```

也有不加 steal_lock 的写法，可以在填入数据前检查 buckets[dest] 中是否有 blockno 对应的 buf，如果有就不填数据而是把空块加入 buckets[dest]，如果没有再填数据，如下所示：

```c
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
```

最后几个要改的函数把原本的锁换成 bucket 的锁就可以了。

```c
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
```

## Read-write lock

我还抽空做了下 2025 版本新增的 Read-write lock，这要求我们实现一个读写锁。读写锁把用户分为读者和写者，同一时间可以有多个读者或一个写者持有读写锁。另外在这道题里，如果读者和写者同时在等待，写者优先持锁。

我的实现思路还是比较朴素的，我们先看数据结构吧：

```c
struct rwspinlock {
    struct spinlock l;
    int readers;
    int writer_active; // 0 or 1
    int waiting_writers;
};
```

读者仅在没有写者持锁且没有写者在等待时才能持锁，所以我们需要 writer_active 和 waiting_writers 两个字段；写者仅在没有别的写者持锁且没有读者持锁时才能持锁，所以我们也需要 readers 字段。这里的 spinlock 则是为了保护这些共享数据。

其实也可以不用 spinlock 而全用原子语句写，但我感觉太难了就没这么做。

函数的实现思路也比较朴素，可以直接看代码。要注意我们被要求实现自旋版本的读写锁，所以在 acquire 的条件不满足时我们不能 sleep，而应释放锁然后不断循环。

现实里也存在睡眠版本的读写锁，不过这和我们的 lab 没关系。

```c
static void read_acquire_inner(struct rwspinlock *rwlk) {
  while (1) {
    acquire(&rwlk->l);

    if (rwlk->writer_active || rwlk->waiting_writers > 0) {
      release(&rwlk->l);
      continue;
    }

    rwlk->readers++;
    release(&rwlk->l);
    return;
  }
}

static void read_release_inner(struct rwspinlock *rwlk) {
  acquire(&rwlk->l);
  rwlk->readers--;
  release(&rwlk->l);
}

static void write_acquire_inner(struct rwspinlock *rwlk) {
  acquire(&rwlk->l);
  rwlk->waiting_writers++;
  release(&rwlk->l);

  while (1) {
    acquire(&rwlk->l);

    if (rwlk->readers > 0 || rwlk->writer_active) {
      release(&rwlk->l);
      continue;
    }

    rwlk->waiting_writers--;
    rwlk->writer_active = 1;
    release(&rwlk->l);
    return;
  }
}

static void write_release_inner(struct rwspinlock *rwlk) {
  acquire(&rwlk->l);
  rwlk->writer_active = 0;
  release(&rwlk->l);
}
```

这里是 initrwlock 的代码

```c
void initrwlock(struct rwspinlock *rwlk) {
  // Replace this with your implementation.
  initlock(&rwlk->l, "rwlk");
  rwlk->readers = 0;
  rwlk->waiting_writers = 0;
  rwlk->writer_active = 0;
}
```

测试时我们遇到了一个神奇的现象，在 make qemu 后第一次运行 rwlktest 总是能通过的，而之后运行总是不能通过的。重新 make qemu 以后还是第一次运行能通过，后续运行无法通过。

经过漫长的 debug 时光，我们发现问题出在下面的测试函数上：

```c
static void rwspinlock_test_step(uint step, const char *msg) {
  static uint barrier;
  const uint ncpu = 4;

  __atomic_fetch_add(&barrier, 1, __ATOMIC_ACQ_REL);
  while (__atomic_load_n(&barrier, __ATOMIC_RELAXED) < ncpu * step) {
    // spin
  }

  if (cpuid() == 0) {
    printf("rwspinlock_test: step %d: %s\n", step, msg);
  }
}
```

这个函数的设计原意应该是起这两个作用：

1. 同步 CPU，保证四个 CPU 都到达了这里再继续往下运行
2. 打印当前运行到的测试编号（step）

这个函数通过下面的代码实现 CPU 同步。它让 barrier 原子性地加一，表示一个 CPU 跑到了这里，然后如果四个 CPU 中还有人没来就自旋等待。

```c
  __atomic_fetch_add(&barrier, 1, __ATOMIC_ACQ_REL);
  while (__atomic_load_n(&barrier, __ATOMIC_RELAXED) < ncpu * step) {
    // spin
  }
```

为了保证 barrier 被四个 CPU 共享，barrier 被设置为静态变量 static uint barrier. 但测试代码又没在测试开始时把它重置为 0，所以就出现了这种第一次测试能通过，之后的测试不能通过的情况。

第一次测试时，barrier 是初始值 0，所以一切正常；之后的测试里 barrier 保留了之前的值，while 的条件始终为假，于是这种同步功能也就完全失效了。在同步功能失效后，跑得快的 CPU 都跑完 30 个 steps 了，跑得慢的 CPU 可能还在第 8 个 step 挣扎，这就让 rwlktest 也跟着失效了。

气氛都到这儿了，不给个修复也不好意思🫠🫠用下面的代码就行了

```c
static void rwspinlock_test_step(uint step, const char *msg) {
  static uint count;
  static uint sense;
  const uint ncpu = 4;

  int local_sense = __atomic_load_n(&sense, __ATOMIC_RELAXED);

  if (__atomic_fetch_add(&count, 1, __ATOMIC_ACQ_REL) == ncpu - 1) {
    // Let the last reached CPU reset values and print message
    printf("rwspinlock_test: step %d: %s\n", step, msg);
    __atomic_store_n(&count, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&sense, !local_sense, __ATOMIC_RELEASE);
  } else {
    while (__atomic_load_n(&sense, __ATOMIC_ACQUIRE) == local_sense) {
      // spin
    }
  }
}
```

count 和 sense 是共享的变量，所以要用原子语句读写。__ATOMIC_RELAXED 之类的东西是对内存顺序（memory orders）的限制，这能防止指令重排，具体可以看 https://gcc.gnu.org/onlinedocs/gcc/_005f_005fatomic-Builtins.html.

发现这个 bug 以后我也给 6S081 的官方发了邮件，没想到还真收到回复了！他们说加入了一个类似的修复，会在 2026 年的版本更新。感觉自己对 MIT 的滤镜更厚了（）