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

#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdio.h>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <openssl/ssl.h>
#include "CommScheduler.h"
#include "WFConnection.h"
#include "WFGlobal.h"
#include "WFServer.h"

// 定义端口号字符串表示的最大长度。
#define PORT_STR_MAX	5

// 从 WFConnection 派生的类。它维护服务器中活动连接的数量
class WFServerConnection : public WFConnection
{
public:
	WFServerConnection(std::atomic<size_t> *conn_count)
	{
		this->conn_count = conn_count;
	}

	virtual ~WFServerConnection()
	{
		(*this->conn_count)--;
	}

private:
	// 一个指向原子 size_t 的指针，跟踪活动连接的数量。
	std::atomic<size_t> *conn_count;
};

// 方法处理SSL/TLS握手期间的服务器名指示（SNI）。它根据服务器名称设置SSL上下文。
long WFServerBase::ssl_ctx_callback(SSL *ssl, int *al, void *arg)
{
	WFServerBase *server = (WFServerBase *)arg; // 服务器对象。
	const char *servername = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name); // 客户端提供的服务器名称。
	SSL_CTX *ssl_ctx = server->get_server_ssl_ctx(servername); // 根据服务器名称获取SSL上下文。

	if (!ssl_ctx)
		return SSL_TLSEXT_ERR_NOACK; // 如果没有相应的SSL上下文，则返回错误。

	if (ssl_ctx != server->get_ssl_ctx())
		SSL_set_SSL_CTX(ssl, ssl_ctx); // 如果当前的SSL上下文与获得的上下文不同，则设置新的上下文。

	return SSL_TLSEXT_ERR_OK; // 返回成功。
}

// 为服务器初始化SSL上下文的方法。它从提供的文件中加载服务器证书和私钥。
int WFServerBase::init_ssl_ctx(const char *cert_file, const char *key_file)
{
	SSL_CTX *ssl_ctx = WFGlobal::new_ssl_server_ctx(); // 创建新的SSL上下文。

	if (!ssl_ctx)
		return -1; // 如果无法创建上下文，则返回错误。

	// 设置SSL上下文选项。
	SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_NONE, NULL);
	if (SSL_CTX_use_certificate_file(ssl_ctx, cert_file, SSL_FILETYPE_PEM) > 0 &&
		SSL_CTX_use_PrivateKey_file(ssl_ctx, key_file, SSL_FILETYPE_PEM) > 0 &&
		SSL_CTX_set_tlsext_servername_callback(ssl_ctx, ssl_ctx_callback) > 0 &&
		SSL_CTX_set_tlsext_servername_arg(ssl_ctx, this) > 0)
	{
		this->set_ssl(ssl_ctx, this->params.ssl_accept_timeout);
		return 0;
	}

	SSL_CTX_free(ssl_ctx);
	return -1;
}

// 初始化服务器的方法。这包括初始化通信服务、设置SSL上下文（如果提供了密钥和证书文件）和懒加载的方式创建通信调度器对象。由被组合到通信调度器对象的通信器对象创建poller线程和创建线程池
int WFServerBase::init(const struct sockaddr *bind_addr, socklen_t addrlen,
					   const char *cert_file, const char *key_file)
{
	int timeout = this->params.peer_response_timeout;  // 设置超时参数

	// 如果设置了接收超时，并且小于响应超时，则使用接收超时作为超时参数
	if (this->params.receive_timeout >= 0)
	{
		if ((unsigned int)timeout > (unsigned int)this->params.receive_timeout)
			timeout = this->params.receive_timeout;
	}

	// 初始化通信服务，如果失败，则返回错误一般情况下。
	// 将listen_timeout设置为-1表示禁用监听超时。这意味着在执行listen操作时，将无限期地等待连接请求的到达，直到有连接请求到达为止，不会因为超时而中断监听。
	if (this->CommService::init(bind_addr, addrlen, -1, timeout) < 0)
		return -1;

	// 如果提供了密钥和证书文件，则初始化SSL上下文
	if (key_file && cert_file)
	{
		if (this->init_ssl_ctx(cert_file, key_file) < 0)
		{
			this->deinit();  // 初始化SSL上下文失败，解除初始化，并返回错误
			return -1;
		}
	}

	// 获取通信调度器对象
	this->scheduler = WFGlobal::get_scheduler();
	return 0;  // 初始化成功，返回0
}

