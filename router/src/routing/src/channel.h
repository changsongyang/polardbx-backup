/*
Copyright (c) 2020, Oracle and/or its affiliates.

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

#ifndef MYSQL_ROUTER_CHANNEL_INCLUDED
#define MYSQL_ROUTER_CHANNEL_INCLUDED

#ifdef _WIN32
// include winsock2.h before openssl/ssl.h
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <openssl/ssl.h>

#include "mysql/harness/net_ts/buffer.h"
#include "mysql/harness/stdx/expected.h"
#include "mysqlrouter/classic_protocol.h"

/**
 * SSL aware socket buffers.
 *
 * manages the raw and plaintext socket buffers of connection which may switch
 * to TLS.
 *
 * an external user like a socket class can
 *
 * - receive from a socket and store the socket data into the recv_buffer().
 * - send to a socket from the send_buffer().
 *
 * Once init_ssl() is called, the recv_plain() and write_plain() methods
 * transparently decrypt and encrypt.
 */
class Channel {
 public:
  /**
   * initialize the SSL session.
   */
  void init_ssl(SSL_CTX *ssl_ctx) {
    ssl_.reset(SSL_new(ssl_ctx));
    // the BIOs are owned by the SSL
    SSL_set_bio(ssl_.get(), BIO_new(BIO_s_mem()), BIO_new(BIO_s_mem()));
  }

  /**
   * accept a TLS session.
   *
   * expects a Tls::ClientHello in the recv_buf.
   */
  stdx::expected<void, std::error_code> tls_accept();

  /**
   * connect a TLS session.
   *
   *
   */
  stdx::expected<void, std::error_code> tls_connect();
  bool tls_init_is_finished();

  /**
   * @return on success, true if shutdown is finished, false if not
   * @return on failure, an error-code
   */
  stdx::expected<bool, std::error_code> tls_shutdown();

  /**
   * write unencrypted net::dynamic_buffer to the channel.
   *
   * if the channel has an ssl session it transparently encrypts before
   * the data is appended to the send_buf()
   */
  template <class DynamicBuffer>
  typename std::enable_if<net::is_dynamic_buffer<DynamicBuffer>::value,
                          stdx::expected<size_t, std::error_code>>::type
  write(DynamicBuffer &dyn_buf) {
    auto orig_size = dyn_buf.size();
    size_t transferred{};

    auto write_res = write_plain(dyn_buf.data(0, orig_size));
    if (!write_res) {
      return write_res.get_unexpected();
    }

    transferred += write_res.value();
    dyn_buf.consume(write_res.value());

    return transferred;
  }

  /**
   * write unencrypted net::const_buffer to the channel.
   *
   * if the channel has an ssl session it transparently encrypts before
   * the data is appended to the send_buf()
   */
  stdx::expected<size_t, std::error_code> write(const net::const_buffer &b) {
    return write_plain(b);
  }

  /**
   * read unencrypted data from channel to a net::dynamic_buffer.
   *
   * if the channel has a ssl session in transparently decrypts before
   * the data is appending to the recv_plain_buf()
   */
  template <class DynamicBuffer>
  stdx::expected<size_t, std::error_code> read(DynamicBuffer &dyn_buf) {
    auto orig_size = dyn_buf.size();
    auto grow_size = 16 * 1024;
    size_t transferred{};

    dyn_buf.grow(grow_size);

    const auto res_res = read_plain(dyn_buf.data(orig_size, grow_size));
    if (!res_res) {
      dyn_buf.shrink(grow_size);
      return res_res.get_unexpected();
    }

    transferred += res_res.value();
    dyn_buf.shrink(grow_size - res_res.value());

    return transferred;
  }

  /**
   * write raw data from a net::const_buffer to the channel.
   */
  stdx::expected<size_t, std::error_code> write_encrypted(
      const net::const_buffer &b);

  /**
   * write unencrypted data from a net::const_buffer to the channel.
   *
   * if the channel has an ssl session it transparently encrypts before
   * the data is appended to the send_buf()
   */
  stdx::expected<size_t, std::error_code> write_plain(
      const net::const_buffer &b);

  /**
   * read raw data from recv_buffer() into b.
   */
  stdx::expected<size_t, std::error_code> read_encrypted(
      const net::mutable_buffer &b);

  /**
   * read plaintext data from recv_plain_buffer() into b.
   */
  stdx::expected<size_t, std::error_code> read_plain(
      const net::mutable_buffer &b);

  /**
   * flush data from receive buffer to recv_plain_buffer().
   *
   * if an SSL session is active, flush_to_recv_buf() ensures that data
   * encrypted data gets decrypted and added to the recv_plain_buffer().
   *
   * In case no SSL session is active, it is a no-op.
   */
  stdx::expected<size_t, std::error_code> flush_from_recv_buf();

