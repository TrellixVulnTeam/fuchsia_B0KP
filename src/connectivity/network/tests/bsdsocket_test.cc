// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// These tests ensure fdio can talk to netstack.
// No network connection is required, only a running netstack binary.

#include <arpa/inet.h>
#include <fcntl.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/if_ether.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include <array>
#include <future>
#include <latch>
#include <thread>

#include <fbl/auto_call.h>
#include <fbl/unique_fd.h>
#include <gtest/gtest.h>

#include "util.h"

namespace {

TEST(LocalhostTest, SendToZeroPort) {
  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port = htons(0),
      .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
  };

  fbl::unique_fd fd;
  ASSERT_TRUE(fd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, 0))) << strerror(errno);
  ASSERT_EQ(sendto(fd.get(), nullptr, 0, 0, reinterpret_cast<const struct sockaddr*>(&addr),
                   sizeof(addr)),
            -1);
  ASSERT_EQ(errno, EINVAL) << strerror(errno);

  addr.sin_port = htons(1234);
  ASSERT_EQ(sendto(fd.get(), nullptr, 0, 0, reinterpret_cast<const struct sockaddr*>(&addr),
                   sizeof(addr)),
            0)
      << strerror(errno);
}

TEST(LocalhostTest, DatagramSocketIgnoresMsgWaitAll) {
  fbl::unique_fd recvfd;
  ASSERT_TRUE(recvfd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0)))
      << strerror(errno);

  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
  };

  ASSERT_EQ(bind(recvfd.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  ASSERT_EQ(recvfrom(recvfd.get(), nullptr, 0, MSG_WAITALL, nullptr, nullptr), -1);
  ASSERT_EQ(errno, EAGAIN) << strerror(errno);

  ASSERT_EQ(close(recvfd.release()), 0) << strerror(errno);
}

TEST(LocalhostTest, DatagramSocketSendMsgNameLenTooBig) {
  fbl::unique_fd fd;
  ASSERT_TRUE(fd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, 0))) << strerror(errno);

  struct sockaddr_in addr = {
      .sin_family = AF_INET,
  };

  struct msghdr msg = {
      .msg_name = &addr,
      .msg_namelen = sizeof(sockaddr_storage) + 1,
  };

  ASSERT_EQ(sendmsg(fd.get(), &msg, 0), -1);
  ASSERT_EQ(errno, EINVAL) << strerror(errno);

  ASSERT_EQ(close(fd.release()), 0) << strerror(errno);
}

#if !defined(__Fuchsia__)
bool IsRoot() {
  uid_t ruid, euid, suid;
  EXPECT_EQ(getresuid(&ruid, &euid, &suid), 0) << strerror(errno);
  gid_t rgid, egid, sgid;
  EXPECT_EQ(getresgid(&rgid, &egid, &sgid), 0) << strerror(errno);
  auto uids = {ruid, euid, suid};
  auto gids = {rgid, egid, sgid};
  return std::all_of(std::begin(uids), std::end(uids), [](uid_t uid) { return uid == 0; }) &&
         std::all_of(std::begin(gids), std::end(gids), [](gid_t gid) { return gid == 0; });
}
#endif

TEST(LocalhostTest, BindToDevice) {
#if !defined(__Fuchsia__)
  if (!IsRoot()) {
    GTEST_SKIP() << "This test requires root";
  }
#endif

  fbl::unique_fd fd;
  ASSERT_TRUE(fd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP))) << strerror(errno);

  {
    // The default is that a socket is not bound to a device.
    char get_dev[IFNAMSIZ] = {};
    socklen_t get_dev_length = sizeof(get_dev);
    EXPECT_EQ(getsockopt(fd.get(), SOL_SOCKET, SO_BINDTODEVICE, get_dev, &get_dev_length), 0)
        << strerror(errno);
    EXPECT_EQ(get_dev_length, socklen_t(0));
    EXPECT_STREQ(get_dev, "");
  }

  const char set_dev[IFNAMSIZ] = "lo\0blahblah";

  // Bind to "lo" with null termination should work even if the size is too big.
  ASSERT_EQ(setsockopt(fd.get(), SOL_SOCKET, SO_BINDTODEVICE, set_dev, sizeof(set_dev)), 0)
      << strerror(errno);

  const char set_dev_unknown[] = "loblahblahblah";
  // Bind to "lo" without null termination but with accurate length should work.
  EXPECT_EQ(setsockopt(fd.get(), SOL_SOCKET, SO_BINDTODEVICE, set_dev_unknown, 2), 0)
      << strerror(errno);

  // Bind to unknown name should fail.
  EXPECT_EQ(
      setsockopt(fd.get(), SOL_SOCKET, SO_BINDTODEVICE, "loblahblahblah", sizeof(set_dev_unknown)),
      -1);
  EXPECT_EQ(errno, ENODEV) << strerror(errno);

  {
    // Reading it back should work.
    char get_dev[IFNAMSIZ] = {};
    socklen_t get_dev_length = sizeof(get_dev);
    EXPECT_EQ(getsockopt(fd.get(), SOL_SOCKET, SO_BINDTODEVICE, get_dev, &get_dev_length), 0)
        << strerror(errno);
    EXPECT_EQ(get_dev_length, strlen(set_dev) + 1);
    EXPECT_STREQ(get_dev, set_dev);
  }

  {
    // Reading it back without enough space in the buffer should fail.
    char get_dev[] = "";
    socklen_t get_dev_length = sizeof(get_dev);
    EXPECT_EQ(getsockopt(fd.get(), SOL_SOCKET, SO_BINDTODEVICE, get_dev, &get_dev_length), -1);
    EXPECT_EQ(errno, EINVAL) << strerror(errno);
    EXPECT_EQ(get_dev_length, sizeof(get_dev));
    EXPECT_STREQ(get_dev, "");
  }

  EXPECT_EQ(close(fd.release()), 0) << strerror(errno);
}

// Raw sockets are typically used for implementing custom protocols. We intend to support custom
// protocols through structured FIDL APIs in the future, so this test ensures that raw sockets are
// disabled to prevent them from accidentally becoming load-bearing.
TEST(LocalhostTest, RawSocketsNotSupported) {
  // No raw INET sockets.
  ASSERT_EQ(socket(AF_INET, SOCK_RAW, 0), -1);
  ASSERT_EQ(errno, EPROTONOSUPPORT) << strerror(errno);

  // No packet sockets.
  ASSERT_EQ(socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL)), -1);
  ASSERT_EQ(errno, EPERM) << strerror(errno);
}

TEST(LocalhostTest, IpAddMembershipAny) {
  fbl::unique_fd s;
  ASSERT_TRUE(s = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP))) << strerror(errno);

  ip_mreqn param = {
      .imr_address.s_addr = htonl(INADDR_ANY),
      .imr_ifindex = 1,
  };
  int n = inet_pton(AF_INET, "224.0.2.1", &param.imr_multiaddr.s_addr);
  ASSERT_GE(n, 0) << strerror(errno);
  ASSERT_EQ(n, 1);
  ASSERT_EQ(setsockopt(s.get(), SOL_IP, IP_ADD_MEMBERSHIP, &param, sizeof(param)), 0)
      << strerror(errno);

  ASSERT_EQ(close(s.release()), 0) << strerror(errno);
}

struct SockOption {
  int level;
  int option;
};

constexpr int INET_ECN_MASK = 3;

std::string socketTypeToString(const int type) {
  switch (type) {
    case SOCK_DGRAM:
      return "Datagram";
    case SOCK_STREAM:
      return "Stream";
    default:
      return std::to_string(type);
  }
}

using SocketKind = std::tuple<int, int>;

std::string socketKindToString(const ::testing::TestParamInfo<SocketKind>& info) {
  auto const& [domain, type] = info.param;

  std::string domain_str;
  switch (domain) {
    case AF_INET:
      domain_str = "IPv4";
      break;
    case AF_INET6:
      domain_str = "IPv6";
      break;
    default:
      domain_str = std::to_string(domain);
      break;
  }
  return domain_str + "_" + socketTypeToString(type);
}

// Share common functions for SocketKind based tests.
class SocketKindTest : public ::testing::TestWithParam<SocketKind> {
 protected:
  static fbl::unique_fd NewSocket() {
    auto const& [domain, type] = GetParam();
    return fbl::unique_fd(socket(domain, type, 0));
  }
};

constexpr int kSockOptOn = 1;
constexpr int kSockOptOff = 0;

class SocketOptsTest : public SocketKindTest {
 protected:
  static bool IsTCP() { return std::get<1>(GetParam()) == SOCK_STREAM; }

  static bool IsIPv6() { return std::get<0>(GetParam()) == AF_INET6; }

  static SockOption GetTOSOption() {
    if (IsIPv6()) {
      return {
          .level = IPPROTO_IPV6,
          .option = IPV6_TCLASS,
      };
    }
    return {
        .level = IPPROTO_IP,
        .option = IP_TOS,
    };
  }

  static SockOption GetMcastLoopOption() {
    if (IsIPv6()) {
      return {
          .level = IPPROTO_IPV6,
          .option = IPV6_MULTICAST_LOOP,
      };
    }
    return {
        .level = IPPROTO_IP,
        .option = IP_MULTICAST_LOOP,
    };
  }

  static SockOption GetMcastTTLOption() {
    if (IsIPv6()) {
      return {
          .level = IPPROTO_IPV6,
          .option = IPV6_MULTICAST_HOPS,
      };
    }
    return {
        .level = IPPROTO_IP,
        .option = IP_MULTICAST_TTL,
    };
  }

  static SockOption GetMcastIfOption() {
    if (IsIPv6()) {
      return {
          .level = IPPROTO_IPV6,
          .option = IPV6_MULTICAST_IF,
      };
    }
    return {
        .level = IPPROTO_IP,
        .option = IP_MULTICAST_IF,
    };
  }

  static SockOption GetRecvTOSOption() {
    if (IsIPv6()) {
      return {
          .level = IPPROTO_IPV6,
          .option = IPV6_RECVTCLASS,
      };
    }
    return {
        .level = IPPROTO_IP,
        .option = IP_RECVTOS,
    };
  }

  static SockOption GetNoChecksum() {
    return {
        .level = SOL_SOCKET,
        .option = SO_NO_CHECK,
    };
  }
};

