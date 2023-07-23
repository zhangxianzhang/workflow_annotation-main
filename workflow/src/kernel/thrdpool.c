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
  线程池是基于消息队列实现的
*/

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "list.h"
#include "thrdpool.h"
#include <stdio.h>
//#include "logger.h"

/**
 * @brief 线程池内部结构 
 * @note  typedef struct __thrdpool thrdpool_t;
 * @note  链式等待：线程池可以通过一个等一个的方式优雅地去结束所有线程。线程池的创建者才会设置pool->tid为0。当前线程会将pool->tid设置为当前线程的线程标识符。在 POSIX 线程库中，线程标识符的具体表示形式和取值范围是不确定的，可以是任何有效的非零值。
 * 
 */
struct __thrdpool
{
	struct list_head task_queue;//msgqueue_t *msgqueue; 新版本由消息队列实现
	size_t nthreads;			// 线程个数
	size_t stacksize;			// 构造线程时的参数，传入pthread_attr_setstacksize 函数设置线程栈的大小
	pthread_t tid;				// 实现链式等待的关键：第一次发起thrdpool_create时，运行期间记录的是个0值，只有发起者拿到0值
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	pthread_key_t key;			// 线程池的key；会将线程池内部数据赋予给每个由线程池创建的线程作为他们的 thread local
	pthread_cond_t *terminate;  // 不仅是退出时的标记位，而且还是调用退出的那个人要等待的condition
}; 

struct __thrdpool_task_entry    // 与thrdpool_task为组合关系，它们的生命周期是紧密相关的。
{
	struct list_head list;		// 新版本为 void *link;
	struct thrdpool_task task;
};	

static pthread_t __zero_tid;

/**	
 * @brief  指向用于执行线程任务的函数。每个执行routine的线程，都是消费者。它不停从队列拿任务出来执行。
 * @note   1.只有线程池的创建者才会设置pool->tid为0 2.可以和接口thrdpool_schedule()关联上，使得”线程任务可以由另一个线程任务调起”。 每个发起schedule的线程，都是生产者
 * @param  *arg: pool 将作为参数传递给线程任务函数 __thrdpool_routine，以便在线程中访问线程池的上下文和数据。
 * @retval None
 */
