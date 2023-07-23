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

#ifndef _COMMUNICATOR_H_
#define _COMMUNICATOR_H_

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <stddef.h>
#include <pthread.h>
#include <openssl/ssl.h>
#include "list.h"
#include "poller.h"
#include "logger.h"

/*
  该头文件定义了一系列的类，用于实现一个异步网络通信库。包含以下几个重要的类：

  - CommConnection：表示一个通信连接，其具体行为和特性可能由具体的派生类定义。

  - CommTarget：表示一个通信目标，封装了网络地址、超时、SSL配置等信息。提供创建新连接和处理SSL的方法。

  - CommMessageOut：表示一个出站消息。定义了一个纯虚函数encode()，需要派生类实现，用于将消息编码为iovec向量。

  - CommMessageIn：表示一个入站消息。定义了一个纯虚函数append()，需要派生类实现，用于处理接收到的数据。还有其他方法用于处理反馈和更新接收时间。

  - CommSession：表示一个通信会话，包含一个目标、一个连接、一个出站消息和一个入站消息。还定义了一些纯虚函数，需要派生类实现，用于处理消息超时和会话状态。

  - CommService：表示一个服务端，封装了绑定地址、超时、SSL配置等信息。提供创建新会话和处理SSL的方法。有一个drain()方法，用于处理挂起的连接。

  - SleepSession：表示一个睡眠会话，定义了一些纯虚函数，需要派生类实现，用于获取睡眠时间和处理会话状态。

  - Communicator：表示一个通信器，管理并发的通信会话。提供了一些方法用于初始化、发送请求、发送回复、绑定服务、解绑服务、休眠等。它内部使用一个poller、一个消息队列和一个线程池来处理并发的会话。
*/

// 代表通信连接。具体的行为可能由派生类实现。
class CommConnection
{
protected:
    virtual ~CommConnection() {} // 虚析构函数，确保正确析构派生类对象
    friend class Communicator;   // 让Communicator可以访问CommConnection的protected和private成员
};

// CommTarget是通讯目标，网络通信中，通信目标通常是指与我们进行通信的对方的地址。基本上就是ip+port, 还有两个超时参数。连接池什么的都在target里。
// 客户端与服务端建立连接所需的参数和连接创建过程的封装。封装了网络地址、超时、SSL上下文等信息。
class CommTarget
{
public:
    int init(const struct sockaddr *addr, socklen_t addrlen,
             int connect_timeout, int response_timeout);
    void deinit();

public:
    void get_addr(const struct sockaddr **addr, socklen_t *addrlen) const
    {
        *addr = this->addr;
        *addrlen = this->addrlen;
    }

    int get_connect_timeout() const { return this->connect_timeout; }
    int get_response_timeout() const { return this->response_timeout; }

protected:
    void set_ssl(SSL_CTX *ssl_ctx, int ssl_connect_timeout)
    {
        this->ssl_ctx = ssl_ctx;
        this->ssl_connect_timeout = ssl_connect_timeout;
    }

    // 获取SSL上下文
    SSL_CTX *get_ssl_ctx() const { return this->ssl_ctx; }

private:
    // 创建连接的文件描述符
    virtual int create_connect_fd()
    {
        return socket(this->addr->sa_family, SOCK_STREAM, 0);
    }

    // 新建一个连接
    virtual CommConnection *new_connection(int connect_fd)
    {
        return new CommConnection;
    }

    virtual int init_ssl(SSL *ssl) { return 0; }

public:
    virtual void release(int keep_alive) {}

private:
    struct sockaddr *addr;  // 确定了要连接到的服务器地址
    socklen_t addrlen;      // 确定了要连接到的服务器地址
    int connect_timeout;    // 连接超时时间
    int response_timeout;   // 响应超时时间
    int ssl_connect_timeout;
    SSL_CTX *ssl_ctx;
    // SSL_CTX数据结构主要用于SSL握手前的环境准备，设置CA文件和目录、设置SSL握手中的证书文件和私钥、设置协议版本以及其他一些SSL握手时的选项。
    // SSL_CTX中缓存了所有SSL_SESSION信息
    // 一般SSL_CTX的初始化在程序最开始调用
private:
    struct list_head idle_list; // idle_list本来是指keep-alive的可复用连接, 此处命名有歧意
    pthread_mutex_t mutex;

public:
    virtual ~CommTarget() {}

    // 允许CommSession和Communicator类访问此类的保护和私有成员。
    friend class CommSession;
    friend class Communicator;
};

