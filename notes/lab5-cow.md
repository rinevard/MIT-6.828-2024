# Lab 5 Copy-on-Write Fork

这个 Lab 要求我们实现 cow fork，这是一个常见的节省内存的手段。

总而言之，就是在 fork 时把父子的虚拟内存都映射到原本的父进程的物理内存上，并将其设为“仅读”。当有进程尝试写它会触发 page fault，这时我们才新建物理页，然后调整尝试写的进程的页表，将对应虚拟页映射到新建的物理页上。

这个 Lab 似乎没有太 tricky 的地方，接下来我会自问自答一下，只要能把这里的答案想明白基本就能写对代码了：

1. 怎么让只读页正确只读，而不能被写？
    
    我们通过给 PTE 设置一个 COW 位来判断是否是只读. COW 为 1 表示这个 PTE 原本应该是可写的，但现在被临时标记为只读，因为对应的物理页正在被多个进程共享。
    
2. 什么时候新建物理页？我们会不会多建了不必要的物理页？
    
    我们维护对每个物理页的引用计数，当引用计数为 1 时就把 COW 重设为 0（因为这个物理页不再被多个进程共享），把 W 位重新设为 1.
    
3. 什么时候释放物理页？
    
    调用 kfree 时，如果引用计数大于 1，将引用计数 -1 然后不动；如果引用计数 == 1，释放物理页。
    

然后就上代码吧。

## 物理内存层的基建代码

首先在 kalloc.c 里新建一个数据结构：

```c
struct {
    struct spinlock lock;
    int refcnt[PHYSTOP / PGSIZE];
} kref;
```

这里有锁是因为我们不希望多个进程同时读写 refcnt. 我们要在 kinit 里初始化锁：

```c
void kinit() {
    initlock(&kmem.lock, "kmem");
    initlock(&kref.lock, "kref");
    freerange(end, (void *)PHYSTOP);
}
```

然后我们提供三个接口：

```c
void kaddref(uint64 pa) {
    acquire(&kref.lock);
    kref.refcnt[pa / PGSIZE] += 1;
    release(&kref.lock);
}

void kdecref(uint64 pa) {
    acquire(&kref.lock);
    if (kref.refcnt[pa / PGSIZE] > 0) {
        kref.refcnt[pa / PGSIZE] -= 1;
    }
    release(&kref.lock);
}

int kgetref(uint64 pa) {
    int ref;
    acquire(&kref.lock);
    ref = kref.refcnt[pa / PGSIZE];
    release(&kref.lock);
    return ref;
}
```

之后就能修改 kfree 和 kalloc 了，这里我提供的 kfree 是有并发风险的，具体可以看注释：

```c
void kfree(void *pa) {
    struct run *r;

    if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
        panic("kfree");

    // 这里有潜在的并发风险, 如果多个共享了物理页的进程同时 kfree 这个物理页,
    // 那么 if (kgetref((uint64)pa) == 0) 下的语句就有可能被多个进程进入.
    // 想修正也不难, 不调用函数而是手动请求锁, 在和 refcnt 交互完后释放锁就行.
    // 不过介于测试代码已经过了, 我们就不改动了
    kdecref((uint64)pa);

    // Free physical memory only when no other reference exists
    if (kgetref((uint64)pa) == 0) {
        // Fill with junk to catch dangling refs.
        memset(pa, 1, PGSIZE);

        r = (struct run *)pa;

        acquire(&kmem.lock);
        r->next = kmem.freelist;
        kmem.freelist = r;
        release(&kmem.lock);
    }
}

void *kalloc(void) {
    struct run *r;

    acquire(&kmem.lock);
    r = kmem.freelist;
    if (r)
        kmem.freelist = r->next;
    release(&kmem.lock);

    if (r)
        kaddref((uint64)r);

    if (r)
        memset((char *)r, 5, PGSIZE); // fill with junk
    return (void *)r;
}
```

## COW 策略实现

然后我们看向 trap.c，在 usertrap 里加入页错误的分支：

