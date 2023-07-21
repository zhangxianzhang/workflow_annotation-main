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

  Authors: Wu Jiaxu (wujiaxu@sogou-inc.com)
           Li Yingxin (liyingxin@sogou-inc.com)
           Liu Kai (liukaidx@sogou-inc.com)
*/

#include <assert.h>
#include <string>
#include <openssl/bio.h>
#include <openssl/ssl.h>
#include "WFTaskError.h"
#include "WFTaskFactory.h"
#include "StringUtil.h"
#include "WFGlobal.h"
#include "HttpUtil.h"
#include "SSLWrapper.h"

using namespace protocol;

#define HTTP_KEEPALIVE_DEFAULT	(60 * 1000)
#define HTTP_KEEPALIVE_MAX		(300 * 1000)

/**********Client**********/

class ComplexHttpTask : public WFComplexClientTask<HttpRequest, HttpResponse>
{
public:
	ComplexHttpTask(int redirect_max,
					int retry_max,
					http_callback_t&& callback):
		WFComplexClientTask(retry_max, std::move(callback)),
		redirect_max_(redirect_max),
		redirect_count_(0)
	{
		LOG_TRACE("ComplexHttpTask creator");
		HttpRequest *client_req = this->get_req();

		// 默认Get / HTTP/1.1
		client_req->set_method(HttpMethodGet);
		client_req->set_http_version("HTTP/1.1");
	}

protected:
	virtual CommMessageOut *message_out();
	virtual CommMessageIn *message_in();
	virtual int keep_alive_timeout();
	virtual bool init_success();
	virtual void init_failed();
	virtual bool finish_once();

protected:
	bool need_redirect(ParsedURI& uri);
	bool redirect_url(HttpResponse *client_resp, ParsedURI& uri);
	void set_empty_request();

private:
	int redirect_max_;  
	int redirect_count_;
};

CommMessageOut *ComplexHttpTask::message_out()
{
	auto *req = this->get_req();
	bool is_alive;
	HttpHeaderCursor req_cursor(req);
	struct HttpMessageHeader header;  
	bool chunked = false;

	// "Transfer-Encoding" : ../../other/01_transfer_encoding.md
	header.name = "Transfer-Encoding";
	header.name_len = 17;
	// 如果查找到"Transfer-Encoding"
	if (req_cursor.find(&header) && header.value_len > 0)
	{
		// transfer-encoding的可选值有：chunked,identity，
		// 前者指把要发送传输的数据切割成一系列的块数据传输
		// 后者指传输时不做任何处理，自身的本质数据形式传输
		// 此处是看是否为chunked
		chunked = !(header.value_len == 8 &&
			strncasecmp((const char *)header.value, "identity", 8) == 0);
	}

	// 如果不是chunked
	if (!chunked)
	{
		size_t body_size = req->get_output_body_size();
		const char *method = req->get_method();

		if (body_size != 0 || strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0)
		{
			header.name = "Content-Length";
			header.name_len = 14;
			req_cursor.rewind();  // 返回最前
			// 如果没查找到Content-Length header
			if (!req_cursor.find(&header))
			{
				char buf[32];
				header.value = buf;
				header.value_len = sprintf(buf, "%zu", body_size);
				req->add_header(&header);   // 帮用户拼凑这个字段
			}
		}
	}

	header.name = "Connection";
	header.name_len = 10;
	req_cursor.rewind();   // cursor再次倒退到最前面
	// 查找到Connection字段
	if (req_cursor.find(&header))
	{
		// 看看这个字段是不是Keep-Alive
		is_alive = (header.value_len == 10 &&
				strncasecmp((const char *)header.value, "Keep-Alive", 10) == 0);
	}
	else if (this->keep_alive_timeo != 0)
	{
		// 如果没找到这个字段，但是timeout不为0，那么也是Keep-Alive，不断掉，并帮用户增加上这个字段
		is_alive = true;
		header.value = "Keep-Alive";
		header.value_len = 10;
		req->add_header(&header);
	}
	else
	{
		// 否则的话那么就是短连接
		is_alive = false; 
		header.value = "close";
		header.value_len = 5;
		req->add_header(&header);
	}

	if (!is_alive)  
		this->keep_alive_timeo = 0;  // 短连接，设置time_out 为0
	else
	{
		//req---Connection: Keep-Alive
		//req---Keep-Alive: timeout=0,max=100
		// timeout: An integer representing the time in seconds that the host will allow an idle connection to remain open before it is closed
		// max: indicating the maximum number of requests that can be sent on this connection before closing it
		// https://developer.mozilla.org/en-US/docs/Web/HTTP/Headers/Keep-Alive
		header.name = "Keep-Alive";
		header.name_len = 10;
		req_cursor.rewind();  // cursor 再次倒退
		// 找Keep-Alive
		if (req_cursor.find(&header))
		{
			std::string keep_alive((const char *)header.value, header.value_len);
			// value切割开 timeout, max
			std::vector<std::string> params = StringUtil::split(keep_alive, ',');
			
			for (const auto& kv : params)
			{
				// 再将其 timeout=0 格式 切开
				std::vector<std::string> arr = StringUtil::split(kv, '=');
				if (arr.size() < 2)  // 没有就设置为0
					arr.emplace_back("0");

				std::string key = StringUtil::strip(arr[0]);
				std::string val = StringUtil::strip(arr[1]);
				if (strcasecmp(key.c_str(), "timeout") == 0)
				{
					// 如果是timeout的话，rfc要求单位是s，所以要 * 1000
					this->keep_alive_timeo = 1000 * atoi(val.c_str());
					break;
				}
			}
		}
		// 最大timeout是HTTP_KEEPALIVE_MAX
		if ((unsigned int)this->keep_alive_timeo > HTTP_KEEPALIVE_MAX)
			this->keep_alive_timeo = HTTP_KEEPALIVE_MAX;
		//if (this->keep_alive_timeo < 0 || this->keep_alive_timeo > HTTP_KEEPALIVE_MAX)
	}

	//req->set_header_pair("Accept", "*/*");
	// 然后发送出去
	return this->WFClientTask::message_out();
}