// The SocketOptsTest is adapted from gvisor/tests/syscalls/linux/socket_ip_unbound.cc
TEST_P(SocketOptsTest, TtlDefault) {
  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  int get = -1;
  socklen_t get_sz = sizeof(get);
  constexpr int kDefaultTTL = 64;
  EXPECT_EQ(getsockopt(s.get(), IPPROTO_IP, IP_TTL, &get, &get_sz), 0) << strerror(errno);
  EXPECT_EQ(get, kDefaultTTL);
  EXPECT_EQ(get_sz, sizeof(get));
  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, SetTtl) {
  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  int get1 = -1;
  socklen_t get1_sz = sizeof(get1);
  EXPECT_EQ(getsockopt(s.get(), IPPROTO_IP, IP_TTL, &get1, &get1_sz), 0) << strerror(errno);
  EXPECT_EQ(get1_sz, sizeof(get1));

  int set = 100;
  if (set == get1) {
    set += 1;
  }
  socklen_t set_sz = sizeof(set);
  EXPECT_EQ(setsockopt(s.get(), IPPROTO_IP, IP_TTL, &set, set_sz), 0) << strerror(errno);

  int get2 = -1;
  socklen_t get2_sz = sizeof(get2);
  EXPECT_EQ(getsockopt(s.get(), IPPROTO_IP, IP_TTL, &get2, &get2_sz), 0) << strerror(errno);
  EXPECT_EQ(get2_sz, sizeof(get2));
  EXPECT_EQ(get2, set);
  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, ResetTtlToDefault) {
  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  int get1 = -1;
  socklen_t get1_sz = sizeof(get1);
  EXPECT_EQ(getsockopt(s.get(), IPPROTO_IP, IP_TTL, &get1, &get1_sz), 0) << strerror(errno);
  EXPECT_EQ(get1_sz, sizeof(get1));

  int set1 = 100;
  if (set1 == get1) {
    set1 += 1;
  }
  socklen_t set1_sz = sizeof(set1);
  EXPECT_EQ(setsockopt(s.get(), IPPROTO_IP, IP_TTL, &set1, set1_sz), 0) << strerror(errno);

  int set2 = -1;
  socklen_t set2_sz = sizeof(set2);
  EXPECT_EQ(setsockopt(s.get(), IPPROTO_IP, IP_TTL, &set2, set2_sz), 0) << strerror(errno);

  int get2 = -1;
  socklen_t get2_sz = sizeof(get2);
  EXPECT_EQ(getsockopt(s.get(), IPPROTO_IP, IP_TTL, &get2, &get2_sz), 0) << strerror(errno);
  EXPECT_EQ(get2_sz, sizeof(get2));
  EXPECT_EQ(get2, get1);
  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, ZeroTtl) {
  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  int set = 0;
  socklen_t set_sz = sizeof(set);
  EXPECT_EQ(setsockopt(s.get(), IPPROTO_IP, IP_TTL, &set, set_sz), -1);
  EXPECT_EQ(errno, EINVAL) << strerror(errno);
  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, InvalidLargeTtl) {
  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  int set = 256;
  socklen_t set_sz = sizeof(set);
  EXPECT_EQ(setsockopt(s.get(), IPPROTO_IP, IP_TTL, &set, set_sz), -1);
  EXPECT_EQ(errno, EINVAL) << strerror(errno);
  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, InvalidNegativeTtl) {
  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  int set = -2;
  socklen_t set_sz = sizeof(set);
  EXPECT_EQ(setsockopt(s.get(), IPPROTO_IP, IP_TTL, &set, set_sz), -1);
  EXPECT_EQ(errno, EINVAL) << strerror(errno);
  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, TOSDefault) {
  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  SockOption t = GetTOSOption();
  int get = -1;
  socklen_t get_sz = sizeof(get);
  constexpr int kDefaultTOS = 0;
  EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, &get_sz), 0) << strerror(errno);
  EXPECT_EQ(get_sz, sizeof(get));
  EXPECT_EQ(get, kDefaultTOS);
  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, SetTOS) {
  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  int set = 0xC0;
  socklen_t set_sz = sizeof(set);
  SockOption t = GetTOSOption();
  EXPECT_EQ(setsockopt(s.get(), t.level, t.option, &set, set_sz), 0) << strerror(errno);

  int get = -1;
  socklen_t get_sz = sizeof(get);
  EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, &get_sz), 0) << strerror(errno);
  EXPECT_EQ(get_sz, sizeof(get));
  EXPECT_EQ(get, set);
  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, NullTOS) {
  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  socklen_t set_sz = sizeof(int);
  SockOption t = GetTOSOption();
  if (IsIPv6()) {
    EXPECT_EQ(setsockopt(s.get(), t.level, t.option, nullptr, set_sz), 0) << strerror(errno);
  } else {
    EXPECT_EQ(setsockopt(s.get(), t.level, t.option, nullptr, set_sz), -1);
    EXPECT_EQ(errno, EFAULT) << strerror(errno);
  }
  socklen_t get_sz = sizeof(int);
  EXPECT_EQ(getsockopt(s.get(), t.level, t.option, nullptr, &get_sz), -1);
  EXPECT_EQ(errno, EFAULT) << strerror(errno);
  int get = -1;
  EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, nullptr), -1);
  EXPECT_EQ(errno, EFAULT) << strerror(errno);
  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, ZeroTOS) {
  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  int set = 0;
  socklen_t set_sz = sizeof(set);
  SockOption t = GetTOSOption();
  EXPECT_EQ(setsockopt(s.get(), t.level, t.option, &set, set_sz), 0) << strerror(errno);
  int get = -1;
  socklen_t get_sz = sizeof(get);
  EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, &get_sz), 0) << strerror(errno);
  EXPECT_EQ(get_sz, sizeof(get));
  EXPECT_EQ(get, set);
  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, InvalidLargeTOS) {
  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  // Test with exceeding the byte space.
  int set = 256;
  constexpr int kDefaultTOS = 0;
  socklen_t set_sz = sizeof(set);
  SockOption t = GetTOSOption();
  if (IsIPv6()) {
    EXPECT_EQ(setsockopt(s.get(), t.level, t.option, &set, set_sz), -1);
    EXPECT_EQ(errno, EINVAL) << strerror(errno);
  } else {
    EXPECT_EQ(setsockopt(s.get(), t.level, t.option, &set, set_sz), 0) << strerror(errno);
  }
  int get = -1;
  socklen_t get_sz = sizeof(get);
  EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, &get_sz), 0) << strerror(errno);
  EXPECT_EQ(get_sz, sizeof(get));
  EXPECT_EQ(get, kDefaultTOS);
  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, CheckSkipECN) {
  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  int set = 0xFF;
  socklen_t set_sz = sizeof(set);
  SockOption t = GetTOSOption();
  EXPECT_EQ(setsockopt(s.get(), t.level, t.option, &set, set_sz), 0) << strerror(errno);
  int expect = static_cast<uint8_t>(set);
  if (IsTCP()
#if !defined(__Fuchsia__)
      // gvisor-netstack`s implemention of setsockopt(..IPV6_TCLASS..)
      // clears the ECN bits from the TCLASS value. This keeps gvisor
      // in parity with the Linux test-hosts that run a custom kernel.
      // But that is not the behavior of vanilla Linux kernels.
      // This #if can be removed when we migrate away from gvisor-netstack.
      && !IsIPv6()
#endif
  ) {
    expect &= ~INET_ECN_MASK;
  }
  int get = -1;
  socklen_t get_sz = sizeof(get);
  EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, &get_sz), 0) << strerror(errno);
  EXPECT_EQ(get_sz, sizeof(get));
  EXPECT_EQ(get, expect);
  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, ZeroTOSOptionSize) {
  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  int set = 0xC0;
  socklen_t set_sz = 0;
  SockOption t = GetTOSOption();
  if (IsIPv6()) {
    EXPECT_EQ(setsockopt(s.get(), t.level, t.option, &set, set_sz), -1);
    EXPECT_EQ(errno, EINVAL) << strerror(errno);
  } else {
    EXPECT_EQ(setsockopt(s.get(), t.level, t.option, &set, set_sz), 0) << strerror(errno);
  }
  int get = -1;
  socklen_t get_sz = 0;
  EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, &get_sz), 0) << strerror(errno);
  EXPECT_EQ(get_sz, 0u);
  EXPECT_EQ(get, -1);
  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, SmallTOSOptionSize) {
  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  int set = 0xC0;
  constexpr int kDefaultTOS = 0;
  SockOption t = GetTOSOption();
  for (socklen_t i = 1; i < sizeof(int); i++) {
    int expect_tos;
    socklen_t expect_sz;
    if (IsIPv6()) {
      EXPECT_EQ(setsockopt(s.get(), t.level, t.option, &set, i), -1);
      EXPECT_EQ(errno, EINVAL) << strerror(errno);
      expect_tos = kDefaultTOS;
      expect_sz = i;
    } else {
      EXPECT_EQ(setsockopt(s.get(), t.level, t.option, &set, i), 0) << strerror(errno);
      expect_tos = set;
      expect_sz = sizeof(uint8_t);
    }
    uint get = -1;
    socklen_t get_sz = i;
    EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, &get_sz), 0) << strerror(errno);
    EXPECT_EQ(get_sz, expect_sz);
    // Account for partial copies by getsockopt, retrieve the lower
    // bits specified by get_sz, while comparing against expect_tos.
    EXPECT_EQ(get & ~(~0u << (get_sz * 8)), static_cast<uint>(expect_tos));
  }
  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, LargeTOSOptionSize) {
  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  char buffer[100];
  int* set = reinterpret_cast<int*>(buffer);
  // Point to a larger buffer so that the setsockopt does not overrun.
  *set = 0xC0;
  SockOption t = GetTOSOption();
  for (socklen_t i = sizeof(int); i < 10; i++) {
    EXPECT_EQ(setsockopt(s.get(), t.level, t.option, set, i), 0) << strerror(errno);
    int get = -1;
    socklen_t get_sz = i;
    // We expect the system call handler to only copy atmost sizeof(int) bytes
    // as asserted by the check below. Hence, we do not expect the copy to
    // overflow in getsockopt.
    EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, &get_sz), 0) << strerror(errno);
    EXPECT_EQ(get_sz, sizeof(int));
    EXPECT_EQ(get, *set);
  }
  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, NegativeTOS) {
  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  int set = -1;
  socklen_t set_sz = sizeof(set);
  SockOption t = GetTOSOption();
  EXPECT_EQ(setsockopt(s.get(), t.level, t.option, &set, set_sz), 0) << strerror(errno);
  int expect;
  if (IsIPv6()) {
    // On IPv6 TCLASS, setting -1 has the effect of resetting the
    // TrafficClass.
    expect = 0;
  } else {
    expect = static_cast<uint8_t>(set);
    if (IsTCP()) {
      expect &= ~INET_ECN_MASK;
    }
  }
  int get = -1;
  socklen_t get_sz = sizeof(get);
  EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, &get_sz), 0) << strerror(errno);
  EXPECT_EQ(get_sz, sizeof(get));
  EXPECT_EQ(get, expect);
  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, InvalidNegativeTOS) {
  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  int set = -2;
  socklen_t set_sz = sizeof(set);
  SockOption t = GetTOSOption();
  int expect;
  if (IsIPv6()) {
    EXPECT_EQ(setsockopt(s.get(), t.level, t.option, &set, set_sz), -1);
    EXPECT_EQ(errno, EINVAL) << strerror(errno);
    expect = 0;
  } else {
    EXPECT_EQ(setsockopt(s.get(), t.level, t.option, &set, set_sz), 0) << strerror(errno);
    expect = static_cast<uint8_t>(set);
    if (IsTCP()) {
      expect &= ~INET_ECN_MASK;
    }
  }
  int get = 0;
  socklen_t get_sz = sizeof(get);
  EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, &get_sz), 0) << strerror(errno);
  EXPECT_EQ(get_sz, sizeof(get));
  EXPECT_EQ(get, expect);
  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, MulticastLoopDefault) {
  if (IsTCP()) {
    GTEST_SKIP() << "Skip multicast tests on TCP socket";
  }

  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  int get = -1;
  socklen_t get_len = sizeof(get);
  SockOption t = GetMcastLoopOption();
  EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, &get_len), 0) << strerror(errno);
  EXPECT_EQ(get_len, sizeof(get));
  EXPECT_EQ(get, kSockOptOn);

  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, SetMulticastLoop) {
  if (IsTCP()) {
    GTEST_SKIP() << "Skip multicast tests on TCP socket";
  }

  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  SockOption t = GetMcastLoopOption();
  ASSERT_EQ(setsockopt(s.get(), t.level, t.option, &kSockOptOff, sizeof(kSockOptOff)), 0)
      << strerror(errno);

  int get = -1;
  socklen_t get_len = sizeof(get);
  EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, &get_len), 0) << strerror(errno);
  EXPECT_EQ(get_len, sizeof(get));
  EXPECT_EQ(get, kSockOptOff);

  ASSERT_EQ(setsockopt(s.get(), t.level, t.option, &kSockOptOn, sizeof(kSockOptOn)), 0)
      << strerror(errno);

  EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, &get_len), 0) << strerror(errno);
  EXPECT_EQ(get_len, sizeof(get));
  EXPECT_EQ(get, kSockOptOn);

  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, SetMulticastLoopChar) {
  if (IsTCP()) {
    GTEST_SKIP() << "Skip multicast tests on TCP socket";
  }

  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  constexpr char kSockOptOnChar = kSockOptOn;
  constexpr char kSockOptOffChar = kSockOptOff;

  SockOption t = GetMcastLoopOption();
  int want;
  if (IsIPv6()) {
    EXPECT_EQ(setsockopt(s.get(), t.level, t.option, &kSockOptOffChar, sizeof(kSockOptOffChar)),
              -1);
    EXPECT_EQ(errno, EINVAL) << strerror(errno);
    want = kSockOptOnChar;
  } else {
    EXPECT_EQ(setsockopt(s.get(), t.level, t.option, &kSockOptOffChar, sizeof(kSockOptOffChar)), 0)
        << strerror(errno);
    want = kSockOptOffChar;
  }

  int get = -1;
  socklen_t get_len = sizeof(get);
  EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, &get_len), 0) << strerror(errno);
  EXPECT_EQ(get_len, sizeof(get));
  EXPECT_EQ(get, want);

  if (IsIPv6()) {
    EXPECT_EQ(setsockopt(s.get(), t.level, t.option, &kSockOptOnChar, sizeof(kSockOptOnChar)), -1);
    EXPECT_EQ(errno, EINVAL) << strerror(errno);
  } else {
    EXPECT_EQ(setsockopt(s.get(), t.level, t.option, &kSockOptOnChar, sizeof(kSockOptOnChar)), 0)
        << strerror(errno);
  }

  EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, &get_len), 0) << strerror(errno);
  EXPECT_EQ(get_len, sizeof(get));
  EXPECT_EQ(get, kSockOptOn);

  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, MulticastTTLDefault) {
  if (IsTCP()) {
    GTEST_SKIP() << "Skip multicast tests on TCP socket";
  }

  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  int get = -1;
  socklen_t get_len = sizeof(get);
  SockOption t = GetMcastTTLOption();
  EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, &get_len), 0) << strerror(errno);
  EXPECT_EQ(get_len, sizeof(get));
  EXPECT_EQ(get, 1);

  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, SetUDPMulticastTTLMin) {
  if (IsTCP()) {
    GTEST_SKIP() << "Skip multicast tests on TCP socket";
  }

  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  constexpr int kMin = 0;
  SockOption t = GetMcastTTLOption();
  EXPECT_EQ(setsockopt(s.get(), t.level, t.option, &kMin, sizeof(kMin)), 0) << strerror(errno);

  int get = -1;
  socklen_t get_len = sizeof(get);
  EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, &get_len), 0) << strerror(errno);
  EXPECT_EQ(get_len, sizeof(get));
  EXPECT_EQ(get, kMin);

  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, SetUDPMulticastTTLMax) {
  if (IsTCP()) {
    GTEST_SKIP() << "Skip multicast tests on TCP socket";
  }

  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  constexpr int kMax = 255;
  SockOption t = GetMcastTTLOption();
  EXPECT_EQ(setsockopt(s.get(), t.level, t.option, &kMax, sizeof(kMax)), 0) << strerror(errno);

  int get = -1;
  socklen_t get_len = sizeof(get);
  EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, &get_len), 0) << strerror(errno);
  EXPECT_EQ(get_len, sizeof(get));
  EXPECT_EQ(get, kMax);

  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, SetUDPMulticastTTLNegativeOne) {
  if (IsTCP()) {
    GTEST_SKIP() << "Skip multicast tests on TCP socket";
  }

  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  constexpr int kArbitrary = 6;
  SockOption t = GetMcastTTLOption();
  EXPECT_EQ(setsockopt(s.get(), t.level, t.option, &kArbitrary, sizeof(kArbitrary)), 0)
      << strerror(errno);

  constexpr int kNegOne = -1;
  EXPECT_EQ(setsockopt(s.get(), t.level, t.option, &kNegOne, sizeof(kNegOne)), 0)
      << strerror(errno);

  int get = -1;
  socklen_t get_len = sizeof(get);
  EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, &get_len), 0) << strerror(errno);
  EXPECT_EQ(get_len, sizeof(get));
  EXPECT_EQ(get, 1);

  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, SetUDPMulticastTTLBelowMin) {
  if (IsTCP()) {
    GTEST_SKIP() << "Skip multicast tests on TCP socket";
  }

  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  constexpr int kBelowMin = -2;
  SockOption t = GetMcastTTLOption();
  EXPECT_EQ(setsockopt(s.get(), t.level, t.option, &kBelowMin, sizeof(kBelowMin)), -1);
  EXPECT_EQ(errno, EINVAL) << strerror(errno);

  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, SetUDPMulticastTTLAboveMax) {
  if (IsTCP()) {
    GTEST_SKIP() << "Skip multicast tests on TCP socket";
  }

  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  constexpr int kAboveMax = 256;
  SockOption t = GetMcastTTLOption();
  EXPECT_EQ(setsockopt(s.get(), t.level, t.option, &kAboveMax, sizeof(kAboveMax)), -1);
  EXPECT_EQ(errno, EINVAL) << strerror(errno);

  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, SetUDPMulticastTTLChar) {
  if (IsTCP()) {
    GTEST_SKIP() << "Skip multicast tests on TCP socket";
  }

  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  constexpr char kArbitrary = 6;
  SockOption t = GetMcastTTLOption();
  int want;
  if (IsIPv6()) {
    EXPECT_EQ(setsockopt(s.get(), t.level, t.option, &kArbitrary, sizeof(kArbitrary)), -1);
    EXPECT_EQ(errno, EINVAL) << strerror(errno);
    want = 1;
  } else {
    EXPECT_EQ(setsockopt(s.get(), t.level, t.option, &kArbitrary, sizeof(kArbitrary)), 0)
        << strerror(errno);
    want = kArbitrary;
  }

  int get = -1;
  socklen_t get_len = sizeof(get);
  EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, &get_len), 0) << strerror(errno);
  EXPECT_EQ(get_len, sizeof(get));
  EXPECT_EQ(get, want);

  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, SetUDPMulticastIfImrIfindex) {
  if (IsTCP()) {
    GTEST_SKIP() << "Skip multicast tests on TCP socket";
  }

  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  constexpr int kOne = 1;
  SockOption t = GetMcastIfOption();
  if (IsIPv6()) {
    EXPECT_EQ(setsockopt(s.get(), t.level, t.option, &kOne, sizeof(kOne)), 0) << strerror(errno);

    int param_out;
    socklen_t len = sizeof(param_out);
    ASSERT_EQ(getsockopt(s.get(), t.level, t.option, &param_out, &len), 0) << strerror(errno);
    ASSERT_EQ(len, sizeof(param_out));

    ASSERT_EQ(param_out, kOne);
  } else {
    ip_mreqn param_in = {
        .imr_ifindex = kOne,
    };
    ASSERT_EQ(setsockopt(s.get(), t.level, t.option, &param_in, sizeof(param_in)), 0)
        << strerror(errno);

    in_addr param_out;
    socklen_t len = sizeof(param_out);
    ASSERT_EQ(getsockopt(s.get(), t.level, t.option, &param_out, &len), 0) << strerror(errno);
    ASSERT_EQ(len, sizeof(param_out));

    ASSERT_EQ(param_out.s_addr, INADDR_ANY);
  }

  ASSERT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, SetUDPMulticastIfImrAddress) {
  if (IsTCP()) {
    GTEST_SKIP() << "Skip multicast tests on TCP socket";
  }
  if (IsIPv6()) {
    GTEST_SKIP() << "V6 sockets don't support setting IP_MULTICAST_IF by addr";
  }

  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  SockOption t = GetMcastIfOption();
  ip_mreqn param_in = {
      .imr_address.s_addr = htonl(INADDR_LOOPBACK),
  };
  ASSERT_EQ(setsockopt(s.get(), t.level, t.option, &param_in, sizeof(param_in)), 0)
      << strerror(errno);

  in_addr param_out;
  socklen_t len = sizeof(param_out);
  ASSERT_EQ(getsockopt(s.get(), t.level, t.option, &param_out, &len), 0) << strerror(errno);
  ASSERT_EQ(len, sizeof(param_out));

  ASSERT_EQ(param_out.s_addr, param_in.imr_address.s_addr);

  ASSERT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, ReceiveTOSDefault) {
  if (IsTCP()) {
    GTEST_SKIP() << "Skip receive TOS tests on TCP socket";
  }

  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  int get = -1;
  socklen_t get_len = sizeof(get);
  SockOption t = GetRecvTOSOption();
  EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, &get_len), 0) << strerror(errno);
  EXPECT_EQ(get_len, sizeof(get));
  EXPECT_EQ(get, kSockOptOff);

  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, SetReceiveTOS) {
  if (IsTCP()) {
    GTEST_SKIP() << "Skip receive TOS tests on TCP socket";
  }

  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  SockOption t = GetRecvTOSOption();
  ASSERT_EQ(setsockopt(s.get(), t.level, t.option, &kSockOptOn, sizeof(kSockOptOn)), 0)
      << strerror(errno);

  int get = -1;
  socklen_t get_len = sizeof(get);
  EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, &get_len), 0) << strerror(errno);
  EXPECT_EQ(get_len, sizeof(get));
  EXPECT_EQ(get, kSockOptOn);

  ASSERT_EQ(setsockopt(s.get(), t.level, t.option, &kSockOptOff, sizeof(kSockOptOff)), 0)
      << strerror(errno);

  EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, &get_len), 0) << strerror(errno);
  EXPECT_EQ(get_len, sizeof(get));
  EXPECT_EQ(get, kSockOptOff);

  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

// Tests that a two byte RECVTOS/RECVTCLASS optval is acceptable.
TEST_P(SocketOptsTest, SetReceiveTOSShort) {
  if (IsTCP()) {
    GTEST_SKIP() << "Skip receive TOS tests on TCP socket";
  }

  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  constexpr char kSockOptOn2Byte[] = {kSockOptOn, 0};
  constexpr char kSockOptOff2Byte[] = {kSockOptOff, 0};

  SockOption t = GetRecvTOSOption();
  if (IsIPv6()) {
    ASSERT_EQ(setsockopt(s.get(), t.level, t.option, &kSockOptOn2Byte, sizeof(kSockOptOn2Byte)), -1)
        << strerror(errno);
    EXPECT_EQ(errno, EINVAL) << strerror(errno);
  } else {
    ASSERT_EQ(setsockopt(s.get(), t.level, t.option, &kSockOptOn2Byte, sizeof(kSockOptOn2Byte)), 0)
        << strerror(errno);
  }

  int get = -1;
  socklen_t get_len = sizeof(get);
  EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, &get_len), 0) << strerror(errno);
  EXPECT_EQ(get_len, sizeof(get));
  if (IsIPv6()) {
    EXPECT_EQ(get, kSockOptOff);
  } else {
    EXPECT_EQ(get, kSockOptOn);
  }

  if (IsIPv6()) {
    ASSERT_EQ(setsockopt(s.get(), t.level, t.option, &kSockOptOff2Byte, sizeof(kSockOptOff2Byte)),
              -1)
        << strerror(errno);
    EXPECT_EQ(errno, EINVAL) << strerror(errno);
  } else {
    ASSERT_EQ(setsockopt(s.get(), t.level, t.option, &kSockOptOff2Byte, sizeof(kSockOptOff2Byte)),
              0)
        << strerror(errno);
  }

  EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, &get_len), 0) << strerror(errno);
  EXPECT_EQ(get_len, sizeof(get));
  EXPECT_EQ(get, kSockOptOff);

  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

// Tests that a one byte sized optval is acceptable for RECVTOS and not for
// RECVTCLASS.
TEST_P(SocketOptsTest, SetReceiveTOSChar) {
  if (IsTCP()) {
    GTEST_SKIP() << "Skip receive TOS tests on TCP socket";
  }

  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  constexpr char kSockOptOnChar = kSockOptOn;
  constexpr char kSockOptOffChar = kSockOptOff;

  SockOption t = GetRecvTOSOption();
  if (IsIPv6()) {
    ASSERT_EQ(setsockopt(s.get(), t.level, t.option, &kSockOptOnChar, sizeof(kSockOptOnChar)), -1)
        << strerror(errno);
    EXPECT_EQ(errno, EINVAL) << strerror(errno);
  } else {
    ASSERT_EQ(setsockopt(s.get(), t.level, t.option, &kSockOptOnChar, sizeof(kSockOptOnChar)), 0)
        << strerror(errno);
  }

  int get = -1;
  socklen_t get_len = sizeof(get);
  EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, &get_len), 0) << strerror(errno);
  EXPECT_EQ(get_len, sizeof(get));
  if (IsIPv6()) {
    EXPECT_EQ(get, kSockOptOff);
  } else {
    EXPECT_EQ(get, kSockOptOn);
  }

  if (IsIPv6()) {
    ASSERT_EQ(setsockopt(s.get(), t.level, t.option, &kSockOptOffChar, sizeof(kSockOptOffChar)), -1)
        << strerror(errno);
    EXPECT_EQ(errno, EINVAL) << strerror(errno);
  } else {
    ASSERT_EQ(setsockopt(s.get(), t.level, t.option, &kSockOptOffChar, sizeof(kSockOptOffChar)), 0)
        << strerror(errno);
  }

  EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, &get_len), 0) << strerror(errno);
  EXPECT_EQ(get_len, sizeof(get));
  EXPECT_EQ(get, kSockOptOff);

  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, NoChecksumDefault) {
  if (IsTCP()) {
    GTEST_SKIP() << "Skip NoChecksum tests on TCP socket";
  }

  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  int get = -1;
  socklen_t get_len = sizeof(get);
  SockOption t = GetNoChecksum();
  EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, &get_len), 0) << strerror(errno);
  EXPECT_EQ(get_len, sizeof(get));
  EXPECT_EQ(get, kSockOptOff);

  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST_P(SocketOptsTest, SetNoChecksum) {
  if (IsTCP()) {
    GTEST_SKIP() << "Skip NoChecksum tests on TCP socket";
  }

  fbl::unique_fd s;
  ASSERT_TRUE(s = NewSocket()) << strerror(errno);

  SockOption t = GetNoChecksum();
  ASSERT_EQ(setsockopt(s.get(), t.level, t.option, &kSockOptOn, sizeof(kSockOptOn)), 0)
      << strerror(errno);

  int get = -1;
  socklen_t get_len = sizeof(get);
  EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, &get_len), 0) << strerror(errno);
  EXPECT_EQ(get_len, sizeof(get));
  EXPECT_EQ(get, kSockOptOn);

  ASSERT_EQ(setsockopt(s.get(), t.level, t.option, &kSockOptOff, sizeof(kSockOptOff)), 0)
      << strerror(errno);

  EXPECT_EQ(getsockopt(s.get(), t.level, t.option, &get, &get_len), 0) << strerror(errno);
  EXPECT_EQ(get_len, sizeof(get));
  EXPECT_EQ(get, kSockOptOff);

  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

INSTANTIATE_TEST_SUITE_P(LocalhostTest, SocketOptsTest,
                         ::testing::Combine(::testing::Values(AF_INET, AF_INET6),
                                            ::testing::Values(SOCK_DGRAM, SOCK_STREAM)),
                         socketKindToString);

using typeMulticast = std::tuple<int, bool>;

std::string typeMulticastToString(const ::testing::TestParamInfo<typeMulticast>& info) {
  auto const& [type, multicast] = info.param;
  std::string addr;
  if (multicast) {
    addr = "Multicast";
  } else {
    addr = "Loopback";
  }
  return socketTypeToString(type) + addr;
}

class ReuseTest : public ::testing::TestWithParam<typeMulticast> {};

TEST_P(ReuseTest, AllowsAddressReuse) {
  const int on = true;
  auto const& [type, multicast] = GetParam();

#if defined(__Fuchsia__)
  if (multicast && type == SOCK_STREAM) {
    GTEST_SKIP() << "Cannot bind a TCP socket to a multicast address on Fuchsia";
  }
#endif

  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
  };
  if (multicast) {
    int n = inet_pton(addr.sin_family, "224.0.2.1", &addr.sin_addr);
    ASSERT_GE(n, 0) << strerror(errno);
    ASSERT_EQ(n, 1);
  }

  fbl::unique_fd s1;
  ASSERT_TRUE(s1 = fbl::unique_fd(socket(AF_INET, type, 0))) << strerror(errno);

// TODO(gvisor.dev/issue/3839): Remove this.
#if defined(__Fuchsia__)
  // Must outlive the block below.
  fbl::unique_fd s;
  if (type != SOCK_DGRAM && multicast) {
    ASSERT_EQ(bind(s1.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)), -1);
    ASSERT_EQ(errno, EADDRNOTAVAIL) << strerror(errno);
    ASSERT_TRUE(s = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP))) << strerror(errno);
    ip_mreqn param = {
        .imr_multiaddr = addr.sin_addr,
        .imr_address.s_addr = htonl(INADDR_ANY),
        .imr_ifindex = 1,
    };
    ASSERT_EQ(setsockopt(s.get(), SOL_IP, IP_ADD_MEMBERSHIP, &param, sizeof(param)), 0)
        << strerror(errno);
  }
