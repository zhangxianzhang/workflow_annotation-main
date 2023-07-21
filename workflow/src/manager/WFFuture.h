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

  Authors: Li Yingxin (liyingxin@sogou-inc.com)
*/


/*
 对C++ 标准库中 std::future 和 std::promise 的封装
*/


#ifndef _WFFUTURE_H_
#define _WFFUTURE_H_

#include <future>
#include <chrono>
#include <utility>
#include "WFGlobal.h"

// WFFuture 类模板，封装 std::future
template<typename RES>
class WFFuture
{
public:
    // 带参数的构造函数，使用 std::move 移动构造 std::future 对象
    WFFuture(std::future<RES>&& fr) :
        future(std::move(fr))
    {
    }

    // 默认构造函数
    WFFuture() = default;
    // 删除拷贝构造函数，禁止拷贝
    WFFuture(const WFFuture&) = delete;
    // 默认的移动构造函数
    WFFuture(WFFuture&& move) = default;

    // 删除拷贝赋值运算符，禁止赋值
    WFFuture& operator=(const WFFuture&) = delete;
    // 默认的移动赋值运算符
    WFFuture& operator=(WFFuture&& move) = default;

    // 等待 future 完成的方法
    void wait() const;

    // 等待指定时间，看 future 是否完成
    template<class REP, class PERIOD>
    std::future_status wait_for(const std::chrono::duration<REP, PERIOD>& time_duration) const;

    // 等待至指定的时刻，看 future 是否完成
    template<class CLOCK, class DURATION>
    std::future_status wait_until(const std::chrono::time_point<CLOCK, DURATION>& timeout_time) const;

    // 获取 future 的结果，会先等待 future 完成
    RES get()
    {
        this->wait();
        return this->future.get();
    }

    // 检查 future 是否 valid，即是否有结果可以获取
    bool valid() const { return this->future.valid(); }

private:
    // 封装的 std::future 对象
    std::future<RES> future;
};

// WFPromise 类模板，封装 std::promise
template<typename RES>
class WFPromise
{
public:
    // 默认构造函数
    WFPromise() = default;
    // 删除拷贝构造函数，禁止拷贝
    WFPromise(const WFPromise& promise) = delete;
    // 默认的移动构造函数
    WFPromise(WFPromise&& move) = default;
    // 删除拷贝赋值运算符，禁止赋值
    WFPromise& operator=(const WFPromise& promise) = delete;
    // 默认的移动赋值运算符
    WFPromise& operator=(WFPromise&& move) = default;

    // 从 promise 中获取 future
    WFFuture<RES> get_future()
    {
        return WFFuture<RES>(this->promise.get_future());
    }

    // 设置 promise 的值，后续可以从对应的 future 中获取到
    void set_value(const RES& value) { this->promise.set_value(value); }
    void set_value(RES&& value) { this->promise.set_value(std::move(value)); }

private:
    // 封装的 std::promise 对象
    std::promise<RES> promise;
};

// WFFuture 类模板的 wait 方法实现
template<typename RES>
void WFFuture<RES>::wait() const
{
    // 如果 future 对象不处于 ready 状态
    if (this->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) // std::future_status::ready 表示异步操作已完成。
    {
        // 检查当前线程是否处于任务处理线程（通过key区分这个线程是否是线程池创建的）
        bool in_handler = WFGlobal::get_scheduler()->is_handler_thread();

        // 如果是任务处理线程（是线程池创建的），则调用同步操作开始函数————给线程池扩容，增加处理线程
        if (in_handler)
            WFGlobal::sync_operation_begin();

        // 等待 future 对象变为 ready
        this->future.wait();

        // 如果不是任务处理线程（不是线程池创建的），则调用同步操作结束函数
        if (in_handler)
            WFGlobal::sync_operation_end();
    }
}

// WFFuture 类模板的 wait_for 方法实现
template<typename RES>
template<class REP, class PERIOD>
std::future_status WFFuture<RES>::wait_for(const std::chrono::duration<REP, PERIOD>& time_duration) const
{
    // 默认状态为 ready
    std::future_status status = std::future_status::ready;

    // 如果 future 对象不处于 ready 状态
    if (this->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
    {
        // 检查当前线程是否处于任务处理线程
        bool in_handler = WFGlobal::get_scheduler()->is_handler_thread();

        // 如果是任务处理线程，则调用同步操作开始函数
        if (in_handler)
            WFGlobal::sync_operation_begin();

        // 等待指定的时间，看 future 是否 ready
        status = this->future.wait_for(time_duration);

        // 如果是任务处理线程，则调用同步操作结束函数
        if (in_handler)
            WFGlobal::sync_operation_end();
    }

    // 返回等待状态
    return status;
}


// WFFuture 类模板的 wait_until 方法实现
template<typename RES>
template<class CLOCK, class DURATION>
std::future_status WFFuture<RES>::wait_until(const std::chrono::time_point<CLOCK, DURATION>& timeout_time) const
{
    // 默认状态为 ready
    std::future_status status = std::future_status::ready;

    // 如果 future 对象不处于 ready 状态
    if (this->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
    {
        // 检查当前线程是否处于任务处理线程
        bool in_handler = WFGlobal::get_scheduler()->is_handler_thread();

        // 如果是任务处理线程，则调用同步操作开始函数
        if (in_handler)
            WFGlobal::sync_operation_begin();

        // 等待至指定的时间点，看 future 是否 ready
        status = this->future.wait_until(timeout_time);

        // 如果是任务处理线程，则调用同步操作结束函数
        if (in_handler)
            WFGlobal::sync_operation_end();
    }

    // 返回等待状态
    return status;
}

// 对于 void 类型的特化实现，因为 void 类型的 future 对象在 get 结果时，实际上没有返回值
template<>
inline void WFFuture<void>::get()
{
    // 等待 future 对象变为 ready
    this->wait();

    // 从 future 对象中获取结果（实际上没有返回值）
    this->future.get();
}

// 对于 void 类型的 WFPromise 特化实现
template<>
class WFPromise<void>
{
public:
    // 默认构造函数
    WFPromise() = default;
    // 删除拷贝构造函数，禁止拷贝
    WFPromise(const WFPromise& promise) = delete;
    // 默认的移动构造函数
    WFPromise(WFPromise&& move) = default;
    // 删除拷贝赋值运算符，禁止赋值
    WFPromise& operator=(const WFPromise& promise) = delete;
    // 默认的移动赋值运算符
    WFPromise& operator=(WFPromise&& move) = default;

    // 从 promise 中获取 future
    WFFuture<void> get_future()
    {
        return WFFuture<void>(this->promise.get_future());
    }

    // 设置 promise 的值，由于 RES 类型为 void，所以不需要参数
    void set_value() { this->promise.set_value(); }
    // 对于 void 类型的 promise，不需要设置 value，所以以下两行被注释掉
    // void set_value(const RES& value) { this->promise.set_value(value); }
    // void set_value(RES&& value) { this->promise.set_value(std::move(value)); }

private:
    // 封装的 std::promise 对象
    std::promise<void> promise;
};

#endif

