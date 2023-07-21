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

  Authors: Li Yingxin (liyingxin@sogou-inc.com)
           Wu Jiaxu (wujiaxu@sogou-inc.com)
*/

#ifndef _WFFACILITIES_H_
#define _WFFACILITIES_H_

#include "WFFuture.h"
#include "WFTaskFactory.h"

/*
这个类是一个单例，因为所有的成员函数都是静态函数。它提供了一组实用工具，能够方便的进行一些常见操作，如休眠、异步执行函数、网络请求、异步文件IO等。它通过 WFFuture 类实现了异步编程，这是一种非阻塞的编程方式，能够提高程序的性能。
*/
class WFFacilities
{
public:
    // 静态的 usleep 方法，让当前线程休眠一段时间
	static void usleep(unsigned int microseconds);
	// 异步休眠方法，返回一个表示未来结果的 WFFuture 对象
	static WFFuture<void> async_usleep(unsigned int microseconds);

public:
    // 用于创建并执行一个异步任务的模板方法。运行一个新的任务，传入任务队列名称、函数以及参数列表
	template<class FUNC, class... ARGS>
	static void go(const std::string& queue_name, FUNC&& func, ARGS&&... args);

public:
    // 网络请求结果的结构体，用于封装网络请求的响应和一些任务相关的状态信息
	template<class RESP>
	struct WFNetworkResult
	{
		RESP resp;		// 响应
		long long seqid;// 序列号
		int task_state; // 任务状态
		int task_error; // 错误
	};

    // // 同步网络请求方法，发送请求并阻塞等待响应，传入请求方式、URL、请求和重试次数
	template<class REQ, class RESP>
	static WFNetworkResult<RESP> request(TransportType type, const std::string& url, REQ&& req, int retry_max);

    // 异步网络请求方法，发送请求并返回一个表示未来结果的 WFFuture 对象，传入请求方式、URL、请求和重试次数
	template<class REQ, class RESP>
	static WFFuture<WFNetworkResult<RESP>> async_request(TransportType type, const std::string& url, REQ&& req, int retry_max);

public:
    // 以下几个方法都是异步的文件IO操作，它们返回一个表示未来结果的 WFFuture 对象
	static WFFuture<ssize_t> async_pread(int fd, void *buf, size_t count, off_t offset); // 异步预读
	static WFFuture<ssize_t> async_pwrite(int fd, const void *buf, size_t count, off_t offset); // 异步预写
	static WFFuture<ssize_t> async_preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset); // 异步分散读
	static WFFuture<ssize_t> async_pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset); // 异步集中写
	static WFFuture<int> async_fsync(int fd); // 异步文件同步
	static WFFuture<int> async_fdatasync(int fd); // 异步数据同步

public:
    // 一个简单的计数同步器，用于在多个线程中同步一个或多个任务的完成状态
	class WaitGroup
	{
	public:
        // 构造函数，传入需要等待的任务数量
		WaitGroup(int n);
        // 析构函数
		~WaitGroup();

        // 标记一个任务已完成
		void done();
        // 阻塞等待所有任务完成
		void wait() const;
        // 阻塞等待所有任务完成或超时，返回等待状态
		std::future_status wait(int timeout) const;

	private:
        // 内部用于回调的函数，用于通知所有任务已完成
		static void __wait_group_callback(WFCounterTask *task);

        // 剩余未完成的任务数
		std::atomic<int> nleft;
        // 内部的计数任务
		WFCounterTask *task;
        // 表示未来结果的 WFFuture 对象
		WFFuture<void> future;
	};

private:
    // 以下几个方法都是内部的回调函数，用于处理不同类型的任务

	// 定时器任务的回调函数
	static void __timer_future_callback(WFTimerTask *task); 
	
	// 文件IO任务的回调函数
	static void __fio_future_callback(WFFileIOTask *task);

	// 文件虚拟IO任务的回调函数
	static void __fvio_future_callback(WFFileVIOTask *task);

	// 文件同步任务的回调函数
	static void __fsync_future_callback(WFFileSyncTask *task);
};

#include "WFFacilities.inl"

#endif

