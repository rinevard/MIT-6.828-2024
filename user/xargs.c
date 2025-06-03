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
