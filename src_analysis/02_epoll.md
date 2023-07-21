#! https://zhuanlan.zhihu.com/p/474211432
# workflow 源码解析 : epoll 

项目源码 : https://github.com/sogou/workflow

更加详细的源码注释可看 : https://github.com/chanchann/workflow_annotation

workflow作为一个网络库，必然先从epoll看起

##  怎样设置epoll的等待事件类型
设置epoll的等待事件类型通常是在调用`epoll_ctl()`函数时完成的，具体来说，主要是通过在`struct epoll_event`结构中设置`events`字段来实现的。

以下是一个例子来说明如何设置事件类型：

```c
#include <sys/epoll.h>
#include <fcntl.h>

int main()
{
    int fd = open("test.txt", O_RDONLY);  // 打开文件
    int epoll_fd = epoll_create1(0);      // 创建epoll实例

    struct epoll_event event;
    event.data.fd = fd;                   // 需要监听的文件描述符

    // 设置需要监听的事件类型
    event.events = EPOLLIN | EPOLLET;     // EPOLLIN 表示可读，EPOLLET 表示采用边缘触发模式

    // 将文件描述符添加到epoll实例，设置对应的监听事件
    if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) == -1)
    {
        perror("epoll_ctl");
        exit(EXIT_FAILURE);
    }

    // ...此处可以调用epoll_wait等待事件发生

    return 0;
}
```

在这个例子中，我们首先打开了一个文件，并创建了一个epoll实例。然后，我们创建了一个`epoll_event`结构，并在其中设置了我们想要监听的文件描述符和事件类型。在这种情况下，我们选择监听`EPOLLIN`事件（表示文件描述符可以读取数据），并选择`EPOLLET`（边缘触发模式）。最后，我们通过调用`epoll_ctl()`将这个文件描述符和对应的事件添加到epoll实例中。

注意：在实际的程序中，还需要正确地处理错误和进行资源的清理。

是的，可以在`epoll_wait()`之后再调用`epoll_ctl()`。实际上，这是非常常见的使用模式。

`epoll_wait()`函数是阻塞的，它会一直等待，直到至少有一个文件描述符准备好了对应的事件（或者当超时或者接收到一个信号时返回）。一旦`epoll_wait()`返回，我们可以查看哪些文件描述符准备好了事件，并相应地处理它们。

在处理事件的过程中，我们可能需要修改某些文件描述符的监听事件，或者添加或删除一些文件描述符。这时，我们就可以再次调用`epoll_ctl()`来进行这些操作。

以下是一个简单的例子：

```c
struct epoll_event event;
// ...其他初始化代码...

// 首次添加文件描述符到epoll实例
epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event);

while (1) {
    // 等待事件
    int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
    for (int i = 0; i < n; i++) {
        // 处理事件...
        // 在处理事件的过程中，我们可能需要修改文件描述符的监听事件
        event.events = EPOLLOUT; // 例如，我们现在只关心可写事件
        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &event);
    }
}
```

这个例子中，我们首次添加文件描述符到epoll实例进行监听。然后我们进入一个无限循环，等待事件发生并处理它们。在事件处理过程中，我们可能需要修改文件描述符的监听事件，这时我们就再次调用`epoll_ctl()`进行修改。

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

### 监听的scoket被设置为非阻塞模式会影响epoll_wait()吗？
非阻塞模式对 `epoll_wait()` 的行为没有影响。无论你监听的 socket 是否设置为非阻塞模式，只要有事件发生，`epoll_wait()` 就会返回。

要理解这个问题，首先需要明确阻塞和非阻塞模式的区别：

- 阻塞模式（Blocking mode）：在这种模式下，对 socket 的操作（比如读、写）会阻塞调用的进程，直到操作完成（比如，读操作读到了数据，或者写操作将数据写出去）。
- 非阻塞模式（Non-blocking mode）：在这种模式下，如果对 socket 的操作不能立即完成，操作会立即返回，通常会返回一个错误，表明操作未能完成。

当你把 socket 设置为非阻塞模式后，对这个 socket 的 I/O 操作（如 `read()`, `write()`, `accept()`, `connect()`）会立即返回，而不会阻塞调用的进程。但这并不影响 `epoll_wait()` 的行为：`epoll_wait()` 仍然会在没有事件发生时阻塞，而在有事件发生时返回。

简单来说，socket 的阻塞/非阻塞模式决定了你如何与 socket 进行交互，而 `epoll_wait()` 是 epoll 的事件等待机制，两者属于不同的层面，不应该混淆。

## epoll切入口 ： __poller_wait

从man page我们可以看到

https://man7.org/linux/man-pages/man7/epoll.7.html

epoll一共就三个核心api

