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
*/

#ifndef _WFGLOBAL_H_
#define _WFGLOBAL_H_

#if __cplusplus < 201100
#error CPLUSPLUS VERSION required at least C++11. Please use "-std=c++11".
#include <C++11_REQUIRED>
#endif

#include <string>
#include <openssl/ssl.h>
#include "CommScheduler.h"
#include "DnsCache.h"
#include "RouteManager.h"
#include "Executor.h"
#include "EndpointParams.h"
#include "WFNameService.h"
#include "WFDnsResolver.h"
#include "logger.h"
/**
 * @file    WFGlobal.h
 * @brief   Workflow Global Settings & Workflow Global APIs
 */

/**
 * @brief   Workflow Library Global Setting
 * @details
 * If you want set different settings with default, please call WORKFLOW_library_init at the beginning of the process
*/
struct WFGlobalSettings
{
	struct EndpointParams endpoint_params;
	struct EndpointParams dns_server_params;
	unsigned int dns_ttl_default;	///< in seconds, DNS TTL when network request success
	unsigned int dns_ttl_min;		///< in seconds, DNS TTL when network request fail
	int dns_threads;
	int poller_threads;
	int handler_threads;
	int compute_threads;			///< auto-set by system CPU number if value<=0
	const char *resolv_conf_path;
	const char *hosts_path;
};

/**
 * @brief   Default Workflow Library Global Settings
 */
static constexpr struct WFGlobalSettings GLOBAL_SETTINGS_DEFAULT =
{
	.endpoint_params	=	ENDPOINT_PARAMS_DEFAULT,
	.dns_server_params	=	ENDPOINT_PARAMS_DEFAULT,
	.dns_ttl_default	=	12 * 3600,
	.dns_ttl_min		=	180,
	.dns_threads		=	4,  
	.poller_threads		=	4,
	.handler_threads	=	20,
	.compute_threads	=	-1,
	.resolv_conf_path	=	"/etc/resolv.conf",
	.hosts_path			=	"/etc/hosts",
};

/**
 * @brief      Reset Workflow Library Global Setting
 * @param[in]  settings          custom settings pointer
*/
extern void WORKFLOW_library_init(const struct WFGlobalSettings *settings);

/**
 * @brief   Workflow 全局管理类
 * @details Workflow 全局 API
 */
class WFGlobal
{
public:
	/**
	 * @brief      注册一个方案的默认端口
	 * @param[in]  scheme           方案字符串
	 * @param[in]  port             默认端口值
	 * @warning    当方案为 "http"/"https"/"redis"/"rediss"/"mysql"/"kafka" 时无效
	 */
	static void register_scheme_port(const std::string& scheme, unsigned short port);

	/**
	 * @brief      获取一个方案的默认端口字符串
	 * @param[in]  scheme           方案字符串
	 * @return     端口字符串常量指针
	 * @retval     NULL             失败，未找到方案
	 * @retval     非 NULL          成功
	 */
	static const char *get_default_port(const std::string& scheme);

	/**
	 * @brief      获取当前全局设置
	 * @return     当前全局设置常量指针
	 * @note       返回值永不为 NULL
	 */
	static const struct WFGlobalSettings *get_global_settings();

	/**
	 * @brief      获取错误字符串
	 * @param[in]  state            错误状态
	 * @param[in]  error            错误码
	 * @return     错误字符串
	 */
	static const char *get_error_string(int state, int error);

	// 仅限内部使用
public:
	/**
	 * @brief      获取通信调度器
	 * @return     通信调度器指针
	 */
	static CommScheduler *get_scheduler();

	/**
	 * @brief      获取 DNS 缓存
	 * @return     DNS 缓存指针
	 */
	static DnsCache *get_dns_cache();

	/**
	 * @brief      获取路由管理器
	 * @return     路由管理器指针
	 */
	static RouteManager *get_route_manager();

	/**
	 * @brief      获取 SSL 客户端上下文
	 * @return     SSL 客户端上下文指针
	 */
	static SSL_CTX *get_ssl_client_ctx();

	/**
	 * @brief      创建新的 SSL 服务器上下文
	 * @return     SSL 服务器上下文指针
	 */
	static SSL_CTX *new_ssl_server_ctx();

	/**
	 * @brief      获取执行队列
	 * @param[in]  queue_name       执行队列名称
	 * @return     执行队列指针
	 */
	static ExecQueue *get_exec_queue(const std::string& queue_name);

	/**
	 * @brief      获取计算执行器
	 * @return     计算执行器指针
	 */
	static Executor *get_compute_executor();

	/**
	 * @brief      获取 IO 服务
	 * @return     IO 服务指针
	 */
	static IOService *get_io_service();

	/**
	 * @brief      获取 DNS 队列
	 * @return     DNS 队列指针
	 */
	static ExecQueue *get_dns_queue();

	/**
	 * @brief      获取 DNS 执行器
	 * @return     DNS 执行器指针
	 */
	static Executor *get_dns_executor();

	/**
	 * @brief      获取名称服务
	 * @return     名称服务指针
	 */
	static WFNameService *get_name_service();

	/**
	 * @brief      获取 DNS 解析器
	 * @return     DNS 解析器指针
	 */
	static WFDnsResolver *get_dns_resolver();

	/**
	 * @brief      获取 DNS 客户端
	 * @return     DNS 客户端指针
	 */
	static class WFDnsClient *get_dns_client();

public:
	/**
	 * @brief      开始一个同步操作
	 */
	static void sync_operation_begin();

	/**
	 * @brief      结束一个同步操作
	 */
	static void sync_operation_end();
};

#endif

