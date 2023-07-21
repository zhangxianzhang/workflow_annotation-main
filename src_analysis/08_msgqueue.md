#! https://zhuanlan.zhihu.com/p/416556786
# workflow 源码解析 06 : msgqueue 

项目源码 : https://github.com/sogou/workflow

更加详细的源码注释可看 : https://github.com/chanchann/workflow_annotation

##

内部使用两个队列，
分别拆开了生产者和消费者之间的争抢，
提升了吞吐的同时，
依然维持了比较优秀的长尾、
且兼顾保序与代码极简的优良传统；
而其中极度节约的链表实现方式，
在减少内存分配上也有可学习之处


首先，我们来看看这个消息队列库的优点：

1. **简单易懂**：代码清晰，逻辑易于理解。尽管涉及到多线程和同步操作，但代码中对复杂的问题进行了很好的抽象，使得其理解起来比较直观。

2. **线程安全**：通过使用互斥锁（mutexes）和条件变量（condition variables），代码可以在多线程环境下安全地执行，避免了数据竞争（data race）等问题。

3. **阻塞和非阻塞模式**：队列提供了阻塞和非阻塞两种模式。阻塞模式下，如果队列为空，`get` 操作会阻塞等待，直到有新的消息入队。如果队列已满，`put` 操作也会阻塞等待，直到队列有空位。在非阻塞模式下，`get` 和 `put` 操作在上述情况下不会阻塞。

4. **较少的上下文切换**：当 get_list 为空时，将会和 put_list 进行交换。如果队列非常繁忙并且消费者数量较大，这种策略可以减少上下文切换。

然后，我们再来看看它的一些潜在缺点：

1. **内存管理**：这个库并没有内置内存管理。你需要自己手动分配和释放消息所需的内存，这可能会导致内存泄漏，如果程序员不小心的话。

2. **缺乏灵活性**：在当前的设计下，所有的消息都需要有相同的大小，这对于有不同大小需求的应用程序来说可能不够灵活。

3. **消息类型单一**：消息队列只支持一种类型的消息，对于需要处理多种类型消息的应用程序，可能需要做一些额外的工作，如设计一种通用的消息类型。

4. **缺乏错误处理**：代码中几乎没有错误处理。例如，如果 `pthread_mutex_lock` 或 `pthread_cond_wait` 失败，程序可能会处于不可预知的状态。

5. **缺乏可配置性**：消息队列的最大长度和消息的链接偏移量都是在创建队列时设置的，并且之后不能更改。对于需要动态调整这些参数的应用来说，这可能是一个问题。

总的来说，这是一个简单但有效的消息队列实现，适合于需要高效进行大量消息传递的场景。然而，对于更复杂的需求，可能需要进行一些改进或寻找其他的消息队列库。

## 不同类型的消息队列的实现

这些是各种不同类型的消息队列的实现。他们在使用场景、性能和复杂性等方面都有所不同。

**链表版：LinkedBlockingQueue**

`LinkedBlockingQueue`是一个基于链表结构的阻塞队列，其内部维护了一个链表来存放队列元素。这个阻塞队列实现了Java中的`BlockingQueue`接口，支持线程安全的插入和移除操作。如果队列已满，插入操作将会阻塞直到有空间可用；同样，如果队列为空，移除操作将会阻塞直到有元素可用。

**gRPC版：mpmc queue**

这个`mpmc queue`（多生产者多消费者队列）是gRPC库中用于线程间通信的一种消息队列。它支持多个生产者线程和多个消费者线程同时向队列中添加和移除元素。这个队列的实现使用了原子操作和内存屏障来确保线程安全，而不是使用锁，因此它的性能可能会比使用锁的实现更好。

**无锁版：内核的单生产者单消费者kfifo**

Linux内核的`kfifo`是一个无锁的循环缓冲区（或称为环形队列），它特别适用于单生产者单消费者的场景。`kfifo`使用原子操作来同步生产者和消费者，避免了使用锁，因此它的性能非常好。但是，这种无锁的实现也有一些限制，例如，它只能在单生产者单消费者的场景中使用。

