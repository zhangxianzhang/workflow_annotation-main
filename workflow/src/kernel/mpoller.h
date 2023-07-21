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

#ifndef _MPOLLER_H_
#define _MPOLLER_H_

#include <sys/types.h>
#include <sys/socket.h>
#include "poller.h"
#include "logger.h"

typedef struct __mpoller mpoller_t;// // 多线程轮询器 这个结构体在多线程网络编程中非常有用，因为它可以让我们为每个线程分配一个独立的轮询器实例，从而提高并发处理的能力。在这种设计中，每个线程都可以独立地处理它自己的 I/O 操作，而不需要等待其他线程。

/**
 * @brief mpoller 主要的功能函数
 * 
 */

#ifdef __cplusplus
extern "C"
{
#endif

mpoller_t *mpoller_create(const struct poller_params *params, size_t nthreads);
int mpoller_start(mpoller_t *mpoller);
void mpoller_stop(mpoller_t *mpoller);
void mpoller_destroy(mpoller_t *mpoller);

#ifdef __cplusplus
}
#endif

// 多线程轮询器 这个结构体在多线程网络编程中非常有用，因为它可以让我们为每个线程分配一个独立的轮询器实例，从而提高并发处理的能力。在这种设计中，每个线程都可以独立地处理它自己的 I/O 操作，而不需要等待其他线程。
struct __mpoller
{
	unsigned int nthreads; // 每个线程分配一个独立的轮询器实例
	poller_t *poller[1];  // 结构体的尾数组（伸缩数组），指向所有的poller结构体
};

// 将一个待监听的事件（在这里是文件描述符）通过取模运算添加到一个指定的poller线程中
static inline int mpoller_add(const struct poller_data *data, int timeout,
							  mpoller_t *mpoller)
{
	// 根据文件描述符(fd)计算出其应该分配到的poller线程，这里使用的是取模运算
	// 这样可以尽量均匀地分配文件描述符到各个poller线程中，避免某个线程过载
	unsigned int index = (unsigned int)data->fd % mpoller->nthreads;

	// 在计算出的poller线程中添加需要进行监听的事件和相关数据
	// poller_add函数负责添加事件到epoll中，并设定相应的超时时间
	return poller_add(data, timeout, mpoller->poller[index]);
}

static inline int mpoller_del(int fd, mpoller_t *mpoller)
{
	unsigned int index = (unsigned int)fd % mpoller->nthreads;
	return poller_del(fd, mpoller->poller[index]);
}

static inline int mpoller_mod(const struct poller_data *data, int timeout,
							  mpoller_t *mpoller)
{
	unsigned int index = (unsigned int)data->fd % mpoller->nthreads;
	return poller_mod(data, timeout, mpoller->poller[index]);
}

static inline int mpoller_set_timeout(int fd, int timeout, mpoller_t *mpoller)
{
	unsigned int index = (unsigned int)fd % mpoller->nthreads;
	return poller_set_timeout(fd, timeout, mpoller->poller[index]);
}

static inline int mpoller_add_timer(const struct timespec *value, void *context,
									mpoller_t *mpoller)
{
	static unsigned int n = 0;
	unsigned int index = n++ % mpoller->nthreads;
	
	LOG_TRACE("mpoller_add_timer add to %d's thrd", index);

	return poller_add_timer(value, context, mpoller->poller[index]);
}

#endif

