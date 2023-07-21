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

#ifndef _SUBTASK_H_
#define _SUBTASK_H_

#include <stddef.h>

class ParallelTask;

// SubTask是所有Task类型的基类
// SubTask要求用户实现两个接口，dispatch和done。
// https://github.com/sogou/workflow/issues/192
class SubTask
{
public:
	// dispatch代表任务的发起，可以是任何行为，
	// 并且要求任务在完成的时候调用subtask_done方法，一般这时候已经不在dispatch线程了。
	// 虚函数，任务开始的接口，具体实现由派生类完成
	virtual void dispatch() = 0;

private:
	// done是任务的完成行为，你可以看到subtask_done里立刻调用了任务的done。
	// done方法把任务交回实现者，实现者要负责内存的回收，比如可能delete this
	// done方法可以返回一个新任务，可以认为原任务转移到新任务并且立刻dispatch，所以return this也是一个常见做法。
	// 虚函数，任务完成的接口，具体实现由派生类完成
	virtual SubTask *done() = 0;

protected:
	void subtask_done();

public:
	// 获取父任务
	ParallelTask *get_parent_task() const { return this->parent; }
	// 获取 pointer 成员
	void *get_pointer() const { return this->pointer; }
	// 设置 pointer 成员
	void set_pointer(void *pointer) { this->pointer = pointer; }

private:
    ParallelTask *parent;  // 指向父任务的指针

	// 指向指针的指针，它存储了一个指向 SubTask 对象指针的地址。在 SubTask 类中是一个用于协作任务执行和控制流程的指针变量。它允许子任务完成后将执行控制权传递给其他任务
    SubTask **entry;       
    void *pointer;         // 指向任意数据的指针，用于存储附加信息

public:
	SubTask()
	{
		this->parent = NULL;
		this->entry = NULL;
		this->pointer = NULL;
	}

	virtual ~SubTask() { }
	friend class ParallelTask;
};

class ParallelTask : public SubTask
{
public:
	ParallelTask(SubTask **subtasks, size_t n)
	{
		this->subtasks = subtasks;
		this->subtasks_nr = n;
	}

	SubTask **get_subtasks(size_t *n) const
	{
		*n = this->subtasks_nr;
		return this->subtasks;
	}

	void set_subtasks(SubTask **subtasks, size_t n)
	{
		this->subtasks = subtasks;
		this->subtasks_nr = n;
	}

public:
	virtual void dispatch();

protected:
	SubTask **subtasks;
	size_t subtasks_nr;

private:
	size_t nleft;
	friend class SubTask;
};

#endif

