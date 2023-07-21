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

  Authors: Wu Jiaxu (wujiaxu@sogou-inc.com)
           Liu Kai (liukaidx@sogou-inc.com)
           Xie Han (xiehan@sogou-inc.com)
*/

#include <assert.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <string>
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/engine.h>
#include <openssl/conf.h>
#include <openssl/crypto.h>
#include "WFGlobal.h"
#include "EndpointParams.h"
#include "CommScheduler.h"
#include "DnsCache.h"
#include "RouteManager.h"
#include "Executor.h"
#include "WFTask.h"
#include "WFTaskError.h"
#include "WFNameService.h"
#include "WFDnsResolver.h"
#include "WFDnsClient.h"
#include "logger.h"

class __WFGlobal
{
public:
	static __WFGlobal *get_instance()
	{
		static __WFGlobal kInstance; // 唯一的实例
		return &kInstance; // 返回实例指针
	}

	const WFGlobalSettings *get_global_settings() const
	{
		return &settings_;
	}

	void set_global_settings(const WFGlobalSettings *settings)
	{
		settings_ = *settings; // 设置全局设置
	}

	const char *get_default_port(const std::string& scheme)
	{
		const auto it = static_scheme_port_.find(scheme);

		if (it != static_scheme_port_.end())
			return it->second; // 如果静态方案端口映射中存在该方案，则返回端口

		const char *port = NULL;
		user_scheme_port_mutex_.lock();
		const auto it2 = user_scheme_port_.find(scheme);

		if (it2 != user_scheme_port_.end())
			port = it2->second.c_str(); // 如果用户方案端口映射中存在该方案，则返回端口

		user_scheme_port_mutex_.unlock();
		return port;
	}

	// 将方案名称 scheme 和对应的端口号 port 注册到用户方案端口映射中。
	void register_scheme_port(const std::string& scheme, unsigned short port)
	{
		user_scheme_port_mutex_.lock();
		user_scheme_port_[scheme] = std::to_string(port); // 注册用户自定义方案端口
		user_scheme_port_mutex_.unlock();
	}

	// 用于记录同步操作的计数，并根据需要增加处理线程。增加处理线程的条件是同步操作计数超过最大同步操作计数。
	void sync_operation_begin()
	{
		bool inc;

		sync_mutex_.lock();
		inc = ++sync_count_ > sync_max_; // 是否需要增加处理线程

		if (inc)
			sync_max_ = sync_count_; // 更新最大同步操作计数
		sync_mutex_.unlock();
		if (inc)
			WFGlobal::get_scheduler()->increase_handler_thread(); // 增加处理线程
	}

	// 用于将同步操作计数减一
	void sync_operation_end()
	{
		sync_mutex_.lock();
		sync_count_--; // 同步操作计数减一
		sync_mutex_.unlock();
	}

private:
	__WFGlobal(); // 构造函数

private:
	struct WFGlobalSettings settings_; // 全局设置
	std::unordered_map<std::string, const char *> static_scheme_port_; // 静态方案端口映射
	std::unordered_map<std::string, std::string> user_scheme_port_; // 用户方案端口映射
	std::mutex user_scheme_port_mutex_; // 用户方案端口映射互斥锁
	std::mutex sync_mutex_; // 同步操作互斥锁
	int sync_count_; // 同步操作计数
	int sync_max_; // 最大同步操作计数
};

__WFGlobal::__WFGlobal() : settings_(GLOBAL_SETTINGS_DEFAULT)
{
	static_scheme_port_["dns"] = "53";
	static_scheme_port_["Dns"] = "53";
	static_scheme_port_["DNS"] = "53";

	static_scheme_port_["dnss"] = "853";
	static_scheme_port_["Dnss"] = "853";
	static_scheme_port_["DNSs"] = "853";
	static_scheme_port_["DNSS"] = "853";

	static_scheme_port_["http"] = "80";
	static_scheme_port_["Http"] = "80";
	static_scheme_port_["HTTP"] = "80";

	static_scheme_port_["https"] = "443";
	static_scheme_port_["Https"] = "443";
	static_scheme_port_["HTTPs"] = "443";
	static_scheme_port_["HTTPS"] = "443";

	static_scheme_port_["redis"] = "6379";
	static_scheme_port_["Redis"] = "6379";
	static_scheme_port_["REDIS"] = "6379";

	static_scheme_port_["rediss"] = "6379";
	static_scheme_port_["Rediss"] = "6379";
	static_scheme_port_["REDISs"] = "6379";
	static_scheme_port_["REDISS"] = "6379";

	static_scheme_port_["mysql"] = "3306";
	static_scheme_port_["Mysql"] = "3306";
	static_scheme_port_["MySql"] = "3306";
	static_scheme_port_["MySQL"] = "3306";
	static_scheme_port_["MYSQL"] = "3306";

	static_scheme_port_["mysqls"] = "3306";
	static_scheme_port_["Mysqls"] = "3306";
	static_scheme_port_["MySqls"] = "3306";
	static_scheme_port_["MySQLs"] = "3306";
	static_scheme_port_["MYSQLs"] = "3306";
	static_scheme_port_["MYSQLS"] = "3306";

	static_scheme_port_["kafka"] = "9092";
	static_scheme_port_["Kafka"] = "9092";
	static_scheme_port_["KAFKA"] = "9092";

	sync_count_ = 0;
	sync_max_ = 0;
}

