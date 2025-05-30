// Copyright 2018 The gVisor Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <netinet/tcp.h>
#include <sys/socket.h>

#include <cstring>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "test/syscalls/linux/socket_inet_loopback_test_params.h"
#include "test/util/capability_util.h"
#include "test/util/save_util.h"
#include "test/util/socket_util.h"
#include "test/util/test_util.h"

// Unit tests in this file will run in their own network namespace.

namespace gvisor {
namespace testing {

namespace {

using SocketInetLoopbackIsolatedTest =
    ::testing::TestWithParam<SocketInetTestParam>;

TEST_P(SocketInetLoopbackIsolatedTest, TCPActiveCloseTimeWaitTest) {
  SocketInetTestParam const& param = GetParam();
  sockaddr_storage listen_addr, conn_bound_addr;
  listen_addr = param.listener.addr;
  SetupTimeWaitClose(&param.listener, &param.connector, false /*reuse*/,
                     false /*accept_close*/, &listen_addr, &conn_bound_addr);
  FileDescriptor conn_fd = ASSERT_NO_ERRNO_AND_VALUE(
      Socket(param.connector.family(), SOCK_STREAM, IPPROTO_TCP));

  ASSERT_THAT(bind(conn_fd.get(), AsSockAddr(&conn_bound_addr),
                   param.connector.addr_len),
              SyscallFailsWithErrno(EADDRINUSE));
}

TEST_P(SocketInetLoopbackIsolatedTest, TCPActiveCloseTimeWaitReuseTest) {
  SocketInetTestParam const& param = GetParam();
  sockaddr_storage listen_addr, conn_bound_addr;
  listen_addr = param.listener.addr;
  SetupTimeWaitClose(&param.listener, &param.connector, true /*reuse*/,
                     false /*accept_close*/, &listen_addr, &conn_bound_addr);
  FileDescriptor conn_fd = ASSERT_NO_ERRNO_AND_VALUE(
      Socket(param.connector.family(), SOCK_STREAM, IPPROTO_TCP));
  ASSERT_THAT(setsockopt(conn_fd.get(), SOL_SOCKET, SO_REUSEADDR, &kSockOptOn,
                         sizeof(kSockOptOn)),
              SyscallSucceeds());
  ASSERT_THAT(bind(conn_fd.get(), AsSockAddr(&conn_bound_addr),
                   param.connector.addr_len),
              SyscallFailsWithErrno(EADDRINUSE));
}

// These tests are disabled under random save as the restore run
// results in the stack.Seed() being different which can cause
// sequence number of final connect to be one that is considered
// old and can cause the test to be flaky.
//
// Test re-binding of client and server bound addresses when the older
// connection is in TIME_WAIT.
TEST_P(SocketInetLoopbackIsolatedTest, TCPPassiveCloseNoTimeWaitTest) {
  SocketInetTestParam const& param = GetParam();
  sockaddr_storage listen_addr, conn_bound_addr;
  listen_addr = param.listener.addr;
  SetupTimeWaitClose(&param.listener, &param.connector, false /*reuse*/,
                     true /*accept_close*/, &listen_addr, &conn_bound_addr);

  // Now bind a new socket and verify that we can immediately rebind the address
  // bound by the conn_fd as it never entered TIME_WAIT.
  const FileDescriptor conn_fd = ASSERT_NO_ERRNO_AND_VALUE(
      Socket(param.connector.family(), SOCK_STREAM, IPPROTO_TCP));
  ASSERT_THAT(bind(conn_fd.get(), AsSockAddr(&conn_bound_addr),
                   param.connector.addr_len),
              SyscallSucceeds());

  FileDescriptor listen_fd = ASSERT_NO_ERRNO_AND_VALUE(
      Socket(param.listener.family(), SOCK_STREAM, IPPROTO_TCP));
  ASSERT_THAT(
      bind(listen_fd.get(), AsSockAddr(&listen_addr), param.listener.addr_len),
      SyscallFailsWithErrno(EADDRINUSE));
}

TEST_P(SocketInetLoopbackIsolatedTest, TCPPassiveCloseNoTimeWaitReuseTest) {
  SocketInetTestParam const& param = GetParam();
  sockaddr_storage listen_addr, conn_bound_addr;
  listen_addr = param.listener.addr;
  SetupTimeWaitClose(&param.listener, &param.connector, true /*reuse*/,
                     true /*accept_close*/, &listen_addr, &conn_bound_addr);

  FileDescriptor listen_fd = ASSERT_NO_ERRNO_AND_VALUE(
      Socket(param.listener.family(), SOCK_STREAM, IPPROTO_TCP));
  ASSERT_THAT(setsockopt(listen_fd.get(), SOL_SOCKET, SO_REUSEADDR, &kSockOptOn,
                         sizeof(kSockOptOn)),
              SyscallSucceeds());
  ASSERT_THAT(
      bind(listen_fd.get(), AsSockAddr(&listen_addr), param.listener.addr_len),
      SyscallSucceeds());
  ASSERT_THAT(listen(listen_fd.get(), SOMAXCONN), SyscallSucceeds());

  // Now bind and connect  new socket and verify that we can immediately rebind
  // the address bound by the conn_fd as it never entered TIME_WAIT.
  const FileDescriptor conn_fd = ASSERT_NO_ERRNO_AND_VALUE(
      Socket(param.connector.family(), SOCK_STREAM, IPPROTO_TCP));
  ASSERT_THAT(setsockopt(conn_fd.get(), SOL_SOCKET, SO_REUSEADDR, &kSockOptOn,
                         sizeof(kSockOptOn)),
              SyscallSucceeds());
  ASSERT_THAT(bind(conn_fd.get(), AsSockAddr(&conn_bound_addr),
                   param.connector.addr_len),
              SyscallSucceeds());

  uint16_t const port =
      ASSERT_NO_ERRNO_AND_VALUE(AddrPort(param.listener.family(), listen_addr));
  sockaddr_storage conn_addr = param.connector.addr;
  ASSERT_NO_ERRNO(SetAddrPort(param.connector.family(), &conn_addr, port));
  ASSERT_THAT(RetryEINTR(connect)(conn_fd.get(), AsSockAddr(&conn_addr),
                                  param.connector.addr_len),
              SyscallSucceeds());
}

// TCPFinWait2Test creates a pair of connected sockets then closes one end to
// trigger FIN_WAIT2 state for the closed endpoint. Then it binds the same local
// IP/port on a new socket and tries to connect. The connect should fail w/
// an EADDRINUSE. Then we wait till the FIN_WAIT2 timeout is over and try the
// bind/connect again with a new socket and this time it should succeed.
//
// TCP timers are not S/R today, this can cause this test to be flaky when run
// under random S/R due to timer being reset on a restore.
TEST_P(SocketInetLoopbackIsolatedTest, TCPFinWait2Test) {
  SocketInetTestParam const& param = GetParam();
  TestAddress const& listener = param.listener;
  TestAddress const& connector = param.connector;

  // Disable cooperative saves after this point. As a save between the first
  // bind/connect and the second one can cause the linger timeout timer to
  // be restarted causing the final bind/connect to fail.
  DisableSave ds;

  // Create the listening socket.
  const FileDescriptor listen_fd = ASSERT_NO_ERRNO_AND_VALUE(
      Socket(listener.family(), SOCK_STREAM, IPPROTO_TCP));
  sockaddr_storage listen_addr = listener.addr;
  ASSERT_THAT(
      bind(listen_fd.get(), AsSockAddr(&listen_addr), listener.addr_len),
      SyscallSucceeds());
  ASSERT_THAT(listen(listen_fd.get(), SOMAXCONN), SyscallSucceeds());

  // Get the port bound by the listening socket.
  socklen_t addrlen = listener.addr_len;
  ASSERT_THAT(getsockname(listen_fd.get(), AsSockAddr(&listen_addr), &addrlen),
              SyscallSucceeds());

  uint16_t const port =
      ASSERT_NO_ERRNO_AND_VALUE(AddrPort(listener.family(), listen_addr));

  // Connect to the listening socket.
  FileDescriptor conn_fd = ASSERT_NO_ERRNO_AND_VALUE(
      Socket(connector.family(), SOCK_STREAM, IPPROTO_TCP));

  // Lower FIN_WAIT2 state to 5 seconds for test.
  constexpr int kTCPLingerTimeout = 5;
  EXPECT_THAT(setsockopt(conn_fd.get(), IPPROTO_TCP, TCP_LINGER2,
                         &kTCPLingerTimeout, sizeof(kTCPLingerTimeout)),
              SyscallSucceedsWithValue(0));

  sockaddr_storage conn_addr = connector.addr;
  ASSERT_NO_ERRNO(SetAddrPort(connector.family(), &conn_addr, port));
  ASSERT_THAT(RetryEINTR(connect)(conn_fd.get(), AsSockAddr(&conn_addr),
                                  connector.addr_len),
              SyscallSucceeds());

  // Accept the connection.
  auto accepted =
      ASSERT_NO_ERRNO_AND_VALUE(Accept(listen_fd.get(), nullptr, nullptr));

  // Get the address/port bound by the connecting socket.
  sockaddr_storage conn_bound_addr;
  socklen_t conn_addrlen = connector.addr_len;
  ASSERT_THAT(
      getsockname(conn_fd.get(), AsSockAddr(&conn_bound_addr), &conn_addrlen),
      SyscallSucceeds());

  // close the connecting FD to trigger FIN_WAIT2  on the connected fd.
  conn_fd.reset();

  // Now bind and connect a new socket.
  const FileDescriptor conn_fd2 = ASSERT_NO_ERRNO_AND_VALUE(
      Socket(connector.family(), SOCK_STREAM, IPPROTO_TCP));

  ASSERT_THAT(bind(conn_fd2.get(), AsSockAddr(&conn_bound_addr), conn_addrlen),
              SyscallFailsWithErrno(EADDRINUSE));

  // Sleep for a little over the linger timeout to reduce flakiness in
  // save/restore tests.
  absl::SleepFor(absl::Seconds(kTCPLingerTimeout + 2));

  ASSERT_THAT(bind(conn_fd2.get(), AsSockAddr(&conn_bound_addr), conn_addrlen),
              SyscallSucceeds());

  // Close the `accepted` end otherwise connect can return ECONNREFUSED.
  constexpr int kTCPLingerTimeout0 = 0;
  EXPECT_THAT(setsockopt(accepted.get(), IPPROTO_TCP, TCP_LINGER2,
                         &kTCPLingerTimeout0, sizeof(kTCPLingerTimeout0)),
              SyscallSucceedsWithValue(0));
  shutdown(accepted.get(), SHUT_WR);

  ASSERT_THAT(
      RetryEINTR(connect)(conn_fd2.get(), AsSockAddr(&conn_addr), conn_addrlen),
      SyscallSucceeds());
}

// TCPLinger2TimeoutAfterClose creates a pair of connected sockets
// then closes one end to trigger FIN_WAIT2 state for the closed endpoint.
// It then sleeps for the TCP_LINGER2 timeout and verifies that bind/
// connecting the same address succeeds.
//
// TCP timers are not S/R today, this can cause this test to be flaky when run
// under random S/R due to timer being reset on a restore.
TEST_P(SocketInetLoopbackIsolatedTest, TCPLinger2TimeoutAfterClose) {
  SocketInetTestParam const& param = GetParam();
  TestAddress const& listener = param.listener;
  TestAddress const& connector = param.connector;

  // Create the listening socket.
  const FileDescriptor listen_fd = ASSERT_NO_ERRNO_AND_VALUE(
      Socket(listener.family(), SOCK_STREAM, IPPROTO_TCP));
  sockaddr_storage listen_addr = listener.addr;
  ASSERT_THAT(
      bind(listen_fd.get(), AsSockAddr(&listen_addr), listener.addr_len),
      SyscallSucceeds());
  ASSERT_THAT(listen(listen_fd.get(), SOMAXCONN), SyscallSucceeds());

  // Get the port bound by the listening socket.
  socklen_t addrlen = listener.addr_len;
  ASSERT_THAT(getsockname(listen_fd.get(), AsSockAddr(&listen_addr), &addrlen),
              SyscallSucceeds());

  uint16_t const port =
      ASSERT_NO_ERRNO_AND_VALUE(AddrPort(listener.family(), listen_addr));

  // Connect to the listening socket.
  FileDescriptor conn_fd = ASSERT_NO_ERRNO_AND_VALUE(
      Socket(connector.family(), SOCK_STREAM, IPPROTO_TCP));

  sockaddr_storage conn_addr = connector.addr;
  ASSERT_NO_ERRNO(SetAddrPort(connector.family(), &conn_addr, port));
  ASSERT_THAT(RetryEINTR(connect)(conn_fd.get(), AsSockAddr(&conn_addr),
                                  connector.addr_len),
              SyscallSucceeds());

  // Accept the connection.
  auto accepted =
      ASSERT_NO_ERRNO_AND_VALUE(Accept(listen_fd.get(), nullptr, nullptr));

  // Get the address/port bound by the connecting socket.
  sockaddr_storage conn_bound_addr;
  socklen_t conn_addrlen = connector.addr_len;
  ASSERT_THAT(
      getsockname(conn_fd.get(), AsSockAddr(&conn_bound_addr), &conn_addrlen),
      SyscallSucceeds());

  // Disable cooperative saves after this point as TCP timers are not restored
  // across a S/R.
  {
    DisableSave ds;
    constexpr int kTCPLingerTimeout = 4;
    EXPECT_THAT(setsockopt(conn_fd.get(), IPPROTO_TCP, TCP_LINGER2,
                           &kTCPLingerTimeout, sizeof(kTCPLingerTimeout)),
                SyscallSucceedsWithValue(0));

    // close the connecting FD to trigger FIN_WAIT2  on the connected fd.
    conn_fd.reset();

    absl::SleepFor(absl::Seconds(kTCPLingerTimeout + 2));

    // ds going out of scope will Re-enable S/R's since at this point the timer
    // must have fired and cleaned up the endpoint.
  }

  // Now bind and connect a new socket and verify that we can immediately
  // rebind the address bound by the conn_fd as it never entered TIME_WAIT.
  const FileDescriptor conn_fd2 = ASSERT_NO_ERRNO_AND_VALUE(
      Socket(connector.family(), SOCK_STREAM, IPPROTO_TCP));

  ASSERT_THAT(bind(conn_fd2.get(), AsSockAddr(&conn_bound_addr), conn_addrlen),
              SyscallSucceeds());
  ASSERT_THAT(
      RetryEINTR(connect)(conn_fd2.get(), AsSockAddr(&conn_addr), conn_addrlen),
      SyscallSucceeds());
}

TEST_P(SocketInetLoopbackIsolatedTest, TCPConnectionReuseAddrConflicts) {
  SocketInetTestParam const& param = GetParam();
  TestAddress const& listener = param.listener;
  TestAddress const& connector = param.connector;

  const FileDescriptor listen_fd =
      ASSERT_NO_ERRNO_AND_VALUE(Socket(listener.family(), SOCK_STREAM, 0));

  sockaddr_storage listen_addr = listener.addr;
  ASSERT_THAT(
      bind(listen_fd.get(), AsSockAddr(&listen_addr), listener.addr_len),
      SyscallSucceeds());
  ASSERT_THAT(listen(listen_fd.get(), SOMAXCONN), SyscallSucceeds());

  // Get the port bound by the listening socket.
  socklen_t addrlen = listener.addr_len;
  ASSERT_THAT(getsockname(listen_fd.get(), AsSockAddr(&listen_addr), &addrlen),
              SyscallSucceeds());

  const uint16_t port =
      ASSERT_NO_ERRNO_AND_VALUE(AddrPort(listener.family(), listen_addr));

  // Create a first connection.
  FileDescriptor conn_fd1 = ASSERT_NO_ERRNO_AND_VALUE(
      Socket(connector.family(), SOCK_STREAM, IPPROTO_TCP));
  ASSERT_THAT(setsockopt(conn_fd1.get(), SOL_SOCKET, SO_REUSEADDR, &kSockOptOn,
                         sizeof(kSockOptOn)),
              SyscallSucceeds());

  sockaddr_storage conn_addr = connector.addr;
  ASSERT_NO_ERRNO(SetAddrPort(connector.family(), &conn_addr, port));
  ASSERT_THAT(RetryEINTR(connect)(conn_fd1.get(), AsSockAddr(&conn_addr),
                                  connector.addr_len),
              SyscallSucceeds());
  sockaddr_storage conn_bound_addr;
  addrlen = sizeof(conn_bound_addr);
  ASSERT_THAT(
      getsockname(conn_fd1.get(), AsSockAddr(&conn_bound_addr), &addrlen),
      SyscallSucceeds());
  ASSERT_EQ(addrlen, connector.addr_len);

  // Create the second connection that is bind to the same local address as the
  // first.
  FileDescriptor conn_fd2 = ASSERT_NO_ERRNO_AND_VALUE(
      Socket(connector.family(), SOCK_STREAM, IPPROTO_TCP));
  ASSERT_THAT(setsockopt(conn_fd2.get(), SOL_SOCKET, SO_REUSEADDR, &kSockOptOn,
                         sizeof(kSockOptOn)),
              SyscallSucceeds());
  // Bind should succeed.
  ASSERT_THAT(bind(conn_fd2.get(), AsSockAddr(&conn_bound_addr), addrlen),
              SyscallSucceeds());

  // Connect should fail.
  ASSERT_NO_ERRNO(SetAddrPort(connector.family(), &conn_addr, port));
  ASSERT_THAT(RetryEINTR(connect)(conn_fd2.get(), AsSockAddr(&conn_addr),
                                  connector.addr_len),
              SyscallFailsWithErrno(EADDRNOTAVAIL));
}

INSTANTIATE_TEST_SUITE_P(All, SocketInetLoopbackIsolatedTest,
                         SocketInetLoopbackTestValues(),
                         DescribeSocketInetTestParam);

using SocketMultiProtocolInetLoopbackIsolatedTest =
    ::testing::TestWithParam<ProtocolTestParam>;

TEST_P(SocketMultiProtocolInetLoopbackIsolatedTest, BindToDeviceReusePort) {
  // setsockopt(SO_BINDTODEVICE) requires CAP_NET_RAW.
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveRawIPSocketCapability()));