CommMessageIn *ComplexHttpTask::message_in()
{
	auto *resp = this->get_resp();

	if (strcmp(this->get_req()->get_method(), HttpMethodHead) == 0)
		resp->parse_zero_body();

	return this->WFClientTask::message_in();
}

int ComplexHttpTask::keep_alive_timeout()
{
	return this->resp.is_keep_alive() ? this->keep_alive_timeo : 0;
}

void ComplexHttpTask::set_empty_request()
{
	HttpRequest *client_req = this->get_req();
	client_req->set_request_uri("/");
	client_req->set_header_pair("Host", "");
}

void ComplexHttpTask::init_failed()
{
	this->set_empty_request();
}

bool ComplexHttpTask::init_success()
{
	HttpRequest *client_req = this->get_req();
	std::string request_uri;
	std::string header_host;
	bool is_ssl;
	bool is_unix = false;

	if (uri_.scheme && strcasecmp(uri_.scheme, "http") == 0)
		is_ssl = false;
	else if (uri_.scheme && strcasecmp(uri_.scheme, "https") == 0)
		is_ssl = true;
	else
	{
		this->state = WFT_STATE_TASK_ERROR;
		this->error = WFT_ERR_URI_SCHEME_INVALID;
		this->set_empty_request();
		return false;
	}

	//todo http+unix
	//https://stackoverflow.com/questions/26964595/whats-the-correct-way-to-use-a-unix-domain-socket-in-requests-framework
	//https://stackoverflow.com/questions/27037990/connecting-to-postgres-via-database-url-and-unix-socket-in-rails

	if (uri_.path && uri_.path[0])
		request_uri = uri_.path;
	else
		request_uri = "/";

	if (uri_.query && uri_.query[0])
	{
		request_uri += "?";
		request_uri += uri_.query;
	}

	if (uri_.host && uri_.host[0])
	{
		header_host = uri_.host;
		if (uri_.host[0] == '/')
			is_unix = true;
	}

	if (!is_unix && uri_.port && uri_.port[0])
	{
		int port = atoi(uri_.port);

		if (is_ssl)
		{
			if (port != 443)
			{
				header_host += ":";
				header_host += uri_.port;
			}
		}
		else
		{
			if (port != 80)
			{
				header_host += ":";
				header_host += uri_.port;
			}
		}
	}

	this->WFComplexClientTask::set_transport_type(is_ssl ? TT_TCP_SSL : TT_TCP);
	client_req->set_request_uri(request_uri.c_str());
	client_req->set_header_pair("Host", header_host.c_str());

	return true;
}

