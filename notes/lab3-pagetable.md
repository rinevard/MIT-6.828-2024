# Lab 3 Page Tables

## Inspect a user-process page table

这道题让我们解释一下 `print_pgtbl` 的打印结果，首先让我们来看看它打印了什么：

```
print_pgtbl starting
va 0x0 pte 0x21FCD85B pa 0x87F36000 perm 0x5B
va 0x1000 pte 0x21FD141B pa 0x87F45000 perm 0x1B
va 0x2000 pte 0x21FD1017 pa 0x87F44000 perm 0x17
va 0x3000 pte 0x21FD4007 pa 0x87F50000 perm 0x7
va 0x4000 pte 0x21FC70D7 pa 0x87F1C000 perm 0xD7
va 0x5000 pte 0x0 pa 0x0 perm 0x0
va 0x6000 pte 0x0 pa 0x0 perm 0x0
va 0x7000 pte 0x0 pa 0x0 perm 0x0
va 0x8000 pte 0x0 pa 0x0 perm 0x0
va 0x9000 pte 0x0 pa 0x0 perm 0x0
va 0xFFFF6000 pte 0x0 pa 0x0 perm 0x0
va 0xFFFF7000 pte 0x0 pa 0x0 perm 0x0
va 0xFFFF8000 pte 0x0 pa 0x0 perm 0x0
va 0xFFFF9000 pte 0x0 pa 0x0 perm 0x0
va 0xFFFFA000 pte 0x0 pa 0x0 perm 0x0
va 0xFFFFB000 pte 0x0 pa 0x0 perm 0x0
va 0xFFFFC000 pte 0x0 pa 0x0 perm 0x0
va 0xFFFFD000 pte 0x0 pa 0x0 perm 0x0
va 0xFFFFE000 pte 0x21FC94C7 pa 0x87F25000 perm 0xC7
va 0xFFFFF000 pte 0x2000184B pa 0x80006000 perm 0x4B
print_pgtbl: OK
```

结合 `print_pgtbl` 的实现，我们可以知道 `pgtbltest` 遍历了用户进程虚拟地址空间开头和末尾的一些地址，并打印出它们对应的 PTE、物理地址、权限位。让我们先来看看 PTE 的结构：

![](images/lab3-pagetable/pte.png)

然后我们以 `va 0x4000 pte 0x21FC70D7 pa 0x87F1C000 perm 0xD7` 为例，解析一下其打印的内容。首先我们从 PTE 中提取出 Physical Page Number（PPN）和权限位：

```
// 打印 PPN
// 虚拟地址的末尾 12 位是 0, 所以物理地址偏移量为 0
// 物理地址为 ((pte >> 10) << 12) + 0 即 0x87f1c000
(gdb) p /x (0x21FC70D7 >> 10)
$17 = 0x87f1c

// 构造一个末尾 10 位为 1 的量, 方便后续取出权限位
(gdb) set $mask = ((1 << 10) - 1)
(gdb) p /t $mask
$18 = 1111111111

// 分别用二进制和十六进制打印权限位
(gdb) p /t (0x21FC70D7 & $mask)
$19 = 11010111
(gdb) p /x (0x21FC70D7 & $mask)
$20 = 0xd7
```

检验可知和 `print_pgtbl` 的打印结果一致。对照权限位的末五位 10111 和 PTE 的末尾五位 UXWRV，可知这里是用户内存、不可执行、可写、可读、合法。

我们可以再检查一下 `va 0xFFFFF000 pte 0x2000184B pa 0x80006000 perm 0x4B`，会发现它的权限位末五位是 01011，说明这里是非用户内存、可以执行、不可写、可读、合法。

对照下面的用户进程虚拟内存空间图可知我们分析的第一段虚拟地址 `0x4000` 大约在开头位置，确实是用户的东西；而分析的第二段 `0xFFFFF000` 则在接近末尾的位置，用于存储系统调用相关的陷阱代码。

