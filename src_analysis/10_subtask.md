18#! https://zhuanlan.zhihu.com/p/462702955
# workflow 源码解析 : SubTask

更加详细的源码注释可看 : https://github.com/chanchann/workflow_annotation

## SubTask结构
在给定的代码中，`SubTask` 类有三个私有成员变量：`parent`、`entry` 和 `pointer`。下面是对它们的注释：

```cpp
class SubTask {
private:
    ParallelTask *parent;  // 指向父任务的指针
    SubTask **entry;       // 指向指针的指针，用于任务间的协作
    void *pointer;         // 指向任意数据的指针，用于存储附加信息

public:
    // 其他成员函数和构造函数...

};
```

- `ParallelTask *parent;`：指向父任务的指针
  - 这个指针变量用于指向该子任务所属的父任务，用于在需要时获取或操作父任务的相关信息。

- `SubTask **entry;`：指向指针的指针，用于任务间的协作
  - `entry` 是一个指向指针的指针，用于在任务之间传递指针，并实现任务的协作和控制流程。
  - 在某个子任务完成时，可以通过修改 `entry` 中的指针值，将执行控制权传递给下一个需要执行的子任务。

- `void *pointer;`：指向任意数据的指针，用于存储附加信息
  - `pointer` 是一个 `void` 类型的指针，可以指向任意类型的数据。
  - 它用于存储附加信息或上下文数据，以便在任务执行过程中进行传递和使用。

这些私有成员变量在 `SubTask` 类中起到不同的作用，用于实现任务间的协作、父子任务关系的管理，以及附加信息的传递。它们为任务的执行提供了灵活性和可扩展性。

在给定的代码中，`entry` 是 `SubTask` 类的私有成员变量，它是一个指向指针的指针。

在 `SubTask` 类中，`entry` 主要用于协作并行任务（`ParallelTask`）的执行和完成。以下是关于 `entry` 变量的解释：

1. **定义和类型**：`SubTask **entry;`
   - `entry` 是一个指向指针的指针，它存储了一个指向 `SubTask` 对象指针的地址。

2. **作用和用途**：
   - `entry` 在协作任务执行过程中起到重要的作用。它被用来传递指向当前子任务的指针，并通过该指针来实现任务的完成和转移。
   - 当子任务完成时，通过修改 `entry` 中的指针，可以将执行控制权传递给其他子任务，或者回到父任务（`ParallelTask`）。

3. **使用方式**：
   - 在任务完成时，调用 `subtask_done()` 方法，该方法将会修改 `entry` 中的指针值，以指向下一个需要执行的子任务。
   - 通过 `entry` 的间接引用，可以在任务执行的上下文中访问和修改子任务指针。
   - `entry` 可能会被用来实现任务链的连续执行，即完成一个子任务后，立即开始执行下一个子任务，以实现并行或串行的任务调度。

总结而言，`entry` 在 `SubTask` 类中是一个用于协作任务执行和控制流程的指针变量。它允许子任务完成后将执行控制权传递给其他任务，并且在任务链中起到了重要的作用。

好的，让我们尝试通过一个更具体的示例来解释 `entry` 的作用和用途。

假设我们有一个并行下载器的任务，需要下载多个文件，并在下载完成后进行一些处理。我们可以使用 `ParallelTask` 类来管理并发下载任务，并使用 `SubTask` 类来表示每个下载任务。

