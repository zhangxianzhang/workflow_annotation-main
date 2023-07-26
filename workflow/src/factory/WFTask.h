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

#ifndef _WFTASK_H_
#define _WFTASK_H_

#include <errno.h>
#include <string.h>
#include <assert.h>
#include <atomic>
#include <utility>
#include <functional>
#include "Executor.h"
#include "ExecRequest.h"
#include "Communicator.h"
#include "CommScheduler.h"
#include "CommRequest.h"
#include "SleepRequest.h"
#include "IORequest.h"
#include "Workflow.h"
#include "WFConnection.h"
#include "logger.h"

// task->get_state() 
// state代表任务的结束状态
enum
{
	WFT_STATE_UNDEFINED = -1,
	WFT_STATE_SUCCESS = CS_STATE_SUCCESS,  // 任务成功。client接收到完整的回复，或server把回复完全写进入发送缓冲（但不能确保对方一定能收到）。
	WFT_STATE_TOREPLY = CS_STATE_TOREPLY,		/* for server task only server 任务回复之前，没有被调用过task->noreply()，都是TOREPLY状态 */
	WFT_STATE_NOREPLY = CS_STATE_TOREPLY + 1,	/* for server task only  server任务被调用了task->noreply()之后，一直是NOREPLY状态。callback里也是这个状态。连接会被关闭。 */
	WFT_STATE_SYS_ERROR = CS_STATE_ERROR,   // 系统错误。这种情况，task->get_error()得到的是系统错误码errno。
											// 当get_error()得到ETIMEDOUT，可以调用task->get_timeout_reason()进一步得到超时原因

	WFT_STATE_SSL_ERROR = 65,				// SSL错误。get_error()得到的是SSL_get_error()的返回值
											// 目前SSL错误信息没有做得很全，得不到ERR_get_error()的值。
											// 所以，基本上get_error()返回值也就三个可能 : SSL_ERROR_ZERO_RETURN, SSL_ERROR_X509_LOOKUP, SSL_ERROR_SSL。

	WFT_STATE_DNS_ERROR = 66,				/* for client task only */   
											// DNS解析错误。get_error()得到的是getaddrinfo()调用的返回码
											// https://github.com/sogou/workflow/blob/master/docs/about-error.md
											
	WFT_STATE_TASK_ERROR = 67,  			// 任务错误。常见的例如URL不合法，登录失败等
	WFT_STATE_ABORTED = CS_STATE_STOPPED		/* main process terminated */
};

template<class INPUT, class OUTPUT>
class WFThreadTask : public ExecRequest
{
public:
	void start()
	{
		assert(!series_of(this));
		Workflow::start_series_work(this, nullptr);
	}

	void dismiss()
	{
		assert(!series_of(this));
		delete this;
	}

public:
	INPUT *get_input() { return &this->input; }
	OUTPUT *get_output() { return &this->output; }

public:
	void *user_data;

public:
	int get_state() const { return this->state; }
	int get_error() const { return this->error; }

public:
	void set_callback(std::function<void (WFThreadTask<INPUT, OUTPUT> *)> cb)
	{
		this->callback = std::move(cb);
	}

protected:
	virtual SubTask *done()
	{
		LOG_INFO("WFThreadTask done");
		SeriesWork *series = series_of(this);

		if (this->callback)
			this->callback(this);

		delete this;
		return series->pop();
	}

protected:
	INPUT input;
	OUTPUT output;
	std::function<void (WFThreadTask<INPUT, OUTPUT> *)> callback;

public:
	WFThreadTask(ExecQueue *queue, Executor *executor,
				 std::function<void (WFThreadTask<INPUT, OUTPUT> *)>&& cb) :
		ExecRequest(queue, executor),
		callback(std::move(cb))
	{
		LOG_TRACE("WFThreadTask constructor");
		this->user_data = NULL;
		this->state = WFT_STATE_UNDEFINED;
		this->error = 0;
	}

protected:
	virtual ~WFThreadTask() { }
};