![](images/lab3-pagetable/user_addr_space.png)

我们还可以进一步打印一下内存里的值。启动 gdb，用 `file user/_pgtbltest` 加载符号表，然后在 `main` 设置断点，这样我们就能在加载用户页表时打印各个虚拟地址的值：

```
(gdb) p *0x0
$7 = -335146751
(gdb) p *0x1000
$8 = 72
(gdb) p *0x5000
Cannot access memory at address 0x5000
(gdb) p *0xFFFFE000
Cannot access memory at address 0xffffe000
```

对照最开始的 `print_pgtbl` 的打印结果（关注其权限位），可以看到我们能正确打印 `0x0` 和 `0x1000` ，它们是用户内存、可读、合法的虚拟地址的内容。而 `0x5000` 是不合法的地址所以无法打印，`0xFFFFE000` 是非用户内存（内核内存）所以无法打印。

## Speed up system calls

这道题让我们创建一个特殊的内存页并把进程 pid 存储在里面，这样我们在获取 pid 时就不必做系统调用 `getpid`，而是可以直接调用 `ugetpid`，从而无需陷入内核以提高效率。

提示里说我们创建的这个页面会和 `trapframe` 有很多相似之处——它们都在进程初始化时自动创建，在进程退出时自动释放。所以我们可以分为以下几步完成任务：

1. 定义 `usyscall`：在 `struct proc` 里添加 `struct usyscall *usyscall`
2. 进程初始化时创建页面：阅读 `fork` 的代码可以知道我们调用 `allocproc` 以创建进程。
    
    在 `allocproc` 里，我们用 `kalloc` 给 `trapframe` 分配了一个物理页，然后调用 `proc_pagetable` 来做页表映射。
    
    ```c
    static struct proc *allocproc(void) {
        // ...
        // Allocate a trapframe page.
        if ((p->trapframe = (struct trapframe *)kalloc()) == 0) {
            freeproc(p);
            release(&p->lock);
            return 0;
        }
    
        // An empty user page table.
        p->pagetable = proc_pagetable(p);
        // ...
    ```
    
    所以照葫芦画瓢就行，先在 `allocproc` 里加入给 `usyscall` 分配物理页的代码（记得把 `pid` 存进去）：
    
    ```c
    // Allocate a usyscall page
    if ((p->usyscall = (struct usyscall *)kalloc()) == 0) {
        freeproc(p);
        release(&p->lock);
        return 0;
    }
    p->usyscall->pid = p->pid;
    ```
    
    然后在 `proc_pagetable` 加入页表映射的代码：
    
    ```c
    // map the usyscall page just below the trapframe page, for
    // data sharing between userspace and the kernel
    if (mappages(pagetable, USYSCALL, PGSIZE, (uint64)(p->usyscall),
                 PTE_R | PTE_U) < 0) {
        uvmunmap(pagetable, TRAPFRAME, 1, 0);
        uvmunmap(pagetable, TRAMPOLINE, 1, 0);
        uvmfree(pagetable, 0);
        return 0;
    }
    ```
    
3. 退出时释放：我们知道进程调用 `exit` 来下班，但其实调用 `exit` 后进程并没有立即销毁自身，而是进入 ZOMBIE 态等待父进程的 `wait` 来回收。具体可以看看 `proc.c`，这里不多赘述。
    
    总之看向 `wait` 函数，可以发现我们调用 `freeproc` 来释放进程。
    
    ```c
    static void freeproc(struct proc *p) {
        if (p->trapframe)
            kfree((void *)p->trapframe);
        p->trapframe = 0;
        if (p->pagetable)
            proc_freepagetable(p->pagetable, p->sz);
        // ...
    ```
    
    对照代码对 `trapframe` 的处理，我们要在 `freeproc` 里加入释放物理页的代码：
    
    ```c
    if (p->usyscall)
        kfree((void *)p->usyscall);
    p->usyscall = 0;
    ```
    
    然后修改 `proc_freepagetable` 以删除页表映射：
    
    ```c
    uvmunmap(pagetable, USYSCALL, 1, 0);
    ```
    

