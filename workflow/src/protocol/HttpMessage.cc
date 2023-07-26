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
#include <string.h>
#include <utility>
#include "HttpMessage.h"

namespace protocol
{

struct HttpMessageBlock
{
	struct list_head list;
	const void *ptr;
	size_t size;
};

bool HttpMessage::append_output_body(const void *buf, size_t size)
{
	size_t n = sizeof (struct HttpMessageBlock) + size;
	struct HttpMessageBlock *block = (struct HttpMessageBlock *)malloc(n);

	if (block)
	{
		memcpy(block + 1, buf, size);
		block->ptr = block + 1;
		block->size = size;
		list_add_tail(&block->list, &this->output_body);
		this->output_body_size += size;
		return true;
	}

	return false;
}

bool HttpMessage::append_output_body_nocopy(const void *buf, size_t size)
{
	size_t n = sizeof (struct HttpMessageBlock);
	struct HttpMessageBlock *block = (struct HttpMessageBlock *)malloc(n);

	if (block)
	{
		block->ptr = buf;
		block->size = size;
		list_add_tail(&block->list, &this->output_body);
		this->output_body_size += size;
		return true;
	}

	return false;
}

void HttpMessage::clear_output_body()
{
	struct HttpMessageBlock *block;
	struct list_head *pos, *tmp;

	list_for_each_safe(pos, tmp, &this->output_body)
	{
		block = list_entry(pos, struct HttpMessageBlock, list);
		list_del(pos);
		free(block);
	}

	this->output_body_size = 0;
}

struct list_head *HttpMessage::combine_from(struct list_head *pos, size_t size)
{
	size_t n = sizeof (struct HttpMessageBlock) + size;
	struct HttpMessageBlock *block = (struct HttpMessageBlock *)malloc(n);
	struct HttpMessageBlock *entry;
	char *ptr;

	if (block)
	{
		block->ptr = block + 1;
		block->size = size;
		ptr = (char *)block->ptr;

		do
		{
			entry = list_entry(pos, struct HttpMessageBlock, list);
			pos = pos->next;
			list_del(&entry->list);
			memcpy(ptr, entry->ptr, entry->size);
			ptr += entry->size;
			free(entry);
		} while (pos != &this->output_body);

		list_add_tail(&block->list, &this->output_body);
		return &block->list;
	}

