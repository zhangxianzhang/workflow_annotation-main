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

#ifndef _HTTPMESSAGE_H_
#define _HTTPMESSAGE_H_

#include <string.h>
#include <utility>
#include <string>
#include "list.h"
#include "ProtocolMessage.h"
#include "http_parser.h"

/**
 * @file   HttpMessage.h
 * @brief  Http Protocol Interface
 */

namespace protocol
{

/**
 * @brief http header 是 kv : {name : value}
 * @brief 这个结构体通常在解析HTTP消息时使用，用于获取和处理消息头部的信息。
 */
struct HttpMessageHeader
{
	const void *name;      // 是一个指向头部名字的指针，例如 "Content-Type" 或 "Authorization" 等。这是一个常量指针，指向的值不能被修改。
	size_t name_len;       // 是头部名字的长度，以字节为单位。
	const void *value;     // 是一个指向头部值的指针。例如，如果头部是 "Content-Type"，那么值可能是 "text/html" 或 "application/json" 等。这是一个常量指针，指向的值不能被修改。
	size_t value_len;      // 是头部值的长度，以字节为单位。
};

/*
HttpMessage 类主要用于构建和解析 HTTP 消息(通过聚合的http_parser_t储存解析信息)，包括 HTTP 请求和响应。
其中包含了获取和设置 HTTP 版本，检查消息是否使用了分块传输编码或 Keep-Alive，添加和设置消息头部，获取和追加消息正文等方法。
*/
class HttpMessage : public ProtocolMessage
{
public:
	// 获取 HTTP 消息的版本，如 "HTTP/1.1"
	const char *get_http_version() const
	{
		return http_parser_get_version(this->parser);
	}

	// 设置 HTTP 消息的版本，输入参数为版本字符串，如 "HTTP/1.1"
	bool set_http_version(const char *version)
	{
		return http_parser_set_version(version, this->parser) == 0;
	}

	/* is_chunked(), is_keep_alive() only reflect the parsed result.
	 * set_header(), add_header() do not effect on it. 
	   检查 HTTP 消息是否使用了分块传输编码（chunked）*/
	bool is_chunked() const
	{
		return http_parser_chunked(this->parser);
	}

	// 检查 HTTP 连接是否应保持打开状态，也就是检查 HTTP 消息是否使用了 Keep-Alive
	bool is_keep_alive() const
	{
		return http_parser_keep_alive(this->parser);
	}

	// 添加 HTTP 消息头部，输入参数为 HttpMessageHeader 结构体，包含了头部名字和值
	bool add_header(const struct HttpMessageHeader *header)
	{
		return http_parser_add_header(header->name, header->name_len,
									  header->value, header->value_len,
									  this->parser) == 0;
	}

	// 添加 HTTP 消息头部，输入参数为头部名字和值的字符串
	bool add_header_pair(const char *name, const char *value)
	{
		return http_parser_add_header(name, strlen(name),
									  value, strlen(value),
									  this->parser) == 0;
	}

	// 设置（替换）HTTP 消息头部，输入参数为 HttpMessageHeader 结构体，包含了头部名字和值
	bool set_header(const struct HttpMessageHeader *header)
	{
		return http_parser_set_header(header->name, header->name_len,
									  header->value, header->value_len,
									  this->parser) == 0;
	}

	// 设置（替换）HTTP 消息头部，输入参数为头部名字和值的字符串
	bool set_header_pair(const char *name, const char *value)
	{
		return http_parser_set_header(name, strlen(name),
									  value, strlen(value),
									  this->parser) == 0;
	}

	// 获取 HTTP 消息的正文部分，输入参数为用于保存正文数据和长度的指针
	bool get_parsed_body(const void **body, size_t *size) const
	{
		return http_parser_get_body(body, size, this->parser) == 0;
	}

	/* Call when the message is incomplete, but you want the parsed body.
	 * If get_parse_body() still returns false after calling this function,
	 * even header is incomplete. In a success state task, messages are
	 * always complete. 
	 * 当消息不完整但你需要解析正文时调用此函数。在成功状态的任务中，消息总是完整的。*/
	void end_parsing()
	{
		http_parser_close_message(this->parser);
	}

