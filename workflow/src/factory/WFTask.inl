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

template<class REQ, class RESP>
int WFNetworkTask<REQ, RESP>::get_peer_addr(struct sockaddr *addr,
											socklen_t *addrlen) const
{
	const struct sockaddr *p;
	socklen_t len;

	if (this->target)
	{
		this->target->get_addr(&p, &len);
		if (*addrlen >= len)
		{
			memcpy(addr, p, len);
			*addrlen = len;
			return 0;
		}

		errno = ENOBUFS;
	}
	else
		errno = ENOTCONN;

	return -1;
}

template<class REQ, class RESP>
class WFClientTask : public WFNetworkTask<REQ, RESP>
{
protected:
	virtual CommMessageOut *message_out()
	{
		LOG_TRACE("WFClientTask message_out");
		/* By using prepare function, users can modify request after
		 * the connection is established. */
		if (this->prepare)
			this->prepare(this);

		return &this->req;
	}

	virtual CommMessageIn *message_in() { 
		LOG_TRACE("WFClientTask message_in");
		return &this->resp; 
	}

protected:
	virtual WFConnection *get_connection() const
	{
		LOG_TRACE("WFClientTask get_connection");
		CommConnection *conn;

		if (this->target)
		{
			conn = this->CommSession::get_connection();
			if (conn)
				return (WFConnection *)conn;
		}

		errno = ENOTCONN;
		return NULL;
	}

public:
	void set_prepare(std::function<void (WFNetworkTask<REQ, RESP> *)> prep)
	{
		LOG_TRACE("WFClientTask set_prepare");
		this->prepare = std::move(prep);
	}

protected:
	std::function<void (WFNetworkTask<REQ, RESP> *)> prepare;

public:
	WFClientTask(CommSchedObject *object, CommScheduler *scheduler,
				 std::function<void (WFNetworkTask<REQ, RESP> *)>&& cb) :
		WFNetworkTask<REQ, RESP>(object, scheduler, std::move(cb))
	{
		LOG_TRACE("WFClientTask create");
	}

protected:
	virtual ~WFClientTask() { }
};

/*
此类是 WFNetworkTask 的特化版本，特别用于处理服务器任务。
它包含了用于处理请求和响应的方法，处理任务的调度，以及获取与任务相关联的连接。
此外，它还定义了内部类 Processor 和 Series，用于处理特定的子任务和管理任务序列
*/
template<class REQ, class RESP>
class WFServerTask : public WFNetworkTask<REQ, RESP>
{
protected:
    // 为了获取响应消息，返回响应的引用
	virtual CommMessageOut *message_out() { return &this->resp; }

    // 为了获取请求消息，返回请求的引用
	virtual CommMessageIn *message_in() { return &this->req; }

    // 处理任务的主要逻辑，根据状态和错误调用内部处理器类，处理任务处理
	virtual void handle(int state, int error);

protected:
    // 用于任务调度，根据任务状态进行不同的处理
	virtual void dispatch()
	{
		if (this->state == WFT_STATE_TOREPLY)
		{
			/* Enable get_connection() again if the reply() call is success. */
			this->processor.task = this;
			if (this->scheduler->reply(this) >= 0)
				return;

			this->state = WFT_STATE_SYS_ERROR;
			this->error = errno;
			this->processor.task = NULL;
		}

		this->subtask_done();
	}

	/* CommSession::get_connection() is supposed to be called only in the
	 * implementations of it's virtual functions. As a server task, to call
	 * this function after process() and before callback() is very dangerous
	 * and should be blocked. */
	// 获取与任务相关联的连接
	virtual WFConnection *get_connection() const
	{
		if (this->processor.task)
			return (WFConnection *)this->CommSession::get_connection();

		errno = EPERM;
		return NULL;
	}

protected:
	CommService *service;	// 任务所属的服务端

protected:
	// 内部处理器类，处理任务
	class Processor : public SubTask
	{
	public:
		Processor(WFServerTask<REQ, RESP> *task,
				  std::function<void (WFNetworkTask<REQ, RESP> *)>& proc) :
			process(proc)
		{
			this->task = task;
		}

		// 处理任务调度
		virtual void dispatch()
		{
			this->process(this->task);
			this->task = NULL;	/* As a flag. get_conneciton() disabled. */
			this->subtask_done();
		}

		// 处理完成的任务
		virtual SubTask *done()
		{
			return series_of(this)->pop();
		}

		std::function<void (WFNetworkTask<REQ, RESP> *)>& process; // 处理任务的函数
		WFServerTask<REQ, RESP> *task; // 要处理的任务
	} processor;

    // 任务序列类，用于管理任务序列
	class Series : public SeriesWork
	{
	public:
		Series(WFServerTask<REQ, RESP> *task) :
			SeriesWork(&task->processor, nullptr)
		{
			this->set_last_task(task);
			this->service = task->service;
			this->service->incref();
		}

		virtual ~Series()
		{
			this->callback = nullptr;
			this->service->decref();
		}

		CommService *service; // 任务序列所属的服务端
	};

public:
	// 任务构造函数，接受服务端，调度器和处理函数作为参数
	WFServerTask(CommService *service, CommScheduler *scheduler,
				 std::function<void (WFNetworkTask<REQ, RESP> *)>& proc) :
		WFNetworkTask<REQ, RESP>(NULL, scheduler, nullptr),
		processor(this, proc)
	{
		this->service = service;
	}

protected:
	virtual ~WFServerTask() { }
};

template<class REQ, class RESP>
void WFServerTask<REQ, RESP>::handle(int state, int error)
{
    // 如果输入状态是WFT_STATE_TOREPLY，表示任务已经处理完成，需要回复
	if (state == WFT_STATE_TOREPLY)
	{
		this->state = WFT_STATE_TOREPLY; // 将当前任务状态设置为需要回复
		this->target = this->get_target(); // 获取任务目标
		new Series(this); // 创建一个新的任务序列
		this->processor.dispatch(); // 开始执行任务处理
	}
	// 如果当前任务状态是WFT_STATE_TOREPLY，但输入的状态不是，表示在任务处理过程中发生了错误
	else if (this->state == WFT_STATE_TOREPLY)
	{
		this->state = state; // 更新当前任务状态为输入的状态
		this->error = error; // 更新当前任务错误为输入的错误
		if (error == ETIMEDOUT) // 如果错误是超时错误
			this->timeout_reason = TOR_TRANSMIT_TIMEOUT; // 设置超时原因为传输超时

		this->subtask_done(); // 完成子任务处理
	}
    // 如果输入的状态和当前状态都不是WFT_STATE_TOREPLY，表示这个任务已经不需要处理，直接删除
	else
		delete this;
}