  ProtocolTestParam const& param = GetParam();
  TestAddress const& test_addr = V4Loopback();

  auto socket1 =
      ASSERT_NO_ERRNO_AND_VALUE(Socket(test_addr.family(), param.type, 0));
  auto socket2 =
      ASSERT_NO_ERRNO_AND_VALUE(Socket(test_addr.family(), param.type, 0));

  const char kLoopbackDeviceName[] = "lo";

  // Bind socket1 with REUSEPORT and BINDTODEVICE.
  ASSERT_THAT(setsockopt(socket1.get(), SOL_SOCKET, SO_REUSEPORT, &kSockOptOn,
                         sizeof(kSockOptOn)),
              SyscallSucceeds());
  ASSERT_THAT(setsockopt(socket1.get(), SOL_SOCKET, SO_BINDTODEVICE,
                         kLoopbackDeviceName, strlen(kLoopbackDeviceName)),
              SyscallSucceeds());

  // Bind the first socket to the loopback and take note of the selected port.
  auto addr = V4Loopback();
  ASSERT_THAT(bind(socket1.get(), AsSockAddr(&addr.addr), addr.addr_len),
              SyscallSucceeds());
  socklen_t addr_len = addr.addr_len;
  ASSERT_THAT(getsockname(socket1.get(), AsSockAddr(&addr.addr), &addr_len),
              SyscallSucceeds());
  EXPECT_EQ(addr_len, addr.addr_len);

