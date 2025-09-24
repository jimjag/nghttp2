/*
 * nghttp2 - HTTP/2 C Library
 *
 * Copyright (c) 2012 Tatsuhiro Tsujikawa
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
#ifndef SHRPX_CONNECTION_HANDLER_H
#define SHRPX_CONNECTION_HANDLER_H

#include "shrpx.h"

#include <sys/types.h>
#ifdef HAVE_SYS_SOCKET_H
#  include <sys/socket.h>
#endif // defined(HAVE_SYS_SOCKET_H)

#include <mutex>
#include <memory>
#include <vector>
#include <random>
#ifndef NOTHREADS
#  include <future>
#endif // !defined(NOTHREADS)

#ifdef HAVE_LIBBPF
#  include <bpf/libbpf.h>
#endif // defined(HAVE_LIBBPF)

#include "ssl_compat.h"

#ifdef NGHTTP2_OPENSSL_IS_WOLFSSL
#  include <wolfssl/options.h>
#  include <wolfssl/openssl/ssl.h>
#else // !defined(NGHTTP2_OPENSSL_IS_WOLFSSL)
#  include <openssl/ssl.h>
#endif // !defined(NGHTTP2_OPENSSL_IS_WOLFSSL)

#include <ev.h>

#ifdef HAVE_NEVERBLEED
#  include <neverbleed.h>
#endif // defined(HAVE_NEVERBLEED)

#include "shrpx_downstream_connection_pool.h"
#include "shrpx_config.h"

namespace shrpx {

class Http2Session;
class ConnectBlocker;
class Worker;
struct WorkerStat;
struct TicketKeys;
class MemcachedDispatcher;
struct UpstreamAddr;

namespace tls {

class CertLookupTree;

} // namespace tls

// SerialEvent is an event sent from Worker thread.
enum class SerialEventType {
  NONE,
  REPLACE_DOWNSTREAM,
};

struct SerialEvent {
  // ctor for event uses DownstreamConfig
  SerialEvent(SerialEventType type,
              const std::shared_ptr<DownstreamConfig> &downstreamconf)
    : type(type), downstreamconf(downstreamconf) {}

  SerialEventType type;
  std::shared_ptr<DownstreamConfig> downstreamconf;
};

#ifdef ENABLE_HTTP3
#  ifdef HAVE_LIBBPF
struct BPFRef {
  bpf_object *obj;
  bpf_map *reuseport_array;
  bpf_map *worker_id_map;
};
#  endif // defined(HAVE_LIBBPF)

// QUIC IPC message type.
enum class QUICIPCType {
  NONE,
  // Send forwarded QUIC UDP datagram and its metadata.
  DGRAM_FORWARD,
};

// WorkerProcesses which are in graceful shutdown period.
struct QUICLingeringWorkerProcess {
  QUICLingeringWorkerProcess(std::vector<WorkerID> worker_ids, int quic_ipc_fd)
    : worker_ids{std::move(worker_ids)}, quic_ipc_fd{quic_ipc_fd} {}

  std::vector<WorkerID> worker_ids;
  // Socket to send QUIC IPC message to this worker process.
  int quic_ipc_fd;
};
#endif // defined(ENABLE_HTTP3)

class ConnectionHandler {
public:
  ConnectionHandler(struct ev_loop *loop, std::mt19937 &gen);
  ~ConnectionHandler();
  // Creates Worker object for single threaded configuration.
  int create_single_worker();
  // Creates |num| Worker objects for multi threaded configuration.
  // The |num| must be strictly more than 1.
  int create_worker_thread(size_t num);
  void
  set_ticket_keys_to_worker(const std::shared_ptr<TicketKeys> &ticket_keys);
  void worker_reopen_log_files();
  void set_ticket_keys(std::shared_ptr<TicketKeys> ticket_keys);
  const std::shared_ptr<TicketKeys> &get_ticket_keys() const;
  struct ev_loop *get_loop() const;
  Worker *get_single_worker() const;
  void graceful_shutdown_worker();
  void set_graceful_shutdown(bool f);
  bool get_graceful_shutdown() const;
  void join_worker();

  void set_tls_ticket_key_memcached_dispatcher(
    std::unique_ptr<MemcachedDispatcher> dispatcher);

  MemcachedDispatcher *get_tls_ticket_key_memcached_dispatcher() const;
  void on_tls_ticket_key_network_error(ev_timer *w);
  void on_tls_ticket_key_not_found(ev_timer *w);
  void
  on_tls_ticket_key_get_success(const std::shared_ptr<TicketKeys> &ticket_keys,
                                ev_timer *w);
  void schedule_next_tls_ticket_key_memcached_get(ev_timer *w);
  SSL_CTX *create_tls_ticket_key_memcached_ssl_ctx();
  // Returns the SSL_CTX at all_ssl_ctx_[idx].  This does not perform
  // array bound checking.
  SSL_CTX *get_ssl_ctx(size_t idx) const;

  const std::vector<SSL_CTX *> &get_indexed_ssl_ctx(size_t idx) const;
#ifdef ENABLE_HTTP3
  const std::vector<SSL_CTX *> &get_quic_indexed_ssl_ctx(size_t idx) const;

  int forward_quic_packet(const UpstreamAddr *faddr, const Address &remote_addr,
                          const Address &local_addr, const ngtcp2_pkt_info &pi,
                          const WorkerID &wid, std::span<const uint8_t> data);

  void set_quic_keying_materials(std::shared_ptr<QUICKeyingMaterials> qkms);
  const std::shared_ptr<QUICKeyingMaterials> &get_quic_keying_materials() const;

  void set_worker_ids(std::vector<WorkerID> worker_ids);
  Worker *find_worker(const WorkerID &wid) const;

  void set_quic_lingering_worker_processes(
    const std::vector<QUICLingeringWorkerProcess> &quic_lwps);

  // Return matching QUICLingeringWorkerProcess which has a Worker ID
  // such that |dcid| starts with it.  If no such
  // QUICLingeringWorkerProcess, it returns nullptr.
  QUICLingeringWorkerProcess *
  match_quic_lingering_worker_process_worker_id(const WorkerID &wid);

  int forward_quic_packet_to_lingering_worker_process(
    QUICLingeringWorkerProcess *quic_lwp, const Address &remote_addr,
    const Address &local_addr, const ngtcp2_pkt_info &pi,
    std::span<const uint8_t> data);

  void set_quic_ipc_fd(int fd);

  int quic_ipc_read();

#  ifdef HAVE_LIBBPF
  std::vector<BPFRef> &get_quic_bpf_refs();
  void unload_bpf_objects();
#  endif // defined(HAVE_LIBBPF)
#endif   // defined(ENABLE_HTTP3)

#ifdef HAVE_NEVERBLEED
  void set_neverbleed(neverbleed_t *nb);
#endif // defined(HAVE_NEVERBLEED)

  // Send SerialEvent SerialEventType::REPLACE_DOWNSTREAM to this
  // object.
  void send_replace_downstream(
    const std::shared_ptr<DownstreamConfig> &downstreamconf);
  // Internal function to send |ev| to this object.
  void send_serial_event(SerialEvent ev);
  // Handles SerialEvents received.
  void handle_serial_event();
  // Sends WorkerEvent to make them replace downstream.
  void
  worker_replace_downstream(std::shared_ptr<DownstreamConfig> downstreamconf);

private:
  // Stores all SSL_CTX objects.
  std::vector<SSL_CTX *> all_ssl_ctx_;
  // Stores all SSL_CTX objects in a way that its index is stored in
  // cert_tree.  The SSL_CTXs stored in the same index share the same
  // hostname, but could have different signature algorithm.  The
  // selection among them are performed by hostname presented by SNI,
  // and signature algorithm presented by client.
  std::vector<std::vector<SSL_CTX *>> indexed_ssl_ctx_;
#ifdef ENABLE_HTTP3
  std::vector<WorkerID> worker_ids_;
  std::vector<WorkerID> lingering_worker_ids_;
  int quic_ipc_fd_;
  std::vector<QUICLingeringWorkerProcess> quic_lingering_worker_processes_;
#  ifdef HAVE_LIBBPF
  std::vector<BPFRef> quic_bpf_refs_;
#  endif // defined(HAVE_LIBBPF)
  std::shared_ptr<QUICKeyingMaterials> quic_keying_materials_;
  std::vector<SSL_CTX *> quic_all_ssl_ctx_;
  std::vector<std::vector<SSL_CTX *>> quic_indexed_ssl_ctx_;
#endif // defined(ENABLE_HTTP3)
  std::mt19937 &gen_;
  // ev_loop for each worker
  std::vector<struct ev_loop *> worker_loops_;
  // Worker instances when multi threaded mode (-nN, N >= 2) is used.
  // If at least one frontend enables API request, we allocate 1
  // additional worker dedicated to API request .
  std::vector<std::unique_ptr<Worker>> workers_;
  // mutex for serial event resive buffer handling
  std::mutex serial_event_mu_;
  // SerialEvent receive buffer
  std::vector<SerialEvent> serial_events_;
  // Worker instance used when single threaded mode (-n1) is used.
  // Otherwise, nullptr and workers_ has instances of Worker instead.
  std::unique_ptr<Worker> single_worker_;
  std::unique_ptr<tls::CertLookupTree> cert_tree_;
#ifdef ENABLE_HTTP3
  std::unique_ptr<tls::CertLookupTree> quic_cert_tree_;
#endif // defined(ENABLE_HTTP3)
  std::unique_ptr<MemcachedDispatcher> tls_ticket_key_memcached_dispatcher_;
  // Current TLS session ticket keys.  Note that TLS connection does
  // not refer to this field directly.  They use TicketKeys object in
  // Worker object.
  std::shared_ptr<TicketKeys> ticket_keys_;
  struct ev_loop *loop_;
#ifdef HAVE_NEVERBLEED
  neverbleed_t *nb_;
#endif // defined(HAVE_NEVERBLEED)
  ev_async thread_join_asyncev_;
  ev_async serial_event_asyncev_;
#ifndef NOTHREADS
  std::future<void> thread_join_fut_;
#endif // defined(NOTHREADS)
  size_t tls_ticket_key_memcached_get_retry_count_;
  size_t tls_ticket_key_memcached_fail_count_;
  unsigned int worker_round_robin_cnt_;
  bool graceful_shutdown_;
};

} // namespace shrpx

#endif // !defined(SHRPX_CONNECTION_HANDLER_H)