**不保序版：go的调度workstealing**

Go语言的运行时使用了一种叫做work-stealing的调度算法。在这个算法中，每个线程都有一个本地的任务队列。当一个线程的本地任务队列为空时，它会尝试从其他线程的任务队列中"窃取"任务。这种方法能够充分利用CPU资源，避免某些线程闲置而其他线程过载。但是，由于任务可能在不同的线程之间移动，所以这种方法不能保证任务的执行顺序。

## msgqueueg接口

perl calltree.pl '(?i)msgqueue' '' 1 1 2

```
(?i)msgqueue
├── __msgqueue_swap
│   └── msgqueue_get  [vim src/kernel/msgqueue.c +102]
├── msgqueue_create
│   └── Communicator::create_poller   [vim src/kernel/Communicator.cc +1319]
├── msgqueue_destroy
│   ├── Communicator::create_poller   [vim src/kernel/Communicator.cc +1319]
│   ├── Communicator::init    [vim src/kernel/Communicator.cc +1356]
│   └── Communicator::deinit  [vim src/kernel/Communicator.cc +1380]
├── msgqueue_get
│   └── Communicator::handler_thread_routine  [vim src/kernel/Communicator.cc +1083]
├── msgqueue_put
│   └── Communicator::callback        [vim src/kernel/Communicator.cc +1256]
└── msgqueue_set_nonblock
    ├── Communicator::create_handler_threads  [vim src/kernel/Communicator.cc +1286]
    └── Communicator::deinit  [vim src/kernel/Communicator.cc +1380]
```

```cpp
msgqueue_t *msgqueue_create(size_t maxlen, int linkoff);
void msgqueue_put(void *msg, msgqueue_t *queue);
void *msgqueue_get(msgqueue_t *queue);
void msgqueue_set_nonblock(msgqueue_t *queue);
void msgqueue_set_block(msgqueue_t *queue);
void msgqueue_destroy(msgqueue_t *queue);
```

作为一个生产者消费者模型，我们最为核心的两个接口就是msgqueue_put和msgqueue_get。

我们重点讲解这两个接口及其在wf中是如何使用的

## 一个小demo

https://github.com/chanchann/workflow_annotation/blob/main/demos/25_msgque/25_msgque.cc

```cpp
int main()
{
    msgqueue_t *mq = msgqueue_create(10, -static_cast<int>(sizeof (void *)));
    char str[sizeof (void *) + 6];
    char *p = str + sizeof (void *);
    strcpy(p, "hello");
    msgqueue_put(p, mq);
    p = static_cast<char *>(msgqueue_get(mq));
    printf("%s\n", p);
    return 0;
}
```

## msgqueue_create - 先初始化(需要注意linkoff)

我们上一节看到的 create_poller

```cpp
int Communicator::create_poller(size_t poller_threads)
{
	struct poller_params params = 默认参数;

	msgqueue_create(4096, sizeof (struct poller_result));
	mpoller_create(&params, poller_threads);
	mpoller_start(this->mpoller);
}
```

create_poller 完成这几件事 : msgqueue_create, mpoller_create, mpoller_start

我们已经知道如何创建poller并启动，现在来看看创建msgqueue

这里就是分配空间，初始化

```cpp
msgqueue_t *msgqueue_create(size_t maxlen, int linkoff)
{
	msgqueue_t *queue = (msgqueue_t *)malloc(sizeof (msgqueue_t));

	pthread_mutex_init(&queue->get_mutex, NULL)
	pthread_mutex_init(&queue->put_mutex, NULL);
	pthread_cond_init(&queue->get_cond, NULL);
	pthread_cond_init(&queue->put_cond, NULL);

	queue->msg_max = maxlen;
	queue->linkoff = linkoff;
	queue->head1 = NULL;
	queue->head2 = NULL;
	queue->get_head = &queue->head1;
	queue->put_head = &queue->head2;
	queue->put_tail = &queue->head2;
	queue->msg_cnt = 0;
	queue->nonblock = 0;
	...
}
```

