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
           Wu Jiaxu (wujiaxu@sogou-inc.com)
*/
// 防止多次包含此头文件
#ifndef _WFSERVER_H_
#define _WFSERVER_H_

#include <sys/types.h>
#include <sys/socket.h>
#include <openssl/ssl.h>
#include <functional>
#include <atomic>
#include <mutex>
#include <condition_variable>

// 引入自定义的通信调度器和任务工厂。
#include "CommScheduler.h"
#include "WFTaskFactory.h"

// 定义一个结构体，用于设置服务器的一些参数
struct WFServerParams
{
	size_t max_connections;  // 最大的并发连接数
	int peer_response_timeout;	// 对等端读写操作的超时时间
	int receive_timeout;	// 接收整个消息的超时时间
	int keep_alive_timeout;  // keep-alive连接的超时时间
	size_t request_size_limit;  // 请求的大小限制
	int ssl_accept_timeout;	// SSL接受的超时时间，如果不使用SSL，这个参数会被忽略
};

// 定义默认的服务器参数
static constexpr struct WFServerParams SERVER_PARAMS_DEFAULT =
{
	.max_connections		=	2000,    // 默认最大连接2000
	.peer_response_timeout	=	10 * 1000,  // 默认对等端读写操作的超时时间
	.receive_timeout		=	-1,  // 默认接收整个消息的超时时间
	.keep_alive_timeout		=	60 * 1000,  // 默认keep-alive连接的超时时间
	.request_size_limit		=	(size_t)-1,  // 默认请求的大小限制
	.ssl_accept_timeout		=	10 * 1000,  // 默认SSL接受的超时时间
};

// 定义一个TCP服务器基类，保护继承自CommService
class WFServerBase : protected CommService
{
public:
	WFServerBase(const struct WFServerParams *params) :
		conn_count(0)
	{
		this->params = *params;
		this->listen_fd = -1;
		this->unbind_finish = false;
	}

public:
	/* To start a TCP server */

	// 服务器的启动函数，支持各种启动方式，包括指定IP版本、主机名、端口、证书文件等

	/*  使用端口号启动服务器，默认IPv4(family) */
	int start(unsigned short port)
	{
		// https://man7.org/linux/man-pages/man3/getaddrinfo.3.html
		
		// 当host = NULL 传进去时
		// If the AI_PASSIVE flag is specified in hints.ai_flags, and node
		// is NULL, then the returned socket addresses will be suitable for
		// bind(2)ing a socket that will accept(2) connections. 
		
		// The returned socket address will contain the "wildcard address"
		// (INADDR_ANY for IPv4 addresses, IN6ADDR_ANY_INIT for IPv6
		// address).  
		// The wildcard address is used by applications
		// (typically servers) that intend to accept connections on any of
		// the host's network addresses.  
		
		// If node is not NULL, then the AI_PASSIVE flag is ignored.

		return start(AF_INET, NULL, port, NULL, NULL);
	}

	/* 使用IP版本(family)，可能是AF_INET (IPv4) 或 AF_INET6 (IPv6)，和端口号启动服务器 */
	int start(int family, unsigned short port)
	{
		return start(family, NULL, port, NULL, NULL);
	}

	/* 使用主机名和端口号启动服务器 */
	int start(const char *host, unsigned short port)
	{
		return start(AF_INET, host, port, NULL, NULL);
	}

	/* 使用IP版本(family)，主机名和端口号启动服务器 */
	int start(int family, const char *host, unsigned short port)
	{
		return start(family, host, port, NULL, NULL);
	}

	/* 使用sockaddr绑定地址启动服务器. */
	int start(const struct sockaddr *bind_addr, socklen_t addrlen)
	{
		return start(bind_addr, addrlen, NULL, NULL);
	}

	/* To start an SSL server. */

	// 使用端口号、证书文件和密钥文件启动SSL服务器
	int start(unsigned short port, const char *cert_file, const char *key_file)
	{
		return start(AF_INET, NULL, port, cert_file, key_file);
	}

	// 使用IP版本(family)，端口号，证书文件和密钥文件启动SSL服务器
	int start(int family, unsigned short port,
			  const char *cert_file, const char *key_file)
	{
		return start(family, NULL, port, cert_file, key_file);
	}

	// 使用主机名，端口号，证书文件和密钥文件启动SSL服务器
	int start(const char *host, unsigned short port,
			  const char *cert_file, const char *key_file)
	{
		return start(AF_INET, host, port, cert_file, key_file);
	}

	// 使用IP版本(family)，主机名，端口号，证书文件和密钥文件启动SSL服务器
	int start(int family, const char *host, unsigned short port,
			  const char *cert_file, const char *key_file);

