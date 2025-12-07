# Lab 4 Traps

## RISC-V assembly

在 lab 开始时，我们要简单回答一些问题。这里只简单聊聊最后两道：

1. 下面的代码会打印什么？
    
    ```c
    unsigned int i = 0x00646c72;
    printf("H%x Wo%s", 57616, (char *) &i);
    ```
    
    57616 转换到十六进制是 E110，所以空格前打印的是 HE110.
    
    RISC-V 使用小端法，所以 `i` 在内存中的存储是 `72 6c 64 00`，所以空格后打印的是 rld.
    
    总之我们打印了 HE110, World!（以后咱就用这个去水贴
    
2. 下面的代码在 “y=” 后会打印什么？
    
    ```c
    printf("x=%d y=%d", 3);
    ```
    
    我们没有传入第三个参数，所以我们会把寄存器 a2 里的东西当作 int 来打印。
    

## Backtrace

这里要求我们实现一个 backtrace 函数，打印出当前调用栈。

### 解法

我们先来回顾一下 xv6 的栈结构：

```
                   .
                   .
      +->          .
      |   +-----------------+   |
      |   | return address  |   |
      |   |   previous fp ------+
      |   | saved registers |
      |   | local variables |
      |   |       ...       | <-+
      |   +-----------------+   |
      |   | return address  |   |
      +------ previous fp   |   |
          | saved registers |   |
          | local variables |   |
      +-> |       ...       |   |
      |   +-----------------+   |
      |   | return address  |   |
      |   |   previous fp ------+
      |   | saved registers |
      |   | local variables |
      |   |       ...       | <-+
      |   +-----------------+   |
      |   | return address  |   |
      +------ previous fp   |   |
          | saved registers |   |
          | local variables |   |
  $fp --> |       ...       |   |
          +-----------------+   |
          | return address  |   |
          |   previous fp ------+
          | saved registers |
  $sp --> | local variables |
          +-----------------+
```

可以概括算法流程如下：

1. 找到当前 stack frame 的 fp，这在初始化时就是找到寄存器 s0 里存储的指针。
2. 找到当前 stack frame 的 previous fp，判断 previous fp 和当前 fp 是否在一个 page 内
    1. 如果在，用 %p 打印这个 frame 存储的跳转值，然后继续循环
    2. 如果不在就结束循环

总之代码如下：

```c
void backtrace(void) {
    uint64 fp = r_fp();

    printf("backtrace:\n");
    for (uint64 cur_fp = fp; PGROUNDDOWN(cur_fp) == PGROUNDDOWN(fp);
         cur_fp = *(uint64 *)(cur_fp - 16)) {
        uint64 ret_addr = *(uint64 *)(cur_fp - 8);
        printf("%p\n", (void *)ret_addr);
    }
}
```

### 和 CSAPP 介绍的栈结构的对比

我们会注意到 xv6 的栈结构和 CSAPP 里介绍的略有差异，主要是 CSAPP 的栈帧不包含 `previous fp`。下面的是 CSAPP 介绍的栈结构

```
高地址
            |  参数7                |
            |  参数8                |
            |  ...                 |  ← 调用前push
            |  返回地址             |  ← call指令自动push
            |  保存的寄存器值        |  ← 刚进入函数时push
            |  本地变量1            |
            |  本地变量2            |
            |  ...                 |   ← 当前rsp指向这里
低地址
```

下面我们来编译 CSAPP 书中的一段代码：

```c
long caller()
{
    long arg1 = 534;
    long arg2 = 1057;
    long sum = swap_add(&arg1, &arg2);
    long diff = arg1 - arg2;
    return sum * diff;
}
```

这里是 CSAPP 书上给出的编译结果：

```nasm
caller:
    subq    $16, %rsp       ; Allocate 16 bytes for stack frame
    movq    $534, (%rsp)    ; Store 534 in arg1
    movq    $1057, 8(%rsp)  ; Store 1057 in arg2
    leaq    8(%rsp), %rsi   ; Compute &arg2 as second argument
    movq    %rsp, %rdi      ; Compute &arg1 as first argument
    call    swap_add        ; Call swap_add(&arg1, &arg2)
    movq    (%rsp), %rdx    ; Get arg1
    subq    8(%rsp), %rdx   ; Compute diff = arg1 - arg2
    imulq   %rdx, %rax      ; Compute sum * diff
    addq    $16, %rsp       ; Deallocate stack frame
    ret                     ; Return
```

这里则是在 xv6 里得到的编译结果：

