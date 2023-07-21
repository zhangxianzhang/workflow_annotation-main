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

  Author: Xie Han (xiehan@sogou-inc.com)
*/

#ifndef _MSGQUEUE_H_
#define _MSGQUEUE_H_

#include <stddef.h>

typedef struct __msgqueue msgqueue_t; // 由两个链表组成的消息队列

/*
在给定的示例中，`__msgqueue` 和 `msgqueue_t` 是相同的，这是一种常见的命名约定。
在某些代码中，`__msgqueue` 可能是一个结构体的原始定义，而 `msgqueue_t` 是对该结构体类型的别名。这种命名约定通常用于隐藏实现细节并提供更简洁的类型名称。
使用别名 `msgqueue_t` 有以下几个优点：
1. **抽象化**: 通过使用别名，将实现细节封装在结构体 `__msgqueue` 中，而 `msgqueue_t` 提供了一个更抽象、更易读的名称，使代码更易理解。
2. **易于修改**: 如果以后需要更改结构体类型的实现，只需修改 `__msgqueue` 的定义，而不必修改所有引用了该类型的代码。通过别名，代码的其他部分不受影响。
3. **代码组织**: 使用别名可以更好地组织代码，并区分抽象接口和底层实现。这有助于提高代码的可维护性和模块化。
请注意，`__msgqueue` 和 `msgqueue_t` 之间的具体关系取决于代码的上下文和编码风格。在不同的代码中，这两个名称可能会有不同的用途和含义。因此，这只是一种常见的命名约定，并没有固定的规定。
*/

#ifdef __cplusplus
extern "C"
{
#endif

/* A simple implementation of message queue. The max pending messages may
 * reach two times 'maxlen' when the queue is in blocking mode, and infinite
 * in nonblocking mode. 'linkoff' is the offset from the head of each message,
 * where spaces of one pointer size should be available for internal usage.
 * 'linkoff' can be positive or negative or zero. */

/*
第一个参数maxlen代表生产队列的最大长度，默认的模式下，达到最大长度时生产者将会阻塞。
第二个参数linkoff是这个模块的一个亮点。
它让用户指定一个消息的偏移量，每条消息的这个位置用户需要预留一个指针大小的空间，用于内部拉链。类似linux内核通过 `struct list_head` 和 `container_of` 宏实现的解耦
这一个简单的设计，避免进出消息队列时，多一次内存分配与释放（因为用户预留一个指针大小空间用于消息队列的连接）。
*/
msgqueue_t *msgqueue_create(size_t maxlen, int linkoff);

void msgqueue_put(void *msg, msgqueue_t *queue);
void *msgqueue_get(msgqueue_t *queue);
void msgqueue_set_nonblock(msgqueue_t *queue);
void msgqueue_set_block(msgqueue_t *queue);
void msgqueue_destroy(msgqueue_t *queue);

#ifdef __cplusplus
}
#endif

#endif