#endif

  ASSERT_EQ(setsockopt(s1.get(), SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on)), 0) << strerror(errno);
  ASSERT_EQ(bind(s1.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  socklen_t addrlen = sizeof(addr);
  ASSERT_EQ(getsockname(s1.get(), reinterpret_cast<struct sockaddr*>(&addr), &addrlen), 0)
      << strerror(errno);
  ASSERT_EQ(addrlen, sizeof(addr));

  fbl::unique_fd s2;
  ASSERT_TRUE(s2 = fbl::unique_fd(socket(AF_INET, type, 0))) << strerror(errno);
  ASSERT_EQ(setsockopt(s2.get(), SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on)), 0) << strerror(errno);
  ASSERT_EQ(bind(s2.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);
}

INSTANTIATE_TEST_SUITE_P(LocalhostTest, ReuseTest,
                         ::testing::Combine(::testing::Values(SOCK_DGRAM, SOCK_STREAM),
                                            ::testing::Values(false, true)),
                         typeMulticastToString);

TEST(LocalhostTest, Accept) {
  fbl::unique_fd serverfd;
  ASSERT_TRUE(serverfd = fbl::unique_fd(socket(AF_INET6, SOCK_STREAM, 0))) << strerror(errno);

  struct sockaddr_in6 serveraddr = {
      .sin6_family = AF_INET6,
      .sin6_addr = IN6ADDR_LOOPBACK_INIT,
  };
  socklen_t serveraddrlen = sizeof(serveraddr);
  ASSERT_EQ(bind(serverfd.get(), (sockaddr*)&serveraddr, serveraddrlen), 0) << strerror(errno);
  ASSERT_EQ(getsockname(serverfd.get(), (sockaddr*)&serveraddr, &serveraddrlen), 0)
      << strerror(errno);
  ASSERT_EQ(serveraddrlen, sizeof(serveraddr));
  ASSERT_EQ(listen(serverfd.get(), 1), 0) << strerror(errno);

  fbl::unique_fd clientfd;
  ASSERT_TRUE(clientfd = fbl::unique_fd(socket(AF_INET6, SOCK_STREAM, 0))) << strerror(errno);
  ASSERT_EQ(connect(clientfd.get(), (sockaddr*)&serveraddr, serveraddrlen), 0) << strerror(errno);

  struct sockaddr_in connaddr;
  socklen_t connaddrlen = sizeof(connaddr);
  fbl::unique_fd connfd;
  ASSERT_TRUE(connfd = fbl::unique_fd(accept(serverfd.get(), (sockaddr*)&connaddr, &connaddrlen)))
      << strerror(errno);
  ASSERT_GT(connaddrlen, sizeof(connaddr));
}

TEST(LocalhostTest, AcceptAfterReset) {
  fbl::unique_fd server;
  ASSERT_TRUE(server = fbl::unique_fd(socket(AF_INET6, SOCK_STREAM, 0))) << strerror(errno);

  struct sockaddr_in6 addr = {
      .sin6_family = AF_INET6,
      .sin6_addr = IN6ADDR_LOOPBACK_INIT,
  };
  socklen_t addrlen = sizeof(addr);
  ASSERT_EQ(bind(server.get(), (const sockaddr*)&addr, addrlen), 0) << strerror(errno);
  ASSERT_EQ(getsockname(server.get(), (sockaddr*)&addr, &addrlen), 0) << strerror(errno);
  ASSERT_EQ(addrlen, sizeof(addr));
  ASSERT_EQ(listen(server.get(), 1), 0) << strerror(errno);

  {
    fbl::unique_fd client;
    ASSERT_TRUE(client = fbl::unique_fd(socket(AF_INET6, SOCK_STREAM, 0))) << strerror(errno);
    ASSERT_EQ(connect(client.get(), (const sockaddr*)&addr, addrlen), 0) << strerror(errno);
    struct linger opt = {
        .l_onoff = 1,
        .l_linger = 0,
    };
    ASSERT_EQ(setsockopt(client.get(), SOL_SOCKET, SO_LINGER, &opt, sizeof(opt)), 0)
        << strerror(errno);
    ASSERT_EQ(close(client.release()), 0) << strerror(errno);
  }

  memset(&addr, 0, sizeof(addr));

  fbl::unique_fd conn;
  ASSERT_TRUE(conn = fbl::unique_fd(accept(server.get(), (sockaddr*)&addr, &addrlen)))
      << strerror(errno);
  ASSERT_EQ(addrlen, sizeof(addr));
  ASSERT_EQ(addr.sin6_family, AF_INET6);
  char buf[INET6_ADDRSTRLEN];
  ASSERT_TRUE(IN6_IS_ADDR_LOOPBACK(&addr.sin6_addr))
      << inet_ntop(addr.sin6_family, &addr.sin6_addr, buf, sizeof(buf));
  ASSERT_NE(addr.sin6_port, 0);

  // Wait for the connection to close to avoid flakes when this code is reached before the RST
  // arrives at |conn|.
  {
    struct pollfd pfd = {
        .fd = conn.get(),
        .events = POLLIN,
    };

    int n = poll(&pfd, 1, kTimeout);
    ASSERT_GE(n, 0) << strerror(errno);
    ASSERT_EQ(n, 1);
    EXPECT_EQ(pfd.revents, POLLIN | POLLERR | POLLHUP);
  }

  int err;
  socklen_t optlen = sizeof(err);
  ASSERT_EQ(getsockopt(conn.get(), SOL_SOCKET, SO_ERROR, &err, &optlen), 0) << strerror(errno);
  ASSERT_EQ(err, ECONNRESET) << strerror(errno);
  ASSERT_EQ(optlen, sizeof(err));
}

TEST(LocalhostTest, ConnectAFMismatchINET) {
  fbl::unique_fd s;
  ASSERT_TRUE(s = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP))) << strerror(errno);

  struct sockaddr_in6 addr = {
      .sin6_family = AF_INET6,
      .sin6_port = htons(1337),
      .sin6_addr = IN6ADDR_LOOPBACK_INIT,
  };
  EXPECT_EQ(connect(s.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)), -1);
  EXPECT_EQ(errno, EAFNOSUPPORT) << strerror(errno);
  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST(LocalhostTest, ConnectAFMismatchINET6) {
  fbl::unique_fd s;
  ASSERT_TRUE(s = fbl::unique_fd(socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP))) << strerror(errno);

  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port = htons(1337),
      .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
  };
  EXPECT_EQ(connect(s.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);
  EXPECT_EQ(close(s.release()), 0) << strerror(errno);
}

TEST(NetStreamTest, ConnectTwice) {
  fbl::unique_fd client, listener;
  ASSERT_TRUE(client = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);
  ASSERT_TRUE(listener = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);

  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
  };

  ASSERT_EQ(bind(listener.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  socklen_t addrlen = sizeof(addr);
  ASSERT_EQ(getsockname(listener.get(), reinterpret_cast<struct sockaddr*>(&addr), &addrlen), 0)
      << strerror(errno);
  ASSERT_EQ(addrlen, sizeof(addr));

  ASSERT_EQ(connect(client.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)),
            -1);
  ASSERT_EQ(errno, ECONNREFUSED) << strerror(errno);

  ASSERT_EQ(listen(listener.get(), 1), 0) << strerror(errno);

  // TODO(fxbug.dev/61594): decide if we want to match Linux's behaviour.
  ASSERT_EQ(connect(client.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)),
#if defined(__linux__)
            0)
      << strerror(errno);
#else
            -1);
  ASSERT_EQ(errno, ECONNREFUSED) << strerror(errno);
#endif

  ASSERT_EQ(close(listener.release()), 0) << strerror(errno);
  ASSERT_EQ(close(client.release()), 0) << strerror(errno);
}

TEST(NetStreamTest, ConnectCloseRace) {
  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
  };

  // Use the ephemeral port allocated by the stack as destination address for connect.
  {
    fbl::unique_fd tmp;
    ASSERT_TRUE(tmp = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);

    ASSERT_EQ(bind(tmp.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)), 0)
        << strerror(errno);
    socklen_t addrlen = sizeof(addr);
    ASSERT_EQ(getsockname(tmp.get(), reinterpret_cast<struct sockaddr*>(&addr), &addrlen), 0)
        << strerror(errno);
    ASSERT_EQ(addrlen, sizeof(addr));
  }

  std::array<std::thread, 50> threads;
  for (auto& t : threads) {
    t = std::thread([&] {
      for (int i = 0; i < 5; i++) {
        fbl::unique_fd client;
        ASSERT_TRUE(client = fbl::unique_fd(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)))
            << strerror(errno);

        ASSERT_EQ(
            connect(client.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)),
            -1);
        ASSERT_TRUE(errno == EINPROGRESS
#if !defined(__Fuchsia__)
                    // Linux could return ECONNREFUSED if it processes the incoming RST before
                    // connect system
                    // call returns.
                    || errno == ECONNREFUSED
#endif
                    )
            << strerror(errno);
        ASSERT_EQ(close(client.release()), 0) << strerror(errno);
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }
}

void TestHangupDuringConnect(void (*hangup)(fbl::unique_fd*)) {
  fbl::unique_fd client, listener;
  ASSERT_TRUE(client = fbl::unique_fd(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)))
      << strerror(errno);
  ASSERT_TRUE(listener = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);

  struct sockaddr_in addr_in = {
      .sin_family = AF_INET,
      .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
  };
  auto addr = reinterpret_cast<struct sockaddr*>(&addr_in);
  socklen_t addr_len = sizeof(addr_in);

  ASSERT_EQ(bind(listener.get(), addr, addr_len), 0) << strerror(errno);
  {
    socklen_t addr_len_in = addr_len;
    ASSERT_EQ(getsockname(listener.get(), addr, &addr_len), 0) << strerror(errno);
    EXPECT_EQ(addr_len, addr_len_in);
  }
  ASSERT_EQ(listen(listener.get(), 1), 0) << strerror(errno);

  // Connect asynchronously and immediately hang up the listener.
  int ret = connect(client.get(), addr, addr_len);
#if !defined(__Fuchsia__)
  // Linux connect may succeed if the handshake completes before the system call returns.
  if (ret != 0)
#endif
  {
    ASSERT_EQ(ret, -1);
    ASSERT_EQ(errno, EINPROGRESS) << strerror(errno);
  }

  ASSERT_NO_FATAL_FAILURE(hangup(&listener));

  // Wait for the connection to close.
  {
    struct pollfd pfd = {
        .fd = client.get(),
        .events = POLLIN,
    };

    int n = poll(&pfd, 1, kTimeout);
    ASSERT_GE(n, 0) << strerror(errno);
    ASSERT_EQ(n, 1);
  }

  ASSERT_EQ(close(client.release()), 0) << strerror(errno);
}

TEST(NetStreamTest, CloseDuringConnect) {
  TestHangupDuringConnect([](fbl::unique_fd* listener) {
    ASSERT_EQ(close(listener->release()), 0) << strerror(errno);
  });
}

TEST(NetStreamTest, ShutdownDuringConnect) {
  TestHangupDuringConnect([](fbl::unique_fd* listener) {
    ASSERT_EQ(shutdown(listener->get(), SHUT_RD), 0) << strerror(errno);
  });
}

TEST(LocalhostTest, RaceLocalPeerClose) {
  fbl::unique_fd listener;
  ASSERT_TRUE(listener = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);
#if !defined(__Fuchsia__)
  // Make the listener non-blocking so that we can let accept system call return
  // below when there are no acceptable connections.
  int flags = fcntl(listener.get(), F_GETFL, 0);
  ASSERT_EQ(fcntl(listener.get(), F_SETFL, flags | O_NONBLOCK), 0) << strerror(errno);
#endif
  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
  };

  ASSERT_EQ(bind(listener.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  socklen_t addrlen = sizeof(addr);
  ASSERT_EQ(getsockname(listener.get(), reinterpret_cast<struct sockaddr*>(&addr), &addrlen), 0)
      << strerror(errno);
  ASSERT_EQ(addrlen, sizeof(addr));

  std::array<std::thread, 50> threads;
  ASSERT_EQ(listen(listener.get(), threads.size()), 0) << strerror(errno);

  // Run many iterations in parallel in order to increase load on Netstack and increase the
  // probability we'll hit the problem.
  for (auto& t : threads) {
    t =
        std::thread([&] {
          fbl::unique_fd peer;
          ASSERT_TRUE(peer = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);

          // Connect and immediately close a peer with linger. This causes the network-initiated
          // close that will race with the accepted connection close below. Linger is necessary
          // because we need a TCP RST to force a full teardown, tickling Netstack the right way to
          // cause a bad race.
          struct linger opt = {
              .l_onoff = 1,
              .l_linger = 0,
          };
          EXPECT_EQ(setsockopt(peer.get(), SOL_SOCKET, SO_LINGER, &opt, sizeof(opt)), 0)
              << strerror(errno);
          ASSERT_EQ(
              connect(peer.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)), 0)
              << strerror(errno);
          ASSERT_EQ(close(peer.release()), 0) << strerror(errno);

          // Accept the connection and close it, adding new racing signal (operating on `close`) to
          // Netstack.
          int local = accept(listener.get(), nullptr, nullptr);
          if (local < 0) {
#if !defined(__Fuchsia__)
            // We get EAGAIN when there are no pending acceptable connections. Though the peer
            // connect was a blocking call, it can return before the final ACK is sent out causing
            // the RST from linger0+close to be sent out before the final ACK. This would result in
            // that connection to be not completed and hence not added to the acceptable queue.
            //
            // The above race does not currently exist on Fuchsia where the final ACK would always
            // be sent out over lo before connect() call returns.
            ASSERT_EQ(errno, EAGAIN)
#else
            FAIL()
#endif
                << strerror(errno);
          } else {
            ASSERT_EQ(close(local), 0) << strerror(errno);
          }
        });
  }

  for (auto& t : threads) {
    t.join();
  }

  ASSERT_EQ(close(listener.release()), 0) << strerror(errno);
}

TEST(LocalhostTest, GetAddrInfo) {
  struct addrinfo hints = {
      .ai_family = AF_UNSPEC,
      .ai_socktype = SOCK_STREAM,
  };

  struct addrinfo* result;
  ASSERT_EQ(getaddrinfo("localhost", nullptr, &hints, &result), 0) << strerror(errno);

  int i = 0;
  for (struct addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
    i++;

    EXPECT_EQ(ai->ai_socktype, hints.ai_socktype);
    const struct sockaddr* sa = ai->ai_addr;

    switch (ai->ai_family) {
      case AF_INET: {
        EXPECT_EQ(ai->ai_addrlen, (socklen_t)16);

        unsigned char expected_addr[4] = {0x7f, 0x00, 0x00, 0x01};

        auto sin = reinterpret_cast<const struct sockaddr_in*>(sa);
        EXPECT_EQ(sin->sin_addr.s_addr, *reinterpret_cast<uint32_t*>(expected_addr));

        break;
      }
      case AF_INET6: {
        EXPECT_EQ(ai->ai_addrlen, (socklen_t)28);

        const char expected_addr[16] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};

        auto sin6 = reinterpret_cast<const struct sockaddr_in6*>(sa);
        EXPECT_STREQ((const char*)sin6->sin6_addr.s6_addr, expected_addr);

        break;
      }
    }
  }
  EXPECT_EQ(i, 2);
  freeaddrinfo(result);
}

TEST(LocalhostTest, GetSockName) {
  fbl::unique_fd sockfd;
  ASSERT_TRUE(sockfd = fbl::unique_fd(socket(AF_INET6, SOCK_STREAM, 0))) << strerror(errno);

  struct sockaddr sa;
  socklen_t len = sizeof(sa);
  ASSERT_EQ(getsockname(sockfd.get(), &sa, &len), 0) << strerror(errno);
  ASSERT_GT(len, sizeof(sa));
  ASSERT_EQ(sa.sa_family, AF_INET6);
}

class NetStreamSocketsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    fbl::unique_fd listener;
    ASSERT_TRUE(listener = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    ASSERT_EQ(bind(listener.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)),
              0)
        << strerror(errno);

    socklen_t addrlen = sizeof(addr);
    ASSERT_EQ(getsockname(listener.get(), reinterpret_cast<struct sockaddr*>(&addr), &addrlen), 0)
        << strerror(errno);
    ASSERT_EQ(addrlen, sizeof(addr));

    ASSERT_EQ(listen(listener.get(), 1), 0) << strerror(errno);

    ASSERT_TRUE(client_ = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);
    ASSERT_EQ(connect(client_.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)),
              0)
        << strerror(errno);

    ASSERT_TRUE(server_ = fbl::unique_fd(accept(listener.get(), nullptr, nullptr)))
        << strerror(errno);
    ASSERT_EQ(close(listener.release()), 0) << strerror(errno);
  }

  void TearDown() override {
    if (client_.is_valid())
      EXPECT_EQ(close(client_.release()), 0) << strerror(errno);
    if (server_.is_valid())
      EXPECT_EQ(close(server_.release()), 0) << strerror(errno);
  }

  fbl::unique_fd& client() { return client_; }

  fbl::unique_fd& server() { return server_; }

 private:
  fbl::unique_fd client_;
  fbl::unique_fd server_;
};

TEST_F(NetStreamSocketsTest, PartialWriteStress) {
  // Generate a payload large enough to fill the client->server buffers.
  std::string big_string;
  {
    uint32_t sndbuf_opt;
    socklen_t sndbuf_optlen = sizeof(sndbuf_opt);
    EXPECT_EQ(getsockopt(client().get(), SOL_SOCKET, SO_SNDBUF, &sndbuf_opt, &sndbuf_optlen), 0)
        << strerror(errno);
    EXPECT_EQ(sndbuf_optlen, sizeof(sndbuf_opt));

    uint32_t rcvbuf_opt;
    socklen_t rcvbuf_optlen = sizeof(rcvbuf_opt);
    EXPECT_EQ(getsockopt(server().get(), SOL_SOCKET, SO_RCVBUF, &rcvbuf_opt, &rcvbuf_optlen), 0)
        << strerror(errno);
    EXPECT_EQ(rcvbuf_optlen, sizeof(rcvbuf_opt));

    // SO_{SND,RCV}BUF lie and report double the real value.
    size_t size = (sndbuf_opt + rcvbuf_opt) >> 1;
#if defined(__Fuchsia__)
    // TODO(https://fxbug.dev/60337): We can avoid this additional space once zircon sockets are not
    // artificially increasing the buffer sizes.
    size += 2 * (1 << 18);
#endif

    big_string.reserve(size);
    while (big_string.size() < size) {
      big_string += "Though this upload be but little, it is fierce.";
    }
  }

  {
    // Write in small chunks to allow the outbound TCP to coalesce adjacent writes into a single
    // segment; that is the circumstance in which the data corruption bug that prompted writing this
    // test was observed.
    //
    // Loopback MTU is 64KiB, so use a value smaller than that.
    constexpr size_t write_size = 1 << 10;  // 1 KiB.

    auto s = big_string;
    while (!s.empty()) {
      ssize_t w = write(client().get(), s.data(), std::min(s.size(), write_size));
      ASSERT_GE(w, 0) << strerror(errno);
      s = s.substr(w);
    }
    ASSERT_EQ(shutdown(client().get(), SHUT_WR), 0) << strerror(errno);
  }

  // Read the data and validate it against our payload.
  {
    // Read in small chunks to increase the probability of partial writes from the network endpoint
    // into the zircon socket; that is the circumstance in which the data corruption bug that
    // prompted writing this test was observed.
    //
    // zircon sockets are 256KiB deep, so use a value smaller than that.
    //
    // Note that in spite of the trickery we employ in this test to create the conditions necessary
    // to trigger the data corruption bug, it is still not guaranteed to happen. This is because a
    // race is still necessary to trigger the bug; as netstack is copying bytes from the network to
    // the zircon socket, the application on the other side of this socket (this test) must read
    // between a partial write and the next write.
    constexpr size_t read_size = 1 << 13;  // 8 KiB.

    std::string buf;
    buf.resize(read_size);
    for (size_t i = 0; i < big_string.size();) {
      ssize_t r = read(server().get(), buf.data(), buf.size());
      ASSERT_GT(r, 0) << strerror(errno);

      auto actual = buf.substr(0, r);
      auto expected = big_string.substr(i, r);

      constexpr size_t kChunkSize = 100;
      for (size_t j = 0; j < actual.size(); j += kChunkSize) {
        auto actual_chunk = actual.substr(j, kChunkSize);
        auto expected_chunk = expected.substr(j, actual_chunk.size());
        ASSERT_EQ(actual_chunk, expected_chunk) << "offset " << i + j;
      }
      i += r;
    }
  }
}

TEST_F(NetStreamSocketsTest, PeerClosedPOLLOUT) {
  fill_stream_send_buf(server().get(), client().get());

  EXPECT_EQ(close(client().release()), 0) << strerror(errno);

  struct pollfd pfd = {
      .fd = server().get(),
      .events = POLLOUT,
  };
  int n = poll(&pfd, 1, kTimeout);
  EXPECT_GE(n, 0) << strerror(errno);
  EXPECT_EQ(n, 1);
  EXPECT_EQ(pfd.revents, POLLOUT | POLLERR | POLLHUP);
}

TEST_F(NetStreamSocketsTest, BlockingAcceptWrite) {
  const char msg[] = "hello";
  ASSERT_EQ(write(server().get(), msg, sizeof(msg)), (ssize_t)sizeof(msg)) << strerror(errno);
  ASSERT_EQ(close(server().release()), 0) << strerror(errno);

  char buf[sizeof(msg) + 1] = {};
  ASSERT_EQ(read(client().get(), buf, sizeof(buf)), (ssize_t)sizeof(msg)) << strerror(errno);
  ASSERT_STREQ(buf, msg);
}

