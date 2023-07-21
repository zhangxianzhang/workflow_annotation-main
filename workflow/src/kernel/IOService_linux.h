/*
  Copyright (c) 2020 Sogou, Inc.

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

#ifndef _IOSERVICE_LINUX_H_
#define _IOSERVICE_LINUX_H_

#include <sys/uio.h>
#include <sys/eventfd.h>
#include <stddef.h>
#include "list.h"

#define IOS_STATE_SUCCESS	0
#define IOS_STATE_ERROR		1

/*
IOSession 是一个抽象基类，定义了一系列用于各种 IO 操作的接口和方法。它的实例通过链接在一起形成一个列表。
*/
class IOSession
{
private:
	// 用于准备IO操作的抽象函数。每个具体类必须实现此函数。
	virtual int prepare() = 0;

	// 用于处理IO操作结果的抽象函数。每个具体类必须实现此函数。
	virtual void handle(int state, int error) = 0;

protected:
// 准备不同类型的IO操作的函数。这些函数都不返回值，也不修改任何成员变量。它们的主要作用是将待执行的操作的所有必要信息打包到内部的 iocb_buf 成员中，以供之后的 IO 操作使用。

    // 函数 prep_pread() 准备执行一个预读取操作。这种操作将数据从指定偏移量的文件中读取到缓冲区。
    // 参数 fd 是待操作的文件描述符，buf 指向接收数据的缓冲区，count 是待读取的字节数，offset 是文件中的偏移量。
	void prep_pread(int fd, void *buf, size_t count, long long offset);

    // 函数 prep_pwrite() 准备执行一个预写入操作。这种操作将数据从缓冲区写入到指定偏移量的文件中。
    // 参数 fd 是待操作的文件描述符，buf 指向包含数据的缓冲区，count 是待写入的字节数，offset 是文件中的偏移量。
	void prep_pwrite(int fd, void *buf, size_t count, long long offset);

    // 函数 prep_preadv() 准备执行一个预读取向量操作。这种操作将数据从指定偏移量的文件中读取到一组缓冲区（即一个 iovec 结构的数组）。
    // 参数 fd 是待操作的文件描述符，iov 指向 iovec 结构的数组，iovcnt 是数组中的元素数量，offset 是文件中的偏移量。
	void prep_preadv(int fd, const struct iovec *iov, int iovcnt, long long offset);

    // 函数 prep_pwritev() 准备执行一个预写入向量操作。这种操作将数据从一组缓冲区（即一个 iovec 结构的数组）写入到指定偏移量的文件中。
    // 参数 fd 是待操作的文件描述符，iov 指向 iovec 结构的数组，iovcnt 是数组中的元素数量，offset 是文件中的偏移量。
	void prep_pwritev(int fd, const struct iovec *iov, int iovcnt, long long offset);

    // 函数 prep_fsync() 准备执行一个文件同步操作。这种操作将文件的数据和元数据都刷新到磁盘上。
    // 参数 fd 是待操作的文件描述符。
	void prep_fsync(int fd);

    // 函数 prep_fdsync() 准备执行一个文件数据同步操作。这种操作只将文件的数据刷新到磁盘上，而不包括元数据。
    // 参数 fd 是待操作的文件描述符。
	void prep_fdsync(int fd);

	// 获取IO操作结果的函数。
	long get_res() const { return this->res; }

private:
	// 用于描述IO操作的IO控制块(iocb)的缓冲区。
	char iocb_buf[64];

	// IO操作的结果。
	long res;

	// 链接所有IO会话实例的链表节点。
	struct list_head list;

public:
	// 虚析构函数。
	virtual ~IOSession() { }

	// 授予IOService和Communicator类友元访问权限。
	friend class IOService;
	friend class Communicator;
};

// IO服务类
class IOService
{
public:
	// 使用最大并发事件数初始化IO服务。
	int init(int maxevents);

	// 反初始化IO服务。
	void deinit();

	// 请求IO操作。
	int request(IOSession *session);

private:
	// 处理停止错误。可以被子类覆盖。
	virtual void handle_stop(int error) { }

	// 子类必须实现此函数以处理解绑定情况。
	virtual void handle_unbound() = 0;

	// 创建事件文件描述符。可以被子类覆盖。
	virtual int create_event_fd()
	{
		return eventfd(0, 0);
	}

private:
	// 指向io_context结构的指针。
	struct io_context *io_ctx;

	// 增加引用计数。
	void incref();

	// 减少引用计数。
	void decref();

private:
	// 用于事件通知的文件描述符。
	int event_fd;

	// 引用计数。
	int ref;

	// IO会话实例的链表。
	struct list_head session_list;

	// 用于线程安全的互斥锁。
	pthread_mutex_t mutex;

private:
	// 用于处理完成的IO操作的函数。
	static void *aio_finish(void *context);

public:
	// 虚析构函数。
	virtual ~IOService() { }

	// 授予Communicator类友元访问权限。
	friend class Communicator;
};

#endif

