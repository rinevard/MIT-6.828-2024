# Lab 8 File System

感觉 xv6 的文件系统还是挺有深度的。这个 lab 带我们简单复习了下 inode 如何指向 blocks，以及通过路径名查找文件的方法。

## Large files

这道题让我们增大 xv6 最大文件的大小。原本的 xv6 的每个 inode 支持 12 个普通块和一个间接块，那么原本的最大文件占据 12 + 256 = 268 个块。我们需要把 inode 修改为支持 11 个普通块、一个间接块、一个双重间接块，这样最大文件就能占据 11 + 256 + 256 \* 256 = 65803 个块。

首先我们要去 fs.h 里修改 dinode 和一些常量的定义，再去 file.h 里修改 inode 的定义，主要是修改 addrs：

```c
#define NDIRECT 11
#define NINDIRECT (BSIZE / sizeof(uint))
#define NDOUBLYINDIRECT (NINDIRECT * NINDIRECT)
#define MAXFILE (NDIRECT + NINDIRECT + NDOUBLYINDIRECT)

struct dinode {
    // 其他内容保持不变
    uint addrs[NDIRECT + 2];
};
```

```c
struct inode {
    // 其他内容保持不变
    uint addrs[NDIRECT + 2];
};
```

然后我们可以画一画 ip->addrs 的结构，如下图所示：

![](images/lab8-fs/doub_indirect.png)

对给定的 bn，如果它在 $[0,11)$ 之间，就查找普通块；如果在 $[11,11+256)$ 之间，就查找间接块；如果在 $[11+256,256+256\times256)$ 之间，就查找双重间接块。再参考 bmap 原有的代码就能完成修改了：

```c
static uint bmap(struct inode *ip, uint bn) {
    uint addr, *a;
    struct buf *bp;

    if (bn < NDIRECT) {
        if ((addr = ip->addrs[bn]) == 0) {
            addr = balloc(ip->dev);
            if (addr == 0)
                return 0;
            ip->addrs[bn] = addr;
        }
        return addr;
    }
    bn -= NDIRECT;

    if (bn < NINDIRECT) {
        // Load indirect block, allocating if necessary.
        if ((addr = ip->addrs[NDIRECT]) == 0) {
            addr = balloc(ip->dev);
            if (addr == 0)
                return 0;
            ip->addrs[NDIRECT] = addr;
        }
        bp = bread(ip->dev, addr);
        a = (uint *)bp->data;
        if ((addr = a[bn]) == 0) {
            addr = balloc(ip->dev);
            if (addr) {
                a[bn] = addr;
                log_write(bp);
            }
        }
        brelse(bp);
        return addr;
    }
    bn -= NINDIRECT;

    if (bn < NDOUBLYINDIRECT) {
        // Load doubly indirect block, allocating if necessary.
        if ((addr = ip->addrs[NDIRECT + 1]) == 0) {
            addr = balloc(ip->dev);
            if (addr == 0)
                return 0;
            ip->addrs[NDIRECT + 1] = addr;
        }

        // Load indirect block, allocating if necessary.
        bp = bread(ip->dev, addr);
        a = (uint *)bp->data;
        if ((addr = a[(bn / NINDIRECT)]) == 0) {
            addr = balloc(ip->dev);
            if (addr) {
                a[(bn / NINDIRECT)] = addr;
                log_write(bp);
            }
        }
        brelse(bp);
        if (addr == 0) {
            return 0;
        }

        // Find addr in block
        bp = bread(ip->dev, addr);
        a = (uint *)bp->data;
        if ((addr = a[bn % NINDIRECT]) == 0) {
            addr = balloc(ip->dev);
            if (addr) {
                a[bn % NINDIRECT] = addr;
                log_write(bp);
            }
        }
        brelse(bp);
        return addr;
    }

    panic("bmap: out of range");
}
```

我们还需要修改 itrunc，它释放传入的 inode 占用的所有数据块。仿照 itrunc 原有的代码加入释放双重间接块的代码就行：

```c
void itrunc(struct inode *ip) {
    int i, j, k;
    struct buf *bp, *bbp;
    uint *a, *aa;

    for (i = 0; i < NDIRECT; i++) {
        if (ip->addrs[i]) {
            bfree(ip->dev, ip->addrs[i]);
            ip->addrs[i] = 0;
        }
    }

    if (ip->addrs[NDIRECT]) {
        bp = bread(ip->dev, ip->addrs[NDIRECT]);
        a = (uint *)bp->data;
        for (j = 0; j < NINDIRECT; j++) {
            if (a[j])
                bfree(ip->dev, a[j]);
        }
        brelse(bp);
        bfree(ip->dev, ip->addrs[NDIRECT]);
        ip->addrs[NDIRECT] = 0;
    }

    if (ip->addrs[NDIRECT + 1]) {
        // Free doubly indirect block
        bp = bread(ip->dev, ip->addrs[NDIRECT + 1]);
        a = (uint *)bp->data;
        for (j = 0; j < NINDIRECT; j++) {
            if (a[j]) {
                // Free indirect block j
                bbp = bread(ip->dev, a[j]);
                aa = (uint *)bbp->data;
                for (k = 0; k < NINDIRECT; k++) {
                    if (aa[k])
                        bfree(ip->dev, aa[k]);
                }
                brelse(bbp);
                bfree(ip->dev, a[j]);
            }
        }
        brelse(bp);
        bfree(ip->dev, ip->addrs[NDIRECT + 1]);
        ip->addrs[NDIRECT + 1] = 0;
    }

    ip->size = 0;
    iupdate(ip);
}
```