class TimeoutSockoptsTest : public ::testing::TestWithParam<int /* optname */> {};

TEST_P(TimeoutSockoptsTest, TimeoutSockopts) {
  int optname = GetParam();
  ASSERT_TRUE(optname == SO_RCVTIMEO || optname == SO_SNDTIMEO);

  fbl::unique_fd socket_fd;
  ASSERT_TRUE(socket_fd = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);

  // Set the timeout.
  const struct timeval expected_tv = {
      .tv_sec = 39,
      // NB: for some reason, Linux's resolution is limited to 4ms.
      .tv_usec = 504000,
  };
  EXPECT_EQ(setsockopt(socket_fd.get(), SOL_SOCKET, optname, &expected_tv, sizeof(expected_tv)), 0)
      << strerror(errno);

  // Reading it back should work.
  struct timeval actual_tv;
  socklen_t optlen = sizeof(actual_tv);
  EXPECT_EQ(getsockopt(socket_fd.get(), SOL_SOCKET, optname, &actual_tv, &optlen), 0)
      << strerror(errno);
  EXPECT_EQ(optlen, sizeof(actual_tv));
  EXPECT_EQ(actual_tv.tv_sec, expected_tv.tv_sec);
  EXPECT_EQ(actual_tv.tv_usec, expected_tv.tv_usec);

  // Reading it back with too much space should work and set optlen.
  char actual_tv2_buffer[sizeof(struct timeval) * 2];
  memset(&actual_tv2_buffer, 44, sizeof(actual_tv2_buffer));
  optlen = sizeof(actual_tv2_buffer);
  auto actual_tv2 = reinterpret_cast<struct timeval*>(actual_tv2_buffer);
  EXPECT_EQ(getsockopt(socket_fd.get(), SOL_SOCKET, optname, actual_tv2, &optlen), 0)
      << strerror(errno);
  EXPECT_EQ(optlen, sizeof(struct timeval));
  EXPECT_EQ(actual_tv2->tv_sec, expected_tv.tv_sec);
  EXPECT_EQ(actual_tv2->tv_usec, expected_tv.tv_usec);
  for (auto i = sizeof(struct timeval); i < sizeof(struct timeval) * 2; i++) {
    EXPECT_EQ(actual_tv2_buffer[i], 44);
  }

  // Reading it back without enough space should fail gracefully.
  memset(&actual_tv, 0, sizeof(actual_tv));
  optlen = sizeof(actual_tv) - 7;  // Not enough space to store the result.
  // TODO(eyalsoha): Decide if we want to match Linux's behaviour.  It writes to
  // only the first optlen bytes of the timeval.
  EXPECT_EQ(getsockopt(socket_fd.get(), SOL_SOCKET, optname, &actual_tv, &optlen),
#if defined(__Fuchsia__)
            -1);
  EXPECT_EQ(errno, EINVAL) << strerror(errno);
#else
            0)
      << strerror(errno);
  EXPECT_EQ(optlen, sizeof(actual_tv) - 7);
  struct timeval linux_expected_tv = expected_tv;
  memset(reinterpret_cast<char*>(&linux_expected_tv) + optlen, 0,
         sizeof(linux_expected_tv) - optlen);
  EXPECT_EQ(memcmp(&actual_tv, &linux_expected_tv, sizeof(actual_tv)), 0);
#endif

  // Setting it without enough space should fail gracefully.
  optlen = sizeof(expected_tv) - 1;  // Not big enough.
  EXPECT_EQ(setsockopt(socket_fd.get(), SOL_SOCKET, optname, &expected_tv, optlen), -1);
  EXPECT_EQ(errno, EINVAL) << strerror(errno);

  // Setting it with too much space should work okay.
  const struct timeval expected_tv2 = {
      .tv_sec = 42,
      .tv_usec = 0,
  };
  optlen = sizeof(expected_tv2) + 1;  // Too big.
  EXPECT_EQ(setsockopt(socket_fd.get(), SOL_SOCKET, optname, &expected_tv2, optlen), 0)
      << strerror(errno);
  EXPECT_EQ(getsockopt(socket_fd.get(), SOL_SOCKET, optname, &actual_tv, &optlen), 0)
      << strerror(errno);
  EXPECT_EQ(optlen, sizeof(expected_tv2));
  EXPECT_EQ(actual_tv.tv_sec, expected_tv2.tv_sec);
  EXPECT_EQ(actual_tv.tv_usec, expected_tv2.tv_usec);

  // Disabling rcvtimeo by setting it to zero should work.
  const struct timeval zero_tv = {
      .tv_sec = 0,
      .tv_usec = 0,
  };
  optlen = sizeof(zero_tv);
  EXPECT_EQ(setsockopt(socket_fd.get(), SOL_SOCKET, optname, &zero_tv, optlen), 0)
      << strerror(errno);

  // Reading back the disabled timeout should work.
  memset(&actual_tv, 55, sizeof(actual_tv));
  optlen = sizeof(actual_tv);
  EXPECT_EQ(getsockopt(socket_fd.get(), SOL_SOCKET, optname, &actual_tv, &optlen), 0)
      << strerror(errno);
  EXPECT_EQ(optlen, sizeof(actual_tv));
  EXPECT_EQ(actual_tv.tv_sec, zero_tv.tv_sec);
  EXPECT_EQ(actual_tv.tv_usec, zero_tv.tv_usec);
}

INSTANTIATE_TEST_SUITE_P(NetStreamTest, TimeoutSockoptsTest,
                         ::testing::Values(SO_RCVTIMEO, SO_SNDTIMEO));

const int32_t kConnections = 100;

TEST(NetStreamTest, BlockingAcceptWriteMultiple) {
  fbl::unique_fd acptfd;
  ASSERT_TRUE(acptfd = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);

  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr.s_addr = htonl(INADDR_ANY),
  };
  ASSERT_EQ(bind(acptfd.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  socklen_t addrlen = sizeof(addr);
  ASSERT_EQ(getsockname(acptfd.get(), reinterpret_cast<struct sockaddr*>(&addr), &addrlen), 0)
      << strerror(errno);

  ASSERT_EQ(listen(acptfd.get(), kConnections), 0) << strerror(errno);

  fbl::unique_fd clientfds[kConnections];
  for (auto& clientfd : clientfds) {
    ASSERT_TRUE(clientfd = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);
    ASSERT_EQ(
        connect(clientfd.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)), 0)
        << strerror(errno);
  }

  const char msg[] = "hello";
  for (int i = 0; i < kConnections; i++) {
    fbl::unique_fd connfd;
    ASSERT_TRUE(connfd = fbl::unique_fd(accept(acptfd.get(), nullptr, nullptr))) << strerror(errno);

    ASSERT_EQ(write(connfd.get(), msg, sizeof(msg)), (ssize_t)sizeof(msg)) << strerror(errno);
    ASSERT_EQ(close(connfd.release()), 0) << strerror(errno);
  }

  for (auto& clientfd : clientfds) {
    char buf[sizeof(msg) + 1] = {};
    ASSERT_EQ(read(clientfd.get(), buf, sizeof(buf)), (ssize_t)sizeof(msg)) << strerror(errno);
    ASSERT_STREQ(buf, msg);
    ASSERT_EQ(close(clientfd.release()), 0) << strerror(errno);
  }

  EXPECT_EQ(close(acptfd.release()), 0) << strerror(errno);
}

TEST_F(NetStreamSocketsTest, BlockingAcceptDupWrite) {
  fbl::unique_fd dupfd;
  ASSERT_TRUE(dupfd = fbl::unique_fd(dup(server().get()))) << strerror(errno);
  ASSERT_EQ(close(server().release()), 0) << strerror(errno);

  const char msg[] = "hello";
  ASSERT_EQ(write(dupfd.get(), msg, sizeof(msg)), (ssize_t)sizeof(msg)) << strerror(errno);
  ASSERT_EQ(close(dupfd.release()), 0) << strerror(errno);

  char buf[sizeof(msg) + 1] = {};
  ASSERT_EQ(read(client().get(), buf, sizeof(buf)), (ssize_t)sizeof(msg)) << strerror(errno);
  ASSERT_STREQ(buf, msg);
}

TEST(NetStreamTest, NonBlockingAcceptWrite) {
  fbl::unique_fd acptfd;
  ASSERT_TRUE(acptfd = fbl::unique_fd(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)))
      << strerror(errno);

  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr.s_addr = htonl(INADDR_ANY),
  };
  ASSERT_EQ(bind(acptfd.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  socklen_t addrlen = sizeof(addr);
  ASSERT_EQ(getsockname(acptfd.get(), reinterpret_cast<struct sockaddr*>(&addr), &addrlen), 0)
      << strerror(errno);

  ASSERT_EQ(listen(acptfd.get(), 1), 0) << strerror(errno);

  fbl::unique_fd clientfd;
  ASSERT_TRUE(clientfd = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);
  ASSERT_EQ(connect(clientfd.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)),
            0)
      << strerror(errno);

  struct pollfd pfd = {
      .fd = acptfd.get(),
      .events = POLLIN,
  };
  int n = poll(&pfd, 1, kTimeout);
  ASSERT_GE(n, 0) << strerror(errno);
  ASSERT_EQ(n, 1);

  fbl::unique_fd connfd;
  ASSERT_TRUE(connfd = fbl::unique_fd(accept(acptfd.get(), nullptr, nullptr))) << strerror(errno);

  const char msg[] = "hello";
  ASSERT_EQ(write(connfd.get(), msg, sizeof(msg)), (ssize_t)sizeof(msg)) << strerror(errno);
  ASSERT_EQ(close(connfd.release()), 0) << strerror(errno);

  char buf[sizeof(msg) + 1] = {};
  ASSERT_EQ(read(clientfd.get(), buf, sizeof(buf)), (ssize_t)sizeof(msg)) << strerror(errno);
  ASSERT_STREQ(buf, msg);
  ASSERT_EQ(close(clientfd.release()), 0) << strerror(errno);
  EXPECT_EQ(close(acptfd.release()), 0) << strerror(errno);
}

TEST(NetStreamTest, NonBlockingAcceptDupWrite) {
  fbl::unique_fd acptfd;
  ASSERT_TRUE(acptfd = fbl::unique_fd(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)))
      << strerror(errno);

  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr.s_addr = htonl(INADDR_ANY),
  };
  ASSERT_EQ(bind(acptfd.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  socklen_t addrlen = sizeof(addr);
  ASSERT_EQ(getsockname(acptfd.get(), reinterpret_cast<struct sockaddr*>(&addr), &addrlen), 0)
      << strerror(errno);

  ASSERT_EQ(listen(acptfd.get(), 1), 0) << strerror(errno);

  fbl::unique_fd clientfd;
  ASSERT_TRUE(clientfd = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);
  ASSERT_EQ(connect(clientfd.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)),
            0)
      << strerror(errno);

  struct pollfd pfd = {
      .fd = acptfd.get(),
      .events = POLLIN,
  };
  int n = poll(&pfd, 1, kTimeout);
  ASSERT_GE(n, 0) << strerror(errno);
  ASSERT_EQ(n, 1);

  fbl::unique_fd connfd;
  ASSERT_TRUE(connfd = fbl::unique_fd(accept(acptfd.get(), nullptr, nullptr))) << strerror(errno);

  fbl::unique_fd dupfd;
  ASSERT_TRUE(dupfd = fbl::unique_fd(dup(connfd.get()))) << strerror(errno);
  ASSERT_EQ(close(connfd.release()), 0) << strerror(errno);

  const char msg[] = "hello";
  ASSERT_EQ(write(dupfd.get(), msg, sizeof(msg)), (ssize_t)sizeof(msg)) << strerror(errno);
  ASSERT_EQ(close(dupfd.release()), 0) << strerror(errno);

  char buf[sizeof(msg) + 1] = {};
  ASSERT_EQ(read(clientfd.get(), buf, sizeof(buf)), (ssize_t)sizeof(msg)) << strerror(errno);
  ASSERT_STREQ(buf, msg);
  ASSERT_EQ(close(clientfd.release()), 0) << strerror(errno);
  EXPECT_EQ(close(acptfd.release()), 0) << strerror(errno);
}

TEST(NetStreamTest, NonBlockingConnectWrite) {
  fbl::unique_fd acptfd;
  ASSERT_TRUE(acptfd = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);

  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr.s_addr = htonl(INADDR_ANY),
  };
  ASSERT_EQ(bind(acptfd.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  socklen_t addrlen = sizeof(addr);
  ASSERT_EQ(getsockname(acptfd.get(), reinterpret_cast<struct sockaddr*>(&addr), &addrlen), 0)
      << strerror(errno);

  ASSERT_EQ(listen(acptfd.get(), 1), 0) << strerror(errno);

  fbl::unique_fd connfd;
  ASSERT_TRUE(connfd = fbl::unique_fd(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)))
      << strerror(errno);

  int ret;
  EXPECT_EQ(
      ret = connect(connfd.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)),
      -1);
  if (ret == -1) {
    ASSERT_EQ(EINPROGRESS, errno) << strerror(errno);

    struct pollfd pfd = {
        .fd = connfd.get(),
        .events = POLLOUT,
    };
    int n = poll(&pfd, 1, kTimeout);
    ASSERT_GE(n, 0) << strerror(errno);
    ASSERT_EQ(n, 1);

    int err;
    socklen_t optlen = sizeof(err);
    ASSERT_EQ(getsockopt(connfd.get(), SOL_SOCKET, SO_ERROR, &err, &optlen), 0) << strerror(errno);
    ASSERT_EQ(err, 0);
    ASSERT_EQ(optlen, sizeof(err));
  }

  fbl::unique_fd clientfd;
  ASSERT_TRUE(clientfd = fbl::unique_fd(accept(acptfd.get(), nullptr, nullptr))) << strerror(errno);

  const char msg[] = "hello";
  ASSERT_EQ(write(connfd.get(), msg, sizeof(msg)), (ssize_t)sizeof(msg)) << strerror(errno);
  ASSERT_EQ(close(connfd.release()), 0) << strerror(errno);

  char buf[sizeof(msg) + 1] = {};
  ASSERT_EQ(read(clientfd.get(), buf, sizeof(buf)), (ssize_t)sizeof(msg)) << strerror(errno);
  ASSERT_STREQ(buf, msg);
  ASSERT_EQ(close(clientfd.release()), 0) << strerror(errno);
  EXPECT_EQ(close(acptfd.release()), 0) << strerror(errno);
}

TEST(NetStreamTest, NonBlockingConnectRead) {
  fbl::unique_fd acptfd;
  ASSERT_TRUE(acptfd = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);

  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr.s_addr = htonl(INADDR_ANY),
  };
  ASSERT_EQ(bind(acptfd.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  socklen_t addrlen = sizeof(addr);
  ASSERT_EQ(getsockname(acptfd.get(), reinterpret_cast<struct sockaddr*>(&addr), &addrlen), 0)
      << strerror(errno);

  ASSERT_EQ(listen(acptfd.get(), 1), 0) << strerror(errno);

  fbl::unique_fd connfd;
  ASSERT_TRUE(connfd = fbl::unique_fd(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)))
      << strerror(errno);

  int ret;
  EXPECT_EQ(
      ret = connect(connfd.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)),
      -1);
  if (ret == -1) {
    ASSERT_EQ(EINPROGRESS, errno) << strerror(errno);

    fbl::unique_fd clientfd;
    ASSERT_TRUE(clientfd = fbl::unique_fd(accept(acptfd.get(), nullptr, nullptr)))
        << strerror(errno);

    const char msg[] = "hello";
    ASSERT_EQ(write(clientfd.get(), msg, sizeof(msg)), (ssize_t)sizeof(msg)) << strerror(errno);
    ASSERT_EQ(close(clientfd.release()), 0) << strerror(errno);

    // Note: the success of connection can be detected with POLLOUT, but
    // we use POLLIN here to wait until some data is written by the peer.
    struct pollfd pfd = {
        .fd = connfd.get(),
        .events = POLLIN,
    };
    int n = poll(&pfd, 1, kTimeout);
    ASSERT_GE(n, 0) << strerror(errno);
    ASSERT_EQ(n, 1);

    int err;
    socklen_t optlen = sizeof(err);
    ASSERT_EQ(getsockopt(connfd.get(), SOL_SOCKET, SO_ERROR, &err, &optlen), 0) << strerror(errno);
    ASSERT_EQ(err, 0);
    ASSERT_EQ(optlen, sizeof(err));

    char buf[sizeof(msg) + 1] = {};
    ASSERT_EQ(read(connfd.get(), buf, sizeof(buf)), (ssize_t)sizeof(msg)) << strerror(errno);
    ASSERT_STREQ(buf, msg);
    ASSERT_EQ(close(connfd.release()), 0) << strerror(errno);
    EXPECT_EQ(close(acptfd.release()), 0) << strerror(errno);
  }
}

enum class AnyAddr {
  V4,
  V6,
  V4MAPPEDV6,
};

template <int type>
class SocketAnyAddr : public ::testing::TestWithParam<AnyAddr> {
 protected:
  void SetUp() override {
    ASSERT_TRUE(sock_ = fbl::unique_fd(socket(AddressFamily(), type, 0))) << strerror(errno);
  }

  void TearDown() override { ASSERT_EQ(close(sock_.release()), 0) << strerror(errno); }

  sa_family_t AddressFamily() const {
    switch (GetParam()) {
      case AnyAddr::V4:
        return AF_INET;
      case AnyAddr::V6:
      case AnyAddr::V4MAPPEDV6:
        return AF_INET6;
    }
  }

  struct sockaddr_storage AnyAddress() const {
    struct sockaddr_storage addr {
      .ss_family = AddressFamily(),
    };

    switch (GetParam()) {
      case AnyAddr::V4: {
        auto sin = reinterpret_cast<struct sockaddr_in*>(&addr);
        sin->sin_addr.s_addr = htonl(INADDR_ANY);
        return addr;
      }
      case AnyAddr::V6: {
        auto sin6 = reinterpret_cast<struct sockaddr_in6*>(&addr);
        sin6->sin6_addr = IN6ADDR_ANY_INIT;
        return addr;
      }
      case AnyAddr::V4MAPPEDV6: {
        auto sin6 = reinterpret_cast<struct sockaddr_in6*>(&addr);
        sin6->sin6_addr = IN6ADDR_ANY_INIT;
        sin6->sin6_addr.s6_addr[10] = 0xff;
        sin6->sin6_addr.s6_addr[11] = 0xff;
        return addr;
      }
    }
  }

  socklen_t AddrLen() const {
    if (AddressFamily() == AF_INET) {
      return sizeof(sockaddr_in);
    }
    return sizeof(sockaddr_in6);
  }
  fbl::unique_fd sock_;
};

using StreamSocketAnyAddr = SocketAnyAddr<SOCK_STREAM>;
using DatagramSocketAnyAddr = SocketAnyAddr<SOCK_DGRAM>;

TEST_P(StreamSocketAnyAddr, Connect) {
  struct sockaddr_storage any = AnyAddress();
  socklen_t addrlen = AddrLen();
  ASSERT_EQ(connect(sock_.get(), reinterpret_cast<const struct sockaddr*>(&any), addrlen), -1);
  ASSERT_EQ(errno, ECONNREFUSED) << strerror(errno);
}

TEST_P(DatagramSocketAnyAddr, Connect) {
  struct sockaddr_storage any = AnyAddress();
  socklen_t addrlen = AddrLen();
  EXPECT_EQ(connect(sock_.get(), reinterpret_cast<const struct sockaddr*>(&any), addrlen), 0)
      << strerror(errno);
}

INSTANTIATE_TEST_SUITE_P(NetStreamTest, StreamSocketAnyAddr,
                         ::testing::Values(AnyAddr::V4, AnyAddr::V6, AnyAddr::V4MAPPEDV6));
INSTANTIATE_TEST_SUITE_P(NetDatagramTest, DatagramSocketAnyAddr,
                         ::testing::Values(AnyAddr::V4, AnyAddr::V6, AnyAddr::V4MAPPEDV6));

class IOMethod {
 public:
  enum class Op {
    READ,
    READV,
    RECV,
    RECVFROM,
    RECVMSG,
    WRITE,
    WRITEV,
    SEND,
    SENDTO,
    SENDMSG,
  };

  explicit IOMethod(enum Op op) : op(op) {}
  enum Op Op() const { return op; }

