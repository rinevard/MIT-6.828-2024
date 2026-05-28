# 系统调用流程

我们知道应用程序在User mode下运行，而系统函数的执行需要Supervisor mode，那在系统调用时，User mode是怎么进入Supervisor mode的呢？我们以sleep的执行为例，看看都发生了什么。首先我们用 `git checkout util` 切换到 util 分支上。

在此之前，我们要先简介一下**ECALL**指令和**stvec**（Supervisor Trap Vector Base Address Register）寄存器。

ECALL 会触发一个异常，如果我们在用户态触发这个异常，程序计数器 pc 就会根据 stvec 进行跳转，到达异常处理处。

以下是 [risc-v 手册](https://www.scs.stanford.edu/~zyedidia/docs/riscv/riscv-privileged.pdf) 对 ECALL 的解释：

> When executed in U-mode, S-mode, or M-mode, it generates an environment-call-from-U-mode exception, environment-call-from-S-mode exception, or environment-call-from-M-mode exception, respectively, and performs no other operation.

> 如果在 U-mode 执行，ECALL 指令会产生一个来自 U-mode 的环境调用异常；如果在 S-mode 执行，则产生来自 S-mode 的环境调用异常；如果在 M-mode 执行，则产生来自 M-mode 的环境调用异常。

以下是 risc-v 手册对 stvec 寄存器的简介：

![](images/syscall_process/stvec.png)

BASE是4字节对齐的（RISC-V 手册 P80），所以在MODE == 0时，我们跳转到的地址是 `(stvec >> 2) << 2`，而由于 MODE == 0，这个值就是 stvec ；在MODE == 1时，我们转到的地址是 `(stvec >> 2) << 2 + 4*cause`

接下来我们就可以开始看看系统调用时究竟发生了什么！我们会以sleep为例。

首先我们打开gdb-multiarch（参考 Note 0 的”用终端调试“），然后用 `set prompt \\001\\033[1;33m\\002(gdb) \\001\\033[0m\\002` 来高亮 “(gdb)” 以方便观察自己的输入。

用 `file user/_sleep` 切换到用户符号表，接着用`c`运行到xv6的shell启动。

观察右边的终端可以看到shell已经成功启动了。

![](images/syscall_process/debug_start.png)

然后我们在gdb里按下ctrl+c来中断，并用 `b main` 设置断点，再使用 `layout split` 让gdb显示出源码和汇编代码。接下来我们在gdb里输入 `c` 以让xv6继续执行，否则xv6的shell会处于暂停状态，不能处理输入。

然后在xv6的shell里执行 `sleep 1` ，它会在断点处停下。

观察左边的gdb，可以看到它停在了sleep.c的main函数。

![](images/syscall_process/sleep_main.png)

随便执行一下直到到达sleep这行，`n` 表示执行一行c语言，`si`表示执行一个汇编语句。

![](images/syscall_process/sleep_jal_atoi.png)

我们用`si`进入atoi，然后交替着用`n`和`si`，在快到return时用`si`，就能离开atoi回到sleep.c。

观察左边的gdb，和上图相比，虽然c语言的位置没有变化，但汇编代码的位置是有变化的。

![](images/syscall_process/sleep_jal_sleep.png)

之后用`si`，会发现我们跳转到了 `usys.S`，它把sleep的系统调用号放到寄存器a7里，然后借助ecall来执行系统调用。

观察左边的gdb发现我们已经进入了`usys.S`

![](images/syscall_process/usys_enter.png)

如果我们直接用几个`si`，会发现`ecall`并没有像`jal`之类的跳转语句一样带我们到一些神奇的地方，而是直接到了下一行。这是因为`ecall`不是跳转，而是抛出了一个异常，内核自动处理了异常。

这样看来，我们要找到这个异常的开始处并设置断点才行。还记得我们之前说过`ecall`抛出异常时pc会跳转到哪里吗？答案是它会根据寄存器stvec的值进行跳转。让我们借助`p /x $stvec`看看stvec的值。

观察左边的gdb，发现stvec的值是 0x3ffffff000，至少在我这里是这样。

![](images/syscall_process/print_stvec.png)

回顾一下stvec寄存器的结构和功能，会发现执行ecall后，pc会跳转到BASE所在的地址。我们之前说过，BASE是4字节对齐的（RISC-V 手册 P80），所以在MODE == 0时，我们跳转到的地址是 `(stvec >> 2) << 2`，而由于 MODE == 0，这个值就是 stvec。

![](images/syscall_process/stvec.png)

我们接下来把断点设置在这个值，然后用`si`到达`ecall`语句处

观察左边的gdb，我们现在已经在`ecall`这里了。

![](images/syscall_process/break_ecall_target.png)

如果我们的操作正确，接下来的`si`会触发一个异常导致我们跳转到 0x3ffffff000 并触发断点，希望我们没有翻车~

观察左边的gdb，发现我们成功跳转到了一个未知的地方！

按我的理解，由于我们在U-mode下执行ecall以触发异常，所以我们现在已经进入了S-mode。不过我翻了很多资料还是没有找到能明确支持这一点的证据（也没有找到明确反对的证据），所以我保留我的观点。

![](images/syscall_process/ecall.png)

总之一切顺利！让我们暂停一下，想想这个 0x3ffffff000 的地址代表什么。在内核启动时通过`file kernel/kernel` 加载内核符号表并在`usertrapret`设置断点，我们可以发现 0x3ffffff000 这个地址和 `kernel/trap.c` 里的`trampoline_uservec` 相等。

trampoline是什么？trampoline是在进程虚拟内存的顶部的一块空间，映射到的物理地址存放着跳转进和跳转出内核的代码。看起来在做系统调用时，我们通过ecall跳转进内核。这样一切都说得通了~

让我们打开trampoline.S，对比左边gdb显示的代码和右边的trampoline.S，会发现它们确实能对上！

好吧，也不是完全能对上，你会注意到右边的 `li a0,TRAPFRAME` 在左边似乎对应了三条语句，这是怎么回事呢？实际上，`li` 是RISC-V 汇编中的伪指令，实际执行时，`li` 会被汇编器翻译为一条或多条真正的 RISC-V 指令。

嗯，这样就能对上了！

![](images/syscall_process/trapoline_enter.png)

读一读trampoline.S，我们发现它会把用户进程的寄存器等信息保存到trapframe里，然后跳转到内核的usertrap函数。

因此接下来我们要用`file kernel/kernel`把符号表切换到kernel，再用`si n` 来一次执行多条汇编语句，直到指令`jr`处：

比较左边和右边，发现它们的汇编代码确实能对上，接下来我们要准备跳转了。

![](images/syscall_process/trapoline_jump.png)

陷阱，启动！

![](images/syscall_process/usertrap_enter.png)

如果你发现你的gdb没有顺利显示出c语言代码，可能是你忘记切换符号表到内核了，用`file kernel/kernel`来切换，然后补一个`si`就能显示出来了。

由于一切正常，我们可以一路按`n`直到到达syscall这里。

比较左边和右边的代码，它们是能对应上的。接下来让我们准备进入`syscall`

![](images/syscall_process/usertrap_jump.png)

我们用`s`进入syscall函数。`n`和`s`都会执行当前行，不过如果当前行是函数，`n`不会进入函数，而`s`会。

![](images/syscall_process/syscall_enter.png)

syscall函数从trapframe中取出系统调用号，然后调用它。还记得吗，我们调用系统函数时先进入了usys.S，然后把系统调用号保存到了a7。之后由于我们转入了内核，我们在trapoline.S里把所有的用户空间的寄存器都保存到了trapframe中。

总之我们在这里调用了sys_sleep，我们进入它看看。

![](images/syscall_process/syssleep_enter.png)

可以发现它就是真正干活的地方！它非常忠实地执行了sleep的逻辑。

我们可以注意到 n 就是存放着我们最开始传入的参数的变量，内核通过argint来找到我们一开始传入的参数。argint的思路和我们之前找到系统调用号类似，也是从trapframe中找到存放着传入参数的寄存器。

让我们继续往后，看看在执行完逻辑以后发生了什么吧。

![](images/syscall_process/syssleep_return.png)

回到`usertrap`，我们一路往下到达`usertrapret`，然后用`s`进入它。

![](images/syscall_process/usertrapret_enter.png)

看函数名上面的注释就能发现它会带我们回到用户空间。

我们用`n`一路运行到底，再看看注释，发现注释说我们会跳转到trampoline.S。

观察左边的gdb，`p /x trampoline_userret` 没有打印值是因为trampoline_userret被编译器优化掉了，我们要手动把它的表达式写出来再打印，总之我们打印出了 0x3ffffff09c。

在那里设个断点，然后准备继续。

![](images/syscall_process/usertrapret_break.png)

多按几个`si` 到达跳转语句处，不出意外的话再用一次`si` 就会带我们进入trapoline.S的userret部分了。

![](images/syscall_process/usertrapret_jump.png)

一切的一切都符合预期，我们成功进入了trampoline.S。

对比左右的汇编代码可以发现它们是对应的。与之前相同，`li`被展开了。

![](images/syscall_process/trapoline_enter_again.png)

然后我们一路按`si`到达`sret`语句处。`sret`会做什么呢？让我们看看riscv手册：

> The SRET instruction is used to return from a trap taken into S-mode. [...] When executing SRET, the privilege level is set to the value in the SPP field of the sstatus register; [...] the pc is set to the value stored in the sepc register.

> `SRET` 指令用于从进入 S-mode 的陷阱中返回。[...] 当执行 `SRET` 时，特权级别被设置为`sstatus` 寄存器中 `SPP` 字段的值；[...] 程序计数器 pc 被设置为存储在 `sepc` 寄存器中的值。

我们借助 `p /x sepc` 打印这个寄存器的值看看~（请忽视左图里我之前写成spec的手误）

![](images/syscall_process/trapoline_ret_user.png)

0x342是什么？如果你记忆力很好的话，会发现它恰好就是usys.S里的`ret`那行！

（这鬼才记得住啊喂）

总之我们看看之前的截图吧，我们可以发现0x342确实是ret那行，就是下面的截图中高亮的汇编代码下面那行。

![](images/syscall_process/hello_world_again.png)

我们用`b *0x342`设个断点在那里，然后执行`si` ，我们回到了用户空间！同时，`sret`也让我们回到了U-mode。

![](images/syscall_process/return_to_user.png)

用`file user/_sleep`切换符号表，再执行`si`，我们回来了。

![](images/syscall_process/im_back.png)

至此，我们就完成了一个完整的系统调用。

总结一下，当我们进行系统调用时，我们先进入usys.S，然后ecall触发异常并跳转到trapoline.S的uservec处，之后到达trap.c的usertrap函数，它会调用syscall函数以执行系统函数的逻辑，执行完后进入usertrapret函数，再跳转到trapoline.S的userret处，最后回到usys.S，再回到用户的代码里。

第一次跳转到trapoline.S主要是保存了用户的各个寄存器到trapframe，第二次跳转是从trapframe中恢复了这些寄存器。