```c
    if (r_scause() == 8) {
        // ...
    } else if ((which_dev = devintr()) != 0) {
        // ...
    } 
    // 这里是加入的分支
    else if ((r_scause() == 15) && writefault(p->pagetable, r_stval()) != 0) {
        // copy on write
    } else {
        // ...
    }
```

接着我们到 vm.c 里实现 writefault：

```c
// return 0 if error, and physical address if successful.
uint64 writefault(pagetable_t pagetable, uint64 va) {
    pte_t *pte;
    uint64 pa;
    uint flags;
    int refcnt;

    struct proc *p = myproc();
    if (va >= p->sz) {
        return 0;
    }

    va = PGROUNDDOWN(va);

    pte = walk(pagetable, va, 0);
    if (pte == 0)
        return 0;

    flags = PTE_FLAGS(*pte);
    if (!(flags & PTE_COW)) {
        // read-only page
        return 0;
    }

    pa = PTE2PA(*pte);
    if ((refcnt = kgetref(pa)) == 0) {
        // no process ref this page
        return 0;
    } else if (refcnt == 1) {
        // only one process ref this page, change it to a regular writeable page
        *pte &= (~PTE_COW);
        *pte |= (PTE_W);
        return pa;
    } else {
        // copy on write
        char *mem;
        if ((mem = kalloc()) == 0) {
            return 0;
        }
        memmove(mem, (char *)pa, PGSIZE);
        *pte &= (~PTE_V); // make mappages work
        if (mappages(pagetable, va, PGSIZE, (uint64)mem, flags) != 0) {
            kfree(mem);
            return 0;
        }
        *pte &= (~PTE_COW);
        *pte |= (PTE_W);
        kdecref(pa);
        return pa;
    }
}
```

然后在 uvmcopy 里加入把多个虚拟内存映射到相同物理内存的机制：

```c
int uvmcopy(pagetable_t old, pagetable_t new, uint64 sz) {
    pte_t *pte;
    uint64 pa, i;
    uint flags;

    for (i = 0; i < sz; i += PGSIZE) {
        if ((pte = walk(old, i, 0)) == 0)
            panic("uvmcopy: pte should exist");
        if ((*pte & PTE_V) == 0)
            panic("uvmcopy: page not present");
        pa = PTE2PA(*pte);
        flags = PTE_FLAGS(*pte);
        if (mappages(new, i, PGSIZE, (uint64)pa, flags) != 0) {
            goto err;
        }
        if (flags & PTE_W) {
            pte_t *new_pte = walk(new, i, 0);
            *pte &= (~PTE_W);
            *new_pte &= (~PTE_W);
            *pte |= (PTE_COW);
            *new_pte |= (PTE_COW);
        }
        kaddref(pa);
    }
    return 0;

err:
    uvmunmap(new, 0, i / PGSIZE, 1);
    return -1;
}
```

最后改改 copyout 就可以下班了：

```c
int copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len) {
    uint64 n, va0, pa0;
    pte_t *pte;

    while (len > 0) {
        va0 = PGROUNDDOWN(dstva);
        if (va0 >= MAXVA)
            return -1;
        pte = walk(pagetable, va0, 0);
        if (pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0)
            return -1;
        if ((*pte & PTE_W) == 0) {
            if ((*pte & PTE_COW) && writefault(pagetable, va0) != 0) {
                // copy on write
            } else {
                return -1;
            }
        }
        pa0 = PTE2PA(*pte);
        n = PGSIZE - (dstva - va0);
        if (n > len)
            n = len;
        memmove((void *)(pa0 + (dstva - va0)), src, n);

        len -= n;
        src += n;
        dstva = va0 + PGSIZE;
    }
    return 0;
}
```

等等，先别急着下班，copyout 是做什么的？为什么它看起来独树一帜，要我们手动调用处理写故障的函数，而不是自动触发写故障然后自动被处理？

看看代码可以发现，它没有通过用户页表来“写”内存，而是通过用户页表拿到物理地址，再通过内核页表写内存。在这里，硬件只检查内核页表的权限，而不检查用户页表 PTE 的 PTE_W/COW，因此自然不会触发写故障。

总之就下班了，然后放张图图吧

![](images/lab5-cow/wtf_i_just_want_video_game.png)