static void *__thrdpool_routine(void *arg)
{
	//LOG_TRACE("__thrdpool_routine");
	thrdpool_t *pool = (thrdpool_t *)arg;
	struct list_head **pos = &pool->task_queue.next;  // 第一个任务
	/*
	struct __thrdpool_task_entry
	{
		struct list_head list;    // 用来串起来thrdpool_task
		struct thrdpool_task task;
	};
	*/
	struct __thrdpool_task_entry *entry;
	void (*task_routine)(void *);
	void *task_context;
	pthread_t tid;

/*
	`pthread_setspecific` 是 POSIX 线程库中的一个函数，它被用于设置线程特定数据。
	线程特定数据（Thread-Specific Data，TSD）是一种允许每个线程都有其自己的独立数据副本的机制。
	在这种情况下，即使多个线程可能访问和操作同一份代码，但是每个线程对其线程特定数据的读写都是隔离的。
	因此，不同的线程之间不会共享通过 `pthread_setspecific` 设置的数据。例如，如果你在一个线程中
	使用 `pthread_setspecific(key, value)` 设置了一个特定的值，那么这个值只能在这个线程中通过
	 `pthread_getspecific(key)` 获取。在其他线程中使用相同的键调用 `pthread_getspecific(key)` 
	 将会返回那个线程为这个键设置的值，如果没有设置过，将会返回 `NULL`。因此，`pthread_setspecific` 
	 可以用于在多线程环境中存储和操作线程局部（thread-local）数据，而不需要使用互斥锁等同步机制来防止
	 数据竞争。但是要注意的是，线程特定数据在使用完毕后需要通过相应的机制进行清理，以避免内存泄漏。
*/
	pthread_setspecific(pool->key, pool);  
	while (1)
	{	
		// 1.注意这里还持有锁
		pthread_mutex_lock(&pool->mutex);
		while (!pool->terminate && list_empty(&pool->task_queue))// 此处是消费者行为，如果没停止，且任务队列没任务，就wait在这
			pthread_cond_wait(&pool->cond, &pool->mutex);

		// 2. 这既是线程池退出标识位，也是发起销毁的那个人所等待的condition
		if (pool->terminate)  
			break;

		entry = list_entry(*pos, struct __thrdpool_task_entry, list);// 拿到任务
		list_del(*pos); 

		pthread_mutex_unlock(&pool->mutex);

		/*
		struct thrdpool_task
		{
			void (*routine)(void *);
			void *context;
		}; 
		一个task，包含了需要做什么，还需要一个上下文(task运行需要的一些参数)
		*/
		task_routine = entry->task.routine;
		task_context = entry->task.context;
		free(entry);

		task_routine(task_context); // 未加锁状态下执行拿到的任务（每个执行routine的线程，都是消费者）

		// 如果如果routine()里调用destroy() 。内部线程回收线程池内存
		if (pool->nthreads == 0)
		{
			/* Thread pool was destroyed by the task. */
			free(pool);
			return NULL;
		}
	}

	/* One thread joins another. Don't need to keep all thread IDs. */
	// 3. 把线程池上记录的那个tid拿下来，我来负责上一人。因为锁还被持有，所以能够依次退出
	tid = pool->tid;
	// 4. 把我自己记录到线程池上，下一个人来负责我 
	pool->tid = pthread_self(); // 线程池里线程唯一一次设置pool->tid
	// 5. 每个人都减1，最后一个人负责叫醒发起detroy的人。并且发起detroy的人等待最后一个人执行完毕
	if (--pool->nthreads == 0)
		pthread_cond_signal(pool->terminate);

	// 6. 这里可以解锁进行等待了
	pthread_mutex_unlock(&pool->mutex);   // 因为线程池结束后使用break！所以锁还被持有

	// 7. 只有第一个人拿到0值
	if (memcmp(&tid, &__zero_tid, sizeof (pthread_t)) != 0)
		// 8. 只要不0值，我就要负责等上一个结束才能退（回收掉上一个的系统资源）。只有线程池创建者pool->tid为0。
		pthread_join(tid, NULL);

	return NULL;// 9. 退出，干干净净～
}

/**
 * @brief  初始化lock相关，就是mutex，conditional_variable
 * @note   
 * @param  *pool: 
 * @retval 
 */
static int __thrdpool_init_locks(thrdpool_t *pool)
{
	int ret;

	ret = pthread_mutex_init(&pool->mutex, NULL); // POSIX 线程有两种方法来初始化锁，这里是初始化的动态方法（即在运行时）
	if (ret == 0)
	{
		ret = pthread_cond_init(&pool->cond, NULL);
		if (ret == 0)
			return 0;

		pthread_mutex_destroy(&pool->mutex); // 如果初始化条件变量失败，需要先销毁已经初始化成功的互斥锁
	}

	errno = ret;
	return -1;
}

static void __thrdpool_destroy_locks(thrdpool_t *pool)
{
	pthread_mutex_destroy(&pool->mutex);
	pthread_cond_destroy(&pool->cond);
}

static void __thrdpool_terminate(int in_pool, thrdpool_t *pool)
{
	pthread_cond_t term = PTHREAD_COND_INITIALIZER; // 调用退出的那个人要等待的condition。 静态初始化的条件变量只能在定义时进行初始化，而不能在运行时重新初始化。

	pthread_mutex_lock(&pool->mutex);
	// 1. 加锁设置标志位，之后的添加任务不会被执行，但可以pending拿到

	pool->terminate = &term; // 调用退出的那个人要等待的condition  

	// 2. 广播所有等待的消费者
             
	pthread_cond_broadcast(&pool->cond);

	if (in_pool) // 内部线程进行销毁
	{
		/* Thread pool destroyed in a pool thread is legal. */
		pthread_detach(pthread_self()); // 使线程执行完毕后自动释放
		pool->nthreads--;
	}

	// 4. 如果还有线程没有退完，我会等，注意这里是while
	while (pool->nthreads > 0)
		pthread_cond_wait(&term, &pool->mutex);

	pthread_mutex_unlock(&pool->mutex);

	// 5.同样地等待打算退出的上一个人 
	if (memcmp(&pool->tid, &__zero_tid, sizeof (pthread_t)) != 0)
		pthread_join(pool->tid, NULL); // 等待线程池里最后一个线程执行完毕
}

