在开始 lab 之前，我们先来解释一下为什么我们写的代码能直接在 xv6 的 shell 里执行。我们来看看 `sh.c` 的部分代码：

```c
// Read and run input commands.
while (getcmd(buf, sizeof(buf)) >= 0) {
    if (buf[0] == 'c' && buf[1] == 'd' && buf[2] == ' ') {
        // Chdir must be called by the parent, not the child.
        buf[strlen(buf) - 1] = 0; // chop \n
        if (chdir(buf + 3) < 0)
            fprintf(2, "cannot cd %s\n", buf + 3);
        continue;
    }
    if (fork1() == 0)
        runcmd(parsecmd(buf));
    wait(0);
}
```

从这里我们看出，shell通过fork子进程来执行命令。如果进一步看看 `runcmd` 函数，就会发现它调用了 `exec`，因此 shell 能执行用户写的代码。

有趣的是，从这段代码里我们也能看出 `cd` 是内置在 shell 里的命令，这是因为 cd 改变了shell自身的工作目录。

# sleep

这题让我们简单熟悉下接口。其实这里用 `printf` 更易读，不过我当时做的时候没发现有 `printf` 

```c
#include "kernel/types.h"
#include "user/user.h"
#include "kernel/fcntl.h"

int main(int argc, char *argv[]) {
    char *errmsg = "sleep: missing operand\n";
    if (argc != 2) {
        write(1, errmsg, strlen(errmsg));
        exit(1);
    }
    sleep(atoi(argv[1]));
    exit(0);
}
```

# **pingpong**

这题让我们接触一下管道的使用，难者不会，会者不难

```c
#include "kernel/types.h"
#include "user/user.h"
#include "kernel/fcntl.h"

int main() {
    char buf[512];
    int p_parent_sender[2];
    int p_child_sender[2];

    pipe(p_parent_sender);
    pipe(p_child_sender);

    if (fork() == 0) {
        // 子进程先读
        read(p_parent_sender[0], buf, 2);
        printf("%d: received ping\n", getpid());
        write(p_child_sender[1], buf, 2);

        close(p_parent_sender[0]);
        close(p_parent_sender[1]);
        close(p_child_sender[0]);
        close(p_child_sender[1]);
        exit(0);
    }

    // 父进程先写
    write(p_parent_sender[1], buf, 2);
    read(p_child_sender[0], buf, 2);
    printf("%d: received pong\n", getpid());

    close(p_parent_sender[0]);
    close(p_parent_sender[1]);
    close(p_child_sender[0]);
    close(p_child_sender[1]);
    exit(0);
}
```

# primes

这题很有趣，是一个并发素数筛法。这个算法理论上是可以提高效率的，毕竟在运行了一段时间后，各个进程里都有一些数等待筛选，这时各个进程在同时用自己的素数筛选输入的数。

![](../figures/labs/lab1%20util/prime-concurrent.png)

网上很多代码都是2020版的，当时的要求是找出 2-35 间的素数。这些代码能过老版本的要求，但过不了2024版要求的 2-280。我认真看了一两份代码，发现它们在用 fork 创建子进程的子进程后，忘了关闭子子进程的连接到父进程的描述符。可以看下面示意图的第三个框，这些代码忘了关闭 child child process 的 fd_read.

我画了示意图来解释我的代码\~

![](../figures/labs/lab1%20util/prime-code.png)

```c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

const int NUM = 280;

int connected_fork(int *);
void do_child(int);

int main() {
    int pid;
    int fd = -1;
    if ((pid = connected_fork(&fd)) == 0) {
        do_child(fd);
        exit(0);
    }

    int n;
    for (n = 2; n <= NUM; n++) {
        write(fd, (void *)&n, sizeof(int));
    }

    close(fd);
    wait(0);
    exit(0);
}

/*
 * 创建子进程. 父进程的 fd 和子进程的 fd 会被分别设置为一个 pipe 的两端.
 * 对父进程, fd 被设置为写端.
 * 对子进程, fd 被设置为读端.
 *
 * return 0 if is child else child's pid
 */
int connected_fork(int *fd) {
    int p[2];
    pipe(p);
    int pid;

    if ((pid = fork()) == 0) {
        // child
        close(p[1]);
        *fd = p[0];
    } else {
        // parent
        close(p[0]);
        *fd = p[1];
    }

    return pid;
}

/*
 * 从 fd_read 中读取数字, 打印第一个数,
 * 筛选其他数并新建子进程把被筛选后的数写入子进程. 在运行完成后关闭 fd_read.
 */
void do_child(int fd_read) {
    int n = -1;
    int prime = -1;
    int fd = -1;

    while (read(fd_read, (void *)&n, sizeof(int)) > 0) {
        if (prime == -1) {
            prime = n;
            printf("prime %d\n", prime);
        }
        if ((n % prime) != 0) {
            if (fd == -1 && connected_fork(&fd) == 0) {
                // fd == -1 等价于没有子进程
                // 如果没有子进程就创建子进程并让它开始工作
                close(fd_read);
                do_child(fd);
                return;
            }
            write(fd, (void *)&n, sizeof(int));
        }
    }
    close(fd_read);
    // fd == -1 等价于没有子进程, 说明它是最后一个进程
    // 最后一个进程不需要关闭描述符, 也不需要等待
    if (fd == -1) {
        return;
    }
    close(fd);
    wait(0);
}

/*
 * 个人认为关闭 fd_read 不应该是 do_child 的工作, 我觉得"谁创建,
 * 谁关闭"会更合适. 也就是说, 我觉得让调用 do_child 的函数关闭 fd_read
 * 更合适.
 *
 * 但如果 do_child 不关闭 fd_read, 子孙进程就会保留父进程未关闭的描述符,
 * 从而耗尽 xv6 的资源.
 */
```