  // Bind socket2 to the same device and address as socket1.
  ASSERT_THAT(setsockopt(socket2.get(), SOL_SOCKET, SO_BINDTODEVICE,
                         kLoopbackDeviceName, strlen(kLoopbackDeviceName)),
              SyscallSucceeds());
  ASSERT_THAT(setsockopt(socket2.get(), SOL_SOCKET, SO_REUSEPORT, &kSockOptOn,
                         sizeof(kSockOptOn)),
              SyscallSucceeds());
  ASSERT_THAT(bind(socket2.get(), AsSockAddr(&addr.addr), addr.addr_len),
              SyscallSucceeds());
}

TEST_P(SocketMultiProtocolInetLoopbackIsolatedTest,
       V4EphemeralPortReservedReuseAddr) {
  ProtocolTestParam const& param = GetParam();

  // Bind the v4 loopback on a v4 socket.
  TestAddress const& test_addr = V4Loopback();
  sockaddr_storage bound_addr = test_addr.addr;
  const FileDescriptor bound_fd =
      ASSERT_NO_ERRNO_AND_VALUE(Socket(test_addr.family(), param.type, 0));

  ASSERT_THAT(setsockopt(bound_fd.get(), SOL_SOCKET, SO_REUSEADDR, &kSockOptOn,
                         sizeof(kSockOptOn)),
              SyscallSucceeds());

  ASSERT_THAT(bind(bound_fd.get(), AsSockAddr(&bound_addr), test_addr.addr_len),
              SyscallSucceeds());

  // Listen iff TCP.
  if (param.type == SOCK_STREAM) {
    ASSERT_THAT(listen(bound_fd.get(), SOMAXCONN), SyscallSucceeds());
  }

  // Get the port that we bound.
  socklen_t bound_addr_len = test_addr.addr_len;
  ASSERT_THAT(
      getsockname(bound_fd.get(), AsSockAddr(&bound_addr), &bound_addr_len),
      SyscallSucceeds());

  // Connect to bind an ephemeral port.
  const FileDescriptor connected_fd =
      ASSERT_NO_ERRNO_AND_VALUE(Socket(test_addr.family(), param.type, 0));

  ASSERT_THAT(setsockopt(connected_fd.get(), SOL_SOCKET, SO_REUSEADDR,
                         &kSockOptOn, sizeof(kSockOptOn)),
              SyscallSucceeds());

  ASSERT_THAT(RetryEINTR(connect)(connected_fd.get(), AsSockAddr(&bound_addr),
                                  bound_addr_len),
              SyscallSucceeds());

  // Get the ephemeral port.
  sockaddr_storage connected_addr = {};
  socklen_t connected_addr_len = sizeof(connected_addr);
  ASSERT_THAT(getsockname(connected_fd.get(), AsSockAddr(&connected_addr),
                          &connected_addr_len),
              SyscallSucceeds());
  uint16_t const ephemeral_port =
      ASSERT_NO_ERRNO_AND_VALUE(AddrPort(test_addr.family(), connected_addr));

  // Verify that we actually got an ephemeral port.
  ASSERT_NE(ephemeral_port, 0);

  // Verify that the ephemeral port is not reserved.
  const FileDescriptor checking_fd =
      ASSERT_NO_ERRNO_AND_VALUE(Socket(test_addr.family(), param.type, 0));
  ASSERT_THAT(setsockopt(checking_fd.get(), SOL_SOCKET, SO_REUSEADDR,
                         &kSockOptOn, sizeof(kSockOptOn)),
              SyscallSucceeds());
  EXPECT_THAT(
      bind(checking_fd.get(), AsSockAddr(&connected_addr), connected_addr_len),
      SyscallSucceeds());
}

TEST_P(SocketMultiProtocolInetLoopbackIsolatedTest,
       V4MappedEphemeralPortReservedReuseAddr) {
  ProtocolTestParam const& param = GetParam();

  // Bind the v4 loopback on a dual stack socket.
  TestAddress const& test_addr = V4MappedLoopback();
  sockaddr_storage bound_addr = test_addr.addr;
  const FileDescriptor bound_fd =
      ASSERT_NO_ERRNO_AND_VALUE(Socket(test_addr.family(), param.type, 0));
  ASSERT_THAT(bind(bound_fd.get(), AsSockAddr(&bound_addr), test_addr.addr_len),
              SyscallSucceeds());

  ASSERT_THAT(setsockopt(bound_fd.get(), SOL_SOCKET, SO_REUSEADDR, &kSockOptOn,
                         sizeof(kSockOptOn)),
              SyscallSucceeds());

  // Listen iff TCP.
  if (param.type == SOCK_STREAM) {
    ASSERT_THAT(listen(bound_fd.get(), SOMAXCONN), SyscallSucceeds());
  }

  // Get the port that we bound.
  socklen_t bound_addr_len = test_addr.addr_len;
  ASSERT_THAT(
      getsockname(bound_fd.get(), AsSockAddr(&bound_addr), &bound_addr_len),
      SyscallSucceeds());

  // Connect to bind an ephemeral port.
  const FileDescriptor connected_fd =
      ASSERT_NO_ERRNO_AND_VALUE(Socket(test_addr.family(), param.type, 0));
  ASSERT_THAT(setsockopt(connected_fd.get(), SOL_SOCKET, SO_REUSEADDR,
                         &kSockOptOn, sizeof(kSockOptOn)),
              SyscallSucceeds());
  ASSERT_THAT(RetryEINTR(connect)(connected_fd.get(), AsSockAddr(&bound_addr),
                                  bound_addr_len),
              SyscallSucceeds());

  // Get the ephemeral port.
  sockaddr_storage connected_addr = {};
  socklen_t connected_addr_len = sizeof(connected_addr);
  ASSERT_THAT(getsockname(connected_fd.get(), AsSockAddr(&connected_addr),
                          &connected_addr_len),
              SyscallSucceeds());
  uint16_t const ephemeral_port =
      ASSERT_NO_ERRNO_AND_VALUE(AddrPort(test_addr.family(), connected_addr));

  // Verify that we actually got an ephemeral port.
  ASSERT_NE(ephemeral_port, 0);

  // Verify that the ephemeral port is not reserved.
  const FileDescriptor checking_fd =
      ASSERT_NO_ERRNO_AND_VALUE(Socket(test_addr.family(), param.type, 0));
  ASSERT_THAT(setsockopt(checking_fd.get(), SOL_SOCKET, SO_REUSEADDR,
                         &kSockOptOn, sizeof(kSockOptOn)),
              SyscallSucceeds());
  EXPECT_THAT(
      bind(checking_fd.get(), AsSockAddr(&connected_addr), connected_addr_len),
      SyscallSucceeds());
}

TEST_P(SocketMultiProtocolInetLoopbackIsolatedTest,
       V6EphemeralPortReservedReuseAddr) {
  ProtocolTestParam const& param = GetParam();

  // Bind the v6 loopback on a dual stack socket.
  TestAddress const& test_addr = V6Loopback();
  sockaddr_storage bound_addr = test_addr.addr;
  const FileDescriptor bound_fd =
      ASSERT_NO_ERRNO_AND_VALUE(Socket(test_addr.family(), param.type, 0));
  ASSERT_THAT(bind(bound_fd.get(), AsSockAddr(&bound_addr), test_addr.addr_len),
              SyscallSucceeds());
  ASSERT_THAT(setsockopt(bound_fd.get(), SOL_SOCKET, SO_REUSEADDR, &kSockOptOn,
                         sizeof(kSockOptOn)),
              SyscallSucceeds());

  // Listen iff TCP.
  if (param.type == SOCK_STREAM) {
    ASSERT_THAT(listen(bound_fd.get(), SOMAXCONN), SyscallSucceeds());
  }

  // Get the port that we bound.
  socklen_t bound_addr_len = test_addr.addr_len;
  ASSERT_THAT(
      getsockname(bound_fd.get(), AsSockAddr(&bound_addr), &bound_addr_len),
      SyscallSucceeds());

  // Connect to bind an ephemeral port.
  const FileDescriptor connected_fd =
      ASSERT_NO_ERRNO_AND_VALUE(Socket(test_addr.family(), param.type, 0));
  ASSERT_THAT(setsockopt(connected_fd.get(), SOL_SOCKET, SO_REUSEADDR,
                         &kSockOptOn, sizeof(kSockOptOn)),
              SyscallSucceeds());
  ASSERT_THAT(RetryEINTR(connect)(connected_fd.get(), AsSockAddr(&bound_addr),
                                  bound_addr_len),
              SyscallSucceeds());

  // Get the ephemeral port.
  sockaddr_storage connected_addr = {};
  socklen_t connected_addr_len = sizeof(connected_addr);
  ASSERT_THAT(getsockname(connected_fd.get(), AsSockAddr(&connected_addr),
                          &connected_addr_len),
              SyscallSucceeds());
  uint16_t const ephemeral_port =
      ASSERT_NO_ERRNO_AND_VALUE(AddrPort(test_addr.family(), connected_addr));

  // Verify that we actually got an ephemeral port.
  ASSERT_NE(ephemeral_port, 0);

  // Verify that the ephemeral port is not reserved.
  const FileDescriptor checking_fd =
      ASSERT_NO_ERRNO_AND_VALUE(Socket(test_addr.family(), param.type, 0));
  ASSERT_THAT(setsockopt(checking_fd.get(), SOL_SOCKET, SO_REUSEADDR,
                         &kSockOptOn, sizeof(kSockOptOn)),
              SyscallSucceeds());
  EXPECT_THAT(
      bind(checking_fd.get(), AsSockAddr(&connected_addr), connected_addr_len),
      SyscallSucceeds());
}

TEST_P(SocketMultiProtocolInetLoopbackIsolatedTest,
       DualStackV6AnyCloseAndBindAgain) {
  int conn_port = 9000;
  int listen_port = 9001;

  FileDescriptor listen_fd =
      ASSERT_NO_ERRNO_AND_VALUE(Socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP));
  sockaddr_storage listen_addr;
  memset(&listen_addr, 0, sizeof(listen_addr));
  listen_addr.ss_family = AF_INET6;
  auto& addr_in6 = reinterpret_cast<struct sockaddr_in6&>(listen_addr);
  addr_in6.sin6_addr = in6addr_any;
  socklen_t laddrlen = sizeof(listen_addr);
  ASSERT_NO_ERRNO(SetAddrPort(AF_INET6, &listen_addr, listen_port));