闲着也是闲着，不如和我一起看看 `uvmfree` 为什么没有自动释放 `trapframe` 和 `usyscall`：

```c
void uvmfree(pagetable_t pagetable, uint64 sz) {
    if (sz > 0)
        uvmunmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1);
    freewalk(pagetable);
}
```

这段代码调用 `uvmunmap` 来释放所有在页表里有记录的物理页，然后调用 `freewalk` 来释放页表自身。

这里的 `uvmunmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1)` 表示释放虚拟地址从 `0` 到 `0 + PGROUNDUP(sz) / PGSIZE` 的内容，即释放用户内存的内容。

但 `trapframe` 和 `usyscall` 不是用户内存的内容，它们被存放在虚拟地址的顶部（分别在 TRAPFRAME 和 USYSCALL），所以它们没有被释放。

## Print a page table

先来看看打印结果吧：

```
page table 0x0000000087f22000
 ..0x0000000000000000: pte 0x0000000021fc7801 pa 0x0000000087f1e000
 .. ..0x0000000000000000: pte 0x0000000021fc7401 pa 0x0000000087f1d000
 .. .. ..0x0000000000000000: pte 0x0000000021fc7c5b pa 0x0000000087f1f000
 .. .. ..0x0000000000001000: pte 0x0000000021fc701b pa 0x0000000087f1c000
 .. .. ..0x0000000000002000: pte 0x0000000021fc6cd7 pa 0x0000000087f1b000
 .. .. ..0x0000000000003000: pte 0x0000000021fc6807 pa 0x0000000087f1a000
 .. .. ..0x0000000000004000: pte 0x0000000021fc64d7 pa 0x0000000087f19000
 ..0xffffffffc0000000: pte 0x0000000021fc8401 pa 0x0000000087f21000
 .. ..0xffffffffffe00000: pte 0x0000000021fc8001 pa 0x0000000087f20000
 .. .. ..0xffffffffffffd000: pte 0x0000000021fd4c13 pa 0x0000000087f53000
 .. .. ..0xffffffffffffe000: pte 0x0000000021fd00c7 pa 0x0000000087f40000
 .. .. ..0xfffffffffffff000: pte 0x000000002000184b pa 0x0000000080006000
```

结果比 2024 的作业文档多了一行

```
 .. .. ..0x0000000000000000: pte 0x0000000021fc7c5b pa 0x0000000087f1f000
```

这是正确表现。2024 的作业文档这里写错了，2025 的版本就加入了这行。

如提示所言，这个函数和 `freewalk` 很像，我们先来看看 `freewalk`：

```c
void freewalk(pagetable_t pagetable) {
    // there are 2^9 = 512 PTEs in a page table.
    for (int i = 0; i < 512; i++) {
        pte_t pte = pagetable[i];
        if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0) {
            // this PTE points to a lower-level page table.
            uint64 child = PTE2PA(pte);
            freewalk((pagetable_t)child);
            pagetable[i] = 0;
        } else if (pte & PTE_V) {
            panic("freewalk: leaf");
        }
    }
    kfree((void *)pagetable);
}
```

它遍历 L0 级页表的 512 个 PTE，对每个 PTE，用 `PTE2PA(pte)` 找到它指向的次级页表的物理地址，然后递归。它忽略了叶子 PTE，在打印时我们不需要忽略叶子。

我们再来看看打印结果的形式

```c
.. 虚拟地址: pte PTE的值 pa 物理地址的值
```

参考 `freewalk` 我们就能拿到 PTE，调用 `PTE2PA` 就能得到 `pa`，但怎么获得虚拟地址呢？我们来看看虚拟地址的翻译：

![](images/lab3-pagetable/addr_translation.png)