/*在C++中，友元（`friend`）机制是一种允许其他类或函数访问当前类的私有（`private`）或受保护（`protected`）成员的方法。这是一种明确授权非类成员函数或类访问当前类内部信息的方式。

在这个例子中，`CommTarget`类声明`CommSession`和`Communicator`为其友元类，意味着`CommSession`和`Communicator`可以访问`CommTarget`类的所有成员，包括私有和受保护的。

有些情况下，你可能需要允许某些类或函数访问另一个类的私有或受保护成员。比如：

1. 当两个或更多的类需要紧密协作以完成一些任务时。例如，可能有一个类负责数据的存储，而另一个类负责数据的处理。在这种情况下，处理类可能需要直接访问存储类的内部数据，而不仅仅是通过存储类提供的公共接口。

2. 当某些类设计成服务于其他类时。例如，一个类可能专门设计为对另一个类进行单元测试，因此需要访问被测试类的所有成员。

在这个通信库的案例中，`CommSession`和`Communicator`可能需要直接访问和修改`CommTarget`的某些成员，这些成员可能被设置为私有或受保护，以防止其他类随意访问和修改。
*/

/*
序列化函数encode

encode函数在消息被发送之前调用，每条消息只调用一次。

encode函数里，用户需要将消息序列化到一个vector数组，数组元素个数不超过max。目前max的值为8192。

结构体struct iovec定义在请参考系统调用readv和writev。

encode函数正确情况下的返回值在0到max之间，表示消息使用了多少个vector。

如果是UDP协议，请注意总长度不超过64k，并且使用不超过1024个vector（Linux一次writev只能1024个vector）。
UDP协议只能用于client，无法实现UDP server。

encode返回-1表示错误。返回-1时，需要置errno。如果返回值>max，将得到一个EOVERFLOW错误。错误都在callback里得到。
为了性能考虑vector里的iov_base指针指向的内容不会被复制。所以一般指向消息类的成员。
*/

// 输出消息类
class CommMessageOut
{
private:
    // 纯虚函数，为派生类定义消息编码方法，函数参数包含一个用于接收编码结果的iovec向量数组和该数组的最大大小
    virtual int encode(struct iovec vectors[], int max) = 0;

public:
    // 虚析构函数，用于保证派生类的析构函数被正确调用
    virtual ~CommMessageOut() {}
    // 声明友元类Communicator，允许其访问本类的私有和保护成员
    friend class Communicator;
};

// 输入消息类，继承自poller_message_t，采用私有继承，基类的公有和保护成员在派生类中都变成私有
class CommMessageIn : private poller_message_t
{
private:
    // 纯虚函数，为派生类定义附加数据到消息的方法，函数参数包含数据缓冲区和数据大小
    virtual int append(const void *buf, size_t *size) = 0;

protected:
    // 虚函数，用于在接收数据时发送小数据包，只在append()函数中调用
    virtual int feedback(const void *buf, size_t size);

    // 虚函数，用于在append()函数中重置接收开始时间为当前时间
    virtual void renew();

private:
    // 私有成员，指向CommConnEntry的指针
    struct CommConnEntry *entry;

public:
    // 虚析构函数，用于保证派生类的析构函数被正确调用
    virtual ~CommMessageIn() {}
    // 声明友元类Communicator，允许其访问本类的私有和保护成员
    friend class Communicator;
};

// 定义会话状态枚举值
#define CS_STATE_SUCCESS 0 // 成功状态
#define CS_STATE_ERROR 1   // 错误状态
#define CS_STATE_STOPPED 2 // 停止状态
#define CS_STATE_TOREPLY 3 // 待回复状态，仅用于服务会话 for service session only.