这里比较重要的就是linkoff，我们在msgqueue_put可以看出他的作用

```cpp
typedef struct __msgqueue msgqueue_t;

// 消息队列就是个单链表
// 此处有两个链表，高效swap使用
struct __msgqueue
{
	size_t msg_max;
	size_t msg_cnt;
	int linkoff;
	int nonblock;
	void *head1;     // get_list   
	void *head2;     // put_list
	// 两个list，高效率，一个在get_list拿，一个在put_list放
	// 如果get_list空，如果put_list放了的话，那么swap一下就可了，O(1),非常高效，而且互不干扰
	void **get_head;	
	void **put_head;
	void **put_tail;
	pthread_mutex_t get_mutex;
	pthread_mutex_t put_mutex;
	pthread_cond_t get_cond;
	pthread_cond_t put_cond;
};
```

## msgqueue_put, put - 生产者

就是把epoll收到的消息队列加入到消息队列中

```cpp
void Communicator::callback(struct poller_result *res, void *context)
{
	Communicator *comm = (Communicator *)context;
	msgqueue_put(res, comm->queue);
}
```

```cpp
void msgqueue_put(void *msg, msgqueue_t *queue)
{
	// 这里转char* 是因为，void* 不能加减运算，但char* 可以
	void **link = (void **)((char *)msg + queue->linkoff);
	/*
	this->queue = msgqueue_create(4096, sizeof (struct poller_result));
	初始化的时候把linkoff大小设置成了sizeof (struct poller_result)
	*/
	// msg头部偏移linkoff字节，是链表指针的位置。使用者s需要留好空间。这样我们就无需再malloc和free了
	// 我们就是把一个个的struct poller_result 串起来
	*link = NULL; 

	pthread_mutex_lock(&queue->put_mutex);
	
	// 当收到的cnt大于最大限制 且 阻塞mode(default)， 那么wait在这, 等待消费者去给消费了
	while (queue->msg_cnt > queue->msg_max - 1 && !queue->nonblock)
		pthread_cond_wait(&queue->put_cond, &queue->put_mutex);

	*queue->put_tail = link;  // 把 link串到链尾
	queue->put_tail = link;   // 然后把这个指针移过来

	queue->msg_cnt++;
	pthread_mutex_unlock(&queue->put_mutex);

	pthread_cond_signal(&queue->get_cond);
}
```

这一行代码在进行类型转换和内存地址的计算。让我们一步步地解释它。

首先，`msg`是一个 `void*` 类型的指针，指向了一个消息对象的内存。`queue->linkoff` 是一个偏移量，表示在这个消息对象内部，链接到下一个消息对象的指针字段相对于消息对象开始的字节偏移。

`(char *)msg + queue->linkoff` 这部分代码先将 `msg` 从 `void*` 类型转换为 `char*` 类型。因为 `char` 的大小为 1 字节，所以 `char*` 可以用来表示字节级别的内存地址。然后，它在这个地址上加上 `queue->linkoff`，结果是一个新的 `char*` 指针，指向消息对象内部的链接字段的地址。

最后， `(void **)((char *)msg + queue->linkoff)` 这部分代码将这个 `char*` 指针再次转换为 `void**` 类型。这是因为链接字段实际上是一个 `void*` 类型的指针，而我们需要一个指向这个链接字段（即指向这个 `void*` 指针）的指针，所以用 `void**` 类型。

所以，整行代码的意思是计算出消息对象内部链接字段的地址，并将这个地址以 `void**` 类型的指针形式保存在 `link` 中。这个 link 指针会被用来存储下一个消息的地址

让我们通过一个具体的例子来理解这个概念。假设我们有一个消息对象，我们称之为 `Message`，其中有一个 `void*` 类型的成员 `A`，它用于链接到下一个 `Message` 对象。假设 `A` 成员相对于 `Message` 对象开始的字节偏移是 `linkoff`。

```cpp
struct Message {
    // ... 其他成员 ...
    void* A;
};

// 创建一个 Message 对象
Message* msg = new Message;

// 计算 A 成员的地址
void** link = (void**)((char*)msg + linkoff);
```