# find

在 `ls.c` 的基础上稍微改改就好了，要注意的是递归时不要递归进 “.” 和 “..” 两个文件夹

我们实现的 `find <path> <name>` 的功能和 Linux 里的 `find <path> -name <name>` 一致

```c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"

void find(const char *path, const char *name);
const char *basename(const char *path);

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("usage: find <path> <filename>\n");
    }
    find(argv[1], argv[2]);
    exit(0);
}

/*
 * 在以 path 为根节点的文件树下搜索名为 name 的文件,
 * 如果找到则打印其路径.
 */
void find(const char *path, const char *name) {
    int fd;
    if ((fd = open(path, O_RDONLY)) < 0) {
        fprintf(2, "find: cannot open %s\n", path);
        return;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        fprintf(2, "find: cannot stat %s\n", path);
        close(fd);
        return;
    }

    if (strcmp(basename(path), name) == 0) {
        printf("%s\n", path);
    }

    // 只有文件夹有递归的必要
    if (st.type != T_DIR) {
        close(fd);
        return;
    }

    char buf[512], *p;
    struct dirent de;
    // 第一个 +1 对应 '/', 第二个 +1 对应结尾的 '\0'
    if (strlen(path) + 1 + DIRSIZ + 1 > sizeof(buf)) {
        printf("ls: path too long\n");
    }
    strcpy(buf, path);
    p = buf + strlen(buf);
    *p++ = '/';
    while (read(fd, &de, sizeof(de)) == sizeof(de)) {
        if (de.inum == 0 || strcmp(de.name, ".") == 0 ||
            strcmp(de.name, "..") == 0)
            continue;
        memmove(p, de.name, DIRSIZ);
        p[DIRSIZ] = 0;
        if (stat(buf, &st) < 0) {
            printf("ls: cannot stat %s\n", buf);
            continue;
        }
        find(buf, name);
    }
}

/*
 * 获取路径 path 的最后一部分
 */
const char *basename(const char *path) {
    const char *p;
    for (p = path + strlen(path); p >= path && *p != '/'; --p)
        ;
    ++p;
    return p;
}
```

# **xargs**

xargs 将标准输入（stdin）数据转换成命令行参数，一般和管道一起使用。在 linux 中，xargs 默认的命令是 echo.

上面的解释可能不太清晰，通过两个例子就能看出来它在做什么了：

第一个例子是直接使用 xargs

```bash
$ xargs -n 1 echo Im prefix
111
Im prefix 111
222
Im prefix 222
（按 ctrl+D 终止输入)
```

第二个例子是和管道联用

```bash
$ (echo 1 ; echo 2) | xargs -n 1 echo
1
2
$
```

我们实现的 `xargs <command>` 的功能和 Linux 里的 `xargs -n 1 <command>` 一致

```c
#include "kernel/types.h"
#include "kernel/param.h"
#include "user/user.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(2, "usage: xargs command\n");
        exit(1);
    }

    char *cmd = argv[1];
    char *cmdargs[MAXARG];
    int cmdargc = 0; // 在更改 cmdargc 前, 最好检查 cmdargc < MAXARG,
                     // 不过为了简化代码, 我们就不检查了

    // argv[0] 是 'xargs', argv[1] 是 command, 之后是参数
    // cmdargs 应当形如 [command, arg1, arg2, ..., addition_arg1, ...]
    cmdargc = argc - 1;
    for (int i = 0; i < cmdargc; i++) {
        cmdargs[i] = argv[i + 1];
    }

    char buf[512]; // 输入行
    char *p = buf; // 输入行的末尾
    while (read(0, p, 1) > 0) {
        if (p[0] == '\n') {
            p[0] = '\0';
            cmdargs[cmdargc] = buf;
            ++cmdargc;
            cmdargs[cmdargc] = 0;
            ++cmdargc;
            if (fork() == 0) {
                exec(cmd, cmdargs);
            }
            wait(0);
            // 重置
            p = buf;
            cmdargc = argc - 1;
        } else {
            ++p;
        }
    }
    exit(0);
}
```