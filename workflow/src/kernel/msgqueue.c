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

/*
 * This message queue originates from the project of Sogou C++ Workflow:
 * https://github.com/sogou/workflow
 *
 * The idea of this implementation is quite simple and abvious. When the
 * get_list is not empty, consumer takes a message. Otherwise the consumer
 * waits till put_list is not empty, and swap two lists. This method performs
 * well when the queue is very busy, and the number of consumers is big.
 */

#include <errno.h>
#include <stdlib.h>
#include <pthread.h>
#include "msgqueue.h"

// 消息队列就是个单链表
// 此处有两个链表，高效swap使用
/*
	这里直接用了void **去做链表，
	这样做的一大优势是：

	充分利用用户分配的msg内存，
	消息队列内部可以省去分配释放空间的开销
*/
struct __msgqueue
{
	size_t msg_max;	 // 生产队列允许的最大长度
	size_t msg_cnt;  // 生产者队列当前实际长度
	int linkoff;	 // linkoff是距每条消息头部的偏移量，表示在这个消息对象内部，链接到下一个消息对象的指针字段相对于消息对象开始的字节偏移。其中一个指针大小的空间应可供内部使用。
	int nonblock;	 // 0：阻塞 1：非阻塞
	void *head1;     // get_list  头指针，指向队列中当前处于最前面的元素 
	void *head2;     // put_list  头指针，指向队列中当前处于最前面的元素 
	// 两个list，高效率，一个在get_list拿，一个在put_list放
	// 如果get_list空，如果put_list放了的话，那么swap一下就可了，O(1),非常高效，而且互不干扰
	void **get_head;	
	void **put_head;
	void **put_tail;
	pthread_mutex_t get_mutex; // 消费者锁
	pthread_mutex_t put_mutex; //生产者锁
	pthread_cond_t get_cond;
	pthread_cond_t put_cond;
};

void msgqueue_set_nonblock(msgqueue_t *queue)
{
	queue->nonblock = 1;
	pthread_mutex_lock(&queue->put_mutex);
	
	// 叫醒一个消费者
	pthread_cond_signal(&queue->get_cond);
	// 叫醒所有生产者
	pthread_cond_broadcast(&queue->put_cond);
	
	pthread_mutex_unlock(&queue->put_mutex);
}

void msgqueue_set_block(msgqueue_t *queue)
{
	queue->nonblock = 0;
}

/*
	__msgqueue_swap(queue) 是一个内部函数，它会交换 queue->get_head 和 queue->put_head，
	如果交换后队列中有消息，那么这个函数就会返回消息的数量（大于0），否则返回0。
*/
static size_t __msgqueue_swap(msgqueue_t *queue)
{	
	// 1. 用临时变量记录下当前的get队列偏移量
	void **get_head = queue->get_head;
	size_t cnt;

	// 2. 把刚才的生产者队列换给消费者队列
	// 将get_head切换好，因为就算put在加，put_head也不会变, 所以不需要加锁
	queue->get_head = queue->put_head;  
	// 3. 只有这个地方才会同时持有消费者锁和生产者锁
	pthread_mutex_lock(&queue->put_mutex);

    // 4. 如果当前队列本身就是空的
    //    这里就会帮等待下一个来临的生产者通get_cond叫醒我
	// 如果put_list也没有消息且为阻塞态，那么就wait等到放进来消息
	while (queue->msg_cnt == 0 && !queue->nonblock)
		pthread_cond_wait(&queue->get_cond, &queue->put_mutex);

	cnt = queue->msg_cnt;  
    // 5. 如果当前队列是满的，说明可能有生产者在等待
    //    通过put_cond叫醒生产者（可能有多个，所以用broadcast）
	// 如果cnt大于最大接收的msg，那么通知put，因为大于msg_max put_list wait在那里了，所以swap清空了就要唤醒生产者put
	if (cnt > queue->msg_max - 1)
		pthread_cond_broadcast(&queue->put_cond);

   // 6. 把第一行的临时变量换给生产者队列，清空生产者队列
	queue->put_head = get_head;    // put_list就交换设置到get_list那个地方了  
	queue->put_tail = get_head;

	// put_list清0了
	// 收到put消息是queue->msg_cnt++, 并没有拿走消息queue->msg_cnt--;
	// 靠的就是put_list swap 到 get_list 就清0了
	queue->msg_cnt = 0;    

	pthread_mutex_unlock(&queue->put_mutex);
	// 7. 返回刚才多少个，这个会影响get里的逻辑
	return cnt;
}

/**
 * @brief 就是把msg串到queue后面去
 * msqqueue是epoll消息回来之后，以网络线程作为生产者往queue里放、执行线程作为消费者从queue里拿数据，从而做到线程互不干扰
 * @note 函数首先计算你想要放入队列的消息中的link位置
 * struct message {
 *   int data;         // 消息的实际数据
 *   void* next;       // 指向下一个消息的指针
 *};
 * @param msg 
 * @param queue 
 */