// 创建监听套接字的方法。如果已经存在监听套接字，则使用现有的；否则，创建一个新的。
int WFServerBase::create_listen_fd()
{
	int listen_fd  = this->listen_fd;

	if (listen_fd < 0)  // 如果当前没有监听套接字，则创建一个新的
	{
		const struct sockaddr *bind_addr;
		socklen_t addrlen;
		int reuse = 1;

		this->get_addr(&bind_addr, &addrlen);  // 获取绑定的地址和地址长度
		listen_fd = socket(bind_addr->sa_family, SOCK_STREAM, 0);  // 创建新的套接字
		if (listen_fd >= 0)
		{
			// 设置套接字选项，允许重用地址
			setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR,
					   &reuse, sizeof (int));
		}
	}

	return listen_fd;
}

// 创建新的服务器连接的方法。设置接受套接字选项，允许重用地址。如果连接数量已经达到最大限制，则不会创建新的连接。
WFConnection *WFServerBase::new_connection(int accept_fd)
{
	if (++this->conn_count <= this->params.max_connections ||  // 检查连接数量是否超过最大限制
		this->drain(1) == 1)
	{
		int reuse = 1;
		// 设置接受套接字选项，允许重用地址
		setsockopt(accept_fd, SOL_SOCKET, SO_REUSEADDR,
				   &reuse, sizeof (int));
		// 创建新的服务器连接
		return new WFServerConnection(&this->conn_count);
	}

	this->conn_count--;
	errno = EMFILE;
	return NULL;
}

// 删除一个服务器连接的方法。它删除指定的服务器连接对象。
void WFServerBase::delete_connection(WFConnection *conn)
{
	delete (WFServerConnection *)conn;
}

// 一个在服务器解除绑定后被调用的方法。它设置解除绑定完成标志，并通知任何等待解除绑定完成的线程。
void WFServerBase::handle_unbound()
{
	this->mutex.lock();  // 上锁
	this->unbind_finish = true;  // 设置解除绑定完成标志为真
	this->cond.notify_one();  // 通知一个等待解除绑定完成的线程
	this->mutex.unlock();  // 解锁
}

int WFServerBase::start(const struct sockaddr *bind_addr, socklen_t addrlen,
						const char *cert_file, const char *key_file)
{
	SSL_CTX *ssl_ctx; // SSL上下文

	// 如果服务器初始化成功
	if (this->init(bind_addr, addrlen, cert_file, key_file) >= 0)
	{
		// 如果被调度器管理的服务端中非阻塞listen套接字能够添加到mpoller中
		if (this->scheduler->bind(this) >= 0)
			return 0;

		// 如果服务器不能被调度器绑定，进行清理操作
		ssl_ctx = this->get_ssl_ctx();
		this->deinit();
		if (ssl_ctx)
			SSL_CTX_free(ssl_ctx);
	}

	// 如果服务器初始化失败，返回错误
	return -1;
}