template<class INPUT, class OUTPUT>
class WFMultiThreadTask : public ParallelTask
{
public:
	void start()
	{
		assert(!series_of(this));
		Workflow::start_series_work(this, nullptr);
	}

	void dismiss()
	{
		assert(!series_of(this));
		delete this;
	}

public:
	INPUT *get_input(size_t index)
	{
		return static_cast<Thread *>(this->subtasks[index])->get_input();
	}

	OUTPUT *get_output(size_t index)
	{
		return static_cast<Thread *>(this->subtasks[index])->get_output();
	}

public:
	void *user_data;

public:
	int get_state(size_t index) const
	{
		return static_cast<const Thread *>(this->subtasks[index])->get_state();
	}

	int get_error(size_t index) const
	{
		return static_cast<const Thread *>(this->subtasks[index])->get_error();
	}

public:
	void set_callback(
		std::function<void (WFMultiThreadTask<INPUT, OUTPUT> *)> cb)
	{
		this->callback = std::move(cb);
	}

protected:
	virtual SubTask *done()
	{
		SeriesWork *series = series_of(this);

		if (this->callback)
			this->callback(this);

		delete this;
		return series->pop();
	}

protected:
	std::function<void (WFMultiThreadTask<INPUT, OUTPUT> *)> callback;

protected:
	using Thread = WFThreadTask<INPUT, OUTPUT>;

public:
	WFMultiThreadTask(Thread *const tasks[], size_t n,
			std::function<void (WFMultiThreadTask<INPUT, OUTPUT> *)>&& cb) :
		ParallelTask(new SubTask *[n], n),
		callback(std::move(cb))
	{
		size_t i;

		for (i = 0; i < n; i++)
			this->subtasks[i] = tasks[i];

		this->user_data = NULL;
	}

protected:
	virtual ~WFMultiThreadTask()
	{
		size_t n = this->subtasks_nr;

		while (n > 0)
			delete this->subtasks[--n];

		delete []this->subtasks;
	}
};

/* WFNetworkTask 是一个模板类，是一个网络任务，这个类继承自CommRequest类。
这个类主要封装了一个网络任务的操作，包括启动、取消、设置超时时间、设置回调函数等。
其中的 start、dismiss、noreply、push 等函数有特殊的使用场景，如只在客户端或服务器任务中调用。
*/
template<class REQ, class RESP>
class WFNetworkTask : public CommRequest
{
public:
    // 启动网络任务，这个函数只在客户端任务中调用。
    void start()
    {
        assert(!series_of(this)); // 断言这个任务不在任务序列中。
        Workflow::start_series_work(this, nullptr); // 启动这个任务。
    }

    // 取消网络任务，这个函数只在客户端任务中调用。
    void dismiss()
    {
        assert(!series_of(this)); // 断言这个任务不在任务序列中。
        delete this; // 删除这个任务。
    }

public:
    // 获取请求对象。
    REQ *get_req() { return &this->req; }
    // 获取响应对象。
    RESP *get_resp() { return &this->resp; }

public:
    // 用户数据，可以用于存储任务相关的用户数据。
    void *user_data;

public:
    // 获取任务的状态。
    int get_state() const { return this->state; }
    // 获取任务的错误代码。
    int get_error() const { return this->error; }

    // 获取任务超时的原因。
    int get_timeout_reason() const { return this->timeout_reason; }

    // 获取任务的序列号，只在回调函数或服务器的处理函数中调用。
    long long get_task_seq() const
    {
        if (!this->target) // 如果目标不存在，返回错误。
        {
            errno = ENOTCONN;
            return -1;
        }

        return this->get_seq(); // 返回任务的序列号。
    }

    // 获取对等方的地址，返回值是成功获取的地址的长度，如果失败，返回-1。
    int get_peer_addr(struct sockaddr *addr, socklen_t *addrlen) const;