```cpp
#include <iostream>
#include <vector>

class SubTask {
public:
    virtual void dispatch() = 0;
    virtual SubTask* done() = 0;

    void set_entry(SubTask** entry) {
        this->entry = entry;
    }

protected:
    void subtask_done() {
        if (entry && *entry) {
            (*entry)->dispatch();
        }
    }

private:
    SubTask** entry;
};

class DownloadTask : public SubTask {
public:
    explicit DownloadTask(const std::string& url) : url(url) {}

    void dispatch() override {
        std::cout << "开始下载：" << url << std::endl;
        // 下载操作的具体实现
        // ...
        std::cout << "下载完成：" << url << std::endl;
        subtask_done();  // 下载完成后触发下一个任务
    }

    SubTask* done() override {
        return nullptr;  // 没有下一个任务，返回 nullptr
    }

private:
    std::string url;
};

class ProcessTask : public SubTask {
public:
    explicit ProcessTask(const std::string& filename) : filename(filename) {}

    void dispatch() override {
        std::cout << "开始处理文件：" << filename << std::endl;
        // 文件处理操作的具体实现
        // ...
        std::cout << "文件处理完成：" << filename << std::endl;
        subtask_done();  // 文件处理完成后触发下一个任务
    }

    SubTask* done() override {
        return nullptr;  // 没有下一个任务，返回 nullptr
    }

private:
    std::string filename;
};

class ParallelTask {
public:
    void add_subtask(SubTask* subtask) {
        subtasks.push_back(subtask);
    }

    void execute() {
        if (subtasks.empty()) {
            return;
        }

        for (size_t i = 0; i < subtasks.size() - 1; ++i) {
            subtasks[i]->set_entry(&subtasks[i + 1]);
        }

        subtasks[0]->dispatch();
    }

private:
    std::vector<SubTask*> subtasks;
};

int main() {
    ParallelTask parallelTask;

    // 创建下载任务
    DownloadTask downloadTask1("https://example.com/file1.txt");
    DownloadTask downloadTask2("https://example.com/file2.txt");

    // 创建文件处理任务
    ProcessTask processTask1("file1.txt");
    ProcessTask processTask2("file2.txt");

    // 添加任务到并行任务中
    parallelTask.add_subtask(&downloadTask1);
    parallelTask.add_subtask(&downloadTask2);
    parallelTask.add_subtask(&processTask1);
    parallelTask.add_subtask(&processTask2);

    // 执行并行任务
    parallelTask.execute();

    return 0;
}
```

在上述示例中，我们有两种类型的子任务：`DownloadTask` 用于下载文件，`ProcessTask` 用于处理文件。

首先，我们创建了两个下载任务和两个文件处理任务，并将它们添加到并行任务 `ParallelTask` 中。

通过调用 `set_entry()` 方法，我们将每个任务的 `entry` 设置为下一个任务的地址。这样，当一个任务完成时，它会调用 `subtask_done()` 方法来触发下一个任务的执行。

在 `execute()` 方法中，我们将所有子任务连接在一起，使其形成一个任务链。然后，我们调用第一个子任务的 `dispatch()` 方法来启动整个任务链的执行。

在执行过程中，每个任务完成后，它会调用 `subtask_done()` 方法，这会触发下一个任务的执行，直到所有任务都完成。

通过使用 `entry` 变量，我们实现了子任务之间的顺序协作和控制。每个子任务完成后，根据设置的 `entry` 值，控制权会传递给下一个任务，从而实现了任务的有序执行。

在示例中，下载任务完成后触发文件处理任务的执行，因此下载和文件处理操作是有序的。如果需要更复杂的任务依赖关系，可以根据需求设置不同的 `entry` 值，以实现灵活的任务调度和控制。

### 简单的demo

```cpp
#include <workflow/Workflow.h>
#include <workflow/WFTaskFactory.h>
#include <workflow/WFFacilities.h>

int main()
{
    WFTimerTask *task = WFTaskFactory::create_timer_task(3000 * 1000,    
    [](WFTimerTask *timer)
    {
        fprintf(stderr, "timer end\n");
    });

    task->start();
    getchar();
    return 0;
}
```

我们之前分析了timerTask, 我们先用这个简单的task来分析一番workflow的task是如何组织的。

workflow最为基本的运行逻辑，就是串并联，最为简单的运行模式就是一个Series串联起来依次执行。

### 运行起来

我们先来看看task如何运行起来的，我在创建出task后，start启动

```cpp
class WFTimerTask : public SleepRequest
{
public:
	void start()
	{
		assert(!series_of(this));
		Workflow::start_series_work(this, nullptr);
	}
    ...
};
```

我们所有的task都要依赖在series中进行，所以我们不能让这个裸的task执行，先给他创建series_work

```cpp
inline void
Workflow::start_series_work(SubTask *first, series_callback_t callback)
{
	new SeriesWork(first, std::move(callback));
	first->dispatch();
}
```

## 创建SeriesWork

我们先来观察一下SeriesWork的结构

```cpp
class SeriesWork
{
....
protected:
	void *context;
	series_callback_t callback;
private:
	SubTask *first;  
	SubTask *last;   
	SubTask **queue;  
	int queue_size;
	int front;
	int back;
	bool in_parallel;
	bool canceled;
	std::mutex mutex;
};
```

看的出，是一个queue，也很容易理解，我们把任务装到一个队列里，挨着挨着执行。

我们在new 构造的时候，初始化一下

