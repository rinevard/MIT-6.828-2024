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