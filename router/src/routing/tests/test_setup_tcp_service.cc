/*
  Copyright (c) 2017, 2020, Oracle and/or its affiliates.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is also distributed with certain software (including
  but not limited to OpenSSL) that is licensed under separate terms,
  as designated in a particular file or component or in included license
  documentation.  The authors of MySQL hereby grant you an additional
  permission to link the program and your derivative works with the
  separately licensed software that they have included with MySQL.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include <cstdlib>  // memset
#include <memory>   // unique_ptr
#include <utility>  // exchange

#include <gmock/gmock.h>

#include "../../routing/src/mysql_routing.h"
#include "mock_io_service.h"
#include "mock_socket_service.h"
#include "mysql/harness/net_ts/impl/resolver.h"
#include "mysql/harness/net_ts/io_context.h"
#include "mysql/harness/stdx/expected.h"
#include "mysql/harness/stdx/expected_ostream.h"
#include "mysqlrouter/routing.h"
#include "test/helpers.h"  // init_test_loggers

using ::testing::_;
using ::testing::ByMove;
using ::testing::Return;

namespace std {
std::ostream &operator<<(std::ostream &os, const std::pair<int, int> &v) {
  os << "(" << v.first << ", " << v.second << ")" << std::endl;
  return os;
}
}  // namespace std

class TestSetupTcpService : public ::testing::Test {
 protected:
  template <class T>
  using result = stdx::expected<T, std::error_code>;

  // create some linked list of the addresses that getaddrinfo returns
  std::unique_ptr<addrinfo, void (*)(addrinfo *)> get_test_addresses_list(
      size_t qty = 1) {
    addrinfo *prev{nullptr}, *head{nullptr};
    for (size_t i = 0; i < qty; i++) {
      auto ai = new addrinfo();
      // fields values don't matter for us in those tests
      memset(ai, 0x0, sizeof(addrinfo));

      if (prev) {
        prev->ai_next = ai;
      }
      if (!head) {
        head = ai;
      }

      prev = ai;
    }

    return {head, [](addrinfo *head) {
              while (head != nullptr) {
                auto next = std::exchange(head->ai_next, nullptr);
                delete head;
                head = next;
              }
            }};
  }

 protected:
  net::io_context io_ctx_{std::make_unique<MockSocketService>(),
                          std::make_unique<MockIoService>()};
};

TEST_F(TestSetupTcpService, single_addr_ok) {
  auto &sock_ops = *dynamic_cast<MockSocketService *>(io_ctx_.socket_service());

  MySQLRouting r(io_ctx_, routing::RoutingStrategy::kFirstAvailable, 7001,
                 Protocol::Type::kClassicProtocol,
                 routing::AccessMode::kReadWrite, "127.0.0.1",
                 mysql_harness::Path(), "routing-name", 1,
                 std::chrono::seconds(1), 1, std::chrono::seconds(1),
                 routing::kDefaultNetBufferLength);

  EXPECT_CALL(sock_ops, getaddrinfo(_, _, _))
      .WillOnce(Return(ByMove(get_test_addresses_list(1))));

  EXPECT_CALL(sock_ops, socket(_, _, _)).WillOnce(Return(1));
#ifndef _WIN32
  EXPECT_CALL(sock_ops, setsockopt(_, _, _, _, _))
      .WillOnce(Return(result<void>{}));
#endif
  EXPECT_CALL(sock_ops, bind(_, _, _)).WillOnce(Return(result<void>{}));

  EXPECT_CALL(sock_ops, listen(_, _)).WillOnce(Return(result<void>{}));

  // those are called in the MySQLRouting destructor
  EXPECT_CALL(sock_ops, close(_));

  EXPECT_THAT(r.setup_tcp_service(),
              ::testing::Truly([](const auto &v) { return bool(v); }));
}

TEST_F(TestSetupTcpService, getaddrinfo_fails) {
  auto &sock_ops = *dynamic_cast<MockSocketService *>(io_ctx_.socket_service());

  MySQLRouting r(io_ctx_, routing::RoutingStrategy::kFirstAvailable, 7001,
                 Protocol::Type::kClassicProtocol,
                 routing::AccessMode::kReadWrite, "127.0.0.1",
                 mysql_harness::Path(), "routing-name", 1,
                 std::chrono::seconds(1), 1, std::chrono::seconds(1),
                 routing::kDefaultNetBufferLength);

  EXPECT_CALL(sock_ops, getaddrinfo(_, _, _))
      .WillOnce(Return(ByMove(stdx::make_unexpected(
          make_error_code(net::ip::resolver_errc::host_not_found)))));

  EXPECT_EQ(r.setup_tcp_service(),
            stdx::make_unexpected(
                make_error_code(net::ip::resolver_errc::host_not_found)));
}

TEST_F(TestSetupTcpService, socket_fails_for_all_addr) {
  auto &sock_ops = *dynamic_cast<MockSocketService *>(io_ctx_.socket_service());

  MySQLRouting r(io_ctx_, routing::RoutingStrategy::kFirstAvailable, 7001,
                 Protocol::Type::kClassicProtocol,
                 routing::AccessMode::kReadWrite, "127.0.0.1",
                 mysql_harness::Path(), "routing-name", 1,
                 std::chrono::seconds(1), 1, std::chrono::seconds(1),
                 routing::kDefaultNetBufferLength);

  EXPECT_CALL(sock_ops, getaddrinfo(_, _, _))
      .WillOnce(Return(ByMove(get_test_addresses_list(2))));

  // make all calls to socket() fail
  EXPECT_CALL(sock_ops, socket(_, _, _))
      .WillOnce(Return(stdx::make_unexpected(
          make_error_code(std::errc::address_family_not_supported))))
      .WillOnce(Return(stdx::make_unexpected(
          make_error_code(std::errc::address_family_not_supported))));

  EXPECT_EQ(r.setup_tcp_service(),
            stdx::make_unexpected(
                make_error_code(std::errc::address_family_not_supported)));
}

TEST_F(TestSetupTcpService, socket_fails) {
  auto &sock_ops = *dynamic_cast<MockSocketService *>(io_ctx_.socket_service());

  MySQLRouting r(io_ctx_, routing::RoutingStrategy::kFirstAvailable, 7001,
                 Protocol::Type::kClassicProtocol,
                 routing::AccessMode::kReadWrite, "127.0.0.1",
                 mysql_harness::Path(), "routing-name", 1,
                 std::chrono::seconds(1), 1, std::chrono::seconds(1),
                 routing::kDefaultNetBufferLength);

  EXPECT_CALL(sock_ops, getaddrinfo(_, _, _))
      .WillOnce(Return(ByMove(get_test_addresses_list(2))));

  // make the first call to socket() fail
  EXPECT_CALL(sock_ops, socket(_, _, _))
      .WillOnce(Return(stdx::make_unexpected(
          make_error_code(std::errc::address_family_not_supported))))
      .WillOnce(Return(1));

#ifndef _WIN32
  EXPECT_CALL(sock_ops, setsockopt(_, _, _, _, _))
      .WillOnce(Return(result<void>{}));
#endif
  EXPECT_CALL(sock_ops, bind(_, _, _)).WillOnce(Return(result<void>{}));

  EXPECT_CALL(sock_ops, listen(_, _)).WillOnce(Return(result<void>{}));

  // those are called in the MySQLRouting destructor
  EXPECT_CALL(sock_ops, close(_));

  EXPECT_THAT(r.setup_tcp_service(),
              ::testing::Truly([](const auto &v) { return bool(v); }));
}

#ifndef _WIN32
TEST_F(TestSetupTcpService, setsockopt_fails) {
  auto &sock_ops = *dynamic_cast<MockSocketService *>(io_ctx_.socket_service());
  MySQLRouting r(io_ctx_, routing::RoutingStrategy::kFirstAvailable, 7001,
                 Protocol::Type::kClassicProtocol,
                 routing::AccessMode::kReadWrite, "127.0.0.1",
                 mysql_harness::Path(), "routing-name", 1,
                 std::chrono::seconds(1), 1, std::chrono::seconds(1),
                 routing::kDefaultNetBufferLength);

  EXPECT_CALL(sock_ops, getaddrinfo(_, _, _))
      .WillOnce(Return(ByMove(get_test_addresses_list(2))));

  EXPECT_CALL(sock_ops, socket(_, _, _))
      .WillOnce(Return(1))
      .WillOnce(Return(1));

  // make the first call to setsockopt() fail
  EXPECT_CALL(sock_ops, setsockopt(_, _, _, _, _))
      .WillOnce(Return(stdx::make_unexpected(
          make_error_code(std::errc::bad_file_descriptor))))
      .WillOnce(Return(result<void>{}));

  EXPECT_CALL(sock_ops, bind(_, _, _)).WillOnce(Return(result<void>{}));

  EXPECT_CALL(sock_ops, listen(_, _)).WillOnce(Return(result<void>{}));

  // those are called in the MySQLRouting destructor
  EXPECT_CALL(sock_ops, close(_)).Times(2);

  EXPECT_THAT(r.setup_tcp_service(),
              ::testing::Truly([](const auto &v) { return bool(v); }));
}
#endif

TEST_F(TestSetupTcpService, bind_fails) {
  auto &sock_ops = *dynamic_cast<MockSocketService *>(io_ctx_.socket_service());

  MySQLRouting r(io_ctx_, routing::RoutingStrategy::kFirstAvailable, 7001,
                 Protocol::Type::kClassicProtocol,
                 routing::AccessMode::kReadWrite, "127.0.0.1",
                 mysql_harness::Path(), "routing-name", 1,
                 std::chrono::seconds(1), 1, std::chrono::seconds(1),
                 routing::kDefaultNetBufferLength);

  EXPECT_CALL(sock_ops, getaddrinfo(_, _, _))
      .WillOnce(Return(ByMove(get_test_addresses_list(2))));

  // make the first call to socket() fail
  EXPECT_CALL(sock_ops, socket(_, _, _))
      .WillOnce(Return(1))
      .WillOnce(Return(1));

#ifndef _WIN32
  EXPECT_CALL(sock_ops, setsockopt(_, _, _, _, _))
      .WillOnce(Return(result<void>{}))
      .WillOnce(Return(result<void>{}));
#endif
  EXPECT_CALL(sock_ops, bind(_, _, _))
      .WillOnce(Return(
          stdx::make_unexpected(make_error_code(std::errc::invalid_argument))))
      .WillOnce(Return(result<void>{}));

  EXPECT_CALL(sock_ops, listen(_, _)).WillOnce(Return(result<void>{}));

  // those are called in the MySQLRouting destructor
  EXPECT_CALL(sock_ops, close(_)).Times(2);

  EXPECT_THAT(r.setup_tcp_service(),
              ::testing::Truly([](const auto &v) { return bool(v); }));
}

TEST_F(TestSetupTcpService, listen_fails) {
  auto &sock_ops = *dynamic_cast<MockSocketService *>(io_ctx_.socket_service());

  MySQLRouting r(io_ctx_, routing::RoutingStrategy::kFirstAvailable, 7001,
                 Protocol::Type::kClassicProtocol,
                 routing::AccessMode::kReadWrite, "127.0.0.1",
                 mysql_harness::Path(), "routing-name", 1,
                 std::chrono::seconds(1), 1, std::chrono::seconds(1),
                 routing::kDefaultNetBufferLength);

  EXPECT_CALL(sock_ops, getaddrinfo(_, _, _))
      .WillOnce(Return(ByMove(get_test_addresses_list(2))));

  // make the first call to socket() fail
  EXPECT_CALL(sock_ops, socket(_, _, _)).WillOnce(Return(1));

#ifndef _WIN32
  EXPECT_CALL(sock_ops, setsockopt(_, _, _, _, _))
      .WillOnce(Return(result<void>{}));
#endif
  EXPECT_CALL(sock_ops, bind(_, _, _)).WillOnce(Return(result<void>{}));

  EXPECT_CALL(sock_ops, listen(_, _))
      .WillOnce(Return(
          stdx::make_unexpected(make_error_code(std::errc::invalid_argument))));

  // those are called in the MySQLRouting destructor
  EXPECT_CALL(sock_ops, close(_));

  // the listen()'s error-code
  EXPECT_EQ(
      r.setup_tcp_service(),
      stdx::make_unexpected(make_error_code(std::errc::invalid_argument)));
}

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  init_test_logger();
  return RUN_ALL_TESTS();
}
