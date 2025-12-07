# Lab 6 Networking

这个 lab 分两个部分，第一部分是补全 E1000 网卡驱动的数据包收发相关代码，第二部分是添加代码完成 UDP 包的接收。在开始之前，我们应该先去把讲网络的 lecture 看掉，因为里面涉及了很多相关内容。你可能会疑惑为什么课程把 lab 安排在 lec 前面，我在本文的文末吐槽里写了可能的原因。

让我们进入正题。

## E1000 网卡驱动

我们在编写代码时要回答两个问题——我们如何借助网卡接收数据，又如何发送数据？我们对照文档第三章来分析一下，然后在分析的结尾分别给出 e1000_recv 和 e1000_transmit 的代码：

1. 我们如何借助网卡接收数据？
    
    我们在初始化里设置了 RDTR，这让网卡在收到包时发出中断，这一部分和文档的 3.2.8 Receive Interrupts 有关：
    
    ```c
    // ask e1000 for receive interrupts.
    regs[E1000_RDTR] = 0; // interrupt after every received packet (no timer)
    ```
    
    我们还在初始化里设置了 RDBAL，这让网卡在收到包时把数据放到 rx_ring 中：
    
    ```c
    // [E1000 14.4] Receive initialization
    memset(rx_ring, 0, sizeof(rx_ring));
    for (i = 0; i < RX_RING_SIZE; i++) {
        rx_bufs[i] = kalloc();
        if (!rx_bufs[i])
            panic("e1000");
        rx_ring[i].addr = (uint64)rx_bufs[i];
    }
    regs[E1000_RDBAL] = (uint64)rx_ring;
    ```
    
    具体来说，它会把包放到 rx_ring[RDH] 里，我们则用 RDT 来读取。
    
    下图是 rx_ring 的示意图，硬件可向 [HEAD, TAIL) 中写入内容， TAIL 处留空以区分空环和满环。
    
    ![](images/lab6-net/rxring.png)
    
    网卡传来的数据格式在 e1000_dev.h 里定义，可参考文档 3.2.3 Receive Descriptor Format：
    
    ```c
    struct rx_desc {
        uint64 addr;   /* Address of the descriptor's data buffer */
        uint16 length; /* Length of data DMAed into data buffer */
        uint16 csum;   /* Packet checksum */
        uint8 status;  /* Descriptor status */
        uint8 errors;  /* Descriptor Errors */
        uint16 special;
    };
    ```
    
    网卡发出中断后，内核会调用 e1000_intr，e1000_intr 又会进一步调用 e1000_recv. 
    
    对照 rx_ring 的示意图，我们就知道 e1000_recv 应该去 rx_ring[RDT + 1] 处读取数据，直到读到无效数据为止。参考文档的 3.2.3.1 Receive Descriptor Status Field，我们可以根据 rx_desc 的 status 的 DD 位得知数据是否有效。
    
    总之代码如下所示：
    
    ```c
    static void e1000_recv(void) {
        //
        // Your code here.
        //
        // Check for packets that have arrived from the e1000
        // Create and deliver a buf for each packet (using net_rx()).
        //
        acquire(&e1000_lock);
        uint32 idx = (regs[E1000_RDT] + 1) % RX_RING_SIZE;
        while (rx_ring[idx].status & E1000_RXD_STAT_DD) {
    
            char *recv_buf = rx_bufs[idx];
            int buflen = rx_ring[idx].length;
    
            rx_bufs[idx] = kalloc();
            if (!rx_bufs[idx]) {
                panic("e1000");
            }
            rx_ring[idx].addr = (uint64)rx_bufs[idx];
            rx_ring[idx].status = 0;
            idx = (idx + 1) % RX_RING_SIZE;
    
            // net_rx could call e1000_transmit, so release lock before calling it
            release(&e1000_lock);
            net_rx(recv_buf, buflen);
            acquire(&e1000_lock);
        }
        // set RDT to the last processed index
        regs[E1000_RDT] = (idx + RX_RING_SIZE - 1) % RX_RING_SIZE;
        release(&e1000_lock);
    }
    ```
    
    这里的 `regs[E1000_RDT] = (idx + RX_RING_SIZE - 1) % RX_RING_SIZE` 可能比较 tricky，总之就是 RDT 应该指在最后一个被取出的描述符处以实现留空。
    