  ssize_t executeIO(int fd, char* buf, size_t len) const {
    struct iovec iov[] = {{
        .iov_base = buf,
        .iov_len = len,
    }};
    struct msghdr msg = {
        .msg_iov = iov,
        .msg_iovlen = std::size(iov),
    };
    switch (op) {
      case Op::READ:
        return read(fd, buf, len);
      case Op::READV:
        return readv(fd, iov, std::size(iov));
      case Op::RECV:
        return recv(fd, buf, len, 0);
      case Op::RECVFROM:
        return recvfrom(fd, buf, len, 0, nullptr, nullptr);
      case Op::RECVMSG:
        return recvmsg(fd, &msg, 0);
      case Op::WRITE:
        return write(fd, buf, len);
      case Op::WRITEV:
        return writev(fd, iov, std::size(iov));
      case Op::SEND:
        return send(fd, buf, len, 0);
      case Op::SENDTO:
        return sendto(fd, buf, len, 0, nullptr, 0);
      case Op::SENDMSG:
        return sendmsg(fd, &msg, 0);
    }
  }

  bool isWrite() const {
    switch (op) {
      case Op::READ:
      case Op::READV:
      case Op::RECV:
      case Op::RECVFROM:
      case Op::RECVMSG:
        return false;
      case Op::WRITE:
      case Op::WRITEV:
      case Op::SEND:
      case Op::SENDTO:
      case Op::SENDMSG:
      default:
        return true;
    }
  }

  constexpr const char* IOMethodToString() const {
    switch (op) {
      case Op::READ:
        return "Read";
      case Op::READV:
        return "Readv";
      case Op::RECV:
        return "Recv";
      case Op::RECVFROM:
        return "Recvfrom";
      case Op::RECVMSG:
        return "Recvmsg";
      case Op::WRITE:
        return "Write";
      case Op::WRITEV:
        return "Writev";
      case Op::SEND:
        return "Send";
      case Op::SENDTO:
        return "Sendto";
      case Op::SENDMSG:
        return "Sendmsg";
    }
  }

 private:
  enum Op op;
};

#if !defined(__Fuchsia__)
// disableSIGPIPE is typically invoked on Linux, in cases where the caller
// expects to perform stream socket writes on an unconnected socket. In such
// cases, SIGPIPE is expected on Linux. This returns a fbl::AutoCall object
// whose destructor would undo the signal masking performed here.
//
// send{,to,msg} support the MSG_NOSIGNAL flag to suppress this behaviour, but
// write and writev do not.
auto disableSIGPIPE(bool isWrite) {
  struct sigaction act = {
      .sa_handler = SIG_IGN,
  };
  struct sigaction oldact;
  if (isWrite) {
    EXPECT_EQ(sigaction(SIGPIPE, &act, &oldact), 0) << strerror(errno);
  }
  return fbl::MakeAutoCall([=]() {
    if (isWrite) {
      EXPECT_EQ(sigaction(SIGPIPE, &oldact, nullptr), 0) << strerror(errno);
    }
  });
}
#endif

class IOMethodTest : public ::testing::TestWithParam<IOMethod> {};

void doNullPtrIO(const fbl::unique_fd& fd, const fbl::unique_fd& other, IOMethod ioMethod,
                 bool datagram) {
  // A version of ioMethod::executeIO with special handling for vectorized operations: a 1-byte
  // buffer is prepended to the argument.
  auto executeIO = [ioMethod](int fd, char* buf, size_t len) {
    char buffer[1];
    struct iovec iov[] = {
        {
            .iov_base = buffer,
            .iov_len = std::size(buffer),
        },
        {
            .iov_base = buf,
            .iov_len = len,
        },
    };
    struct msghdr msg = {
        .msg_iov = iov,
        .msg_iovlen = std::size(iov),
    };

    switch (ioMethod.Op()) {
      case IOMethod::Op::READ:
      case IOMethod::Op::RECV:
      case IOMethod::Op::RECVFROM:
      case IOMethod::Op::WRITE:
      case IOMethod::Op::SEND:
      case IOMethod::Op::SENDTO:
        return ioMethod.executeIO(fd, buf, len);
      case IOMethod::Op::READV:
        return readv(fd, iov, std::size(iov));
      case IOMethod::Op::RECVMSG:
        return recvmsg(fd, &msg, 0);
      case IOMethod::Op::WRITEV:
        return writev(fd, iov, std::size(iov));
      case IOMethod::Op::SENDMSG:
        return sendmsg(fd, &msg, 0);
    }
  };

  auto prepareForRead = [&](const char* buf, size_t len) {
    ASSERT_EQ(send(other.get(), buf, len, 0), static_cast<ssize_t>(len)) << strerror(errno);

    // Wait for the packet to arrive since we are nonblocking.
    struct pollfd pfd = {
        .fd = fd.get(),
        .events = POLLIN,
    };

    int n = poll(&pfd, 1, kTimeout);
    ASSERT_GE(n, 0) << strerror(errno);
    ASSERT_EQ(n, 1);
    EXPECT_EQ(pfd.revents, POLLIN);
  };

  auto confirmWrite = [&]() {
    char buffer[1];
#if defined(__Fuchsia__)
    if (!datagram) {
      switch (ioMethod.Op()) {
        case IOMethod::Op::WRITE:
        case IOMethod::Op::SEND:
        case IOMethod::Op::SENDTO:
          break;
        case IOMethod::Op::WRITEV:
        case IOMethod::Op::SENDMSG: {
          // Fuchsia doesn't comply because zircon sockets do not implement atomic vector
          // operations, so these vector operations end up having sent the byte provided in the
          // executeIO closure. See https://fxbug.dev/67928 for more details.
          //
          // Wait for the packet to arrive since we are nonblocking.
          struct pollfd pfd = {
              .fd = other.get(),
              .events = POLLIN,
          };
          int n = poll(&pfd, 1, kTimeout);
          ASSERT_GE(n, 0) << strerror(errno);
          ASSERT_EQ(n, 1);
          EXPECT_EQ(pfd.revents, POLLIN);
          EXPECT_EQ(recv(other.get(), buffer, std::size(buffer), 0), 1) << strerror(errno);
          return;
        }
        default:
          FAIL() << "unexpected method " << ioMethod.IOMethodToString();
      }
    }
#endif
    // Nothing was sent. This is not obvious in the vectorized case.
    EXPECT_EQ(recv(other.get(), buffer, std::size(buffer), 0), -1);
    EXPECT_EQ(errno, EAGAIN) << strerror(errno);
  };

  // Receive some data so we can attempt to read it below.
  if (!ioMethod.isWrite()) {
    char buffer[] = {0x74, 0x75};
    prepareForRead(buffer, std::size(buffer));
  }

  [&]() {
#if defined(__Fuchsia__)
    if (!datagram) {
      switch (ioMethod.Op()) {
        case IOMethod::Op::READ:
        case IOMethod::Op::RECV:
        case IOMethod::Op::RECVFROM:
        case IOMethod::Op::WRITE:
        case IOMethod::Op::SEND:
        case IOMethod::Op::SENDTO:
          break;

        case IOMethod::Op::READV:
        case IOMethod::Op::RECVMSG:
        case IOMethod::Op::WRITEV:
        case IOMethod::Op::SENDMSG:
          // Fuchsia doesn't comply because zircon sockets do not implement atomic vector
          // operations, so these vector operations report success on the byte provided in the
          // executeIO closure.
          EXPECT_EQ(executeIO(fd.get(), nullptr, 1), 1) << strerror(errno);
          return;
      }
    }
#endif
    EXPECT_EQ(executeIO(fd.get(), nullptr, 1), -1);
    EXPECT_EQ(errno, EFAULT) << strerror(errno);
  }();

  if (ioMethod.isWrite()) {
    confirmWrite();
  } else {
    char buffer[1];
    auto result = executeIO(fd.get(), buffer, std::size(buffer));
    if (datagram) {
      // The datagram was consumed in spite of the buffer being null.
      EXPECT_EQ(result, -1);
      EXPECT_EQ(errno, EAGAIN) << strerror(errno);
    } else {
      ssize_t space = std::size(buffer);
      switch (ioMethod.Op()) {
        case IOMethod::Op::READ:
        case IOMethod::Op::RECV:
        case IOMethod::Op::RECVFROM:
          break;
        case IOMethod::Op::READV:
        case IOMethod::Op::RECVMSG:
#if defined(__Fuchsia__)
          // Fuchsia consumed one byte above.
#else
          // An additional byte of space was provided in the executeIO closure.
          space += 1;
#endif
          break;
        default:
          FAIL() << "unexpected method " << ioMethod.IOMethodToString();
      }
      EXPECT_EQ(result, space) << strerror(errno);
    }
  }

  // Do it again, but this time write less data so that vector operations can work normally.
  if (!ioMethod.isWrite()) {
    char buffer[] = {0x74};
    prepareForRead(buffer, std::size(buffer));
  }

  switch (ioMethod.Op()) {
    case IOMethod::Op::WRITEV:
    case IOMethod::Op::SENDMSG:
#if defined(__Fuchsia__)
      if (!datagram) {
        // Fuchsia doesn't comply because zircon sockets do not implement atomic vector
        // operations, so these vector operations report success on the byte provided in the
        // executeIO closure.
        EXPECT_EQ(executeIO(fd.get(), nullptr, 1), 1) << strerror(errno);
        break;
      }
#endif
    case IOMethod::Op::READ:
    case IOMethod::Op::RECV:
    case IOMethod::Op::RECVFROM:
    case IOMethod::Op::WRITE:
    case IOMethod::Op::SEND:
    case IOMethod::Op::SENDTO:
      EXPECT_EQ(executeIO(fd.get(), nullptr, 1), -1);
      EXPECT_EQ(errno, EFAULT) << strerror(errno);
      break;
    case IOMethod::Op::READV:
    case IOMethod::Op::RECVMSG:
      // These vectorized operations never reach the faulty buffer, so they work normally.
      EXPECT_EQ(executeIO(fd.get(), nullptr, 1), 1) << strerror(errno);
      break;
  }

  if (ioMethod.isWrite()) {
    confirmWrite();
  } else {
    char buffer[1];
    auto result = executeIO(fd.get(), buffer, std::size(buffer));
    if (datagram) {
      // The datagram was consumed in spite of the buffer being null.
      EXPECT_EQ(result, -1);
      EXPECT_EQ(errno, EAGAIN) << strerror(errno);
    } else {
      switch (ioMethod.Op()) {
        case IOMethod::Op::READ:
        case IOMethod::Op::RECV:
        case IOMethod::Op::RECVFROM:
          EXPECT_EQ(result, static_cast<ssize_t>(std::size(buffer))) << strerror(errno);
          break;
        case IOMethod::Op::READV:
        case IOMethod::Op::RECVMSG:
          // The byte we sent was consumed in the executeIO closure.
          EXPECT_EQ(result, -1);
          EXPECT_EQ(errno, EAGAIN) << strerror(errno);
          break;
        default:
          FAIL() << "unexpected method " << ioMethod.IOMethodToString();
      }
    }
  }
}

TEST_P(IOMethodTest, NullptrFaultDGRAM) {
  fbl::unique_fd fd;
  ASSERT_TRUE(fd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0)))
      << strerror(errno);
  const struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port = 1235,
      .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
  };

  ASSERT_EQ(bind(fd.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  ASSERT_EQ(connect(fd.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  doNullPtrIO(fd, fd, GetParam(), true);
}

TEST_P(IOMethodTest, NullptrFaultSTREAM) {
  fbl::unique_fd listener, client, server;
  ASSERT_TRUE(listener = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);

  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr.s_addr = htonl(INADDR_ANY),
  };
  ASSERT_EQ(bind(listener.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  socklen_t addrlen = sizeof(addr);
  ASSERT_EQ(getsockname(listener.get(), reinterpret_cast<struct sockaddr*>(&addr), &addrlen), 0)
      << strerror(errno);
  ASSERT_EQ(addrlen, sizeof(addr));

  ASSERT_EQ(listen(listener.get(), 1), 0) << strerror(errno);

  ASSERT_TRUE(client = fbl::unique_fd(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)))
      << strerror(errno);
  int ret;
  EXPECT_EQ(
      ret = connect(client.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)),
      -1);
  if (ret == -1) {
    ASSERT_EQ(EINPROGRESS, errno) << strerror(errno);

    struct pollfd pfd = {
        .fd = client.get(),
        .events = POLLOUT,
    };
    int n = poll(&pfd, 1, kTimeout);
    ASSERT_GE(n, 0) << strerror(errno);
    ASSERT_EQ(n, 1);
  }

  ASSERT_TRUE(server = fbl::unique_fd(accept4(listener.get(), nullptr, nullptr, SOCK_NONBLOCK)))
      << strerror(errno);
  ASSERT_EQ(close(listener.release()), 0) << strerror(errno);

  doNullPtrIO(client, server, GetParam(), false);
}

// BeforeConnect tests the application behavior when we start to
// read and write from a stream socket that is not yet connected.
TEST_P(IOMethodTest, BeforeConnect) {
  auto ioMethod = GetParam();
  fbl::unique_fd listener;
  ASSERT_TRUE(listener = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);

  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr.s_addr = htonl(INADDR_ANY),
  };
  ASSERT_EQ(bind(listener.get(), reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  socklen_t addrlen = sizeof(addr);
  ASSERT_EQ(getsockname(listener.get(), reinterpret_cast<struct sockaddr*>(&addr), &addrlen), 0)
      << strerror(errno);
  ASSERT_EQ(addrlen, sizeof(addr));

  ASSERT_EQ(listen(listener.get(), 0), 0) << strerror(errno);

  // Setup a test client connection over which we test socket reads
  // when the connection is not yet established.

  // Linux default behavior is to complete one more connection than what
  // was passed as listen backlog (zero here).
  // Hence we initiate 2 client connections in this order:
  // (1) a precursor client for the sole purpose of filling up the server
  //     accept queue after handshake completion.
  // (2) a test client that keeps trying to establish connection with
  //     server, but remains in SYN-SENT.
#if !defined(__Fuchsia__)
  // TODO(gvisor.dev/issue/3153): Unlike Linux, gVisor does not complete
  // handshake for a connection when listen backlog is zero. Hence, we
  // do not maintain the precursor client connection on Fuchsia.
  fbl::unique_fd precursor_client;
  ASSERT_TRUE(precursor_client = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0)))
      << strerror(errno);
  ASSERT_EQ(
      connect(precursor_client.get(), reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  // Observe the precursor client connection on the server side. This ensures that the TCP stack's
  // server accept queue is updated with the precursor client connection before any subsequent
  // client connect requests. The precursor client connect call returns after handshake completion,
  // but not necessarily after the server side has processed the ACK from the client and updated its
  // accept queue.
  struct pollfd pfd = {
      .fd = listener.get(),
      .events = POLLIN,
  };
  int n = poll(&pfd, 1, kTimeout);
  ASSERT_GE(n, 0) << strerror(errno);
  ASSERT_EQ(n, 1);
  ASSERT_EQ(pfd.revents, POLLIN);
#endif

  // The test client connection would get established _only_ after both
  // these conditions are met:
  // (1) prior client connections are accepted by the server thus
  //     making room for a new connection.
  // (2) the server-side TCP stack completes handshake in response to
  //     the retransmitted SYN for the test client connection.
  //
  // The test would likely perform socket reads before any connection
  // timeout.
  fbl::unique_fd test_client;
  ASSERT_TRUE(test_client = fbl::unique_fd(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)))
      << strerror(errno);

  // Sample data to be written.
  char sample_data[] = "Sample Data";
  // To correctly test reads, keep alteast one byte larger read buffer than what would be written.
  char recvbuf[sizeof(sample_data) + 1] = {};
  bool isWrite = ioMethod.isWrite();
  auto executeIO = [&]() {
    if (isWrite) {
      return ioMethod.executeIO(test_client.get(), sample_data, sizeof(sample_data));
    }
    return ioMethod.executeIO(test_client.get(), recvbuf, sizeof(recvbuf));
  };
#if !defined(__Fuchsia__)
  auto undo = disableSIGPIPE(isWrite);
#endif

  EXPECT_EQ(executeIO(), -1);
  if (isWrite) {
    EXPECT_EQ(errno, EPIPE) << strerror(errno);
  } else {
    EXPECT_EQ(errno, ENOTCONN) << strerror(errno);
  }

  ASSERT_EQ(connect(test_client.get(), reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)),
            -1);
  ASSERT_EQ(EINPROGRESS, errno) << strerror(errno);

  // Test socket I/O without waiting for connection to be established.
  EXPECT_EQ(executeIO(), -1);
  EXPECT_EQ(errno, EWOULDBLOCK) << strerror(errno);

  std::latch fut_started(1);
  // Asynchronously block on I/O from the test client socket.
  const auto fut = std::async(std::launch::async, [&]() {
    // Make the socket blocking.
    int flags = fcntl(test_client.get(), F_GETFL, 0);
    EXPECT_EQ(0, fcntl(test_client.get(), F_SETFL, flags ^ O_NONBLOCK)) << strerror(errno);

    fut_started.count_down();

    EXPECT_EQ(executeIO(), static_cast<ssize_t>(sizeof(sample_data)));
  });
  fut_started.wait();
  EXPECT_EQ(fut.wait_for(std::chrono::milliseconds(10)), std::future_status::timeout);

#if !defined(__Fuchsia__)
  // Accept the precursor connection to make room for the test client
  // connection to complete.
  fbl::unique_fd precursor_accept;
  ASSERT_TRUE(precursor_accept = fbl::unique_fd(accept(listener.get(), nullptr, nullptr)))
      << strerror(errno);
  ASSERT_EQ(close(precursor_accept.release()), 0) << strerror(errno);
  ASSERT_EQ(close(precursor_client.release()), 0) << strerror(errno);
#endif

  // TODO(gvisor.dev/issue/3153): Unlike Linux, gVisor does not accept a connection
  // when listen backlog is zero.
#if defined(__Fuchsia__)
  ASSERT_EQ(listen(listener.get(), 1), 0) << strerror(errno);
#endif

  // Accept the test client connection.
  fbl::unique_fd test_accept;
  ASSERT_TRUE(test_accept = fbl::unique_fd(accept(listener.get(), nullptr, nullptr)))
      << strerror(errno);

  if (isWrite) {
    // Ensure that we read the data whose send request was enqueued until
    // the connection was established.
    ASSERT_EQ(read(test_accept.get(), recvbuf, sizeof(recvbuf)),
              static_cast<ssize_t>(sizeof(sample_data)))
        << strerror(errno);
    ASSERT_STREQ(recvbuf, sample_data);
  } else {
    // Write data to unblock the socket read on the test client connection.
    ASSERT_EQ(write(test_accept.get(), sample_data, sizeof(sample_data)),
              static_cast<ssize_t>(sizeof(sample_data)))
        << strerror(errno);
  }

  EXPECT_EQ(fut.wait_for(std::chrono::milliseconds(kTimeout)), std::future_status::ready);

  ASSERT_EQ(close(listener.release()), 0) << strerror(errno);
  ASSERT_EQ(close(test_accept.release()), 0) << strerror(errno);
  ASSERT_EQ(close(test_client.release()), 0) << strerror(errno);
}

INSTANTIATE_TEST_SUITE_P(NetStreamTest, IOMethodTest,
                         ::testing::Values(IOMethod::Op::READ, IOMethod::Op::READV,
                                           IOMethod::Op::RECV, IOMethod::Op::RECVFROM,
                                           IOMethod::Op::RECVMSG, IOMethod::Op::WRITE,
                                           IOMethod::Op::WRITEV, IOMethod::Op::SEND,
                                           IOMethod::Op::SENDTO, IOMethod::Op::SENDMSG),
                         [](const ::testing::TestParamInfo<IOMethod>& info) {
                           return info.param.IOMethodToString();
                         });

namespace {
// Test close/shutdown of listening socket with multiple non-blocking connects.
// This tests client sockets in connected and connecting states.
void TestListenWhileConnect(const IOMethod& ioMethod, void (*stopListen)(fbl::unique_fd&)) {
  fbl::unique_fd client, listener;
  ASSERT_TRUE(listener = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);
  constexpr int kBacklog = 2;
  // Linux completes one more connection than the listen backlog argument.
  // To ensure that there is at least one client connection that stays in
  // connecting state, keep 2 more client connections than the listen backlog.
  // gVisor differs in this behavior though, gvisor.dev/issue/3153.
  constexpr int kClients = kBacklog + 2;

  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
  };

  ASSERT_EQ(bind(listener.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);
  ASSERT_EQ(listen(listener.get(), kBacklog), 0) << strerror(errno);

  socklen_t addrlen = sizeof(addr);
  ASSERT_EQ(getsockname(listener.get(), reinterpret_cast<struct sockaddr*>(&addr), &addrlen), 0)
      << strerror(errno);
  ASSERT_EQ(addrlen, sizeof(addr));

  std::array<fbl::unique_fd, kClients> clients;
  for (fbl::unique_fd& client : clients) {
    ASSERT_TRUE(client = fbl::unique_fd(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)))
        << strerror(errno);
    int ret = connect(client.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    // Linux manpage for connect, for EINPROGRESS error:
    // "The socket is nonblocking and the connection cannot be completed immediately."
    // Which means that the non-blocking connect may succeed (ie. ret == 0) in the unlikely case
    // where the connection does complete immediately before the system call returns.
    //
    // On Fuchsia, a non-blocking connect would always fail with EINPROGRESS.
#if !defined(__Fuchsia__)
    if (ret != 0)
#endif
    {
      EXPECT_EQ(ret, -1);
      EXPECT_EQ(errno, EINPROGRESS) << strerror(errno);
    }
  }

  ASSERT_NO_FATAL_FAILURE(stopListen(listener));

  for (auto& client : clients) {
    struct pollfd pfd = {
        .fd = client.get(),
        .events = POLLIN,
    };
    // When the listening socket is stopped, then we expect the remote to reset
    // the connection.
    int n = poll(&pfd, 1, kTimeout);
    ASSERT_GE(n, 0) << strerror(errno);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(pfd.revents, POLLIN | POLLHUP | POLLERR);
    char c;
    ASSERT_EQ(ioMethod.executeIO(client.get(), &c, sizeof(c)), -1);
    // Subsequent read can fail with:
    // ECONNRESET: If the client connection was established and was reset by the
    // remote.
    // ECONNREFUSED: If the client connection failed to be established.
    ASSERT_TRUE(errno == ECONNRESET || errno == ECONNREFUSED) << strerror(errno);
    // The last client connection would be in connecting (SYN_SENT) state.
    if (client == clients[kClients - 1]) {
      ASSERT_EQ(errno, ECONNREFUSED) << strerror(errno);
    }

    bool isWrite = ioMethod.isWrite();
#if !defined(__Fuchsia__)
    auto undo = disableSIGPIPE(isWrite);
#endif

    if (isWrite) {
      ASSERT_EQ(ioMethod.executeIO(client.get(), &c, sizeof(c)), -1);
      EXPECT_EQ(errno, EPIPE) << strerror(errno);
    } else {
      ASSERT_EQ(ioMethod.executeIO(client.get(), &c, sizeof(c)), 0) << strerror(errno);
    }
  }
}

class StopListenWhileConnect : public ::testing::TestWithParam<IOMethod> {};

TEST_P(StopListenWhileConnect, Close) {
  TestListenWhileConnect(
      GetParam(), [](fbl::unique_fd& f) { ASSERT_EQ(close(f.release()), 0) << strerror(errno); });
}

TEST_P(StopListenWhileConnect, Shutdown) {
  TestListenWhileConnect(GetParam(), [](fbl::unique_fd& f) {
    ASSERT_EQ(shutdown(f.get(), SHUT_RD), 0) << strerror(errno);
  });
}

INSTANTIATE_TEST_SUITE_P(NetStreamTest, StopListenWhileConnect,
                         ::testing::Values(IOMethod::Op::READ, IOMethod::Op::READV,
                                           IOMethod::Op::RECV, IOMethod::Op::RECVFROM,
                                           IOMethod::Op::RECVMSG, IOMethod::Op::WRITE,
                                           IOMethod::Op::WRITEV, IOMethod::Op::SEND,
                                           IOMethod::Op::SENDTO, IOMethod::Op::SENDMSG),
                         [](const ::testing::TestParamInfo<IOMethod>& info) {
                           return info.param.IOMethodToString();
                         });
}  // namespace

TEST(NetStreamTest, NonBlockingConnectRefused) {
  fbl::unique_fd acptfd;
  ASSERT_TRUE(acptfd = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);

  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr.s_addr = htonl(INADDR_ANY),
  };
  ASSERT_EQ(bind(acptfd.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  socklen_t addrlen = sizeof(addr);
  ASSERT_EQ(getsockname(acptfd.get(), reinterpret_cast<struct sockaddr*>(&addr), &addrlen), 0)
      << strerror(errno);

  // No listen() on acptfd.

  fbl::unique_fd connfd;
  ASSERT_TRUE(connfd = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);

  int flags = fcntl(connfd.get(), F_GETFL, 0);
  ASSERT_EQ(0, fcntl(connfd.get(), F_SETFL, flags | O_NONBLOCK));

  int ret;
  EXPECT_EQ(
      ret = connect(connfd.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)),
      -1);
  if (ret == -1) {
    ASSERT_EQ(EINPROGRESS, errno) << strerror(errno);

    struct pollfd pfd = {
        .fd = connfd.get(),
        .events = POLLOUT,
    };
    int n = poll(&pfd, 1, kTimeout);
    ASSERT_GE(n, 0) << strerror(errno);
    ASSERT_EQ(n, 1);

    int err;
    socklen_t optlen = sizeof(err);
    ASSERT_EQ(getsockopt(connfd.get(), SOL_SOCKET, SO_ERROR, &err, &optlen), 0) << strerror(errno);
    ASSERT_EQ(err, ECONNREFUSED);
    ASSERT_EQ(optlen, sizeof(err));
  }

  EXPECT_EQ(close(connfd.release()), 0) << strerror(errno);
  EXPECT_EQ(close(acptfd.release()), 0) << strerror(errno);
}

TEST(NetStreamTest, GetTcpInfo) {
  fbl::unique_fd connfd;
  ASSERT_TRUE(connfd = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);

  tcp_info info;
  socklen_t info_len = sizeof(tcp_info);
  ASSERT_GE(getsockopt(connfd.get(), SOL_TCP, TCP_INFO, &info, &info_len), 0) << strerror(errno);
  ASSERT_EQ(sizeof(tcp_info), info_len);

  // Test that we can partially retrieve TCP_INFO.
  uint8_t tcpi_state;
  info_len = sizeof(tcpi_state);
  ASSERT_GE(getsockopt(connfd.get(), SOL_TCP, TCP_INFO, &tcpi_state, &info_len), 0)
      << strerror(errno);
  ASSERT_EQ(sizeof(tcpi_state), info_len);

  ASSERT_EQ(0, close(connfd.release()));
}

TEST(NetStreamTest, GetSocketAcceptConn) {
  fbl::unique_fd fd;
  ASSERT_TRUE(fd = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);

  int got = -1;
  socklen_t got_len = sizeof(got);
  ASSERT_EQ(getsockopt(fd.get(), SOL_SOCKET, SO_ACCEPTCONN, &got, &got_len), 0) << strerror(errno);
  EXPECT_EQ(got_len, sizeof(got));
  EXPECT_EQ(got, 0);

  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr.s_addr = htonl(INADDR_ANY),
  };
  ASSERT_EQ(bind(fd.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);
  got = -1;
  got_len = sizeof(got);
  ASSERT_EQ(getsockopt(fd.get(), SOL_SOCKET, SO_ACCEPTCONN, &got, &got_len), 0) << strerror(errno);
  EXPECT_EQ(got_len, sizeof(got));
  EXPECT_EQ(got, 0);

  ASSERT_EQ(listen(fd.get(), 0), 0) << strerror(errno);
  got = -1;
  got_len = sizeof(got);
  ASSERT_EQ(getsockopt(fd.get(), SOL_SOCKET, SO_ACCEPTCONN, &got, &got_len), 0) << strerror(errno);
  EXPECT_EQ(got_len, sizeof(got));
  EXPECT_EQ(got, 1);

  ASSERT_EQ(shutdown(fd.get(), SHUT_WR), 0) << strerror(errno);
  // TODO(https://fxbug.dev/61714): Fix the race with shutdown and getsockopt.
#if !defined(__Fuchsia__)
  got = -1;
  got_len = sizeof(got);
  ASSERT_EQ(getsockopt(fd.get(), SOL_SOCKET, SO_ACCEPTCONN, &got, &got_len), 0) << strerror(errno);
  EXPECT_EQ(got_len, sizeof(got));
  EXPECT_EQ(got, 1);
#endif

  ASSERT_EQ(shutdown(fd.get(), SHUT_RD), 0) << strerror(errno);
  // TODO(https://fxbug.dev/61714): Fix the race with shutdown and getsockopt.
#if !defined(__Fuchsia__)
  got = -1;
  got_len = sizeof(got);
  ASSERT_EQ(getsockopt(fd.get(), SOL_SOCKET, SO_ACCEPTCONN, &got, &got_len), 0) << strerror(errno);
  EXPECT_EQ(got_len, sizeof(got));
  EXPECT_EQ(got, 0);
#endif
}

// Test socket reads on disconnected stream sockets.
TEST(NetStreamTest, DisconnectedRead) {
  fbl::unique_fd socketfd;
  ASSERT_TRUE(socketfd = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);
  struct timeval tv = {
      // Use minimal non-zero timeout as we expect the blocking recv to return before it actually
      // starts reading. Without the timeout, the test could deadlock on a blocking recv, when the
      // underlying code is broken.
      .tv_usec = 1u,
  };
  EXPECT_EQ(setsockopt(socketfd.get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)), 0)
      << strerror(errno);
  // Test blocking socket read.
  EXPECT_EQ(recvfrom(socketfd.get(), nullptr, 0, 0, nullptr, nullptr), -1);
  EXPECT_EQ(errno, ENOTCONN) << strerror(errno);
  // Test with MSG_PEEK.
  EXPECT_EQ(recvfrom(socketfd.get(), nullptr, 0, MSG_PEEK, nullptr, nullptr), -1);
  EXPECT_EQ(errno, ENOTCONN) << strerror(errno);

  // Test non blocking socket read.
  int flags;
  EXPECT_GE(flags = fcntl(socketfd.get(), F_GETFL, 0), 0) << strerror(errno);
  EXPECT_EQ(fcntl(socketfd.get(), F_SETFL, flags | O_NONBLOCK), 0) << strerror(errno);
  EXPECT_EQ(recvfrom(socketfd.get(), nullptr, 0, 0, nullptr, nullptr), -1);
  EXPECT_EQ(errno, ENOTCONN) << strerror(errno);
  // Test with MSG_PEEK.
  EXPECT_EQ(recvfrom(socketfd.get(), nullptr, 0, MSG_PEEK, nullptr, nullptr), -1);
  EXPECT_EQ(errno, ENOTCONN) << strerror(errno);
  EXPECT_EQ(close(socketfd.release()), 0) << strerror(errno);
}

