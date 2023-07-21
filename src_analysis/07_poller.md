#! https://zhuanlan.zhihu.com/p/415072416
# workflow 源码解析 02 : epoll 2

项目源码 : https://github.com/sogou/workflow

更加详细的源码注释可看 : https://github.com/chanchann/workflow_annotation

## 结构体
下面是你给出的C代码的注释：

```c
/* 定义轮询器节点结构 */
struct __poller_node
{
    int state;         /* 节点状态 */
    int error;         /* 错误码 */
    struct poller_data data;  /* 轮询器数据 */
#pragma pack(1)       /* 设置数据对齐为1字节，避免内存浪费 */
    union               /* 联合体，同一时间只能使用一个成员 */
    {
        struct list_head list; /* 双向链表头，用于链表操作 */
        struct rb_node rb;     /* 红黑树节点，用于红黑树操作 */
    };
#pragma pack()       /* 取消指定的数据对齐，恢复编译器默认对齐 */
    char in_rbtree;   /* 标记此节点是否在红黑树中 */
    char removed;     /* 标记此节点是否已被移除 */
    int event;        /* 事件类型 */
    struct timespec timeout; /* 节点超时时间 */
    struct __poller_node *res; /* 用于存储结果的节点指针 */
};

/* 定义轮询器结构 */
struct __poller
{
    size_t max_open_files;  /* 最大可打开文件数量 */
    void (*cb)(struct poller_result *, void *);  /* 回调函数 */
    void *ctx;   /* 用户上下文 */

    pthread_t tid;  /* 线程ID */
    int pfd;  /* 轮询文件描述符 */
    int timerfd;  /* 定时器文件描述符 */
    int pipe_rd;  /* 管道读文件描述符 */
    int pipe_wr;  /* 管道写文件描述符 */
    int stopped;  /* 标记轮询器是否已停止 */
    struct rb_root timeo_tree;  /* 超时时间红黑树的根节点 */
    struct rb_node *tree_first;  /* 红黑树中的第一个节点 */
    struct rb_node *tree_last;  /* 红黑树中的最后一个节点 */
    struct list_head timeo_list;  /* 超时时间链表头 */
    struct list_head no_timeo_list;  /* 无超时时间链表头 */
    struct __poller_node **nodes;  /* 轮询器节点指针数组 */
    pthread_mutex_t mutex;  /* 用于同步的互斥量 */
    char buf[POLLER_BUFSIZE];  /* 缓冲区 */
};
typedef struct __poller poller_t; /* 定义轮询器类型 */
```

这个轮询器使用了红黑树和链表来存储和管理数据。红黑树和链表各有其优点，红黑树在搜索操作中效率更高，链表在插入和删除操作中效率更高。轮询器可能根据具体的需求来选择使用哪种数据结构。这两种数据结构在__poller_node中通过联合体实现，这样可以节省内存，因为同一时间只会用到其中一个

让我们假设我们有两个线程，线程A和线程B。线程A负责处理一些任务并生成结果，线程B负责处理线程A生成的结果。

首先，我们需要创建一个管道并将其读端添加到epoll的监听列表中：

```cpp
poller_t *poller = ...;  // epoll实例
int pipefd[2];
pipe(pipefd);
__poller_add_fd(pipefd[0], EPOLLIN, (void *)1, poller);
```

在上述代码中，我们创建了一个管道并将其读端`pipefd[0]`添加到了epoll的监听列表中。

然后，在线程A中，我们可以向管道的写端写入数据：

```cpp
int result = do_some_work();  // 假设do_some_work函数执行一些任务并生成结果
write(pipefd[1], &result, sizeof(result));
```

在上述代码中，我们执行了一些任务并将结果写入到了管道的写端。

在线程B中，我们可以使用epoll_wait来监听epoll事件：

```cpp
struct epoll_event events[MAX_EVENTS];
int nfds = epoll_wait(poller->pfd, events, MAX_EVENTS, -1);
for (int i = 0; i < nfds; i++) {
    if (events[i].data.fd == pipefd[0]) {  // 检查是否是管道读端的事件
        int result;
        read(pipefd[0], &result, sizeof(result));  // 从管道的读端读取结果
        handle_result(result);  // 处理结果
    }
}
```

