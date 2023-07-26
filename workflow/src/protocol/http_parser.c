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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "list.h"
#include "http_parser.h"

#define MIN(x, y)	((x) <= (y) ? (x) : (y))
#define MAX(x, y)	((x) >= (y) ? (x) : (y))

#define HTTP_START_LINE_MAX		8192
#define HTTP_HEADER_VALUE_MAX	8192
#define HTTP_CHUNK_LINE_MAX		1024
#define HTTP_TRAILER_LINE_MAX	8192
#define HTTP_MSGBUF_INIT_SIZE	2048

// 表示HTTP消息头解析过程中的状态
enum
{
	HPS_START_LINE,       // 表示解析HTTP的开始行的状态（例如："GET / HTTP/1.1"）
	HPS_HEADER_NAME,      // 表示解析HTTP头部名称的状态（例如："Content-Type"）
	HPS_HEADER_VALUE,     // 表示解析HTTP头部值的状态（例如："text/html"）
	HPS_HEADER_COMPLETE   // 表示HTTP头部解析完成的状态
};

// 表示HTTP消息体解析过程中的状态（主要用于处理分块编码）
enum
{
	CPS_CHUNK_DATA,       // 表示解析块数据的状态
	CPS_TRAILER_PART,     // 表示解析分块数据后的附加头部信息的状态
	CPS_CHUNK_COMPLETE    // 表示一个分块数据解析完成的状态
};

struct __header_line
{
	struct list_head list;
	int name_len;
	int value_len;
	char *buf;
};

int http_parser_add_header(const void *name, size_t name_len,
						   const void *value, size_t value_len,
						   http_parser_t *parser)
{
	size_t size = sizeof (struct __header_line) + name_len + value_len + 4;
	struct __header_line *line;

	line = (struct __header_line *)malloc(size);
	if (line)
	{
		line->buf = (char *)(line + 1);
		memcpy(line->buf, name, name_len);
		line->buf[name_len] = ':';
		line->buf[name_len + 1] = ' ';
		memcpy(line->buf + name_len + 2, value, value_len);
		line->buf[name_len + 2 + value_len] = '\r';
		line->buf[name_len + 2 + value_len + 1] = '\n';
		line->name_len = name_len;
		line->value_len = value_len;
		list_add_tail(&line->list, &parser->header_list);
		return 0;
	}

	return -1;
}

int http_parser_set_header(const void *name, size_t name_len,
						   const void *value, size_t value_len,
						   http_parser_t *parser)
{
	struct __header_line *line;
	struct list_head *pos;
	char *buf;

	list_for_each(pos, &parser->header_list)
	{
		line = list_entry(pos, struct __header_line, list);
		if (line->name_len == name_len &&
			strncasecmp(line->buf, name, name_len) == 0)
		{
			if (value_len > line->value_len)
			{
				buf = (char *)malloc(name_len + value_len + 4);
				if (!buf)
					return -1;

				if (line->buf != (char *)(line + 1))
					free(line->buf);

				line->buf = buf;
				memcpy(buf, name, name_len);
				buf[name_len] = ':';
				buf[name_len + 1] = ' ';
			}

			memcpy(line->buf + name_len + 2, value, value_len);
			line->buf[name_len + 2 + value_len] = '\r';
			line->buf[name_len + 2 + value_len + 1] = '\n';
			line->value_len = value_len;
			return 0;
		}
	}

	return http_parser_add_header(name, name_len, value, value_len, parser);
}