	/* Output body is for sending. Want to transfer a message received, maybe:
	 * msg->get_parsed_body(&body, &size);
	 * msg->append_output_body_nocopy(body, size); */
	bool append_output_body(const void *buf, size_t size); 		  // 追加数据到 HTTP 消息的正文部分
	bool append_output_body_nocopy(const void *buf, size_t size); // 追加数据到 HTTP 消息的正文部分
	void clear_output_body();

	// 获取 HTTP 消息正文的大小
	size_t get_output_body_size() const
	{
		return this->output_body_size;
	}

	/* std::string interface */
public:
// 以下方法是对上述方法的重载，接受和返回 std::string 类型的参数，用法与上述方法相同
	bool get_http_version(std::string& version) const
	{
		const char *str = this->get_http_version();

		if (str)
		{
			version.assign(str);
			return true;
		}

		return false;
	}

	bool set_http_version(const std::string& version)
	{
		return this->set_http_version(version.c_str());
	}

	bool add_header_pair(const std::string& name, const std::string& value)
	{
		return http_parser_add_header(name.c_str(), name.size(),
									  value.c_str(), value.size(),
									  this->parser) == 0;
	}

	bool set_header_pair(const std::string& name, const std::string& value)
	{
		return http_parser_set_header(name.c_str(), name.size(),
									  value.c_str(), value.size(),
									  this->parser) == 0;
	}

	bool append_output_body(const std::string& buf)
	{
		return this->append_output_body(buf.c_str(), buf.size());
	}

protected:
	http_parser_t *parser;	// HTTP 消息解析器
	size_t cur_size;

public:
	/* for header visitors. 
	   为头部访问器获取解析器*/
	const http_parser_t *get_parser() const
	{
		return this->parser;
	}

protected:
    // 将 HTTP 消息编码为 iovec 结构体数组，输入参数为 iovec 数组和数组的最大长度
	virtual int encode(struct iovec vectors[], int max);

    // 将给定的缓冲区（buf）解析（追加）到HTTP消息中
	virtual int append(const void *buf, size_t *size);

private:
	// 从给定位置开始，组合指定大小的数据
	struct list_head *combine_from(struct list_head *pos, size_t size);

private:
    // 用于存放 HTTP 消息正文的链表
	struct list_head output_body;

    // HTTP 消息正文的大小
	size_t output_body_size;

public:
	HttpMessage(bool is_resp) : parser(new http_parser_t)
	{
		http_parser_init(is_resp, this->parser);
		INIT_LIST_HEAD(&this->output_body);
		this->output_body_size = 0;
		this->cur_size = 0;
	}

	virtual ~HttpMessage()
	{
		this->clear_output_body();
		if (this->parser)
		{
			http_parser_deinit(this->parser);
			delete this->parser;
		}
	}

	/* for std::move() */
public:
	HttpMessage(HttpMessage&& msg);
	HttpMessage& operator = (HttpMessage&& msg);
};

/*
这个类继承自HttpMessage，并增加了特定于HTTP请求的方法和属性，即对HTTP请求进行封装。
比如设置和获取HTTP方法（GET，POST等），以及设置和获取请求的URI。对于大部分HTTP请求操作，这个类应该提供了你需要的所有功能。
*/
class HttpRequest : public HttpMessage
{
public:
    // 获取请求的HTTP方法，如"GET"，"POST"等
	const char *get_method() const
	{
		return http_parser_get_method(this->parser);
	}

    // 获取请求的URI
	const char *get_request_uri() const
	{
		return http_parser_get_uri(this->parser);
	}

    // 设置请求的HTTP方法，输入参数为方法名，返回值表示设置是否成功
	bool set_method(const char *method)
	{
		return http_parser_set_method(method, this->parser) == 0;
	}

    // 设置请求的URI，输入参数为URI，返回值表示设置是否成功
	bool set_request_uri(const char *uri)
	{
		return http_parser_set_uri(uri, this->parser) == 0;
	}