static int __thrdpool_create_threads(size_t nthreads, thrdpool_t *pool)
{
	pthread_attr_t attr;
	pthread_t tid;
	int ret;

	ret = pthread_attr_init(&attr);
	if (ret == 0)
	{
		if (pool->stacksize)
			pthread_attr_setstacksize(&attr, pool->stacksize);

		while (pool->nthreads < nthreads) // 此函数最关键处：循环创建nthreads个线程
		{
			ret = pthread_create(&tid, &attr, __thrdpool_routine, pool);
			if (ret == 0)
				pool->nthreads++;
			else
				break;
		}

		pthread_attr_destroy(&attr); // 销毁线程属性对象并不会影响已经创建的线程。线程属性对象仅用于线程的创建过程中，一旦线程创建成功，线程将独立运行，与线程属性对象无关。
		if (pool->nthreads == nthreads)
			return 0;

		__thrdpool_terminate(0, pool); // main线程不属于线程池
	}

	errno = ret;
	return -1;
}

/**
 * @brief  线程池的创建
 * @note   只有线程池的创建者才会设置pool->tid为0
 * @param  nthreads: 循环创建nthreads个线程
 * @param  stacksize: 将传入pthread_attr_setstacksize 函数设置线程栈的大小
 * @retval 
 */
thrdpool_t *thrdpool_create(size_t nthreads, size_t stacksize)
{
	thrdpool_t *pool;
	int ret;
	// 1. 分配
	pool = (thrdpool_t *)malloc(sizeof (thrdpool_t)); // 在堆上分配，需要管理其生命周期
	if (pool)
	{
		// 2. 初始化
		if (__thrdpool_init_locks(pool) >= 0) // “>=” 而不是 “==”，是为了更好地处理未来可能出现的情况？
		{
			ret = pthread_key_create(&pool->key, NULL);
			if (ret == 0)
			{
				INIT_LIST_HEAD(&pool->task_queue);  // 初始化信息池的消息队列
				pool->stacksize = stacksize;
				pool->nthreads = 0;
				memset(&pool->tid, 0, sizeof (pthread_t)); //只有线程池的创建者才会设置pool->tid为0
				pool->terminate = NULL;
				if (__thrdpool_create_threads(nthreads, pool) >= 0)
					return pool;

				pthread_key_delete(pool->key);
			}
			else
				errno = ret;

			__thrdpool_destroy_locks(pool);
		}

		free(pool);
	}

	return NULL;
}

/**
 * @brief  这里的线程池调度，其实就是生产者，加入任务到list里，通知消费者来消费
 * @note   每个发起schedule的线程，都是生产者。
 * @param  *task: 
 * @param  *buf: 这里就是struct __thrdpool_task_entry，为了简便写成void *buf
 * @param  *pool: 
 * @retval None
 */
inline void __thrdpool_schedule(const struct thrdpool_task *task, void *buf,
								thrdpool_t *pool)
{
	//LOG_TRACE("__thrdpool_schedule");
	struct __thrdpool_task_entry *entry = (struct __thrdpool_task_entry *)buf;

	entry->task = *task;
	pthread_mutex_lock(&pool->mutex);

	// 把线程任务放入线程池队列中
	//LOG_TRACE("add entry list to pool task Queue");
	list_add_tail(&entry->list, &pool->task_queue);

	// 叫醒在等待的线程
	pthread_cond_signal(&pool->cond);
	pthread_mutex_unlock(&pool->mutex);
}

