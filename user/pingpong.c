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