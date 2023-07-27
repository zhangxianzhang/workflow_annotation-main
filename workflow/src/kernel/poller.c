/*
  Copyright (c) 2019 Sogou, Inc.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Author: Xie Han (xiehan@sogou-inc.com)
*/

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#ifdef __linux__
# include <sys/epoll.h>
# include <sys/timerfd.h>
#else
# include <sys/event.h>
# undef LIST_HEAD
# undef SLIST_HEAD
#endif
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include "list.h"
#include "rbtree.h"
#include "poller.h"


/* 定义轮询器的缓冲区大小为256KB。这个大小可能用于网络接收或发送缓冲区，或者其他需要一定大小缓冲区的操作。 */
#define POLLER_BUFSIZE			(256 * 1024)

/* 定义轮询器最大节点数为65536。这个数量可能用于限制轮询器可以处理的并发连接或事件的数量。这个数值的选取可能基于资源的考量，比如内存使用。 */
#define POLLER_NODES_MAX		65536

/* 定义轮询器一次可以处理的最大事件数为256。可能用于限制每次调用轮询函数时可以返回的事件数量。这可能会影响程序的响应速度和处理负载。 */
#define POLLER_EVENTS_MAX		256

/* 定义一个错误的__poller_node指针，当后续在处理epoll事件时，如果发现事件对应的文件描述符在nodes数组中的位置被标记为POLLER_NODE_ERROR，就知道这是一个特殊的情况。 */
#define POLLER_NODE_ERROR		((struct __poller_node *)-1)


/**
 * @brief 为了性能考虑我们的poller_node是一个以fd为下标的数组，而每个node只能关注一种事件，READ或WRITE。
 * todo: 这个数组在哪体现？？
 * 对于一个fd，在poller里是单工的。如果需要全双工，方法是通过系统调用dup()产生一个新的fd再加进来。
 * @note 可被转化为 struct poller_result
 * @note 在轮询器框架中，每一个被监控的对象（例如一个套接字或者一个定时器）都会被封装为一个__poller_node节点。轮询器会周期性地检查这些节点，当节点对应的事件（例如可读、可写、超时等）发生时，轮询器会调用预设的处理函数来处理这个事件。
 */
struct __poller_node
{
    int state;         /* 节点状态 */
    int error;         /* 错误码 */
    struct poller_data data;  /* 轮询器数据 */
#pragma pack(1)       /* 设置数据对齐为1字节，避免内存浪费 */

/*
红黑树在搜索操作中效率更高，链表在插入和删除操作中效率更高。轮询器可能根据具体的需求来选择使用哪种数据结构。
*/
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
    struct __poller_node *res; /* 被用来保存一个新创建的 __poller_node 结构体的地址，这个新创建的结构体用于存储一些临时信息，比如尝试将数据追加到节点的消息时产生的结果。 */
};

/* 轮询器结构体 */
struct __poller
{
	size_t max_open_files;  // 如果为0则默认为65536
	// 发生读事件时创建一条消息。poller既不是proactor也不是reactor，而是以完整的一条消息为交互单位。
	// 其中的void *参数来自于struct poller_data里的void *context（注意不是poller_params里的context）。
	// poller_message_t是一条完整的消息，这是一个变长结构体，需要而且只需要实现append
	poller_message_t *(*create_message)(void *); 
	// 写的时候表示写成功了一部分数据，一般用来更新下一个超时。
	// 参数void *是poller_data里的context。
	int (*partial_written)(size_t, void *);
	// 回调函数。用于处理由 poller 管理的各种事件（比如网络I/O事件或定时器事件）的结果，参数是一个poller_result和context。
	void (*cb)(struct poller_result *, void *);
	// callback里的void *参数。
	void *ctx;  

	pthread_t tid;	  // 将在线程创建成功后用于存储新线程的标识符
	int pfd;		  // epoll文件描述符
	int timerfd;	  ///* 定时器文件描述符 */
	int pipe_rd;      // pipe 的读端
	int pipe_wr;      // pipe 的写端
	int stopped;     // 是否运行标志位
    struct rb_root timeo_tree;  /* 是仅包含一个节点指针的类，用来表示超时时间红黑树的根节点 */
    struct rb_node *tree_first;  /* 红黑树中的第一个节点 */
    struct list_head timeo_list;  /* 超时时间链表头 */
    struct list_head no_timeo_list;  /* 无超时时间链表头 */
    struct __poller_node **nodes;  /* 新版本mpoller也聚合此动态数组。nodes指向数组元素为指针的动态数组，每个数组元素都指向一个与被监控对象相关联的 struct __poller_node 结构体实例。用红黑树或链表进行管理 */
    pthread_mutex_t mutex;  /* 用于同步的互斥量 */
    char buf[POLLER_BUFSIZE];  /* 缓冲区 */
};

#ifdef __linux__

// 之所以要封装，是因为不同平台，保持同样的接口

static inline int __poller_create_pfd()
{
	return epoll_create(1);
}

// __poller_add_fd函数的目的是将一个文件描述符添加到epoll事件监听列表中。
// 参数fd是要添加的文件描述符；
// 参数event是要监听的事件类型（如EPOLLIN表示监听输入事件）；
// 参数data是关联的数据，通常用作回调函数的参数；
// 参数poller是封装了epoll的结构体，poller->pfd是epoll的文件描述符。
// 函数通过调用epoll_ctl函数将文件描述符添加到epoll事件监听列表中，
// 并将返回值返回给调用者，表示操作是否成功。
static inline int __poller_add_fd(int fd, int event, void *data,
								  poller_t *poller)
{
	struct epoll_event ev = {
		.events		=	event,	// 设置关注的事件类型，可以是EPOLLIN（可读）、EPOLLOUT（可写）
		.data		=	{
			.ptr	=	data 	// 用指针设置关联的用户数据，供__poller_thread_routine
		}
	};
	// 调用 epoll_ctl() 函数向 epoll 实例（由 poller->pfd 指定）添加文件描述符（由 fd 指定）
	// 并设置关联的事件（由 ev 指定）
	return epoll_ctl(poller->pfd, EPOLL_CTL_ADD, fd, &ev);
}

static inline int __poller_del_fd(int fd, int event, poller_t *poller)
{
	return epoll_ctl(poller->pfd, EPOLL_CTL_DEL, fd, NULL);
}

static inline int __poller_mod_fd(int fd, int old_event,
								  int new_event, void *data,
								  poller_t *poller)
{
	struct epoll_event ev = {
		.events		=	new_event,
		.data		=	{
			.ptr	=	data
		}
	};
	return epoll_ctl(poller->pfd, EPOLL_CTL_MOD, fd, &ev);
}

static inline int __poller_create_timerfd()
{
	return timerfd_create(CLOCK_MONOTONIC, 0); // 使用CLOCK_MONOTONIC参数指定定时器的时钟类型为单调递增时钟。
}

// epoll 上添加上timerfd。EPOLLET:边缘触发
static inline int __poller_add_timerfd(int fd, poller_t *poller)
{
	struct epoll_event ev = {
		.events		=	EPOLLIN | EPOLLET,  // EPOLLET:边缘触发
		.data		=	{
			.ptr	=	NULL
		}
	};
	// epoll 上添加上timerfd
	return epoll_ctl(poller->pfd, EPOLL_CTL_ADD, fd, &ev);
}

/**
 * @brief 设置定时器文件描述符的定时器，使其在指定的绝对时间触发事件
 * @param  fd:定时器文件描述符   
 * */