TEST_F(NetStreamSocketsTest, Shutdown) {
  EXPECT_EQ(shutdown(server().get(), SHUT_WR), 0) << strerror(errno);

  struct pollfd pfd = {
      .fd = client().get(),
      .events = POLLRDHUP,
  };
  int n = poll(&pfd, 1, kTimeout);
  EXPECT_GE(n, 0) << strerror(errno);
  EXPECT_EQ(n, 1);
  EXPECT_EQ(pfd.revents, POLLRDHUP);
}

TEST_F(NetStreamSocketsTest, ResetOnFullReceiveBufferShutdown) {
  // Fill the receive buffer of the client socket.
  fill_stream_send_buf(server().get(), client().get());

  // Setting SO_LINGER to 0 and `close`ing the server socket should
  // immediately send a TCP RST.
  struct linger opt = {
      .l_onoff = 1,
      .l_linger = 0,
  };
  EXPECT_EQ(setsockopt(server().get(), SOL_SOCKET, SO_LINGER, &opt, sizeof(opt)), 0)
      << strerror(errno);

  // Close the server to trigger a TCP RST now that linger is 0.
  EXPECT_EQ(close(server().release()), 0) << strerror(errno);

  // Wait for the RST.
  struct pollfd pfd = {
      .fd = client().get(),
  };
  int n = poll(&pfd, 1, kTimeout);
  ASSERT_GE(n, 0) << strerror(errno);
  ASSERT_EQ(n, 1);
  EXPECT_EQ(pfd.revents, POLLHUP | POLLERR);

  // The socket is no longer connected.
  EXPECT_EQ(shutdown(client().get(), SHUT_RD), -1);
  EXPECT_EQ(errno, ENOTCONN) << strerror(errno);

  // Create another socket to ensure that the networking stack hasn't panicked.
  fbl::unique_fd test_sock;
  ASSERT_TRUE(test_sock = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);
}

// Tests that a socket which has completed SHUT_RDWR responds to incoming data with RST.
TEST_F(NetStreamSocketsTest, ShutdownReset) {
  // This test is tricky. In Linux we could shutdown(SHUT_RDWR) the server socket, write() some data
  // on the client socket, and observe the server reply with RST. The SHUT_WR would move the server
  // socket state out of ESTABLISHED (to FIN-WAIT2 after sending FIN and receiving an ACK) and
  // SHUT_RD would close the receiver. Only when the server socket has transitioned out of
  // ESTABLISHED state. At this point, the server socket would respond to incoming data with RST.
  //
  // In Fuchsia this is more complicated because each socket is a distributed system (consisting of
  // netstack and fdio) wherein the socket state is eventually consistent. We must take care to
  // synchronize our actions with netstack's state as we're testing that netstack correctly sends a
  // RST in response to data received after shutdown(SHUT_RDWR).
  //
  // We can manipulate and inspect state using only shutdown() and poll(), both of which operate on
  // fdio state rather than netstack state. Combined with the fact that SHUT_RD is not observable by
  // the peer (i.e. doesn't cause any network traffic), means we are in a pickle.
  //
  // On the other hand, SHUT_WR does cause a FIN to be sent, which can be observed by the peer using
  // poll(POLLRDHUP). Note also that netstack observes SHUT_RD and SHUT_WR on different threads,
  // meaning that a race condition still exists. At the time of writing, this is the best we can do.

  // Change internal state to disallow further reads and writes. The state change propagates to
  // netstack at some future time. We have no way to observe that SHUT_RD has propagated (because it
  // propagates independently from SHUT_WR).
  ASSERT_EQ(shutdown(server().get(), SHUT_RDWR), 0) << strerror(errno);

  // Wait for the FIN to arrive at the client and for the state to propagate to the client's fdio.
  {
    struct pollfd pfd = {
        .fd = client().get(),
        .events = POLLRDHUP,
    };
    int n = poll(&pfd, 1, kTimeout);
    ASSERT_GE(n, 0) << strerror(errno);
    ASSERT_EQ(n, 1);
    EXPECT_EQ(pfd.revents, POLLRDHUP);
  }

  // Send data from the client(). The server should now very likely be in SHUT_RD and respond with
  // RST.
  char c;
  ASSERT_EQ(write(client().get(), &c, sizeof(c)), static_cast<ssize_t>(sizeof(c)))
      << strerror(errno);

  // Wait for the client to receive the RST and for the state to propagate through its fdio.
  struct pollfd pfd = {
      .fd = client().get(),
  };
  int n = poll(&pfd, 1, kTimeout);
  ASSERT_GE(n, 0) << strerror(errno);
  ASSERT_EQ(n, 1);
  EXPECT_EQ(pfd.revents, POLLHUP | POLLERR);
}

// ShutdownPendingWrite tests for all of the application writes that
// occurred before shutdown SHUT_WR, to be received by the remote.
TEST_F(NetStreamSocketsTest, ShutdownPendingWrite) {
  // Fill the send buffer of the server socket so that we have some
  // pending data waiting to be sent out to the remote.
  ssize_t wrote = fill_stream_send_buf(server().get(), client().get());

  // SHUT_WR should enqueue a FIN after all of the application writes.
  EXPECT_EQ(shutdown(server().get(), SHUT_WR), 0) << strerror(errno);

  // All client reads are expected to return here, including the last
  // read on receiving a FIN. Keeping a timeout for unexpected failures.
  struct timeval tv = {
      .tv_sec = kTimeout,
  };
  EXPECT_EQ(setsockopt(client().get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)), 0)
      << strerror(errno);

  ssize_t rcvd = 0;
  ssize_t ret;
  // Keep a large enough buffer to reduce the number of read calls, as
  // we expect the receive buffer to be filled up at this point.
  char buf[4096];
  // Each read would make room for the server to send out more data
  // that has been enqueued from successful server socket writes.
  while ((ret = read(client().get(), &buf, sizeof(buf))) > 0) {
    rcvd += ret;
  }
  // Expect the last read to return 0 after the stack sees a FIN.
  EXPECT_EQ(ret, 0) << strerror(errno);
  // Expect no data drops and all written data by server is received
  // by the client().
  EXPECT_EQ(rcvd, wrote);
}

enum class CloseTarget {
  CLIENT,
  SERVER,
};

constexpr const char* CloseTargetToString(const CloseTarget s) {
  switch (s) {
    case CloseTarget::CLIENT:
      return "Client";
    case CloseTarget::SERVER:
      return "Server";
  }
}

using blockedIOParams = std::tuple<IOMethod, CloseTarget, bool>;

class BlockedIOTest : public NetStreamSocketsTest,
                      public ::testing::WithParamInterface<blockedIOParams> {};

TEST_P(BlockedIOTest, CloseWhileBlocked) {
  auto const& [ioMethod, closeTarget, lingerEnabled] = GetParam();

  bool isWrite = ioMethod.isWrite();

  // If linger is enabled, closing the socket will cause a TCP RST (by definition).
  bool closeRST = lingerEnabled;
  if (isWrite) {
    // Fill the send buffer of the client socket to cause write to block.
    fill_stream_send_buf(client().get(), server().get());
    // Buffes are full. Closing the socket will now cause a TCP RST.
    closeRST = true;
  }

  // While blocked in I/O, close the peer.
  std::latch fut_started(1);
  // NB: lambdas are not allowed to capture reference to local binding declared
  // in enclosing function.
  const auto fut = std::async(std::launch::async, [&, op = ioMethod]() {
    fut_started.count_down();
    char c;
    if (closeRST) {
      ASSERT_EQ(op.executeIO(client().get(), &c, sizeof(c)), -1);
      EXPECT_EQ(errno, ECONNRESET) << strerror(errno);
    } else {
      ASSERT_EQ(op.executeIO(client().get(), &c, sizeof(c)), 0) << strerror(errno);
    }
  });
  fut_started.wait();
  // Give the asynchronous blocking operation some time to reach the blocking state. Clocks
  // sometimes jump in infrastructure, which may cause a single wait to trip sooner than expected,
  // without the asynchronous task getting a meaningful shot at running. We protect against that by
  // splitting the wait into multiple calls as an attempt to guarantee that clock jumps are not what
  // causes the wait below to continue prematurely.
  for (int i = 0; i < 50; i++) {
    EXPECT_EQ(fut.wait_for(std::chrono::milliseconds(1)), std::future_status::timeout);
  }

  // When enabled, causes `close` to send a TCP RST.
  struct linger opt = {
      .l_onoff = lingerEnabled,
      .l_linger = 0,
  };

  switch (closeTarget) {
    case CloseTarget::CLIENT: {
      ASSERT_EQ(setsockopt(client().get(), SOL_SOCKET, SO_LINGER, &opt, sizeof(opt)), 0)
          << strerror(errno);

      int fd = client().release();

      ASSERT_EQ(close(fd), 0) << strerror(errno);

      // Closing the file descriptor does not interrupt the pending I/O.
      ASSERT_EQ(fut.wait_for(std::chrono::milliseconds(10)), std::future_status::timeout);

      // The pending I/O is still blocked, but the file descriptor is gone.
      ASSERT_EQ(fsync(fd), -1) << strerror(errno);
      ASSERT_EQ(errno, EBADF) << errno;

      // Fallthrough to unblock the future.
    }
    case CloseTarget::SERVER: {
      ASSERT_EQ(setsockopt(server().get(), SOL_SOCKET, SO_LINGER, &opt, sizeof(opt)), 0)
          << strerror(errno);
      ASSERT_EQ(close(server().release()), 0) << strerror(errno);
      break;
    }
  }
  ASSERT_EQ(fut.wait_for(std::chrono::milliseconds(kTimeout)), std::future_status::ready);

#if !defined(__Fuchsia__)
  auto undo = disableSIGPIPE(isWrite);
#endif

  char c;
  switch (closeTarget) {
    case CloseTarget::CLIENT: {
      ASSERT_EQ(ioMethod.executeIO(client().get(), &c, sizeof(c)), -1);
      EXPECT_EQ(errno, EBADF) << strerror(errno);
      break;
    }
    case CloseTarget::SERVER: {
      if (isWrite) {
        ASSERT_EQ(ioMethod.executeIO(client().get(), &c, sizeof(c)), -1);
        EXPECT_EQ(errno, EPIPE) << strerror(errno);
      } else {
        ASSERT_EQ(ioMethod.executeIO(client().get(), &c, sizeof(c)), 0) << strerror(errno);
      }
      break;
    }
  }
}

std::string blockedIOParamsToString(const ::testing::TestParamInfo<blockedIOParams> info) {
  // NB: this is a freestanding function because structured binding declarations are not allowed in
  // lambdas.
  auto const& [ioMethod, closeTarget, lingerEnabled] = info.param;
  std::stringstream s;
  s << "close" << CloseTargetToString(closeTarget) << "Linger";
  if (lingerEnabled) {
    s << "Foreground";
  } else {
    s << "Background";
  }
  s << "During" << ioMethod.IOMethodToString();

  return s.str();
}

