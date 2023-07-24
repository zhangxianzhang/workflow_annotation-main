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

// HTTP解析器，用于解析HTTP协议的内容
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

// 光标结构，用于追踪在遍历 HTTP 消息头部列表时的当前位置
typedef struct __http_header_cursor
{
	const struct list_head *head; // 指向链表头的指针
	const struct list_head *next; // 指向链表中的下一个元素的指针
} http_header_cursor_t;

#ifdef __cplusplus
extern "C" // 若为 C++ 编译器，则使用 C 语言的编译和链接规则
{
#endif

// HTTP 解析器相关函数声明
void http_parser_init(int is_resp, http_parser_t *parser); // 初始化 HTTP 解析器
int http_parser_append_message(const void *buf, size_t *n, http_parser_t *parser); // 向解析器追加消息
int http_parser_get_body(const void **body, size_t *size, http_parser_t *parser); // 获取 HTTP 消息体
int http_parser_header_complete(http_parser_t *parser); // 判断 HTTP 头部是否解析完成
int http_parser_set_method(const char *method, http_parser_t *parser); // 设置 HTTP 请求方法
int http_parser_set_uri(const char *uri, http_parser_t *parser); // 设置 HTTP 请求 URI
int http_parser_set_version(const char *version, http_parser_t *parser); // 设置 HTTP 版本
int http_parser_set_code(const char *code, http_parser_t *parser); // 设置 HTTP 响应状态码
int http_parser_set_phrase(const char *phrase, http_parser_t *parser); // 设置 HTTP 响应状态短语
int http_parser_add_header(const void *name, size_t name_len, const void *value, size_t value_len, http_parser_t *parser); // 添加 HTTP 头部字段
int http_parser_set_header(const void *name, size_t name_len, const void *value, size_t value_len, http_parser_t *parser); // 设置 HTTP 头部字段
void http_parser_deinit(http_parser_t *parser); // 销毁 HTTP 解析器

// HTTP 头部字段遍历相关函数声明
int http_header_cursor_next(const void **name, size_t *name_len, const void **value, size_t *value_len, http_header_cursor_t *cursor); // 获取下一个 HTTP 头部字段
int http_header_cursor_find(const void *name, size_t name_len, const void **value, size_t *value_len, http_header_cursor_t *cursor); // 查找特定名称的 HTTP 头部字段

#ifdef __cplusplus
}
#endif

// 以下为一系列内联函数，用于操作和获取 http_parser_t 结构的成员

// 函数功能：检查HTTP消息是否使用了分块传输编码(chunked transfer encoding)
static inline int http_parser_chunked(http_parser_t *parser)
{
    return parser->chunked; // 如果使用了分块传输编码，则返回非0值，否则返回0
}

// 函数功能：检查HTTP连接是否为长连接(keep-alive)
static inline int http_parser_keep_alive(http_parser_t *parser)
{
    return parser->keep_alive; // 如果是长连接，则返回非0值，否则返回0
}

// 函数功能：获取HTTP请求的方法，例如GET、POST等
static inline const char *http_parser_get_method(http_parser_t *parser)
{
    return parser->method; // 返回一个指向请求方法的字符串的指针
}

// 函数功能：获取HTTP请求的URI(统一资源标识符)
static inline const char *http_parser_get_uri(http_parser_t *parser)
{
    return parser->uri; // 返回一个指向URI的字符串的指针
}

// 函数功能：获取HTTP的版本，例如HTTP/1.1、HTTP/2等
static inline const char *http_parser_get_version(http_parser_t *parser)
{
    return parser->version; // 返回一个指向版本的字符串的指针
}

// 函数功能：获取HTTP响应的状态码，例如200、404等
static inline const char *http_parser_get_code(http_parser_t *parser)
{
    return parser->code; // 返回一个指向状态码的字符串的指针
}

// 函数功能：获取HTTP响应的状态短语，例如OK、Not Found等
static inline const char *http_parser_get_phrase(http_parser_t *parser)
{
    return parser->phrase; // 返回一个指向状态短语的字符串的指针
}

// 函数功能：将HTTP消息标记为已接收完成
static inline void http_parser_close_message(http_parser_t *parser)
{
    parser->complete = 1; // 将解析器中的完成标记设为1，表示消息已完全接收
}

// 函数功能：初始化HTTP头部字段遍历器
static inline void http_header_cursor_init(http_header_cursor_t *cursor, const http_parser_t *parser)
{
    cursor->head = &parser->header_list; // 将遍历器的头指针设置为解析器中的头部列表的头部
    cursor->next = cursor->head; // 将遍历器的下一个字段指针也设置为头部，表示从头开始遍历
}

// 函数功能：重置HTTP头部字段遍历器，使其重新从头开始遍历
static inline void http_header_cursor_rewind(http_header_cursor_t *cursor)
{
    cursor->next = cursor->head; // 将下一个字段指针重置为头部
}

// 函数功能：销毁HTTP头部字段遍历器，如果遍历器有任何需要手动清理的资源，应在此函数中进行操作
static inline void http_header_cursor_deinit(http_header_cursor_t *cursor)
{
    // 本函数为空，这可能是因为当前的http_header_cursor_t结构体没有需要特别清理的资源
    // 如果后续添加了需要清理的资源，可以在此函数中进行操作
}

#endif

