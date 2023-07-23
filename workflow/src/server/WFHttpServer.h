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

  Authors: Xie Han (xiehan@sogou-inc.com)
*/
// 防止多次包含此头文件
#ifndef _WFHTTPSERVER_H_
#define _WFHTTPSERVER_H_

#include <utility>
#include "HttpMessage.h"
#include "WFServer.h"
#include "WFTaskFactory.h"

// 定义一个处理HTTP任务的函数类型
using http_process_t = std::function<void (WFHttpTask *)>;

// 定义一个使用HTTP请求和HTTP响应协议的服务器类型
using WFHttpServer = WFServer<protocol::HttpRequest,
							  protocol::HttpResponse>;


// 服务器的默认参数设置
static constexpr struct WFServerParams HTTP_SERVER_PARAMS_DEFAULT =
{
	.max_connections		=	2000, // 最大连接数
	.peer_response_timeout	=	10 * 1000, // 服务器等待客户端响应的超时时间，单位是毫秒。这里设置为 10000 毫秒，即 10 秒。超过这个时间，服务器将关闭连接。
	.receive_timeout		=	-1, // 接收超时
	.keep_alive_timeout		=	60 * 1000, // 保持连接超时
	.request_size_limit		=	(size_t)-1, // 请求大小限制
	.ssl_accept_timeout		=	10 * 1000, // SSL连接接受超时
};

// HTTP服务器的构造函数，接收一个处理HTTP任务的函数作为参数
template<>
inline WFHttpServer::WFServer(http_process_t proc) :
	WFServerBase(&HTTP_SERVER_PARAMS_DEFAULT), // 值向默认的服务器参数结构体
	process(std::move(proc)) // 用于处理网络任务的函数对象
{
}

// 对于新的连接，创建一个HTTP任务并设置各种参数
template<>
inline CommSession *WFHttpServer::new_session(long long seq, CommConnection *conn) // WFHttpTask是CommSession的派生类
{
	WFHttpTask *task;

	task = WFServerTaskFactory::create_http_task(this, this->process);
	task->set_keep_alive(this->params.keep_alive_timeout);
	task->set_receive_timeout(this->params.receive_timeout);
	task->get_req()->set_size_limit(this->params.request_size_limit);

	return task;
}

#endif