从这里可以看出，如果把 Offset 置零，我们需要 L2、L1、L0 页表的 PTE 的索引来确定虚拟地址。但我们当然有办法获得这些索引，我们不是在遍历页表吗，`for (int i = 0; i < 512; i++)` 里的 `i` 就是我们需要的索引！所以如果我们的索引是 `i, j, k`，我们打印的虚拟地址就是 `((i << 30) + (j << 21) + (k << 12))`

大致思路就是这样。还有一些小细节：

1. 作业文档里在遍历 L2 和 L1 级页表时也打印了虚拟地址，这里打印的地址是把次级索引当作零算出的地址。比如如果我们在遍历 L1 页表，L2 页表的索引是 `i`，L1 页表的索引是 `j`，打印的就是 `((i << 30) + (j << 21) + (0 << 12))`
2. 为了在递归时得知上一级页表的信息，我们加入了 `father_va` 用于记录上一级的虚拟地址
3. 符号拓展用了一些 tricky 的性质，在 255 << 30 那行的注释里写的应该还算清楚

总之可以写出下面的代码：

```c
const int MAX_LEVEL = 2;
void _vmprint_helper(pagetable_t pagetable, uint64 father_va, int level) {
    // there are 2^9 = 512 PTEs in a page table.
    for (int i = 0; i < 512; i++) {
        pte_t pte = pagetable[i];
        if (pte & PTE_V) {
            for (int i = level; i <= MAX_LEVEL; i++) {
                printf(" ..");
            }
            // 255 << 30 is automatically sign extended to 0xffffffffc0000000
            uint64 va = father_va + (i << (12 + 9 * level));
            printf("%p: pte %p pa %p\n", (void *)va, (void *)pte,
                   (void *)PTE2PA(pte));
            // this PTE points to a lower-level page table.
            if (level != 0) {
                _vmprint_helper((pagetable_t)PTE2PA(pte), va, level - 1);
            }
        }
    }
}
void vmprint(pagetable_t pagetable) {
    printf("page table %p\n", pagetable);
    _vmprint_helper(pagetable, 0, MAX_LEVEL);
}
```

## Use superpages

这道题让我们给 xv6 加入超级页。当用户请求一块超过 2MB 的内存，我们就分配超级页而非大量的小页来优化性能。

我们先来看看普通页和超级页各自是如何翻译虚拟地址的：

![](images/lab3-pagetable/regular_vs_super.png)

1. 普通页：
    
    普通页在翻译时把虚拟地址分成 L2, L1, L0, Offset 四个部分。前三者用于在不同级别的页表里做偏移找到下一个页的开头的物理地址（一个 Page Directory 也是一个页），Offset 则用于计算最终的物理地址偏移。
    
    Sv39 规定 PPN 宽度为 44 位，Offset 宽度 12 位，一个普通页的大小就是 $2^{12}$ bytes = 4KB.
    
    在找到了 L0 leaf 后，我们就能用 `(PPN << 12) + Offest` 来计算虚拟地址对应的物理地址了。
    
2. 超级页：
    
    超级页则把虚拟地址分成 L2, L1, Offset 三个部分。
    
    Sv39 规定超级页的 Offset 为 21 位，这是把原本的 L0 和 Offset 合并成了一个 21 位的 Offset。超级页大小就是 $2^{21}$ bytes 即 2MB.
    
    超级页是 2MB 对齐的，所以我们需要 $56 - \log (2097152) = 35$ 位来描述一个超级页的开头，所以超级页对应的 L1 leaf 存储的 PPN 的末尾 9 位为 0. 这里有 $2097152$ 是因为 2MB = 2097152 bytes.
    
    在找到 L1 leaf 后，我们同样用 `(PPN << 12) + Offest` 来计算虚拟地址对应的物理地址。
    

### 测试代码

遵循提示，我们先来看看 `superpg_test` 做了什么。要说明的是，2024 版本的测试不全面而且有 bug——在 `supercheck` 函数的末尾的 for 循环里，条件应该是 `i < 512 * PGSIZE` 而非 `i < 512`，应该改成下面这样：

