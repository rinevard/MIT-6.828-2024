# Lab 9 mmap

![](images/lab9-mmap/last.jpg)

这个 Lab 要求我们实现简单版的 mmap，它把文件映射到内存中以提高访问效率。mmap 的定义可以参考 [man 2 mmap](https://linux.die.net/man/2/mmap)，总之它的参数如下：

```c
void *mmap(void *addr, size_t len, int prot, int flags,
           int fd, off_t offset);
int munmap(void *addr, size_t length);
```

我们跳过系统调用的配置，因为想必大家已经品鉴的够多了。

## sys_mmap 和 vmfault

Lab 要求 lazy allocation，因此我们需要把 mmap 被调用时传入的参数记录在一个结构体中以便后续在 page fault 时按需分配，我设计的结构体如下：

```c
struct {
    int valid;      // Is this idx being used?
    uint64 st;     // Start of the mmap, page aligned
    int len;     // Number of bytes to map
    int prot;       // PROT_READ or PROT_WRITE or both
    int flags;      // MAP_SHARED or MAP_PRIVATE
    struct file *f;         // Mapped file
    int off;
} mmap[NMMAP];
```

其中 NMMAP 被定义在 param.h 中。这里定义成 16 是因为 Lab 文档说用长度为 16 的定长数组来记录 mmap 信息就够了。

```c
#define NMMAP 16
```

这里简单聊下我是怎么设计结构体的。我在设计结构体时习惯快速写一个初版，然后在写后续代码的过程中迭代。像这里的初版就是把 mmap 的参数全塞进去再加个 valid 位，然后写 sys_mmap 时发现 offset 默认是 0 于是就删掉了 off，后来在写 sys_munmap 时又发现 off 是有用的就加了回来。

有了结构体以后我们就能实现 sys_mmap 了。在这个函数里简单地保存数据即可，无需复制文件内容到物理内存中，毕竟我们是 lazy allocation.

```c
uint64 sys_mmap(void) {
    int len, prot, flags;
    struct file *f;
    struct proc *p;
    int i;

    // Ignore args[0], `addr`, which is always zero
    argint(1, &len);
    argint(2, &prot);
    argint(3, &flags);
    if (argfd(4, 0, &f) < 0) {
        return -1;
    }
    // Ignore arg[5], `offset`, which is always zero

    if (!f->writable && (flags & MAP_SHARED) && (prot & PROT_WRITE)) {
        return -1;
    }

    p = myproc();
    for (i = 0; i < NMMAP; i++) {
        if (!p->mmap[i].valid) {
            p->mmap[i].valid = 1;
            p->mmap[i].st = PGROUNDUP(p->sz);
            p->mmap[i].len = len;
            p->mmap[i].prot = prot;
            p->mmap[i].flags = flags;
            p->mmap[i].f = f;
            p->mmap[i].off = 0;
            filedup(f);
            break;
        }
    }
    if (i == NMMAP) {
        // No space to save metadata for this mmap
        return -1;
    }

    p->sz = PGROUNDUP(p->sz) + PGROUNDUP(len);

    return p->mmap[i].st;
}
```

然后我们需要实现 page fault 来把传入 mmap 的文件复制到物理内存中。和 cow lab 类似，要修改 trap.c 的 usertrap 函数：

```c
    if (r_scause() == 8) {
        // system call
        // ...
    } else if ((which_dev = devintr()) != 0) {
        // ok
    } else if ((r_scause() == 15 || r_scause() == 13) &&
               vmfault(p->pagetable, r_stval(), (r_scause() == 13) ? 1 : 0) != 0) {
        // page fault on mmap page
    } else {
        // kill process
        // ...
    }
```

然后这里是我对 vmfault 的实现：

```c
int ismapped(pagetable_t pagetable, uint64 va) {
    pte_t *pte = walk(pagetable, va, 0);
    if (pte == 0) {
        return 0;
    }
    if (*pte & PTE_V) {
        return 1;
    }
    return 0;
}

uint64 vmfault(pagetable_t pagetable, uint64 va, int read) {
    printf("vmfault\n");
    printf("va: %lu, related page: %lu\n", va, PGROUNDDOWN(va));
    uint64 mem;
    int perm, i;
    uint off;
    struct file *f = 0;
    struct proc *p = myproc();

    if (va >= p->sz)
        return 0;
    if (ismapped(pagetable, va)) {
        return 0;
    }

    // Find mmap metadata corresponding to va
    for (i = 0; i < NMMAP; i++) {
        if (p->mmap[i].valid && p->mmap[i].st <= va &&
            va < p->mmap[i].st + p->mmap[i].len) {
            f = p->mmap[i].f;
            break;
        }
    }
    if (!f)
        return 0;

    // Alloc new page and fill with file data
    mem = (uint64)kalloc();
    if (mem == 0)
        return 0;
    memset((void *)mem, 0, PGSIZE);
    ilock(f->ip);
    off = PGROUNDDOWN(va) - p->mmap[i].st + p->mmap[i].off;
    if (off > f->ip->size)
        return 0;
    readi(f->ip, 0, mem, off, PGSIZE);
    iunlock(f->ip);

    // Map page
    perm = PTE_U;
    if (p->mmap[i].prot & PROT_READ)
        perm |= PTE_R;
    if (p->mmap[i].prot & PROT_WRITE)
        perm |= PTE_W;
    if (mappages(p->pagetable, PGROUNDDOWN(va), PGSIZE, mem, perm) != 0) {
        kfree((void *)mem);
        return 0;
    }

    return mem;
}
```

至此我们就能通过第一个 munmap 之前的所有测试了。我们可能会疑惑，如果传入 mmap 的 len 比 f->ip->size 大很多怎么办？对照 man 文档可以发现我们应该在访问非文件区域时（超出文件大小的区域）应该发 SIGBUS 信号，但所幸测试代码没有测这一点，因此我们只要按需分页并复制就行了。如实验文档所言，“If mmaptest doesn't use a mmap feature, you don't need to implement that feature.”

## sys_munmap

接下来我们实现一个便宜版本的 sys_munmap. 感觉在这个 Lab 里要不断回顾 ”If mmaptest doesn't use a mmap feature, you don't need to implement that feature”，不然实现起来就没完没了了🫠🫠

在 Lab 中，munmap 总是在尝试 unmap 一块形如 [start of a map, mid of this map] 或 [mid of a map, end of this map] 或 [start of a map, end of this map] 的区域。总之就是它只在一个 map 区域内 unmap，且不会挖洞。

自然而然地我们会想问，如果传入的参数覆盖了多个 mmap 区域呢？如果覆盖了普通内存呢？如果挖了个洞呢？

我只能说，不要多想🫠🫠🫠🫠

但提出了问题总归是要给解答的，虽然我没有实现这些功能，而且不实现它们也能通过测试。根据 man 手册推测这几种情况的行为如下：

1. 如果传入的参数覆盖了多个 mmap 区域，把它们都 unmap 掉（潜在的风险是每个区域的 flags 可能不同）
2. 如果覆盖了普通内存，把那块内存释放掉
3. 如果挖了个洞，把这块 mmap 区域分割成两块 mmap 区域

回到我们的 Lab. 在实现 sys_munmap 前有两点要注意，第一点是在 unmap 时要检查 map 区域是否被设为 MAP_SHARED，如果是要把内容写回文件；第二点是我们现在使用 lazy allocation，所以虚拟内存空间可能有未映射的虚拟页，因此要修改 uvmunmap 让它在遇到未映射的虚拟页时跳过而非 panic.

先来看看第一点，写回文件的实现如下，如 hint 所言参考 filewrite 就行：

```c
// Write parts of a shared mmap[i] back to file
// theses parts must in mappepd pages and addr must be page-aligned
int mmap_wbk(int i, uint64 addr, int len) {
    struct file *f;
    uint64 fileend, src;
    struct proc *p;
    int pg;

    if (addr % PGSIZE != 0) {
        return -1;
    }

    p = myproc();

    if (i >= NMMAP || !(p->mmap[i].flags & MAP_SHARED) ||
        addr < p->mmap[i].st || addr + len > p->mmap[i].st + p->mmap[i].len) {
        return -1;
    }

    f = p->mmap[i].f;
    ilock(f->ip);
    fileend = p->mmap[i].st - p->mmap[i].off + f->ip->size;
    iunlock(f->ip);

    // Write valid pages back to file
    for (pg = 0; pg * PGSIZE < len; pg++) {
        if (walkaddr(p->pagetable, addr + pg * PGSIZE)) {
            int max = ((MAXOPBLOCKS - 1 - 1 - 2) / 2) * BSIZE;
            int tot = min(PGSIZE, fileend - (addr + pg * PGSIZE));
            int r, written = 0;

            while (written < tot) {
                int n = tot - written;
                if (n > max)
                    n = max;

                begin_op();
                ilock(f->ip);
                src = addr + pg * PGSIZE + written;
                r = writei(f->ip, 1, src, src - p->mmap[i].st + p->mmap[i].off,
                           n);
                iunlock(f->ip);
                end_op();

                if (r != n) {
                    return -1;
                }
                written += r;
            }
        }
    }
    return 0;
}
```

这里以页为单位写回文件是因为 lazy allocation 导致可能存在未被映射的虚拟页，我们不希望把未映射的页写回文件。另外要注意写回的长度不能超过文件本身长度，也就是说对每一个尝试写回的页，实际写入的长度为：

```c
int tot = min(PGSIZE, fileend - (addr + pg * PGSIZE));
```

我把这个 mmap_wbk 函数放在 proc.c 中，因为后续在 exit 函数里也要用到它。

然后是第二点，让 uvmunmap 在遇到未映射的虚拟页时跳过而非 panic，我们简单改了一下 uvmunmap：

```c
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free) {
    uint64 a;
    pte_t *pte;

    if ((va % PGSIZE) != 0)
        panic("uvmunmap: not aligned");

    for (a = va; a < va + npages * PGSIZE; a += PGSIZE) {
        if ((pte = walk(pagetable, a, 0)) == 0) {
            continue;
            // panic("uvmunmap: walk");
        }
        if ((*pte & PTE_V) == 0) {
            continue;
            // panic("uvmunmap: not mapped");
        }
        if (PTE_FLAGS(*pte) == PTE_V)
            panic("uvmunmap: not a leaf");
        if (do_free) {
            uint64 pa = PTE2PA(*pte);
            kfree((void *)pa);
        }
        *pte = 0;
    }
}
```

最后让我们来看看 sys_munmap：

```c
#define min(a, b) ((a) < (b) ? (a) : (b))
uint64 sys_munmap(void) {
    struct file *f;
    uint64 addr;
    int len;
    struct proc *p;
    int i;

    argaddr(0, &addr);
    argint(1, &len);

    if (addr % PGSIZE != 0) {
        return -1;
    }

    p = myproc();

    // Find mmap corresponding to addr
    for (i = 0; i < NMMAP; i++) {
        if (p->mmap[i].valid && p->mmap[i].st <= addr &&
            addr < p->mmap[i].st + p->mmap[i].len) {
            f = p->mmap[i].f;
            break;
        }
    }
    if (i == NMMAP) {
        // No mmap corresponding to addr
        uvmunmap(p->pagetable, addr, PGROUNDUP(len) / PGSIZE, 1);
        return 0;
    }

    if (p->mmap[i].flags & MAP_SHARED) {
        if (mmap_wbk(i, addr, min(len, p->mmap[i].st + p->mmap[i].len - addr)) <
            0) {
            return -1;
        };
    }

    uvmunmap(p->pagetable, addr, PGROUNDUP(len) / PGSIZE, 1);

    if (addr == p->mmap[i].st && addr + len == p->mmap[i].st + p->mmap[i].len) {
        p->mmap[i].valid = 0;
        fileclose(f);
        return 0;
    } else if (addr == p->mmap[i].st) {
        p->mmap[i].st += PGROUNDUP(len);
        p->mmap[i].len -= PGROUNDUP(len);
        p->mmap[i].off += PGROUNDUP(len);
        return 0;
    } else if (addr + len == p->mmap[i].st + p->mmap[i].len) {
        p->mmap[i].len -= PGROUNDUP(len);
        return 0;
    } else {
        // No need to consider hole punching case in this lab
    }

    return -1;
}
```

它能通过测试代码，也算是够用了吧🫠🫠

## exit 和 fork

有了 munmap 的经验，exit 函数的修改就很简单了。我把关闭 mmap 的代码放在了关闭文件后面：

```c
void exit(int status) {
    // ...
    
    // Close all open files.
    // ...

    // Close all mmaps
    for (int i = 0; i < NMMAP; i++) {
        if (p->mmap[i].valid) {
            struct file *f;
            uint64 addr;

            f = p->mmap[i].f;
            addr = p->mmap[i].st;

            if (p->mmap[i].flags & MAP_SHARED) {
                if (mmap_wbk(i, addr, p->mmap[i].len) < 0) {
                    panic("mmap wbk failed");
                };
            }
            for (int pg = 0; pg * PGSIZE < p->mmap[i].len; pg++) {
                if (walkaddr(p->pagetable, addr + pg * PGSIZE)) {
                    uvmunmap(p->pagetable, addr + pg * PGSIZE, 1, 1);
                }
            }
            p->mmap[i].valid = 0;
            fileclose(f);
        }
    }

    // ...
}
```

而 fork 的修改也不难：

```c
int fork(void) {
    // ...

    // increment reference counts on open file descriptors.
    // ...

    // Copy mmap
    for (i = 0; i < NMMAP; i++) {
        if (p->mmap[i].valid) {
            np->mmap[i] = p->mmap[i];
            filedup(np->mmap[i].f);
        }
    }

    safestrcpy(np->name, p->name, sizeof(p->name));

    // ...
}
```

再见了，所有的 6S081

![](images/lab9-mmap/goodbye.png)

完结撒花，感谢陪伴！Bilibili 干杯 - ( ゜- ゜)つロ[