// 使用IP版本(family)，主机名，端口号，证书文件和密钥文件启动SSL服务器
int WFServerBase::start(int family, const char *host, unsigned short port,
						const char *cert_file, const char *key_file)
{
	struct addrinfo hints = {
		.ai_flags		=	AI_PASSIVE,   // 指定地址信息的标志位。AI_PASSIVE可以告诉getaddrinfo()函数返回适用于被动套接字(监听套接字)的地址信息，即用于接受传入连接请求的服务器地址信息。
		.ai_family		=	family, // 地址族
		.ai_socktype	=	SOCK_STREAM, // 套接字类型为流式套接字
	}; //ai的缩写是"Address Information"。

	struct addrinfo *addrinfo; // 在getaddrinfo()函数调用后，如果成功获取到地址信息，则将其保存在 addrinfo 指向的指针中。 
	char port_str[PORT_STR_MAX + 1]; // 端口号字符串
	int ret;

	// 在snprintf函数中，"sn"表示"safe and bounded"
	snprintf(port_str, PORT_STR_MAX + 1, "%d", port); // 将端口号转换为字符串 整数 port 转换为字符串形式，并存储在 port_str 中，以便后续在获取地址信息时使用。确保字符串长度不超过 PORT_STR_MAX 的限制。
	
	/*
	获取用于绑定的地址信息，传入主机名、端口号字符串、提示和返回的地址信息结构体指针。 
	addrinfo **__restrict__ __pai：这是一个指向addrinfo指针的指针。
	在成功调用getaddrinfo()后，这个指针将指向一个动态分配的addrinfo结构体链表，这个链表中通过成员变量 ai_addr 指向了符合要求的套接字地址。
	const char *__restrict__ __name：这是一个主机名或者是一个IP地址（IPv4或IPv6）。如果此参数是NULL，那么返回的地址信息将是用于绑定到所有接口的套接字地址。
	在 getaddrinfo() 函数中，如果你把主机名参数设置为 NULL，那么返回的地址信息中的 IP 地址部分就会被设置为对应的通配 IP 地址。
	对于 IPv4，这个通配地址是 "0.0.0.0"；对于 IPv6，是 "::"
	这意味着，如果你使用这个地址信息去绑定一个套接字，那么这个套接字将可以接收发往该主机上所有网络接口的数据。
	*/
	ret = getaddrinfo(host, port_str, &hints, &addrinfo);  // addrinfo 现在指向一个包含主机地址信息的链表，这个链表包含了可以用来创建套接字的所有必要信息
	if (ret == 0) // 如果获取地址信息成功
	{	/*struct sockaddr *ai_addr;	 Socket address for socket. sockaddr 结构提供的是一个通用的套接字地址结构 */
		ret = start(addrinfo->ai_addr, (socklen_t)addrinfo->ai_addrlen,
					cert_file, key_file); // 调用重载的 start 函数，启动服务器
					
		freeaddrinfo(addrinfo); // 释放地址信息的内存。由于 getaddrinfo 函数使用了动态内存分配，因此在使用完获取到的地址信息后，需要显式地释放内存，以避免内存泄漏。
	}
	else
	{
		if (ret != EAI_SYSTEM)
			errno = EINVAL; // 设置错误码为 EINVAL（无效的参数）
		ret = -1; // 返回错误码 -1
	}

	return ret; // 返回结果
}

// 检查套接字是否已绑定的内部函数
static int __get_addr_bound(int sockfd, struct sockaddr *addr, socklen_t *len)
{
	int family;
	socklen_t i;

	if (getsockname(sockfd, addr, len) < 0)
		return -1;

	family = addr->sa_family;
	addr->sa_family = 0;
	for (i = 0; i < *len; i++)
	{
		if (((char *)addr)[i])
			break;
	}

	if (i == *len)
	{
		errno = EINVAL;
		return -1;
	}

	addr->sa_family = family;
	return 0;
}

// 使用一个已经存在的监听套接字来启动服务器的方法
int WFServerBase::serve(int listen_fd,
						const char *cert_file, const char *key_file)
{
	struct sockaddr_storage ss;
	socklen_t len = sizeof ss;
	int ret;

	// 获取并检查监听套接字的地址
	if (__get_addr_bound(listen_fd, (struct sockaddr *)&ss, &len) < 0)
		return -1;

	listen_fd = dup(listen_fd);  // 复制监听套接字
	if (listen_fd < 0)
		return -1;

	this->listen_fd = listen_fd;  // 设置监听套接字
	// 启动服务器
	ret = start((struct sockaddr *)&ss, len, cert_file, key_file);
	this->listen_fd = -1;  // 清除监听套接字
	return ret;
}

// 关闭服务器的方法。它通知调度器解除绑定服务器。
void WFServerBase::shutdown()
{
	this->scheduler->unbind(this);
}

// 等待服务器关闭完成的方法。它将阻塞，直到解除绑定完成。
void WFServerBase::wait_finish()
{
	SSL_CTX *ssl_ctx = this->get_ssl_ctx();
	std::unique_lock<std::mutex> lock(this->mutex);

	// 等待解除绑定完成
	while (!this->unbind_finish)
		this->cond.wait(lock);

	this->deinit();  // 解除初始化
	this->unbind_finish = false;  // 设置解除绑定完成标志为假
	lock.unlock();  // 解锁
	if (ssl_ctx)
		SSL_CTX_free(ssl_ctx);  // 释放SSL上下文
}