```c
// check whether different va are mapped to different pa
for (int i = 0; i < 512 * PGSIZE; i += PGSIZE) {
    *(int *)(s + i) = i;
}
for (int i = 0; i < 512 * PGSIZE; i += PGSIZE) {
    if (*(int *)(s + i) != i)
        err("wrong value");
}
```

总之我们下面分析 2025 版本的测试。2025 版本的测试共包括 `supercheck`、`superpg_fork` 和 `superpg_free` 三个函数，我们主要关注 `supercheck`。`supercheck` 判断从 `end` 之后第一个对齐超级页的地址开始是否是超级页，具体的判断方法可以参考下面的注释：

```c
void supercheck(char *end) {
    pte_t last_pte = 0;
    uint64 a = (uint64)end;
    uint64 s = SUPERPGROUNDUP(a);

    // Check that virtual address up to the next superpage boundary are mapped to PTE
    for (; a < s; a += PGSIZE) {
        pte_t pte = (pte_t)pgpte((void *)a);
        if (pte == 0) {
            err("no pte");
        }
    }

    // Check that all virtual address in the superpage share the same valid PTE
    for (uint64 p = s; p < s + 512 * PGSIZE; p += PGSIZE) {
        pte_t pte = (pte_t)pgpte((void *)p);
        if (pte == 0)
            err("no pte");
        if ((uint64)last_pte != 0 && pte != last_pte) {
            err("pte different");
        }
        if ((pte & PTE_V) == 0 || (pte & PTE_R) == 0 || (pte & PTE_W) == 0) {
            err("pte wrong");
        }
        last_pte = pte;
    }

    // Check that different va are mapped to different pa
    for (int i = 0; i < 512 * PGSIZE; i += PGSIZE) {
        *(int *)(s + i) = i;
    }
    for (int i = 0; i < 512 * PGSIZE; i += PGSIZE) {
        if (*(int *)(s + i) != i)
            err("wrong value");
    }
}
```

然后我们简要提一下另外两个函数的作用：

1. `superpg_fork` 检查超级页在 `fork` 后是否被正确复制为超级页。
2. `superpg_free` 检查 `sbrk` 能否正确释放整个超级页，以及能否在释放超级页的部分内存时将超级页退化为多个普通页（2024 版本没测试这一点，但这个是很值得做的功能）。

### sbrk 调用链

接下来我们分析用户在调用 `sbrk` 时具体调用了哪些函数。这分为两种情况：

1. 分配：sys_sbrk->growproc->uvmalloc -> kalloc/kfree
2. 释放：sys_sbrk -> growproc -> uvmdealloc -> uvmunmap

要注意的是当分配内存出错时，`uvmalloc` 也会调用 `uvmdealloc`。

我们从最底层出发，先在 kalloc.c 里完成对超级页的支持。这包括以下步骤：

1. 添加一个超级页链表
    
    ```c
    struct {
        struct spinlock lock;
        struct run *superlist;
        struct run *freelist;
    } kmem;
    ```
    
2. 在初始化代码里初始化这个超级页链表
    
    ```c
    const int MAX_SUPER = 32;
    void freerange(void *pa_start, void *pa_end) {
        char *p;
        p = (char *)PGROUNDUP((uint64)pa_start);
        int sz;
        int super_cnt = 0;
    
        for (; p + PGSIZE <= (char *)pa_end; p += sz) {
            if (super_cnt < MAX_SUPER && (uint64)p % SUPERPGSIZE == 0 &&
                p + SUPERPGSIZE <= (char *)pa_end) {
                sz = SUPERPGSIZE;
                superfree(p);
                super_cnt += 1;
            } else {
                sz = PGSIZE;
                kfree(p);
            }
        }
    }
    ```
    