    // 获取网络连接，这是一个纯虚函数，由派生类实现。
    virtual WFConnection *get_connection() const = 0;

public:
    // 设置发送超时时间（毫秒），-1表示无限超时。
    void set_send_timeout(int timeout) { this->send_timeo = timeout; }
    // 设置接收超时时间（毫秒），-1表示无限超时。
    void set_receive_timeout(int timeout) { this->receive_timeo = timeout; }
    // 设置保持连接的时间（毫秒）。
    void set_keep_alive(int timeout) { this->keep_alive_timeo = timeout; }

public:
    // 不回复，这个函数只在服务器任务中调用。
    void noreply()
    {
        if (this->state == WFT_STATE_TOREPLY)
            this->state = WFT_STATE_NOREPLY;
    }

    // 推送数据，这个函数只在服务器任务中调用。
    virtual int push(const void *buf, size_t size)
    {
        return this->scheduler->push(buf, size, this);
    }

public:
    // 设置任务的回调函数。
    void set_callback(std::function<void (WFNetworkTask<REQ, RESP> *)> cb)
    {
        this->callback = std::move(cb);
    }

protected:
    // 获取发送超时时间。
    virtual int send_timeout() { return this->send_timeo; }
    // 获取接收超时时间。
    virtual int receive_timeout() { return this->receive_timeo; }
    // 获取保持连接的时间。
    virtual int keep_alive_timeout() { return this->keep_alive_timeo; }

protected:
    // 完成任务。
    virtual SubTask *done()
    {
        SeriesWork *series = series_of(this);

        if (this->state == WFT_STATE_SYS_ERROR && this->error < 0)
        {
            this->state = WFT_STATE_SSL_ERROR;
            this->error = -this->error;
        }

        if (this->callback) // 如果设置了回调函数，调用回调函数。
            this->callback(this);

        delete this; // 删除任务。
        return series->pop(); // 返回任务序列的下一个任务。
    }

protected:
    // 发送超时时间。
    int send_timeo;
    // 接收超时时间。
    int receive_timeo;
    // 保持连接的时间。
    int keep_alive_timeo;
    // 请求对象。
    REQ req;
    // 响应对象。
    RESP resp;
    // 任务的回调函数。
    std::function<void (WFNetworkTask<REQ, RESP> *)> callback;

protected:
    // 构造函数。
    WFNetworkTask(CommSchedObject *object, CommScheduler *scheduler,
                  std::function<void (WFNetworkTask<REQ, RESP> *)>&& cb) :
        CommRequest(object, scheduler),
        callback(std::move(cb))
    {
        LOG_TRACE("WFNetworkTask create"); // 记录创建任务的日志。
        this->user_data = NULL;
        this->send_timeo = -1;
        this->receive_timeo = -1;
        this->keep_alive_timeo = 0;
        this->target = NULL;
        this->timeout_reason = TOR_NOT_TIMEOUT;
        this->state = WFT_STATE_UNDEFINED;
        this->error = 0;
    }

    // 析构函数。
    virtual ~WFNetworkTask() { }
};
class WFTimerTask : public SleepRequest
{
public:
	void start()
	{
		assert(!series_of(this));
		Workflow::start_series_work(this, nullptr);
	}

	void dismiss()
	{
		assert(!series_of(this));
		delete this;
	}

public:
	void *user_data;

public:
	int get_state() const { return this->state; }
	int get_error() const { return this->error; }

public:
	void set_callback(std::function<void (WFTimerTask *)> cb)
	{
		this->callback = std::move(cb);
	}

protected:
	virtual SubTask *done()
	{
		SeriesWork *series = series_of(this);

		if (this->callback)
			this->callback(this);

		delete this;
		return series->pop();
	}

protected:
	std::function<void (WFTimerTask *)> callback;

public:
	WFTimerTask(CommScheduler *scheduler,
				std::function<void (WFTimerTask *)> cb) :
		SleepRequest(scheduler),
		callback(std::move(cb))
	{
		LOG_TRACE("WFTimerTask creator");
		this->user_data = NULL;
		this->state = WFT_STATE_UNDEFINED;
		this->error = 0;
	}

protected:
	virtual ~WFTimerTask() { }
};