  ASSERT_THAT(bind(listen_fd.get(), AsSockAddr(&listen_addr), laddrlen),
              SyscallSucceeds());
  constexpr int kBacklog = 1;
  ASSERT_THAT(listen(listen_fd.get(), kBacklog), SyscallSucceeds());

  // Bind the v6 any on a dual stack socket.
  TestAddress const& test_addr_dual = V6Any();
  sockaddr_storage addr_dual = test_addr_dual.addr;
  FileDescriptor fd_dual = ASSERT_NO_ERRNO_AND_VALUE(
      Socket(test_addr_dual.family(), SOCK_STREAM, 0));
  ASSERT_NO_ERRNO(SetAddrPort(test_addr_dual.family(), &addr_dual, conn_port));
  ASSERT_THAT(
      bind(fd_dual.get(), AsSockAddr(&addr_dual), test_addr_dual.addr_len),
      SyscallSucceeds());

  // Connect and accept.
  ASSERT_THAT(connect(fd_dual.get(), AsSockAddr(&listen_addr), laddrlen),
              SyscallSucceeds());
  ASSERT_THAT(accept(listen_fd.get(), nullptr, nullptr), SyscallSucceeds());

  // Close the socket.
  close(fd_dual.release());

  // Verify that binding the v4 loopback on the same port with a v4 socket
  // succeeds.
  TestAddress const& test_addr_v4 = V4Loopback();
  sockaddr_storage addr_v4 = test_addr_v4.addr;
  ASSERT_NO_ERRNO(SetAddrPort(test_addr_v4.family(), &addr_v4, conn_port));
  const FileDescriptor fd_v4 = ASSERT_NO_ERRNO_AND_VALUE(
      Socket(test_addr_v4.family(), SOCK_STREAM, IPPROTO_TCP));
  ASSERT_THAT(bind(fd_v4.get(), AsSockAddr(&addr_v4), test_addr_v4.addr_len),
              SyscallSucceeds());
}