2. 我们如何借助网卡发送数据？
    
    如文档 3.4.3 Transmit Interrupts 所写，我们也可以设置网卡在传输完成时发生中断，不过在 xv6 里我们没有这么设置，所以发送成功不会中断。
    
    与读取数据要把 rx_ring 的位置告诉网卡类似，我们要把 tx_ring 的位置在初始化里告诉网卡：
    
    ```c
    // [E1000 14.5] Transmit initialization
    memset(tx_ring, 0, sizeof(tx_ring));
    for (i = 0; i < TX_RING_SIZE; i++) {
        tx_ring[i].status = E1000_TXD_STAT_DD;
        tx_bufs[i] = 0;
    }
    regs[E1000_TDBAL] = (uint64)tx_ring;
    ```
    
    我们会把数据放到 tx_ring[TDT] 里，网卡会自动走动 TDH 直到追上 TDT.
    
    下图是 rx_ring 的示意图，TAIL 指向硬件能够处理的最后一个描述符之后的位置，这也是软件写入第一个新描述符的位置。与 rx_ring 不同，tx_ring 无留空。
    
    ![](images/lab6-net/txring.png)
    
    传给网卡的数据结构也在 e1000_dev.h 里定义，可参考文档 3.3.2 Transmit Descriptors：
    
    ```c
    struct tx_desc {
        uint64 addr;
        uint16 length;
        uint8 cso;
        uint8 cmd;
        uint8 status;
        uint8 css;
        uint16 special;
    };
    ```
    
    对照 tx_ring 的示意图，我们要把数据存在 tx_ring[TDT] 里。要注意如果 TDT 指向的内容已经被网卡成功发送了（可以通过 status 的 DD 位来判断），我们要释放 TDT 指向的内存以避免内存泄漏；另外我们还要更新 cmd 位，具体可以参考 3.3.3.1 Transmit Descriptor Command Field Format. 总之下面是代码：
    
    ```c
    int e1000_transmit(char *buf, int len) {
        //
        // Your code here.
        //
        // buf contains an ethernet frame; program it into
        // the TX descriptor ring so that the e1000 sends it. Stash
        // a pointer so that it can be freed after send completes.
        //
        acquire(&e1000_lock);
        uint32 idx = regs[E1000_TDT];
        if (!(tx_ring[idx].status & E1000_TXD_STAT_DD)) {
            release(&e1000_lock);
            return -1;
        }
    
        // free old buf that has been sent away
        if (tx_ring[idx].addr) {
            kfree(tx_bufs[idx]);
        }
        tx_bufs[idx] = buf;
        tx_ring[idx].addr = (uint64)tx_bufs[idx];
        tx_ring[idx].length = len;
        tx_ring[idx].cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS;
        tx_ring[idx].status = 0;
        regs[E1000_TDT] = (idx + 1) % TX_RING_SIZE;
        release(&e1000_lock);
    
        return 0;
    }
    ```
    

## UDP 包的接收

我们先来梳理一下整个收信流程是怎样的：外部会发 packet 给网卡，网卡在收到后会引发中断，内核发现中断由网卡引发就会调用 `e1000_intr`，之后 packet 会被发给 `net_rx` 来让内核做进一步处理。如果 `net_rx` 发现这是一个 IP packet 就会进一步调用 `ip_rx`，然后就到我们的工作了。

我们只需要处理 ETH-IP-UDP 嵌套的 packet，让我们回顾一下它的结构：

![](images/lab6-net/packet.png)

具体的 packet header 的定义在 net.h 里。

总之我们要实现 recv、bind、ip_rx 这三个函数。应用会调用 bind 来监听一个端口，当端口被监听时我们需要保存到达这个端口的数据包，所以要在 `net.h` 中定义如下的数据结构：

```c
#define MAX_LISTEN_PORTS 128
#define PACKET_RING_SIZE 16 // for any given port, no more than 16 packets should be saved
struct listener {
    int used;
    int port;
    char *packet_ring[PACKET_RING_SIZE];
    int head; // used by writer
    int tail; // used by reader
};
```

这里的 used 字段可能有点奇怪，我来解释一下：我们希望每当一个应用调用 bind，我们就新增一个 listener，但由于 C 语言实现动态数组很麻烦，所以我们选择创建静态大小的 listeners 数组。那为什么不用链表呢？因为我们新增内存的手段只有 kalloc，而一整个页对一个 listener 来说太大了。

struct listener 里的 used 字段就是为了判断 listeners 数组里的某个 listener 是否已经被使用。

我们要在 net.c 里初始化 listeners 数组：

```c
static struct listener listeners[MAX_LISTEN_PORTS];

void netinit(void) {
    initlock(&netlock, "netlock");
    for (int i = 0; i < MAX_LISTEN_PORTS; i++) {
        listeners[i].used = 0;
        for (int j = 0; j < PACKET_RING_SIZE; j++) {
            listeners[i].packet_ring[j] = 0;
        }
    }
}
```

接下来我们分析一下每个函数要做什么，再给出代码：