在这个例子中，`(char*)msg + linkoff` 这部分代码首先将 `msg` 从 `Message*` 类型转换为 `char*` 类型，然后在这个地址上加上 `linkoff`。结果是一个 `char*` 指针，指向 `Message` 对象内部的 `A` 成员的地址。

然后， `(void**)((char*)msg + linkoff)` 这部分代码将这个 `char*` 指针再次转换为 `void**` 类型。这是因为 `A` 成员实际上是一个 `void*` 类型的指针，而我们需要一个指向这个 `A` 成员（即指向这个 `void*` 指针）的指针，所以用 `void**` 类型。

所以，整行代码的意思是计算出 `Message` 对象内部 `A` 成员的地址，并将这个地址以 `void**` 类型的指针形式保存在 `link` 中。

需要注意的是，`linkoff` 的值需要根据 `Message` 对象的实际内存布局来确定。在实际的代码中，通常会有某种方式来获取或计算 `linkoff` 的值，例如通过 `offsetof` 宏等。

实际上，`&A` 和 `link` 表达的是相同的概念。这两个都代表了内存中某个位置的地址。`&A` 是直接获取变量 `A` 的内存地址，而 `link` 是通过一些计算（根据消息对象的起始地址和链接字段的偏移）得到的同样的地址。

当你写下 `void **link = (void **)((char *)msg + queue->linkoff);`，这里的 `link` 是一个指向指针的指针，它的值（也就是它所指向的地址）是消息对象内部链接字段 `A` 的地址。

如果 `queue->linkoff` 正确地表示了链接字段 `A` 相对于消息对象开始的字节偏移，那么 `link` 和 `&A` 就应该是同一个地址。换句话说，如果 `queue->linkoff` 是 `A` 字段的偏移量，那么 `link` 的值应该等于 `&A` 的值，也就是说 `link == &A`。

在C++中，这两个表达式（`link` 和 `&A`）都表示内存中相同位置的地址，这个位置就是 `A` 的存储位置。所以，是的，如果 `queue->linkoff` 是 `A` 字段的偏移量，那么 `&A == link`。

这个函数就是把msg添加到了queue后面串起来

```cpp
*queue->put_tail = link;  // 把 link串到链尾
queue->put_tail = link;   // 然后把这个指针移过来
```

在这个上下文中，`queue->put_tail` 是一个指向链表最后一个元素中的 "link" 字段的指针。"link" 字段是用于链表链接的字段。

当你添加一个新的元素（比如说，节点D）到链表的末尾时，你先获得这个新元素的 "link" 字段的地址，然后存储到 `link` 变量中。换句话说，`link` 是一个指针，它指向新添加元素的 "link" 字段。

然后，你用这个 `link` 变量（它包含新添加元素的 "link" 字段的地址）来更新链表的最后一个元素的 "link" 字段。你通过 `*queue->put_tail = link;` 来完成这个操作。这一步在链表的最后一个元素（节点C）的 "link" 字段中存储了新节点D的 "link" 字段的地址。此时，节点C的 "link" 字段指向了节点D，从而将节点D添加到了链表的末尾。

接着，你用 `queue->put_tail = link;` 来更新 `queue->put_tail` 的值。`queue->put_tail` 是一个指向链表最后一个元素 "link" 字段的指针，因此你需要将其更新为新添加的元素（节点D）的 "link" 字段的地址，也就是 `link` 的值。这样，`queue->put_tail` 现在指向的是新添加元素（节点D）的 "link" 字段，这也是链表新的最后一个元素。

让我们基于这个特定的链表 `A -> B -> C -> D` 来解释这两行代码。

在开始时，链表是 `A -> B -> C`，其中 `queue->put_tail` 指向节点C的 "link" 字段。

当我们要添加新的节点D到链表时：

1. 我们首先获取新节点D的 "link" 字段的地址，存储在 `link` 中。换句话说，`link` 指向新节点D的 "link" 字段的地址。

