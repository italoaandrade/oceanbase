/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#define USING_LOG_PREFIX LIB
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include "lib/charset/ob_charset.h"
#include "lib/oblog/ob_log.h"
#include "lib/utility/ob_print_utils.h"
#include "lib/net/ob_net_util.h"

using namespace oceanbase::common;

namespace oceanbase {
namespace obsys {

int ObNetUtil::get_local_addr_ipv6(const char *dev_name, char *ipv6, int len,
                                   bool *is_linklocal)
{
  int ret = OB_ERROR;
  int level = -1; // 0: loopback; 1: linklocal; 2: sitelocal; 3: v4mapped; 4: global
  struct ifaddrs *ifa = nullptr, *ifa_tmp = nullptr;

  if (nullptr == dev_name) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("devname can not be NULL", K(ret));
  } else if (len < INET6_ADDRSTRLEN) {
    ret = OB_BUF_NOT_ENOUGH;
    LOG_WARN("the buffer size cannot be less than INET6_ADDRSTRLEN",
             "INET6_ADDRSTRLEN", INET6_ADDRSTRLEN, K(len), K(ret));
  } else if (-1 == getifaddrs(&ifa)) {
    ret = OB_ERR_SYS;
    LOG_WARN("call getifaddrs fail", K(errno), K(ret));
  } else {
    ifa_tmp = ifa;
    while (ifa_tmp) {
      if (ifa_tmp->ifa_addr &&
          ifa_tmp->ifa_addr->sa_family == AF_INET6 &&
          0 == strcmp(ifa_tmp->ifa_name, dev_name)) {
        struct sockaddr_in6 *in6 = (struct sockaddr_in6 *) ifa_tmp->ifa_addr;
        int cur_level = -1;
        bool linklocal = false;
        if (IN6_IS_ADDR_LOOPBACK(&in6->sin6_addr)) {
          cur_level = 0;
        } else if (IN6_IS_ADDR_LINKLOCAL(&in6->sin6_addr)) {
          cur_level = 1;
          linklocal = true;
        } else if (IN6_IS_ADDR_SITELOCAL(&in6->sin6_addr)) {
          cur_level = 2;
        } else if (IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr)) {
          cur_level = 3;
        } else {
          cur_level = 4;
        }
        if (cur_level > level) {
          if (nullptr == inet_ntop(AF_INET6, &in6->sin6_addr, ipv6, len)) {
            ret = OB_ERR_SYS;
            LOG_WARN("call inet_ntop fail", K(errno), K(ret));
          } else {
            level = cur_level;
            ret = OB_SUCCESS;
            if (nullptr != is_linklocal) {
              *is_linklocal = linklocal;
            }
          }
        }
      } // if end
      ifa_tmp = ifa_tmp->ifa_next;
    } // while end

    // if dev_name is invalid, then level will keep its initial value(-1)
    if (level == -1) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid devname specified by -i", "devname", dev_name, "info",
               "you may list devnames by shell command: ifconfig");
    }
  }

  if (nullptr != ifa) {
    freeifaddrs(ifa);
  }

  return ret;
}

int ObNetUtil::get_local_addr_ipv4(const char *dev_name, uint32_t &addr)
{
  int ret = OB_SUCCESS;
  struct ifaddrs *ifa = nullptr, *ifa_tmp = nullptr;

  if (nullptr == dev_name) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("devname can not be NULL", K(ret));
  } else if (-1 == getifaddrs(&ifa)) {
    ret = OB_ERR_SYS;
    LOG_WARN("call getifaddrs fail", K(errno), K(ret));
  } else {
    ifa_tmp = ifa;
    bool has_found = false;
    while (nullptr != ifa_tmp && !has_found) {
      if (ifa_tmp->ifa_addr &&
          ifa_tmp->ifa_addr->sa_family == AF_INET &&
          0 == strcmp(ifa_tmp->ifa_name, dev_name)) {
        has_found = true;
        struct sockaddr_in *in = (struct sockaddr_in *) ifa_tmp->ifa_addr;
        addr = in->sin_addr.s_addr;
      } // if end
      ifa_tmp = ifa_tmp->ifa_next;
    } // while end

    if (!has_found) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid devname specified by -i", "devname", dev_name, "info",
               "you may list devnames by shell command: ifconfig");
    }
  }

  if (nullptr != ifa) {
    freeifaddrs(ifa);
  }

  return ret;
}