3. 添加 superalloc 和 superfree 函数（照抄 kalloc 和 kfree）：
    
    ```c
    void *superalloc(void) {
        struct run *r;
    
        acquire(&kmem.lock);
        r = kmem.superlist;
        if (r)
            kmem.superlist = r->next;
        release(&kmem.lock);
    
        if (r)
            memset((char *)r, 5, SUPERPGSIZE); // fill with junk
        return (void *)r;
    }
    
    void superfree(void *pa) {
        struct run *r;
    
        if (((uint64)pa % SUPERPGSIZE) != 0 || (char *)pa < end ||
            (uint64)pa >= PHYSTOP) {
            panic("superfree");
        }
    
        // Fill with junk to catch dangling refs.
        memset(pa, 1, SUPERPGSIZE);
    
        r = (struct run *)pa;
    
        acquire(&kmem.lock);
        r->next = kmem.superlist;
        kmem.superlist = r;
        release(&kmem.lock);
    }
    ```
    

至此，最底层的工作就做完了。

### 分配内存

回顾内存分配的调用链：sys_sbrk->growproc->uvmalloc -> kalloc/kfree，在开始编写代码之前我们先分析一下每一层的职责。

1. sys_sbrk 是系统调用，负责系统和用户的交互
2. grow_proc 是进程抽象的一个接口，负责管理进程状态
3. uvmalloc 是虚拟内存层，负责管理进程眼中的“内存”，管理虚拟到物理的映射
4. kalloc 和 kfree 则是物理内存层，管理物理内存

我们在这里要修改的是 uvmalloc. 简单读下它原本的代码可以发现它主要调用的是 kalloc 和 mappages. 在超级页中，前者对应 superalloc，而对后者我自己写了一个 mapsuperpages 作为对应。为了让 mapsuperpages 跑起来，我还写了个 l1walk 函数用于找到一个虚拟地址对应的 L1 PTE. 先来看看 l1walk：

```c
pte_t *l1walk(pagetable_t pagetable, uint64 va, int alloc) {
    if (va >= MAXVA)
        panic("walk");

    pte_t *pte = &pagetable[PX(2, va)];
    if (*pte & PTE_V) {
        pagetable = (pagetable_t)PTE2PA(*pte);
        return &pagetable[PX(1, va)];
    } else {
        if (!alloc || (pagetable = (pde_t *)kalloc()) == 0) {
            return 0;
        }
        memset(pagetable, 0, PGSIZE);
        *pte = PA2PTE(pagetable) | PTE_V;
        return &pagetable[PX(1, va)];
    }
}
```

它和 walk 函数基本一致，不过 walk 是返回 leaf，而这里的 l1walk 返回 L1 PTE.

然后看看 mapsuperpages：

```c
int mapsuperpages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa,
                  int perm) {
    uint64 a, last;
    pte_t *pte;

    if ((va % SUPERPGSIZE) != 0)
        panic("mapsuperpages: va not aligned");

    if ((size % SUPERPGSIZE) != 0)
        panic("mapsuperpages: size not aligned");

    if (size == 0)
        panic("mapsuperpages: size");

    a = va;
    last = va + size - SUPERPGSIZE;
    for (;;) {
        if ((pte = l1walk(pagetable, a, 1)) == 0) {
            printf("mapsuperpages: l1walk failed\n");
            return -1;
        }
        if (*pte & PTE_V)
            panic("mapsuperpages: remap");
        *pte = PA2PTE(pa) | perm | PTE_V | PTE_R;
        if (a == last)
            break;
        a += SUPERPGSIZE;
        pa += SUPERPGSIZE;
    }
    return 0;
}
```

这基本是照抄 mappages，只是把普通页改成了超级页。

最后看看我们对 uvmalloc 的修改。uvmalloc 主要分为两部分，第一部分是用 kalloc/superalloc 请求物理内存，第二部分是用 mappages/mapsuperpages 把虚拟内存映射到物理内存上。

我们在注释里标出了修改的地方，修改 1 对应请求物理内存的第一部分，修改 2 对应做映射的第二部分：