2. 然后，我们执行 `*queue->put_tail = link;`。这一步在链表的最后一个元素（即节点C）的 "link" 字段中存储了新节点D的 "link" 字段的地址。这实际上创建了一个从节点C到节点D的链接，所以现在链表看起来像这样：`A -> B -> C -> D`。

3. 接着，我们执行 `queue->put_tail = link;` 来更新 `queue->put_tail` 的值。`queue->put_tail` 是一个指向链表最后一个元素 "link" 字段的指针，因此我们将其更新为新节点D的 "link" 字段的地址。这样，`queue->put_tail` 现在指向新节点D的 "link" 字段，这也是链表新的最后一个元素。

所以，通过这两行代码，我们实现了将新的节点D添加到链表的末尾，并更新了 `queue->put_tail` 的值，使其指向新添加节点的 "link" 字段。这就是这两行代码所做的事情。


## msgqueue_get - get ：消费者

```cpp
void Communicator::handler_thread_routine(void *context)
{
	...
	while ((res = (struct poller_result *)msgqueue_get(comm->queue)) != NULL)
	{
		switch (res->data.operation)
		{
		case PD_OP_READ:
			comm->handle_read_result(res);
			break;
		...
		}
	}
}
```

msqqueue是epoll消息回来之后，以网络线程作为生产者往queue里放(上面`msgqueue_put(res, comm->queue);`)

执行线程作为消费者从queue里拿数据，从而做到线程互不干扰

```cpp
void *msgqueue_get(msgqueue_t *queue)
{
	pthread_mutex_lock(&queue->get_mutex);

	// 如果get_list有消息
	// 若get_list无消息了，那么看看put_list有没有，如果有，swap一下即可
	if (*queue->get_head || __msgqueue_swap(queue) > 0)
	{
		// *queue->get_head 是第一个
		// 转换为(char *)可做加减法
		// 其中保留了linkoff这么大的空间
		// this->queue = msgqueue_create(4096, sizeof (struct poller_result));
		// 初始化的时候把linkoff大小设置成了sizeof (struct poller_result)
		// 退回后就是msg的起始位置了
		msg = (char *)*queue->get_head - queue->linkoff;
		// *queue->get_head就是第一个元素
		// *(void **)*queue->get_head 就是第一个元素指向的下一个元素
		// 第一个元素移动过来
		*queue->get_head = *(void **)*queue->get_head;
	}
	else
	{
		msg = NULL;
		errno = ENOENT;
	}

	pthread_mutex_unlock(&queue->get_mutex);
	return msg;
}
```

这里还有个非常重要的细节__msgqueue_swap

我们两个list，一个在get_list拿，一个在put_list放

如果get_list空，如果put_list放了的话，那么swap一下就可了，O(1),非常高效，而且互不干扰

```cpp
static size_t __msgqueue_swap(msgqueue_t *queue)
{
	void **get_head = queue->get_head;
	size_t cnt;
	// 将get_head切换好，因为就算put在加，put_head也不会变, 所以不需要加锁
	queue->get_head = queue->put_head;  

	pthread_mutex_lock(&queue->put_mutex);

	// 如果put_list也没有消息且为阻塞态，那么就wait等到放进来消息
	while (queue->msg_cnt == 0 && !queue->nonblock)
		pthread_cond_wait(&queue->get_cond, &queue->put_mutex);

	cnt = queue->msg_cnt;  
	// 如果cnt大于最大接收的msg，那么通知put，因为大于msg_max put_list wait在那里了，所以swap清空了就要唤醒生产者put
	if (cnt > queue->msg_max - 1)
		pthread_cond_broadcast(&queue->put_cond);

	queue->put_head = get_head;    // put_list就交换设置到get_list那个地方了  
	queue->put_tail = get_head;

	// put_list清0了
	// 收到put消息是queue->msg_cnt++, 并没有拿走消息queue->msg_cnt--;
	// 靠的就是put_list swap 到 get_list 就清0了
	queue->msg_cnt = 0;    

	pthread_mutex_unlock(&queue->put_mutex);
	return cnt;
}
```