std::string ObNetUtil::addr_to_string(uint64_t ipport)
{
    char str[32];
    uint32_t ip = (uint32_t)(ipport & 0xffffffff);
    int port = (int)((ipport >> 32 ) & 0xffff);
    unsigned char *bytes = (unsigned char *) &ip;
    if (port > 0) {
        snprintf(str, sizeof(str), "%d.%d.%d.%d:%d", bytes[0], bytes[1], bytes[2], bytes[3], port);
    } else {
        snprintf(str, sizeof(str), "%d.%d.%d.%d", bytes[0], bytes[1], bytes[2], bytes[3]);
    }
    return str;
}

uint64_t ObNetUtil::ip_to_addr(uint32_t ip, int port)
{
    uint64_t ipport = port;
    ipport <<= 32;
    ipport |= ip;
    return ipport;
}

char *ObNetUtil::get_addr_by_hostname(const char *hostname)
{
  char *addr = nullptr;
  hostent *host = gethostbyname(hostname);

  if (nullptr != host) {
      addr = inet_ntoa(**(in_addr**)host->h_addr_list);
  }

  return addr;
}

int ObNetUtil::get_ifname_by_addr(const char *local_ip, char *if_name, uint64_t if_name_len)
{
  int ret = OB_SUCCESS;
  struct in_addr ip;
  struct in6_addr ip6;
  int af_type = AF_INET;

  if (1 == inet_pton(AF_INET, local_ip, &ip)) {
    // do nothing
  } else if (1 == inet_pton(AF_INET6, local_ip, &ip6)) {
    af_type = AF_INET6;
  } else {
    ret = OB_ERR_SYS;
    LOG_ERROR("call inet_pton failed, maybe the local_ip is invalid",
               KCSTRING(local_ip), K(errno), K(ret));
  }

  if (OB_SUCCESS == ret) {
    struct ifaddrs *ifa_list = nullptr, *ifa = nullptr;
    if (-1 == getifaddrs(&ifa_list)) {
      ret = OB_ERR_SYS;
      LOG_ERROR("call getifaddrs failed", K(errno), K(ret));
    } else {
      bool has_found = false;
      for (ifa = ifa_list; nullptr != ifa && !has_found; ifa = ifa->ifa_next) {
        if (nullptr != ifa->ifa_addr &&
            ((AF_INET == af_type && AF_INET == ifa->ifa_addr->sa_family &&
                 0 == memcmp(&ip, &(((struct sockaddr_in *)ifa->ifa_addr)->sin_addr), sizeof(ip))) ||
             (AF_INET6 == af_type && AF_INET6 == ifa->ifa_addr->sa_family &&
                 0 == memcmp(&ip6, &(((struct sockaddr_in6 *)ifa->ifa_addr)->sin6_addr), sizeof(ip6))))) {
          has_found = true;
          if (if_name_len < strlen(ifa->ifa_name) + 1) {
            ret = OB_BUF_NOT_ENOUGH;
            _LOG_ERROR("the buffer is not enough, need:%lu, have:%lu, ret:%d",
                       strlen(ifa->ifa_name) + 1, if_name_len, ret);
          } else {
            snprintf(if_name, if_name_len, "%s", ifa->ifa_name);
          }
        }
      } // end for
      if (!has_found) {
        ret = OB_SEARCH_NOT_FOUND;
        LOG_ERROR("can not find ifname by local ip", KCSTRING(local_ip));
      }
    }
    if (nullptr != ifa_list) {
      freeifaddrs(ifa_list);
    }
  }
  return ret;
}

int ObNetUtil::get_int_value(const ObString &str, int64_t &value)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(str.empty())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("str is empty", K(str), K(ret));
  } else {
    static const int32_t MAX_INT64_STORE_LEN = 31;
    char int_buf[MAX_INT64_STORE_LEN + 1];
    int64_t len = std::min(str.length(), MAX_INT64_STORE_LEN);
    MEMCPY(int_buf, str.ptr(), len);
    int_buf[len] = '\0';
    char *end_ptr = NULL;
    value = strtoll(int_buf, &end_ptr, 10);
    if (('\0' != *int_buf) && ('\0' == *end_ptr)) {
      // succ, do nothing
    } else {
      ret = OB_INVALID_DATA;
      LOG_WARN("invalid int value", K(value), K(str), K(ret));
    }
  }
  return ret;
}