```c
uint64 uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm) {
    char *mem;
    uint64 a;
    int sz;

    if (newsz < oldsz)
        return oldsz;

    oldsz = PGROUNDUP(oldsz);

    for (a = oldsz; a < newsz; a += sz) {
        // 修改1从这里开始
        if (a % SUPERPGSIZE == 0 && a + SUPERPGSIZE <= newsz) {
            sz = SUPERPGSIZE;
            mem = superalloc();
            // If no superpages, use regular pages
            if (mem == 0) {
                sz = PGSIZE;
                mem = kalloc();
            }
        } else {
            sz = PGSIZE;
            mem = kalloc();
        }
        // 修改1到这里结束
        if (mem == 0) {
            uvmdealloc(pagetable, a, oldsz);
            return 0;
        }
#ifndef LAB_SYSCALL
        memset(mem, 0, sz);
#endif
        // 修改2从这里开始
        if (sz == PGSIZE) {
            if (mappages(pagetable, a, sz, (uint64)mem,
                         PTE_R | PTE_U | xperm) != 0) {
                kfree(mem);
                uvmdealloc(pagetable, a, oldsz);
            }
        } else {
            if (mapsuperpages(pagetable, a, sz, (uint64)mem,
                              PTE_R | PTE_U | xperm) != 0) {
                superfree(mem);
                uvmdealloc(pagetable, a, oldsz);
            }
        }
        // 修改2到这里结束
    }
    return newsz;
}
```

### 释放内存

在完成了分配内存的工作后，我们就能写释放内存相关的代码了。

回顾调用链：sys_sbrk -> growproc -> uvmdealloc -> uvmunmap，我们要修改 uvmunmap. 这部分代码比较棘手。它原本只是在按部就班地删掉页表里从 va 出发的 npages 个物理页的映射，并选择性释放物理内存，但在加入超级页后它需要完成这些工作：

1. 让 `uint64 a` 从 `va` 出发，如果遇到了普通页，走普通页的 unmap 流程，然后 `a += PGSIZE`
2. 如果遇到了超级页，判断 `a` 是超级页的开头还是超级页的中间
    1. 如果是超级页的开头，走超级页的 unmap 流程（和普通页 unmap 基本一致），然后 `a += SUPERPGSIZE`
    2. 如果是超级页的中间，把超级页退化成若干个普通页，再走普通页释放流程，然后 `a += PGSIZE`

这里主要是上面步骤中的 2b 的“退化”比较棘手，我是把页表里对应超级页的 L1 leaf 设置成了 invalid，然后把超级页视作多个普通页的合并，从超级页的开头出发做若干次 mappages 从而完成退化。思路可以参考下图：

![](images/lab3-pagetable/super_demote.png)

这里是退化部分的代码：

```c
int perm = PTE_FLAGS(*pte);
uint64 super_va_st = SUPERPGROUNDDOWN(a);
uint64 super_pa_st = PTE2PA(*pte);
*pte = (*pte >> 1) << 1; // Make pte invalid to make `mappages` work
for (uint64 super_va = super_va_st, super_pa = super_pa_st;
     super_va < super_va_st + SUPERPGSIZE;
     super_va += PGSIZE, super_pa += PGSIZE) {
    if (mappages(pagetable, super_va, PGSIZE, super_pa, perm) !=
        0) {
        panic("uvmunmap: mappages failed");
        
    }
}
```

当然这样的退化有一个潜在风险——每个进程的超级页上限在进程启动时就决定了，我们的退化会让本进程的超级页上限减一。如果有什么自动合并普通页为超级页的功能就好了…

另外，我的代码把之前步骤里描述的 1 和 2b 整合了一下（毕竟它们最后都走的是普通页释放流程），总之这里是完整代码：

