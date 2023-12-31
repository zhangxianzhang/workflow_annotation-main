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

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <netdb.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include "DnsRoutine.h"

#define PORT_STR_MAX	5

DnsOutput::DnsOutput(DnsOutput&& move)
{
	error_ = move.error_;
	addrinfo_ = move.addrinfo_;

	move.error_ = 0;
	move.addrinfo_ = NULL;
}

DnsOutput& DnsOutput::operator= (DnsOutput&& move)
{
	if (this != &move)
	{
		if (addrinfo_)
			freeaddrinfo(addrinfo_);

		error_ = move.error_;
		addrinfo_ = move.addrinfo_;

		move.error_ = 0;
		move.addrinfo_ = NULL;
	}

	return *this;
}

void DnsRoutine::run_local_path(const std::string& path, DnsOutput *out)
{
	struct sockaddr_un *sun = NULL;

	if (path.size() + 1 <= sizeof sun->sun_path)
	{
		size_t size = sizeof (struct addrinfo) + sizeof (struct sockaddr_un);

		out->addrinfo_ = (struct addrinfo *)calloc(size, 1);
		if (out->addrinfo_)
		{
			sun = (struct sockaddr_un *)(out->addrinfo_ + 1);
			sun->sun_family = AF_UNIX;
			memcpy(sun->sun_path, path.c_str(), path.size());

			out->addrinfo_->ai_family = AF_UNIX;
			out->addrinfo_->ai_socktype = SOCK_STREAM;
			out->addrinfo_->ai_addr = (struct sockaddr *)sun;
			size = offsetof(struct sockaddr_un, sun_path) + path.size() + 1;
			out->addrinfo_->ai_addrlen = size;
			out->error_ = 0;
			return;
		}
	}
	else
		errno = EINVAL;

	out->error_ = EAI_SYSTEM;
}

void DnsRoutine::run(const DnsInput *in, DnsOutput *out)
{
	// 当这个host_是个本地路径
	if (!in->host_.empty() && in->host_[0] == '/')
	{
		run_local_path(in->host_, out);
		return;
	}

	struct addrinfo hints = {
#ifdef AI_ADDRCONFIG
		// AI_ADDRCONFIG: Which address type (IPv4 or IPv6) is requested to be configured.
		.ai_flags    = AI_ADDRCONFIG,
#endif
		.ai_family   = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM
	};
	char port_str[PORT_STR_MAX + 1];

	snprintf(port_str, PORT_STR_MAX + 1, "%u", in->port_);
	
	// 没有cache，我们最终是通过getaddrinfo来获取到addrinfo
	// https://man7.org/linux/man-pages/man3/getaddrinfo.3.html
	// The getaddrinfo() function combines the functionality provided 
	// by the gethostbyname(3) and getservbyname(3) functions into a single interface
	/*
	int getaddrinfo(const char *restrict node,
					const char *restrict service,
					const struct addrinfo *restrict hints,
					struct addrinfo **restrict res);
	*/
	out->error_ = getaddrinfo(in->host_.c_str(),
							  port_str,
							  &hints,
							  &out->addrinfo_);
}