void msgqueue_put(void *msg, msgqueue_t *queue)
{
	// 这里转char* 是因为，void* 不能加减运算，但char* 可以
	// 1. 通过create的时候传进来的linkoffset，算出消息尾部的偏移量。这个 link 指针会被用来存储下一个消息的地址，即link 指针会被用来指向下一个消息
	void **link = (void **)((char *)msg + queue->linkoff); // 见 src_analysis/08_msgqueue.md

	// 2. 设置为空，用于表示生产者队列末尾的后面没有其他数据
	*link = NULL; 

	// 3. 加生产者锁
	pthread_mutex_lock(&queue->put_mutex);
	
    // 4. 如果当前已经有msg_max个消息的话
    //    就要等待消费者通过put_cond来叫醒我
	// 当收到的cnt大于最大限制 且 阻塞mode(default)， 那么wait在这, 等待消费者去给消费了
	while (queue->msg_cnt > queue->msg_max - 1 && !queue->nonblock)
		pthread_cond_wait(&queue->put_cond, &queue->put_mutex);

	// 5. put_tail指向这条消息队列尾部，维护生产者队列的消息个数 见 src_analysis/08_msgqueue.md
	*queue->put_tail = link;  // 把 消息B 串到当前链尾 消息A 后面： (A)*next = (B)link
	queue->put_tail = link;   // 更新 队列链尾索引（put_tail）为 消息B 字段： index = link，即link 指针会被用来指向下一个消息
	queue->msg_cnt++;
	pthread_mutex_unlock(&queue->put_mutex);

	// 6. 如果有消费者在等，通过get_cond叫醒他～
	pthread_cond_signal(&queue->get_cond);
}

void *msgqueue_get(msgqueue_t *queue)
{
	void *msg;

	// 1. 加消费者锁
	pthread_mutex_lock(&queue->get_mutex);

    // 2. 如果目前get_head不为空，表示有数据；
    //    如果空，那么通过__msgqueue_swap()切换队列，也可以拿到数据
	// 若get_list无消息了，那么看看put_list有没有，如果有，swap一下即可
	if (*queue->get_head || __msgqueue_swap(queue) > 0)
	{
		// 3. 对应put中的计算方式，根据减去linkoff把第一个消息的起始偏移量算出来
		/*这里 *queue->get_head 是指向 消息结构体中 next 字段的指针，
		 *而我们想要获取的是整个消息的地址，所以需要减去 linkoff（即 next 字段在消息结构中的偏移量）来得到消息的起始地址。
		 */
		msg = (char *)*queue->get_head - queue->linkoff;

		// 4. 将 *queue->get_head 更新为它指向的下一条消息，这个下一条消息的地址就是 *(void **)*queue->get_head。
		// 更新 队列链头消息next字段为 下一条消息B next字段。往后挪，这时候的*get_head就是下一条数据的偏移量尾部（next 字段）
		*queue->get_head = *(void **)*queue->get_head;
	}
	else
	{
		msg = NULL;
		errno = ENOENT;
	}

	pthread_mutex_unlock(&queue->get_mutex);
	return msg;
}

/**
 * @brief 分配空间+初始化
 * 
 * @param maxlen 
 * @param linkoff 
 * @return msgqueue_t* 
 */
msgqueue_t *msgqueue_create(size_t maxlen, int linkoff)
{
	msgqueue_t *queue = (msgqueue_t *)malloc(sizeof (msgqueue_t));
	int ret;

	if (!queue)
		return NULL;

	ret = pthread_mutex_init(&queue->get_mutex, NULL);
	if (ret == 0)
	{
		ret = pthread_mutex_init(&queue->put_mutex, NULL);
		if (ret == 0)
		{
			ret = pthread_cond_init(&queue->get_cond, NULL);
			if (ret == 0)
			{
				ret = pthread_cond_init(&queue->put_cond, NULL);
				if (ret == 0)
				{
					// 各种初始化，最后设置queue的成员变量如下：
					queue->msg_max = maxlen;
					queue->linkoff = linkoff;
					queue->head1 = NULL;
					queue->head2 = NULL;
					// 借助两个head分别作为两个内部队列的位置
					queue->get_head = &queue->head1; // ->操作符的优先级高于&操作符。
					queue->put_head = &queue->head2;
					// 一开始队列为空，所以生产者队尾也等于队头
					queue->put_tail = &queue->head2;
					queue->msg_cnt = 0;
					queue->nonblock = 0;
					return queue;
				}

				pthread_cond_destroy(&queue->get_cond);
			}

			pthread_mutex_destroy(&queue->put_mutex);
		}

		pthread_mutex_destroy(&queue->get_mutex);
	}

	errno = ret;
	free(queue);
	return NULL;
}

void msgqueue_destroy(msgqueue_t *queue)
{
	pthread_cond_destroy(&queue->put_cond);
	pthread_cond_destroy(&queue->get_cond);
	pthread_mutex_destroy(&queue->put_mutex);
	pthread_mutex_destroy(&queue->get_mutex);
	free(queue);
}