/*
CommSession是一次req->resp的交互，主要要实现message_in(), message_out()等几个虚函数，让核心知道怎么产生消息。
对server来讲，session是被动产生的
*/
// 表示一个通信会话，包含一个目标、一个连接、一个出站消息和一个入站消息。CommRequest、CommRequest和WFHttpServerTask等是CommSession的派生类
class CommSession
{
private:
    // 一系列纯虚函数，为派生类定义输出消息、输入消息、发送超时、接收超时、保持活跃超时、首次超时和处理函数
    virtual CommMessageOut *message_out() = 0; // 往连接上要发的数据
    virtual CommMessageIn *message_in() = 0;   // 连接上收到数据流，如何切下一个数据包
    virtual int send_timeout() { return -1; }
    virtual int receive_timeout() { return -1; }
    virtual int keep_alive_timeout() { return 0; }
    virtual int first_timeout() { return 0; }      /* for client session only. */
    virtual void handle(int state, int error) = 0; // 连接上收到数据流，如何切下一个数据包

private:
    virtual int connect_timeout() { return this->target->connect_timeout; }
    virtual int response_timeout() { return this->target->response_timeout; }

protected:
    // 一系列保护成员获取函数，用于获取target、conn、out、in、seq等成员
    CommTarget *get_target() const { return this->target; }
    CommConnection *get_connection() const { return this->conn; }
    CommMessageOut *get_message_out() const { return this->out; }
    CommMessageIn *get_message_in() const { return this->in; }
    long long get_seq() const { return this->seq; }

private:
    CommTarget *target;
    CommConnection *conn;
    CommMessageOut *out;
    CommMessageIn *in;
    long long seq;

private:
    struct timespec begin_time;
    int timeout;
    int passive;

public:
    CommSession() { this->passive = 0; }
    virtual ~CommSession();
    friend class Communicator;
};

/*
CommService就是服务了，主要是new_session()的实现，因为对server来讲，session是被动产生的。
*
* 用来产生listenfd
* 产生新的连接
*/
// 表示一个服务端，封装了绑定地址、超时、SSL配置等信息
class CommService
{
public:
    // 初始化函数，参数包含服务的绑定地址、地址长度、监听超时时间和响应超时时间
    int init(const struct sockaddr *bind_addr, socklen_t addrlen,
             int listen_timeout, int response_timeout);
    // 去初始化函数，用于释放资源
    void deinit();

    // 处理最多max个事件，返回处理的事件数量
    int drain(int max);

public:
    // 公有函数，获取服务的绑定地址和地址长度
    void get_addr(const struct sockaddr **addr, socklen_t *addrlen) const
    {
        *addr = this->bind_addr;
        *addrlen = this->addrlen;
    }

protected:
    // 保护函数，设置SSL上下文和接收SSL连接的超时时间
    void set_ssl(SSL_CTX *ssl_ctx, int ssl_accept_timeout)
    {
        this->ssl_ctx = ssl_ctx;
        this->ssl_accept_timeout = ssl_accept_timeout;
    }

    // 保护函数，获取SSL上下文
    SSL_CTX *get_ssl_ctx() const { return this->ssl_ctx; }

private:
    // 一系列虚函数，为派生类定义新会话、停止处理、未绑定处理、创建监听套接字、新连接、初始化SSL等操作
    virtual CommSession *new_session(long long seq, CommConnection *conn) = 0; //
    virtual void handle_stop(int error) {}
    virtual void handle_unbound() = 0;

private:
    virtual int create_listen_fd()
    {
        return socket(this->bind_addr->sa_family, SOCK_STREAM, 0);
    }

    virtual CommConnection *new_connection(int accept_fd)
    {
        return new CommConnection;
    }

    virtual int init_ssl(SSL *ssl) { return 0; }

private:
    struct sockaddr *bind_addr; // 用于保存被服务器绑定套接字的地址信息
    socklen_t addrlen;          // 用于保存被服务器绑定套接字的地址信息
    int listen_timeout;
    int response_timeout;
    int ssl_accept_timeout;
    SSL_CTX *ssl_ctx; // SSL上下文

public:
    // 增加引用计数
    void incref()
    {
        __sync_add_and_fetch(&this->ref, 1);
    }

    // 减小引用计数
    void decref()
    {
        if (__sync_sub_and_fetch(&this->ref, 1) == 0)
            this->handle_unbound();
    }

private:
    int listen_fd; // 监听套接字
    int ref;       // 引用计数

private:
    struct list_head alive_list; // 用于维护活动列表的链表头
    pthread_mutex_t mutex;

public:
    virtual ~CommService() {}
    friend class CommServiceTarget;
    friend class Communicator;
};

// 定义会话状态枚举值
#define SS_STATE_COMPLETE 0  // 完成状态
#define SS_STATE_ERROR 1     // 错误状态
#define SS_STATE_DISRUPTED 2 // 中断状态

class SleepSession
{
private:
    // 定义睡眠持续时间
    virtual int duration(struct timespec *value) = 0;
    // 定义睡眠处理函数
    virtual void handle(int state, int error) = 0;

public:
    virtual ~SleepSession() {}
    friend class Communicator;
};