#if OPENSSL_VERSION_NUMBER < 0x10100000L
static std::mutex *__ssl_mutex;

static void ssl_locking_callback(int mode, int type, const char* file, int line)
{
	if (mode & CRYPTO_LOCK)
		__ssl_mutex[type].lock();
	else if (mode & CRYPTO_UNLOCK)
		__ssl_mutex[type].unlock();
}
#endif

class __SSLManager
{
public:
	static __SSLManager *get_instance()
	{
		static __SSLManager kInstance;
		return &kInstance;
	}

	SSL_CTX *get_ssl_client_ctx() { return ssl_client_ctx_; }
	SSL_CTX *new_ssl_server_ctx() { return SSL_CTX_new(SSLv23_server_method()); }

private:
	__SSLManager()
	{
#if OPENSSL_VERSION_NUMBER < 0x10100000L
		__ssl_mutex = new std::mutex[CRYPTO_num_locks()];
		CRYPTO_set_locking_callback(ssl_locking_callback);
		SSL_library_init();
		SSL_load_error_strings();
		//ERR_load_crypto_strings();
		//OpenSSL_add_all_algorithms();
#endif

		ssl_client_ctx_ = SSL_CTX_new(SSLv23_client_method());
		assert(ssl_client_ctx_ != NULL);
	}

	~__SSLManager()
	{
		SSL_CTX_free(ssl_client_ctx_);

#if OPENSSL_VERSION_NUMBER < 0x10100000L
		//free ssl to avoid memory leak
		FIPS_mode_set(0);
		CRYPTO_set_locking_callback(NULL);
# ifdef CRYPTO_LOCK_ECDH
		CRYPTO_THREADID_set_callback(NULL);
# else
		CRYPTO_set_id_callback(NULL);
# endif
		ENGINE_cleanup();
		CONF_modules_unload(1);
		ERR_free_strings();
		EVP_cleanup();
# ifdef CRYPTO_LOCK_ECDH
		ERR_remove_thread_state(NULL);
# else
		ERR_remove_state(0);
# endif
		CRYPTO_cleanup_all_ex_data();
		sk_SSL_COMP_free(SSL_COMP_get_compression_methods());
		delete []__ssl_mutex;
#endif
	}

private:
	SSL_CTX *ssl_client_ctx_;
};

class IOServer : public IOService
{
public:
	IOServer(CommScheduler *scheduler):
		scheduler_(scheduler),
		flag_(true)
	{}

	int bind()
	{
		mutex_.lock();
		flag_ = false;

		int ret = scheduler_->io_bind(this);

		if (ret < 0)
			flag_ = true;

		mutex_.unlock();
		return ret;
	}

	void deinit()
	{
		std::unique_lock<std::mutex> lock(mutex_);
		while (!flag_)
			cond_.wait(lock);

		lock.unlock();
		IOService::deinit();
	}

private:
	virtual void handle_unbound()
	{
		mutex_.lock();
		flag_ = true;
		cond_.notify_one();
		mutex_.unlock();
	}

	virtual void handle_stop(int error)
	{
		scheduler_->io_unbind(this);
	}

	CommScheduler *scheduler_;
	std::mutex mutex_;
	std::condition_variable cond_;
	bool flag_;
};

class __DnsManager
{
public:
	ExecQueue *get_dns_queue() { return &dns_queue_; }
	Executor *get_dns_executor() { return &dns_executor_; }

	__DnsManager()
	{
		int ret;

		ret = dns_queue_.init();
		if (ret < 0)
			abort();

		ret = dns_executor_.init(__WFGlobal::get_instance()->
											 get_global_settings()->
											 dns_threads);
		if (ret < 0)
			abort();
	}