template<class ARGS>
class WFFileTask : public IORequest
{
public:
	void start()
	{
		assert(!series_of(this));
		Workflow::start_series_work(this, nullptr);
	}

	void dismiss()
	{
		assert(!series_of(this));
		delete this;
	}

public:
	ARGS *get_args() { return &this->args; }

	long get_retval() const
	{
		if (this->state == WFT_STATE_SUCCESS)
			return this->get_res();
		else
			return -1;
	}

public:
	void *user_data;

public:
	int get_state() const { return this->state; }
	int get_error() const { return this->error; }

public:
	void set_callback(std::function<void (WFFileTask<ARGS> *)> cb)
	{
		this->callback = std::move(cb);
	}

protected:
	virtual SubTask *done()
	{
		SeriesWork *series = series_of(this);

		if (this->callback)
			this->callback(this);

		delete this;
		return series->pop();
	}

protected:
	ARGS args;
	std::function<void (WFFileTask<ARGS> *)> callback;

public:
	WFFileTask(IOService *service,
			   std::function<void (WFFileTask<ARGS> *)>&& cb) :
		IORequest(service),
		callback(std::move(cb))
	{
		this->user_data = NULL;
		this->state = WFT_STATE_UNDEFINED;
		this->error = 0;
	}

protected:
	virtual ~WFFileTask() { }
};

// 通用任务类 
class WFGenericTask : public SubTask
{
public:
	// 开始任务
	void start()
	{
		assert(!series_of(this));
		Workflow::start_series_work(this, nullptr);
	}

	// 终止任务
	void dismiss()
	{
		assert(!series_of(this));
		delete this;
	}

public:
	void *user_data; // 用户数据指针

public:
	// 获取状态
	int get_state() const { return this->state; }
	// 获取错误码
	int get_error() const { return this->error; }

protected:
	// 开始任务，调用子任务完成方法
	virtual void dispatch()
	{
		this->subtask_done();
	}

	// 任务完成，从系列任务中移除，并删除自身
	virtual SubTask *done()
	{
		SeriesWork *series = series_of(this);
		delete this;
		return series->pop();
	}

protected:
    int state;  // 任务状态
    int error;  // 任务错误码

public:
	WFGenericTask()
	{
		this->user_data = NULL;
		this->state = WFT_STATE_UNDEFINED;
		this->error = 0;
	}

protected:
	virtual ~WFGenericTask() { }
};

// 计数任务类，继承自通用任务类
class WFCounterTask : public WFGenericTask
{
public:
	// 每当有一个子任务开始时，都会调用 count() 方法使计数器减一，并在计数为0时调用子任务完成方法
	virtual void count()
	{
		if (--this->value == 0)
		{
			this->state = WFT_STATE_SUCCESS;
			this->subtask_done();
		}
	}

public:
	void set_callback(std::function<void (WFCounterTask *)> cb)
	{
		this->callback = std::move(cb);
	}

protected:
	// 调用 count 方法来开始子任务
	virtual void dispatch()
	{
		this->WFCounterTask::count();
	}

	// 任务完成，如果有回调函数则调用，然后删除自身
	virtual SubTask *done()
	{
		SeriesWork *series = series_of(this);

		if (this->callback)
			this->callback(this);

		delete this;
		return series->pop();
	}

protected:
	std::atomic<unsigned int> value;  // 计数值
	std::function<void (WFCounterTask *)> callback;  // 回调函数

public:
	WFCounterTask(unsigned int target_value,
				  std::function<void (WFCounterTask *)>&& cb) :
		value(target_value + 1), // +1 是因为 WFCounterTask 自身也是一个任务
		callback(std::move(cb))
	{
	}

protected:
	virtual ~WFCounterTask() { }
};