  /**
   * flush data to the send buffer.
   *
   * if write_plain() was used and an SSL session is active,
   * flush_to_send_buf() ensures that data plaintext data gets encrypted and
   * added to the send_buf().
   *
   * In case no SSL session is active, it is a no-op.
   */
  stdx::expected<size_t, std::error_code> flush_to_send_buf();

  /**
   * bytes wanted.
   *
   * signals to the socket layer how many bytes should be at least read into the
   * buffer.
   */
  void want_recv(size_t wanted) { want_recv_ = wanted; }

  /**
   * bytes wanted.
   *
   * @return bytes wanted to be received at least.
   */
  size_t want_recv() const { return want_recv_; }

  /**
   * buffer of data that was received from the socket.
   *
   * written into by write(), write_plain(), write_encrypted().
   */
  std::vector<uint8_t> &recv_buffer() { return recv_buffer_; }

  /**
   * buffer of data to be sent to the socket.
   *
   * written into by write(), write_plain(), write_encrypted().
   */
  std::vector<uint8_t> &send_buffer() { return send_buffer_; }

  /**
   * buffer of data that was received from the socket.
   *
   * written into by write(), write_plain(), write_encrypted().
   */
  const std::vector<uint8_t> &recv_buffer() const { return recv_buffer_; }

  /**
   * buffer of data to be sent to the socket.
   *
   * written into by write(), write_plain(), write_encrypted().
   */
  const std::vector<uint8_t> &send_buffer() const { return send_buffer_; }

  /**
   * unencrypted data after a recv().
   */
  std::vector<uint8_t> &recv_plain_buffer() { return recv_plain_buffer_; }

  /**
   * mark channel as containing TLS data in the recv_buffer().
   *
   * it is independent of calling init_tls() as the channel
   * may be used to transfer encrypted data as is without
   * ever call init_ssl().
   */
  void is_tls(bool v) { is_tls_ = v; }

  /**
   * check if connection switched to TLS.
   */
  bool is_tls() const { return is_tls_; }

  /**
   * get access to the raw SSL handle.
   *
   * can be used to call:
   *
   * - SSL_get_cipher_name()
   * - SSL_version()
   *
   * @retval nullptr if channel has no SSL initialized.
   */
  SSL *ssl() const { return ssl_.get(); }

 private:
  size_t want_recv_{};

  std::vector<uint8_t> recv_buffer_;
  std::vector<uint8_t> recv_plain_buffer_;
  std::vector<uint8_t> send_buffer_;

  bool is_tls_{false};

  class Deleter_SSL {
   public:
    void operator()(SSL *v) { SSL_free(v); }
  };

  std::unique_ptr<SSL, Deleter_SSL> ssl_{};
};

/**
 * protocol state of a classic protocol connection.
 */
class ClassicProtocolState {
 public:
  void server_capabilities(classic_protocol::capabilities::value_type caps) {
    server_caps_ = caps;
  }

  void client_capabilities(classic_protocol::capabilities::value_type caps) {
    client_caps_ = caps;
  }

  classic_protocol::capabilities::value_type client_capabilities() const {
    return client_caps_;
  }

  classic_protocol::capabilities::value_type server_capabilities() const {
    return server_caps_;
  }

  classic_protocol::capabilities::value_type shared_capabilities() const {
    return server_caps_ & client_caps_;
  }

  stdx::expected<classic_protocol::message::client::Greeting, void>
  client_greeting() const {
    return client_greeting_;
  }

  void client_greeting(
      stdx::expected<classic_protocol::message::client::Greeting, void> msg) {
    client_greeting_ = std::move(msg);
  }

  void server_greeting(
      stdx::expected<classic_protocol::message::server::Greeting, void> msg) {
    server_greeting_ = std::move(msg);
  }

  uint8_t &seq_id() { return seq_id_; }
  uint8_t seq_id() const { return seq_id_; }

  void seq_id(uint8_t id) { seq_id_ = id; }

 private:
  classic_protocol::capabilities::value_type server_caps_{};
  classic_protocol::capabilities::value_type client_caps_{};

  stdx::expected<classic_protocol::message::client::Greeting, void>
      client_greeting_{stdx::make_unexpected()};
  stdx::expected<classic_protocol::message::server::Greeting, void>
      server_greeting_{stdx::make_unexpected()};

  uint8_t seq_id_{};
};

/**
 * protocol state of a xproto connection.
 */
class XProtocolState {
 public:
  XProtocolState() = default;

 private:
  int _;
};

#endif