INSTANTIATE_TEST_SUITE_P(
    NetStreamTest, BlockedIOTest,
    ::testing::Combine(::testing::Values(IOMethod::Op::READ, IOMethod::Op::READV,
                                         IOMethod::Op::RECV, IOMethod::Op::RECVFROM,
                                         IOMethod::Op::RECVMSG, IOMethod::Op::WRITE,
                                         IOMethod::Op::WRITEV, IOMethod::Op::SEND,
                                         IOMethod::Op::SENDTO, IOMethod::Op::SENDMSG),
                       ::testing::Values(CloseTarget::CLIENT, CloseTarget::SERVER),
                       ::testing::Values(false, true)),
    blockedIOParamsToString);

// Use this routine to test blocking socket reads. On failure, this attempts to recover the blocked
// thread.
// Return value:
//      (1) actual length of read data on successful recv
//      (2) 0, when we abort a blocked recv
//      (3) -1, on failure of both of the above operations.
ssize_t asyncSocketRead(int recvfd, int sendfd, char* buf, ssize_t len, int flags,
                        struct sockaddr_in* addr, const socklen_t* addrlen, int socketType,
                        std::chrono::duration<double> timeout) {
  std::future<ssize_t> recv = std::async(std::launch::async, [recvfd, buf, len, flags]() {
    memset(buf, 0xde, len);
    return recvfrom(recvfd, buf, len, flags, nullptr, nullptr);
  });

  if (recv.wait_for(timeout) == std::future_status::ready) {
    return recv.get();
  }

  // recover the blocked receiver thread
  switch (socketType) {
    case SOCK_STREAM: {
      // shutdown() would unblock the receiver thread with recv returning 0.
      EXPECT_EQ(shutdown(recvfd, SHUT_RD), 0) << strerror(errno);
      // We do not use 'timeout' because that maybe short here. We expect to succeed and hence use a
      // known large timeout to ensure the test does not hang in case underlying code is broken.
      EXPECT_EQ(recv.wait_for(std::chrono::milliseconds(kTimeout)), std::future_status::ready);
      EXPECT_EQ(recv.get(), 0);
      break;
    }
    case SOCK_DGRAM: {
      // Send a 0 length payload to unblock the receiver.
      // This would ensure that the async-task deterministically exits before call to future`s
      // destructor. Calling close(.release()) on recvfd when the async task is blocked on recv(),
      // __does_not__ cause recv to return; this can result in undefined behavior, as the descriptor
      // can get reused. Instead of sending a valid packet to unblock the recv() task, we could call
      // shutdown(), but that returns ENOTCONN (unconnected) but still causing recv() to return.
      // shutdown() becomes unreliable for unconnected UDP sockets because, irrespective of the
      // effect of calling this call, it returns error.
      EXPECT_EQ(sendto(sendfd, nullptr, 0, 0, reinterpret_cast<struct sockaddr*>(addr), *addrlen),
                0)
          << strerror(errno);
      // We use a known large timeout for the same reason as for the above case.
      EXPECT_EQ(recv.wait_for(std::chrono::milliseconds(kTimeout)), std::future_status::ready);
      EXPECT_EQ(recv.get(), 0);
      break;
    }
    default: {
      return -1;
    }
  }
  return 0;
}

class DatagramSendTest : public ::testing::TestWithParam<IOMethod> {};

TEST_P(DatagramSendTest, SendToIPv4MappedIPv6FromIPv4) {
  auto ioMethod = GetParam();

  fbl::unique_fd fd;
  ASSERT_TRUE(fd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, 0))) << strerror(errno);

  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
  };

  ASSERT_EQ(bind(fd.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  socklen_t addrlen = sizeof(addr);
  ASSERT_EQ(getsockname(fd.get(), reinterpret_cast<struct sockaddr*>(&addr), &addrlen), 0)
      << strerror(errno);
  ASSERT_EQ(addrlen, sizeof(addr));

  struct sockaddr_in6 addr6 = {
      .sin6_family = AF_INET6,
      .sin6_port = addr.sin_port,
  };
  addr6.sin6_addr.s6_addr[10] = 0xff;
  addr6.sin6_addr.s6_addr[11] = 0xff;
  memcpy(&addr6.sin6_addr.s6_addr[12], &addr.sin_addr.s_addr, sizeof(addr.sin_addr.s_addr));

  char buf[INET6_ADDRSTRLEN];
  ASSERT_TRUE(IN6_IS_ADDR_V4MAPPED(&addr6.sin6_addr))
      << inet_ntop(addr6.sin6_family, &addr6.sin6_addr, buf, sizeof(buf));

  switch (ioMethod.Op()) {
    case IOMethod::Op::SENDTO: {
      ASSERT_EQ(sendto(fd.get(), nullptr, 0, 0, reinterpret_cast<const struct sockaddr*>(&addr6),
                       sizeof(addr6)),
                -1);
      ASSERT_EQ(errno, EAFNOSUPPORT) << strerror(errno);
      break;
    }
    case IOMethod::Op::SENDMSG: {
      struct msghdr msghdr = {
          .msg_name = &addr6,
          .msg_namelen = sizeof(addr6),
      };
      ASSERT_EQ(sendmsg(fd.get(), &msghdr, 0), -1);
      ASSERT_EQ(errno, EAFNOSUPPORT) << strerror(errno);
      break;
    }
    default: {
      FAIL() << "unexpected test variant";
      break;
    }
  }
}

TEST_P(DatagramSendTest, DatagramSend) {
  auto ioMethod = GetParam();
  fbl::unique_fd recvfd;
  ASSERT_TRUE(recvfd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, 0))) << strerror(errno);

  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
  };

  EXPECT_EQ(bind(recvfd.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  socklen_t addrlen = sizeof(addr);
  EXPECT_EQ(getsockname(recvfd.get(), reinterpret_cast<struct sockaddr*>(&addr), &addrlen), 0)
      << strerror(errno);
  EXPECT_EQ(addrlen, sizeof(addr));

  std::string msg = "hello";
  char recvbuf[32] = {};
  struct iovec iov = {
      .iov_base = msg.data(),
      .iov_len = msg.size(),
  };
  struct msghdr msghdr = {
      .msg_name = &addr,
      .msg_namelen = addrlen,
      .msg_iov = &iov,
      .msg_iovlen = 1,
  };

  fbl::unique_fd sendfd;
  ASSERT_TRUE(sendfd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, 0))) << strerror(errno);
  switch (ioMethod.Op()) {
    case IOMethod::Op::SENDTO: {
      EXPECT_EQ(sendto(sendfd.get(), msg.data(), msg.size(), 0,
                       reinterpret_cast<struct sockaddr*>(&addr), addrlen),
                (ssize_t)msg.size())
          << strerror(errno);
      break;
    }
    case IOMethod::Op::SENDMSG: {
      EXPECT_EQ(sendmsg(sendfd.get(), &msghdr, 0), (ssize_t)msg.size()) << strerror(errno);
      break;
    }
    default: {
      FAIL() << "unexpected test variant";
      break;
    }
  }
  auto expect_success_timeout = std::chrono::milliseconds(kTimeout);
  auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(asyncSocketRead(recvfd.get(), sendfd.get(), recvbuf, sizeof(recvbuf), 0, &addr,
                            &addrlen, SOCK_DGRAM, expect_success_timeout),
            (ssize_t)msg.size());
  auto success_rcv_duration = std::chrono::steady_clock::now() - start;
  EXPECT_EQ(std::string(recvbuf, msg.size()), msg);
  EXPECT_EQ(close(sendfd.release()), 0) << strerror(errno);

  // sendto/sendmsg on connected sockets does accept sockaddr input argument and
  // also lets the dest sockaddr be overridden from what was passed for connect.
  ASSERT_TRUE(sendfd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, 0))) << strerror(errno);
  EXPECT_EQ(connect(sendfd.get(), reinterpret_cast<struct sockaddr*>(&addr), addrlen), 0)
      << strerror(errno);
  switch (ioMethod.Op()) {
    case IOMethod::Op::SENDTO: {
      EXPECT_EQ(sendto(sendfd.get(), msg.data(), msg.size(), 0,
                       reinterpret_cast<struct sockaddr*>(&addr), addrlen),
                (ssize_t)msg.size())
          << strerror(errno);
      break;
    }
    case IOMethod::Op::SENDMSG: {
      EXPECT_EQ(sendmsg(sendfd.get(), &msghdr, 0), (ssize_t)msg.size()) << strerror(errno);
      break;
    }
    default: {
      FAIL() << "unexpected test variant";
      break;
    }
  }
  EXPECT_EQ(asyncSocketRead(recvfd.get(), sendfd.get(), recvbuf, sizeof(recvbuf), 0, &addr,
                            &addrlen, SOCK_DGRAM, expect_success_timeout),
            (ssize_t)msg.size());
  EXPECT_EQ(std::string(recvbuf, msg.size()), msg);

  // Test sending to an address that is different from what we're connected to.
  addr.sin_port = htons(ntohs(addr.sin_port) + 1);
  switch (ioMethod.Op()) {
    case IOMethod::Op::SENDTO: {
      EXPECT_EQ(sendto(sendfd.get(), msg.data(), msg.size(), 0,
                       reinterpret_cast<struct sockaddr*>(&addr), addrlen),
                (ssize_t)msg.size())
          << strerror(errno);
      break;
    }
    case IOMethod::Op::SENDMSG: {
      EXPECT_EQ(sendmsg(sendfd.get(), &msghdr, 0), (ssize_t)msg.size()) << strerror(errno);
      break;
    }
    default: {
      FAIL() << "unexpected test variant";
      break;
    }
  }
  // Expect blocked receiver and try to recover it by sending a packet to the
  // original connected sockaddr.
  addr.sin_port = htons(ntohs(addr.sin_port) - 1);
  // As we expect failure, to keep the recv wait time minimal, we base it on the time taken for a
  // successful recv.
  EXPECT_EQ(asyncSocketRead(recvfd.get(), sendfd.get(), recvbuf, sizeof(recvbuf), 0, &addr,
                            &addrlen, SOCK_DGRAM, success_rcv_duration * 10),
            0);

  EXPECT_EQ(close(sendfd.release()), 0) << strerror(errno);
  EXPECT_EQ(close(recvfd.release()), 0) << strerror(errno);
}

INSTANTIATE_TEST_SUITE_P(NetDatagramTest, DatagramSendTest,
                         ::testing::Values(IOMethod::Op::SENDTO, IOMethod::Op::SENDMSG),
                         [](const ::testing::TestParamInfo<IOMethod>& info) {
                           return info.param.IOMethodToString();
                         });

TEST(NetDatagramTest, DatagramConnectWrite) {
  fbl::unique_fd recvfd;
  ASSERT_TRUE(recvfd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, 0))) << strerror(errno);

  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
  };

  ASSERT_EQ(bind(recvfd.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  socklen_t addrlen = sizeof(addr);
  ASSERT_EQ(getsockname(recvfd.get(), reinterpret_cast<struct sockaddr*>(&addr), &addrlen), 0)
      << strerror(errno);

  const char msg[] = "hello";

  fbl::unique_fd sendfd;
  ASSERT_TRUE(sendfd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, 0))) << strerror(errno);
  ASSERT_EQ(connect(sendfd.get(), reinterpret_cast<struct sockaddr*>(&addr), addrlen), 0)
      << strerror(errno);
  ASSERT_EQ(write(sendfd.get(), msg, sizeof(msg)), (ssize_t)sizeof(msg)) << strerror(errno);
  ASSERT_EQ(close(sendfd.release()), 0) << strerror(errno);

  struct pollfd pfd = {
      .fd = recvfd.get(),
      .events = POLLIN,
  };
  int n = poll(&pfd, 1, kTimeout);
  ASSERT_GE(n, 0) << strerror(errno);
  ASSERT_EQ(n, 1);
  char buf[sizeof(msg) + 1] = {};
  ASSERT_EQ(read(recvfd.get(), buf, sizeof(buf)), (ssize_t)sizeof(msg)) << strerror(errno);
  ASSERT_STREQ(buf, msg);
  EXPECT_EQ(close(recvfd.release()), 0) << strerror(errno);
}

TEST(NetDatagramTest, DatagramPartialRecv) {
  fbl::unique_fd recvfd;
  ASSERT_TRUE(recvfd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, 0))) << strerror(errno);

  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
  };

  ASSERT_EQ(bind(recvfd.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  socklen_t addrlen = sizeof(addr);
  ASSERT_EQ(getsockname(recvfd.get(), reinterpret_cast<struct sockaddr*>(&addr), &addrlen), 0)
      << strerror(errno);

  const char kTestMsg[] = "hello";
  const int kTestMsgSize = sizeof(kTestMsg);

  fbl::unique_fd sendfd;
  ASSERT_TRUE(sendfd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, 0))) << strerror(errno);
  ASSERT_EQ(kTestMsgSize, sendto(sendfd.get(), kTestMsg, kTestMsgSize, 0,
                                 reinterpret_cast<sockaddr*>(&addr), addrlen));

  char recv_buf[kTestMsgSize];

  // Read only first 2 bytes of the message. recv() is expected to discard the
  // rest.
  const int kPartialReadSize = 2;

  struct iovec iov = {
      .iov_base = recv_buf,
      .iov_len = kPartialReadSize,
  };
  struct msghdr msg = {
      .msg_iov = &iov,
      .msg_iovlen = 1,
  };

  ASSERT_EQ(recvmsg(recvfd.get(), &msg, 0), kPartialReadSize);
  ASSERT_EQ(std::string(kTestMsg, kPartialReadSize), std::string(recv_buf, kPartialReadSize));
  EXPECT_EQ(MSG_TRUNC, msg.msg_flags);

  // Send the second packet.
  ASSERT_EQ(kTestMsgSize, sendto(sendfd.get(), kTestMsg, kTestMsgSize, 0,
                                 reinterpret_cast<sockaddr*>(&addr), addrlen));

  // Read the whole packet now.
  recv_buf[0] = 0;
  iov.iov_len = sizeof(recv_buf);
  ASSERT_EQ(recvmsg(recvfd.get(), &msg, 0), kTestMsgSize);
  ASSERT_EQ(std::string(kTestMsg, kTestMsgSize), std::string(recv_buf, kTestMsgSize));
  EXPECT_EQ(msg.msg_flags, 0);

  EXPECT_EQ(close(sendfd.release()), 0) << strerror(errno);
  EXPECT_EQ(close(recvfd.release()), 0) << strerror(errno);
}

TEST(NetDatagramTest, DatagramPOLLOUT) {
  fbl::unique_fd fd;
  ASSERT_TRUE(fd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, 0))) << strerror(errno);

  struct pollfd pfd = {
      .fd = fd.get(),
      .events = POLLOUT,
  };
  int n = poll(&pfd, 1, kTimeout);
  ASSERT_GE(n, 0) << strerror(errno);
  ASSERT_EQ(n, 1);

  EXPECT_EQ(close(fd.release()), 0) << strerror(errno);
}

// DatagramSendtoRecvfrom tests if UDP send automatically binds an ephemeral
// port where the receiver can responds to.
TEST(NetDatagramTest, DatagramSendtoRecvfrom) {
  fbl::unique_fd recvfd;
  ASSERT_TRUE(recvfd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, 0))) << strerror(errno);

  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
  };

  ASSERT_EQ(bind(recvfd.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  socklen_t addrlen = sizeof(addr);
  ASSERT_EQ(getsockname(recvfd.get(), reinterpret_cast<struct sockaddr*>(&addr), &addrlen), 0)
      << strerror(errno);
  ASSERT_EQ(addrlen, sizeof(addr));

  const char msg[] = "hello";

  fbl::unique_fd sendfd;
  ASSERT_TRUE(sendfd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, 0))) << strerror(errno);
  ASSERT_EQ(
      sendto(sendfd.get(), msg, sizeof(msg), 0, reinterpret_cast<struct sockaddr*>(&addr), addrlen),
      (ssize_t)sizeof(msg))
      << strerror(errno);

  char buf[sizeof(msg) + 1] = {};

  struct sockaddr_in peer;
  socklen_t peerlen = sizeof(peer);
  ASSERT_EQ(recvfrom(recvfd.get(), buf, sizeof(buf), 0, reinterpret_cast<struct sockaddr*>(&peer),
                     &peerlen),
            (ssize_t)sizeof(msg))
      << strerror(errno);
  ASSERT_EQ(peerlen, sizeof(peer));
  ASSERT_STREQ(msg, buf);

  ASSERT_EQ(
      sendto(recvfd.get(), buf, sizeof(msg), 0, reinterpret_cast<struct sockaddr*>(&peer), peerlen),
      (ssize_t)sizeof(msg))
      << strerror(errno);

  ASSERT_EQ(recvfrom(sendfd.get(), buf, sizeof(buf), 0, reinterpret_cast<struct sockaddr*>(&peer),
                     &peerlen),
            (ssize_t)sizeof(msg))
      << strerror(errno);
  ASSERT_EQ(peerlen, sizeof(peer));
  ASSERT_STREQ(msg, buf);

  char addrbuf[INET_ADDRSTRLEN], peerbuf[INET_ADDRSTRLEN];
  const char* addrstr = inet_ntop(addr.sin_family, &addr.sin_addr, addrbuf, sizeof(addrbuf));
  ASSERT_NE(addrstr, nullptr);
  const char* peerstr = inet_ntop(peer.sin_family, &peer.sin_addr, peerbuf, sizeof(peerbuf));
  ASSERT_NE(peerstr, nullptr);
  ASSERT_STREQ(peerstr, addrstr);

  ASSERT_EQ(close(sendfd.release()), 0) << strerror(errno);

  EXPECT_EQ(close(recvfd.release()), 0) << strerror(errno);
}

// DatagramSendtoRecvfromV6 tests if UDP send automatically binds an ephemeral
// port where the receiver can responds to.
TEST(NetDatagramTest, DatagramSendtoRecvfromV6) {
  fbl::unique_fd recvfd;
  ASSERT_TRUE(recvfd = fbl::unique_fd(socket(AF_INET6, SOCK_DGRAM, 0))) << strerror(errno);

  struct sockaddr_in6 addr = {
      .sin6_family = AF_INET6,
      .sin6_addr = IN6ADDR_LOOPBACK_INIT,
  };

  ASSERT_EQ(bind(recvfd.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)), 0)
      << strerror(errno);

  socklen_t addrlen = sizeof(addr);
  ASSERT_EQ(getsockname(recvfd.get(), reinterpret_cast<struct sockaddr*>(&addr), &addrlen), 0)
      << strerror(errno);
  ASSERT_EQ(addrlen, sizeof(addr));

  const char msg[] = "hello";

  fbl::unique_fd sendfd;
  ASSERT_TRUE(sendfd = fbl::unique_fd(socket(AF_INET6, SOCK_DGRAM, 0))) << strerror(errno);
  ASSERT_EQ(
      sendto(sendfd.get(), msg, sizeof(msg), 0, reinterpret_cast<struct sockaddr*>(&addr), addrlen),
      (ssize_t)sizeof(msg))
      << strerror(errno);

  char buf[sizeof(msg) + 1] = {};

  struct sockaddr_in6 peer;
  socklen_t peerlen = sizeof(peer);
  ASSERT_EQ(recvfrom(recvfd.get(), buf, sizeof(buf), 0, reinterpret_cast<struct sockaddr*>(&peer),
                     &peerlen),
            (ssize_t)sizeof(msg))
      << strerror(errno);
  ASSERT_EQ(peerlen, sizeof(peer));
  ASSERT_STREQ(msg, buf);

  ASSERT_EQ(
      sendto(recvfd.get(), buf, sizeof(msg), 0, reinterpret_cast<struct sockaddr*>(&peer), peerlen),
      (ssize_t)sizeof(msg))
      << strerror(errno);

  ASSERT_EQ(recvfrom(sendfd.get(), buf, sizeof(buf), 0, reinterpret_cast<struct sockaddr*>(&peer),
                     &peerlen),
            (ssize_t)sizeof(msg))
      << strerror(errno);
  ASSERT_EQ(peerlen, sizeof(peer));
  ASSERT_STREQ(msg, buf);

  char addrbuf[INET6_ADDRSTRLEN], peerbuf[INET6_ADDRSTRLEN];
  const char* addrstr = inet_ntop(addr.sin6_family, &addr.sin6_addr, addrbuf, sizeof(addrbuf));
  ASSERT_NE(addrstr, nullptr);
  const char* peerstr = inet_ntop(peer.sin6_family, &peer.sin6_addr, peerbuf, sizeof(peerbuf));
  ASSERT_NE(peerstr, nullptr);
  ASSERT_STREQ(peerstr, addrstr);

  ASSERT_EQ(close(sendfd.release()), 0) << strerror(errno);

  EXPECT_EQ(close(recvfd.release()), 0) << strerror(errno);
}

