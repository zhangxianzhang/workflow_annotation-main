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
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <openssl/ssl.h>
#include <openssl/bio.h>
#include "list.h"
#include "msgqueue.h"
#include "thrdpool.h"
#include "poller.h"
#include "mpoller.h"
#include "Communicator.h"

//这个结构体代表了在网络通信中的一个连接实体。主要是把一个连接的所有相关信息都集中在一起，这样在处理连接时就可以方便地获取所有需要的信息。
//在网络服务器中，每当有一个新的连接到来，都会创建一个 CommConnEntry 实例，保存这个连接的所有相关信息，并将这个实例加入到服务器的连接管理中。服务器就通过操作这些 CommConnEntry 实例，实现对所有连接的管理和控制。
struct CommConnEntry
{
	struct list_head list;   // 用于串起来
	CommConnection *conn;	 // 是一个指向 CommConnection 类型的指针，用于与CommSession互联。
	long long seq;		// 可能是用于表示这个连接的序列号或者版本号。
	int sockfd;         // 这个连接对应的套接字文件描述符
#define CONN_STATE_CONNECTING	0  // 这个状态表示正在尝试建立一个网络连接，但是连接还没有被完全建立起来。
#define CONN_STATE_CONNECTED	1  // 这个状态表示已经成功建立起了一个网络连接。
#define CONN_STATE_RECEIVING	2  // 这个状态表示网络连接现在正在接收数据。
#define CONN_STATE_SUCCESS		3  // 这个状态表示网络连接已经成功完成了一次数据传输。
#define CONN_STATE_IDLE			4  // 这个状态表示网络连接当前没有活动。通常这意味着它已经完成了一次数据传输，现在等待下一次的数据传输。
#define CONN_STATE_KEEPALIVE	5  // 这个状态表示网络连接现在正在保持活动状态，即使没有数据传输。这通常用于HTTP连接，HTTP连接可以在发送完一个响应之后保持开启，以便于发送下一个请求。
#define CONN_STATE_CLOSING		6  // 这个状态表示网络连接正在关闭。
#define CONN_STATE_ERROR		7  // 这个状态表示在网络连接上发生了一个错误。
	int state;
	int error;
	int ref;					  // 可能是用于管理该结构体的引用计数，以便于内存管理
	struct iovec *write_iov;      // 写缓冲区
	SSL *ssl;									
	CommSession *session;	   // 是一个指向 CommSession 类型的指针，可能存储了这个连接对应的会话信息。	
	CommTarget *target;        // 对端的通信目标
	CommService *service;
	mpoller_t *mpoller;
	/* Connection entry's mutex is for client session only. */
	pthread_mutex_t mutex;
};