/*
解析HTTP请求消息的起始行时匹配并处理请求方法、URI和HTTP版本
这个函数的主要目的是解析和存储HTTP请求的起始行中的method、uri和version三个部分。
在处理过程中，它首先检查HTTP版本，并根据版本决定是否需要保持连接。然后，它使用strdup函数分别复制method、uri和version这三个字符串。
如果任何一个复制操作失败，函数会释放已经复制的字符串的内存，并返回错误码。
如果所有复制操作都成功，函数会释放解析器中存储旧字符串的内存，并将新的字符串指针赋给解析器，然后返回0表示成功。
*/
static int __match_request_line(const char *method,
								const char *uri,
								const char *version,
								http_parser_t *parser)
{
	// 如果版本是 HTTP/1.0 或者版本号以 HTTP/0 开头，将keep_alive设置为0，表示不保持连接
	if (strcmp(version, "HTTP/1.0") == 0 || strncmp(version, "HTTP/0", 6) == 0)
		parser->keep_alive = 0;

	// 使用strdup复制method字符串，这个函数会为新字符串分配内存
	method = strdup(method);
	if (method)  // 如果分配成功
	{
		// 使用strdup复制uri字符串
		uri = strdup(uri);
		if (uri)  // 如果分配成功
		{
			// 使用strdup复制version字符串
			version = strdup(version);
			if (version)  // 如果分配成功
			{
				// 释放旧的method、uri和version的内存
				free(parser->method);
				free(parser->uri);
				free(parser->version);

				// 将新的method、uri和version赋值给解析器
				parser->method = (char *)method;
				parser->uri = (char *)uri;
				parser->version = (char *)version;

				// 返回0表示成功
				return 0;
			}

			// 如果version的内存分配失败，释放uri的内存
			free((char *)uri);
		}

		// 如果uri或version的内存分配失败，释放method的内存
		free((char *)method);
	}

	// 如果任何内存分配失败，返回-1
	return -1;
}

static int __match_status_line(const char *version,
							   const char *code,
							   const char *phrase,
							   http_parser_t *parser)
{
	if (strcmp(version, "HTTP/1.0") == 0 || strncmp(version, "HTTP/0", 6) == 0)
		parser->keep_alive = 0;

	if (*code == '1' || strcmp(code, "204") == 0 || strcmp(code, "304") == 0)
		parser->transfer_length = 0;

	version = strdup(version);
	if (version)
	{
		code = strdup(code);
		if (code)
		{
			phrase = strdup(phrase);
			if (phrase)
			{
				free(parser->version);
				free(parser->code);
				free(parser->phrase);
				parser->version = (char *)version;
				parser->code = (char *)code;
				parser->phrase = (char *)phrase;
				return 0;
			}

			free((char *)code);
		}

		free((char *)version);
	}

	return -1;
}

/*
用于匹配 HTTP 消息头部的名称和值。
这个函数具体地检查了四种头部字段："Expect", "Connection", "Content-Length", 和 "Transfer-Encoding"
*/
static int __match_message_header(const char *name, size_t name_len,
								  const char *value, size_t value_len,
								  http_parser_t *parser)
{
	// 使用 switch 语句检查头部名称的长度
	switch (name_len)
	{
	case 6:
		// 如果头部名称的长度是6，检查它是否是 "Expect"
		if (strcasecmp(name, "Expect") == 0)
		{
			// 如果头部的值是 "100-continue"，设置 expect_continue 字段为1
			if (strcasecmp(value, "100-continue") == 0)
				parser->expect_continue = 1;
		}
		// 注意：这里没有 break 语句，所以代码会接着检查下一个 case

	case 10:
		// 如果头部名称的长度是10，检查它是否是 "Connection"
		if (strcasecmp(name, "Connection") == 0)
		{
			// 如果头部的值是 "Keep-Alive"，设置 keep_alive 字段为1
			if (strcasecmp(value, "Keep-Alive") == 0)
				parser->keep_alive = 1;
			// 如果头部的值是 "close"，设置 keep_alive 字段为0
			else if (strcasecmp(value, "close") == 0)
				parser->keep_alive = 0;
		}

		break;  // 结束此 case

	case 14:
		// 如果头部名称的长度是14，检查它是否是 "Content-Length"
		if (strcasecmp(name, "Content-Length") == 0)
		{
			// 如果头部的值是一个数字，将它转换为整数，并赋值给 content_length 字段
			if (*value >= '0' && *value <= '9')
				parser->content_length = atoi(value);
		}

		break;  // 结束此 case

	case 17:
		// 如果头部名称的长度是17，检查它是否是 "Transfer-Encoding"
		if (strcasecmp(name, "Transfer-Encoding") == 0)
		{
			// 如果头部的值不是 "identity"，设置 chunked 字段为1
			if (value_len != 8 || strcasecmp(value, "identity") != 0)
				parser->chunked = 1;
		}

		break;  // 结束此 case
	}

	// 将头部的名称和值添加到解析器，如果成功返回0，如果失败返回错误代码
	return http_parser_add_header(name, name_len, value, value_len, parser);
}