#ifdef __linux__
#include "IOService_linux.h"
#else
#include "IOService_thread.h"
#endif

/*
类的析构函数是一个特殊的成员函数，它在类的对象被销毁时自动调用。析构函数的主要任务是执行"清理作业"，比如释放对象可能占用的资源。比如，如果类的对象动态分配了内存，那么应该在析构函数中释放这些内存。如果类的对象打开了文件或者数据库连接，那么应该在析构函数中关闭它们。

在这个`Communicator`类中，析构函数是空的。这可能有以下几种理由：

1. 该类的对象没有需要在析构函数中清理的资源。比如，它没有动态分配的内存，没有打开的文件或者数据库连接等。

2. 所有的资源清理工作都在类的其他函数（比如`deinit()`函数）中完成了。

3. 该类被设计为可以被其他类继承，并且希望在派生类中提供自定义的析构函数。这时候，通常会提供一个虚析构函数，即使它是空的。

然而，即使析构函数为空，定义析构函数仍然是有好处的。这是因为它能确保当删除一个指向派生类对象的`Communicator`指针时，派生类的析构函数能被正确调用。这是C++多态性的一个重要特性。
*/
/*
通信器：提供了低级别的网络通信功能，管理一个线程池，并在这些线程上执行并发的网络通信任务
*/
class Communicator
{
public:
    //  初始化Communicator。设置轮询器线程和处理线程的数量。 在init中create_epoll
    int init(size_t poller_threads, size_t handler_threads);

    // 释放资源函数
    void deinit();

    // Communicator::request(CommSession *session, CommTarget *target)这个接口就可以实现一个异步的网络请求了
    // 向指定目标发起请求，session参数指定了会话的具体内容和行为，target确定了请求的目标
    int request(CommSession *session, CommTarget *target);
    // 对接收到的请求进行回复，session参数包含了回复的内容和行为。
    int reply(CommSession *session);

    // 将缓冲区中的数据推送到指定的session会话中，该数据将被用于之后的通信过程。
    int push(const void *buf, size_t size, CommSession *session);

    // 封装bind和listen。绑定CommService的非阻塞listen套接字到mpoller指向的某个poller线程管理的多路复用器中，使其可以通过此Communicator进行通信调度。
    int bind(CommService *service);

    // 将先前绑定的CommService从此Communicator上解绑。
    void unbind(CommService *service);

    // 使指定的SleepSession进入休眠状态。
    int sleep(SleepSession *session);

    // 绑定IOService，使其可以通过此Communicator进行IO操作。
    int io_bind(IOService *service);

    // 将先前绑定的IOService从此Communicator上解绑。
    void io_unbind(IOService *service);

public:
    // 判断当前线程是否为处理线程。
    int is_handler_thread() const;

    // 增加处理线程的数量。
    int increase_handler_thread();

private:
    struct __mpoller *mpoller;   // 监控多个多路复用器（如select、poll、epoll等）的事件，并根据事件类型调用相应的处理函数。同时，它也处理管道事件和超时事件。
    struct __msgqueue *queue;    // 消息队列，存放待处理的消息。
    struct __thrdpool *thrdpool; // 线程池，用于处理消息队列中的消息。
    int stop_flag;               // 停止调度标志。

    /*以下是一系列用于内部处理的方法，包括创建轮询器和处理线程，进行非阻塞连接和监听，启动和接收连接，
    发送消息，处理各种结果等等。这些操作封装了通信的底层逻辑。*/
private:
    // 创建多路复用轮询器，需要设定轮询线程的数量。
    int create_poller(size_t poller_threads);

    // 创建处理线程，需要设定处理线程的数量。
    int create_handler_threads(size_t handler_threads);

    // 创建一个非阻塞的连接到指定的目标。
    int nonblock_connect(CommTarget *target);

    // 创建一个非阻塞的监听服务，服务将被绑定到指定的CommService。
    int nonblock_listen(CommService *service);

    // 启动一个连接，连接的会话和目标由参数指定。
    struct CommConnEntry *launch_conn(CommSession *session, CommTarget *target);

    // 接受一个新的连接，连接的目标和服务由参数指定。
    struct CommConnEntry *accept_conn(class CommServiceTarget *target, CommService *service);

    // 释放一个连接，此操作会关闭连接并释放相关资源。
    void release_conn(struct CommConnEntry *entry);

    // 关闭一个服务，此操作会关闭服务并释放相关资源。
    void shutdown_service(CommService *service);