bool ObNetUtil::calc_ip(const ObString &host_ip, ObAddr &addr)
{
  return addr.set_ip_addr(host_ip, FAKE_PORT);
}

bool ObNetUtil::calc_ip_mask(const ObString &host_name, ObAddr &host, ObAddr &mask)
{
  bool ret_bool = false;
  int64_t ip_mask_int64 = 0;
  ObString host_ip_mask = host_name;
  ObString ip = host_ip_mask.split_on('/');
  if (OB_UNLIKELY(host_ip_mask.empty())) {
  } else if (calc_ip(ip, host)) {
    if (host_ip_mask.find('.') || host_ip_mask.find(':')) {
      ret_bool = mask.set_ip_addr(host_ip_mask, FAKE_PORT);
    } else {
      if (OB_SUCCESS != (get_int_value(host_ip_mask, ip_mask_int64))) {
        // break
      } else if (mask.as_mask(ip_mask_int64, host.get_version())) {
        ret_bool = true;
      }
    }
  }
  return ret_bool;
}

bool ObNetUtil::is_ip_match(const ObString &client_ip, ObString host_name)
{
  bool ret_bool = false;
  ObAddr client;
  ObAddr host;
  if (OB_UNLIKELY(host_name.empty()) || OB_UNLIKELY(client_ip.empty())) {
    // not match
  } else if (host_name.find('/')) {
    ObAddr mask;
    if (calc_ip(client_ip, client) && calc_ip_mask(host_name, host, mask) &&
        client.get_version() == host.get_version()) {
      ret_bool = (client.as_subnet(mask) == host.as_subnet(mask));
    }
  } else {
    if (calc_ip(host_name, host) && calc_ip(client_ip, client)) {
      ret_bool = (client == host);
    }
  }
  return ret_bool;
}

bool ObNetUtil::is_wild_match(const ObString &client_ip, const ObString &host_name)
{
  return ObCharset::wildcmp(CS_TYPE_UTF8MB4_BIN, client_ip, host_name, 0, '_', '%');
}

bool ObNetUtil::is_match(const ObString &client_ip, const ObString &host_name)
{
  return ObNetUtil::is_wild_match(client_ip, host_name) || ObNetUtil::is_ip_match(client_ip, host_name);
}

bool ObNetUtil::is_in_white_list(const ObString &client_ip, ObString &orig_ip_white_list)
{
  bool ret_bool = false;
  ObAddr client;
  if (orig_ip_white_list.empty() || client_ip.empty()) {
    LOG_WARN_RET(OB_SUCCESS, "ip_white_list or client_ip is emtpy, denied any client", K(client_ip), K(orig_ip_white_list));
  } else if (calc_ip(client_ip, client)) {
    const char COMMA = ',';
    ObString ip_white_list = orig_ip_white_list;
    while (NULL != ip_white_list.find(COMMA) && !ret_bool) {
      ObString invited_ip = ip_white_list.split_on(COMMA).trim();
      if (!invited_ip.empty()) {
        if (ObNetUtil::is_match(client_ip, invited_ip)) {
          ret_bool = true;
        }
        LOG_TRACE("match result", K(ret_bool), K(client_ip), K(invited_ip));
      }
    }
    if (!ret_bool) {
      ip_white_list = ip_white_list.trim();
      if (ip_white_list.empty()) {
        LOG_WARN_RET(OB_SUCCESS, "ip_white_list is emtpy, denied any client", K(client_ip), K(orig_ip_white_list));
      } else if (!ObNetUtil::is_match(client_ip, ip_white_list)) {
        LOG_WARN_RET(OB_SUCCESS, "client ip is not in ip_white_list", K(client_ip), K(orig_ip_white_list));
      } else {
        ret_bool = true;
        LOG_TRACE("match result", K(ret_bool), K(client_ip), K(ip_white_list));
      }
    }
  }
  return ret_bool;
}

}  // namespace obsys
}  // namespace oceanbase