/*
解析HTTP消息的起始行
这个函数首先检查是否是一个空行，如果是，则跳过。
然后，函数从消息内容中复制起始行到缓冲区，并在遇到回车符时截断，并进一步找到字符串中的两个空格，将其替换为'\0'。
这样就可以得到起始行中的三部分。接着，根据是请求还是响应调用对应的匹配函数进行匹配。
如果匹配成功，更新头部偏移量和头部状态，并返回1表示解析完成。如果在任何步骤中发生错误，函数会返回错误码。
*/
static int __parse_start_line(const char *ptr, size_t len,
							  http_parser_t *parser)
{
	char start_line[HTTP_START_LINE_MAX];  // 定义一个缓冲区用于存放起始行
	size_t min = MIN(HTTP_START_LINE_MAX, len);  // 取缓冲区大小和剩余长度的较小值
	char *p1, *p2, *p3;  // 用于存放空格分隔后的字符串指针
	size_t i;  // 循环变量
	int ret;  // 存储函数返回值

	// 如果起始行就是一个换行符，则跳过换行符，并返回1表示解析完成
	if (len >= 2 && ptr[0] == '\r' && ptr[1] == '\n')
	{
		parser->header_offset += 2;
		return 1;
	}

	// 遍历需要解析的内容，复制到缓冲区中
	for (i = 0; i < min; i++)
	{
		start_line[i] = ptr[i];
		// 如果碰到回车符，则需要进行进一步的处理
		if (start_line[i] == '\r')
		{
			// 如果回车符是最后一个字符，那么还需要更多的内容才能继续解析
			if (i == len - 1)
				return 0;

			// 如果回车符后面不是换行符，那么消息格式不正确
			if (ptr[i + 1] != '\n')
				return -2;

			// 截断字符串，并找到第一个和第二个空格，将其替换为'\0'
			start_line[i] = '\0';
			p1 = start_line;
			p2 = strchr(p1, ' ');
			if (p2)
				*p2++ = '\0';
			else
				return -2;

			p3 = strchr(p2, ' ');
			if (p3)
				*p3++ = '\0';
			else
				return -2;

			// 如果是响应消息，那么匹配状态行；如果是请求消息，那么匹配请求行
			if (parser->is_resp)
				ret = __match_status_line(p1, p2, p3, parser);
			else
				ret = __match_request_line(p1, p2, p3, parser);

			// 如果匹配失败，返回-1
			if (ret < 0)
				return -1;

			// 更新头部偏移量，并将头部状态设置为HPS_HEADER_NAME，返回1表示解析完成
			parser->header_offset += i + 2;
			parser->header_state = HPS_HEADER_NAME;
			return 1;
		}

		// 如果碰到不可见字符，那么消息格式不正确
		if ((signed char)start_line[i] <= 0)
			return -2;
	}

	// 如果缓冲区已满但还没有解析完，那么消息格式不正确
	if (i == HTTP_START_LINE_MAX)
		return -2;

	// 如果还需要更多的内容才能继续解析，返回0
	return 0;
}