	~__DnsManager()
	{
		dns_executor_.deinit();
		dns_queue_.deinit();
	}

private:
	ExecQueue dns_queue_;
	Executor dns_executor_;
};


/*
在网络服务器库架构中，`__CommManager` 类的作用可能是提供一个全局的通信管理器，用于集中管理与通信相关的组件和资源。它可能承担以下功能和作用：

1. **提供全局访问点**：作为单例模式的管理器，它可以提供全局唯一的访问点，使得不同模块或组件可以方便地访问通信相关的功能和资源。

2. **调度器管理**：通过 `scheduler_` 对象提供调度器的管理和操作，负责处理任务的调度和执行。其他模块可以通过 `get_scheduler()` 函数获取调度器对象，以便添加、删除和执行任务。

3. **路由管理**：通过 `route_manager_` 对象提供路由管理的功能，用于管理和配置网络请求的路由规则，以便将请求分发到不同的处理程序。

4. **IO服务管理**：通过 `io_server_` 对象提供IO服务的管理和操作，用于处理网络IO操作，如接受连接、读写数据等。`get_io_service()` 函数用于获取IO服务的实例，并确保只有一个实例被创建和初始化。这可以提供高效的异步IO支持，并避免在多个模块之间重复创建和初始化IO服务。

5. **DNS服务管理**：通过 `dns_manager_` 对象提供DNS服务的管理和操作，用于解析域名和获取IP地址。`get_dns_manager_safe()` 函数确保只有一个实例被创建和初始化，提供线程安全的访问。

6. **全局设置获取**：通过 `__WFGlobal::get_instance()` 获取全局设置的实例，并使用其中的参数进行初始化和配置，以便根据设置调整调度器和其他组件的行为。

总的来说，`__CommManager` 在网络服务器库架构中起到集中管理和协调不同组件的作用，提供全局访问和资源管理，以支持高效的网络通信和请求处理。它整合了调度器、路由管理、IO服务和DNS服务等核心功能，提供统一的接口和配置，简化了网络服务器的开发和管理。
*/
// 一个全局的通信管理器，用于访问和管理调度器、路由管理器、IO服务和DNS服务。通过使用饿汉（？懒汉）式单例模式，确保只有一个全局的通信管理器实例存在，方便在程序的不同部分共享和访问通信相关的功能组件。同时，使用互斥锁和双重检查锁定等机制保证了线程安全性。
class __CommManager
{
public:

/*
在C++的饿汉式单例模式中，实例对象在程序开始执行时就会被创建，而不是在第一次使用时才创建。具体来说，饿汉式单例模式使用了静态成员变量来存储单例实例，静态成员变量在程序运行时会被初始化，因此实例对象会在程序开始时被创建。
这意味着，在程序启动时，单例类的实例对象就会被创建，并且在整个程序的生命周期中，只会存在一个实例对象。该实例对象可以在任何需要的地方被访问和使用。
在 __CommManager 类中，静态成员函数 get_instance() 并没有通过懒加载的方式创建实例，而是直接返回一个静态的 __CommManager 对象。

选择饿汉式单例模式可能是出于以下考虑：

    线程安全：饿汉式单例模式在类加载时就创建实例，保证了多线程环境下的线程安全性，无需额外的线程同步措施。

    简单易用：饿汉式单例模式实现起来相对简单，代码量较少，易于理解和维护。

    性能优化：通过在类加载时创建实例，避免了在多个地方重复创建实例的开销，可以提高程序的性能。

需要注意的是，饿汉式单例模式在类加载时就创建实例，可能会造成资源的浪费，特别是在实例对象较大或需要耗费较多资源时。因此，如果实例对象的创建和初始化较为复杂，或者希望实现延迟加载，可以考虑使用懒汉式单例模式。但在 __CommManager 类中，并没有体现这种需求，因此选择了饿汉式单例模式。

非常抱歉之前的回答有误。根据提供的代码，可以看到 `get_instance()` 函数实际上是通过懒加载方式创建实例的。

在上述代码中，`get_instance()` 函数返回的是一个指向静态 `__CommManager` 对象的指针。静态局部变量 `kInstance` 在第一次调用 `get_instance()` 函数时被初始化，并且该静态变量会一直存在于函数的整个生命周期中。

通过使用静态局部变量的方式，实现了懒加载的效果。也就是说，实例对象只有在第一次调用 `get_instance()` 函数时才会被创建。之后的调用都会返回同一个实例对象。

这种实现方式也被称为懒汉式单例模式，因为实例对象在第一次使用时才会被创建。

需要注意的是，这种懒加载的方式在多线程环境下可能会存在竞态条件（race condition）的问题。如果多个线程同时调用 `get_instance()` 函数并且实例尚未创建时，可能会导致多个实例对象被创建。为了确保线程安全，可以采用线程同步机制，例如使用互斥锁（mutex）或双重检查锁定（double-checked locking）来解决竞态条件的问题。
*/
	static __CommManager *get_instance()
	{
		static __CommManager kInstance; // 唯一的实例
		return &kInstance; // 返回实例指针
	}