## Symbolic links

这道题让我们给 xv6 加入符号链接，不过这里的符号链接是青春版。符号链接正统版应该能指向目录，我们的只能指向文件。

这里就不放配置系统调用的代码了（大家都已经品鉴过无数次了），我们先看看 man 手册是怎么介绍 symlink 的。这里我们主要看 man 2 symlink 和 man 7 symlink，前者是给使用者看的函数说明，后者是函数原理。

[symlink(2): make new name for file - Linux man page](https://linux.die.net/man/2/symlink)

[symlink(7): symbolic link handling - Linux man page](https://linux.die.net/man/7/symlink)

symlink 的签名是 `int symlink(const char *oldpath, const char *newpath);`，它创建一个名为 newpath 的指向 oldpath 的符号链接文件，这个文件里存储着 oldpath 这个字符串。

对照 hint，我们知道要去 stat.h 里新增一种叫作 T_SYMLINK 的文件类型，这就是符号链接类型。至此我们就能新增 sys_symlink 函数了。

```c
uint64 sys_symlink(void) {
    char old[MAXPATH], new[MAXPATH];
    struct inode *ip;
    int len;

    begin_op();
    if (argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0 ||
        (ip = create(new, T_SYMLINK, 0, 0)) == 0) {
        end_op();
        return -1;
    }
    len = strlen(old);
    writei(ip, 0, (uint64)&len, 0, sizeof(len));
    writei(ip, 0, (uint64)old, sizeof(len), len + 1);
    iunlockput(ip);
    end_op();

    return 0;
}
```

我一开始对 iunlock、iunlockput 很疑惑，主要是不知道 iput 有什么用。其实 iput 和 inode 的工作方式密切相关，我们接下来简要分析一下。这里涉及到的文件是 fs.c。

考虑到我们只能直接和内存交互，而磁盘上又有很多 inode，我们会把有人在用的的 inode 保存在 itable.inode 中，并用 inode.ref 来记录有多少人在用。用完的人要调用 iput 说自己用完了，这样 ref 就会减少。未来我们在把磁盘 inode 放到内存中时会找到 ref == 0 的东西来替换。

举个例子，dirlookup 找路径对应的 inode，这种“找”最终总会调用 iget，iget 会检查想找的 inode 在不在内存中的 itable.inode 中，如果不在就在 itable.inode 里找个 ref == 0 的元素，把它替换成想找的 inode 并设置其 ref。相应地，调用者在用完 inode 后要负责调用 iput 说自己用完了，不然 itable.inode 的空间被占满后就不能加载新的 inode 了。

所以在 sys_symlink 的末尾我们要调用 iunlockput 而不是只调用 iunlock，毕竟我们在 sys_symlink 里不再使用这个 inode 了。

接下来我们看看 open 函数。首先照着 hint 在 fcntl.h 里加入 O_NOFOLLOW，然后对符号链接类型的文件，读取其 path 然后顺藤摸瓜就行：

```c
uint64 sys_open(void) {
    char path[MAXPATH];
    int fd, omode;
    struct file *f;
    struct inode *ip;
    int n, symcnt, len;

    argint(1, &omode);
    if ((n = argstr(0, path, MAXPATH)) < 0)
        return -1;

    begin_op();

    if (omode & O_CREATE) {
        ip = create(path, T_FILE, 0, 0);
        if (ip == 0) {
            end_op();
            return -1;
        }
    } else if (omode & O_NOFOLLOW) {
        if ((ip = namei(path)) == 0) {
            end_op();
            return -1;
        }
        ilock(ip);
        if (ip->type == T_DIR && omode != O_RDONLY) {
            iunlockput(ip);
            end_op();
            return -1;
        }
    } else {
        symcnt = 0;
        if ((ip = namei(path)) == 0) {
            end_op();
            return -1;
        }
        ilock(ip);

        while (ip->type == T_SYMLINK) {
            symcnt++;
            if (symcnt >= 10) {
                iunlockput(ip);
                end_op();
                return -1;
            }
            readi(ip, 0, (uint64)&len, 0, sizeof(len));
            readi(ip, 0, (uint64)path, sizeof(len), len + 1);
            iunlockput(ip);
            if ((ip = namei(path)) == 0) {
                end_op();
                return -1;
            }
            ilock(ip);
        }
        if (ip->type == T_DIR && omode != O_RDONLY) {
            iunlockput(ip);
            end_op();
            return -1;
        }
    }

    if (ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)) {
        iunlockput(ip);
        end_op();
        return -1;
    }

    if ((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0) {
        if (f)
            fileclose(f);
        iunlockput(ip);
        end_op();
        return -1;
    }

    if (ip->type == T_DEVICE) {
        f->type = FD_DEVICE;
        f->major = ip->major;
    } else {
        f->type = FD_INODE;
        f->off = 0;
    }
    f->ip = ip;
    f->readable = !(omode & O_WRONLY);
    f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

    if ((omode & O_TRUNC) && ip->type == T_FILE) {
        itrunc(ip);
    }

    iunlock(ip);
    end_op();

    return fd;
}
```

顺提一句，make grade 显示 Timeout FAIL 可能是因为后台的进程太多了导致 CPU 没有全力以赴。尽量把别的进程（比如浏览器）关掉只保留一个在跑 make grade 的终端。