	return NULL;
}

/* 将HTTP消息编码为一个IO向量（iovec结构体数组）。
此函数首先将HTTP消息的起始行（请求行或状态行）和头部编码到向量数组中，然后将消息体编码到向量数组中。
如果iovec数组的容量不足，函数会尝试合并剩余的消息体块。如果合并失败，函数返回错误
*/
int HttpMessage::encode(struct iovec vectors[], int max)
{
	const char *start_line[3]; 		// start_line数组用来存储HTTP消息的起始行（请求行或状态行）的各个部分
	http_header_cursor_t cursor; 	// cursor用来遍历HTTP消息的头部
	struct HttpMessageHeader header;// header用来存储当前遍历到的头部字段
	struct HttpMessageBlock *block; // block用来存储当前遍历到的消息体的块
	struct list_head *pos;			// pos用来在消息体的链表中进行遍历
	size_t size;					// size用来存储消息体的大小
	int i;							// i用来存储当前编码到的向量的索引

	// 获取HTTP请求/响应的方法、URI和版本信息
	start_line[0] = http_parser_get_method(this->parser);
	if (start_line[0])
	{
		start_line[1] = http_parser_get_uri(this->parser);
		start_line[2] = http_parser_get_version(this->parser);
	}
	else // 如果请求方法为NULL，那么这是一个HTTP响应，获取版本、状态码和状态短语
	{
		start_line[0] = http_parser_get_version(this->parser);
		start_line[1] = http_parser_get_code(this->parser);
		start_line[2] = http_parser_get_phrase(this->parser);
	}

	// 如果任何一个起始行的元素为空，则返回错误
	if (!start_line[0] || !start_line[1] || !start_line[2])
	{
		errno = EBADMSG;
		return -1;
	}

	// 把起始行的内容添加到iovec数组中，每一个元素之间用空格分隔
	vectors[0].iov_base = (void *)start_line[0];
	vectors[0].iov_len = strlen(start_line[0]);
	vectors[1].iov_base = (void *)" ";
	vectors[1].iov_len = 1;

	vectors[2].iov_base = (void *)start_line[1];
	vectors[2].iov_len = strlen(start_line[1]);
	vectors[3].iov_base = (void *)" ";
	vectors[3].iov_len = 1;

	vectors[4].iov_base = (void *)start_line[2];
	vectors[4].iov_len = strlen(start_line[2]);
	vectors[5].iov_base = (void *)"\r\n";
	vectors[5].iov_len = 2;

	i = 6;
	// 用一个cursor来遍历HTTP消息的头部字段
	http_header_cursor_init(&cursor, this->parser);
	while (http_header_cursor_next(&header.name, &header.name_len,
								   &header.value, &header.value_len,
								   &cursor) == 0)
	{
		// 如果iovec数组已经达到了它的容量限制，就退出循环
		if (i == max)
			break;

		// 把当前的HTTP头部字段添加到iovec数组中
		vectors[i].iov_base = (void *)header.name;
		vectors[i].iov_len = header.name_len + 2 + header.value_len + 2;
		i++;
	}

	http_header_cursor_deinit(&cursor);
	// 如果iovec数组的容量不足以容纳所有的HTTP头部字段和消息体，返回错误
	if (i + 1 >= max)
	{
		errno = EOVERFLOW;
		return -1;
	}

	// 添加一个空行，表示HTTP头部字段的结束
	vectors[i].iov_base = (void *)"\r\n";
	vectors[i].iov_len = 2;
	i++;

	// 获取消息体的大小，遍历消息体的每一个块，添加到iovec数组中
	size = this->output_body_size;
	list_for_each(pos, &this->output_body)
	{
		// 如果iovec数组的容量已经达到了它的上限，并且还有剩余的消息体块未处理，尝试合并剩余的块
		if (i + 1 == max && pos != this->output_body.prev)
		{
			pos = this->combine_from(pos, size);
			if (!pos)
				return -1;
		}

		// 把当前的消息体块添加到iovec数组中
		block = list_entry(pos, struct HttpMessageBlock, list);
		vectors[i].iov_base = (void *)block->ptr;
		vectors[i].iov_len = block->size;
		size -= block->size;
		i++;
	}

	// 返回iovec数组的长度（元素的数量）
	return i;
}

inline int HttpMessage::append(const void *buf, size_t *size)
{
	int ret = http_parser_append_message(buf, size, this->parser);

	if (ret >= 0) // 如果返回值大于等于0，表示追加操作成功。
	{
		// 更新当前消息的大小。
		this->cur_size += *size;

		// 如果当前消息的大小超过了预设的大小限制，设置错误码为EMSGSIZE（消息过长），并将返回值设为-1，表示追加操作失败。
		if (this->cur_size > this->size_limit)
		{
			errno = EMSGSIZE;
			ret = -1;
		}
	}
	else if (ret == -2) // 如果返回值为-2，表示消息格式错误。
	{
		// 设置错误码为EBADMSG（错误的消息格式），并将返回值设为-1，表示追加操作失败。
		errno = EBADMSG;
		ret = -1;
	}

	// 返回追加操作的结果。如果ret为-1，表示追加操作失败；如果ret为0或1，表示追加操作成功。
	return ret;
}

HttpMessage::HttpMessage(HttpMessage&& msg) :
	ProtocolMessage(std::move(msg))
{
	this->parser = msg.parser;
	msg.parser = NULL;

	INIT_LIST_HEAD(&this->output_body);
	list_splice_init(&msg.output_body, &this->output_body);
	this->output_body_size = msg.output_body_size;
	msg.output_body_size = 0;

	this->cur_size = msg.cur_size;
	msg.cur_size = 0;
}

HttpMessage& HttpMessage::operator = (HttpMessage&& msg)
{
	if (&msg != this)
	{
		*(ProtocolMessage *)this = std::move(msg);

		if (this->parser)
		{
			http_parser_deinit(this->parser);
			delete this->parser;
		}

		this->parser = msg.parser;
		msg.parser = NULL;

		this->clear_output_body();
		list_splice_init(&msg.output_body, &this->output_body);
		this->output_body_size = msg.output_body_size;
		msg.output_body_size = 0;

		this->cur_size = msg.cur_size;
		msg.cur_size = 0;
	}

	return *this;
}

#define HTTP_100_STATUS_LINE	"HTTP/1.1 100 Continue"
#define HTTP_400_STATUS_LINE	"HTTP/1.1 400 Bad Request"
#define HTTP_413_STATUS_LINE	"HTTP/1.1 413 Request Entity Too Large"
#define HTTP_417_STATUS_LINE	"HTTP/1.1 417 Expectation Failed"
#define CONTENT_LENGTH_ZERO		"Content-Length: 0"
#define CONNECTION_CLOSE		"Connection: close"
#define CRLF					"\r\n"

#define HTTP_100_RESP			HTTP_100_STATUS_LINE CRLF \
								CRLF
#define HTTP_400_RESP			HTTP_400_STATUS_LINE CRLF \
								CONTENT_LENGTH_ZERO CRLF \
								CONNECTION_CLOSE CRLF \
								CRLF
#define HTTP_413_RESP			HTTP_413_STATUS_LINE CRLF \
								CONTENT_LENGTH_ZERO CRLF \
								CONNECTION_CLOSE CRLF \
								CRLF
#define HTTP_417_RESP			HTTP_417_STATUS_LINE CRLF \
								CONTENT_LENGTH_ZERO CRLF \
								CONNECTION_CLOSE CRLF \
								CRLF

/*
它处理了期待连续数据的HTTP请求，主要是处理HTTP/1.1协议中定义的Expect: 100-continue请求头。
这个函数主要处理了两种情况：一种是客户端的传输长度超过了服务器的大小限制，这种情况下，服务器会给出HTTP 417的响应并设置错误号为EMSGSIZE；
另一种是服务器成功处理了Expect请求头，这种情况下，服务器会给出HTTP 100的响应。如果在发送响应的过程中发生错误，函数会返回-1并设置相应的错误号。
*/
int HttpRequest::handle_expect_continue()
{
	// 提取传输长度信息
	size_t trans_len = this->parser->transfer_length;

	int ret;

	// 如果传输长度信息有效
	if (trans_len != (size_t)-1)
	{
		// 如果头部偏移加上传输长度超过了设定的大小限制
		if (this->parser->header_offset + trans_len > this->size_limit)
		{
			// 给出HTTP 417的响应，417状态码表示服务器无法满足 Expect 请求头字段的期望
			this->feedback(HTTP_417_RESP, strlen(HTTP_417_RESP));
			// 设置错误号为消息过长
			errno = EMSGSIZE;
			// 返回-1表示处理失败
			return -1;
		}
	}

	// 给出HTTP 100的响应，100状态码表示继续，客户端应继续其请求
	ret = this->feedback(HTTP_100_RESP, strlen(HTTP_100_RESP));
	// 如果实际发送的响应长度和预期的响应长度不匹配
	if (ret != strlen(HTTP_100_RESP))
	{
		// 如果返回结果为非负值，即发送了部分响应，设置错误号为EAGAIN（资源暂时不可用）
		if (ret >= 0)
			errno = EAGAIN;
		// 返回-1表示处理失败
		return -1;
	}

	// 返回0表示处理成功
	return 0;
}

/*
将数据解析(追加) 到HTTP请求，输入参数为数据和数据大小
如果返回值为-1，表示追加操作失败；如果返回值为0或1，表示追加操作成功。
如果请求期待连续的数据并且请求头已经完全接收，函数将处理连续数据的情况并返回处理结果。如果追加操作失败，函数会根据错误码发送相应的HTTP错误响应。
*/
int HttpRequest::append(const void *buf, size_t *size)
{
	// 调用基类HttpMessage的append方法，将缓冲区信息解析（追加—）到HTTP请求中。
	// append方法返回一个整数，代表操作的结果。如果返回值大于等于0，说明追加操作成功；如果返回值小于0，说明追加操作失败。
	int ret = HttpMessage::append(buf, size);

	// 检查HttpMessage::append的返回结果。
	if (ret == 0) // 如果返回值为0，表示追加操作成功，但消息尚未接收完全。
	{
		// 如果请求期待有连续的数据，并且请求头已经完全接收
		if (this->parser->expect_continue && http_parser_header_complete(this->parser))
		{
			// 设置期待连续数据的标志位为0
			this->parser->expect_continue = 0;
			// 处理期待连续数据的情况，并将处理结果作为返回值
			ret = this->handle_expect_continue();
		}
	}
	else if (ret < 0) // 如果返回值小于0，表示追加操作失败。
	{
		// 根据错误码来处理错误。这里主要处理了两种错误：
		// 1. 如果错误码为EBADMSG（错误的消息格式），调用feedback方法发送HTTP 400错误响应；
		// 2. 如果错误码为EMSGSIZE（消息过长），调用feedback方法发送HTTP 413错误响应。
		if (errno == EBADMSG)
			this->feedback(HTTP_400_RESP, strlen(HTTP_400_RESP));
		else if (errno == EMSGSIZE)
			this->feedback(HTTP_413_RESP, strlen(HTTP_413_RESP));
	}

	// 返回追加操作的结果。如果ret为-1，表示追加操作失败；如果ret为0或1，表示追加操作成功。
	return ret;
}
int HttpResponse::append(const void *buf, size_t *size)
{
	int ret = HttpMessage::append(buf, size);

	if (ret > 0)
	{
		if (strcmp(http_parser_get_code(this->parser), "100") == 0)
		{
			http_parser_deinit(this->parser);
			http_parser_init(1, this->parser);
			ret = 0;
		}
	}

	return ret;
}

}