	CommScheduler *get_scheduler() { return &scheduler_; } // 获取调度器的函数，返回调度器指针

	RouteManager *get_route_manager() { return &route_manager_; } // 获取路由管理器的函数，返回路由管理器指针

	IOService *get_io_service()
	{
		if (!io_flag_) // 如果 IO 服务尚未初始化
		{
			io_mutex_.lock(); // 加锁
			if (!io_flag_) // 双重检查，确保只有一个线程初始化 IO 服务
			{
				io_server_ = new IOServer(&scheduler_); // 创建 IOServer 实例，并传入调度器指针
				if (io_server_->init(8192) < 0) // 初始化 IO 服务，传入缓冲区大小
					abort(); // 初始化失败，终止程序

				if (io_server_->bind() < 0) // 绑定 IO 服务
					abort(); // 绑定失败，终止程序

				io_flag_ = true; // 标记 IO 服务已初始化
			}

			io_mutex_.unlock(); // 解锁
		}

		return io_server_; // 返回 IO 服务指针
	}

	ExecQueue *get_dns_queue()
	{
		return get_dns_manager_safe()->get_dns_queue();
	}

	Executor *get_dns_executor()
	{
		return get_dns_manager_safe()->get_dns_executor();
	}

private:
	// 初始化通信管理器对象
	/*
	在 __CommManager 类中，静态的 __CommManager 对象 kInstance 是一个局部静态变量，它在 get_instance() 函数内部被定义。当程序第一次调用 get_instance() 函数时，kInstance 对象会被创建并初始化。随后的每次调用 get_instance() 函数都会返回这个唯一的 __CommManager 对象。
	*/
	__CommManager():
		io_server_(NULL),
		io_flag_(false),
		dns_manager_(NULL),
		dns_flag_(false)
	{
		LOG_TRACE("__CommManager creator"); // 输出日志，表示创建 __CommManager 对象。
		const auto *settings = __WFGlobal::get_instance()->get_global_settings();
		if (scheduler_.init(settings->poller_threads,
							settings->handler_threads) < 0) // 通过调用 scheduler_ 对象的 init() 函数，初始化调度器。传入设置的轮询线程数和处理线程数。
			abort();

		signal(SIGPIPE, SIG_IGN); // 忽略 SIGPIPE 信号，防止在管道破裂时终止程序。确保在处理套接字写入时不会导致进程终止
	}

	~__CommManager()
	{
		if (dns_manager_)
			delete dns_manager_;

		scheduler_.deinit();
		if (io_server_)
		{
			io_server_->deinit();
			delete io_server_;
		}
	}

	__DnsManager *get_dns_manager_safe()
	{
		if (!dns_flag_)
		{
			dns_mutex_.lock();
			if (!dns_flag_)
			{
				dns_manager_ = new __DnsManager();
				dns_flag_ = true;
			}

			dns_mutex_.unlock();
		}

		return dns_manager_;
	}

private:
	CommScheduler scheduler_; // 通信调度器对象，用于管理任务的调度和执行
	RouteManager route_manager_; // 路由管理器对象，用于管理路由信息
	IOServer *io_server_; // IO 服务指针，用于提供异步IO操作
	volatile bool io_flag_; // IO 服务初始化标志
	std::mutex io_mutex_; // IO 服务初始化互斥锁
	__DnsManager *dns_manager_; // DNS 管理器对象，用于管理DNS服务
	volatile bool dns_flag_; // DNS 服务初始化标志
	std::mutex dns_mutex_; // DNS 服务初始化互斥锁
};

class __DnsCache
{
public:
	static __DnsCache *get_instance()
	{
		static __DnsCache kInstance;
		return &kInstance;
	}

	DnsCache *get_dns_cache() { return &dns_cache_; }

private:
	__DnsCache() { }

	~__DnsCache() { }

private:
	DnsCache dns_cache_;
};