TEST(NetDatagramTest, ConnectUnspecV4) {
  fbl::unique_fd fd;
  ASSERT_TRUE(fd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP))) << strerror(errno);

  struct sockaddr_in addr = {
      .sin_family = AF_UNSPEC,
  };

  EXPECT_EQ(connect(fd.get(), reinterpret_cast<const struct sockaddr*>(&addr),
                    offsetof(sockaddr_in, sin_family) + sizeof(addr.sin_family)),
            0)
      << strerror(errno);
  ASSERT_EQ(close(fd.release()), 0) << strerror(errno);
}

TEST(NetDatagramTest, ConnectUnspecV6) {
  fbl::unique_fd fd;
  ASSERT_TRUE(fd = fbl::unique_fd(socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP))) << strerror(errno);

  struct sockaddr_in6 addr = {
      .sin6_family = AF_UNSPEC,
  };

  EXPECT_EQ(connect(fd.get(), reinterpret_cast<const struct sockaddr*>(&addr),
                    offsetof(sockaddr_in6, sin6_family) + sizeof(addr.sin6_family)),
            0)
      << strerror(errno);
  ASSERT_EQ(close(fd.release()), 0) << strerror(errno);
}

// Note: we choose 100 because the max number of fds per process is limited to
// 256.
const int32_t kListeningSockets = 100;

TEST(NetStreamTest, MultipleListeningSockets) {
  fbl::unique_fd listenfds[kListeningSockets];
  fbl::unique_fd connfd[kListeningSockets];

  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
  };
  socklen_t addrlen = sizeof(addr);

  for (auto& listenfd : listenfds) {
    ASSERT_TRUE(listenfd = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);

    ASSERT_EQ(bind(listenfd.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)),
              0)
        << strerror(errno);

    ASSERT_EQ(listen(listenfd.get(), 1), 0) << strerror(errno);
  }

  for (int i = 0; i < kListeningSockets; i++) {
    ASSERT_EQ(getsockname(listenfds[i].get(), reinterpret_cast<struct sockaddr*>(&addr), &addrlen),
              0)
        << strerror(errno);
    ASSERT_EQ(addrlen, sizeof(addr));

    ASSERT_TRUE(connfd[i] = fbl::unique_fd(socket(AF_INET, SOCK_STREAM, 0))) << strerror(errno);

    ASSERT_EQ(
        connect(connfd[i].get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)), 0)
        << strerror(errno);
  }

  for (int i = 0; i < kListeningSockets; i++) {
    ASSERT_EQ(0, close(connfd[i].release()));
    ASSERT_EQ(0, close(listenfds[i].release()));
  }
}

// Socket tests across multiple socket-types, SOCK_DGRAM, SOCK_STREAM.
class NetSocketTest : public ::testing::TestWithParam<int> {};

// Test MSG_PEEK
// MSG_PEEK : Peek into the socket receive queue without moving the contents from it.
//
// TODO(fxbug.dev/33100): change this test to use recvmsg instead of recvfrom to exercise MSG_PEEK
// with scatter/gather.
TEST_P(NetSocketTest, SocketPeekTest) {
  int socketType = GetParam();
  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
  };
  socklen_t addrlen = sizeof(addr);
  fbl::unique_fd sendfd;
  fbl::unique_fd recvfd;
  ssize_t expectReadLen = 0;
  char sendbuf[8] = {};
  char recvbuf[2 * sizeof(sendbuf)] = {};
  ssize_t sendlen = sizeof(sendbuf);

  ASSERT_TRUE(sendfd = fbl::unique_fd(socket(AF_INET, socketType, 0))) << strerror(errno);
  // Setup the sender and receiver sockets.
  switch (socketType) {
    case SOCK_STREAM: {
      fbl::unique_fd acptfd;
      ASSERT_TRUE(acptfd = fbl::unique_fd(socket(AF_INET, socketType, 0))) << strerror(errno);
      EXPECT_EQ(bind(acptfd.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)),
                0)
          << strerror(errno);
      EXPECT_EQ(getsockname(acptfd.get(), reinterpret_cast<struct sockaddr*>(&addr), &addrlen), 0)
          << strerror(errno);
      EXPECT_EQ(addrlen, sizeof(addr));
      EXPECT_EQ(listen(acptfd.get(), 1), 0) << strerror(errno);
      EXPECT_EQ(
          connect(sendfd.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)), 0)
          << strerror(errno);
      ASSERT_TRUE(recvfd = fbl::unique_fd(accept(acptfd.get(), nullptr, nullptr)))
          << strerror(errno);
      EXPECT_EQ(close(acptfd.release()), 0) << strerror(errno);
      // Expect to read both the packets in a single recv() call.
      expectReadLen = sizeof(recvbuf);
      break;
    }
    case SOCK_DGRAM: {
      ASSERT_TRUE(recvfd = fbl::unique_fd(socket(AF_INET, socketType, 0))) << strerror(errno);
      EXPECT_EQ(bind(recvfd.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)),
                0)
          << strerror(errno);
      EXPECT_EQ(getsockname(recvfd.get(), reinterpret_cast<struct sockaddr*>(&addr), &addrlen), 0)
          << strerror(errno);
      EXPECT_EQ(addrlen, sizeof(addr));
      // Expect to read single packet per recv() call.
      expectReadLen = sizeof(sendbuf);
      break;
    }
    default: {
      FAIL() << "unexpected test variant";
    }
  }

  // This test sends 2 packets with known values and validates MSG_PEEK across the 2 packets.
  sendbuf[0] = 0x56;
  sendbuf[6] = 0x78;

  // send 2 separate packets and test peeking across
  EXPECT_EQ(sendto(sendfd.get(), sendbuf, sizeof(sendbuf), 0,
                   reinterpret_cast<const struct sockaddr*>(&addr), addrlen),
            sendlen)
      << strerror(errno);
  EXPECT_EQ(sendto(sendfd.get(), sendbuf, sizeof(sendbuf), 0,
                   reinterpret_cast<const struct sockaddr*>(&addr), addrlen),
            sendlen)
      << strerror(errno);

  auto expect_success_timeout = std::chrono::milliseconds(kTimeout);
  auto start = std::chrono::steady_clock::now();
  // First peek on first byte.
  EXPECT_EQ(asyncSocketRead(recvfd.get(), sendfd.get(), recvbuf, 1, MSG_PEEK, &addr, &addrlen,
                            socketType, expect_success_timeout),
            1);
  auto success_rcv_duration = std::chrono::steady_clock::now() - start;
  EXPECT_EQ(recvbuf[0], sendbuf[0]);

  // Second peek across first 2 packets and drain them from the socket receive queue.
  // Toggle the flags to MSG_PEEK every other iteration.
  ssize_t torecv = sizeof(recvbuf);
  for (int i = 0; torecv > 0; i++) {
    int flags = i % 2 ? 0 : MSG_PEEK;
    ssize_t readLen = 0;
    EXPECT_EQ(readLen = asyncSocketRead(recvfd.get(), sendfd.get(), recvbuf, sizeof(recvbuf), flags,
                                        &addr, &addrlen, socketType, expect_success_timeout),
              expectReadLen);
    if (HasFailure()) {
      break;
    }
    EXPECT_EQ(recvbuf[0], sendbuf[0]);
    EXPECT_EQ(recvbuf[6], sendbuf[6]);
    // For SOCK_STREAM, we validate peek across 2 packets with a single recv call.
    if (readLen == sizeof(recvbuf)) {
      EXPECT_EQ(recvbuf[8], sendbuf[0]);
      EXPECT_EQ(recvbuf[14], sendbuf[6]);
    }
    if (flags != MSG_PEEK) {
      torecv -= readLen;
    }
  }

  // Third peek on empty socket receive buffer, expect failure.
  //
  // As we expect failure, to keep the recv wait time minimal, we base it on the time taken for a
  // successful recv.
  EXPECT_EQ(asyncSocketRead(recvfd.get(), sendfd.get(), recvbuf, 1, MSG_PEEK, &addr, &addrlen,
                            socketType, success_rcv_duration * 10),
            0);
  EXPECT_EQ(close(recvfd.release()), 0) << strerror(errno);
  EXPECT_EQ(close(sendfd.release()), 0) << strerror(errno);
}

INSTANTIATE_TEST_SUITE_P(NetSocket, NetSocketTest, ::testing::Values(SOCK_DGRAM, SOCK_STREAM));

TEST_P(SocketKindTest, IoctlInterfaceLookupRoundTrip) {
  fbl::unique_fd fd;
  ASSERT_TRUE(fd = NewSocket()) << strerror(errno);

  // This test assumes index 1 is bound to a valid interface. In Fuchsia's test environment (the
  // component executing this test), 1 is always bound to "lo".
  struct ifreq ifr_iton = {
      .ifr_ifindex = 1,
  };
  // Set ifr_name to random chars to test ioctl correctly sets null terminator.
  memset(ifr_iton.ifr_name, 0xde, IFNAMSIZ);
  ASSERT_EQ(strnlen(ifr_iton.ifr_name, IFNAMSIZ), (size_t)IFNAMSIZ);
  ASSERT_EQ(ioctl(fd.get(), SIOCGIFNAME, &ifr_iton), 0) << strerror(errno);
  ASSERT_LT(strnlen(ifr_iton.ifr_name, IFNAMSIZ), (size_t)IFNAMSIZ);

  struct ifreq ifr_ntoi;
  strncpy(ifr_ntoi.ifr_name, ifr_iton.ifr_name, IFNAMSIZ);
  ASSERT_EQ(ioctl(fd.get(), SIOCGIFINDEX, &ifr_ntoi), 0) << strerror(errno);
  EXPECT_EQ(ifr_ntoi.ifr_ifindex, 1);

  struct ifreq ifr_err;
  memset(ifr_err.ifr_name, 0xde, IFNAMSIZ);
  // Although the first few bytes of ifr_name contain the correct name, there is no null terminator
  // and the remaining bytes are gibberish, should match no interfaces.
  memcpy(ifr_err.ifr_name, ifr_iton.ifr_name, strnlen(ifr_iton.ifr_name, IFNAMSIZ));

  struct ioctl_request {
    std::string name;
    uint64_t request;
  };
  const ioctl_request requests[] = {
      {
          .name = "SIOCGIFINDEX",
          .request = SIOCGIFINDEX,
      },
      {
          .name = "SIOCGIFFLAGS",
          .request = SIOCGIFFLAGS,
      },
  };
  for (const auto& request : requests) {
    ASSERT_EQ(ioctl(fd.get(), request.request, &ifr_err), -1) << request.name;
    EXPECT_EQ(errno, ENODEV) << request.name << ": " << strerror(errno);
  }
}

TEST_P(SocketKindTest, IoctlInterfaceNotFound) {
  fbl::unique_fd fd;
  ASSERT_TRUE(fd = NewSocket()) << strerror(errno);

  // Invalid ifindex "-1" should match no interfaces.
  struct ifreq ifr_iton = {
      .ifr_ifindex = -1,
  };
  ASSERT_EQ(ioctl(fd.get(), SIOCGIFNAME, &ifr_iton), -1);
  EXPECT_EQ(errno, ENODEV) << strerror(errno);

  // Empty name should match no interface.
  struct ifreq ifr = {
      .ifr_name = 0,
  };
  struct ioctl_request {
    std::string name;
    uint64_t request;
  };
  const ioctl_request requests[] = {
      {
          .name = "SIOCGIFINDEX",
          .request = SIOCGIFINDEX,
      },
      {
          .name = "SIOCGIFFLAGS",
          .request = SIOCGIFFLAGS,
      },
  };
  for (const auto& request : requests) {
    ASSERT_EQ(ioctl(fd.get(), request.request, &ifr), -1) << request.name;
    EXPECT_EQ(errno, ENODEV) << request.name << ": " << strerror(errno);
  }
}

TEST(SocketKindTest, IoctlLookupForNonSocketFd) {
  fbl::unique_fd fd;
  ASSERT_TRUE(fd = fbl::unique_fd(open("/", O_RDONLY | O_DIRECTORY))) << strerror(errno);

  struct ifreq ifr_iton = {
      .ifr_ifindex = 1,
  };
  ASSERT_EQ(ioctl(fd.get(), SIOCGIFNAME, &ifr_iton), -1);
  EXPECT_EQ(errno, ENOTTY) << strerror(errno);

  struct ifreq ifr;
  strcpy(ifr.ifr_name, "loblah");
  struct ioctl_request {
    std::string name;
    uint64_t request;
  };
  const ioctl_request requests[] = {
      {
          .name = "SIOCGIFINDEX",
          .request = SIOCGIFINDEX,
      },
      {
          .name = "SIOCGIFFLAGS",
          .request = SIOCGIFFLAGS,
      },
  };
  for (const auto& request : requests) {
    ASSERT_EQ(ioctl(fd.get(), request.request, &ifr), -1) << request.name;
    EXPECT_EQ(errno, ENOTTY) << request.name << ": " << strerror(errno);
  }
}

TEST(IoctlTest, IoctlGetInterfaceFlags) {
  fbl::unique_fd fd;
  ASSERT_TRUE(fd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, 0))) << strerror(errno);

  struct ifreq ifr_ntof = {
      .ifr_name = "lo",
  };
  ASSERT_EQ(ioctl(fd.get(), SIOCGIFFLAGS, &ifr_ntof), 0) << strerror(errno);
  struct expected_flag {
    std::string name;
    uint16_t bitmask;
    bool value;
  };
  const expected_flag flags[] = {
      {
          .name = "IFF_UP",
          .bitmask = IFF_UP,
          .value = true,
      },
      {
          .name = "IFF_LOOPBACK",
          .bitmask = IFF_LOOPBACK,
          .value = true,
      },
      {
          .name = "IFF_RUNNING",
          .bitmask = IFF_RUNNING,
          .value = true,
      },
      {
          .name = "IFF_PROMISC",
          .bitmask = IFF_PROMISC,
          .value = false,
      },
  };
  for (const auto& flag : flags) {
    EXPECT_EQ(static_cast<bool>(ifr_ntof.ifr_flags & flag.bitmask), flag.value)
        << std::bitset<16>(ifr_ntof.ifr_flags) << ", " << std::bitset<16>(flag.bitmask);
  }
  // Don't check strict equality of `ifr_ntof.ifr_flags` with expected flag
  // values, except on Fuchsia, because gVisor does not set all the interface
  // flags that Linux does.
#if defined(__Fuchsia__)
  uint16_t expected_flags = IFF_UP | IFF_LOOPBACK | IFF_RUNNING | IFF_MULTICAST;
  ASSERT_EQ(ifr_ntof.ifr_flags, expected_flags)
      << std::bitset<16>(ifr_ntof.ifr_flags) << ", " << std::bitset<16>(expected_flags);
#endif
}

TEST(IoctlTest, IoctlGetInterfaceAddressesNullIfConf) {
  fbl::unique_fd fd;
  ASSERT_TRUE(fd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, 0))) << strerror(errno);

  ASSERT_EQ(ioctl(fd.get(), SIOCGIFCONF, nullptr), -1);
  ASSERT_EQ(errno, EFAULT) << strerror(errno);
}

TEST(IoctlTest, IoctlGetInterfaceAddressesPartialRecord) {
  fbl::unique_fd fd;
  ASSERT_TRUE(fd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, 0))) << strerror(errno);

  // Get the interface configuration information, but only pass an `ifc_len`
  // large enough to hold a partial `struct ifreq`, and ensure that the buffer
  // is not overwritten.
  const char FILLER = 0xa;
  struct ifreq ifr;
  memset(&ifr, FILLER, sizeof(ifr));
  struct ifconf ifc = {
      .ifc_len = sizeof(ifr) - 1,
      .ifc_req = &ifr,
  };

  ASSERT_EQ(ioctl(fd.get(), SIOCGIFCONF, &ifc), 0) << strerror(errno);
  ASSERT_EQ(ifc.ifc_len, 0);
  char* buffer = reinterpret_cast<char*>(&ifr);
  for (size_t i = 0; i < sizeof(ifr); i++) {
    EXPECT_EQ(buffer[i], FILLER) << i;
  }
}

INSTANTIATE_TEST_SUITE_P(NetSocket, SocketKindTest,
                         ::testing::Combine(::testing::Values(AF_INET, AF_INET6),
                                            ::testing::Values(SOCK_DGRAM, SOCK_STREAM)),
                         socketKindToString);

using DomainProtocol = std::tuple<int, int>;
class IcmpSocketTest : public ::testing::TestWithParam<DomainProtocol> {};

TEST_P(IcmpSocketTest, GetSockoptSoProtocol) {
#if !defined(__Fuchsia__)
  if (!IsRoot()) {
    GTEST_SKIP() << "This test requires root";
  }
#endif
  auto const& [domain, protocol] = GetParam();

  fbl::unique_fd fd;
  ASSERT_TRUE(fd = fbl::unique_fd(socket(domain, SOCK_DGRAM, protocol))) << strerror(errno);

  int opt;
  socklen_t optlen = sizeof(opt);
  EXPECT_EQ(getsockopt(fd.get(), SOL_SOCKET, SO_PROTOCOL, &opt, &optlen), 0) << strerror(errno);
  EXPECT_EQ(opt, protocol);
}

INSTANTIATE_TEST_SUITE_P(NetSocket, IcmpSocketTest,
                         ::testing::Values(std::make_pair(AF_INET, IPPROTO_ICMP),
                                           std::make_pair(AF_INET6, IPPROTO_ICMPV6)));

TEST(NetDatagramTest, PingIpv4LoopbackAddresses) {
  const char msg[] = "hello";
  char addrbuf[INET_ADDRSTRLEN];
  std::array<int, 5> sampleAddrOctets = {0, 1, 100, 200, 255};
  for (auto i : sampleAddrOctets) {
    for (auto j : sampleAddrOctets) {
      for (auto k : sampleAddrOctets) {
        // Skip the subnet and broadcast addresses.
        if ((i == 0 && j == 0 && k == 0) || (i == 255 && j == 255 && k == 255)) {
          continue;
        }
        // loopback_addr = 127.i.j.k
        struct in_addr loopback_sin_addr = {
            .s_addr = htonl((127 << 24) + (i << 16) + (j << 8) + k),
        };
        const char* loopback_addrstr =
            inet_ntop(AF_INET, &loopback_sin_addr, addrbuf, sizeof(addrbuf));
        ASSERT_NE(nullptr, loopback_addrstr);

        fbl::unique_fd recvfd;
        ASSERT_TRUE(recvfd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, 0))) << strerror(errno);
        struct sockaddr_in rcv_addr = {
            .sin_family = AF_INET,
            .sin_addr = loopback_sin_addr,
        };
        ASSERT_EQ(bind(recvfd.get(), reinterpret_cast<const struct sockaddr*>(&rcv_addr),
                       sizeof(rcv_addr)),
                  0)
            << "recvaddr=" << loopback_addrstr << ": " << strerror(errno);

        socklen_t rcv_addrlen = sizeof(rcv_addr);
        ASSERT_EQ(
            getsockname(recvfd.get(), reinterpret_cast<struct sockaddr*>(&rcv_addr), &rcv_addrlen),
            0)
            << strerror(errno);
        ASSERT_EQ(sizeof(rcv_addr), rcv_addrlen);

        fbl::unique_fd sendfd;
        ASSERT_TRUE(sendfd = fbl::unique_fd(socket(AF_INET, SOCK_DGRAM, 0))) << strerror(errno);
        struct sockaddr_in sendto_addr = {
            .sin_family = AF_INET,
            .sin_port = rcv_addr.sin_port,
            .sin_addr = loopback_sin_addr,
        };
        ASSERT_EQ(sendto(sendfd.get(), msg, sizeof(msg), 0,
                         reinterpret_cast<struct sockaddr*>(&sendto_addr), sizeof(sendto_addr)),
                  (ssize_t)sizeof(msg))
            << "sendtoaddr=" << loopback_addrstr << ": " << strerror(errno);
        EXPECT_EQ(close(sendfd.release()), 0) << strerror(errno);

        struct pollfd pfd = {
            .fd = recvfd.get(),
            .events = POLLIN,
        };
        int n = poll(&pfd, 1, kTimeout);
        ASSERT_GE(n, 0) << strerror(errno);
        ASSERT_EQ(n, 1);
        char buf[sizeof(msg) + 1] = {};
        ASSERT_EQ(read(recvfd.get(), buf, sizeof(buf)), (ssize_t)sizeof(msg)) << strerror(errno);
        ASSERT_STREQ(buf, msg);

        EXPECT_EQ(close(recvfd.release()), 0) << strerror(errno);
      }
    }
  }
}

}  // namespace
