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