class __ExecManager
{
protected:
	using ExecQueueMap = std::unordered_map<std::string, ExecQueue *>;

public:
	static __ExecManager *get_instance()
	{
		static __ExecManager kInstance;
		return &kInstance;
	}
	/**
	 * @brief 创建ExecQueue
	 * 
	 * @param queue_name 
	 * @return ExecQueue* 
	 */
	ExecQueue *get_exec_queue(const std::string& queue_name)
	{
		ExecQueue *queue = NULL;
		/*
		关于计算队列名
		我们的计算任务并没有优化级的概念，唯一可以影响调度顺序的是计算任务的队列名，本示例中队列名为字符串"add"。
		队列名的指定非常简单，需要说明以下几点：

		1. 队列名是一个静态字符串，不可以无限产生新的队列名。例如不可以根据请求id来产生队列名，因为内部会为每个队列分配一小块资源。
		当计算线程没有被100%占满，所有任务都是实时调起，队列名没有任何影响。

		2. 如果一个服务流程里有多个计算步骤，穿插在多个网络通信之间，可以简单的给每种计算步骤起一个名字，这个会比整体用一个名字要好。
		
		3.如果所有计算任务用同一个名字，那么所有任务的被调度的顺序与提交顺序一致，在某些场景下会影响平均响应时间。
		
		4. 每种计算任务有一个独立名字，那么相当于每种任务之间是公平调度的，而同一种任务内部是顺序调度的，实践效果更好。
		
		总之，除非机器的计算负载已经非常繁重，否则没有必要特别关心队列名，只要每种任务起一个名字就可以了。
		*/
		pthread_rwlock_rdlock(&rwlock_);

		const auto iter = queue_map_.find(queue_name);
		// 先查找队列名
		if (iter != queue_map_.cend())
			queue = iter->second;

		pthread_rwlock_unlock(&rwlock_);

		// 如果还没有此队列名，那么new一个, 并初始化
		if (!queue)
		{
			queue = new ExecQueue();
			if (queue->init() < 0)
			{
				delete queue;
				queue = NULL;
			}
			else
			{
				pthread_rwlock_wrlock(&rwlock_);
				// 存起来
				const auto ret = queue_map_.emplace(queue_name, queue);

				if (!ret.second)
				{
					queue->deinit();
					delete queue;
					queue = ret.first->second;
				}

				pthread_rwlock_unlock(&rwlock_);
			}
		}

		return queue;
	}

	Executor *get_compute_executor() { return &compute_executor_; }

private:
	__ExecManager():
		rwlock_(PTHREAD_RWLOCK_INITIALIZER)
	{
		LOG_TRACE("__ExecManager creator");
		int compute_threads = __WFGlobal::get_instance()->
										  get_global_settings()->
										  compute_threads;

		if (compute_threads <= 0)
			compute_threads = sysconf(_SC_NPROCESSORS_ONLN);

		if (compute_executor_.init(compute_threads) < 0)
			abort();
	}

	~__ExecManager()
	{
		compute_executor_.deinit();

		for (auto& kv : queue_map_)
		{
			kv.second->deinit();
			delete kv.second;
		}
	}

private:
	pthread_rwlock_t rwlock_;
	ExecQueueMap queue_map_;
	Executor compute_executor_;
};

class __NameServiceManager
{
public:
	static __NameServiceManager *get_instance()
	{
		static __NameServiceManager kInstance;
		return &kInstance;
	}

public:
	WFDnsResolver *get_dns_resolver() { return &resolver_; }
	WFNameService *get_name_service() { return &service_; }

private:
	WFDnsResolver resolver_;
	WFNameService service_;

public:
	// 注意此处service_是怎么初始化的
	__NameServiceManager() : service_(&resolver_) { }
};

#define MAX(x, y)	((x) >= (y) ? (x) : (y))
#define HOSTS_LINEBUF_INIT_SIZE	128

static void __split_merge_str(const char *p, bool is_nameserver,
							  std::string& result)
{
	// 格式 : 
	/*
	nameserver 10.0.12.210
	search foobar.com foo.com
	options ndots:5
	*/

	const char *start;
	
	if (!isspace(*p)) // 如果从开始不是空格，则格式不正确，直接返回
		return;

	while (1)
	{
		while (isspace(*p))
			p++;
		// 从第一个不为空格的地方开始
		start = p; // 此处start记录下p的位置，然后去移动p
		// ; 或者 # 开头的为注释
		// 如果p不在 '\0' 结束处 且 不是 # 且 不是 ; 且不为空格，则往后移动
		while (*p && *p != '#' && *p != ';' && !isspace(*p))
			p++;

		if (start == p)  // 如果都没动，这一排后面是注释或者后面没有东西，就直接break了
			break;
		// 第一次传入的url是一个空的str，拿进来拼凑出url
		// 第一个之后都用 ',' 分割开
		if (!result.empty())
			result.push_back(',');
		
		std::string str(start, p); 
		if (is_nameserver)
		{
			struct in6_addr buf;
			// 如果是nameserver字段的话，那么这里读出的str则是 ip 地址
			// 这里如果是ipv6则[str]格式
			if (inet_pton(AF_INET6, str.c_str(), &buf) > 0)
				str = "[" + str + "]";
		}

		result.append(str);
	}
}

