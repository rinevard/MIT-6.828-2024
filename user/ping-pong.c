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
        // 父进程先写, 子进程先读
        while (read(p_parent_sender[0], buf, 2)) {
            write(1, "child read: ", 12);
            write(1, buf, 2);
            write(1, "\n", 1);
            sleep(1);
            write(p_child_sender[1], buf, 2);
        }
        close(p_parent_sender[0]);
        close(p_parent_sender[1]);
        close(p_child_sender[0]);
        close(p_child_sender[1]);
        exit(0);
    }
    // 父进程先写, 子进程先读
    buf[0] = 'b';
    buf[1] = '\0';
    write(p_parent_sender[1], buf, 2);
    while (read(p_child_sender[0], buf, 2)) {
        write(1, "paren read: ", 12);
        write(1, buf, 2);
        write(1, "\n", 1);
        write(p_parent_sender[1], buf, 2);
    }
    close(p_parent_sender[0]);
    close(p_parent_sender[1]);
    close(p_child_sender[0]);
    close(p_child_sender[1]);
    exit(0);
}