/*
这个函数是用于解析HTTP消息中的头部名称（Header Name）的。
它检查输入缓冲区中的字符并将它们复制到解析器的namebuf字段中，直到达到':'（用于分隔头部名称和值的冒号）或达到缓冲区的末尾。
当找到':'或头部完整时，函数将更新解析器的状态并返回。这个函数的输入包括头部开始的位置（ptr）、其长度（len）以及一个HTTP解析器对象（parser）
*/
static int __parse_header_name(const char *ptr, size_t len,
							   http_parser_t *parser)
{
	// 取待解析消息的长度和预设的最大头部名称长度中的较小值作为待解析的长度。
	size_t min = MIN(HTTP_HEADER_NAME_MAX, len);
	size_t i;

	// 检查待解析的消息是否是一个空行（即头部结束的标志）。如果是，更新解析器的头部偏移值和头部状态，然后返回。
	if (len >= 2 && ptr[0] == '\r' && ptr[1] == '\n')
	{
		parser->header_offset += 2;
		parser->header_state = HPS_HEADER_COMPLETE;
		return 1;
	}

	// 遍历待解析消息的每个字符，检查是否到达了头部名称的结束（即":"），如果到达了，将解析器的名称缓冲区（namebuf）设置为字符串结束符（'\0'），
	// 更新解析器的头部偏移值和头部状态，然后返回。
	for (i = 0; i < min; i++)
	{
		if (ptr[i] == ':')
		{
			parser->namebuf[i] = '\0';
			parser->header_offset += i + 1;
			parser->header_state = HPS_HEADER_VALUE;
			return 1;
		}

		// 如果遇到非法字符，则返回错误。
		if ((signed char)ptr[i] <= 0)
			return -2;

		// 如果当前字符是合法的，就将其复制到解析器的名称缓冲区（namebuf）中。
		parser->namebuf[i] = ptr[i];
	}

	// 如果遍历完预设长度的字符还没有找到头部名称的结束，返回错误。
	if (i == HTTP_HEADER_NAME_MAX)
		return -2;

	// 如果处理完所有字符都没有找到头部名称的结束，那么返回0以继续读取。
	return 0;
}

/*
解析 HTTP 消息头部的值
此函数首先过滤掉头部值前面的空格或制表符，然后开始读取头部值。
如果遇到回车符，它会检查下一个字符是否为换行符，如果是，则继续处理，否则返回错误。
在处理过程中，函数会检查字符的ASCII值是否小于或等于0，如果是，则返回错误。
如果在读取过程中遇到空格或制表符，函数会视它为值的一部分，并用空格替换。
最后，函数会尝试匹配消息头部，如果匹配失败，返回错误。如果成功，更新已解析的字符数，并将头部状态设置为准备解析名称。
*/
static int __parse_header_value(const char *ptr, size_t len,
								http_parser_t *parser)
{
	// 定义一个字符数组，用于保存 HTTP 头部的值，最大长度为 HTTP_HEADER_VALUE_MAX
	char header_value[HTTP_HEADER_VALUE_MAX];

	// 定义一个指针，指向要解析字符串的尾部
	const char *end = ptr + len;

	// 定义一个指针，指向要解析字符串的开始位置
	const char *begin = ptr;

	// 初始化一个计数器 i，用于跟踪当前解析到的字符位置
	size_t i = 0;

	// 开始解析头部的值
	while (1)
	{
		// 去除前导空格和制表符
		while (1)
		{
			// 如果到达了字符串尾部，返回 0 表示需要更多数据
			if (ptr == end)
				return 0;

			// 如果字符是空格或制表符，跳过该字符
			if (*ptr == ' ' || *ptr == '\t')
				ptr++;
			else
				break;  // 如果字符不是空格或制表符，跳出循环
		}

		// 读取头部的值
		while (1)
		{
			// 如果头部的值的长度超过最大限制，返回 -2 表示错误
			if (i == HTTP_HEADER_VALUE_MAX)
				return -2;

			// 读取一个字符
			header_value[i] = *ptr++;

			// 如果到达了字符串尾部，返回 0 表示需要更多数据
			if (ptr == end)
				return 0;

			// 如果读到了回车符，跳出循环
			if (header_value[i] == '\r')
				break;

			// 如果字符的 ASCII 值小于或等于 0，返回 -2 表示错误
			if ((signed char)header_value[i] <= 0)
				return -2;

			i++;  // 递增计数器
		}

		// 如果下一个字符是换行符，跳过该字符，否则返回 -2 表示错误
		if (*ptr == '\n')
			ptr++;
		else
			return -2;

		// 如果到达了字符串尾部，返回 0 表示需要更多数据
		if (ptr == end)
			return 0;

		// 去除尾部的空格和制表符
		while (i > 0)
		{
			if (header_value[i - 1] == ' ' || header_value[i - 1] == '\t')
				i--;
			else
				break;  // 如果字符不是空格或制表符，跳出循环
		}

		// 如果下一个字符不是空格和制表符，跳出循环
		if (*ptr != ' ' && *ptr != '\t')
			break;

		// 如果下一个字符是空格或制表符，将它视为值的一部分，并用空格替换
		ptr++;
		header_value[i++] = ' ';
	}

	// 将头部的值的尾部设为 null 字符，形成一个 C 字符串
	header_value[i] = '\0';

	// 尝试匹配消息头部，如果匹配失败，返回 -1
	if (__match_message_header(parser->namebuf, strlen(parser->namebuf),
							   header_value, i, parser) < 0)
		return -1;

	// 更新已解析的字符数
	parser->header_offset += ptr - begin;

	// 将头部状态设置为准备解析名称
	parser->header_state = HPS_HEADER_NAME;

	// 返回 1 表示解析成功
	return 1;
}