bool ComplexHttpTask::redirect_url(HttpResponse *client_resp, ParsedURI& uri)
{
	if (redirect_count_ < redirect_max_)
	{
		redirect_count_++;
		std::string url;
		HttpHeaderCursor cursor(client_resp);

		if (!cursor.find("Location", url) || url.empty())
		{
			this->state = WFT_STATE_TASK_ERROR;
			this->error = WFT_ERR_HTTP_BAD_REDIRECT_HEADER;
			return false;
		}

		if (url[0] == '/')
		{
			if (url[1] != '/')
			{
				if (uri.port)
					url = ':' + (uri.port + url);

				url = "//" + (uri.host + url);
			}

			url = uri.scheme + (':' + url);
		}

		URIParser::parse(url, uri);
		return true;
	}

	return false;
}

bool ComplexHttpTask::need_redirect(ParsedURI& uri)
{
	HttpRequest *client_req = this->get_req();
	HttpResponse *client_resp = this->get_resp();
	const char *status_code_str = client_resp->get_status_code();
	const char *method = client_req->get_method();

	if (!status_code_str || !method)
		return false;

	int status_code = atoi(status_code_str);

	switch (status_code)
	{
	case 301:
	case 302:
	case 303:
		if (redirect_url(client_resp, uri))
		{
			if (strcasecmp(method, HttpMethodGet) != 0 &&
				strcasecmp(method, HttpMethodHead) != 0)
			{
				client_req->set_method(HttpMethodGet);
			}

			return true;
		}
		else
			break;

	case 307:
	case 308:
		if (redirect_url(client_resp, uri))
			return true;
		else
			break;

	default:
		break;
	}

	return false;
}

bool ComplexHttpTask::finish_once()
{
	if (this->state == WFT_STATE_SUCCESS)
	{
		if (need_redirect(uri_))
			this->set_redirect(uri_);
		else if (this->state != WFT_STATE_SUCCESS)
			this->disable_retry();
	}
	else
		this->get_resp()->end_parsing();

	return true;
}

/*******Proxy Client*******/

static int __encode_auth(const char *p, std::string& auth)
{
	static SSL_CTX *init_ssl = WFGlobal::get_ssl_client_ctx();
	(void)init_ssl;
	BUF_MEM *bptr;
	BIO *bmem;
	BIO *b64;

	b64 = BIO_new(BIO_f_base64());
	if (b64)
	{
		bmem = BIO_new(BIO_s_mem());
		if (bmem)
		{
			BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
			b64 = BIO_push(b64, bmem);
			BIO_write(b64, p, strlen(p));
			(void)BIO_flush(b64);
			BIO_get_mem_ptr(b64, &bptr);

			if (bptr->length > 0)
			{
				auth.append("Basic ");
				auth.append(bptr->data, bptr->length);
			}

			BIO_free_all(b64);
			return 0;
		}

		BIO_free_all(b64);
	}

	return -1;
}

static SSL *__create_ssl(SSL_CTX *ssl_ctx)
{
	BIO *wbio;
	BIO *rbio;
	SSL *ssl;

	rbio = BIO_new(BIO_s_mem());
	if (rbio)
	{
		wbio = BIO_new(BIO_s_mem());
		if (wbio)
		{
			ssl = SSL_new(ssl_ctx);
			if (ssl)
			{
				SSL_set_bio(ssl, rbio, wbio);
				return ssl;
			}

			BIO_free(wbio);
		}

		BIO_free(rbio);
	}

	return NULL;
}

class ComplexHttpProxyTask : public ComplexHttpTask
{
public:
	ComplexHttpProxyTask(int redirect_max,
						 int retry_max,
						 http_callback_t&& callback):
		ComplexHttpTask(redirect_max, retry_max, std::move(callback)),
		is_user_request_(true)
	{ }

	void set_user_uri(ParsedURI&& uri) { user_uri_ = std::move(uri); }
	void set_user_uri(const ParsedURI& uri) { user_uri_ = uri; }

	virtual const ParsedURI *get_current_uri() const { return &user_uri_; }

protected:
	virtual CommMessageOut *message_out();
	virtual CommMessageIn *message_in();
	virtual int keep_alive_timeout();
	virtual bool init_success();
	virtual bool finish_once();

protected:
	virtual WFConnection *get_connection() const
	{
		WFConnection *conn = this->ComplexHttpTask::get_connection();

		if (conn && is_ssl_)
			return (SSLConnection *)conn->get_context();

		return conn;
	}

private:
	struct SSLConnection : public WFConnection
	{
		SSL *ssl_;
		SSLHandshaker handshaker_;
		SSLWrapper wrapper_;
		SSLConnection(SSL *ssl) : handshaker_(ssl), wrapper_(&wrapper_, ssl)
		{
			ssl_ = ssl;
		}
	};