1. recv(int dport, int \*src, short \*sport, char \*buf, int maxlen)
    
    接收到达 dport 端口的 UDP packet. 如果到达了多个，取出第一个 packet；如果没有东西到达，等待直到有东西到达。
    
    本函数把取出的 packet 里 32 位的源 IP 地址复制到 \*src，UDP 源端口号复制到 \*sport，payload 的至多 maxlen 个字节复制到 buf. 成功则返回复制的 payload 字节数，失败返回 -1.
    
    注意网络字节序和系统字节序不同，要用 `ntohs` 和 `ntohl` 来逆转字节序。
    
    另外，这里的各个指针是用户空间的虚拟地址，我们要用 copyout 把 packet 里的内容（它们在内核空间）复制到用户空间。
    
    ```c
    uint64 sys_recv(void) {
        //
        // Your code here.
        //
        struct proc *p;
        int dport;
        uint64 src;
        uint64 sport;
        uint64 buf;
        int maxlen;
        struct listener *listener = 0;
    
        p = myproc();
        argint(0, &dport);
        argaddr(1, &src);
        argaddr(2, &sport);
        argaddr(3, &buf);
        argint(4, &maxlen);
    
        // find listener related to dport
        acquire(&netlock);
        for (int i = 0; i < MAX_LISTEN_PORTS; i++) {
            listener = &listeners[i];
            if (listener->used && listener->port == dport) {
                break;
            }
            listener = 0;
        }
        if (!listener) {
            goto bad;
        }
    
        // return the earliest waiting packet or wait until a packet arrives
        while (1) {
            if (listener->packet_ring[listener->tail]) {
                // extract a packet from the ring
                char *packet = listener->packet_ring[listener->tail];
                listener->packet_ring[listener->tail] = 0;
                listener->tail = (listener->tail + 1) % PACKET_RING_SIZE;
    
                // extract data from the packet
                struct ip *ip_packet = (struct ip *)(packet + sizeof(struct eth));
                struct udp *udp_packet =
                    (struct udp *)(packet + sizeof(struct eth) + sizeof(struct ip));
                void *payload = ((char *)udp_packet) + sizeof(struct udp);
    
                // re-arrange the bytes
                uint32 src_ip = ntohl(ip_packet->ip_src);
                uint16 src_port = ntohs(udp_packet->sport);
                if (copyout(p->pagetable, src, (char *)&src_ip, sizeof(src_ip)) <
                    0) {
                    kfree(packet);
                    goto bad;
                }
                if (copyout(p->pagetable, sport, (char *)&src_port,
                            sizeof(src_port)) < 0) {
                    kfree(packet);
                    goto bad;
                }
    
                // copy at most maxlen bytes of the payload and free the packet
                uint16 buflen = ntohs(udp_packet->ulen) - sizeof(struct udp);
                uint64 cplen = (buflen > maxlen) ? maxlen : buflen;
                if (copyout(p->pagetable, buf, payload, cplen) < 0) {
                    kfree(packet);
                    goto bad;
                }
                kfree(packet);
    
                release(&netlock);
                return cplen;
            } else {
                // wait until a packet arrives
                sleep(listener->packet_ring, &netlock);
            }
        }
        goto bad;
    
    bad:
        release(&netlock);
        return -1;
    }
    ```
    
2. bind(int port)
    
    进程在调用 recv 前应当先调用 bind 来监听特定端口，bind 被调用时应该要初始化 port 对应的待处理区。
    
    ```c
    uint64 sys_bind(void) {
        //
        // Your code here.
        //
    
        int port;
    
        argint(0, &port);
    
        acquire(&netlock);
    
        // return error if repeated binding
        for (int i = 0; i < MAX_LISTEN_PORTS; i++) {
            if (listeners[i].used && listeners[i].port == port) {
                goto bad;
            }
        }
    
        for (int i = 0; i < MAX_LISTEN_PORTS; i++) {
            if (!listeners[i].used) {
                // use the first unused listener for port
                listeners[i].used = 1;
                listeners[i].port = port;
                listeners[i].head = 0;
                listeners[i].tail = 0;
                release(&netlock);
                return 0;
            }
        }
        goto bad;
    
    bad:
        release(&netlock);
        return -1;
    }
    ```
    