TEST_P(SocketMultiProtocolInetLoopbackIsolatedTest, DualStackV6AnyBindAgain) {
  int conn_port = 9000;
  int listen_port = 9001;

  FileDescriptor listen_fd =
      ASSERT_NO_ERRNO_AND_VALUE(Socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP));
  sockaddr_storage listen_addr;
  memset(&listen_addr, 0, sizeof(listen_addr));
  listen_addr.ss_family = AF_INET6;
  auto& addr_in6 = reinterpret_cast<struct sockaddr_in6&>(listen_addr);
  addr_in6.sin6_addr = in6addr_any;
  socklen_t laddrlen = sizeof(listen_addr);
  ASSERT_NO_ERRNO(SetAddrPort(AF_INET6, &listen_addr, listen_port));

  ASSERT_THAT(bind(listen_fd.get(), AsSockAddr(&listen_addr), laddrlen),
              SyscallSucceeds());
  constexpr int kBacklog = 1;
  ASSERT_THAT(listen(listen_fd.get(), kBacklog), SyscallSucceeds());

  // Bind the v6 any on a dual stack socket.
  TestAddress const& test_addr_dual = V6Any();
  sockaddr_storage addr_dual = test_addr_dual.addr;
  FileDescriptor fd_dual = ASSERT_NO_ERRNO_AND_VALUE(
      Socket(test_addr_dual.family(), SOCK_STREAM, 0));
  ASSERT_NO_ERRNO(SetAddrPort(test_addr_dual.family(), &addr_dual, conn_port));
  ASSERT_THAT(
      bind(fd_dual.get(), AsSockAddr(&addr_dual), test_addr_dual.addr_len),
      SyscallSucceeds());

  // Connect and accept.
  ASSERT_THAT(connect(fd_dual.get(), AsSockAddr(&listen_addr), laddrlen),
              SyscallSucceeds());
  ASSERT_THAT(accept(listen_fd.get(), nullptr, nullptr), SyscallSucceeds());

  // Verify that binding the v4 loopback on the same port with a v4 socket
  // succeeds.
  TestAddress const& test_addr_v4 = V4Loopback();
  sockaddr_storage addr_v4 = test_addr_v4.addr;
  ASSERT_NO_ERRNO(SetAddrPort(test_addr_v4.family(), &addr_v4, conn_port));
  const FileDescriptor fd_v4 = ASSERT_NO_ERRNO_AND_VALUE(
      Socket(test_addr_v4.family(), SOCK_STREAM, IPPROTO_TCP));
  ASSERT_THAT(bind(fd_v4.get(), AsSockAddr(&addr_v4), test_addr_v4.addr_len),
              SyscallSucceeds());
}

INSTANTIATE_TEST_SUITE_P(AllFamilies,
                         SocketMultiProtocolInetLoopbackIsolatedTest,
                         ProtocolTestValues(), DescribeProtocolTestParam);

}  // namespace

}  // namespace testing
}  // namespace gvisor