	SSLHandshaker *get_ssl_handshaker() const
	{
		return &((SSLConnection *)this->get_connection())->handshaker_;
	}

	SSLWrapper *get_ssl_wrapper(ProtocolMessage *msg) const
	{
		SSLConnection *conn = (SSLConnection *)this->get_connection();
		conn->wrapper_ = SSLWrapper(msg, conn->ssl_);
		return &conn->wrapper_;
	}

	int init_ssl_connection();

	std::string proxy_auth_;
	ParsedURI user_uri_;
	bool is_ssl_;
	bool is_user_request_;
	short state_;
	int error_;
};

int ComplexHttpProxyTask::init_ssl_connection()
{
	SSL *ssl = __create_ssl(WFGlobal::get_ssl_client_ctx());
	WFConnection *conn;

	if (!ssl)
		return -1;

	SSL_set_tlsext_host_name(ssl, user_uri_.host);
	SSL_set_connect_state(ssl);

	conn = this->ComplexHttpTask::get_connection();
	SSLConnection *ssl_conn = new SSLConnection(ssl);

	auto&& deleter = [] (void *ctx)
	{
		SSLConnection *ssl_conn = (SSLConnection *)ctx;
		SSL_free(ssl_conn->ssl_);
		delete ssl_conn;
	};
	conn->set_context(ssl_conn, std::move(deleter));
	return 0;
}

CommMessageOut *ComplexHttpProxyTask::message_out()
{
	long long seqid = this->get_seq();

	if (seqid == 0) // CONNECT
	{
		HttpRequest *conn_req = new HttpRequest;
		std::string request_uri(user_uri_.host);

		request_uri += ":";
		if (user_uri_.port)
			request_uri += user_uri_.port;
		else
			request_uri += is_ssl_ ? "443" : "80";

		conn_req->set_method("CONNECT");
		conn_req->set_request_uri(request_uri);
		conn_req->set_http_version("HTTP/1.1");
		conn_req->add_header_pair("Host", request_uri.c_str());

		if (!proxy_auth_.empty())
			conn_req->add_header_pair("Proxy-Authorization", proxy_auth_);

		is_user_request_ = false;
		return conn_req;
	}
	else if (seqid == 1 && is_ssl_) // HANDSHAKE
	{
		is_user_request_ = false;
		return get_ssl_handshaker();
	}

	auto *msg = (ProtocolMessage *)this->ComplexHttpTask::message_out();
	return is_ssl_ ? get_ssl_wrapper(msg) : msg;
}

CommMessageIn *ComplexHttpProxyTask::message_in()
{
	long long seqid = this->get_seq();

	if (seqid == 0)
	{
		HttpResponse *conn_resp = new HttpResponse;
		conn_resp->parse_zero_body();
		return conn_resp;
	}
	else if (seqid == 1 && is_ssl_)
		return get_ssl_handshaker();

	auto *msg = (ProtocolMessage *)this->ComplexHttpTask::message_in();
	if (is_ssl_)
		return get_ssl_wrapper(msg);

	return msg;
}

int ComplexHttpProxyTask::keep_alive_timeout()
{
	long long seqid = this->get_seq();

	state_ = WFT_STATE_SUCCESS;
	error_ = 0;
	if (seqid == 0)
	{
		HttpResponse *resp = this->get_resp();
		const char *code_str;
		int status_code;

		*resp = std::move(*(HttpResponse *)this->get_message_in());
		code_str = resp->get_status_code();
		status_code = code_str ? atoi(code_str) : 0;

		switch (status_code)
		{
		case 200:
			break;
		case 407:
			this->disable_retry();
		default:
			state_ = WFT_STATE_TASK_ERROR;
			error_ = WFT_ERR_HTTP_PROXY_CONNECT_FAILED;
			return 0;
		}

		this->clear_resp();

		if (is_ssl_ && init_ssl_connection() < 0)
		{
			state_ = WFT_STATE_SYS_ERROR;
			error_ = errno;
			return 0;
		}

		return HTTP_KEEPALIVE_DEFAULT;
	}
	else if (seqid == 1 && is_ssl_)
		return HTTP_KEEPALIVE_DEFAULT;

	return this->ComplexHttpTask::keep_alive_timeout();
}