3. ip_rx(char \*buf, int len)
    
    e1000_recv 会把 packet 转发给 net_rx，net_rx 在发现收到的 packet 是 IP packet 时会把 packet 转发给 ip_rx.
    
    我们需要检查 destination port 是否在正在被监听，如果未监听或待处理区已满就丢弃包，否则存到待处理区。
    
    ```c
    void ip_rx(char *buf, int len) {
        // don't delete this printf; make grade depends on it.
        static int seen_ip = 0;
        if (seen_ip == 0)
            printf("ip_rx: received an IP packet\n");
        seen_ip = 1;
    
        //
        // Your code here.
        //
        struct udp *udp_packet;
        struct listener *listener = 0;
    
        udp_packet = (struct udp *)(buf + sizeof(struct eth) + sizeof(struct ip));
    
        acquire(&netlock);
        for (int i = 0; i < MAX_LISTEN_PORTS; i++) {
            listener = &listeners[i];
            // try best to save packet in listener related to dport
            if (listener->used && listener->port == ntohs(udp_packet->dport)) {
                int head = listener->head;
    
                // if ring is full, drop the incoming packet
                if (listener->packet_ring[head] != 0) {
                    kfree(buf);
                    release(&netlock);
                    return;
                }
    
                // save packet in listener
                listener->packet_ring[head] = buf;
                listener->head = (head + 1) % PACKET_RING_SIZE;
    
                wakeup(listener->packet_ring);
                release(&netlock);
                return;
            }
        }
        kfree(buf);
        release(&netlock);
        return;
    }
    
    ```
    

然后我们就下班了！

## 杂七杂八小知识

### 所有权转移

看一眼 e1000_recv 的代码，我们会发现它调用了 kalloc，却没有在内部显式释放新建的内存，这是内存泄漏吗？

```c
static void e1000_recv(void) {
    //
    // Your code here.
    //
    // Check for packets that have arrived from the e1000
    // Create and deliver a buf for each packet (using net_rx()).
    //
    acquire(&e1000_lock);
    uint32 idx = (regs[E1000_RDT] + 1) % RX_RING_SIZE;
    while (rx_ring[idx].status & E1000_RXD_STAT_DD) {

        char *recv_buf = rx_bufs[idx]; // 在这里取出
        int buflen = rx_ring[idx].length;

        rx_bufs[idx] = kalloc(); // 在这里分配新内存
        if (!rx_bufs[idx]) {
            panic("e1000");
        }
        rx_ring[idx].addr = (uint64)rx_bufs[idx];
        rx_ring[idx].status = 0;
        idx = (idx + 1) % RX_RING_SIZE;

        // net_rx could call e1000_transmit, so release lock before calling it
        release(&e1000_lock);
        net_rx(recv_buf, buflen); // 在这里转移所有权
        acquire(&e1000_lock);
    }
    // set RDT to the last processed index
    regs[E1000_RDT] = (idx + RX_RING_SIZE - 1) % RX_RING_SIZE;
    release(&e1000_lock);
}
```

这并不是内存泄漏，而是一种所有权转移。对每个新建的 `rx_bufs[idx] = kalloc()`，我们都会在未来通过 `recv_buf = rx_bufs[idx]` 把它取出，并把其所有权通过 `net_rx(recv_buf, buflen)` 转移给 `net_rx`. 也就是说，`net_rx` 负责释放传入的 buf.

而 `net_rx` 也可能把 buf 的所有权转移给 `app_rx` 或 `ip_rx`.

```c
void net_rx(char *buf, int len) {
    struct eth *eth = (struct eth *)buf;

    if (len >= sizeof(struct eth) + sizeof(struct arp) &&
        ntohs(eth->type) == ETHTYPE_ARP) {
        arp_rx(buf);
    } else if (len >= sizeof(struct eth) + sizeof(struct ip) &&
               ntohs(eth->type) == ETHTYPE_IP) {
        ip_rx(buf, len);
    } else {
        kfree(buf);
    }
}
```

这和 C++ 里的 move 语义类似，都是“转移所有权”。

### 什么是 __attribute((packed))__

net.h 里有 packet header 的定义，这里 struct eth 里的 `__attribute((packed))__` 是在告知编译器取消内存对齐优化，让结构体成员紧凑排列。

```c
struct eth {
    uint8 dhost[ETHADDR_LEN];
    uint8 shost[ETHADDR_LEN];
    uint16 type;
} __attribute__((packed));
```

还记得对齐优化是什么吗？为了提高 CPU 访问内存的效率，结构体中的每个成员的起始地址相对于结构体起始地址的偏移量需要是该成员自身大小的整数倍。如果不是，编译器会在前一个成员后面填充一些空白字节。

比如对下面的结构体，sizeof(struct Test) 是 12 字节，而不是成员大小之和的 1 + 4 + 1 = 6 字节。

```c
struct Test {
    char c1;
    int i;
    char c2;
};
```

## 文末吐槽

Lab Net 经历了不少变迁，在 2019 和 2020 年它还是课程的最后一个 lab，而从 2021 年开始它就被移到了课程中期，而网络 lec 则一直在课程后期，所以就造成了 lab 和 lec 的错位。

[2023 年版本的 Net Lab](https://pdos.csail.mit.edu/6.828/2023/labs/net.html) 只要求实现 Part One 而且标注的难度还是 hard，2024 年就变成了 moderate 还新加了一个 Part Two，果然课程难度也是会通货膨胀的🫠🫠