static inline int __poller_set_timerfd(int fd, const struct timespec *abstime,
									   poller_t *poller)
{
	// 创建一个 itimerspec 结构体，它是 Linux 中定时器的一个数据结构。
	// .it_interval 字段设置为 { }，表示定时器的间隔，这里设置为空，即定时器不会重复。
	// .it_value 字段设置为 *abstime，这是定时器的绝对触发时间，即定时器将在这个时间触发。
	struct itimerspec timer = {
		.it_interval	=	{ },
		.it_value		=	*abstime // 指向struct timespec结构体的指针，表示定时器的绝对时间
	};

	// 调用 timerfd_settime 函数将定时器设置到文件描述符（fd）上。
	// timerfd_settime 是 Linux 中的一个系统调用，用于设置定时器的触发时间。
	// 参数 fd 是要设置的文件描述符。
	// 参数 TFD_TIMER_ABSTIME 表示设置的时间是绝对时间，如果不设置这个标志，那么时间会被视为相对于现在的时间。
	// 参数 &timer 是一个指向定时器设置的指针。
	// 参数 NULL 是旧的定时器设置，这里不关心旧的设置，所以设置为 NULL。
	// timerfd_settime 函数的返回值就是这个函数的返回值，如果设置成功，返回 0，否则返回 -1 并设置 errno。
	return timerfd_settime(fd, TFD_TIMER_ABSTIME, &timer, NULL);
}

// Linux epoll API 相关的结构定义，表示一个 epoll 事件。events 成员保存了该事件的类型，可以是 EPOLL_EVENTS 枚举中的一个或者多个值的按位或（bitwise OR）；data 成员保存了用户自定义的数据类型为 epoll_data_t。
// epoll_data_t：这是一个联合体，用于在 epoll 事件中携带用户自定义的数据。联合体的特性是，所有成员共享同一块内存，但是可以按照不同的数据类型进行访问。在这里，用户可以选择用 ptr 存储一个指针，用 fd 存储一个文件描述符，或者用 u32 或 u64 存储一个无符号整数。
typedef struct epoll_event __poller_event_t;

// 定义一个内联函数，__poller_wait，它接收三个参数：
// 1. events - 指向存储触发事件的数组的指针
// 2. maxevents - 触发的事件的最大数量
// 3. poller - 用户定义的poller对象，通常包含了一个epoll文件描述符
static inline int __poller_wait(__poller_event_t *events, int maxevents,
								poller_t *poller)
{
	// int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);
	// Specifying a timeout of -1 causes epoll_wait() to block indefinitely
	// 调用系统的epoll_wait函数等待事件发生
	// poller->pfd 是 epoll 的文件描述符
	// events 指向的数组用于接收发生的事件
	// maxevents 指定了事件数组的最大长度
	// -1 使得 epoll_wait 函数无限等待，直到有事件发生为止
	// 函数返回发生的事件数量
	return epoll_wait(poller->pfd, events, maxevents, -1);
}

// 这是一个内联函数，用于从给定的 epoll 事件中提取用户自定义数据。
// 参数 event 是一个指向 __poller_event_t 类型的指针，表示一个 epoll 事件。
// 这个函数返回 event 中的 data.ptr 值，即用户自定义数据。
static inline void *__poller_event_data(const __poller_event_t *event)
{
	return event->data.ptr;
}

#else /* BSD, macOS */

static inline int __poller_create_pfd()
{
	return kqueue();
}

static inline int __poller_add_fd(int fd, int event, void *data,
								  poller_t *poller)
{
	struct kevent ev;
	EV_SET(&ev, fd, event, EV_ADD, 0, 0, data);
	return kevent(poller->pfd, &ev, 1, NULL, 0, NULL);
}

static inline int __poller_del_fd(int fd, int event, poller_t *poller)
{
	struct kevent ev;
	EV_SET(&ev, fd, event, EV_DELETE, 0, 0, NULL);
	return kevent(poller->pfd, &ev, 1, NULL, 0, NULL);
}

static inline int __poller_mod_fd(int fd, int old_event,
								  int new_event, void *data,
								  poller_t *poller)
{
	struct kevent ev[2];
	EV_SET(&ev[0], fd, old_event, EV_DELETE, 0, 0, NULL);
	EV_SET(&ev[1], fd, new_event, EV_ADD, 0, 0, data);
	return kevent(poller->pfd, ev, 2, NULL, 0, NULL);
}

static inline int __poller_create_timerfd()
{
	return dup(0);
}

static inline int __poller_add_timerfd(int fd, poller_t *poller)
{
	return 0;
}

static int __poller_set_timerfd(int fd, const struct timespec *abstime,
								poller_t *poller)
{
	struct timespec curtime;
	long long nseconds;
	struct kevent ev;
	int flags;

	if (abstime->tv_sec || abstime->tv_nsec)
	{
		clock_gettime(CLOCK_MONOTONIC, &curtime);
		nseconds = 1000000000LL * (abstime->tv_sec - curtime.tv_sec);
		nseconds += abstime->tv_nsec - curtime.tv_nsec;
		flags = EV_ADD;
	}
	else
	{
		nseconds = 0;
		flags = EV_DELETE;
	}

	EV_SET(&ev, fd, EVFILT_TIMER, flags, NOTE_NSECONDS, nseconds, NULL);
	return kevent(poller->pfd, &ev, 1, NULL, 0, NULL);
}

typedef struct kevent __poller_event_t;

static inline int __poller_wait(__poller_event_t *events, int maxevents,
								poller_t *poller)
{
	return kevent(poller->pfd, NULL, 0, events, maxevents, NULL);
}

static inline void *__poller_event_data(const __poller_event_t *event)
{
	return event->udata;
}

#define EPOLLIN		EVFILT_READ
#define EPOLLOUT	EVFILT_WRITE
#define EPOLLET		0

#endif

static inline long __timeout_cmp(const struct __poller_node *node1,
								 const struct __poller_node *node2)
{
	long ret = node1->timeout.tv_sec - node2->timeout.tv_sec;

	if (ret == 0)
		ret = node1->timeout.tv_nsec - node2->timeout.tv_nsec;

	return ret;
}

// 定义一个静态函数 __poller_tree_insert
static void __poller_tree_insert(struct __poller_node *node, poller_t *poller)
{
    // 定义一系列的变量和指针
	struct rb_node **p = &poller->timeo_tree.rb_node;  // 用于遍历红黑树的节点
	struct rb_node *parent = NULL;  // 记录父节点
	struct __poller_node *entry;  // 用于记录当前遍历到的 __poller_node 节点
	int first = 1;  // 标记新插入的节点是否是红黑树中最左侧的节点（即是否最早超时）

    // 遍历红黑树，寻找插入位置
	while (*p)
	{
		parent = *p;
		entry = rb_entry(*p, struct __poller_node, rb);
		if (__timeout_cmp(node, entry) < 0)  // 如果新节点的超时时间小于当前节点，继续遍历左子树
			p = &(*p)->rb_left;
		else  // 否则，继续遍历右子树，并更新 first 标记
		{
			p = &(*p)->rb_right;
			first = 0;
		}
	}

    // 如果新节点是最早超时的节点，更新 poller->tree_first 指针
	if (first)
		poller->tree_first = &node->rb;

    // 标记新节点已经被插入到红黑树中
	node->in_rbtree = 1;
    
    // 连接新节点和父节点，插入到红黑树中
	rb_link_node(&node->rb, parent, p);
    
    // 调整红黑树，使之平衡
	rb_insert_color(&node->rb, &poller->timeo_tree);
}


static inline void __poller_tree_erase(struct __poller_node *node,
									   poller_t *poller)
{
	if (&node->rb == poller->tree_first)
		poller->tree_first = rb_next(&node->rb);

	rb_erase(&node->rb, &poller->timeo_tree);
	node->in_rbtree = 0;
}

static int __poller_remove_node(struct __poller_node *node, poller_t *poller)
{
	int removed;

	pthread_mutex_lock(&poller->mutex);
	removed = node->removed;
	if (!removed)
	{
		poller->nodes[node->data.fd] = NULL;

		if (node->in_rbtree)
			__poller_tree_erase(node, poller);
		else
			list_del(&node->list);

		__poller_del_fd(node->data.fd, node->event, poller);
	}

	pthread_mutex_unlock(&poller->mutex);
	return removed;
}