	/* std::string interface */
public:
    // 以std::string形式获取HTTP方法，返回值表示获取是否成功
	bool get_method(std::string& method) const
	{
		const char *str = this->get_method();

		if (str)
		{
			method.assign(str);
			return true;
		}

		return false;
	}

    // 以std::string形式获取请求的URI，返回值表示获取是否成功
	bool get_request_uri(std::string& uri) const
	{
		const char *str = this->get_request_uri();

		if (str)
		{
			uri.assign(str);
			return true;
		}

		return false;
	}

    // 以std::string形式设置HTTP方法，返回值表示设置是否成功
	bool set_method(const std::string& method)
	{
		return this->set_method(method.c_str());
	}

    // 以std::string形式设置请求的URI，返回值表示设置是否成功
	bool set_request_uri(const std::string& uri)
	{
		return this->set_request_uri(uri.c_str());
	}

protected:
    // 将数据解析(追加) 到HTTP请求，输入参数为数据和数据大小
	virtual int append(const void *buf, size_t *size);

private:
    // 处理期望的继续状态，例如处理HTTP 100 Continue状态
	int handle_expect_continue();

public:
    // 构造函数，初始化一个HttpRequest对象，由于它是请求，所以传入的参数为false
	HttpRequest() : HttpMessage(false) { }

	/* for std::move() */
public:
    // 移动构造函数和移动赋值运算符
	HttpRequest(HttpRequest&& req) = default;
	HttpRequest& operator = (HttpRequest&& req) = default;
};

/*
这个类继承自HttpMessage，并增加了特定于HTTP响应的方法和属性。
比如设置和获取HTTP状态码（如200, 404等），以及设置和获取响应的原因短语（如"OK", "Not Found"等）。
*/
class HttpResponse : public HttpMessage
{
public:
    // 获取HTTP响应的状态码，如"200"，"404"等
	const char *get_status_code() const
	{
		return http_parser_get_code(this->parser);
	}

    // 获取HTTP响应的原因短语，如"OK"，"Not Found"等
	const char *get_reason_phrase() const
	{
		return http_parser_get_phrase(this->parser);
	}

    // 设置HTTP响应的状态码，输入参数为状态码，返回值表示设置是否成功
	bool set_status_code(const char *code)
	{
		return http_parser_set_code(code, this->parser) == 0;
	}

    // 设置HTTP响应的原因短语，输入参数为原因短语，返回值表示设置是否成功
	bool set_reason_phrase(const char *phrase)
	{
		return http_parser_set_phrase(phrase, this->parser) == 0;
	}

	/* 告诉解析器，这是一个没有主体的响应（比如HEAD响应） Tell the parser, it is a HEAD response. */
	void parse_zero_body()
	{
		this->parser->transfer_length = 0;
	}

	/* std::string interface */
public:
    // 以std::string形式获取HTTP响应的状态码，返回值表示获取是否成功	
	bool get_status_code(std::string& code) const
	{
		const char *str = this->get_status_code();

		if (str)
		{
			code.assign(str);
			return true;
		}

		return false;
	}

    // 以std::string形式获取HTTP响应的原因短语，返回值表示获取是否成功
	bool get_reason_phrase(std::string& phrase) const
	{
		const char *str = this->get_reason_phrase();

		if (str)
		{
			phrase.assign(str);
			return true;
		}

		return false;
	}

    // 以std::string形式设置HTTP响应的状态码，返回值表示设置是否成功
	bool set_status_code(const std::string& code)
	{
		return this->set_status_code(code.c_str());
	}

    // 以std::string形式设置HTTP响应的原因短语，返回值表示设置是否成功
	bool set_reason_phrase(const std::string& phrase)
	{
		return this->set_reason_phrase(phrase.c_str());
	}

protected:
    // 向HTTP响应中追加数据，输入参数为数据和数据大小
	virtual int append(const void *buf, size_t *size);

public:
    // 构造函数，初始化一个HttpResponse对象，由于它是响应，所以传入的参数为true
	HttpResponse() : HttpMessage(true) { }

	/* for std::move() */
public:
	HttpResponse(HttpResponse&& resp) = default;
	HttpResponse& operator = (HttpResponse&& resp) = default;
};

}

#endif