// 解析HTTP消息的头部。依据当前的头部状态（由parser->header_state决定），调用对应的解析函数进行解析。在解析函数返回值大于0（表示解析成功）的情况下，函数会继续解析。当头部解析完成，函数会返回1。否则，返回最后一个解析函数的返回值。
static int __parse_message_header(const void *message, size_t size,
								  http_parser_t *parser)
{
	const char *ptr;  // 指向当前需要解析的头部的位置的指针
	size_t len;  // 剩余需要解析的长度
	int ret;  // 存储各个解析函数的返回值

	do
	{
		ptr = (const char *)message + parser->header_offset;  // 计算当前需要解析的位置
		len = size - parser->header_offset;  // 计算剩余需要解析的长度
		if (parser->header_state == HPS_START_LINE)  // 如果头部状态为起始行
			ret = __parse_start_line(ptr, len, parser);  // 解析起始行
		else if (parser->header_state == HPS_HEADER_VALUE)  // 如果头部状态为头部值
			ret = __parse_header_value(ptr, len, parser);  // 解析头部值
		else /* if (parser->header_state == HPS_HEADER_NAME) */  // 如果头部状态为头部名字
		{
			ret = __parse_header_name(ptr, len, parser);  // 解析头部名字
			// 如果解析头部名字后，头部状态变为头部解析完成
			if (parser->header_state == HPS_HEADER_COMPLETE)
				return 1;  // 返回1表示头部解析完成
		}
	} while (ret > 0);  // 当解析函数返回值大于0时，继续解析

	return ret;  // 返回最后一个解析函数的返回值
}

#define CHUNK_SIZE_MAX		(2 * 1024 * 1024 * 1024U - HTTP_CHUNK_LINE_MAX - 4)

static int __parse_chunk_data(const char *ptr, size_t len,
							  http_parser_t *parser)
{
	char chunk_line[HTTP_CHUNK_LINE_MAX];
	size_t min = MIN(HTTP_CHUNK_LINE_MAX, len);
	long chunk_size;
	char *end;
	size_t i;

	for (i = 0; i < min; i++)
	{
		chunk_line[i] = ptr[i];
		if (chunk_line[i] == '\r')
		{
			if (i == len - 1)
				return 0;

			if (ptr[i + 1] != '\n')
				return -2;

			chunk_line[i] = '\0';
			chunk_size = strtol(chunk_line, &end, 16);
			if (end == chunk_line)
				return -2;

			if (chunk_size == 0)
			{
				chunk_size = i + 2;
				parser->chunk_state = CPS_TRAILER_PART;
			}
			else if ((unsigned long)chunk_size < CHUNK_SIZE_MAX)
			{
				chunk_size += i + 4;
				if (len < (size_t)chunk_size)
					return 0;
			}
			else
				return -2;

			parser->chunk_offset += chunk_size;
			return 1;
		}
	}

	if (i == HTTP_CHUNK_LINE_MAX)
		return -2;

	return 0;
}