class WFMailboxTask : public WFGenericTask
{
public:
	void send(void *msg)
	{
		*this->next++ = msg;
		this->count();
	}

	void **get_mailbox(size_t *n)
	{
		*n = this->next - this->mailbox;
		return this->mailbox;
	}

public:
	void set_callback(std::function<void (WFMailboxTask *)> cb)
	{
		this->callback = std::move(cb);
	}

public:
	virtual void count()
	{
		if (--this->value == 0)
		{
			this->state = WFT_STATE_SUCCESS;
			this->subtask_done();
		}
	}

protected:
	virtual void dispatch()
	{
		this->WFMailboxTask::count();
	}

	virtual SubTask *done()
	{
		SeriesWork *series = series_of(this);

		if (this->callback)
			this->callback(this);

		delete this;
		return series->pop();
	}

protected:
	void **mailbox;
	std::atomic<void **> next;
	std::atomic<size_t> value;
	std::function<void (WFMailboxTask *)> callback;

public:
	WFMailboxTask(void **mailbox, size_t size,
				  std::function<void (WFMailboxTask *)>&& cb) :
		next(mailbox),
		value(size + 1),
		callback(std::move(cb))
	{
		this->mailbox = mailbox;
	}

	WFMailboxTask(std::function<void (WFMailboxTask *)>&& cb) :
		next(&this->user_data),
		value(2),
		callback(std::move(cb))
	{
		this->mailbox = &this->user_data;
	}

protected:
	virtual ~WFMailboxTask() { }
};

class WFConditional : public WFGenericTask
{
public:
	virtual void signal(void *msg)
	{
		LOG_TRACE("WFConditional signal");
		*this->msgbuf = msg;
		// https://stackoverflow.com/questions/7007834/what-is-the-use-case-for-the-atomic-exchange-read-write-operation
		// Return value
		// The value of the atomic variable before the call
		// 所以第二次才exchange 才走到 subtask_done
		if (this->flag.exchange(true))
			this->subtask_done();
	}

protected:
	virtual void dispatch()
	{
		LOG_TRACE("WFConditional dispatch");
		series_of(this)->push_front(this->task);
		this->task = NULL;  
		if (this->flag.exchange(true))
			this->subtask_done();
	}

protected:
	std::atomic<bool> flag;
	SubTask *task;
	void **msgbuf;

public:
	WFConditional(SubTask *task, void **msgbuf) :
		flag(false)
	{
		this->task = task;
		this->msgbuf = msgbuf;
	}

	WFConditional(SubTask *task) :
		flag(false)
	{
		this->task = task;
		this->msgbuf = &this->user_data;
	}

protected:
	virtual ~WFConditional()
	{
		delete this->task;
	}
};

class WFGoTask : public ExecRequest
{
public:
	void start()
	{
		assert(!series_of(this));
		Workflow::start_series_work(this, nullptr);
	}

	void dismiss()
	{
		assert(!series_of(this));
		delete this;
	}

public:
	void *user_data;

public:
	int get_state() const { return this->state; }
	int get_error() const { return this->error; }

public:
	void set_callback(std::function<void (WFGoTask *)> cb)
	{
		this->callback = std::move(cb);
	}

protected:
	virtual SubTask *done()
	{
		SeriesWork *series = series_of(this);

		if (this->callback)
			this->callback(this);

		delete this;
		return series->pop();
	}

protected:
	std::function<void (WFGoTask *)> callback;

public:
	WFGoTask(ExecQueue *queue, Executor *executor) :
		ExecRequest(queue, executor)
	{
		this->user_data = NULL;
		this->state = WFT_STATE_UNDEFINED;
		this->error = 0;
	}

protected:
	virtual ~WFGoTask() { }
};

#include "WFTask.inl"

#endif