// 尝试将缓存区数据追加到该节点的消息中。如果该节点没有消息将会创建消息
static int __poller_append_message(const void *buf, size_t *n,
								   struct __poller_node *node,
								   poller_t *poller)
{
	// 获取当前节点的消息
	poller_message_t *msg = node->data.message;
	struct __poller_node *res; //res 将被转化为 struct poller_result 指针，并被传递给 poller->cb 回调函数
	int ret;

	// 如果该节点没有消息
	if (!msg)
	{	
		// 申请一个新的 __poller_node 结构体空间
		res = (struct __poller_node *)malloc(sizeof (struct __poller_node));
		if (!res)
			return -1;

		// 网络请求读取后需要创建会话任务。调用 int Communicator::create_service_session(struct CommConnEntry *entry) 函数，尝试创建一个CommSession或其派生类对象
		msg = poller->create_message(node->data.context); // 网络请求读取后需要创建会话任务。新版本将调用 Communicator::create_request， 见新版本 void Communicator::handle_listen_result(struct poller_result *res); 
		if (!msg)
		{
			free(res);
			return -1;
		}

		// 设置当前节点的消息为新创建的消息
		node->data.message = msg;
		// 设置节点的 res 指向刚刚申请的空间
		node->res = res;
	}
	// 如果该节点已有消息，则获取其对应的 res
	else
		res = node->res;

	// 尝试将 buf 中的数据追加到 msg 中，追加成功的数据长度会写入 n
	ret = msg->append(buf, n, msg); // Communicator::append 见poller_message_t *Communicator::create_message(void *context)
	// 如果 append 函数返回值大于 0，表示数据追加成功
	if (ret > 0)
	{	
		// 将 node 的数据复制到 res 中
		res->data = node->data;
		res->error = 0;
		res->state = PR_ST_SUCCESS;
		// 完成数据追加任务，调用 poller 的回调函数处理任务结果res
		poller->cb((struct poller_result *)res, poller->ctx); // 可将struct __poller_node 截断为 struct poller_result

		// 清空 node 的 message 和 res
		node->data.message = NULL;
		node->res = NULL;
	}

	// 返回 append 函数的结果
	return ret;
}

static int __poller_handle_ssl_error(struct __poller_node *node, int ret,
									 poller_t *poller)
{
	int error = SSL_get_error(node->data.ssl, ret);
	int event;

	switch (error)
	{
	case SSL_ERROR_WANT_READ:
		event = EPOLLIN | EPOLLET;
		break;
	case SSL_ERROR_WANT_WRITE:
		event = EPOLLOUT | EPOLLET;
		break;
	default:
		errno = -error;
	case SSL_ERROR_SYSCALL:
		return -1;
	}

	if (event == node->event)
		return 0;

	pthread_mutex_lock(&poller->mutex);
	if (!node->removed)
	{
		ret = __poller_mod_fd(node->data.fd, node->event, event, node, poller);
		if (ret >= 0)
			node->event = event;
	}
	else
		ret = 0;

	pthread_mutex_unlock(&poller->mutex);
	return ret;
}

static void __poller_handle_read(struct __poller_node *node,
								 poller_t *poller)
{
	ssize_t nleft;
	size_t n;
	char *p; // 指向缓冲区首地址，依次作为读缓冲区;

	while (1)
	{
		// char buf[POLLER_BUFSIZE];
		p = poller->buf;
		if (node->data.ssl)
		{
			nleft = SSL_read(node->data.ssl, p, POLLER_BUFSIZE);
			if (nleft < 0)
			{
				if (__poller_handle_ssl_error(node, nleft, poller) >= 0)
					return;
			}
		}
		else
		{
			// 读到poller->buf 中
			nleft = read(node->data.fd, p, POLLER_BUFSIZE);
			if (nleft < 0)
			{
				if (errno == EAGAIN)
					return;
			}
		}

		if (nleft <= 0)
			break;

		do
		{
			n = nleft;
			if (__poller_append_message(p, &n, node, poller) >= 0)
			{
				nleft -= n;
				p += n;
			}
			else
				nleft = -1;
		} while (nleft > 0); // 接受完整消息才跳出循环

		if (nleft < 0)
			break;
	}

	if (__poller_remove_node(node, poller))
		return;

	if (nleft == 0)
	{
		node->error = 0;
		node->state = PR_ST_FINISHED;
	}
	else
	{
		node->error = errno;
		node->state = PR_ST_ERROR;
	}

	free(node->res);
	poller->cb((struct poller_result *)node, poller->ctx);
}

#ifndef IOV_MAX
# ifdef UIO_MAXIOV
#  define IOV_MAX	UIO_MAXIOV
# else
#  define IOV_MAX	1024
# endif
#endif

static void __poller_handle_write(struct __poller_node *node,
								  poller_t *poller)
{
	struct iovec *iov = node->data.write_iov;
	size_t count = 0;
	ssize_t nleft;
	int iovcnt;
	int ret;

	while (node->data.iovcnt > 0 && iov->iov_len == 0)
	{
		iov++;
		node->data.iovcnt--;
	}

	while (node->data.iovcnt > 0)
	{
		if (node->data.ssl)
		{
			nleft = SSL_write(node->data.ssl, iov->iov_base, iov->iov_len);
			if (nleft <= 0)
			{
				ret = __poller_handle_ssl_error(node, nleft, poller);
				break;
			}
		}
		else
		{
			iovcnt = node->data.iovcnt;
			if (iovcnt > IOV_MAX)
				iovcnt = IOV_MAX;

			nleft = writev(node->data.fd, iov, iovcnt);
			if (nleft < 0)
			{
				ret = errno == EAGAIN ? 0 : -1;
				break;
			}
		}

		count += nleft;
		do
		{
			if (nleft >= iov->iov_len)
			{
				nleft -= iov->iov_len;
				iov->iov_base = (char *)iov->iov_base + iov->iov_len;
				iov->iov_len = 0;
				iov++;
				node->data.iovcnt--;
			}
			else
			{
				iov->iov_base = (char *)iov->iov_base + nleft;
				iov->iov_len -= nleft;
				break;
			}
		} while (node->data.iovcnt > 0);
	}

	node->data.write_iov = iov;
	if (node->data.iovcnt > 0 && ret >= 0)
	{
		if (count == 0)
			return;

		if (poller->partial_written(count, node->data.context) >= 0)
			return;
	}

	if (__poller_remove_node(node, poller))
		return;

	if (node->data.iovcnt == 0)
	{
		node->error = 0;
		node->state = PR_ST_FINISHED;
	}
	else
	{
		node->error = errno;
		node->state = PR_ST_ERROR;
	}

	poller->cb((struct poller_result *)node, poller->ctx);
}

