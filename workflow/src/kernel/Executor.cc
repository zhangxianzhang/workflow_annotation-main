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

#include <errno.h>
#include <stdlib.h>
#include <pthread.h>
#include "list.h"
#include "thrdpool.h"
#include "Executor.h"
#include "logger.h"

/**
 * @brief  `ExecSessionEntry`的主要作用是封装一项任务（`ExecSession`）和执行该任务的线程池（`thrdpool_t`），并且用`list_head list`把多个`ExecSessionEntry`链接起来，形成一个链表。这样就可以在多线程环境下，通过线程池并发地处理这个链表上的任务。

实例化的`ExecSessionEntry`对象会被添加到`ExecQueue`中，等待被线程池中的线程执行。每一个`ExecSessionEntry`对象都包含了一个任务（通过`ExecSession`对象表示）和一个线程池（通过`thrdpool_t`对象表示）。线程池中的线程会按照队列中的顺序，取出任务并执行。执行完成后，会调用`ExecSession`对象的`handle()`函数处理执行结果。

 * @note   
整个流程可以概括为：创建任务 -> 封装任务和线程池到`ExecSessionEntry` -> 添加`ExecSessionEntry`到队列 -> 线程池中的线程执行任务 -> 处理执行结果。

 */
struct ExecTaskEntry
{
	struct list_head list;	// 链表头，用于将此结构体实例链接到其他ExecSessionEntry实例中，形成一个链表。
	ExecSession *session;	// 任务，表示这个ExecSessionEntry实例关联的任务。
	thrdpool_t *thrdpool;	// 执行任务的上下文，表明执行关联任务的线程池。
}; 							// 设计思路见笔记

/**
 * @brief  就是初始化list和mutex
 * @note   
 * @retval 
 */
int ExecQueue::init()
{
	int ret;
	LOG_TRACE("ExecQueue::init");
	ret = pthread_mutex_init(&this->mutex, NULL);
	if (ret == 0)
	{
		INIT_LIST_HEAD(&this->task_list);
		return 0;
	}

	errno = ret;
	return -1;
}

/**
 * @brief  就是销毁mutex
 * @note   
 * @retval None
 */
void ExecQueue::deinit()
{
	pthread_mutex_destroy(&this->mutex);
}

int Executor::init(size_t nthreads)
{
	LOG_TRACE("Executor::init");
	if (nthreads == 0)
	{
		errno = EINVAL;
		return -1;
	}
	LOG_TRACE("Calculate thread pool create");
	this->thrdpool = thrdpool_create(nthreads, 0);
	if (this->thrdpool)
		return 0;

	return -1;
}

void Executor::deinit()
{
	thrdpool_destroy(Executor::executor_cancel_tasks, this->thrdpool);
}

// 为什么C++文件可以获取到这个函数的实现见笔记
extern "C" void __thrdpool_schedule(const struct thrdpool_task *, void *,
									thrdpool_t *);

void Executor::executor_thread_routine(void *context)
{
	LOG_TRACE("Executor::executor_thread_routine");
	ExecQueue *queue = (ExecQueue *)context;
	struct ExecTaskEntry *entry;
	ExecSession *session;

	pthread_mutex_lock(&queue->mutex);

	entry = list_entry(queue->task_list.next, struct ExecTaskEntry, list); // 获取当前正要执行的任务任务
	list_del(&entry->list); // 从任务子队列里删掉当前正要执行的任务

	session = entry->session;

	if (!list_empty(&queue->task_list))
	{ // 如果子队列后面还有任务（也就是同名任务），放进来主队列中
		// 如果任务队列不为空
		// 之前request如果是第一个，需要先去malloc entry
		// 队列后面的就不需要malloc了，直接复用
		struct thrdpool_task task = {
			.routine	=	Executor::executor_thread_routine, // 函数指针
			.context	=	queue							   // 函数上下文
		};
		// 重复利用了这个entry，设置entry->task = task，再次加入线程池中
		__thrdpool_schedule(&task, entry, entry->thrdpool);
	}
	else
		free(entry);

	pthread_mutex_unlock(&queue->mutex);

	session->execute();   // 执行当前任务... 跑啊跑...
	session->handle(ES_STATE_FINISHED, 0);   // 执行完，调用接口，会打通后续任务流
}

void Executor::executor_cancel_tasks(const struct thrdpool_task *task)
{
	ExecQueue *queue = (ExecQueue *)task->context;
	struct ExecTaskEntry *entry;
	struct list_head *pos, *tmp;
	ExecSession *session;

	list_for_each_safe(pos, tmp, &queue->task_list)
	{
		entry = list_entry(pos, struct ExecTaskEntry, list);
		list_del(pos);
		session = entry->session;
		free(entry);

		session->handle(ES_STATE_CANCELED, 0);
	}
}

int Executor::request(ExecSession *session, ExecQueue *queue)
{
	LOG_TRACE("Executor::request");
	struct ExecTaskEntry *entry;

	session->queue = queue;
	
	entry = (struct ExecTaskEntry *)malloc(sizeof (struct ExecTaskEntry));
	if (entry)
	{
		entry->session = session;   // entry封装session
		entry->thrdpool = this->thrdpool;  // entry封装线程池

		pthread_mutex_lock(&queue->mutex);

		// 把任务加到对应的子队列后面 
		list_add_tail(&entry->list, &queue->task_list); 
		LOG_TRACE("add task to task_list");
		
		/* 
			如果这是第一个任务，需要去thrdpool_schedule(第一次调用需要malloc buf(entry(struct __thrdpool_task_entry)))
			后面就复用entry(struct __thrdpool_task_entry)了————> 见 void Executor::executor_thread_routine(void *context)
		*/ 
		if (queue->task_list.next == &entry->list)    
		{ // 子队列刚才是空的，那么可以立刻把此任务提交到Executor中
			// executor_thread_routine 就是取出queue中的任务去运行
			// 我们把包装成一个task，就是为了分发给各个线程
			struct thrdpool_task task = {
				.routine	=	Executor::executor_thread_routine, // 函数指针
				.context	=	queue							   // 函数上下文
			};
			// 将task放入线程池(第一次用就调用thrdpool_schedule，之后就是__thrdpool_schedule)
			if (thrdpool_schedule(&task, this->thrdpool) < 0)
			{
				list_del(&entry->list);
				free(entry);
				entry = NULL;
			}
		}

		pthread_mutex_unlock(&queue->mutex);
	}

	return -!entry;
}