bool ComplexHttpProxyTask::init_success()
{
	if (!uri_.scheme || strcasecmp(uri_.scheme, "http") != 0)
	{
		this->state = WFT_STATE_TASK_ERROR;
		this->error = WFT_ERR_URI_SCHEME_INVALID;
		return false;
	}

	if (user_uri_.state == URI_STATE_ERROR)
	{
		this->state = WFT_STATE_SYS_ERROR;
		this->error = uri_.error;
		return false;
	}
	else if (user_uri_.state != URI_STATE_SUCCESS)
	{
		this->state = WFT_STATE_TASK_ERROR;
		this->error = WFT_ERR_URI_PARSE_FAILED;
		return false;
	}

	if (user_uri_.scheme && strcasecmp(user_uri_.scheme, "http") == 0)
		is_ssl_ = false;
	else if (user_uri_.scheme && strcasecmp(user_uri_.scheme, "https") == 0)
		is_ssl_ = true;
	else
	{
		this->state = WFT_STATE_TASK_ERROR;
		this->error = WFT_ERR_URI_SCHEME_INVALID;
		this->set_empty_request();
		return false;
	}

	int user_port;
	if (user_uri_.port)
	{
		user_port = atoi(user_uri_.port);
		if (user_port <= 0 || user_port > 65535)
		{
			this->state = WFT_STATE_TASK_ERROR;
			this->error = WFT_ERR_URI_PORT_INVALID;
			return false;
		}
	}
	else
		user_port = is_ssl_ ? 443 : 80;

	if (uri_.userinfo && uri_.userinfo[0])
	{
		proxy_auth_.clear();
		if (__encode_auth(uri_.userinfo, proxy_auth_) < 0)
		{
			this->state = WFT_STATE_SYS_ERROR;
			this->error = errno;
			return false;
		}
	}

	std::string info("http-proxy|remote:");
	info += is_ssl_ ? "https://" : "http://";
	info += user_uri_.host;
	info += ":";
	if (user_uri_.port)
		info += user_uri_.port;
	else
		info += is_ssl_ ? "443" : "80";
	info += "|auth:";
	info += proxy_auth_;

	this->WFComplexClientTask::set_info(info);

	std::string request_uri;
	std::string header_host;

	if (user_uri_.path && user_uri_.path[0])
		request_uri = user_uri_.path;
	else
		request_uri = "/";

	if (user_uri_.query && user_uri_.query[0])
	{
		request_uri += "?";
		request_uri += user_uri_.query;
	}

	if (user_uri_.host && user_uri_.host[0])
		header_host = user_uri_.host;

	if ((is_ssl_ && user_port != 443) || (!is_ssl_ && user_port != 80))
	{
		header_host += ":";
		header_host += uri_.port;
	}

	HttpRequest *client_req = this->get_req();
	client_req->set_request_uri(request_uri.c_str());
	client_req->set_header_pair("Host", header_host.c_str());
	this->WFComplexClientTask::set_transport_type(TT_TCP);
	return true;
}

bool ComplexHttpProxyTask::finish_once()
{
	if (!is_user_request_)
	{
		if (this->state == WFT_STATE_SUCCESS && state_ != WFT_STATE_SUCCESS)
		{
			this->state = state_;
			this->error = error_;
		}

		if (this->get_seq() == 0)
		{
			delete this->get_message_in();
			delete this->get_message_out();
		}

		is_user_request_ = true;
		return false;
	}

	if (this->state == WFT_STATE_SUCCESS)
	{
		if (this->need_redirect(user_uri_))
			this->set_redirect(uri_);
		else if (this->state != WFT_STATE_SUCCESS)
			this->disable_retry();
	}
	else
		this->get_resp()->end_parsing();

	return true;
}

/**********Client Factory**********/

WFHttpTask *WFTaskFactory::create_http_task(const std::string& url,
											int redirect_max,
											int retry_max,
											http_callback_t callback)
{
	LOG_TRACE("create_http_task");
	auto *task = new ComplexHttpTask(redirect_max,
									 retry_max,
									 std::move(callback));
	ParsedURI uri;

	URIParser::parse(url, uri);
	task->init(std::move(uri));
	task->set_keep_alive(HTTP_KEEPALIVE_DEFAULT);
	return task;
}