```c
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free) {
    uint64 a;
    pte_t *pte;
    int sz;

    if ((va % PGSIZE) != 0)
        panic("uvmunmap: not aligned");

    for (a = va; a < va + npages * PGSIZE; a += sz) {
        // If `a` corresponds to a superpage and the superpage starts at `a`,
        // free the superpage
        if ((pte = l1walk(pagetable, a, 0)) != 0 && PTE_LEAF(*pte) &&
            a % SUPERPGSIZE == 0) {
            sz = SUPERPGSIZE;
            if ((*pte & PTE_V) == 0) {
                printf("va=%ld pte=%ld\n", a, *pte);
                panic("uvmunmap: not mapped");
            }
            if (do_free) {
                uint64 pa = PTE2PA(*pte);
                superfree((void *)pa);
            }
            *pte = 0;
        } else {
            sz = PGSIZE;
            // If `a` corresponds to a superpage but the superpage doesn't
            // start at `a`, demote the superpage into regular pages
            if (pte != 0 && PTE_LEAF(*pte)) {
                // Potential issue:
                // Each process has a max number of superpages defined as
                // `MAX_SUPER`. But whenever a superpage is demoted, the
                // process's max number of superpages permanently minus one,
                // since we view superpage as multiple regular pages.
                //
                // A possible solution is to kalloc multiple regular pages,
                // copy memory into them, and superfree the superpage.
                int perm = PTE_FLAGS(*pte);
                uint64 super_va_st = SUPERPGROUNDDOWN(a);
                uint64 super_pa_st = PTE2PA(*pte);
                *pte = (*pte >> 1)
                       << 1; // Make pte invalid to make `mappages` work
                for (uint64 super_va = super_va_st, super_pa = super_pa_st;
                     super_va < super_va_st + SUPERPGSIZE;
                     super_va += PGSIZE, super_pa += PGSIZE) {
                    if (mappages(pagetable, super_va, PGSIZE, super_pa, perm) !=
                        0) {
                        panic("uvmunmap: mappages failed");
                    }
                }
            }
            // Free the regular page that begins at `a`
            if ((pte = walk(pagetable, a, 0)) == 0)
                panic("uvmunmap: walk");
            if ((*pte & PTE_V) == 0) {
                printf("va=%ld pte=%ld\n", a, *pte);
                panic("uvmunmap: not mapped");
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
}
```

### fork

最艰难的工作已经做完了，我们只要修改 uvmcopy 就能下班了！这里的修改只是加入一个简单的分类，在遇到普通页时走普通流程，遇到超级页时走超级流程。咱就直接放代码了：

```c
int uvmcopy(pagetable_t old, pagetable_t new, uint64 sz) {
    pte_t *pte;
    uint64 pa, i;
    uint flags;
    char *mem;
    int szinc;

    for (i = 0; i < sz; i += szinc) {
        // super page case
        if ((pte = l1walk(old, i, 0)) != 0 && PTE_LEAF(*pte)) {
            szinc = SUPERPGSIZE;
            if ((*pte & PTE_V) == 0)
                panic("uvmcopy: page not present");
            pa = PTE2PA(*pte);
            flags = PTE_FLAGS(*pte);
            if ((mem = superalloc()) == 0)
                goto err;
            memmove(mem, (char *)pa, SUPERPGSIZE);
            if (mapsuperpages(new, i, SUPERPGSIZE, (uint64)mem, flags) != 0) {
                superfree(mem);
                goto err;
            }
        }
        // regular page case
        else {
            szinc = PGSIZE;
            if ((pte = walk(old, i, 0)) == 0)
                panic("uvmcopy: pte should exist");
            if ((*pte & PTE_V) == 0)
                panic("uvmcopy: page not present");
            pa = PTE2PA(*pte);
            flags = PTE_FLAGS(*pte);
            if ((mem = kalloc()) == 0)
                goto err;
            memmove(mem, (char *)pa, PGSIZE);
            if (mappages(new, i, PGSIZE, (uint64)mem, flags) != 0) {
                kfree(mem);
                goto err;
            }
        }
    }
    return 0;

err:
    uvmunmap(new, 0, i / PGSIZE, 1);
    return -1;
}
```