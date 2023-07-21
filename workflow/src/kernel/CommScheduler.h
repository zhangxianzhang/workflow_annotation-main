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

#ifndef _COMMSCHEDULER_H_
#define _COMMSCHEDULER_H_

#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>
#include <openssl/ssl.h>
// 引入自定义的Communicator类
#include "Communicator.h"
#include "logger.h"

// 通信调度对象类
class CommSchedObject
{
public:
	// 获取最大负载量
	size_t get_max_load() const { return this->max_load; }
	// 获取当前负载量
	size_t get_cur_load() const { return this->cur_load; }

private:
	// 这里是纯虚函数，子类需要实现以获取通信目标。他的两个子类 CommSchedTarget，CommSchedGroup 实现
	virtual CommTarget *acquire(int wait_timeout) = 0;

protected:
	// 最大负载量
	size_t max_load;
	// 当前负载量
	size_t cur_load;

public:
	// 虚析构函数，使得子类可以通过基类指针进行析构
	virtual ~CommSchedObject() { }
	// 声明CommScheduler为友元类，以便它可以访问CommSchedObject的私有和保护成员
	friend class CommScheduler;
};

// 声明通信调度组类
class CommSchedGroup;

// 通信调度目标类，继承自CommSchedObject和CommTarget
class CommSchedTarget : public CommSchedObject, public CommTarget
{
public:
	int init(const struct sockaddr *addr, socklen_t addrlen,
			 int connect_timeout, int response_timeout,
			 size_t max_connections);
	void deinit();

public:
	// 支持SSL的初始化函数
	int init(const struct sockaddr *addr, socklen_t addrlen, SSL_CTX *ssl_ctx,
			 int connect_timeout, int ssl_connect_timeout, int response_timeout,
			 size_t max_connections)
	{
		int ret = this->init(addr, addrlen, connect_timeout, response_timeout,
							 max_connections);

		if (ret >= 0)
			this->set_ssl(ssl_ctx, ssl_connect_timeout);

		return ret;
	}

private:
	// 实现父类的acquire和release虚函数
	virtual CommTarget *acquire(int wait_timeout); /* final */
	virtual void release(int keep_alive); /* final */

private:
	// 调度组的指针
	CommSchedGroup *group;
	// 索引
	int index;
	// 等待计数
	int wait_cnt;
	// 线程互斥锁
	pthread_mutex_t mutex;
	// 线程条件变量
	pthread_cond_t cond;
	// 声明CommSchedGroup为友元类，以便它可以访问CommSchedTarget的私有和保护成员
	friend class CommSchedGroup;
};

// 通信调度组类，继承自CommSchedObject
class CommSchedGroup : public CommSchedObject
{
public:
	// 初始化函数
	int init();
	// 释放资源函数
	void deinit();
	// 添加调度目标函数
	int add(CommSchedTarget *target);
	// 删除调度目标函数
	int remove(CommSchedTarget *target);

private:
	// 实现父类的acquire虚函数
	virtual CommTarget *acquire(int wait_timeout); /* final */

private:
	// 调度目标数组
	CommSchedTarget **tg_heap;
	// 堆大小
	int heap_size;
	// 堆缓冲区大小
	int heap_buf_size;
	// 等待计数
	int wait_cnt;
	// 线程互斥锁
	pthread_mutex_t mutex;
	// 线程条件变量
	pthread_cond_t cond;

private:
	// 比较两个调度目标的静态函数
	static int target_cmp(CommSchedTarget *target1, CommSchedTarget *target2);
	// 堆调整函数
	void heapify(int top);
	void heap_adjust(int index, int swap_on_equal);
	int heap_insert(CommSchedTarget *target);
	void heap_remove(int index);
	// 声明CommSchedTarget为友元类，以便它可以访问CommSchedGroup的私有和保护成员
	friend class CommSchedTarget;
};


// 仅有一个成员变量Communicator，Communicator类和CommScheduler类是组合（Composition）关系
// 对于Communicator来说就是对外封装了一层，提供了更高级别的服务，比如任务调度，使用Communicator的功能，但隐藏了其复杂性
// 加入了一些逻辑操作，本质上都是comm的操作
// 通信调度器类
class CommScheduler
{
public:
	/* 初始化通信调度器 */
	int init(size_t poller_threads, size_t handler_threads)
	{
		LOG_TRACE("CommScheduler::init");
		return this->comm.init(poller_threads, handler_threads);
	}
	
	// 释放资源函数
	void deinit()
	{
		this->comm.deinit();
	}

	/* 请求处理函数：wait_timeout in milliseconds, -1 for no timeout. */
	int request(CommSession *session, CommSchedObject *object,
				int wait_timeout, CommTarget **target)
	{
		LOG_TRACE("CommScheduler request");
		int ret = -1;
		// 就做两件事
		// 1. 获取target
		*target = object->acquire(wait_timeout);
		if (*target)
		{
			// 向target发送request
			ret = this->comm.request(session, *target);
			if (ret < 0)
				(*target)->release(0);
		}

		return ret;
	}

	/* 回应处理函数：for services. */
	int reply(CommSession *session)
	{
		return this->comm.reply(session);
	}

	// 数据推送函数
	int push(const void *buf, size_t size, CommSession *session)
	{
		return this->comm.push(buf, size, session);
	}

	// 绑定通信服务函数
	// 这里的service为了产生sockfd
	int bind(CommService *service)
	{
		return this->comm.bind(service);
	}

	// 解绑通信服务函数
	void unbind(CommService *service)
	{
		this->comm.unbind(service);
	}

	// 睡眠函数
	/* for sleepers. */
	int sleep(SleepSession *session)
	{
		LOG_TRACE("CommScheduler sleep");
		return this->comm.sleep(session);
	}

	/* 绑定文件异步IO服务函数 for file aio services. */
	int io_bind(IOService *service)
	{
		return this->comm.io_bind(service);
	}

	// 解绑文件异步IO服务函数
	void io_unbind(IOService *service)
	{
		this->comm.io_unbind(service);
	}

public:
	// 判断当前线程是否为处理线程（// 通过key区分这个线程是否是线程池创建的）
	int is_handler_thread() const
	{
		return this->comm.is_handler_thread();
	}

	// 给线程池扩容，增加处理线程
	int increase_handler_thread()
	{
		return this->comm.increase_handler_thread();
	}

private:

/*
在CommScheduler类中，`Communicator comm;` 是作为一个对象（值类型）存在的，而不是作为一个指针存在的。当类的对象创建时，`Communicator` 的对象也会被创建，并且在类的对象销毁时，`Communicator` 的对象也会被销毁。这种方式对于简单类型和一些不涉及资源管理的复杂类型的成员变量来说，更加方便和安全。

如果 `Communicator` 是指针，那么 `CommScheduler` 必须在构造函数中分配内存，并在析构函数中释放内存。这就涉及到内存管理，也会有可能因为忘记释放内存导致内存泄露。此外，如果有一个函数试图删除这个指针，可能会导致 `CommScheduler` 使用一个已经被删除的对象，从而引发运行时错误。

另外，使用对象（值类型）而不是指针可以避免一些与指针相关的常见问题，如空指针引用，悬空指针等。

然而，使用指针也有其优点，比如动态分配和删除对象，实现动态绑定和多态性等。在某些情况下，如类的成员是一个大对象，或者成员的存在有条件（比如可能存在，也可能不存在），或者需要共享数据等，使用指针可能是一个更好的选择。选择使用哪种方式，主要取决于特定的需求和场景。
*/
	// Communicator类实例
	Communicator comm;

public:
	virtual ~CommScheduler() { }
};

#endif