static inline const char *__try_options(const char *p, const char *q,
										const char *r)
{
	size_t len = strlen(r);  // 要查找的字段长度
	// 首先在一个字符串中找这个字段，要大于他才行， 然后看这么长是否有是这个字段
	// 如果是则返回末尾的这个位置，否则NULL
	if ((size_t)(q - p) >= len && strncmp(p, r, len) == 0)
		return p + len;
	return NULL;
}

static void __set_options(const char *p,
						  int *ndots, int *attempts, bool *rotate)
{
	// 和__split_merge_str 逻辑一样的找字段
	const char *start;
	const char *opt;

	if (!isspace(*p))
		return;

	while (1)
	{
		while (isspace(*p))
			p++;

		start = p;
		while (*p && *p != '#' && *p != ';' && !isspace(*p))
			p++;

		if (start == p)
			break;
		// 主要找以下几个字段
		if ((opt = __try_options(start, p, "ndots:")) != NULL)
			*ndots = atoi(opt);
		else if ((opt = __try_options(start, p, "attempts:")) != NULL)
			*attempts = atoi(opt);
		else if ((opt = __try_options(start, p, "rotate")) != NULL)
			*rotate = true;  // rotate 出现了就是true，没有其他值
			// 格式 : options rotate
	}
}

/**
 * @brief 这个函数，传入path(resolve.conf), 然后读出里面的url， search_list，ndots， attempts，rotate出来
 * 
 * @param [in] path 
 * @param [out] url 
 * @param [out] search_list 
 * @param [out] ndots 
 * @param [out] attempts 
 * @param [out] rotate 
 * @return int 0 成功，-1 失败
 */
static int __parse_resolv_conf(const char *path,
							   std::string& url, std::string& search_list,
							   int *ndots, int *attempts, bool *rotate)
{
	size_t bufsize = 0;
	char *line = NULL;
	FILE *fp;
	int ret;
	// 打开resolve.conf文件
	fp = fopen(path, "r");
	if (!fp)
		return -1;
	// 然后一排一排的读取出来
	while ((ret = getline(&line, &bufsize, fp)) > 0)
	{
		// 然后判断是哪些字段
		// nameserver x.x.x.x该选项用来制定DNS服务器的，可以配置多个nameserver指定多个DNS。
		if (strncmp(line, "nameserver", 10) == 0)
			__split_merge_str(line + 10, true, url);
		else if (strncmp(line, "search", 6) == 0)
			// search 111.com 222.com该选项可以用来指定多个域名，中间用空格或tab键隔开。
			// 原来当访问的域名不能被DNS解析时，resolver会将该域名加上search指定的参数，重新请求DNS，
			// 直到被正确解析或试完search指定的列表为止。
			__split_merge_str(line + 6, false, search_list);
		else if (strncmp(line, "options", 7) == 0)
			// 接下来就是这只options选项了
			__set_options(line + 7, ndots, attempts, rotate);
	}

	ret = ferror(fp) ? -1 : 0;
	free(line);
	fclose(fp);
	return ret;
}

class __DnsClientManager
{
public:
	static __DnsClientManager *get_instance()
	{
		static __DnsClientManager kInstance;
		return &kInstance;
	}

public:
	WFDnsClient *get_dns_client() { return client; }

private:
	__DnsClientManager()
	{
		// default "/etc/resolv.conf"
		const char *path = WFGlobal::get_global_settings()->resolv_conf_path;

		client = NULL;
		// 如果有resolv.conf路径
		if (path && path[0])
		{
			// man 5 resolv.conf
			int ndots = 1;  // 设置调用res_query()解析域名时域名至少包含的点的数量
			int attempts = 2;  // 设置resolver向DNS服务器发起域名解析的请求次数。默认值RES_DFLRETRY=2，参见<resolv.h>
			bool rotate = false;  // 在_res.options中设置RES_ROTATE，采用轮询方式访问nameserver，实现负载均衡
			std::string url; 
			// 来当访问的域名不能被DNS解析时，resolver会将该域名加上search指定的参数，重新请求DNS
			// 直到被正确解析或试完search指定的列表为止。
			std::string search;
			
			__parse_resolv_conf(path, url, search, &ndots, &attempts, &rotate);
			if (url.size() == 0)  // 如果没有的话，默认为google的 8.8.8.8
				url = "8.8.8.8";

			client = new WFDnsClient;  

			if (client->init(url, search, ndots, attempts, rotate) >= 0)
				return;  // 成功
			
			// 失败则把client复原
			delete client;
			client = NULL;
		}
	}