/**
 * @brief  把任务交给线程池的接口（就是调用__thrdpool_schedule） 其实就是生产者，加入任务到list里，通知消费者来消费
 * @note   __thrdpool_routine()可以和接口thrdpool_schedule()关联上，导致”线程任务可以由另一个线程任务调起”：只要对队列的管理做得好，显然消费者所执行的函数里也可以生产。每个发起schedule的线程，都是生产者。
 * @param  *task: 
 * @param  *pool: 
 * @retval 
 */
int thrdpool_schedule(const struct thrdpool_task *task, thrdpool_t *pool)
{
	//LOG_TRACE("thrdpool_schedule");
	void *buf = malloc(sizeof (struct __thrdpool_task_entry));
	if (buf)
	{
		__thrdpool_schedule(task, buf, pool);
		return 0;
	}

	return -1;
}

/**
 * @brief  给线程池扩容
 * @note   
 * @param  *pool: 
 * @retval 
 */
int thrdpool_increase(thrdpool_t *pool)
{
	pthread_attr_t attr;
	pthread_t tid;
	int ret;

	ret = pthread_attr_init(&attr);
	if (ret == 0)
	{
		if (pool->stacksize)
			pthread_attr_setstacksize(&attr, pool->stacksize);

		pthread_mutex_lock(&pool->mutex);
		ret = pthread_create(&tid, &attr, __thrdpool_routine, pool);
		if (ret == 0)
			pool->nthreads++;

		pthread_mutex_unlock(&pool->mutex);
		pthread_attr_destroy(&attr);
		if (ret == 0)
			return 0;
	}

	errno = ret;
	return -1;
}

// 通过key区分这个线程是否是线程池创建的
inline int thrdpool_in_pool(thrdpool_t *pool)
{	
	printf("***\n in:pool->key -%p   pool -%p \n", pthread_getspecific(pool->key) ,pool);
	return pthread_getspecific(pool->key) == pool;
}

/**
 * @brief  销毁线程池。发起销毁线程池的人来负责回收线程池内存
 * @note   函数销毁线程池，并通过pending回调函数返回还没有被执行的任务。在一个线程任务里调用本函数是一个合法行为，这种情况下线程池正常被销毁，调用者线程不会立刻中断，而是正常运行至routine结束。
 * @note   在退出的时候，我们那些已经提交但是还没有被执行的任务是绝对不能就这么扔掉了的，于是我们可以传入一个pending()函数，上层可以做自己的回收、回调、或任何保证上层逻辑完备的事情。
 * @retval None
 */
void thrdpool_destroy(void (*pending)(const struct thrdpool_task *),
					  thrdpool_t *pool)
{
	int in_pool = thrdpool_in_pool(pool); // 通过key区分这个线程是否是线程池创建的
	printf("in_pool-%d\n", in_pool );
	printf("in_pool_ptr -%p\n", pthread_getspecific(pool->key) );
	struct __thrdpool_task_entry *entry;
	struct list_head *pos, *tmp;

	// 1. 内部会设置pool->terminate，并叫醒所有等在队列拿任务的线程
	__thrdpool_terminate(in_pool, pool);

	// 2. 把队列里还没有执行的任务都拿出来，通过pending返回给用户
	list_for_each_safe(pos, tmp, &pool->task_queue) // tmp：一个临时变量，用于保存下一个节点的指针。在遍历过程中，可能会删除或添加节点，所以需要使用临时变量来保存下一个节点，以确保遍历的安全性
	{
		entry = list_entry(pos, struct __thrdpool_task_entry, list);
		list_del(pos);
		if (pending)
			pending(&entry->task);

		free(entry);
	}

	pthread_key_delete(pool->key);
	__thrdpool_destroy_locks(pool);

	// 如果不是内部线程发起的销毁，要负责回收线程池内存
	if (!in_pool)
		free(pool);
}