在上述代码中，我们使用epoll_wait来监听epoll事件。当管道的读端可以读取数据时，epoll_wait将返回，并且我们可以从管道的读端读取线程A发送过来的结果并处理这个结果。

总的来说，通过将管道的读端添加到epoll事件监听列表中，我们可以实现线程间的实时通信，即线程B可以实时地获取并处理线程A生成的结果。

### __poller_node
`__poller_node`结构体在这个轮询器(poller)框架中起到关键的作用。该结构体实例被称为轮询器节点(poller node)，它代表一个单独的被轮询的对象，可以是一个套接字、一个定时器或者其他类型的文件描述符。

每个`__poller_node`节点都包含一些关键的信息：

1. `state`：表明当前轮询器节点的状态，这可能涉及节点是否有效，是否正在处理中，是否已经完成处理等。

2. `error`：如果在处理该节点过程中发生错误，这个字段将存储相关的错误代码。

3. `data`：这是一个`poller_data`结构体实例，它存储了与当前节点相关的数据，例如要执行的操作（读、写、监听等），以及关于如何处理这些操作的函数指针。

4. `list`和`rb`：这是一个联合体，它存储了当前节点在链接列表或者红黑树中的位置信息。在不同的情况下，轮询器可能会选择使用链表或者红黑树来存储和管理节点，取决于具体情况的需求。

5. `in_rbtree`和`removed`：这些标志位表明节点是否在红黑树中，以及节点是否已经被移除。

6. `event`：代表了当前节点要监听的事件类型。

7. `timeout`：表示了当前节点的超时时间。

8. `res`：这个字段可能用来存储处理结果。

在轮询器框架中，每一个被监控的对象（例如一个套接字或者一个定时器）都会被封装为一个`__poller_node`节点。轮询器会周期性地检查这些节点，当节点对应的事件（例如可读、可写、超时等）发生时，轮询器会调用预设的处理函数来处理这个事件。

### pragma pack(1)
`#pragma pack(1)`是一种特殊的预处理器命令，用于改变编译器对数据成员对齐方式的默认处理。其中的数字1代表对齐的字节数。这条命令告诉编译器，接下来的数据成员应按照1字节对齐方式进行对齐，这可以使得数据紧密地排列，没有额外的空间填充。

默认情况下，C和C++编译器会根据目标平台和性能优化的需要，为结构体和联合体的数据成员添加一些填充字节，使得数据成员的起始地址符合其类型的特定对齐需求。这种对齐可以使得CPU更高效地访问内存，但是也可能导致数据占用更多的内存空间。

比如，如果你有一个包含char和int的结构体，由于int类型通常需要以4字节为单位对齐，所以编译器可能会在char后面插入几个填充字节。使用`#pragma pack(1)`可以避免这种填充，使得结构体的大小最小化，但是可能会牺牲一些访问性能。

需要注意的是，`#pragma pack()`是编译器特有的指令，不同的编译器可能会有不同的行为，或者可能完全不支持这个指令。另外，这个指令对齐的改变只影响其后的数据定义，如果你想恢复到默认的对齐方式，你可以使用没有参数的`#pragma pack()`。

这个指令常常在处理硬件接口、网络协议等需要精确控制数据布局的场合中使用。

### 结构体__poller_node中的res指针
结构体 `__poller_node` 中的 `res` 指针看起来被设计为一个辅助指针，用于临时存储一些额外的信息。这是一种在 C 语言中常见的设计模式，利用了 C 语言指针和类型系统的灵活性。

在你给出的代码中，`res` 有两个主要的用途。首先，`res` 被用来保存一个新创建的 `__poller_node` 结构体的地址，这个新创建的结构体用于存储一些临时信息，比如尝试将数据追加到节点的消息时产生的结果。其次，`res` 被转化为 `struct poller_result` 指针，并被传递给 `poller->cb` 回调函数。这样做的原因是 `poller->cb` 回调函数需要一个 `struct poller_result` 类型的参数，而 `res` 中存储的就是这个参数的数据。

这种设计可以有效地避免了不必要的数据复制和内存分配。通过 `res` 指针，可以直接将数据传递给 `poller->cb` 函数，而不需要创建一个新的 `struct poller_result` 结构体。这对于性能敏感的程序（如网络服务或高性能计算应用）来说是非常重要的。