epoll_create， epoll_ctl， epoll_wait

而把握了epoll_wait，就能知道怎么处理事件的逻辑。

只有一处出现，这里就是最为核心的切入口

```cpp
/src/kernel/poller.c
static inline int __poller_wait(__poller_event_t *events, int maxevents,
								poller_t *poller)
{
	// int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);
	// Specifying a timeout of -1 causes epoll_wait() to block indefinitely
	return epoll_wait(poller->pfd, events, maxevents, -1);
}
```

此处`__poller_wait` 只是为了跨平台统一接口

## __poller_thread_routine

而调用`__poller_wait`的是`__poller_thread_routine`

```cpp
static void *__poller_thread_routine(void *arg)
{
	...
	while (1)
	{
        ...
		nevents = __poller_wait(events, POLLER_EVENTS_MAX, poller);
        ...
		for (i = 0; i < nevents; i++)
		{
			node = (struct __poller_node *)__poller_event_data(&events[i]);
			if (node > (struct __poller_node *)1)
			{
				switch (node->data.operation)
				{
				case PD_OP_READ:
					__poller_handle_read(node, poller);
					break;
				case PD_OP_WRITE:
					__poller_handle_write(node, poller);
					break;
					...
				}
			}
            ...
		}   
        ...
}
```

这里是将epoll触发的事件数组，挨着挨着根据他们的operation分发给不同的行为函数(read/write....)

## poller_start

`__poller_thread_routine` 是一个线程 `pthread_create` 时输入的执行函数

```cpp
int poller_start(poller_t *poller)
{
	...
	pthread_mutex_lock(&poller->mutex);
	if (__poller_open_pipe(poller) >= 0)
	{
		ret = pthread_create(&tid, NULL, __poller_thread_routine, poller);
		...
	}
	pthread_mutex_unlock(&poller->mutex);
	return -poller->stopped;
}
```

poller_start的作用就是启动 `__poller_thread_routine` 线程。

## mpoller_start

调用 `poller_start` 的是 `mpoller_start`

```cpp
int mpoller_start(mpoller_t *mpoller)
{
	...
	for (i = 0; i < mpoller->nthreads; i++)
	{
		if (poller_start(mpoller->poller[i]) < 0)
			break;
	}
	...;
}
```

mpoller的职责，是start我们设置的epoll线程数的epoll线程

## create_poller

`mpoller_start` 在 `create_poller` 的时候启动

```cpp
int Communicator::create_poller(size_t poller_threads)
{
	struct poller_params params = 默认参数;

	msgqueue_create();  // 创建msgqueue
	mpoller_create(&params, poller_threads);  // 创建poller
	mpoller_start();  // poller 启动

	return -1;
}
```

## Communicator::init

```cpp
int Communicator::init(size_t poller_threads, size_t handler_threads)
{
	....
	create_poller(poller_threads);   // 创建poller线程
	create_handler_threads(handler_threads);  // 创建线程池
	....
}
```

## CommScheduler

```cpp
class CommScheduler
{
public:
	int init(size_t poller_threads, size_t handler_threads)
	{
		return this->comm.init(poller_threads, handler_threads);
	}

    ...
private:
	Communicator comm;
};
```

CommScheduler仅有一个成员变量Communicator, 对于Communicator来说就是对外封装了一层, 加入了一些逻辑操作，本质上都是comm的操作

而`CommScheduler` init 也就是 `Communicator` init

我们也可以看出，参数正好传入了epoll线程数量

## WFGlobal::get_scheduler()

CommScheduler是全局的单例

```cpp
class WFGlobal
{
public:
	static CommScheduler *get_scheduler();
    ...
}
```

CommScheduler作为全局单例就很自然，调度就是全局的策略，而且我们看到了CommScheduler需要创建epoll线程池，不可能在运行我们不断多次创建。

我们知道，单例是第一次调用的时候，创建出对象，那么我们从客户端和服务端两方面来看，什么时候调用`get_scheduler`

## 客户端

我们还是从之前文章写过的TimerTask看

```cpp
WFTimerTask *WFTaskFactory::create_timer_task(unsigned int microseconds,
											  timer_callback_t callback)
{
	return new __WFTimerTask((time_t)(microseconds / 1000000),
							 (long)(microseconds % 1000000 * 1000),
							 WFGlobal::get_scheduler(),
							 std::move(callback));
}
```

在`create_timer_task`调用 `WFGlobal::get_scheduler()`

## 服务端

之前的文章也自此分析过`server`端的代码，其实和酷护短一样，在创建task的时候调用