    // 关闭一个IO服务，此操作会关闭服务并释放相关资源。
    void shutdown_io_service(IOService *service);

    // 以同步方式发送消息，此操作会阻塞直到所有数据都被发送。
    int send_message_sync(struct iovec vectors[], int cnt, struct CommConnEntry *entry);

    // 以异步方式发送消息，此操作会立即返回，数据的发送将在后台进行。
    int send_message_async(struct iovec vectors[], int cnt, struct CommConnEntry *entry);

    // 发送消息，此操作会根据指定的连接选择同步或异步方式进行。
    int send_message(struct CommConnEntry *entry);

    // 获取空闲连接
    struct CommConnEntry *get_idle_conn(CommTarget *target);

    // 请求一个空闲的连接，如果没有空闲的连接，此操作会创建一个新的连接。
    int request_idle_conn(CommSession *session, CommTarget *target);

    // 回应一个空闲的连接，此操作通常在接收到请求后进行，用于回应请求者。
    int reply_idle_conn(CommSession *session, CommTarget *target);

    // 请求一个新的连接，此操作会创建一个新的连接并返回。
    int request_new_conn(CommSession *session, CommTarget *target);

    // 处理接收到的请求，此操作会根据请求的内容进行相应的处理。
    void handle_incoming_request(struct poller_result *res);

    // 处理接收到的回应，此操作会根据回应的内容进行相应的处理。
    void handle_incoming_reply(struct poller_result *res);

    // 处理请求的结果，此操作会根据结果的状态进行相应的处理。
    void handle_request_result(struct poller_result *res);

    // 处理回应的结果，此操作会根据结果的状态进行相应的处理。
    void handle_reply_result(struct poller_result *res);

    // 处理写操作的结果，此操作会根据结果的状态进行相应的处理。
    void handle_write_result(struct poller_result *res);

    // 处理读操作的结果，此操作会根据结果的状态进行相应的处理。
    void handle_read_result(struct poller_result *res);

    // 处理连接操作的结果，此操作会根据结果的状态进行相应的处理。
    void handle_connect_result(struct poller_result *res);

    // 处理监听操作的结果，此操作会根据结果的状态进行相应的处理。
    void handle_listen_result(struct poller_result *res);

    // 处理SSL接受操作的结果，此操作会根据结果的状态进行相应的处理。
    void handle_ssl_accept_result(struct poller_result *res);

    // 处理休眠操作的结果，此操作会根据结果的状态进行相应的处理。
    void handle_sleep_result(struct poller_result *res);

    // 处理异步IO操作的结果，此操作会根据结果的状态进行相应的处理。
    void handle_aio_result(struct poller_result *res);

    // 静态方法，用于处理线程的启动，会话的超时处理，消息的添加，以及对poller_result的回调处理。

    // 处理线程例程，此例程在处理线程中执行，用于处理消息队列中的消息。
    static void handler_thread_routine(void *context);

    // 计算会话的首次超时时间，此操作会根据会话的状态计算其首次超时时间。
    static int first_timeout(CommSession *session);

    // 计算会话的下次超时时间，此操作会根据会话的状态计算其下次超时时间。
    static int next_timeout(CommSession *session);

    // 计算会话的首次发送超时时间，此操作会根据会话的状态计算其首次发送超时时间。
    static int first_timeout_send(CommSession *session);

    // 计算会话的首次接收超时时间，此操作会根据会话的状态计算其首次接收超时时间。
    static int first_timeout_recv(CommSession *session);

    // 将请求的数据追加到消息中，此操作会把指定的数据追加到消息的尾部。
    static int append(const void *buf, size_t *size, poller_message_t *msg);

    // 创建一个请求消息，此操作会根据指定的上下文创建一个新的请求消息。
    static int create_service_session(struct CommConnEntry *entry);

    // 创建一个回应消息，此操作会根据指定的上下文创建一个新的回应消息。
    static poller_message_t *create_message(void *context);

    // 计算已经写入的数据量，此操作会根据指定的上下文计算已经写入的数据量。
    static int partial_written(size_t n, void *context);

    // 根据指定的地址、套接字和上下文创建服务器通讯目标。
    static void *accept(const struct sockaddr *addr, socklen_t addrlen, int sockfd, void *context);

    static void callback(struct poller_result *res, void *context);

public:
    // 通讯器的析构函数，此函数在对象生命周期结束时被调用，用于释放资源。
    virtual ~Communicator() {}
};

#endif