### struct __poller中的 void (*cb)(struct poller_result *, void *)
在 `struct __poller` 中，`void (*cb)(struct poller_result *, void *)` 是一个函数指针。这个函数指针指向的函数接受两个参数：一个是 `struct poller_result *` 类型，另一个是 `void *` 类型。

在程序设计中，使用函数指针的常见原因之一是为了实现回调函数（callback function）。回调函数是一种在某些事件或条件发生时由程序主动调用的函数，例如完成某项任务、处理某种错误等。

在你提供的代码中，`cb` 可以看作是一个回调函数，用于处理由 `poller` 管理的各种事件（比如网络I/O事件或定时器事件）的结果。具体的处理方式由该函数的实现决定，可以在程序运行时动态地指定。例如，当某个网络操作完成或出现错误时，`poller` 可能会调用 `cb` 函数来处理这个结果，传递给它一个表示操作结果的 `struct poller_result *` 参数和一个可能包含额外上下文信息的 `void *` 参数。

## int poller_start(poller_t *poller)
将管道的读端添加到epoll事件监听列表中的主要原因是：管道可以用于线程间或进程间的通信，通过监听管道的读端事件，可以实时地获取其他线程或进程通过管道发送过来的数据。

具体来说，线程A可以通过向管道的写端写入数据，这些数据会被发送到管道的读端。当数据到达管道的读端时，如果管道的读端被添加到了epoll的监听列表中，epoll就会收到一个事件，表示管道的读端可以读取数据了。然后，线程B（也就是调用epoll_wait的线程）就可以从管道的读端读取线程A发送过来的数据。

总的来说，通过将管道的读端添加到epoll事件监听列表中，可以实现对管道读端数据的实时监听，从而实现线程间或进程间的实时通信。

## 二级指针： nodes = (struct __poller_node **)calloc(n, sizeof(void *))
让我们详细解释一下这段代码：`nodes = (struct __poller_node **)calloc(n, sizeof(void *));`

这段代码的主要目标是创建一个指向指针的数组，每个指针都可以指向一个 `struct __poller_node` 结构体实例。这种结构通常被称为指针数组。因此，该数组本身不存储 `struct __poller_node` 结构体的实例，而是存储指向这些实例的指针。

让我们逐步了解这段代码：

1. `calloc(n, sizeof(void *))` - 这个函数调用申请了 `n` 个元素的内存空间，每个元素的大小都是 `sizeof(void *)`（一个指针的大小）。也就是说，它创建了一个能够容纳 `n` 个指针的数组。

2. `(struct __poller_node **)` - 这是一个强制类型转换，它告诉编译器，我们将把 `calloc` 返回的原始指针（类型为 `void *`）转换为 `struct __poller_node **` 类型。这意味着我们将这块内存视为一个数组，该数组的每个元素都是一个指向 `struct __poller_node` 的指针。

3. `nodes =` - 这将 `calloc` 返回的地址（现在已转换为 `struct __poller_node **` 类型）赋给 `nodes`。此时，`nodes` 指向了一个可容纳 `n` 个指向 `struct __poller_node` 的指针的数组。

在后续的代码中，你可能会看到像 `nodes[i] = somePointerToPollerNode;` 这样的代码，它将一个 `struct __poller_node *` 类型的值（一个指向 `struct __poller_node` 的指针）赋给数组 `nodes` 的第 `i` 个元素。这样，我们就可以使用动态数组来管理一组 `struct __poller_node` 结构体实例，而不需要预先知道具体的数量。

## epoll_create
`epoll_create()`函数是Linux系统中用于创建epoll对象的函数。epoll是Linux系统中的一个事件驱动型I/O接口，它可以高效地处理大量并发的文件描述符（如套接字socket）。

`epoll_create()`函数的参数是一个整数，它的原始设计意图是用来指定内核为这个epoll实例预留多大的空间（即可以监视的最大文件描述符数目）。然而，实际上在Linux 2.6.8之后的内核版本中，这个参数不再起作用，内核会根据需要动态地分配空间。不论你传入什么数值，行为都是一样的。这就是为什么我之前说“参数1在这里没有实际意义”。

现在创建epoll实例，更推荐使用的函数是`epoll_create1(0)`，它是`epoll_create()`的升级版，可以设置更多的选项。在这个函数中，参数0表示不设置任何选项。