static int __parse_trailer_part(const char *ptr, size_t len,
								http_parser_t *parser)
{
	size_t min = MIN(HTTP_TRAILER_LINE_MAX, len);
	size_t i;

	for (i = 0; i < min; i++)
	{
		if (ptr[i] == '\r')
		{
			if (i == len - 1)
				return 0;

			if (ptr[i + 1] != '\n')
				return -2;

			parser->chunk_offset += i + 2;
			if (i == 0)
				parser->chunk_state = CPS_CHUNK_COMPLETE;

			return 1;
		}
	}

	if (i == HTTP_TRAILER_LINE_MAX)
		return -2;

	return 0;
}

static int __parse_chunk(const void *message, size_t size,
						 http_parser_t *parser)
{
	const char *ptr;
	size_t len;
	int ret;

	do
	{
		ptr = (const char *)message + parser->chunk_offset;
		len = size - parser->chunk_offset;
		if (parser->chunk_state == CPS_CHUNK_DATA)
			ret = __parse_chunk_data(ptr, len, parser);
		else /* if (parser->chunk_state == CPS_TRAILER_PART) */
		{
			ret = __parse_trailer_part(ptr, len, parser);
			if (parser->chunk_state == CPS_CHUNK_COMPLETE)
				return 1;
		}
	} while (ret > 0);

	return ret;
}

void http_parser_init(int is_resp, http_parser_t *parser)
{
	parser->header_state = HPS_START_LINE;
	parser->header_offset = 0;
	parser->transfer_length = (size_t)-1;
	parser->content_length = is_resp ? (size_t)-1 : 0;
	parser->version = NULL;
	parser->method = NULL;
	parser->uri = NULL;
	parser->code = NULL;
	parser->phrase = NULL;
	INIT_LIST_HEAD(&parser->header_list);
	parser->msgbuf = NULL;
	parser->msgsize = 0;
	parser->bufsize = 0;
	parser->expect_continue = 0;
	parser->keep_alive = 1;
	parser->chunked = 0;
	parser->complete = 0;
	parser->is_resp = is_resp;
}

// 确保消息适应其内部缓冲区，并根据消息类型执行相应的解析。返回0，表示还需要继续接收
int http_parser_append_message(const void *buf, size_t *n,
							   http_parser_t *parser)
{
    int ret;

    // 检查解析器是否已完成。如果已完成，将输入大小设置为0，并返回1
    if (parser->complete)
    {
        *n = 0;
        return 1;
    }

    // 如果消息大小超出了当前缓冲区的大小，需要重新分配内存
    if (parser->msgsize + *n + 1 > parser->bufsize)
    {
        size_t new_size = MAX(HTTP_MSGBUF_INIT_SIZE, 2 * parser->bufsize);
        void *new_base;

        // 通过循环将缓冲区大小翻倍，直到满足新的需求
        while (new_size < parser->msgsize + *n + 1)
            new_size *= 2;

        // 重新分配内存，并检查是否成功
        new_base = realloc(parser->msgbuf, new_size);
        if (!new_base)
            return -1;

        // 如果成功，更新缓冲区指针和大小
        parser->msgbuf = new_base;
        parser->bufsize = new_size;
    }

    // 将新的数据拷贝到消息缓冲区，并更新消息大小
    memcpy((char *)parser->msgbuf + parser->msgsize, buf, *n);
    parser->msgsize += *n;

    // 如果消息头还没有解析完成，尝试解析消息头
    if (parser->header_state != HPS_HEADER_COMPLETE)
    {
        ret = __parse_message_header(parser->msgbuf, parser->msgsize, parser);
        if (ret <= 0)
            return ret;

        // 根据消息头的信息，更新解析器的状态
        if (parser->chunked)
        {
            parser->chunk_offset = parser->header_offset;
            parser->chunk_state = CPS_CHUNK_DATA;
        }
        else if (parser->transfer_length == (size_t)-1)
            parser->transfer_length = parser->content_length;
    }

    // 如果存在明确的传输长度，检查是否已经接收完毕
    if (parser->transfer_length != (size_t)-1)
    {
        size_t total = parser->header_offset + parser->transfer_length;

        // 如果已经接收完毕，更新输入大小，截断多余的数据，设置完成标志
        if (parser->msgsize >= total)
        {
            *n -= parser->msgsize - total;
            parser->msgsize = total;
            parser->complete = 1;
            return 1;
        }

        // 否则返回0，表示还需要继续接收
        return 0;
    }

    // 如果不是分块传输，返回0
    if (!parser->chunked)
        return 0;

    // 如果分块数据还没有接收完毕，尝试解析分块数据
    if (parser->chunk_state != CPS_CHUNK_COMPLETE)
    {
        ret = __parse_chunk(parser->msgbuf, parser->msgsize, parser);
        if (ret <= 0)
            return ret;
    }

    // 根据最后一个块的偏移，更新输入大小，截断多余的数据，设置完成标志
    *n -= parser->msgsize - parser->chunk_offset;
    parser->msgsize = parser->chunk_offset;
    parser->complete = 1;
    return 1;
}
int http_parser_header_complete(http_parser_t *parser)
{
	return parser->header_state == HPS_HEADER_COMPLETE;
}

