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

#ifndef _COMMREQUEST_H_
#define _COMMREQUEST_H_

#include <stddef.h>
#include "SubTask.h"
#include "Communicator.h"
#include "CommScheduler.h"

// CommRequest类是网络通信请求的基本类，继承自SubTask和CommSession，能处理与网络通信相关的任务。
class CommRequest : public SubTask, public CommSession
{
public:
    // 构造函数，创建一个新的CommRequest对象。
    // object: 与请求关联的CommSchedObject对象。
    // scheduler: 与请求关联的CommScheduler对象。
    CommRequest(CommSchedObject *object, CommScheduler *scheduler)
    {
        LOG_TRACE("CommRequest create"); // 记录创建请求的日志。
        this->scheduler = scheduler;     // 保存CommScheduler对象。
        this->object = object;           // 保存CommSchedObject对象。
        this->wait_timeout = 0;          // 初始化等待超时时间为0。
    }

    // 获取与请求关联的CommSchedObject对象。
    CommSchedObject *get_request_object() const { return this->object; }
    // 设置与请求关联的CommSchedObject对象。
    void set_request_object(CommSchedObject *object) { this->object = object; }
    // 获取等待超时时间。
    int get_wait_timeout() const { return this->wait_timeout; }
    // 设置等待超时时间。
    void set_wait_timeout(int timeout) { this->wait_timeout = timeout; }

public:
    // 虚函数，负责调度任务的具体实现。
    virtual void dispatch();

protected:
    // 请求的状态。
    int state;
    // 请求的错误代码。
    int error;

protected:
    // 请求的目标。
    CommTarget *target;
    // 超时原因的枚举常量。
    #define TOR_NOT_TIMEOUT			0     // 没有超时。
    #define TOR_WAIT_TIMEOUT		1     // 等待超时。
    #define TOR_CONNECT_TIMEOUT		2     // 连接超时。
    #define TOR_TRANSMIT_TIMEOUT	3     // 传输超时。
    // 超时原因。
    int timeout_reason;

protected:
    // 等待超时时间。
    int wait_timeout;
    // 与请求关联的CommSchedObject对象。
    CommSchedObject *object;
    // 与请求关联的CommScheduler对象。
    CommScheduler *scheduler;

protected:
    // 虚函数，处理请求状态和错误的方法。
    virtual void handle(int state, int error);
};

#endif