所以，`epoll_create(1)`这行代码的作用就是创建一个新的epoll实例，并返回一个文件描述符，这个文件描述符可以用于后续的epoll操作（如添加监听的文件描述符、获取就绪的事件等）。但参数1在这里并没有起到实际作用。

在Linux内核中，`SYSCALL_DEFINE1()`是一个用于定义系统调用的宏。系统调用是运行在用户空间的程序向运行在内核空间的操作系统内核请求服务的一种机制。在这个宏中，"1"表示系统调用接受一个参数，`epoll_create1`和`epoll_create`是系统调用的名称，`int`是参数的类型，`flags`和`size`是参数的名称。

这两个系统调用都是用来创建epoll实例的。

1. `epoll_create1`的参数`flags`是一个整数，它用来设置epoll的行为。在当前的Linux内核中（截至2021年9月），`flags`可以是`EPOLL_CLOEXEC`，这会导致新创建的epoll文件描述符在执行`exec()`系统调用时被关闭，这可以防止子进程无意间继承父进程的文件描述符。

2. `epoll_create`的参数`size`是一个整数，它在原始设计中用于设置epoll实例的大小（即最多可以监听多少个文件描述符）。然而，如前所述，从Linux 2.6.8开始，这个参数不再起作用。如果传入的`size`小于或等于0，函数会返回错误码`-EINVAL`，表示非法参数。否则，函数会调用`do_epoll_create(0)`创建一个新的epoll实例。注意这里传给`do_epoll_create()`的参数是0，而不是`size`，这是因为`size`参数现在已经不再使用。

`do_epoll_create()`是内核中实际创建epoll实例的函数，这两个系统调用都会调用这个函数。这个函数的定义并没有在你给出的代码中，所以我无法给你更多的关于它的信息。
```c
SYSCALL_DEFINE1(epoll_create1, int, flags)
{
	return do_epoll_create(flags);
}

SYSCALL_DEFINE1(epoll_create, int, size)
{
	if (size <= 0)
		return -EINVAL;

	return do_epoll_create(0);
}
```
Linux内核使用一种叫红黑树的数据结构来存储被监视的文件描述符。红黑树是一种自平衡的二叉搜索树，它在添加或删除节点时，会通过一系列的旋转操作来保持树的平衡，从而保证了操作的高效性。

在创建epoll实例时，内核初始化一个空的红黑树。当添加新的文件描述符到epoll实例时，内核会为新的文件描述符创建一个节点，并将这个节点插入到红黑树中。如果插入节点后树失去平衡，内核会自动调整树的结构，使其重新平衡。

当文件描述符从epoll实例中移除时，内核会找到对应的节点，并将其从红黑树中删除。同样，如果删除节点后树失去平衡，内核也会自动调整树的结构。

通过这种方式，内核可以根据需要动态地添加或删除节点，从而动态地分配或释放空间。同时，由于红黑树的性质，这些操作的时间复杂度都是对数级的，因此非常高效。

需要注意的是，这里我提供的是一个简化的描述。实际上，Linux内核的实现可能更复杂，因为它还需要考虑如何处理并发操作，如何高效地寻找超时的文件描述符，等等。但基本的思想是一样的：通过红黑树这样的数据结构，可以动态地分配和释放空间，以处理大量的文件描述符。

### "static inline"
"static inline"在C语言中是两个关键字的组合，各自有各自的含义。

- "static"关键字的意思是，这个函数只在定义它的源文件中可见。这样可以防止其他源文件误用这个函数，增加了代码的封装性和安全性。同时，也避免了命名冲突的问题。

- "inline"关键字的意思是，这个函数是内联函数。当调用一个内联函数时，编译器会尝试将函数的代码直接插入到调用点，而不是生成函数调用的代码。这样可以减少函数调用的开销（例如保存和恢复寄存器，设置栈等），提高程序的运行效率。特别是对于那些很小、调用频繁的函数，使用内联函数可以获得显著的性能提升。

然而，要注意的是，"inline"只是给编译器一个建议，编译器可以选择忽略这个建议。如果函数体很大，或者函数中存在复杂的控制流（例如循环或递归），那么编译器可能会决定不进行内联。