	~__DnsClientManager()
	{
		if (client)
		{
			client->deinit();
			delete client;
		}
	}

	WFDnsClient *client;
};

WFDnsClient *WFGlobal::get_dns_client()
{
	return __DnsClientManager::get_instance()->get_dns_client();
}

/*
在编程中，对于全局的、需要共享的资源，一种常见的管理方式是使用单例模式。在这种模式下，一个类只有一个实例存在，这个实例可以被全局访问。

`CommScheduler` 是用于调度和执行通信任务的对象，它包含了一个或多个线程，这些线程需要在全局共享。在这种情况下，使用一个全局的、单例的管理者 `__CommManager` 来管理这个资源是合理的。这就是 `WFGlobal::get_scheduler()` 通过 `__CommManager::get_instance()->get_scheduler()` 获取 `CommScheduler` 对象的原因。

总的来说，这样做的原因有两个：

1. **封装性**：对于调用 `WFGlobal::get_scheduler()` 的代码来说，它并不需要知道 `CommScheduler` 的生命周期和初始化过程，它只需要通过这个函数就可以获取到 `CommScheduler` 对象。

2. **单一职责**：`__CommManager` 只负责管理 `CommScheduler` 对象，使得代码的职责分明，易于维护和理解。

以上就是 `WFGlobal::get_scheduler()` 通过 `__CommManager::get_instance()->get_scheduler()` 获取 `CommScheduler` 对象的原因。
*/

CommScheduler *WFGlobal::get_scheduler()
{
	return __CommManager::get_instance()->get_scheduler();
}

DnsCache *WFGlobal::get_dns_cache()
{
	return __DnsCache::get_instance()->get_dns_cache();
}

RouteManager *WFGlobal::get_route_manager()
{
	return __CommManager::get_instance()->get_route_manager();
}

SSL_CTX *WFGlobal::get_ssl_client_ctx()
{
	return __SSLManager::get_instance()->get_ssl_client_ctx();
}

SSL_CTX *WFGlobal::new_ssl_server_ctx()
{
	return __SSLManager::get_instance()->new_ssl_server_ctx();
}

ExecQueue *WFGlobal::get_exec_queue(const std::string& queue_name)
{
	return __ExecManager::get_instance()->get_exec_queue(queue_name);
}

Executor *WFGlobal::get_compute_executor()
{
	return __ExecManager::get_instance()->get_compute_executor();
}

IOService *WFGlobal::get_io_service()
{
	return __CommManager::get_instance()->get_io_service();
}

ExecQueue *WFGlobal::get_dns_queue()
{
	return __CommManager::get_instance()->get_dns_queue();
}

Executor *WFGlobal::get_dns_executor()
{
	return __CommManager::get_instance()->get_dns_executor();
}

WFNameService *WFGlobal::get_name_service()
{
	return __NameServiceManager::get_instance()->get_name_service();
}

WFDnsResolver *WFGlobal::get_dns_resolver()
{
	return __NameServiceManager::get_instance()->get_dns_resolver();
}

const char *WFGlobal::get_default_port(const std::string& scheme)
{
	return __WFGlobal::get_instance()->get_default_port(scheme);
}

void WFGlobal::register_scheme_port(const std::string& scheme,
									unsigned short port)
{
	__WFGlobal::get_instance()->register_scheme_port(scheme, port);
}

const WFGlobalSettings *WFGlobal::get_global_settings()
{
	return __WFGlobal::get_instance()->get_global_settings();
}

void WORKFLOW_library_init(const WFGlobalSettings *settings)
{
	__WFGlobal::get_instance()->set_global_settings(settings);
}

void WFGlobal::sync_operation_begin()
{
	__WFGlobal::get_instance()->sync_operation_begin();
}

void WFGlobal::sync_operation_end()
{
	__WFGlobal::get_instance()->sync_operation_end();
}