// 设置文件描述符的标志为非阻塞模式
static inline int __set_fd_nonblock(int fd)
{
    // 获取当前文件描述符的标志
    int flags = fcntl(fd, F_GETFL);

    // 如果成功获取标志
    if (flags >= 0)
    {
        // 设置文件描述符的标志为非阻塞模式（使用位或运算符将 O_NONBLOCK 标志添加到原有标志上）
        flags = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

	return flags;
}

static int __bind_and_listen(int sockfd, const struct sockaddr *addr,
							 socklen_t addrlen)
{
	struct sockaddr_storage ss; // 存储套接字地址的结构
	socklen_t len;

	len = sizeof (struct sockaddr_storage);
	// 获取套接字的本地地址信息
	if (getsockname(sockfd, (struct sockaddr *)&ss, &len) < 0)
		return -1;

	ss.ss_family = 0;

	// 移除结构末尾的零字节
	/*
	移除结构体末尾的零字节的目的是为了确定结构体中最后一个非零字节的位置。这在处理套接字地址结构等情况下是常见的操作。

通常，套接字地址结构体（如 `struct sockaddr_storage`）在初始化或从系统调用中获取后，其字节序列的末尾可能包含零值字节。这些零值字节可以表示结构体的未使用部分或对齐要求。

在特定的情况下，需要确定结构体中真正包含有效数据的部分。通过移除末尾的零字节，可以得到结构体中最后一个非零字节的位置。这对于进一步的处理和判断非常有用。

在给定的代码示例中，`while` 循环通过递减 `len`，从结构体末尾开始逐个检查字节。当找到最后一个非零字节时，循环终止，此时 `len` 的值表示最后一个非零字节的索引位置。

移除结构体末尾的零字节的具体原因可能因具体情况而异。它可以是为了确保正确的内存布局和数据解析，或者是为了避免在后续处理中处理无效的数据。这取决于特定的应用场景和数据结构。
	*/
	while (len != 0)
	{
		if (((char *)&ss)[--len] != 0)
			break;
	}

	// 如果结构末尾全为零字节，说明获取到的本地地址是通配地址
	if (len == 0)
	{
		if (bind(sockfd, addr, addrlen) < 0)
			return -1;
	}

	return listen(sockfd, SOMAXCONN);
}

static int __create_ssl(SSL_CTX *ssl_ctx, struct CommConnEntry *entry)
{
	BIO *bio = BIO_new_socket(entry->sockfd, BIO_NOCLOSE);

	if (bio)
	{
		entry->ssl = SSL_new(ssl_ctx);
		if (entry->ssl)
		{
			SSL_set_bio(entry->ssl, bio, bio);
			return 0;
		}

		BIO_free(bio);
	}

	return -1;
}

int CommTarget::init(const struct sockaddr *addr, socklen_t addrlen,
					 int connect_timeout, int response_timeout)
{
	int ret;

	this->addr = (struct sockaddr *)malloc(addrlen);
	if (this->addr)
	{
		ret = pthread_mutex_init(&this->mutex, NULL);
		if (ret == 0)
		{
			memcpy(this->addr, addr, addrlen);
			this->addrlen = addrlen;
			this->connect_timeout = connect_timeout;
			this->response_timeout = response_timeout;
			INIT_LIST_HEAD(&this->idle_list);

			this->ssl_ctx = NULL;
			this->ssl_connect_timeout = 0;
			return 0;
		}

		errno = ret;
		free(this->addr);
	}

	return -1;
}

void CommTarget::deinit()
{
	pthread_mutex_destroy(&this->mutex);
	free(this->addr);
}

int CommMessageIn::feedback(const void *buf, size_t size)
{
	struct CommConnEntry *entry = this->entry;
	int ret;

	if (!entry->ssl)
		return write(entry->sockfd, buf, size);

	if (size == 0)
		return 0;

	ret = SSL_write(entry->ssl, buf, size);
	if (ret <= 0)
	{
		ret = SSL_get_error(entry->ssl, ret);
		if (ret != SSL_ERROR_SYSCALL)
			errno = -ret;

		ret = -1;
	}

	return ret;
}

int CommService::init(const struct sockaddr *bind_addr, socklen_t addrlen,
					  int listen_timeout, int response_timeout)
{
	int ret;

	this->bind_addr = (struct sockaddr *)malloc(addrlen);
	if (this->bind_addr)
	{
		ret = pthread_mutex_init(&this->mutex, NULL);
		if (ret == 0)
		{
			memcpy(this->bind_addr, bind_addr, addrlen);
			this->addrlen = addrlen;
			this->listen_timeout = listen_timeout;
			this->response_timeout = response_timeout;
			INIT_LIST_HEAD(&this->alive_list);

			this->ssl_ctx = NULL;
			this->ssl_accept_timeout = 0;
			return 0;
		}

		errno = ret;
		free(this->bind_addr);
	}

	return -1;
}

void CommService::deinit()
{
	pthread_mutex_destroy(&this->mutex);
	free(this->bind_addr);
}

int CommService::drain(int max)
{
	struct CommConnEntry *entry;
	struct list_head *pos;
	int errno_bak;
	int cnt = 0;

	errno_bak = errno;
	pthread_mutex_lock(&this->mutex);
	while (cnt != max && !list_empty(&this->alive_list))
	{
		pos = this->alive_list.next;
		entry = list_entry(pos, struct CommConnEntry, list);
		list_del(pos);
		cnt++;

		/* Cannot change the sequence of next two lines. */
		mpoller_del(entry->sockfd, entry->mpoller);
		entry->state = CONN_STATE_CLOSING;
	}

	pthread_mutex_unlock(&this->mutex);
	errno = errno_bak;
	return cnt;
}

// 服务目标是一种特殊的通信目标，多了与服务端通信的套接字和引用计数。在服务器的场景中，当一个新的连接到达时，我们通常会为这个连接创建一个服务目标，用于标记这个连接所请求的实现具体服务逻辑的服务端。
class CommServiceTarget : public CommTarget
{
public:
	void incref()
	{
		__sync_add_and_fetch(&this->ref, 1);
	}

	void decref()
	{
		if (__sync_sub_and_fetch(&this->ref, 1) == 0)
		{
			this->service->decref();
			this->deinit();
			delete this;
		}
	}

private:
	int sockfd;	// 表示的是与服务端通信的套接字
	int ref;	// 用于跟踪当前有多少个地方正在使用这个服务目标

private:
	CommService *service; // 表示的是这个服务目标所对应的服务端，实现具体服务逻辑的地方

private:
	// 覆盖了父类的同名函数，由于这是一个服务目标，而不是一个普通的通信目标，所以不应该创建连接。所以这个函数直接返回一个错误
	virtual int create_connect_fd()
	{
		errno = EPERM;
		return -1;
	}

	friend class Communicator;
};

CommSession::~CommSession()
{
	struct CommConnEntry *entry;
	struct list_head *pos;
	CommTarget *target;
	int errno_bak;

	if (!this->passive)
		return;

	target = this->target;
	if (this->passive == 1)
	{
		pthread_mutex_lock(&target->mutex);
		if (!list_empty(&target->idle_list))
		{
			pos = target->idle_list.next;
			entry = list_entry(pos, struct CommConnEntry, list);
			errno_bak = errno;
			mpoller_del(entry->sockfd, entry->mpoller);
			errno = errno_bak;
		}

		pthread_mutex_unlock(&target->mutex);
	}

	((CommServiceTarget *)target)->decref();
}

inline int Communicator::first_timeout(CommSession *session)
{
	int timeout = session->response_timeout();

	if (timeout < 0 || (unsigned int)session->timeout <= (unsigned int)timeout)
	{
		timeout = session->timeout;
		session->timeout = 0;
		session->begin_time.tv_nsec = 0;
	}
	else
		clock_gettime(CLOCK_MONOTONIC, &session->begin_time);

	return timeout;
}

int Communicator::next_timeout(CommSession *session)
{
	int timeout = session->response_timeout();
	struct timespec cur_time;
	int time_used, time_left;

	if (session->timeout > 0)
	{
		clock_gettime(CLOCK_MONOTONIC, &cur_time);
		time_used = 1000 * (cur_time.tv_sec - session->begin_time.tv_sec) +
					(cur_time.tv_nsec - session->begin_time.tv_nsec) / 1000000;
		time_left = session->timeout - time_used;
		if (time_left <= timeout) /* here timeout >= 0 */
		{
			timeout = time_left < 0 ? 0 : time_left;
			session->timeout = 0;
		}
	}

	return timeout;
}

int Communicator::first_timeout_send(CommSession *session)
{
	session->timeout = session->send_timeout();
	return Communicator::first_timeout(session);
}

int Communicator::first_timeout_recv(CommSession *session)
{
	session->timeout = session->receive_timeout();
	return Communicator::first_timeout(session);
}

void Communicator::release_conn(struct CommConnEntry *entry)
{
	delete entry->conn;
	if (!entry->service)
		pthread_mutex_destroy(&entry->mutex);

	if (entry->ssl)
		SSL_free(entry->ssl);

	close(entry->sockfd);
	free(entry);
}

void Communicator::shutdown_service(CommService *service)
{
	close(service->listen_fd);
	service->listen_fd = -1;
	service->drain(-1);
	service->decref();
}

#ifndef IOV_MAX
# ifdef UIO_MAXIOV
#  define IOV_MAX	UIO_MAXIOV
# else
#  define IOV_MAX	1024
# endif
#endif

/**
 * @brief 同步方式发送，该方法使用iovec数组作为输入，将其内容发送到网络连接上
 * 
 * @param vectors 
 * @param cnt 
 * @param entry 
 * @return int 
 */
int Communicator::send_message_sync(struct iovec vectors[], int cnt,
									struct CommConnEntry *entry)
{
	LOG_TRACE("Communicator::send_message_sync");
	// 获取通信会话和其他相关的变量
	CommSession *session = entry->session;
	CommService *service;
	int timeout;
	ssize_t n;
	int i;

	// 遍历所有的IO向量，将其内容写入到网络连接上
	while (cnt > 0)
	{
		// On success, readv() and preadv() return the number of bytes read; 
		// writev() and pwritev() return the number of bytes written. 
		// On error, -1 is returned, and errno is set appropriately.
		n = writev(entry->sockfd, vectors, cnt <= IOV_MAX ? cnt : IOV_MAX);
		if (n < 0)
			// 如果写入出错，根据错误类型返回相应的错误代码
			return errno == EAGAIN ? cnt : -1;

		// 遍历所有的IO向量，根据writev函数返回的写入字节数更新IO向量的内容和长度
		for (i = 0; i < cnt; i++)
		{
			if ((size_t)n >= vectors[i].iov_len)
				n -= vectors[i].iov_len;
			else
			{
				vectors[i].iov_base = (char *)vectors[i].iov_base + n;
				vectors[i].iov_len -= n;
				break;
			}
		}

		// 更新IO向量数组的起始位置和长度，以便在下一次循环中写入剩余的数据
		vectors += i;
		cnt -= i;
	}

	// 获取通信服务对象，以便进行后续的操作
	service = entry->service;
	if (service)
	{
		// 如果存在通信服务，增加连接条目的引用计数，设置保活超时，并根据超时的情况进行不同的处理
		__sync_add_and_fetch(&entry->ref, 1);
		timeout = session->keep_alive_timeout();
		switch (timeout)
		{
		default:
			// 如果超时的值为非零，设置网络连接的超时，然后将连接条目添加到服务的保活列表中
			mpoller_set_timeout(entry->sockfd, timeout, this->mpoller);
			pthread_mutex_lock(&service->mutex);
			if (service->listen_fd >= 0)
			{
				entry->state = CONN_STATE_KEEPALIVE;
				list_add_tail(&entry->list, &service->alive_list);
				entry = NULL;
			}

			pthread_mutex_unlock(&service->mutex);
			if (entry)
			{
		case 0:
				// 如果超时的值为0，删除网络连接的多路复用，然后设置连接条目的状态为正在关闭
				mpoller_del(entry->sockfd, this->mpoller);
				entry->state = CONN_STATE_CLOSING;
			}
		}
	}
	else
	{
		// 如果没有通信服务，根据会话的超时进行不同的处理，然后设置连接条目的状态为正在接收
		if (entry->state == CONN_STATE_IDLE)
		{
			timeout = session->first_timeout();
			if (timeout == 0)
				timeout = Communicator::first_timeout_recv(session);
			else
			{
				session->timeout = -1;
				session->begin_time.tv_nsec = -1;
			}

			mpoller_set_timeout(entry->sockfd, timeout, this->mpoller);
		}

		entry->state = CONN_STATE_RECEIVING;
	}

	// 如果所有的操作都成功，返回0表示成功
	return 0;
}

/**
 * @brief 异步方式发送
 * 
 * @param vectors 
 * @param cnt 
 * @param entry 
 * @return int 
 */
int Communicator::send_message_async(struct iovec vectors[], int cnt,
									 struct CommConnEntry *entry)
{
	struct poller_data data;
	int timeout;
	int ret;
	int i;

	entry->write_iov = (struct iovec *)malloc(cnt * sizeof (struct iovec));
	if (entry->write_iov)
	{
		for (i = 0; i < cnt; i++)
			entry->write_iov[i] = vectors[i];
	}
	else
		return -1;

	data.operation = PD_OP_WRITE;
	data.fd = entry->sockfd;
	data.ssl = entry->ssl;
	data.context = entry;
	data.write_iov = entry->write_iov;
	data.iovcnt = cnt;
	timeout = Communicator::first_timeout_send(entry->session);
	if (entry->state == CONN_STATE_IDLE)
	{
		ret = mpoller_mod(&data, timeout, this->mpoller);
		if (ret < 0 && errno == ENOENT)
			entry->state = CONN_STATE_RECEIVING;
	}
	else
	{
		ret = mpoller_add(&data, timeout, this->mpoller);
		if (ret >= 0)
		{
			if (this->stop_flag)
				mpoller_del(data.fd, this->mpoller);
		}
	}

	if (ret < 0)
	{
		free(entry->write_iov);
		if (entry->state != CONN_STATE_RECEIVING)
			return -1;
	}

	return 1;
}

#define ENCODE_IOV_MAX		8192

int Communicator::send_message(struct CommConnEntry *entry)
{
	LOG_TRACE("Communicator::send_message");
	struct iovec vectors[ENCODE_IOV_MAX];
	struct iovec *end;
	int cnt;

	// 1. http client，这里的encode是HttpMessage的实现
	cnt = entry->session->out->encode(vectors, ENCODE_IOV_MAX);
	if ((unsigned int)cnt > ENCODE_IOV_MAX)
	{
		if (cnt > ENCODE_IOV_MAX)
			errno = EOVERFLOW;
		return -1;
	}
	
	end = vectors + cnt;
	if (!entry->ssl)
	{
		// 消息一次发得出去，就不走异步写了啊。这样子快。试个大一点的消息，就会进poller了。
		cnt = this->send_message_sync(vectors, cnt, entry);
		if (cnt <= 0)
			return cnt;
	}

	return this->send_message_async(end - cnt, cnt, entry);
}

void Communicator::handle_incoming_request(struct poller_result *res)
{
	struct CommConnEntry *entry = (struct CommConnEntry *)res->data.context; // 处理收到的请求只需要连接信息
	CommTarget *target = entry->target;
	CommSession *session = NULL;
	int state;

	switch (res->state)
	{
	case PR_ST_SUCCESS:
		session = entry->session;
		state = CS_STATE_TOREPLY;
		pthread_mutex_lock(&target->mutex);
		if (entry->state == CONN_STATE_SUCCESS)
		{
			__sync_add_and_fetch(&entry->ref, 1);
			entry->state = CONN_STATE_IDLE;
			list_add(&entry->list, &target->idle_list);
		}

		pthread_mutex_unlock(&target->mutex);
		break;

	case PR_ST_FINISHED:
		res->error = ECONNRESET;
		if (1)
	case PR_ST_ERROR:
			state = CS_STATE_ERROR;
		else
	case PR_ST_DELETED:
	case PR_ST_STOPPED:
			state = CS_STATE_STOPPED;

		pthread_mutex_lock(&target->mutex);
		switch (entry->state)
		{
		case CONN_STATE_KEEPALIVE:
			pthread_mutex_lock(&entry->service->mutex);
			if (entry->state == CONN_STATE_KEEPALIVE)
				list_del(&entry->list);
			pthread_mutex_unlock(&entry->service->mutex);
			break;

		case CONN_STATE_IDLE:
			list_del(&entry->list);
			break;

		case CONN_STATE_ERROR:
			res->error = entry->error;
			state = CS_STATE_ERROR;
		case CONN_STATE_RECEIVING:
			session = entry->session;
			break;

		case CONN_STATE_SUCCESS:
			/* This may happen only if handler_threads > 1. */
			entry->state = CONN_STATE_CLOSING;
			entry = NULL;
			break;
		}

		pthread_mutex_unlock(&target->mutex);
		break;
	}

	if (entry)
	{
		if (session)
			session->handle(state, res->error);

		if (__sync_sub_and_fetch(&entry->ref, 1) == 0)
		{
			this->release_conn(entry);
			((CommServiceTarget *)target)->decref();
		}
	}
}

void Communicator::handle_incoming_reply(struct poller_result *res)
{
	struct CommConnEntry *entry = (struct CommConnEntry *)res->data.context;
	CommTarget *target = entry->target;
	CommSession *session = NULL;
	pthread_mutex_t *mutex;
	int state;

	switch (res->state)
	{
	case PR_ST_SUCCESS:
		session = entry->session;
		state = CS_STATE_SUCCESS;
		pthread_mutex_lock(&target->mutex);
		if (entry->state == CONN_STATE_SUCCESS)
		{
			__sync_add_and_fetch(&entry->ref, 1);
			if (session->timeout != 0) /* This is keep-alive timeout. */
			{
				entry->state = CONN_STATE_IDLE;
				list_add(&entry->list, &target->idle_list);
			}
			else
				entry->state = CONN_STATE_CLOSING;
		}

		pthread_mutex_unlock(&target->mutex);
		break;

	case PR_ST_FINISHED:
		res->error = ECONNRESET;
		if (1)
	case PR_ST_ERROR:
			state = CS_STATE_ERROR;
		else
	case PR_ST_DELETED:
	case PR_ST_STOPPED:
			state = CS_STATE_STOPPED;

		mutex = &entry->mutex;
		pthread_mutex_lock(&target->mutex);
		pthread_mutex_lock(mutex);
		switch (entry->state)
		{
		case CONN_STATE_IDLE:
			list_del(&entry->list);
			break;

		case CONN_STATE_ERROR:
			res->error = entry->error;
			state = CS_STATE_ERROR;
		case CONN_STATE_RECEIVING:
			session = entry->session;
			break;

		case CONN_STATE_SUCCESS:
			/* This may happen only if handler_threads > 1. */
			entry->state = CONN_STATE_CLOSING;
			entry = NULL;
			break;
		}

		pthread_mutex_unlock(&target->mutex);
		pthread_mutex_unlock(mutex);
		break;
	}

	if (entry)
	{
		if (session)
		{
			target->release(entry->state == CONN_STATE_IDLE);
			session->handle(state, res->error);
		}

		if (__sync_sub_and_fetch(&entry->ref, 1) == 0)
			this->release_conn(entry);
	}
}

void Communicator::handle_read_result(struct poller_result *res)
{
	struct CommConnEntry *entry = (struct CommConnEntry *)res->data.context;

	if (res->state != PR_ST_MODIFIED)
	{
		if (entry->service)
			this->handle_incoming_request(res);
		else
			this->handle_incoming_reply(res);
	}
}

void Communicator::handle_reply_result(struct poller_result *res)
{
	struct CommConnEntry *entry = (struct CommConnEntry *)res->data.context;
	CommService *service = entry->service;
	CommSession *session = entry->session;
	CommTarget *target = entry->target;
	int timeout;
	int state;

	switch (res->state)
	{
	case PR_ST_FINISHED:
		timeout = session->keep_alive_timeout();
		if (timeout != 0)
		{
			__sync_add_and_fetch(&entry->ref, 1);
			res->data.operation = PD_OP_READ;
			res->data.message = NULL;
			pthread_mutex_lock(&target->mutex);
			if (mpoller_add(&res->data, timeout, this->mpoller) >= 0)
			{
				pthread_mutex_lock(&service->mutex);
				if (!this->stop_flag && service->listen_fd >= 0)
				{
					entry->state = CONN_STATE_KEEPALIVE;
					list_add_tail(&entry->list, &service->alive_list);
				}
				else
				{
					mpoller_del(res->data.fd, this->mpoller);
					entry->state = CONN_STATE_CLOSING;
				}

				pthread_mutex_unlock(&service->mutex);
			}
			else
				__sync_sub_and_fetch(&entry->ref, 1);

			pthread_mutex_unlock(&target->mutex);
		}

		if (1)
			state = CS_STATE_SUCCESS;
		else if (1)
	case PR_ST_ERROR:
			state = CS_STATE_ERROR;
		else
	case PR_ST_DELETED:		/* DELETED seems not possible. */
	case PR_ST_STOPPED:
			state = CS_STATE_STOPPED;

		session->handle(state, res->error);
		if (__sync_sub_and_fetch(&entry->ref, 1) == 0)
		{
			this->release_conn(entry);
			((CommServiceTarget *)target)->decref();
		}

		break;
	}
}

void Communicator::handle_request_result(struct poller_result *res)
{
	struct CommConnEntry *entry = (struct CommConnEntry *)res->data.context;
	CommSession *session = entry->session;
	int timeout;
	int state;

	switch (res->state)
	{
	case PR_ST_FINISHED:
		entry->state = CONN_STATE_RECEIVING;
		res->data.operation = PD_OP_READ;
		res->data.message = NULL;
		timeout = session->first_timeout();
		if (timeout == 0)
			timeout = Communicator::first_timeout_recv(session);
		else
		{
			session->timeout = -1;
			session->begin_time.tv_nsec = -1;
		}

		if (mpoller_add(&res->data, timeout, this->mpoller) >= 0)
		{
			if (this->stop_flag)
				mpoller_del(res->data.fd, this->mpoller);
			break;
		}

		res->error = errno;
		if (1)
	case PR_ST_ERROR:
			state = CS_STATE_ERROR;
		else
	case PR_ST_DELETED:
	case PR_ST_STOPPED:
			state = CS_STATE_STOPPED;

		entry->target->release(0);
		session->handle(state, res->error);
		pthread_mutex_lock(&entry->mutex);
		/* do nothing */
		pthread_mutex_unlock(&entry->mutex);
		if (__sync_sub_and_fetch(&entry->ref, 1) == 0)
			this->release_conn(entry);

		break;
	}
}

void Communicator::handle_write_result(struct poller_result *res)
{
	struct CommConnEntry *entry = (struct CommConnEntry *)res->data.context;

	free(entry->write_iov);
	if (entry->service)
		this->handle_reply_result(res);
	else
		this->handle_request_result(res);
}

// 将输入的套接字设置为非阻塞模式，并且把一个连接的所有相关信息都集中 CommConnEntry 结构体
struct CommConnEntry *Communicator::accept_conn(CommServiceTarget *target,
												CommService *service)
{
	struct CommConnEntry *entry;  // 用于存储新连接信息的结构体指针 
	size_t size;

	// 将输入的套接字设置为非阻塞模式，这样在等待网络操作时，如果没有数据可读或者没有空间可写，函数可以立即返回，而不是阻塞等待，从而提高程序的效率
	if (__set_fd_nonblock(target->sockfd) >= 0)
	{
		size = offsetof(struct CommConnEntry, mutex);
		entry = (struct CommConnEntry *)malloc(size);
		if (entry)
		{
			entry->conn = service->new_connection(target->sockfd);
			if (entry->conn)
			{
				entry->seq = 0; // 连接序列号设为0
				entry->mpoller = this->mpoller;
				entry->service = service;
				entry->target = target;
				entry->ssl = NULL;
				entry->sockfd = target->sockfd;
				entry->state = CONN_STATE_CONNECTED; // 这个状态表示已经成功建立起了一个网络连接。
				entry->ref = 1;
				return entry;
			}

			free(entry);
		}
	}

	return NULL;
}

void Communicator::handle_listen_result(struct poller_result *res)
{
	CommService *service = (CommService *)res->data.context;// 见int Communicator::bind(CommService *service)
	struct CommConnEntry *entry;
	CommServiceTarget *target;
	int timeout;

	switch (res->state)
	{
	case PR_ST_SUCCESS:
		target = (CommServiceTarget *)res->data.result; // 获取poller_result的处理结果 见static void __poller_handle_listen(struct __poller_node *node,poller_t *poller)
		entry = this->accept_conn(target, service);
		if (entry)
		{
			if (service->ssl_ctx)
			{
				if (__create_ssl(service->ssl_ctx, entry) >= 0 &&
					service->init_ssl(entry->ssl) >= 0)
				{
					res->data.operation = PD_OP_SSL_ACCEPT;
					timeout = service->ssl_accept_timeout;
				}
			}
			else // 监听结果的处理只需要设置 PD_OP_READ 和 响应超时时间
			{
				res->data.operation = PD_OP_READ; // 使得在poller中进行读取
				res->data.message = NULL; 
				timeout = target->response_timeout; // 设置响应超时时间
			}

			if (res->data.operation != PD_OP_LISTEN)
			{
				res->data.fd = entry->sockfd;
				res->data.ssl = entry->ssl;
				res->data.context = entry;	// 非PD_OP_LISTEN过程只需要传递连接相关信息
				if (mpoller_add(&res->data, timeout, this->mpoller) >= 0) // 将处理线程的处理结果传递给一个多路复用器
				{
					if (this->stop_flag)
						mpoller_del(res->data.fd, this->mpoller);
					break;
				}
			}

			this->release_conn(entry);
		}
		else
			close(target->sockfd);

		target->decref();
		break;

	case PR_ST_DELETED:
		this->shutdown_service(service);
		break;

	case PR_ST_ERROR:
	case PR_ST_STOPPED:
		service->handle_stop(res->error);
		break;
	}
}

void Communicator::handle_connect_result(struct poller_result *res)
{
	struct CommConnEntry *entry = (struct CommConnEntry *)res->data.context;
	CommSession *session = entry->session;
	CommTarget *target = entry->target;
	int timeout;
	int state;
	int ret;

	switch (res->state)
	{
	case PR_ST_FINISHED:
		if (target->ssl_ctx && !entry->ssl)
		{
			if (__create_ssl(target->ssl_ctx, entry) >= 0 &&
				target->init_ssl(entry->ssl) >= 0)
			{
				ret = 0;
				res->data.operation = PD_OP_SSL_CONNECT;
				res->data.ssl = entry->ssl;
				timeout = target->ssl_connect_timeout;
			}
			else
				ret = -1;
		}
		else if ((session->out = session->message_out()) != NULL)
		{
			ret = this->send_message(entry);
			if (ret == 0)
			{
				res->data.operation = PD_OP_READ;
				res->data.message = NULL;
   //新版本添加了 res->data.create_message = Communicator::create_request;
				timeout = session->first_timeout();
				if (timeout == 0)
					timeout = Communicator::first_timeout_recv(session);
				else
				{
					session->timeout = -1;
					session->begin_time.tv_nsec = -1;
				}
			}
			else if (ret > 0)
				break;
		}
		else
			ret = -1;

		if (ret >= 0)
		{
			if (mpoller_add(&res->data, timeout, this->mpoller) >= 0)
			{
				if (this->stop_flag)
					mpoller_del(res->data.fd, this->mpoller);
				break;
			}
		}

		res->error = errno;
		if (1)
	case PR_ST_ERROR:
			state = CS_STATE_ERROR;
		else
	case PR_ST_DELETED:
	case PR_ST_STOPPED:
			state = CS_STATE_STOPPED;

		target->release(0);
		session->handle(state, res->error);
		this->release_conn(entry);
		break;
	}
}

void Communicator::handle_ssl_accept_result(struct poller_result *res)
{
	struct CommConnEntry *entry = (struct CommConnEntry *)res->data.context;
	CommTarget *target = entry->target;
	int timeout;

	switch (res->state)
	{
	case PR_ST_FINISHED:
		res->data.operation = PD_OP_READ;
		res->data.message = NULL;
		timeout = target->response_timeout;
		if (mpoller_add(&res->data, timeout, this->mpoller) >= 0)
		{
			if (this->stop_flag)
				mpoller_del(res->data.fd, this->mpoller);
			break;
		}

	case PR_ST_DELETED:
	case PR_ST_ERROR:
	case PR_ST_STOPPED:
		this->release_conn(entry);
		((CommServiceTarget *)target)->decref();
		break;
	}
}

void Communicator::handle_sleep_result(struct poller_result *res)
{
	SleepSession *session = (SleepSession *)res->data.context;
	int state;

	if (res->state == PR_ST_STOPPED)
		state = SS_STATE_DISRUPTED;
	else
		state = SS_STATE_COMPLETE;

	session->handle(state, 0);
}

void Communicator::handle_aio_result(struct poller_result *res)
{
	IOService *service = (IOService *)res->data.context;
	IOSession *session;
	int state, error;

	switch (res->state)
	{
	case PR_ST_SUCCESS:
		session = (IOSession *)res->data.result;
		pthread_mutex_lock(&service->mutex);
		list_del(&session->list);
		pthread_mutex_unlock(&service->mutex);
		if (session->res >= 0)
		{
			state = IOS_STATE_SUCCESS;
			error = 0;
		}
		else
		{
			state = IOS_STATE_ERROR;
			error = -session->res;
		}

		session->handle(state, error);
		service->decref();
		break;

	case PR_ST_DELETED:
		this->shutdown_io_service(service);
		break;

	case PR_ST_ERROR:
	case PR_ST_STOPPED:
		service->handle_stop(res->error);
		break;
	}
}

void Communicator::handler_thread_routine(void *context)
{
	// 将context参数转换为Communicator指针。
    // context参数通常包含需要传递给线程的信息，在这里我们需要传递一个Communicator的实例。
	Communicator *comm = (Communicator *)context;
	struct poller_result *res;

    // 循环读取并处理消息队列中的事件。如果消息队列为空，msgqueue_get会阻塞等待，直到有新的事件被加入队列。
    while ((res = (struct poller_result *)msgqueue_get(comm->queue)) != NULL)
    {
        // 根据事件的操作类型（res->data.operation），调用相应的处理函数。
        switch (res->data.operation)
        {
            // 如果操作类型是读操作，调用handle_read_result处理读结果。
            case PD_OP_READ:
                comm->handle_read_result(res);
                break;
            // 如果操作类型是写操作，调用handle_write_result处理写结果。
            case PD_OP_WRITE:
                comm->handle_write_result(res);
                break;
            // 如果操作类型是连接操作，调用handle_connect_result处理连接结果。
            case PD_OP_CONNECT:
            case PD_OP_SSL_CONNECT:
                comm->handle_connect_result(res);
                break;
            // 如果操作类型是监听操作，调用handle_listen_result处理监听结果。
            case PD_OP_LISTEN:
                comm->handle_listen_result(res);
                break;
            // 如果操作类型是SSL接受操作，调用handle_ssl_accept_result处理SSL接受结果。
            case PD_OP_SSL_ACCEPT:
                comm->handle_ssl_accept_result(res);
                break;
            // 如果操作类型是事件通知，调用handle_aio_result处理事件通知结果。
            case PD_OP_EVENT:
            case PD_OP_NOTIFY:
                comm->handle_aio_result(res);
                break;
            // 如果操作类型是定时器操作，调用handle_sleep_result处理定时器结果。
            case PD_OP_TIMER:
                comm->handle_sleep_result(res);
                break;
        }

        // 事件处理完成后，释放事件结构体占用的内存。
        free(res);
    }
}

int Communicator::append(const void *buf, size_t *size, poller_message_t *msg)
{
	// 首先，将传入的消息（poller_message_t类型）转换为CommMessageIn类型
	CommMessageIn *in = (CommMessageIn *)msg;
	struct CommConnEntry *entry = in->entry;
	CommSession *session = entry->session;
	int timeout;
	int ret;

    // 使用CommMessageIn的append函数，将buf中的数据追加到消息中
    // 如果成功，则返回追加的字节数；如果失败，则返回-1
	ret = in->append(buf, size); // int HttpRequest::append(const void *buf, size_t *size)
    if (ret > 0)
    {
        // 追加成功，将连接状态设为成功
        entry->state = CONN_STATE_SUCCESS;
        
        // 判断服务是否存在
        if (entry->service)
            // 如果服务存在，则将超时设为-1
            timeout = -1;
        else
        {
            // 如果服务不存在，则获取session的保持连接的超时时间，复用session的超时字段
            timeout = session->keep_alive_timeout();
            session->timeout = timeout; 

            // 如果超时为0，从多路复用器中删除该连接，并返回追加的字节数
            if (timeout == 0)
            {
                mpoller_del(entry->sockfd, entry->mpoller);
                return ret;
            }
        }
    }
    else if (ret == 0 && session->timeout != 0)
    {
        // 如果没有数据追加，但是session的超时不为0，计算下一次的超时时间
        if (session->begin_time.tv_nsec == -1)
            // 如果是首次接收，使用first_timeout_recv函数计算首次接收的超时时间
            timeout = Communicator::first_timeout_recv(session);
        else
            // 如果不是首次接收，使用next_timeout函数计算下一次接收的超时时间
            timeout = Communicator::next_timeout(session);
    }
    else
        // 如果追加失败，或者没有数据追加且session的超时为0，直接返回结果
        return ret;

    // 设置多路复用器的超时时间
    mpoller_set_timeout(entry->sockfd, timeout, entry->mpoller);
    
    // 返回追加的字节数或者错误代码
    return ret;
}

int Communicator::create_service_session(struct CommConnEntry *entry)
{
	CommService *service = entry->service;
	CommTarget *target = entry->target;
	CommSession *session;
	int timeout;

	pthread_mutex_lock(&service->mutex);
	if (entry->state == CONN_STATE_KEEPALIVE)
		list_del(&entry->list);
	else if (entry->state != CONN_STATE_CONNECTED)
		entry = NULL;

	pthread_mutex_unlock(&service->mutex);
	if (!entry)
	{
		errno = ENOENT;
		return -1;
	}

	session = service->new_session(entry->seq, entry->conn);  // web服务器将调用 template<> inline CommSession *WFHttpServer::new_session(long long seq, CommConnection *conn)
	if (session)
	{
		session->passive = 1;	// 设置session为被动模式（服务器主动连接到客户端）
		entry->session = session; 
		session->target = target; // 为了通过任务找到通讯目标
		session->conn = entry->conn;// 为了通过任务找到连接信息
		session->seq = entry->seq++;
		session->out = NULL;
		session->in = NULL;

		timeout = Communicator::first_timeout_recv(session);
		mpoller_set_timeout(entry->sockfd, timeout, entry->mpoller);
		entry->state = CONN_STATE_RECEIVING;

		((CommServiceTarget *)target)->incref();
		return 0;
	}

	return -1;
}

poller_message_t *Communicator::create_message(void *context)
{	
	// 从context中获取一个通信连接入口
	struct CommConnEntry *entry = (struct CommConnEntry *)context;
	CommSession *session;

	// 如果连接状态为空闲
	if (entry->state == CONN_STATE_IDLE)
	{
		pthread_mutex_t *mutex;

		// 如果entry有对应的服务，就获取服务的互斥锁
		if (entry->service)
			mutex = &entry->target->mutex;
		else
			// 否则就获取entry自己的互斥锁
			mutex = &entry->mutex;

		// 上锁
		pthread_mutex_lock(mutex);
		/* do nothing */

		// 解锁
		pthread_mutex_unlock(mutex);
	}

	// 如果连接状态是已连接或保持活跃
	if (entry->state == CONN_STATE_CONNECTED ||
		entry->state == CONN_STATE_KEEPALIVE)
	{
		// 创建服务会话
		if (Communicator::create_service_session(entry) < 0)
			return NULL;
	}
	else if (entry->state != CONN_STATE_RECEIVING)
	{	
		// 如果当前状态不是正在接收数据，设置错误码为EBADMSG，并返回NULL
		errno = EBADMSG;
		return NULL;
	}

	// 获取当前的会话
	session = entry->session;
	// 创建一个新的消息并赋值给会话的输入消息
	session->in = session->message_in(); 
	if (session->in)
	{
		// 如果输入消息创建成功，设置消息的append方法和entry字段
		session->in->poller_message_t::append = Communicator::append; // HTTP服务器：追加数据到 HTTP 消息，输入参数为数据和数据大小
		session->in->entry = entry;
	}

	return session->in;
}

int Communicator::partial_written(size_t n, void *context)
{
	struct CommConnEntry *entry = (struct CommConnEntry *)context;
	CommSession *session = entry->session;
	int timeout;

	timeout = Communicator::next_timeout(session);
	mpoller_set_timeout(entry->sockfd, timeout, entry->mpoller);
	return 0;
}

// 当某些特定事件发生时（比如在异步 I/O 或者多线程编程中，某个操作完成或收到某个信号时），将这个事件的结果或相关信息放入 Communicator 对象的消息队列中，，以便在其他地方处理这些事件
void Communicator::callback(struct poller_result *res, void *context)
{
	Communicator *comm = (Communicator *)context;
	msgqueue_put(res, comm->queue);
}

void *Communicator::accept(const struct sockaddr *addr, socklen_t addrlen,
						   int sockfd, void *context)
{
	// 把context转换成 CommService 类型，context中实际上保存的是 CommService 对象的指针
	CommService *service = (CommService *)context;

	// 创建一个新的 CommServiceTarget 对象
	CommServiceTarget *target = new CommServiceTarget;

	// 如果创建成功
	if (target)
	{
		// 初始化target对象，设置地址，地址长度，优先级和响应超时时间
		if (target->init(addr, addrlen, 0, service->response_timeout) >= 0)
		{
			// 增加 service 的引用计数
			service->incref();

			// 设置 target 的 service 属性为 service，设置 target 的 sockfd 属性为接受到的 sockfd
			target->service = service;
			target->sockfd = sockfd;

			// 设置 target 的引用计数为 1
			target->ref = 1;

			// 返回 target 的指针
			return target;
		}

		// 如果初始化 target 失败，删除 target 对象
		delete target;
	}

	// 如果创建或者初始化 target 失败，关闭 sockfd
	close(sockfd);

	// 返回 NULL
	return NULL;
}

int Communicator::create_handler_threads(size_t handler_threads)
{
	struct thrdpool_task task = {
		.routine	=	Communicator::handler_thread_routine,
		.context	=	this
	};
	size_t i;

	LOG_TRACE("create handler thread pool");
	this->thrdpool = thrdpool_create(handler_threads, 0);
	if (this->thrdpool)
	{
		for (i = 0; i < handler_threads; i++)
		{
			if (thrdpool_schedule(&task, this->thrdpool) < 0)
				break;
		}

		if (i == handler_threads)
			return 0;

		msgqueue_set_nonblock(this->queue);
		thrdpool_destroy(NULL, this->thrdpool);
	}

	return -1;
}

/**
 * @brief 创建msgqueue和mpoller
 * 
 * @param poller_threads 
 * @return int 0 : 成功 -1 : 失败
 */
int Communicator::create_poller(size_t poller_threads)
{
	struct poller_params params = {
		.max_open_files		=	65536,
		.create_message		=	Communicator::create_message,
		.partial_written	=	Communicator::partial_written,
		.callback			=	Communicator::callback, // 当某些特定事件发生时（比如在异步 I/O 或者多线程编程中，某个操作完成或收到某个信号时），将这个事件的结果或相关信息放入 Communicator 对象的消息队列中
		.context			=	this 					// // 将Communicator聚合的信息队列作为mpoller内所有poller管理事件发生后的回调参数，以至于能与处理线程通信
	};

	this->queue = msgqueue_create(4096, sizeof (struct poller_result));
	if (this->queue)  // queue若成功创建
	{
		this->mpoller = mpoller_create(&params, poller_threads);
		if (this->mpoller)  // mpoller若成功创建
		{
			if (mpoller_start(this->mpoller) >= 0)  // 成功启动
				return 0;

			mpoller_destroy(this->mpoller);
		}

		msgqueue_destroy(this->queue);
	}

	return -1;
}


/**
 * @brief 主要就两件事，一个是创建poller线程，一个就是创建线程池
 * 
 * @param poller_threads 
 * @param handler_threads 
 * @return int - 0 ：成功， -1 失败(查看errno)
 * 
 */
int Communicator::init(size_t poller_threads, size_t handler_threads)
{
	if (poller_threads == 0)
	{
		errno = EINVAL; // EINVAL表示无效的参数，即为invalid argument 
		return -1;
	}

	if (this->create_poller(poller_threads) >= 0)
	{
		if (this->create_handler_threads(handler_threads) >= 0)
		{
			this->stop_flag = 0;
			return 0;   // init成功
		}
		// 没create成功则
		mpoller_stop(this->mpoller);
		mpoller_destroy(this->mpoller);
		msgqueue_destroy(this->queue);
	}

	return -1;
}

void Communicator::deinit()
{
	this->stop_flag = 1;
	mpoller_stop(this->mpoller);
	msgqueue_set_nonblock(this->queue);
	thrdpool_destroy(NULL, this->thrdpool);
	mpoller_destroy(this->mpoller);
	msgqueue_destroy(this->queue);
}

int Communicator::nonblock_connect(CommTarget *target)
{
	// 创建cfd
	int sockfd = target->create_connect_fd();

	if (sockfd >= 0)
	{
		// 设置非阻塞
		if (__set_fd_nonblock(sockfd) >= 0)
		{
			// 然后调用connec连接
			if (connect(sockfd, target->addr, target->addrlen) >= 0 ||
				errno == EINPROGRESS)
			{
				return sockfd;
			}
		}

		close(sockfd);
	}

	return -1;
}

struct CommConnEntry *Communicator::launch_conn(CommSession *session,
												CommTarget *target)
{
	LOG_TRACE("Communicator::launch_conn");
	struct CommConnEntry *entry;
	int sockfd;
	int ret;
	// 1. target connect 建立连接
	sockfd = this->nonblock_connect(target);
	if (sockfd >= 0)
	{
		entry = (struct CommConnEntry *)malloc(sizeof (struct CommConnEntry));
		if (entry)
		{
			ret = pthread_mutex_init(&entry->mutex, NULL);
			if (ret == 0)
			{
				// 2. 创建新的CommConnection
				entry->conn = target->new_connection(sockfd);
				if (entry->conn)
				{
					entry->seq = 0;
					entry->mpoller = this->mpoller;
					entry->service = NULL;
					entry->target = target;
					entry->session = session;
					entry->ssl = NULL;
					entry->sockfd = sockfd;
					entry->state = CONN_STATE_CONNECTING;
					entry->ref = 1;
					return entry;
				}

				pthread_mutex_destroy(&entry->mutex);
			}
			else
				errno = ret;

			free(entry);
		}

		close(sockfd);
	}

	return NULL;
}

/**
 * @brief 获取空闲连接
 * 
 * @param target 
 * @return struct CommConnEntry* 
 */
struct CommConnEntry *Communicator::get_idle_conn(CommTarget *target)
{
	LOG_TRACE("Communicator::get_idle_conn");
	struct CommConnEntry *entry;
	struct list_head *pos;

	// 遍历target的idle_list 获取空闲连接
	list_for_each(pos, &target->idle_list)
	{
		entry = list_entry(pos, struct CommConnEntry, list);
		// 重新设置超时时间，成功
		if (mpoller_set_timeout(entry->sockfd, -1, this->mpoller) >= 0)
		{
			list_del(pos);
			return entry;
		}
	}

	errno = ENOENT;
	return NULL;
}

// 优先复用connection
int Communicator::request_idle_conn(CommSession *session, CommTarget *target)
{
	LOG_TRACE("Communicator::request_idle_conn");
	struct CommConnEntry *entry;
	int ret = -1;

	pthread_mutex_lock(&target->mutex);

	entry = this->get_idle_conn(target);

	if (entry)
		pthread_mutex_lock(&entry->mutex);
	pthread_mutex_unlock(&target->mutex);
	if (entry)
	{
		entry->session = session;
		session->conn = entry->conn;
		session->seq = entry->seq++;
		// 1. 如果是HTTP client的话
		// 这里是 CommMessageOut *ComplexHttpTask::message_out()
		// 用于拼凑req请求，自动添加一些字段
		// 2. 如果是HTTP Server的话
		session->out = session->message_out(); 
		// message_out获得的是往连接上要发的数据
		// 接下来send_message 把发出去
		if (session->out)
			ret = this->send_message(entry);

		if (ret < 0)
		{
			entry->error = errno;
			mpoller_del(entry->sockfd, this->mpoller);
			entry->state = CONN_STATE_ERROR;
			ret = 1;
		}

		pthread_mutex_unlock(&entry->mutex);
	}

	return ret;
}

int Communicator::request(CommSession *session, CommTarget *target)
{
	LOG_TRACE("Communicator::request");
	struct CommConnEntry *entry;
	struct poller_data data;
	int errno_bak;
	int timeout;
	int ret;

	// 必须要是客户端
	if (session->passive)
	{
		errno = EINVAL;
		return -1;
	}

	errno_bak = errno;
	session->target = target;
	session->out = NULL;
	session->in = NULL;
	ret = this->request_idle_conn(session, target);
	while (ret < 0)    // todo : why while here
	{
		entry = this->launch_conn(session, target);
		if (entry)
		{
			session->conn = entry->conn;
			session->seq = entry->seq++;
			data.operation = PD_OP_CONNECT;
			data.fd = entry->sockfd;
			data.ssl = NULL;
			data.context = entry;
			timeout = session->connect_timeout();
			if (mpoller_add(&data, timeout, this->mpoller) >= 0)
				break;

			this->release_conn(entry);
		}

		session->conn = NULL;
		session->seq = 0;
		return -1;
	}

	errno = errno_bak;
	return 0;
}

int Communicator::nonblock_listen(CommService *service)
{
	int sockfd = service->create_listen_fd(); // 创建监听套接字,返回管理socket的文件的fd

	if (sockfd >= 0)
	{
		if (__set_fd_nonblock(sockfd) >= 0) // 设置套接字文件为非阻塞模式
		{
			if (__bind_and_listen(sockfd, service->bind_addr,
								  service->addrlen) >= 0)
			{
				return sockfd; // 返回监听成功的套接字描述符
			}
		}

		close(sockfd);
	}

	return -1;
}

// 封装bind和listen
int Communicator::bind(CommService *service)
{
	struct poller_data data;
	int sockfd;

    // 使用nonblock_listen函数尝试创建一个非阻塞的监听套接字
    sockfd = this->nonblock_listen(service);
    if (sockfd >= 0)
    {
        // 如果成功创建了套接字，将它保存在service的listen_fd字段中
		service->listen_fd = sockfd;

        // 设置service的引用计数为1
        service->ref = 1;
        
        // 填充poller_data结构，以便将套接字添加到事件驱动的多路复用器中
		data.operation = PD_OP_LISTEN;
		data.fd = sockfd;
		data.accept = Communicator::accept; // 注册套接字listen完毕后需要执行的回调操作 见 static void __poller_handle_listen(struct __poller_node *node,poller_t *poller)
		data.context = service;				// 见void Communicator::handle_listen_result(struct poller_result *res)
		data.result = NULL;

        // 使用mpoller_add函数将套接字添加到一个指定的poller线程管理的多路复用器中
		if (mpoller_add(&data, service->listen_timeout, this->mpoller) >= 0)
			return 0;

		close(sockfd);
	}

	return -1;
}

void Communicator::unbind(CommService *service)
{
	int errno_bak = errno;

	if (mpoller_del(service->listen_fd, this->mpoller) < 0)
	{
		/* Error occurred on listen_fd or Communicator::deinit() called. */
		this->shutdown_service(service);
		errno = errno_bak;
	}
}

int Communicator::reply_idle_conn(CommSession *session, CommTarget *target)
{
	struct CommConnEntry *entry;
	int ret = -1;

	pthread_mutex_lock(&target->mutex);
	// 检查目标的空闲连接列表是否为空
	if (!list_empty(&target->idle_list))
	{
		// 如果列表不为空，取得列表中的第一个元素，它表示一个空闲的连接
		entry = list_entry(target->idle_list.next, struct CommConnEntry, list);
		
		// 从列表中删除这个元素
		list_del(&entry->list);

		// 获取session将要发送的消息
		session->out = session->message_out();

		// 如果有消息要发送，就发送这个消息
		if (session->out)
			ret = this->send_message(entry);

		// 如果消息发送失败
		if (ret < 0)
		{
			// 记录错误码，删除对应的socket的多路复用器，将连接状态设为错误
			entry->error = errno;
			mpoller_del(entry->sockfd, this->mpoller);
			entry->state = CONN_STATE_ERROR;
			// 将返回值设为1
			ret = 1;
		}
	}
	else
	{
		// 如果目标的空闲连接列表为空，设置错误码为ENOENT
		errno = ENOENT;
	}


	pthread_mutex_unlock(&target->mutex);
	return ret;
}

// 如果当前会话在被动模式（服务器主动连接到客户端），则服务器会进行回复。
int Communicator::reply(CommSession *session)
{
	struct CommConnEntry *entry;
	CommTarget *target;
	int errno_bak;
	int ret;

	// 如果session不在被动模式（即，session->passive 不为1），则设置错误码并返回 -1
	if (session->passive != 1)
	{
		errno = session->passive ? ENOENT : EPERM;
		return -1;
	}

	// 保存当前的错误码
	errno_bak = errno;

	// 设置session为被动模式
	session->passive = 2;

	// 获取目标通信对象
	target = session->target;

	// 对目标进行回复，并检查回复结果
	ret = this->reply_idle_conn(session, target);
	if (ret < 0)
		return -1;

	// 如果回复成功
	if (ret == 0)
	{
		// 获取session的输入入口
		entry = session->in->entry;

		// 处理回复成功的事件
		session->handle(CS_STATE_SUCCESS, 0);

		// 减少entry的引用计数，如果引用计数减为0，则释放连接
		if (__sync_sub_and_fetch(&entry->ref, 1) == 0)
		{
			this->release_conn(entry);
			((CommServiceTarget *)target)->decref();
		}
	}

	// 恢复错误码
	errno = errno_bak;

	return 0;
}

int Communicator::push(const void *buf, size_t size, CommSession *session)
{
	CommTarget *target = session->target;
	struct CommConnEntry *entry;
	int ret;

	if (session->passive != 1)
	{
		errno = session->passive ? ENOENT : EPERM;
		return -1;
	}

	pthread_mutex_lock(&target->mutex);
	if (!list_empty(&target->idle_list))
	{
		entry = list_entry(target->idle_list.next, struct CommConnEntry, list);
		if (!entry->ssl)
			ret = write(entry->sockfd, buf, size);
		else if (size == 0)
			ret = 0;
		else
		{
			ret = SSL_write(entry->ssl, buf, size);
			if (ret <= 0)
			{
				ret = SSL_get_error(entry->ssl, ret);
				if (ret != SSL_ERROR_SYSCALL)
					errno = -ret;

				ret = -1;
			}
		}
	}
	else
	{
		errno = ENOENT;
		ret = -1;
	}

	pthread_mutex_unlock(&target->mutex);
	return ret;
}

int Communicator::sleep(SleepSession *session)
{
	LOG_TRACE("Communicator::sleep");
	struct timespec value;

	// 这里就是调用了__WFTimerTask的duration
	if (session->duration(&value) >= 0)
	{
		if (mpoller_add_timer(&value, session, this->mpoller) >= 0)
			return 0;
	}

	return -1;
}

int Communicator::is_handler_thread() const
{
	return thrdpool_in_pool(this->thrdpool);
}

extern "C" void __thrdpool_schedule(const struct thrdpool_task *, void *,
									thrdpool_t *);

int Communicator::increase_handler_thread()
{
	void *buf = malloc(4 * sizeof (void *));

	if (buf)
	{
		if (thrdpool_increase(this->thrdpool) >= 0)
		{
			struct thrdpool_task task = {
				.routine	=	Communicator::handler_thread_routine,
				.context	=	this
			};
			__thrdpool_schedule(&task, buf, this->thrdpool);
			return 0;
		}

		free(buf);
	}

	return -1;
}

#ifdef __linux__

void Communicator::shutdown_io_service(IOService *service)
{
	pthread_mutex_lock(&service->mutex);
	close(service->event_fd);
	service->event_fd = -1;
	pthread_mutex_unlock(&service->mutex);
	service->decref();
}

int Communicator::io_bind(IOService *service)
{
	struct poller_data data;
	int event_fd;

	event_fd = service->create_event_fd();
	if (event_fd >= 0)
	{
		if (__set_fd_nonblock(event_fd) >= 0)
		{
			service->ref = 1;
			data.operation = PD_OP_EVENT;
			data.fd = event_fd;
			data.event = IOService::aio_finish;
			data.context = service;
			data.result = NULL;
			if (mpoller_add(&data, -1, this->mpoller) >= 0)
			{
				service->event_fd = event_fd;
				return 0;
			}
		}

		close(event_fd);
	}

	return -1;
}

void Communicator::io_unbind(IOService *service)
{
	int errno_bak = errno;

	if (mpoller_del(service->event_fd, this->mpoller) < 0)
	{
		/* Error occurred on event_fd or Communicator::deinit() called. */
		this->shutdown_io_service(service);
		errno = errno_bak;
	}
}

#else

void Communicator::shutdown_io_service(IOService *service)
{
	pthread_mutex_lock(&service->mutex);
	close(service->pipe_fd[0]);
	close(service->pipe_fd[1]);
	service->pipe_fd[0] = -1;
	service->pipe_fd[1] = -1;
	pthread_mutex_unlock(&service->mutex);
	service->decref();
}

int Communicator::io_bind(IOService *service)
{
	struct poller_data data;
	int pipe_fd[2];

	if (service->create_pipe_fd(pipe_fd) >= 0)
	{
		if (__set_fd_nonblock(pipe_fd[0]) >= 0)
		{
			service->ref = 1;
			data.operation = PD_OP_NOTIFY;
			data.fd = pipe_fd[0];
			data.notify = IOService::aio_finish;
			data.context = service;
			data.result = NULL;
			if (mpoller_add(&data, -1, this->mpoller) >= 0)
			{
				service->pipe_fd[0] = pipe_fd[0];
				service->pipe_fd[1] = pipe_fd[1];
				return 0;
			}
		}

		close(pipe_fd[0]);
		close(pipe_fd[1]);
	}

	return -1;
}

void Communicator::io_unbind(IOService *service)
{
	int errno_bak = errno;

	if (mpoller_del(service->pipe_fd[0], this->mpoller) < 0)
	{
		/* Error occurred on pipe_fd or Communicator::deinit() called. */
		this->shutdown_io_service(service);
		errno = errno_bak;
	}
}

#endif