至于为什么要将`__poller_add_fd`设为静态内联函数，这主要基于以下考虑：
- 从函数的实现来看，`__poller_add_fd`函数体非常小，基本上就是调用一次`epoll_ctl()`函数。这种小函数非常适合设为内联函数，以减少函数调用的开销。
- 从函数的用途来看，`__poller_add_fd`可能会在多个地方被频繁调用，因此设为内联函数可以提高程序的运行效率。
- 同时，`__poller_add_fd`可能只在定义它的源文件中使用，不需要被其他源文件访问，因此设为静态函数可以增加代码的封装性和安全性。

## epoll_wait

函数`int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout)` 是 Linux 中用于等待事件发生的函数。

解释该函数的参数和行为如下：

- `epfd`：表示 epoll 实例的文件描述符，即通过调用 `epoll_create` 或 `epoll_create1` 函数创建的返回值。该参数指定要等待事件的 epoll 实例。
- `events`：指向 `struct epoll_event` 结构体数组的指针，用于存储事件。函数将从内核空间将触发的事件复制到 `events` 数组中。
- `maxevents`：表示 `events` 数组的大小，即最多能存储多少个事件。
- `timeout`：表示等待事件的超时时间。具体取值有以下三种情况：
  - 正整数：表示等待事件的超时时间（以毫秒为单位）。如果在指定的超时时间内没有任何事件发生，则函数返回 0。
  - 0：表示非阻塞模式，即无论是否有事件发生，函数立即返回。如果没有事件发生，函数返回 0；如果有事件发生，函数返回获取到的事件数量。
  - -1：表示无限等待，即一直等待直到有事件发生。函数将一直阻塞，直到有事件发生后才返回。

函数返回值为整数类型，表示实际获取到的事件数量。如果函数成功获取到事件，返回值大于 0；如果函数发生错误，返回值为 -1，并设置相应的错误码。

在调用 `epoll_wait` 函数后，函数将会等待事件的发生。当有事件发生时，函数将把触发的事件复制到 `events` 数组中，并返回实际获取到的事件数量。程序可以根据返回的事件数量和 `events` 数组中的数据来处理相应的事件。
## 新节点的超时时间在红黑树中的作用，以及在poller中的作用

在很多网络和I/O编程中，我们常常会碰到一种需求，即对一些操作设置超时时间。超时机制对于保证系统的响应性和稳定性非常重要，它可以防止程序因等待某些长时间未完成的操作而被无限期阻塞。

在你提供的代码中，`__poller_node`结构体包含一个名为`timeout`的字段，它表示该节点相关操作的超时时间。该超时时间在`__poller_tree_insert`函数中，通过`__timeout_cmp()`函数，被用来决定新插入节点在红黑树中的位置。这种方式保证了红黑树中节点的超时时间是按照从小到大的顺序排列的，从而可以快速找到最先超时的节点（即红黑树最左侧的节点）。

`poller`中的`tree_first`字段记录了红黑树中最先超时的节点。这在处理超时事件时非常有用。当检查超时事件时，程序只需检查`tree_first`指向的节点是否超时，如果没有超时，则其它节点肯定也没有超时，因为他们的超时时间都比`tree_first`更晚；如果`tree_first`节点超时了，则处理该节点的超时事件，并将`tree_first`移动到下一个超时时间最早的节点。这样的设计大大提高了超时检查的效率。

总的来说，这个设计通过红黑树管理超时的节点，并利用超时时间作为排序依据，从而在大量的节点中快速找到最早超时的节点，并及时处理超时事件。

这个设计中，链表和红黑树都被用来存储和管理超时事件。

链表：超时列表（timeo_list）主要用于存储不需要按照特定顺序（例如，按照超时时间）排序的超时事件。因为链表是一个线性的数据结构，所以它的插入和删除操作都是O(1)的复杂度。当新的超时事件需要被添加到列表中时，只需要将其添加到列表的尾部即可。当一个超时事件被处理后，只需要从链表中删除它即可。

红黑树：当新的超时事件需要按照特定的顺序（例如，按照超时时间）排序时，就需要将其插入到红黑树中。红黑树是一种自平衡的二叉搜索树，它可以在O(log n)的时间内完成搜索、插入和删除操作。这就意味着，无论树中有多少个节点，执行这些操作的速度都是非常快的。在这个设计中，红黑树被用来存储需要按照超时时间排序的超时事件。

总的来说，这两种数据结构的选择是为了在插入、删除和查找超时事件时，能够提供更好的性能。链表在插入和删除操作时非常高效，而红黑树在按照特定顺序查找和插入节点时非常高效。