static inline const char *__get_ssl_error_string(int error)
{
	switch (error)
	{
	case SSL_ERROR_NONE:
		return "SSL Error None";

	case SSL_ERROR_ZERO_RETURN:
		return "SSL Error Zero Return";

	case SSL_ERROR_WANT_READ:
		return "SSL Error Want Read";

	case SSL_ERROR_WANT_WRITE:
		return "SSL Error Want Write";

	case SSL_ERROR_WANT_CONNECT:
		return "SSL Error Want Connect";

	case SSL_ERROR_WANT_ACCEPT:
		return "SSL Error Want Accept";

	case SSL_ERROR_WANT_X509_LOOKUP:
		return "SSL Error Want X509 Lookup";

#ifdef SSL_ERROR_WANT_ASYNC
	case SSL_ERROR_WANT_ASYNC:
		return "SSL Error Want Async";
#endif

#ifdef SSL_ERROR_WANT_ASYNC_JOB
	case SSL_ERROR_WANT_ASYNC_JOB:
		return "SSL Error Want Async Job";
#endif

#ifdef SSL_ERROR_WANT_CLIENT_HELLO_CB
	case SSL_ERROR_WANT_CLIENT_HELLO_CB:
		return "SSL Error Want Client Hello CB";
#endif

	case SSL_ERROR_SYSCALL:
		return "SSL System Error";

	case SSL_ERROR_SSL:
		return "SSL Error SSL";

	default:
		break;
	}

	return "Unknown";
}

static inline const char *__get_task_error_string(int error)
{
	switch (error)
	{
	case WFT_ERR_URI_PARSE_FAILED:
		return "URI Parse Failed";

	case WFT_ERR_URI_SCHEME_INVALID:
		return "URI Scheme Invalid";

	case WFT_ERR_URI_PORT_INVALID:
		return "URI Port Invalid";

	case WFT_ERR_UPSTREAM_UNAVAILABLE:
		return "Upstream Unavailable";

	case WFT_ERR_HTTP_BAD_REDIRECT_HEADER:
		return "Http Bad Redirect Header";

	case WFT_ERR_HTTP_PROXY_CONNECT_FAILED:
		return "Http Proxy Connect Failed";

	case WFT_ERR_REDIS_ACCESS_DENIED:
		return "Redis Access Denied";

	case WFT_ERR_REDIS_COMMAND_DISALLOWED:
		return "Redis Command Disallowed";

	case WFT_ERR_MYSQL_HOST_NOT_ALLOWED:
		return "MySQL Host Not Allowed";

	case WFT_ERR_MYSQL_ACCESS_DENIED:
		return "MySQL Access Denied";

	case WFT_ERR_MYSQL_INVALID_CHARACTER_SET:
		return "MySQL Invalid Character Set";

	case WFT_ERR_MYSQL_COMMAND_DISALLOWED:
		return "MySQL Command Disallowed";

	case WFT_ERR_MYSQL_QUERY_NOT_SET:
		return "MySQL Query Not Set";

	case WFT_ERR_MYSQL_SSL_NOT_SUPPORTED:
		return "MySQL SSL Not Supported";

	case WFT_ERR_KAFKA_PARSE_RESPONSE_FAILED:
		return "Kafka parse response failed";

	case WFT_ERR_KAFKA_PRODUCE_FAILED:
		return "Kafka produce api failed";

	case WFT_ERR_KAFKA_FETCH_FAILED:
		return "Kafka fetch api failed";

	case WFT_ERR_KAFKA_CGROUP_FAILED:
		return "Kafka cgroup failed";

	case WFT_ERR_KAFKA_COMMIT_FAILED:
		return "Kafka commit api failed";

	case WFT_ERR_KAFKA_META_FAILED:
		return "Kafka meta api failed";

	case WFT_ERR_KAFKA_LEAVEGROUP_FAILED:
		return "Kafka leavegroup failed";

	case WFT_ERR_KAFKA_API_UNKNOWN:
		return "Kafka api type unknown";

	case WFT_ERR_KAFKA_VERSION_DISALLOWED:
		return "Kafka broker version not supported";

	default:
		break;
	}

	return "Unknown";
}

const char *WFGlobal::get_error_string(int state, int error)
{
	switch (state)
	{
	case WFT_STATE_SUCCESS:
		return "Success";

	case WFT_STATE_TOREPLY:
		return "To Reply";

	case WFT_STATE_NOREPLY:
		return "No Reply";

	case WFT_STATE_SYS_ERROR:
		return strerror(error);

	case WFT_STATE_SSL_ERROR:
		return __get_ssl_error_string(error);

	case WFT_STATE_DNS_ERROR:
		return gai_strerror(error);

	case WFT_STATE_TASK_ERROR:
		return __get_task_error_string(error);

	case WFT_STATE_UNDEFINED:
		return "Undefined";

	default:
		break;
	}

	return "Unknown";
}