int http_parser_get_body(const void **body, size_t *size,
						 http_parser_t *parser)
{
	if (parser->complete && parser->header_state == HPS_HEADER_COMPLETE)
	{
		*body = (char *)parser->msgbuf + parser->header_offset;
		*size = parser->msgsize - parser->header_offset;
		((char *)parser->msgbuf)[parser->msgsize] = '\0';
		return 0;
	}

	return 1;
}

int http_parser_set_method(const char *method, http_parser_t *parser)
{
	method = strdup(method);
	if (method)
	{
		free(parser->method);
		parser->method = (char *)method;
		return 0;
	}

	return -1;
}

int http_parser_set_uri(const char *uri, http_parser_t *parser)
{
	uri = strdup(uri);
	if (uri)
	{
		free(parser->uri);
		parser->uri = (char *)uri;
		return 0;
	}

	return -1;
}

int http_parser_set_version(const char *version, http_parser_t *parser)
{
	version = strdup(version);
	if (version)
	{
		free(parser->version);
		parser->version = (char *)version;
		return 0;
	}

	return -1;
}

int http_parser_set_code(const char *code, http_parser_t *parser)
{
	code = strdup(code);
	if (code)
	{
		free(parser->code);
		parser->code = (char *)code;
		return 0;
	}

	return -1;
}

int http_parser_set_phrase(const char *phrase, http_parser_t *parser)
{
	phrase = strdup(phrase);
	if (phrase)
	{
		free(parser->phrase);
		parser->phrase = (char *)phrase;
		return 0;
	}

	return -1;
}

void http_parser_deinit(http_parser_t *parser)
{
	struct __header_line *line;
	struct list_head *pos, *tmp;

	list_for_each_safe(pos, tmp, &parser->header_list)
	{
		line = list_entry(pos, struct __header_line, list);
		list_del(pos);
		if (line->buf != (char *)(line + 1))
			free(line->buf);

		free(line);
	}

	free(parser->version);
	free(parser->method);
	free(parser->uri);
	free(parser->code);
	free(parser->phrase);
	free(parser->msgbuf);
}

// 尝试获取指向头部列表中下一个元素的指针
int http_header_cursor_next(const void **name, size_t *name_len,
							const void **value, size_t *value_len,
							http_header_cursor_t *cursor)
{
	struct __header_line *line;

	if (cursor->next->next != cursor->head)
	{
		cursor->next = cursor->next->next;
		line = list_entry(cursor->next, struct __header_line, list);
		*name = line->buf;
		*name_len = line->name_len;
		*value = line->buf + line->name_len + 2;
		*value_len = line->value_len;
		return 0;
	}

	return 1;
}

int http_header_cursor_find(const void *name, size_t name_len,
							const void **value, size_t *value_len,
							http_header_cursor_t *cursor)
{
	struct __header_line *line;

	while (cursor->next->next != cursor->head)
	{
		cursor->next = cursor->next->next;
		line = list_entry(cursor->next, struct __header_line, list);
		if (line->name_len == name_len)
		{
			if (strncasecmp(line->buf, name, name_len) == 0)
			{
				*value = line->buf + name_len + 2;
				*value_len = line->value_len;
				return 0;
			}
		}
	}

	return 1;
}

