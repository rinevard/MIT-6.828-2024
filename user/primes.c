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