	/* This is the only necessary start function. */
	// 这是唯一必需的启动函数。：使用sockaddr绑定地址，地址长度，证书文件和密钥文件启动服务器
	int start(const struct sockaddr *bind_addr, socklen_t addrlen,
			  const char *cert_file, const char *key_file);

	// 最后两个带listen_fd的serve()接口，主要用于优雅重启。或者简单建立一个非TCP协议（如SCTP）的server。
	/* 使用特定的文件描述符启动服务器，主要用于优雅重启或者非TCP协议（如SCTP）的服务器 */
	int serve(int listen_fd)
	{
		return serve(listen_fd, NULL, NULL);
	}
	
	// 使用特定的文件描述符，证书文件和密钥文件启动服务器
	int serve(int listen_fd, const char *cert_file, const char *key_file);

	/* stop()  停止服务器，这是一个阻塞操作. */
	void stop()
	{
		this->shutdown();
		this->wait_finish();
	}

	/* Nonblocking terminating the server. For stopping multiple servers.
	 * Typically, call shutdown() and then wait_finish().
	 * But indeed wait_finish() can be called before shutdown(), even before
	 * start() in another thread. */
	void shutdown();
	void wait_finish();

public:
	// 获取当前连接数
	size_t get_conn_count() const { return this->conn_count; }

protected:
	/*	 重写此函数以实现支持TLS SNI的服务器
		"servername"会是NULL，如果客户端没有设置主机名
		 返回NULL表示服务器不支持servername*/
	virtual SSL_CTX *get_server_ssl_ctx(const char *servername)
	{
		return this->get_ssl_ctx();
	}

protected:
	// 服务器参数
	WFServerParams params;

protected:
	// 创建新的连接
	virtual WFConnection *new_connection(int accept_fd);
	// 删除连接
	void delete_connection(WFConnection *conn);

private:
	// 初始化服务器
	int init(const struct sockaddr *bind_addr, socklen_t addrlen, const char *cert_file, const char *key_file);
	// 初始化SSL上下文
	int init_ssl_ctx(const char *cert_file, const char *key_file);
	static long ssl_ctx_callback(SSL *ssl, int *al, void *arg);
	// 创建监听文件描述符
	virtual int create_listen_fd();
	// 处理解除绑定
	virtual void handle_unbound();

protected:
	// 原子变量，表示当前的连接数
	std::atomic<size_t> conn_count;

private:
	// 监听文件描述符
	int listen_fd;
	// 是否完成解绑：在服务器停止运行时，可能需要解绑（unbind）套接字，这个变量可能用于指示这个过程是否完成。
	bool unbind_finish;

	std::mutex mutex;
	std::condition_variable cond;

	// 用于调度和管理通信任务
	CommScheduler *scheduler;
};

// 定义一个服务器类，继承自服务器基类，增添了创建新的会话的函数和处理服务器任务的函数。模板参数REQ和RESP分别表示请求和响应的类型
template<class REQ, class RESP>
class WFServer : public WFServerBase
{
public:
	//构造函数，接受 WFServerParams 结构体指针和用于处理网络任务的函数对象作为参数
	WFServer(const struct WFServerParams *params,
			 std::function<void (WFNetworkTask<REQ, RESP> *)> proc) :
		WFServerBase(params),
		process(std::move(proc))
	{
	}

	//构造函数，接受用于处理服务器任务的函数对象作为参数。这会导致传入默认的服务器参数
	WFServer(std::function<void (WFNetworkTask<REQ, RESP> *)> proc) :
		WFServerBase(&SERVER_PARAMS_DEFAULT),
		process(std::move(proc))
	{
	}


protected:
	// 创建新的会话的函数
	virtual CommSession *new_session(long long seq, CommConnection *conn);

protected:
	// 处理服务器任务的函数，将在 new_session() 中被绑定到会话任务
	std::function<void (WFNetworkTask<REQ, RESP> *)> process;
};

template<class REQ, class RESP>
CommSession *WFServer<REQ, RESP>::new_session(long long seq, CommConnection *conn) // WFNetworkTask<REQ, RESP>是CommSession的派生类
{
	using factory = WFNetworkTaskFactory<REQ, RESP>;
	WFNetworkTask<REQ, RESP> *task;

	task = factory::create_server_task(this, this->process);
	task->set_keep_alive(this->params.keep_alive_timeout);
	task->set_receive_timeout(this->params.receive_timeout);
	task->get_req()->set_size_limit(this->params.request_size_limit);

	return task;
}

#endif