```cpp
SeriesWork::SeriesWork(SubTask *first, series_callback_t&& cb) :
	callback(std::move(cb))
{
	this->queue = new SubTask *[4];
	this->queue_size = 4;
	this->front = 0;
	this->back = 0;
	this->in_parallel = false;
	this->canceled = false;
	first->set_pointer(this);
	this->first = first;
	this->last = NULL;
	this->context = NULL;
}
```

这里我们注意两点:

1. this->queue = new SubTask *[4]; 

先预留出来4个空间

2. first->set_pointer(this)

把subTask和这个SeriesWork绑定了起来

3. 这里front和back都为0，我们不把first task算在task里，而是用first指针单独存储(首位都较为特殊，都单独存储)

## Subtask

引出本节的重点

```cpp
first->dispatch();
```

我们的SubTask是workflow所有task的爷，他的核心就三个函数

```cpp
class SubTask
{
public:
	virtual void dispatch() = 0;

private:
	virtual SubTask *done() = 0;

protected:
	void subtask_done();
    ...
};
```

其中dispatch和done是纯虚函数，不同的task继承实现不同的逻辑。

## dispatch

dispatch代表任务的发起

在此调用task的核心逻辑，并且要求任务在完成的时候调用subtask_done方法。

我们可以对比两个例子:

就拿我们之前分析过的Timer Task和Go Task

```cpp
// TimerTask
class SleepRequest : public SubTask, public SleepSession
{
	...
	virtual void dispatch()
	{
		if (this->scheduler->sleep(this) < 0)
		{
			this->state = SS_STATE_ERROR;
			this->error = errno;
			this->subtask_done();
		}
	}
	...
}
```

```cpp
// GOTask
class ExecRequest : public SubTask, public ExecSession
{
	...
	virtual void dispatch()
	{
		if (this->executor->request(this, this->queue) < 0)
		{
			this->state = ES_STATE_ERROR;
			this->error = errno;
			this->subtask_done();
		}
	}
	...
}
```

一般在XXRequest这一层继承实现dispatch(), 在上次调用核心逻辑函数，执行完调用subtask_done.

## done

done是任务的完成行为，你可以看到subtask_done里立刻调用了任务的done。(此为执行task逻辑的核心，后面仔细讲解)

我们再拿TimerTask和GoTask对比看看

```cpp
class WFTimerTask : public SleepRequest
{
	...
protected:
	virtual SubTask *done()
	{
		SeriesWork *series = series_of(this);

		if (this->callback)
			this->callback(this);

		delete this;
		return series->pop();
	}
	...
};
```

```cpp
class WFGoTask : public ExecRequest
{
	...
protected:
	virtual SubTask *done()
	{
		SeriesWork *series = series_of(this);

		if (this->callback)
			this->callback(this);

		delete this;
		return series->pop();
	}
	...
};
```

done方法把任务交回实现者，实现者要负责内存的回收，比如可能delete this。

done方法可以返回一个新任务，所以这里我们在series pop出来。(也有return this的情况，这个之后我们遇到再看。)

## subtask_done

接下里是最为核心的task执行逻辑

这里我们先简化一下，不管parallel

```cpp
void SubTask::subtask_done()
{
	SubTask *cur = this;
	...

	while (1)
	{
		...
		cur = cur->done();   
		if (cur)  
		{
			...
			cur->dispatch(); 
		}
		...
		break;
	}
}
```

我们上面的例子，timer task执行，他dispatch后，调用subtask_done,

```cpp
// TimerTask
virtual void dispatch()
{
	if (this->scheduler->sleep(this) < 0)
	{
		this->state = SS_STATE_ERROR;
		this->error = errno;
		this->subtask_done();
	}
}
```

当前任务完成，调用done

```cpp
// TimerTask
virtual SubTask *done()
{
	SeriesWork *series = series_of(this);

	if (this->callback)
		this->callback(this);

	delete this;
	return series->pop();
}
```

返回了series的下一个task继续执行。当然我们这里series只有一个task，pop出去是null，没有可以执行的，就算结束了。

同理，如果是一个series里多个task，这里就下一个task，dipatch，同上执行。

顺便这里说一下，Series最为重要的一个接口就是push_back将SubTask加入到队列中.

```cpp
void SeriesWork::push_back(SubTask *task)
{
	this->mutex.lock();
	task->set_pointer(this);
	this->queue[this->back] = task;
	if (++this->back == this->queue_size)
		this->back = 0;

	if (this->front == this->back)
		this->expand_queue();

	this->mutex.unlock();
}
```