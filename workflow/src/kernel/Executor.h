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

#ifndef _EXECUTOR_H_
#define _EXECUTOR_H_

#include <stddef.h>
#include <pthread.h>
#include "list.h"
#include "logger.h"

/*
 * ExecQueue 类是一个任务队列，用于存储和管理待执行的任务。
 */
class ExecQueue
{
public:
    /*
     * init 函数用于初始化 ExecQueue，创建一个空的任务列表，并初始化互斥锁。
     */
	int init();  

    /*
     * deinit 函数用于清理 ExecQueue，销毁互斥锁。
     */
	void deinit();

private:
    /*
     * task_list 是一个列表，用于存储待执行的任务。列表中的每个元素是一个 ExecSessionEntry 结构体，
     * 包含了一个任务（即 ExecSession）和任务的执行环境（即线程池 thrdpool_t）。
     */
	struct list_head task_list;
	pthread_mutex_t mutex;

public:
	ExecQueue() { LOG_TRACE("ExecQueue creator"); }
	virtual ~ExecQueue() { }
	friend class Executor;
};

#define ES_STATE_FINISHED	0
#define ES_STATE_ERROR		1
#define ES_STATE_CANCELED	2

/*
 * ExecSession 是一个抽象基类，代表了一项可以在 Executor 环境中执行的任务。
 * 每个具体的任务都需要继承这个类，并实现 execute 和 handle 方法。
 */
class ExecSession
{
private:
    /*
     * execute 是一个纯虚函数，需要被子类实现。这个函数中包含了任务的具体执行逻辑。
     */
	virtual void execute() = 0;

    /*
     * handle 是一个纯虚函数，需要被子类实现。这个函数用于处理任务执行完毕后的状态和错误信息。
     */
	virtual void handle(int state, int error) = 0;

protected:
    /*
     * get_queue 函数返回当前任务所在的队列。
     */
	ExecQueue *get_queue() { return this->queue; }

private:
    /*
     * queue 是一个指向 ExecQueue 对象的指针，代表了当前任务所在的队列。
     */
	ExecQueue *queue;  

public:
	ExecSession() { LOG_TRACE("ExecSession creator"); }
	virtual ~ExecSession() { }
	friend class Executor;  // Executor可以直接access这里的private queue
};

/*
 负责管理和调度任务
*/
/*
 * Executor 类是一个执行器类，负责管理线程池，并执行提交给它的任务。
 */
class Executor
{
public:
    /*
     * init 函数用于初始化 Executor，创建一个包含 nthreads 个线程的线程池。
     */
	int init(size_t nthreads); 

    /*
     * deinit 函数用于清理 Executor，销毁线程池。
     */
	void deinit();  

    /*
     * Executor的生产接口：request()。request 函数用于提交一个任务到指定的队列中。该任务随后会被线程池中的一个线程执行
     */
	int request(ExecSession *session, ExecQueue *queue);

private:
    /*
     * thrdpool 是一个指向 __thrdpool 结构体的指针，代表了 Executor 管理的线程池。
     */
	struct __thrdpool *thrdpool;

//类静态成员函数见笔记
private:
    /*
     * Executor的消费接口：routine()。executor_thread_routine 是一个静态函数，代表了线程池中线程的执行逻辑。
     */
	static void executor_thread_routine(void *context);

    /*
     * executor_cancel 是一个静态函数，用于取消所有还未执行的任务。
     */
	static void executor_cancel_tasks(const struct thrdpool_task *task);

public:
	Executor() { LOG_TRACE("Executor creator"); }
	virtual ~Executor() { }
};

#endif