// accept监听结果，并且传递结果 
static void __poller_handle_listen(struct __poller_node *node,
								   poller_t *poller)
{
	struct __poller_node *res = node->res;
	struct sockaddr_storage ss;
	socklen_t len;
	int sockfd;
	void *p;

	while (1)
	{
		len = sizeof (struct sockaddr_storage);
		// 1. 这里调用了accept建立连接
		sockfd = accept(node->data.fd, (struct sockaddr *)&ss, &len);
		if (sockfd < 0)
		{
			if (errno == EAGAIN)
				return;
			else
				break;
		}
        // data.accept = Communicator::accept;
        // 2. 调用Communicator::accept，初始化服务器通讯目标。 见int Communicator::bind(CommService *service)
		p = node->data.accept((const struct sockaddr *)&ss, len,
							  sockfd, node->data.context);
		if (!p)
			break;

		res->data = node->data;
		res->data.result = p; //将服务器通讯目标作为poller_result的处理结果进行存储 见void Communicator::handle_listen_result(struct poller_result *res)
		res->error = 0;
		res->state = PR_ST_SUCCESS;
		
        // .callback			=	Communicator::callback,
        /*
            void Communicator::callback(struct poller_result *res, void *context)
            {
                Communicator *comm = (Communicator *)context;
                msgqueue_put(res, comm->queue);
            }
        */
        // 放回结果到msgqueue中
		poller->cb((struct poller_result *)res, poller->ctx);

		res = (struct __poller_node *)malloc(sizeof (struct __poller_node));
		node->res = res;
		if (!res)
			break;
	}

	if (__poller_remove_node(node, poller))
		return;

	node->error = errno;
	node->state = PR_ST_ERROR;
	free(node->res);
	poller->cb((struct poller_result *)node, poller->ctx);
}

