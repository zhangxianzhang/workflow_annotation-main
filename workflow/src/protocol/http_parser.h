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

#ifndef _HTTP_PARSER_H_
#define _HTTP_PARSER_H_

#include <stddef.h>
#include "list.h"

#define HTTP_HEADER_NAME_MAX	64

typedef struct __http_parser
{
    int header_state;            // 当前解析头部的状态
    int chunk_state;             // 当前解析的是一个chunked编码的消息体的状态
    size_t header_offset;        // 头部信息在输入流中的位置
    size_t chunk_offset;         // 在输入流中的chunk数据的位置
    size_t content_length;       // HTTP消息的`Content-Length`，即消息主体的长度
    size_t transfer_length;      // 在`Transfer-Encoding: chunked`情况下已解析的数据长度
    char *version;               // HTTP的版本，如"HTTP/1.1"
    char *method;                // HTTP请求的方法，如"GET"或"POST"等
    char *uri;                   // 请求的URI，例如"/index.html"
    char *code;                  // HTTP响应的状态码，例如"200"、"404"等
    char *phrase;                // HTTP响应的原因短语，例如"OK"、"Not Found"等
    struct list_head header_list; // 链表，用于存储HTTP消息的头部信息
    char namebuf[HTTP_HEADER_NAME_MAX]; // 字符数组，可能用于存储解析出的头部字段名
    void *msgbuf;                // 存储解析出的HTTP消息体
    size_t msgsize;              // `msgbuf`中的数据长度
    size_t bufsize;              // `msgbuf`的大小
    char expect_continue;        // 是否期望服务器回复100 Continue
    char keep_alive;             // 是否使用keep-alive连接
    char chunked;                // 是否使用chunked传输编码
    char complete;               // HTTP消息是否已完全解析
    char is_resp;                // 该解析器是用于解析请求还是响应
} http_parser_t;

typedef struct __http_header_cursor
{
	const struct list_head *head;
	const struct list_head *next;
} http_header_cursor_t;

#ifdef __cplusplus
extern "C"
{
#endif

void http_parser_init(int is_resp, http_parser_t *parser);
int http_parser_append_message(const void *buf, size_t *n,
							   http_parser_t *parser);
int http_parser_get_body(const void **body, size_t *size,
						 http_parser_t *parser);
int http_parser_header_complete(http_parser_t *parser);
int http_parser_set_method(const char *method, http_parser_t *parser);
int http_parser_set_uri(const char *uri, http_parser_t *parser);
int http_parser_set_version(const char *version, http_parser_t *parser);
int http_parser_set_code(const char *code, http_parser_t *parser);
int http_parser_set_phrase(const char *phrase, http_parser_t *parser);
int http_parser_add_header(const void *name, size_t name_len,
						   const void *value, size_t value_len,
						   http_parser_t *parser);
int http_parser_set_header(const void *name, size_t name_len,
						   const void *value, size_t value_len,
						   http_parser_t *parser);
void http_parser_deinit(http_parser_t *parser);

int http_header_cursor_next(const void **name, size_t *name_len,
							const void **value, size_t *value_len,
							http_header_cursor_t *cursor);
int http_header_cursor_find(const void *name, size_t name_len,
							const void **value, size_t *value_len,
							http_header_cursor_t *cursor);

#ifdef __cplusplus
}
#endif

static inline int http_parser_chunked(http_parser_t *parser)
{
	return parser->chunked;
}

static inline int http_parser_keep_alive(http_parser_t *parser)
{
	return parser->keep_alive;
}

static inline const char *http_parser_get_method(http_parser_t *parser)
{
	return parser->method;
}

static inline const char *http_parser_get_uri(http_parser_t *parser)
{
	return parser->uri;
}

static inline const char *http_parser_get_version(http_parser_t *parser)
{
	return parser->version;
}

static inline const char *http_parser_get_code(http_parser_t *parser)
{
	return parser->code;
}

static inline const char *http_parser_get_phrase(http_parser_t *parser)
{
	return parser->phrase;
}

static inline void http_parser_close_message(http_parser_t *parser)
{
	parser->complete = 1;
}

static inline void http_header_cursor_init(http_header_cursor_t *cursor,
										   const http_parser_t *parser)
{
	cursor->head = &parser->header_list;
	cursor->next = cursor->head;
}

static inline void http_header_cursor_rewind(http_header_cursor_t *cursor)
{
	cursor->next = cursor->head;
}

static inline void http_header_cursor_deinit(http_header_cursor_t *cursor)
{
}

#endif