WFHttpTask *WFTaskFactory::create_http_task(const ParsedURI& uri,
											int redirect_max,
											int retry_max,
											http_callback_t callback)
{
	auto *task = new ComplexHttpTask(redirect_max,
									 retry_max,
									 std::move(callback));

	task->init(uri);
	task->set_keep_alive(HTTP_KEEPALIVE_DEFAULT);
	return task;
}

WFHttpTask *WFTaskFactory::create_http_task(const std::string& url,
											const std::string& proxy_url,
											int redirect_max,
											int retry_max,
											http_callback_t callback)
{
	auto *task = new ComplexHttpProxyTask(redirect_max,
										  retry_max,
										  std::move(callback));

	ParsedURI uri, user_uri;
	URIParser::parse(url, user_uri);
	URIParser::parse(proxy_url, uri);

	task->set_user_uri(std::move(user_uri));
	task->set_keep_alive(HTTP_KEEPALIVE_DEFAULT);
	task->init(std::move(uri));
	return task;
}

WFHttpTask *WFTaskFactory::create_http_task(const ParsedURI& uri,
											const ParsedURI& proxy_uri,
											int redirect_max,
											int retry_max,
											http_callback_t callback)
{
	auto *task = new ComplexHttpProxyTask(redirect_max,
										  retry_max,
										  std::move(callback));

	task->set_user_uri(uri);
	task->set_keep_alive(HTTP_KEEPALIVE_DEFAULT);
	task->init(proxy_uri);
	return task;
}

/**********Server**********/

/*
这些是 `WFHttpServerTask` 类的私有成员变量：

1. `bool req_is_alive_;`：此成员变量表示当前处理的 HTTP 请求是否需要保持连接（keep-alive）。在 HTTP/1.1 中，持久连接是默认选项。持久连接允许在同一个 TCP 连接中处理多个请求/响应，而无需为每个请求/响应打开一个新的连接，这可以显著提高网络效率。

2. `bool req_header_has_keep_alive_;`：此成员变量表示当前处理的 HTTP 请求的头部是否包含 "Keep-Alive" 字段。"Keep-Alive" 头部字段是一个通知消息的发送方当前 TCP 连接在发送消息后应保持活跃状态以等待进一步的请求/响应。在 HTTP/1.0 中，这是实现持久连接的主要方式。

3. `std::string req_keep_alive_;`：此成员变量存储了 "Keep-Alive" 头部字段的值。在 HTTP/1.0 中，"Keep-Alive" 头部字段可以包含额外的参数，比如 "timeout" 和 "max"，这些参数分别指定了这个持久连接应该保持活跃的时间（秒）以及在此时间段内可以处理的最大请求数。这个字符串成员变量就是用来存储这些参数的。

以上这些变量在处理 HTTP 请求时，被用来决定当前连接是否应该保持活跃以及如何处理进一步的请求。
*/
class WFHttpServerTask : public WFServerTask<HttpRequest, HttpResponse>
{
public:
	// 构造函数，接受一个通信服务指针和一个处理函数
	WFHttpServerTask(CommService *service,
					 std::function<void (WFHttpTask *)>& process):
		WFServerTask(service, WFGlobal::get_scheduler(), process),
		req_is_alive_(false),
		req_header_has_keep_alive_(false)
	{}

protected:
    // 处理函数，根据状态和错误码进行处理
	virtual void handle(int state, int error)
	{
		// 如果状态是WFT_STATE_TOREPLY，即待回复状态
		if (state == WFT_STATE_TOREPLY)
		{
			// 判断请求是否是keep alive的
			req_is_alive_ = this->req.is_keep_alive();
			// 如果请求是keep alive的
			if (req_is_alive_)
			{
				// 创建HttpHeaderCursor遍历请求的头部
				HttpHeaderCursor req_cursor(&this->req);
				struct HttpMessageHeader header;

				// 设置要查找的头部名称为"Keep-Alive"
				header.name = "Keep-Alive";
				header.name_len = 10;
				// 查找"Keep-Alive"头部
				req_header_has_keep_alive_ = req_cursor.find(&header);
				// 如果找到"Keep-Alive"头部
				if (req_header_has_keep_alive_)
					// 将"Keep-Alive"头部的值赋值给req_keep_alive_
					req_keep_alive_.assign((const char *)header.value,
											header.value_len);
			}
		}

		// 调用父类的handle函数进行处理
		this->WFServerTask::handle(state, error);
	}