static void __poller_handle_connect(struct __poller_node *node,
									poller_t *poller)
{
	socklen_t len = sizeof (int);
	int error;

	// SO_ERROR
	// Reports information about error status and clears it. This option stores an int value.
	// https://stackoverflow.com/questions/21031717/so-error-vs-errno
	// You set up a non-blocking socket and do a connect() that returns -1/EINPROGRESS or -1/EWOULDBLOCK. 
	// If the connection failed, the reason is hidden away inside something called so_error in the socket. Modern systems let you see so_error with getsockopt(,,SO_ERROR,,) ...
	// So, if you're performing asynchronous operations on sockets, you may need to use SO_ERROR. In any other case, just use errno.
	if (getsockopt(node->data.fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0)
		error = errno;

	if (__poller_remove_node(node, poller))
		return;

	if (error == 0)
	{
		node->error = 0;
		node->state = PR_ST_FINISHED;
	}
	else
	{
		node->error = error;
		node->state = PR_ST_ERROR;
	}

	poller->cb((struct poller_result *)node, poller->ctx);
}

static void __poller_handle_ssl_accept(struct __poller_node *node,
									   poller_t *poller)
{
	int ret = SSL_accept(node->data.ssl);

	if (ret <= 0)
	{
		if (__poller_handle_ssl_error(node, ret, poller) >= 0)
			return;
	}

	if (__poller_remove_node(node, poller))
		return;

	if (ret > 0)
	{
		node->error = 0;
		node->state = PR_ST_FINISHED;
	}
	else
	{
		node->error = errno;
		node->state = PR_ST_ERROR;
	}

	poller->cb((struct poller_result *)node, poller->ctx);
}

static void __poller_handle_ssl_connect(struct __poller_node *node,
										poller_t *poller)
{
	int ret = SSL_connect(node->data.ssl);

	if (ret <= 0)
	{
		if (__poller_handle_ssl_error(node, ret, poller) >= 0)
			return;
	}

	if (__poller_remove_node(node, poller))
		return;

	if (ret > 0)
	{
		node->error = 0;
		node->state = PR_ST_FINISHED;
	}
	else
	{
		node->error = errno;
		node->state = PR_ST_ERROR;
	}

	poller->cb((struct poller_result *)node, poller->ctx);
}

static void __poller_handle_ssl_shutdown(struct __poller_node *node,
										 poller_t *poller)
{
	int ret = SSL_shutdown(node->data.ssl);

	if (ret <= 0)
	{
		if (__poller_handle_ssl_error(node, ret, poller) >= 0)
			return;
	}

	if (__poller_remove_node(node, poller))
		return;

	if (ret > 0)
	{
		node->error = 0;
		node->state = PR_ST_FINISHED;
	}
	else
	{
		node->error = errno;
		node->state = PR_ST_ERROR;
	}

	poller->cb((struct poller_result *)node, poller->ctx);
}

static void __poller_handle_event(struct __poller_node *node,
								  poller_t *poller)
{
	struct __poller_node *res = node->res;
	unsigned long long cnt = 0;
	unsigned long long value;
	ssize_t ret;
	void *p;

	while (1)
	{
		ret = read(node->data.fd, &value, sizeof (unsigned long long));
		if (ret == sizeof (unsigned long long))
			cnt += value;
		else
		{
			if (ret >= 0)
				errno = EINVAL;
			break;
		}
	}

	if (errno == EAGAIN)
	{
		while (1)
		{
			if (cnt == 0)
				return;

			cnt--;
			p = node->data.event(node->data.context);
			if (!p)
				break;

			res->data = node->data;
			res->data.result = p;
			res->error = 0;
			res->state = PR_ST_SUCCESS;
			poller->cb((struct poller_result *)res, poller->ctx);

			res = (struct __poller_node *)malloc(sizeof (struct __poller_node));
			node->res = res;
			if (!res)
				break;
		}
	}

	if (cnt != 0)
		write(node->data.fd, &cnt, sizeof (unsigned long long));

	if (__poller_remove_node(node, poller))
		return;

	node->error = errno;
	node->state = PR_ST_ERROR;
	free(node->res);
	poller->cb((struct poller_result *)node, poller->ctx);
}

static void __poller_handle_notify(struct __poller_node *node,
								   poller_t *poller)
{
	struct __poller_node *res = node->res;
	ssize_t ret;
	void *p;

	while (1)
	{
		ret = read(node->data.fd, &p, sizeof (void *));
		if (ret == sizeof (void *))
		{
			p = node->data.notify(p, node->data.context);
			if (!p)
				break;

			res->data = node->data;
			res->data.result = p;
			res->error = 0;
			res->state = PR_ST_SUCCESS;
			poller->cb((struct poller_result *)res, poller->ctx);

			res = (struct __poller_node *)malloc(sizeof (struct __poller_node));
			node->res = res;
			if (!res)
				break;
		}
		else if (ret < 0 && errno == EAGAIN)
			return;
		else
		{
			if (ret > 0)
				errno = EINVAL;
			break;
		}
	}

	if (__poller_remove_node(node, poller))
		return;

	if (ret == 0)
	{
		node->error = 0;
		node->state = PR_ST_FINISHED;
	}
	else
	{
		node->error = errno;
		node->state = PR_ST_ERROR;
	}

	free(node->res);
	poller->cb((struct poller_result *)node, poller->ctx);
}

static int __poller_handle_pipe(poller_t *poller)
{
	struct __poller_node **node = (struct __poller_node **)poller->buf;
	int stop = 0;
	int n;
	int i;

	n = read(poller->pipe_rd, node, POLLER_BUFSIZE) / sizeof (void *);
	for (i = 0; i < n; i++)
	{
		if (node[i])
		{
			free(node[i]->res);
			poller->cb((struct poller_result *)node[i], poller->ctx);
		}
		else
			stop = 1;
	}

	return stop;
}

static void __poller_handle_timeout(const struct __poller_node *time_node,
                                    poller_t *poller)
{
    struct __poller_node *node;  // 用于在循环中引用当前节点的指针
    struct list_head *pos, *tmp;  // 循环迭代器
    LIST_HEAD(timeo_list);  // 新建一个超时列表

    pthread_mutex_lock(&poller->mutex);  // 加锁，保证线程安全
    // 遍历超时链表
    list_for_each_safe(pos, tmp, &poller->timeo_list)
    {
        node = list_entry(pos, struct __poller_node, list);  // 获取当前节点
        // 如果当前节点的超时时间早于或等于给定的时间节点
        if (__timeout_cmp(node, time_node) <= 0)
        {
            // 如果文件描述符有效
            if (node->data.fd >= 0)
            {
                poller->nodes[node->data.fd] = NULL;  // 从 poller 的节点数组中删除该节点
                __poller_del_fd(node->data.fd, node->event, poller);  // 从 poller 的事件监听中删除该节点
            }

            list_move_tail(pos, &timeo_list);  // 将节点移动到超时列表的尾部
        }
        else
            break;  // 当前节点的超时时间晚于给定的时间节点，退出循环
    }

    // 处理红黑树中的超时节点
    while (poller->tree_first)
    {
        node = rb_entry(poller->tree_first, struct __poller_node, rb);  // 获取红黑树中的首个节点
        // 如果当前节点的超时时间早于给定的时间节点
        if (__timeout_cmp(node, time_node) < 0)
        {
            // 如果文件描述符有效
            if (node->data.fd >= 0)
            {
                poller->nodes[node->data.fd] = NULL;  // 从 poller 的节点数组中删除该节点
                __poller_del_fd(node->data.fd, node->event, poller);  // 从 poller 的事件监听中删除该节点
            }

            poller->tree_first = rb_next(poller->tree_first);  // 更新红黑树首个节点
            rb_erase(&node->rb, &poller->timeo_tree);  // 从红黑树中删除当前节点
            list_add_tail(&node->list, &timeo_list);  // 将节点添加到超时列表的尾部
        }
        else
            break;  // 当前节点的超时时间晚于给定的时间节点，退出循环
    }

    pthread_mutex_unlock(&poller->mutex);  // 解锁

    // 处理超时列表中的节点
    while (!list_empty(&timeo_list))
    {
        node = list_entry(timeo_list.next, struct __poller_node, list);  // 获取超时列表中的首个节点
        list_del(&node->list);  // 从超时列表中删除节点

        node->error = ETIMEDOUT;  // 设置错误码为 ETIMEDOUT（操作超时）
        node->state = PR_ST_ERROR;  // 设置节点状态为错误
        free(node->res);  // 释放节点的资源
        poller->cb((struct poller_result *)node, poller->ctx);  // 调用回调函数处理超时节点
    }
}

// 设置定时器
static void __poller_set_timer(poller_t *poller)
{
	struct __poller_node *node = NULL;
	struct __poller_node *first;
	struct timespec abstime;

	pthread_mutex_lock(&poller->mutex);
	if (!list_empty(&poller->timeo_list))
		node = list_entry(poller->timeo_list.next, struct __poller_node, list);

	if (poller->tree_first)
	{
		first = rb_entry(poller->tree_first, struct __poller_node, rb);
		if (!node || __timeout_cmp(first, node) < 0)
			node = first;
	}

	if (node)
		abstime = node->timeout;
	else
	{
		abstime.tv_sec = 0;
		abstime.tv_nsec = 0;
	}

	__poller_set_timerfd(poller->timerfd, &abstime, poller);
	pthread_mutex_unlock(&poller->mutex);
}

// poller线程的运行函数  
// 在一个持续运行的线程中，监控一个多路复用器（如select、poll、epoll等）的事件，并根据事件类型调用相应的处理函数和注册__poller_node。
// 同时，它也处理管道事件和超时事件
static void *__poller_thread_routine(void *arg)
{
	// 这里传入的参数就是poller
	/*
		struct __poller
		{
			size_t max_open_files;
			poller_message_t *(*create_message)(void *);
			int (*partial_written)(size_t, void *);
			void (*cb)(struct poller_result *, void *);
			void *ctx;

			pthread_t tid;
			int pfd;
			int timerfd;
			int pipe_rd;
			int pipe_wr;
			int stopped;
			struct rb_root timeo_tree;
			struct rb_node *tree_first;
			struct list_head timeo_list;
			struct list_head no_timeo_list;
			struct __poller_node **nodes;
			pthread_mutex_t mutex;
			char buf[POLLER_BUFSIZE];
		};
	*/
	poller_t *poller = (poller_t *)arg;

	// typedef struct epoll_event __poller_event_t;
	// 这里就是一个epoll_event 数组
	__poller_event_t events[POLLER_EVENTS_MAX]; // // 声明一个events数组，大小为POLLER_EVENTS_MAX，用来存放多路复用器的返回结果
	/*
		struct __poller_node
		{
			int state;    
			int error;
			struct poller_data data;
		#pragma pack(1)
			union
			{
				struct list_head list;
				struct rb_node rb;
			};
		#pragma pack()
			char in_rbtree;
			char removed;
			int event;
			struct timespec timeout;
			struct __poller_node *res;
		};
	*/
	struct __poller_node time_node; // 声明一个时间节点
	struct __poller_node *node;
	int has_pipe_event;
	int nevents;
	int i;

	while (1)
	{
		// 设置定时器
		__poller_set_timer(poller); 
		//  __poller_wait 调用在linux下是通过 epoll_wait 来检测 fd 发生的事件。
		// nevents 是返回的事件个数，fd及其事件信息被保存在 events 这个 epoll_event 结构体数组中
		nevents = __poller_wait(events, POLLER_EVENTS_MAX, poller);
		clock_gettime(CLOCK_MONOTONIC, &time_node.timeout); // 获取当前时间，存入time_node的timeout成员
		has_pipe_event = 0;
		for (i = 0; i < nevents; i++)
		{
			node = (struct __poller_node *)__poller_event_data(&events[i]);
			// (struct __poller_node *)1 的含义 :
			// 在event的data里，用0和1分别代表定时器和管道事件。
			// 因为data是一个指针，所以直接就把数值1强转成指针
			// 1不可能是一个合法地址，所以看到node==1就知道是一个pipe事件
			// pipe用来通知各种fd的删除和poller的停止
			// 这里判断if (node>1)，是因为大多数情况下，都是正常的网络事件，于是只需判断一次，就能进入处理逻辑了
			if (node > (struct __poller_node *)1)  
			{
				// 一般我们用epoll_event.events 是操作系统告诉本程序该fd当前发生的事件类型。
				// 但是下面判断 fd 发生的事件类型是通过 node->data.operation 来判断的。
				// 为了性能考虑我们的poller_node是一个以fd为下标的数组，
				// 而每个node只能关注一种事件，READ或WRITE。
				// 所有我们需要通过operation来判断调用哪个处理函数，而不能通过event来判断

				// 为了性能考虑我们的poller_node是一个以fd为下标的数组，而每个node只能关注一种事件，READ或WRITE。
				// 所有我们需要通过operation来判断调用哪个处理函数，而不能通过event来判断。
				switch (node->data.operation)
				{
				case PD_OP_READ:
					__poller_handle_read(node, poller);
					break;
				case PD_OP_WRITE:
					// 测试中发现，向对等方发送消息均是通过Communicator::send_message_sync中的writev(entry->sockfd, vectors, cnt <= IOV_MAX ? cnt : IOV_MAX)来实现，
					// 那此处中的__poller_handle_write的功能是？
					// A : 消息一次发得出去，就不走异步写了。这样子快。试个大一点的消息，就会进poller了。
					__poller_handle_write(node, poller);
					break;
				case PD_OP_LISTEN:
					__poller_handle_listen(node, poller);
					break;
				case PD_OP_CONNECT:
					__poller_handle_connect(node, poller);
					break;
				case PD_OP_SSL_ACCEPT:
					__poller_handle_ssl_accept(node, poller);
					break;
				case PD_OP_SSL_CONNECT:
					__poller_handle_ssl_connect(node, poller);
					break;
				case PD_OP_SSL_SHUTDOWN:
					__poller_handle_ssl_shutdown(node, poller);
					break;
				case PD_OP_EVENT:
					// event其实是linux的eventfd。
					__poller_handle_event(node, poller);
					break;
				case PD_OP_NOTIFY:
					// notify忽略，只有mac下的异步文件IO才使用到的。
					__poller_handle_notify(node, poller);
					break;
				}
			}
			else if (node == (struct __poller_node *)1)  // 1 为 pipe事件
				has_pipe_event = 1;
		}

        // 如果有管道事件，调用管道事件的处理函数
        // 如果处理函数返回非零值，退出循环
        if (has_pipe_event)
        {
            if (__poller_handle_pipe(poller))
                break;
        }

        // 处理超时事件
		__poller_handle_timeout(&time_node, poller);
	}

    // 在退出线程前，根据OpenSSL版本清理线程状态
#if OPENSSL_VERSION_NUMBER < 0x10100000L
# ifdef CRYPTO_LOCK_ECDH
    ERR_remove_thread_state(NULL);
# else
    ERR_remove_state(0);
# endif
#endif
	return NULL;
}

// __poller_open_pipe函数的目的是创建一个管道，并将管道的读端添加到epoll事件监听列表中。
// 参数poller是封装了epoll的结构体。
// 函数首先调用pipe函数创建一个管道，
// 然后调用__poller_add_fd函数将管道的读端添加到epoll事件监听列表中。
// 如果以上操作都成功，函数将管道的读端和写端的文件描述符保存到poller结构体中，并返回0表示操作成功；
// 如果任何操作失败，函数将关闭已经打开的文件描述符，并返回-1表示操作失败
static int __poller_open_pipe(poller_t *poller)
{
	int pipefd[2];

	if (pipe(pipefd) >= 0)
	{
		if (__poller_add_fd(pipefd[0], EPOLLIN, (void *)1, poller) >= 0)
		{
			poller->pipe_rd = pipefd[0];
			poller->pipe_wr = pipefd[1];
			return 0;
		}

		close(pipefd[0]);
		close(pipefd[1]);
	}

	return -1;
}

/**
 * @brief epoll上添加poller中的timerfd， 定时器的时钟类型为单调递增时钟，EPOLLET:边缘触发
 * 
 * @param poller 
 * @return int 0 成功， -1 失败
 */
static int __poller_create_timer(poller_t *poller)
{
	int timerfd = __poller_create_timerfd();  // !!! 到底了， timerfd_create

	if (timerfd >= 0) 
	{
		if (__poller_add_timerfd(timerfd, poller) >= 0)
		{
			poller->timerfd = timerfd;
			return 0;
		}
		// __poller_add_timerfd失败
		close(timerfd);
	}

	return -1;
}

/**
 * @brief 产生一个poller对象。需要传一个params参数，包含的域在poller_t中
 * 
 * 
 * @param params 
 * @return poller_t* 
 */
poller_t *poller_create(const struct poller_params *params)
{
	poller_t *poller = (poller_t *)malloc(sizeof (poller_t));
	size_t n; // poller 可以打开的最大文件数量
	int ret;

	if (!poller)  // malloc 失败
		return NULL;

	n = params->max_open_files;
	if (n == 0)
		n = POLLER_NODES_MAX;  // 如果取的params默认为0， 则设置为max nodes(65536)

    // calloc() gives you a zero-initialized buffer, while malloc() leaves the memory uninitialized.
    // https://stackoverflow.com/questions/1538420/difference-between-malloc-and-calloc
	poller->nodes = (struct __poller_node **)calloc(n, sizeof (void *));  // 分配max_open_files 这么多的__poller_node
	if (poller->nodes)
	{
		poller->pfd = __poller_create_pfd();   // !!!这里终于到底了了，去调用了epoll_create(1);
		if (poller->pfd >= 0)  
		{
			if (__poller_create_timer(poller) >= 0)   // 设置poller中的timerfd，并且epoll上添加poller中的timerfd
			{
				// The pthread_mutex_init() function shall initialize the mutex referenced by mutex with attributes specified by attr.
				// https://linux.die.net/man/3/pthread_mutex_init
				ret = pthread_mutex_init(&poller->mutex, NULL);
				if (ret == 0)  // pthread_mutex_init successfully
				{
					// 通过params设置poller的各个参数
					poller->max_open_files = n;
					poller->create_message = params->create_message;
					poller->partial_written = params->partial_written;
					// .callback			=	Communicator::callback,
					poller->cb = params->callback;
					poller->ctx = params->context;

					poller->timeo_tree.rb_node = NULL;
					poller->tree_first = NULL;
					INIT_LIST_HEAD(&poller->timeo_list);
					INIT_LIST_HEAD(&poller->no_timeo_list);
					poller->nodes[poller->timerfd] = POLLER_NODE_ERROR;
					poller->nodes[poller->pfd] = POLLER_NODE_ERROR;
					poller->stopped = 1;  
					return poller;
				}

				errno = ret;
				close(poller->timerfd);  // __poller_create_timer 失败
			}

			close(poller->pfd);
		}
		// epoll_create 失败
		free(poller->nodes);
	}
	free(poller); // if poller->nodes calloc 失败
	return NULL;
}

/**
 * @brief 销毁停止状态的poller。运行中的poller直接destroy的话，行为无定义。
 * 
 * @param poller 
 */
void poller_destroy(poller_t *poller)
{
	pthread_mutex_destroy(&poller->mutex);
	close(poller->timerfd);
	close(poller->pfd);
	free(poller->nodes);
	free(poller);
}

/**
 * @brief 主要作用就是开启__poller_thread_routine线程
 * 启动poller。poller被创建后，是处于停止状态。调用poller_start可以让poller开始运行。
 * 
 * @param poller 
 * @return int 
 */
int poller_start(poller_t *poller)
{
	pthread_t tid; 
	int ret;

	pthread_mutex_lock(&poller->mutex);

	// 如果能成功打开pipe（通常用于线程间通信），则继续执行
	if (__poller_open_pipe(poller) >= 0)
	{
		ret = pthread_create(&tid, NULL, __poller_thread_routine, poller);
		if (ret == 0)
		{
			poller->tid = tid;

			/*
			POLLER_NODE_ERROR被用于标记pipe的读端和写端在轮询器的nodes数组中的位置。
			数组nodes主要用于存储并追踪正在轮询的文件描述符及其相关的信息（封装在__poller_node结构体中）。
			但是，在这个情况下，pipe的读端和写端并没有与之关联的__poller_node结构体，所以这里用POLLER_NODE_ERROR标记，表明这两个位置是特殊的。
			当后续在处理epoll事件时，如果发现事件对应的文件描述符在nodes数组中的位置被标记为POLLER_NODE_ERROR，就知道这是一个特殊的情况
			（这里是pipe的读端或写端），需要单独处理。这样设计的目的是将所有的文件描述符（包括常规的套接字描述符和特殊的pipe描述符）都统一管理和处理，使得代码结构更清晰，逻辑更简洁。
			*/
			poller->nodes[poller->pipe_rd] = POLLER_NODE_ERROR;   // 将pipe的读端设置为错误状态
			poller->nodes[poller->pipe_wr] = POLLER_NODE_ERROR;	  // 将pipe的写端设置为错误状态
			poller->stopped = 0; // 设置poller的状态为非停止
		}
		else
		{
			errno = ret;
			close(poller->pipe_wr);
			close(poller->pipe_rd);
		}
	}

	pthread_mutex_unlock(&poller->mutex);
	return -poller->stopped;
}

// 函数__poller_insert_node，用于将一个新的poller节点插入到超时列表或红黑树中
static void __poller_insert_node(struct __poller_node *node,
								 poller_t *poller)
{
	LOG_TRACE("__poller_insert_node"); // 日志函数，记录函数调用信息
	struct __poller_node *end; // 用于存放超时列表的尾节点

	// 获取超时列表的尾节点
	end = list_entry(poller->timeo_list.prev, struct __poller_node, list);
	
	// 如果超时列表为空，或者新节点的超时时间不小于尾节点的超时时间，则将新节点添加到超时列表的尾部
	if (list_empty(&poller->timeo_list) || __timeout_cmp(node, end) >= 0)
		list_add_tail(&node->list, &poller->timeo_list);
	else // 否则，将新节点插入到红黑树中
		__poller_tree_insert(node, poller);

	// 如果新节点是超时列表的头节点
	if (&node->list == poller->timeo_list.next)
	{
		if (poller->tree_first) // 如果红黑树不为空，则获取红黑树的首节点
			end = rb_entry(poller->tree_first, struct __poller_node, rb);
		else // 否则，将尾节点设为NULL
			end = NULL;
	}
	else if (&node->rb == poller->tree_first) // 如果新节点是红黑树的首节点，则获取超时列表的头节点
		end = list_entry(poller->timeo_list.next, struct __poller_node, list);
	else // 如果新节点既不是超时列表的头节点，也不是红黑树的首节点，则直接返回
		return;

	// 如果尾节点为NULL，或者新节点的超时时间小于尾节点的超时时间，则设置timerfd的超时时间为新节点的超时时间
	if (!end || __timeout_cmp(node, end) < 0)
		__poller_set_timerfd(poller->timerfd, &node->timeout, poller);
}

/*
设置 poller node 的超时时间
它先获取当前的时间，然后在当前时间的基础上添加超时时间，这样就得到了超时的绝对时间。
如果超时时间的纳秒部分超过了10^9，就把这部分时间转化为秒。
*/
static void __poller_node_set_timeout(int timeout, struct __poller_node *node)
{
    // 使用 clock_gettime 函数获取当前的时间（以纳秒为单位），存储到 node 的 timeout 字段中
    // CLOCK_MONOTONIC 表示一个不受系统时间改变影响的单调递增的时钟
    clock_gettime(CLOCK_MONOTONIC, &node->timeout);

    // 将传入的 timeout（毫秒）转化为秒和纳秒，分别加到 node->timeout 的 tv_sec 和 tv_nsec 字段中
    // tv_sec 是秒数，tv_nsec 是纳秒数（1秒 = 10^9纳秒）
    node->timeout.tv_sec += timeout / 1000;  // 转换毫秒到秒
    node->timeout.tv_nsec += timeout % 1000 * 1000000;  // 转换剩余的毫秒到纳秒

    // 如果纳秒数大于等于10^9，即1秒
    if (node->timeout.tv_nsec >= 1000000000)
    {
        // 减去10^9纳秒，即1秒，并且秒数加1，这样就把纳秒数归一化到了0到10^9之间
        node->timeout.tv_nsec -= 1000000000;
        node->timeout.tv_sec++;
    }
}

// 根据提供的操作类型获取对应的epoll事件类型。只有事件、监听、通知才会返回1：用于存放需要回调函数处理的结果节点
static int __poller_data_get_event(int *event, const struct poller_data *data)
{
	// 根据操作类型进行判断
	switch (data->operation)
	{
	case PD_OP_READ:
		*event = EPOLLIN | EPOLLET;  // 对于读操作，设置为边缘触发的输入事件
		return !!data->message;  // 如果有消息，则返回1，否则返回0
	case PD_OP_WRITE:
		*event = EPOLLOUT | EPOLLET;  // 对于写操作，设置为边缘触发的输出事件
		return 0;
	case PD_OP_LISTEN:
		*event = EPOLLIN | EPOLLET;  // 对于监听操作，设置为边缘触发的输入事件
		return 1;
	case PD_OP_CONNECT:
		*event = EPOLLOUT | EPOLLET;  // 对于连接操作，设置为边缘触发的输出事件
		return 0;
	case PD_OP_SSL_ACCEPT:
		*event = EPOLLIN | EPOLLET;  // 对于SSL接受操作，设置为边缘触发的输入事件
		return 0;
	case PD_OP_SSL_CONNECT:
		*event = EPOLLOUT | EPOLLET;  // 对于SSL连接操作，设置为边缘触发的输出事件
		return 0;
	case PD_OP_SSL_SHUTDOWN:
		*event = EPOLLOUT | EPOLLET;  // 对于SSL关闭操作，设置为边缘触发的输出事件
		return 0;
	case PD_OP_EVENT:
		*event = EPOLLIN | EPOLLET;  // 对于事件操作，设置为边缘触发的输入事件
		return 1;
	case PD_OP_NOTIFY:
		*event = EPOLLIN | EPOLLET;  // 对于通知操作，设置为边缘触发的输入事件
		return 1;
	default:
		errno = EINVAL;  // 对于未知的操作类型，设置错误为无效参数
		return -1;  // 返回-1表示出错
	}
}

/**
 * @brief 函数poller_add，用于将一个指定的poller数据和超时时间添加到指定的poller结构中
 * @note poller_add向poller里添加一个fd，fd必须是nonblocking的，否则可能堵死内部线程。
 * fd的操作由struct poller_data的operation决定。
 * 
 * @param data 
 * @param timeout t表示毫秒级的超时（-1表示无限，同epoll风格）。
 * 					达到这个超时时间，fd以ERROR状态从callback返回，错误码（struct poller_result里的error）为ETIMEDOUT。
 * @param poller 
 * @return int 返回0表示添加成功，-1表示失败。
 * 			同一个fd如果添加两次，那么第二次添加返回-1，而且errno==EEXIST。
 * 			因此，如果需要在同一个poller里同时添加一个fd的读与写，需要通过dup()系统调用产生一个复制再添加。
 */
int poller_add(const struct poller_data *data, int timeout, poller_t *poller)
{
	struct __poller_node *res = NULL; // 监听到event、listen、notify事件后，将由回调函数存储一些临时信息
	struct __poller_node *node; // 用于存放新创建的poller节点
	int need_res; // 用于存放是否需要创建结果节点的标志
	int event; // 用于存放epoll的事件类型

	// 检查文件描述符是否在允许的范围内
	if ((size_t)data->fd >= poller->max_open_files)
	{
		errno = data->fd < 0 ? EBADF : EMFILE; // 文件描述符小于0，则错误为无效文件描述符，否则错误为打开的文件过多
		return -1;
	}

	// 获取需要监听的事件类型，并检查是否需要创建结果节点
	need_res = __poller_data_get_event(&event, data);
	if (need_res < 0)
		return -1; // 如果需要创建结果节点的标志为负，表示出错，返回-1

	// 如果需要创建结果节点，则为其分配空间
	if (need_res)
	{
		res = (struct __poller_node *)malloc(sizeof (struct __poller_node));
		if (!res)
			return -1; // 分配空间失败，返回-1
	}

	// 创建新的poller节点，并为其分配空间
	node = (struct __poller_node *)malloc(sizeof (struct __poller_node));
	if (node)
	{
		// 初始化新创建的poller节点
		node->data = *data;
		node->event = event;
		node->in_rbtree = 0;
		node->removed = 0;
		node->res = res;	// 添加‘捕获到每一个被监控的对象对应事件’后，由回调函数存储一些临时信息，
		if (timeout >= 0)
			__poller_node_set_timeout(timeout, node); // 如果提供了超时时间，则设置节点的超时时间

		pthread_mutex_lock(&poller->mutex); // 对poller结构进行加锁，防止数据竞争
		if (!poller->nodes[data->fd]) // 如果poller结构中还没有关于这个文件描述符的节点
		{
			// 在epoll实例中添加新的节点，并将新节点添加到对应的链表中
			if (__poller_add_fd(data->fd, event, node, poller) >= 0)
			{
				if (timeout >= 0)
					__poller_insert_node(node, poller); // 如果提供了超时时间，则将新节点插入到超时列表或超时红黑树中
				else
					list_add_tail(&node->list, &poller->no_timeo_list); // 否则，将新节点添加到无超时链表的尾部

				poller->nodes[data->fd] = node; // 在poller结构中添加用于被监控对象的新节点
				node = NULL; // 清空节点指针，防止后续误删除
			}
		}
		else if (poller->nodes[data->fd] == POLLER_NODE_ERROR) // 如果poller结构中已经存在错误节点
			errno = EINVAL; // 设置错误为无效参数
		else // 如果poller结构中已经存在这个文件描述符的节点
			errno = EEXIST; // 设置错误为文件已存在

		pthread_mutex_unlock(&poller->mutex); // 解锁poller结构
		if (node == NULL)
			return 0; // 如果节点指针为空，表示已成功添加节点，返回0

		free(node); // 释放节点指针
	}

	free(res); // 释放结果节点指针
	return -1; // 返回-1，表示添加节点失败
}


/**
 * @brief 删除一个fd。
 * 
 * @param fd 
 * @param poller 
 * @return int 返回0表示成果，返回-1表示失败。如果fd不存在，返回-1且errno==ENOENT。
 * 			当poller_del返回0，这个fd必然以DELETED状态从callback里返回。
 */
int poller_del(int fd, poller_t *poller)
{
	struct __poller_node *node;

	if ((size_t)fd >= poller->max_open_files)
	{
		errno = fd < 0 ? EBADF : EMFILE;
		return -1;
	}

	pthread_mutex_lock(&poller->mutex);
	node = poller->nodes[fd];
	if (node)
	{
		poller->nodes[fd] = NULL;

		if (node->in_rbtree)
			__poller_tree_erase(node, poller);
		else
			list_del(&node->list);

		__poller_del_fd(fd, node->event, poller);

		node->error = 0;
		node->state = PR_ST_DELETED;
		if (poller->stopped)
		{
			free(node->res);
			poller->cb((struct poller_result *)node, poller->ctx);
		}
		else
		{
			node->removed = 1;
			write(poller->pipe_wr, &node, sizeof (void *));
		}
	}
	else
		errno = ENOENT;

	pthread_mutex_unlock(&poller->mutex);
	return -!node;
}

int poller_mod(const struct poller_data *data, int timeout, poller_t *poller)
{
	struct __poller_node *res = NULL;
	struct __poller_node *node;
	struct __poller_node *old;
	int need_res;
	int event;

	if ((size_t)data->fd >= poller->max_open_files)
	{
		errno = data->fd < 0 ? EBADF : EMFILE;
		return -1;
	}

	need_res = __poller_data_get_event(&event, data);
	if (need_res < 0)
		return -1;

	if (need_res)
	{
		res = (struct __poller_node *)malloc(sizeof (struct __poller_node));
		if (!res)
			return -1;
	}

	node = (struct __poller_node *)malloc(sizeof (struct __poller_node));
	if (node)
	{
		node->data = *data;
		node->event = event;
		node->in_rbtree = 0;
		node->removed = 0;
		node->res = res;
		if (timeout >= 0)
			__poller_node_set_timeout(timeout, node);

		pthread_mutex_lock(&poller->mutex);
		old = poller->nodes[data->fd];
		if (old && old != POLLER_NODE_ERROR)
		{
			if (__poller_mod_fd(data->fd, old->event, event, node, poller) >= 0)
			{
				if (old->in_rbtree)
					__poller_tree_erase(old, poller);
				else
					list_del(&old->list);

				old->error = 0;
				old->state = PR_ST_MODIFIED;
				if (poller->stopped)
				{
					free(old->res);
					poller->cb((struct poller_result *)old, poller->ctx);
				}
				else
				{
					old->removed = 1;
					write(poller->pipe_wr, &old, sizeof (void *));
				}

				if (timeout >= 0)
					__poller_insert_node(node, poller);
				else
					list_add_tail(&node->list, &poller->no_timeo_list);

				poller->nodes[data->fd] = node;
				node = NULL;
			}
		}
		else if (old == POLLER_NODE_ERROR)
			errno = EINVAL;
		else
			errno = ENOENT;

		pthread_mutex_unlock(&poller->mutex);
		if (node == NULL)
			return 0;

		free(node);
	}

	free(res);
	return -1;
}

/**
 * @brief 重新设置fd的超时
 * 
 * @param fd 
 * @param timeout timeout定义与poller_add一样
 * @param poller 
 * @return int 返回0表示成功，返回-1表示失败。返回-1且errno==ENOENT时，表示fd不存在。
 */
int poller_set_timeout(int fd, int timeout, poller_t *poller)
{
	struct __poller_node time_node;
	struct __poller_node *node;

	if ((size_t)fd >= poller->max_open_files)
	{
		errno = fd < 0 ? EBADF : EMFILE;
		return -1;
	}

	if (timeout >= 0)
		__poller_node_set_timeout(timeout, &time_node);

	pthread_mutex_lock(&poller->mutex);
	node = poller->nodes[fd];
	if (node)
	{
		if (node->in_rbtree)
			__poller_tree_erase(node, poller);
		else
			list_del(&node->list);

		if (timeout >= 0)
		{
			node->timeout = time_node.timeout;
			__poller_insert_node(node, poller);
		}
		else
			list_add_tail(&node->list, &poller->no_timeo_list);
	}
	else
		errno = ENOENT;

	pthread_mutex_unlock(&poller->mutex);
	return -!node;
}

int poller_add_timer(const struct timespec *value, void *context,
					 poller_t *poller)
{
	LOG_TRACE("poller_add_timer");
	struct __poller_node *node;

	node = (struct __poller_node *)malloc(sizeof (struct __poller_node));
	if (node)
	{
		 // 初始化node
		memset(&node->data, 0, sizeof (struct poller_data));
		node->data.operation = PD_OP_TIMER;
		node->data.fd = -1;
		node->data.context = context;
		node->in_rbtree = 0;
		node->removed = 0;
		node->res = NULL;

		clock_gettime(CLOCK_MONOTONIC, &node->timeout);
		node->timeout.tv_sec += value->tv_sec;
		node->timeout.tv_nsec += value->tv_nsec;
		if (node->timeout.tv_nsec >= 1000000000)
		{
			node->timeout.tv_nsec -= 1000000000;
			node->timeout.tv_sec++;
		}

		pthread_mutex_lock(&poller->mutex);
		__poller_insert_node(node, poller);
		pthread_mutex_unlock(&poller->mutex);
		return 0;
	}

	return -1;
}

/**
 * @brief 停止poller
 * 当poller被停止时，所有处理中的fd以STOPPED状态从callback里返回。
 * poller停止之后，可以重新start。
 * start与stop显然必须串行依次调用。否则行为无定义。
 * @param poller 
 */
void poller_stop(poller_t *poller)
{
	struct __poller_node *node;
	struct list_head *pos, *tmp;
	void *p = NULL;

	write(poller->pipe_wr, &p, sizeof (void *));
	pthread_join(poller->tid, NULL);
	poller->stopped = 1;

	pthread_mutex_lock(&poller->mutex);
	poller->nodes[poller->pipe_rd] = NULL;
	poller->nodes[poller->pipe_wr] = NULL;
	close(poller->pipe_wr);
	__poller_handle_pipe(poller);
	close(poller->pipe_rd);

	poller->tree_first = NULL;
	while (poller->timeo_tree.rb_node)
	{
		node = rb_entry(poller->timeo_tree.rb_node, struct __poller_node, rb);
		rb_erase(&node->rb, &poller->timeo_tree);
		list_add(&node->list, &poller->timeo_list);
	}

	list_splice_init(&poller->no_timeo_list, &poller->timeo_list);
	list_for_each_safe(pos, tmp, &poller->timeo_list)
	{
		node = list_entry(pos, struct __poller_node, list);
		list_del(&node->list);
		if (node->data.fd >= 0)
		{
			poller->nodes[node->data.fd] = NULL;
			__poller_del_fd(node->data.fd, node->event, poller);
		}

		node->error = 0;
		node->state = PR_ST_STOPPED;
		free(node->res);
		poller->cb((struct poller_result *)node, poller->ctx);
	}

	pthread_mutex_unlock(&poller->mutex);
}