```cpp
template<class REQ, class RESP>
WFNetworkTask<REQ, RESP> *
WFNetworkTaskFactory<REQ, RESP>::create_server_task(CommService *service,
				std::function<void (WFNetworkTask<REQ, RESP> *)>& process)
{
	return new WFServerTask<REQ, RESP>(service, WFGlobal::get_scheduler(),
									   process);
}
```

## 正向分析流程

我们上文都是从里到外，一层层分析

我们从正常流程出发来分析就清楚了

1. 就拿我们的server来说，我们第一次`create_server_task` 来创建 `server_task` 的时候，调用了全局的 `WFGlobal::get_scheduler()`

2. 初始化CommScheduler

```cpp
CommScheduler *WFGlobal::get_scheduler()
{
	return __CommManager::get_instance()->get_scheduler();
}
```

```cpp
static __CommManager *get_instance()
{
    static __CommManager kInstance;
    return &kInstance;
}
```

```cpp
__CommManager():
    io_server_(NULL),
    io_flag_(false),
    dns_manager_(NULL),
    dns_flag_(false)
{
    const auto *settings = __WFGlobal::get_instance()->get_global_settings();
    if (scheduler_.init(settings->poller_threads,
                        settings->handler_threads) < 0)
        abort();

    signal(SIGPIPE, SIG_IGN);
}
```

这里我们从setting中找到我们设置的epoll线程数，初始化 `CommScheduler`

3. Communicator::init

```cpp
int init(size_t poller_threads, size_t handler_threads)
{
    return this->comm.init(poller_threads, handler_threads);
}
```

4. Communicator::init其实就是在创建并启动epoll线程

```cpp
int Communicator::init(size_t poller_threads, size_t handler_threads)
{
    ...
    this->create_poller(poller_threads);
    ...
}
```

然后这里 `mpoller_create` 为批量生产epoll线程并启动

注意我们的`Communicator::init` 中的 `create_poller` 其实上是 `create_and_start_poller`


5. 创建epoll线程

```cpp
int Communicator::create_poller(size_t poller_threads)
{
    ...
    mpoller_create(&params, poller_threads);
    ...
    mpoller_start(this->mpoller);
    ...
}
```

先创建epoll

```cpp
mpoller_t *mpoller_create(const struct poller_params *params, size_t nthreads)
{
    ...
    __mpoller_create(params, mpoller) >= 0
    ...
}
```

```cpp
static int __mpoller_create(const struct poller_params *params,
							mpoller_t *mpoller)
{
    ...
	for (i = 0; i < mpoller->nthreads; i++)
	{
		mpoller->poller[i] = poller_create(params);  
        ...
	}   
    ...
}
```

```cpp
poller_t *poller_create(const struct poller_params *params)
{
    poller->pfd = __poller_create_pfd();  
    ...
    // 初始化各个poller的参数
    ...
}
```

这里就到底了

```cpp
static inline int __poller_create_pfd()
{
	return epoll_create(1);
}
```

看，这里就是epoll三大api的`epoll_create`, 所以说只要从`epoll_wait`开始，就可以摸清楚epoll如何产生，运行的核心逻辑。

6. 启动epoll线程

```cpp
int mpoller_start(mpoller_t *mpoller)
{
	for (i = 0; i < mpoller->nthreads; i++)
	{
		poller_start(mpoller->poller[i]);
        ...
	}
    ...
}
```

```cpp
int poller_start(poller_t *poller)
{
    ...
    pthread_create(&tid, NULL, __poller_thread_routine, poller);
    ...
}
```

启动了以 `__poller_thread_routine` 为核心的epoll线程

7. epoll线程核心逻辑

```cpp
static void *__poller_thread_routine(void *arg)
{
	...
	while (1)
	{
        ...
		nevents = __poller_wait(events, POLLER_EVENTS_MAX, poller);
        ...
		for (i = 0; i < nevents; i++)
		{
			node = (struct __poller_node *)__poller_event_data(&events[i]);
			if (node > (struct __poller_node *)1)
			{
				switch (node->data.operation)
				{
				case PD_OP_READ:
					__poller_handle_read(node, poller);
					break;
				case PD_OP_WRITE:
					__poller_handle_write(node, poller);
					break;
					...
				}
			}
            ...
		}   
        ...
}
```

这里就是个无限循环，不断的检测是否有事件触发，然后根据注册的operation来分发给不同的操作函数(read/write...)

## 后续

从这里我们就大概理清楚了workflow的核心epoll线程，是如何创建，核心运行的逻辑是什么。

我们这里只是简单分析了整个流程的骨架，但还有很多细节还没分析，比如 `poller_t`，`__poller_node`，`poller_data` 是什么，为何这样设计。我们先屏蔽这些细节，等我们具体分析一个完整的流程再来看隐藏在细节中的魔鬼。