    // 虚函数，用于获取输出的消息，具体实现在类的定义之外
	virtual CommMessageOut *message_out();

private:
    // 请求是否是keep alive的
	bool req_is_alive_;
    // 请求头部是否有"Keep-Alive"字段
	bool req_header_has_keep_alive_;
    // 请求的"Keep-Alive"字段的值
	std::string req_keep_alive_;
};


CommMessageOut *WFHttpServerTask::message_out()
{
	auto *resp = this->get_resp();

	if (!resp->get_http_version())
		resp->set_http_version("HTTP/1.1");

	const char *status_code_str = resp->get_status_code();
	if (!status_code_str || !resp->get_reason_phrase())
	{
		int status_code;

		if (status_code_str)
			status_code = atoi(status_code_str);
		else
			status_code = HttpStatusOK;

		HttpUtil::set_response_status(resp, status_code);
	}

	HttpHeaderCursor resp_cursor(resp);
	struct HttpMessageHeader header;
	bool chunked = false;

	header.name = "Transfer-Encoding";
	header.name_len = 17;
	if (resp_cursor.find(&header) && header.value_len > 0)
	{
		chunked = !(header.value_len == 8 &&
			strncasecmp((const char *)header.value, "identity", 8) == 0);
	}

	size_t body_size = resp->get_output_body_size();

	if (!chunked)
	{
		header.name = "Content-Length";
		header.name_len = 14;
		resp_cursor.rewind();
		if (!resp_cursor.find(&header))
		{
			char buf[32];
			header.value = buf;
			header.value_len = sprintf(buf, "%zu", body_size);
			resp->add_header(&header);
		}
	}

	bool is_alive;
	bool resp_has_connection;

	header.name = "Connection";
	header.name_len = 10;
	resp_cursor.rewind();
	resp_has_connection = resp_cursor.find(&header);
	if (resp_has_connection)
	{
		is_alive = (header.value_len == 10 &&
				strncasecmp((const char *)header.value, "Keep-Alive", 10) == 0);
	}
	else
		is_alive = req_is_alive_;

	if (!is_alive)
		this->keep_alive_timeo = 0;
	else
	{
		//req---Connection: Keep-Alive
		//req---Keep-Alive: timeout=5,max=100

		if (req_header_has_keep_alive_)
		{
			int flag = 0;
			std::vector<std::string> params = StringUtil::split(req_keep_alive_, ',');

			for (const auto& kv : params)
			{
				std::vector<std::string> arr = StringUtil::split(kv, '=');
				if (arr.size() < 2)
					arr.emplace_back("0");

				std::string key = StringUtil::strip(arr[0]);
				std::string val = StringUtil::strip(arr[1]);
				if (!(flag & 1) && strcasecmp(key.c_str(), "timeout") == 0)
				{
					flag |= 1;
					// keep_alive_timeo = 5000ms when Keep-Alive: timeout=5
					this->keep_alive_timeo = 1000 * atoi(val.c_str());
					if (flag == 3)
						break;
				}
				else if (!(flag & 2) && strcasecmp(key.c_str(), "max") == 0)
				{
					flag |= 2;
					if (this->get_seq() >= atoi(val.c_str()))
					{
						this->keep_alive_timeo = 0;
						break;
					}

					if (flag == 3)
						break;
				}
			}
		}

		if ((unsigned int)this->keep_alive_timeo > HTTP_KEEPALIVE_MAX)
			this->keep_alive_timeo = HTTP_KEEPALIVE_MAX;
		//if (this->keep_alive_timeo < 0 || this->keep_alive_timeo > HTTP_KEEPALIVE_MAX)

	}

	if (this->keep_alive_timeo == 0)
	{
		if (!resp_has_connection)
		{
			header.name = "Connection";
			header.name_len = 10;
			header.value = "close";
			header.value_len = 5;
			resp->add_header(&header);
		}
	}
	else
	{
		if (!resp_has_connection)
		{
			header.name = "Connection";
			header.name_len = 10;
			header.value = "Keep-Alive";
			header.value_len = 10;
			resp->add_header(&header);
		}
	}

	return this->WFServerTask::message_out();
}

/**********Server Factory**********/

WFHttpTask *WFServerTaskFactory::create_http_task(CommService *service,
							std::function<void (WFHttpTask *)>& process)
{
	return new WFHttpServerTask(service, process);
}