```
0000000000000034 <caller>:

long caller()
  34:	1141                	addi	sp,sp,-16
  36:	e422                	sd	s0,8(sp)
  38:	0800                	addi	s0,sp,16
  3a:	000cb537          	  lui	a0,0xcb
  3e:	25d50513          	  addi	a0,a0,605 # cb25d <base+0xca24d>
  42:	6422                	ld	s0,8(sp)
  44:	0141                	addi	sp,sp,16
  46:	8082                	ret
```

可以注意到 CSAPP 只是把局部变量 arg1 和 arg2 存储在了栈中，而 xv6 的 `sd s0,8(sp)` 把调用者的栈帧指针压入了栈中。

产生这种差异的原因是编译器的优化，CSAPP 里的编译器认为通过 `subq $16, %rsp` 和 `addq $16, %rsp` 就能完成这个函数的工作，所以没有存储栈帧指针；xv6 则为了方便教学，包含了栈帧指针。

## Alarm

这里我们要实现一个叫做 `sigalarm(interval, handler)` 的系统调用，它会每隔 interval 个 ticks 调用一次 handler. 

每个 tick 都会产生一个中断，我们要做的就是每中断若干次调用一下 handler.

我们还需要实现 `sigreturn`，它是一个辅助 `sigalarm` 正确工作的系统调用。我们有这样的 promise——用户在调用 `sigalram` 时提供的 handler 函数必须在结尾调用 `sigreturn`。总之，`sigreturn` 的工作主要是把用户进程状态恢复到被中断时的状态。

### 开工！

我们可以按下面的流程完成任务：

1. 在开始之前：
    
    在 `usys.pl` 等地方做一些基建来把两个系统调用加入到系统里
    
2. 每隔若干 ticks 调用一次 handler：
    
    在 `proc.h` 里加入存储 interval、handler 以及距离上一次调用 handler 过去的 ticks 的字段
    
    在 `sysproc.c` 里实现 `sys_sigalarm`，它记录并存储用户传进来的 interval 和 handler
    
    在 `trap.c` 的 `usertrap` 里加入对 handler 的调用。还记得吗，当执行 SERT 时，程序计数器 pc 被设置为存储在 sepc 寄存器中的值，所以我们要修改 `p->trapframe->epc`.
    
3. 把用户进程状态恢复到被中断时的状态：
    
    我们需要保存的是用户中断时的 `struct trapframe`. 所以我们需要在 proc.h 里新增 `struct trapframe alarm_tf` 字段，在 handler 被调用时把 p->trapframe 复制到 alarm_tf，并在 `sys_sigreturn` 被调用时把 alarm_tf 复制回 p->trapframe.
    

我们就直接放最终代码了。

首先在 proc.h 的 struct proc 里新增这些字段：

```c
int alarm_freq;              // How many ticks to call alarm_handler once
uint64 alarm_handler;        // Handler's user virtual address
int tick_from_last;          // How many ticks have passed since the last call
int is_alarming;
struct trapframe alarm_tf;
```

然后在 sys_proc.c 里把用户传入的 interval 和 handler 保存下来，并初始化一些和调用 handler 相关的变量：

```c
uint64 sys_sigalarm(void) {
    int ticks;
    uint64 handler;

    argint(0, &ticks);
    argaddr(1, &handler);

    myproc()->alarm_freq = ticks;
    myproc()->alarm_handler = handler;
    myproc()->tick_from_last = 0;
    myproc()->is_alarming = 0;

    return 0;
}
```

之后修改 trap.c 完成对 handler 的调用以及对用户进程状态的保存：

```c
if (which_dev == 2) {
    if (p->alarm_freq != 0) {
        if (!p->is_alarming && p->tick_from_last >= p->alarm_freq &&
            p->tick_from_last % p->alarm_freq == 0) {
            p->is_alarming = 1;
            memmove(&p->alarm_tf, p->trapframe, sizeof(*p->trapframe));
            p->trapframe->epc = p->alarm_handler;
            p->tick_from_last = 0;
        } else {
            p->tick_from_last += 1;
        }
    }
    yield();
}
```

最后在返回时重置用户进程状态：

```c
uint64 sys_sigreturn(void) {
    struct proc *p = myproc();
    memmove(p->trapframe, &p->alarm_tf, sizeof(*p->trapframe));
    p->is_alarming = 0;
    return p->alarm_tf.a0;
}
```

### 一些小想法

为什么说“Prevent re-entrant calls to the handler----if a handler hasn't returned yet, the kernel shouldn't call it again. test2 tests this.”，即不能在上次对 handler 的调用完成前进行下次调用？我猜是因为我们会保存调用前的用户进程状态，而在上次 handler 完成前再次调用 handler 就会保存上次 handler 正在执行时的用户进程状态，这会覆盖最开始的用户进程状态。

再说句题外话，感觉 Page Tables 的 hard 的难度比这个难不少……果然 All hards are hard, but some hards are harder than others🫠🫠