/*
 * nghttp2 - HTTP/2 C Library
 *
 * Copyright (c) 2019 nghttp2 contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "h2load_quic.h"

#include <netinet/udp.h>

#include <iostream>

#if defined(HAVE_LIBNGTCP2_CRYPTO_QUICTLS) ||                                  \
  defined(HAVE_LIBNGTCP2_CRYPTO_LIBRESSL)
#  include <ngtcp2/ngtcp2_crypto_quictls.h>
#endif // defined(HAVE_LIBNGTCP2_CRYPTO_QUICTLS) ||
       // defined(HAVE_LIBNGTCP2_CRYPTO_LIBRESSL)
#ifdef HAVE_LIBNGTCP2_CRYPTO_BORINGSSL
#  include <ngtcp2/ngtcp2_crypto_boringssl.h>
#endif // defined(HAVE_LIBNGTCP2_CRYPTO_BORINGSSL)

#include "ssl_compat.h"

#ifdef NGHTTP2_OPENSSL_IS_WOLFSSL
#  include <wolfssl/options.h>
#  include <wolfssl/openssl/err.h>
#  include <wolfssl/openssl/rand.h>
#else // !defined(NGHTTP2_OPENSSL_IS_WOLFSSL)
#  include <openssl/err.h>
#  include <openssl/rand.h>
#endif // !defined(NGHTTP2_OPENSSL_IS_WOLFSSL)

#include "h2load_http3_session.h"

namespace h2load {

namespace {
int handshake_completed(ngtcp2_conn *conn, void *user_data) {
  auto c = static_cast<Client *>(user_data);

  if (c->quic_handshake_completed() != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

int Client::quic_handshake_completed() { return connection_made(); }

namespace {
int recv_stream_data(ngtcp2_conn *conn, uint32_t flags, int64_t stream_id,
                     uint64_t offset, const uint8_t *data, size_t datalen,
                     void *user_data, void *stream_user_data) {
  auto c = static_cast<Client *>(user_data);
  if (c->quic_recv_stream_data(flags, stream_id, data, datalen) != 0) {
    // TODO Better to do this gracefully rather than
    // NGTCP2_ERR_CALLBACK_FAILURE.  Perhaps, call
    // ngtcp2_conn_write_application_close() ?
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }
  return 0;
}
} // namespace

int Client::quic_recv_stream_data(uint32_t flags, int64_t stream_id,
                                  const uint8_t *data, size_t datalen) {
  if (worker->current_phase == Phase::MAIN_DURATION) {
    worker->stats.bytes_total += datalen;
  }

  auto s = static_cast<Http3Session *>(session.get());
  auto nconsumed = s->read_stream(flags, stream_id, data, datalen);
  if (nconsumed == -1) {
    return -1;
  }

  ngtcp2_conn_extend_max_stream_offset(quic.conn, stream_id,
                                       static_cast<uint64_t>(nconsumed));
  ngtcp2_conn_extend_max_offset(quic.conn, static_cast<uint64_t>(nconsumed));

  return 0;
}

namespace {
int acked_stream_data_offset(ngtcp2_conn *conn, int64_t stream_id,
                             uint64_t offset, uint64_t datalen, void *user_data,
                             void *stream_user_data) {
  auto c = static_cast<Client *>(user_data);
  if (c->quic_acked_stream_data_offset(stream_id, datalen) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }
  return 0;
}
} // namespace

int Client::quic_acked_stream_data_offset(int64_t stream_id, size_t datalen) {
  auto s = static_cast<Http3Session *>(session.get());
  if (s->add_ack_offset(stream_id, datalen) != 0) {
    return -1;
  }
  return 0;
}

namespace {
int stream_close(ngtcp2_conn *conn, uint32_t flags, int64_t stream_id,
                 uint64_t app_error_code, void *user_data,
                 void *stream_user_data) {
  auto c = static_cast<Client *>(user_data);

  if (!(flags & NGTCP2_STREAM_CLOSE_FLAG_APP_ERROR_CODE_SET)) {
    app_error_code = NGHTTP3_H3_NO_ERROR;
  }

  if (c->quic_stream_close(stream_id, app_error_code) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }
  return 0;
}
} // namespace

int Client::quic_stream_close(int64_t stream_id, uint64_t app_error_code) {
  auto s = static_cast<Http3Session *>(session.get());
  if (s->close_stream(stream_id, app_error_code) != 0) {
    return -1;
  }
  return 0;
}

namespace {
int stream_reset(ngtcp2_conn *conn, int64_t stream_id, uint64_t final_size,
                 uint64_t app_error_code, void *user_data,
                 void *stream_user_data) {
  auto c = static_cast<Client *>(user_data);
  if (c->quic_stream_reset(stream_id, app_error_code) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }
  return 0;
}
} // namespace

int Client::quic_stream_reset(int64_t stream_id, uint64_t app_error_code) {
  auto s = static_cast<Http3Session *>(session.get());
  if (s->shutdown_stream_read(stream_id) != 0) {
    return -1;
  }
  return 0;
}

namespace {
int stream_stop_sending(ngtcp2_conn *conn, int64_t stream_id,
                        uint64_t app_error_code, void *user_data,
                        void *stream_user_data) {
  auto c = static_cast<Client *>(user_data);
  if (c->quic_stream_stop_sending(stream_id, app_error_code) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }
  return 0;
}
} // namespace

int Client::quic_stream_stop_sending(int64_t stream_id,
                                     uint64_t app_error_code) {
  auto s = static_cast<Http3Session *>(session.get());
  if (s->shutdown_stream_read(stream_id) != 0) {
    return -1;
  }
  return 0;
}

namespace {
int extend_max_local_streams_bidi(ngtcp2_conn *conn, uint64_t max_streams,
                                  void *user_data) {
  auto c = static_cast<Client *>(user_data);

  if (c->quic_extend_max_local_streams() != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

int Client::quic_extend_max_local_streams() {
  auto s = static_cast<Http3Session *>(session.get());
  if (s->extend_max_local_streams() != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }
  return 0;
}

namespace {
int extend_max_stream_data(ngtcp2_conn *conn, int64_t stream_id,
                           uint64_t max_data, void *user_data,
                           void *stream_user_data) {
  auto c = static_cast<Client *>(user_data);

  if (c->quic_extend_max_stream_data(stream_id) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

int Client::quic_extend_max_stream_data(int64_t stream_id) {
  auto s = static_cast<Http3Session *>(session.get());
  if (s->unblock_stream(stream_id) != 0) {
    return -1;
  }
  return 0;
}

namespace {
int get_new_connection_id(ngtcp2_conn *conn, ngtcp2_cid *cid, uint8_t *token,
                          size_t cidlen, void *user_data) {
  if (RAND_bytes(cid->data,
                 static_cast<nghttp2_ssl_rand_length_type>(cidlen)) != 1) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  cid->datalen = cidlen;

  if (RAND_bytes(token, NGTCP2_STATELESS_RESET_TOKENLEN) != 1) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

namespace {
void debug_log_printf(void *user_data, const char *fmt, ...) {
  va_list ap;

  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);

  fprintf(stderr, "\n");
}
} // namespace

namespace {
int generate_cid(ngtcp2_cid &dest) {
  dest.datalen = 8;

  if (RAND_bytes(dest.data, static_cast<nghttp2_ssl_rand_length_type>(
                              dest.datalen)) != 1) {
    return -1;
  }

  return 0;
}
} // namespace

namespace {
ngtcp2_tstamp quic_timestamp() {
  return static_cast<ngtcp2_tstamp>(
    std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count());
}
} // namespace

// qlog write callback -- excerpted from ngtcp2/examples/client_base.cc
namespace {
void qlog_write_cb(void *user_data, uint32_t flags, const void *data,
                   size_t datalen) {
  auto c = static_cast<Client *>(user_data);
  c->quic_write_qlog(data, datalen);
}
} // namespace

void Client::quic_write_qlog(const void *data, size_t datalen) {
  assert(quic.qlog_file != nullptr);
  fwrite(data, 1, datalen, quic.qlog_file);
}

namespace {
void rand(uint8_t *dest, size_t destlen, const ngtcp2_rand_ctx *rand_ctx) {
  auto rv =
    RAND_bytes(dest, static_cast<nghttp2_ssl_rand_length_type>(destlen));
  if (rv != 1) {
    assert(0);
    abort();
  }
}
} // namespace

namespace {
int recv_rx_key(ngtcp2_conn *conn, ngtcp2_encryption_level level,
                void *user_data) {
  if (level != NGTCP2_ENCRYPTION_LEVEL_1RTT) {
    return 0;
  }

  auto c = static_cast<Client *>(user_data);

  if (c->quic_make_http3_session() != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

int Client::quic_make_http3_session() {
  auto s = std::make_unique<Http3Session>(this);
  if (s->init_conn() == -1) {
    return -1;
  }
  session = std::move(s);

  return 0;
}

namespace {
ngtcp2_conn *get_conn(ngtcp2_crypto_conn_ref *conn_ref) {
  auto c = static_cast<Client *>(conn_ref->user_data);
  return c->quic.conn;
}
} // namespace

int Client::quic_init(const sockaddr *local_addr, socklen_t local_addrlen,
                      const sockaddr *remote_addr, socklen_t remote_addrlen) {
  int rv;

  if (!ssl) {
    ssl = SSL_new(worker->ssl_ctx);

    quic.conn_ref.get_conn = get_conn;
    quic.conn_ref.user_data = this;

    SSL_set_app_data(ssl, &quic.conn_ref);
    SSL_set_connect_state(ssl);
#if OPENSSL_3_5_0_API
    if (ngtcp2_crypto_ossl_configure_client_session(ssl) != 0) {
      std::cerr << "ngtcp2_crypto_ossl_configure_client_session failed"
                << std::endl;
      return -1;
    }

    rv = ngtcp2_crypto_ossl_ctx_new(&quic.ossl_ctx, ssl);
    if (rv != 0) {
      std::cerr << "ngtcp2_crypto_ossl_ctx_new failed with error code " << rv
                << std::endl;
      return -1;
    }
#else  // !OPENSSL_3_5_0_API
    SSL_set_quic_use_legacy_codepoint(ssl, 0);
#endif // !OPENSSL_3_5_0_API
  }

  auto callbacks = ngtcp2_callbacks{
    .client_initial = ngtcp2_crypto_client_initial_cb,
    .recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb,
    .handshake_completed = h2load::handshake_completed,
    .encrypt = ngtcp2_crypto_encrypt_cb,
    .decrypt = ngtcp2_crypto_decrypt_cb,
    .hp_mask = ngtcp2_crypto_hp_mask_cb,
    .recv_stream_data = h2load::recv_stream_data,
    .acked_stream_data_offset = h2load::acked_stream_data_offset,
    .stream_close = h2load::stream_close,
    .recv_retry = ngtcp2_crypto_recv_retry_cb,
    .extend_max_local_streams_bidi = h2load::extend_max_local_streams_bidi,
    .rand = h2load::rand,
    .get_new_connection_id = get_new_connection_id,
    .update_key = ngtcp2_crypto_update_key_cb,
    .stream_reset = h2load::stream_reset,
    .extend_max_stream_data = h2load::extend_max_stream_data,
    .delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb,
    .delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb,
    .get_path_challenge_data = ngtcp2_crypto_get_path_challenge_data_cb,
    .stream_stop_sending = h2load::stream_stop_sending,
    .recv_rx_key = h2load::recv_rx_key,
  };

  ngtcp2_cid scid, dcid;
  if (generate_cid(scid) != 0) {
    return -1;
  }
  if (generate_cid(dcid) != 0) {
    return -1;
  }

  auto config = worker->config;

  ngtcp2_settings settings;
  ngtcp2_settings_default(&settings);
  if (config->verbose) {
    settings.log_printf = debug_log_printf;
  }
  settings.initial_ts = quic_timestamp();
  settings.rand_ctx.native_handle = &worker->randgen;
  if (!config->qlog_file_base.empty()) {
    assert(quic.qlog_file == nullptr);
    auto path = config->qlog_file_base;
    path += '.';
    path += util::utos(worker->id);
    path += '.';
    path += util::utos(id);
    path += ".sqlog";
    quic.qlog_file = fopen(path.c_str(), "w");
    if (quic.qlog_file == nullptr) {
      std::cerr << "Failed to open a qlog file: " << path << std::endl;
      return -1;
    }
    settings.qlog_write = qlog_write_cb;
  }
  if (config->max_udp_payload_size) {
    settings.max_tx_udp_payload_size = config->max_udp_payload_size;
    settings.no_tx_udp_payload_size_shaping = 1;
  }

  ngtcp2_transport_params params;
  ngtcp2_transport_params_default(&params);
  auto max_stream_data = static_cast<uint64_t>(
    std::min((1 << 26) - 1, (1 << config->window_bits) - 1));
  params.initial_max_stream_data_bidi_local = max_stream_data;
  params.initial_max_stream_data_uni = max_stream_data;
  params.initial_max_data = (1 << config->connection_window_bits) - 1;
  params.initial_max_streams_bidi = 0;
  params.initial_max_streams_uni = 100;
  params.max_idle_timeout = 30 * NGTCP2_SECONDS;

  auto path = ngtcp2_path{
    {
      const_cast<sockaddr *>(local_addr),
      local_addrlen,
    },
    {
      const_cast<sockaddr *>(remote_addr),
      remote_addrlen,
    },
  };

  assert(config->alpn_list.size());

  uint32_t quic_version;

  if (config->alpn_list[0] == NGHTTP3_ALPN_H3) {
    quic_version = NGTCP2_PROTO_VER_V1;
  } else {
    quic_version = NGTCP2_PROTO_VER_MIN;
  }

  rv = ngtcp2_conn_client_new(&quic.conn, &dcid, &scid, &path, quic_version,
                              &callbacks, &settings, &params, nullptr, this);
  if (rv != 0) {
    return -1;
  }

#if OPENSSL_3_5_0_API
  ngtcp2_conn_set_tls_native_handle(quic.conn, quic.ossl_ctx);
#else  // !OPENSSL_3_5_0_API
  ngtcp2_conn_set_tls_native_handle(quic.conn, ssl);
#endif // !OPENSSL_3_5_0_API

  return 0;
}

void Client::quic_free() {
#if OPENSSL_3_5_0_API
  ngtcp2_crypto_ossl_ctx_del(quic.ossl_ctx);
#endif // OPENSSL_3_5_0_API

  ngtcp2_conn_del(quic.conn);
  if (quic.qlog_file != nullptr) {
    fclose(quic.qlog_file);
    quic.qlog_file = nullptr;
  }
}

void Client::quic_close_connection() {
  if (!quic.conn) {
    return;
  }

  std::array<uint8_t, NGTCP2_MAX_UDP_PAYLOAD_SIZE> buf;
  ngtcp2_path_storage ps;
  ngtcp2_path_storage_zero(&ps);

  auto nwrite = ngtcp2_conn_write_connection_close(
    quic.conn, &ps.path, nullptr, buf.data(), buf.size(), &quic.last_error,
    quic_timestamp());

  if (nwrite <= 0) {
    return;
  }

  write_udp(reinterpret_cast<sockaddr *>(ps.path.remote.addr),
            ps.path.remote.addrlen, {buf.data(), static_cast<size_t>(nwrite)},
            as_unsigned(nwrite));
}

int Client::quic_write_client_handshake(ngtcp2_encryption_level level,
                                        const uint8_t *data, size_t datalen) {
  int rv;

  assert(level < 2);

  rv = ngtcp2_conn_submit_crypto_data(quic.conn, level, data, datalen);
  if (rv != 0) {
    std::cerr << "ngtcp2_conn_submit_crypto_data: " << ngtcp2_strerror(rv)
              << std::endl;
    return -1;
  }

  return 0;
}

void quic_pkt_timeout_cb(struct ev_loop *loop, ev_timer *w, int revents) {
  auto c = static_cast<Client *>(w->data);

  if (c->quic_pkt_timeout() != 0) {
    c->fail();
    c->worker->free_client(c);
    delete c;
    return;
  }
}

int Client::quic_pkt_timeout() {
  int rv;
  auto now = quic_timestamp();

  rv = ngtcp2_conn_handle_expiry(quic.conn, now);
  if (rv != 0) {
    ngtcp2_ccerr_set_liberr(&quic.last_error, rv, nullptr, 0);
    return -1;
  }

  signal_write();

  return 0;
}

void Client::quic_restart_pkt_timer() {
  auto expiry = ngtcp2_conn_get_expiry(quic.conn);
  auto now = quic_timestamp();
  auto t =
    expiry > now ? static_cast<ev_tstamp>(expiry - now) / NGTCP2_SECONDS : 1e-9;
  quic.pkt_timer.repeat = t;
  ev_timer_again(worker->loop, &quic.pkt_timer);
}

int Client::read_quic() {
  std::array<uint8_t, 64_k> buf;
  sockaddr_union su;
  int rv;
  size_t pktcnt = 0;
  ngtcp2_pkt_info pi;

  iovec msg_iov{
    .iov_base = buf.data(),
    .iov_len = buf.size(),
  };

  uint8_t msg_ctrl[CMSG_SPACE(sizeof(int))];

  msghdr msg{
    .msg_name = &su,
    .msg_iov = &msg_iov,
    .msg_iovlen = 1,
    .msg_control = msg_ctrl,
  };

  auto ts = quic_timestamp();

  for (;;) {
    msg.msg_namelen = sizeof(su);
    msg.msg_controllen = sizeof(msg_ctrl);

    auto nread = recvmsg(fd, &msg, 0);
    if (nread == -1) {
      return 0;
    }

    auto gso_size = util::msghdr_get_udp_gro(&msg);
    if (gso_size == 0) {
      gso_size = static_cast<size_t>(nread);
    }

    assert(quic.conn);

    if (gso_size) {
      worker->stats.udp_dgram_recv +=
        (as_unsigned(nread) + gso_size - 1) / gso_size;
    } else {
      ++worker->stats.udp_dgram_recv;
    }

    auto path = ngtcp2_path{
      {
        &local_addr.su.sa,
        local_addr.len,
      },
      {
        &su.sa,
        msg.msg_namelen,
      },
    };

    auto data = buf.data();

    for (;;) {
      auto datalen = std::min(static_cast<size_t>(nread), gso_size);

      ++pktcnt;

      rv = ngtcp2_conn_read_pkt(quic.conn, &path, &pi, data, datalen, ts);
      if (rv != 0) {
        if (!quic.last_error.error_code) {
          if (rv == NGTCP2_ERR_CRYPTO) {
            ngtcp2_ccerr_set_tls_alert(&quic.last_error,
                                       ngtcp2_conn_get_tls_alert(quic.conn),
                                       nullptr, 0);
          } else {
            ngtcp2_ccerr_set_liberr(&quic.last_error, rv, nullptr, 0);
          }
        }

        return -1;
      }

      nread -= datalen;
      if (nread == 0) {
        break;
      }

      data += datalen;
    }

    if (pktcnt >= 100) {
      break;
    }
  }

  return 0;
}

namespace {
ngtcp2_ssize write_pkt(ngtcp2_conn *conn, ngtcp2_path *path,
                       ngtcp2_pkt_info *pi, uint8_t *dest, size_t destlen,
                       ngtcp2_tstamp ts, void *user_data) {
  auto c = static_cast<Client *>(user_data);

  return c->write_quic_pkt(path, pi, dest, destlen, ts);
}
} // namespace

ngtcp2_ssize Client::write_quic_pkt(ngtcp2_path *path, ngtcp2_pkt_info *pi,
                                    uint8_t *dest, size_t destlen,
                                    ngtcp2_tstamp ts) {
  std::array<nghttp3_vec, 16> vec;
  auto s = static_cast<Http3Session *>(session.get());

  for (;;) {
    int64_t stream_id = -1;
    int fin = 0;
    ssize_t sveccnt = 0;

    if (session && ngtcp2_conn_get_max_data_left(quic.conn)) {
      sveccnt = s->write_stream(stream_id, fin, vec.data(), vec.size());
      if (sveccnt == -1) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
      }
    }

    ngtcp2_ssize ndatalen;
    auto v = vec.data();
    auto vcnt = static_cast<size_t>(sveccnt);

    uint32_t flags =
      NGTCP2_WRITE_STREAM_FLAG_MORE | NGTCP2_WRITE_STREAM_FLAG_PADDING;
    if (fin) {
      flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;
    }

    auto nwrite = ngtcp2_conn_writev_stream(
      quic.conn, path, nullptr, dest, destlen, &ndatalen, flags, stream_id,
      reinterpret_cast<const ngtcp2_vec *>(v), vcnt, ts);
    if (nwrite < 0) {
      switch (nwrite) {
      case NGTCP2_ERR_STREAM_DATA_BLOCKED:
        assert(ndatalen == -1);
        s->block_stream(stream_id);
        continue;
      case NGTCP2_ERR_STREAM_SHUT_WR:
        assert(ndatalen == -1);
        s->shutdown_stream_write(stream_id);
        continue;
      case NGTCP2_ERR_WRITE_MORE:
        assert(ndatalen >= 0);
        if (s->add_write_offset(stream_id, as_unsigned(ndatalen)) != 0) {
          return NGTCP2_ERR_CALLBACK_FAILURE;
        }
        continue;
      }

      ngtcp2_ccerr_set_liberr(&quic.last_error, static_cast<int>(nwrite),
                              nullptr, 0);

      return NGTCP2_ERR_CALLBACK_FAILURE;
    }

    if (ndatalen >= 0 &&
        s->add_write_offset(stream_id, as_unsigned(ndatalen)) != 0) {
      return NGTCP2_ERR_CALLBACK_FAILURE;
    }

    return nwrite;
  }
}

int Client::write_quic() {
  int rv;

  ev_io_stop(worker->loop, &wev);

  if (quic.close_requested) {
    return -1;
  }

  if (quic.tx.send_blocked) {
    rv = send_blocked_packet();
    if (rv != 0) {
      return -1;
    }

    if (quic.tx.send_blocked) {
      return 0;
    }
  }

  auto txbuf = std::span{quic.tx.data.get(), QUIC_TX_DATALEN};
  ngtcp2_path_storage ps;
  size_t gso_size = 0;

  ngtcp2_path_storage_zero(&ps);

  auto nwrite = ngtcp2_conn_write_aggregate_pkt(
    quic.conn, &ps.path, nullptr, txbuf.data(), txbuf.size(), &gso_size,
    h2load::write_pkt, quic_timestamp());
  if (nwrite < 0) {
    return -1;
  }

  quic_restart_pkt_timer();

  if (nwrite == 0) {
    return 0;
  }

  write_udp_or_blocked(ps.path, txbuf.first(static_cast<size_t>(nwrite)),
                       gso_size);

  return 0;
}

void Client::write_udp_or_blocked(const ngtcp2_path &path,
                                  std::span<const uint8_t> data,
                                  size_t gso_size) {
  auto rest = write_udp(path.remote.addr, path.remote.addrlen, data, gso_size);
  if (!rest.empty()) {
    on_send_blocked(path.remote, data, gso_size);
  }
}

void Client::on_send_blocked(const ngtcp2_addr &remote_addr,
                             std::span<const uint8_t> data, size_t gso_size) {
  assert(!quic.tx.send_blocked);

  quic.tx.send_blocked = true;

  auto &p = quic.tx.blocked;

  memcpy(&p.remote_addr.su, remote_addr.addr, remote_addr.addrlen);

  p.remote_addr.len = remote_addr.addrlen;
  p.data = data;
  p.gso_size = gso_size;

  signal_write();
}

int Client::send_blocked_packet() {
  assert(quic.tx.send_blocked);

  auto &p = quic.tx.blocked;

  auto rest =
    write_udp(&p.remote_addr.su.sa, p.remote_addr.len, p.data, p.gso_size);
  if (!rest.empty()) {
    p.data = rest;

    signal_write();

    return 0;
  }

  quic.tx.send_blocked = false;

  return 0;
}

} // namespace h2load
