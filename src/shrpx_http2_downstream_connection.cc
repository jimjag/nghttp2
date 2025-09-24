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
#include "shrpx_http2_downstream_connection.h"

#ifdef HAVE_UNISTD_H
#  include <unistd.h>
#endif // defined(HAVE_UNISTD_H)

#include "llhttp.h"

#include "shrpx_client_handler.h"
#include "shrpx_upstream.h"
#include "shrpx_downstream.h"
#include "shrpx_config.h"
#include "shrpx_error.h"
#include "shrpx_http.h"
#include "shrpx_http2_session.h"
#include "shrpx_worker.h"
#include "shrpx_log.h"
#include "http2.h"
#include "util.h"
#include "ssl_compat.h"

using namespace nghttp2;

namespace shrpx {

Http2DownstreamConnection::Http2DownstreamConnection(Http2Session *http2session)
  : dlnext(nullptr),
    dlprev(nullptr),
    http2session_(http2session),
    sd_(nullptr) {}

Http2DownstreamConnection::~Http2DownstreamConnection() {
  if (LOG_ENABLED(INFO)) {
    DCLOG(INFO, this) << "Deleting";
  }
  if (downstream_) {
    downstream_->disable_downstream_rtimer();
    downstream_->disable_downstream_wtimer();

    uint32_t error_code;
    if (downstream_->get_request_state() == DownstreamState::STREAM_CLOSED &&
        downstream_->get_upgraded()) {
      // For upgraded connection, send NO_ERROR.  Should we consider
      // request states other than DownstreamState::STREAM_CLOSED ?
      error_code = NGHTTP2_NO_ERROR;
    } else {
      error_code = NGHTTP2_INTERNAL_ERROR;
    }

    if (http2session_->get_state() == Http2SessionState::CONNECTED &&
        downstream_->get_downstream_stream_id() != -1) {
      submit_rst_stream(downstream_, error_code);

      auto &resp = downstream_->response();

      http2session_->consume(
        static_cast<int32_t>(downstream_->get_downstream_stream_id()),
        resp.unconsumed_body_length);

      resp.unconsumed_body_length = 0;

      http2session_->signal_write();
    }
  }
  http2session_->remove_downstream_connection(this);

  if (LOG_ENABLED(INFO)) {
    DCLOG(INFO, this) << "Deleted";
  }
}

int Http2DownstreamConnection::attach_downstream(Downstream *downstream) {
  if (LOG_ENABLED(INFO)) {
    DCLOG(INFO, this) << "Attaching to DOWNSTREAM:" << downstream;
  }
  http2session_->add_downstream_connection(this);
  http2session_->signal_write();

  downstream_ = downstream;
  downstream_->reset_downstream_rtimer();

  auto &req = downstream_->request();

  // HTTP/2 disables HTTP Upgrade.
  if (req.method != HTTP_CONNECT && req.connect_proto == ConnectProto::NONE) {
    req.upgrade_request = false;
  }

  return 0;
}

void Http2DownstreamConnection::detach_downstream(Downstream *downstream) {
  if (LOG_ENABLED(INFO)) {
    DCLOG(INFO, this) << "Detaching from DOWNSTREAM:" << downstream;
  }

  auto &resp = downstream_->response();

  if (downstream_->get_downstream_stream_id() != -1) {
    if (submit_rst_stream(downstream) == 0) {
      http2session_->signal_write();
    }

    http2session_->consume(
      static_cast<int32_t>(downstream_->get_downstream_stream_id()),
      resp.unconsumed_body_length);

    resp.unconsumed_body_length = 0;

    http2session_->signal_write();
  }

  downstream->disable_downstream_rtimer();
  downstream->disable_downstream_wtimer();
  downstream_ = nullptr;
}

int Http2DownstreamConnection::submit_rst_stream(Downstream *downstream,
                                                 uint32_t error_code) {
  int rv = -1;
  if (http2session_->get_state() == Http2SessionState::CONNECTED &&
      downstream->get_downstream_stream_id() != -1) {
    switch (downstream->get_response_state()) {
    case DownstreamState::MSG_RESET:
    case DownstreamState::MSG_BAD_HEADER:
    case DownstreamState::MSG_COMPLETE:
      break;
    default:
      if (LOG_ENABLED(INFO)) {
        DCLOG(INFO, this) << "Submit RST_STREAM for DOWNSTREAM:" << downstream
                          << ", stream_id="
                          << downstream->get_downstream_stream_id()
                          << ", error_code=" << error_code;
      }
      rv = http2session_->submit_rst_stream(
        static_cast<int32_t>(downstream->get_downstream_stream_id()),
        error_code);
    }
  }
  return rv;
}

namespace {
nghttp2_ssize http2_data_read_callback(nghttp2_session *session,
                                       int32_t stream_id, uint8_t *buf,
                                       size_t length, uint32_t *data_flags,
                                       nghttp2_data_source *source,
                                       void *user_data) {
  int rv;
  auto sd = static_cast<StreamData *>(
    nghttp2_session_get_stream_user_data(session, stream_id));
  if (!sd || !sd->dconn) {
    return NGHTTP2_ERR_DEFERRED;
  }
  auto dconn = sd->dconn;
  auto downstream = dconn->get_downstream();
  if (!downstream) {
    // In this case, RST_STREAM should have been issued. But depending
    // on the priority, DATA frame may come first.
    return NGHTTP2_ERR_DEFERRED;
  }
  const auto &req = downstream->request();
  auto input = downstream->get_request_buf();

  auto nread = std::min(input->rleft(), length);
  auto input_empty = input->rleft() == nread;

  *data_flags |= NGHTTP2_DATA_FLAG_NO_COPY;

  if (input_empty &&
      downstream->get_request_state() == DownstreamState::MSG_COMPLETE &&
      // If connection is upgraded, don't set EOF flag, since HTTP/1
      // will set MSG_COMPLETE to request state after upgrade response
      // header is seen.
      (!req.upgrade_request ||
       (downstream->get_response_state() == DownstreamState::HEADER_COMPLETE &&
        !downstream->get_upgraded()))) {
    *data_flags |= NGHTTP2_DATA_FLAG_EOF;

    const auto &trailers = req.fs.trailers();
    if (!trailers.empty()) {
      std::vector<nghttp2_nv> nva;
      nva.reserve(trailers.size());
      http2::copy_headers_to_nva_nocopy(nva, trailers, http2::HDOP_STRIP_ALL);
      if (!nva.empty()) {
        rv = nghttp2_submit_trailer(session, stream_id, nva.data(), nva.size());
        if (rv != 0) {
          if (nghttp2_is_fatal(rv)) {
            return NGHTTP2_ERR_CALLBACK_FAILURE;
          }
        } else {
          *data_flags |= NGHTTP2_DATA_FLAG_NO_END_STREAM;
        }
      }
    }
  }

  if (nread == 0 && (*data_flags & NGHTTP2_DATA_FLAG_EOF) == 0) {
    downstream->disable_downstream_wtimer();

    return NGHTTP2_ERR_DEFERRED;
  }

  return as_signed(nread);
}
} // namespace

int Http2DownstreamConnection::push_request_headers() {
  int rv;
  if (!downstream_) {
    return 0;
  }
  if (!http2session_->can_push_request(downstream_)) {
    // The HTTP2 session to the backend has not been established or
    // connection is now being checked.  This function will be called
    // again just after it is established.
    downstream_->set_request_pending(true);
    http2session_->start_checking_connection();
    return 0;
  }

  downstream_->set_request_pending(false);

  const auto &req = downstream_->request();

  if (req.connect_proto != ConnectProto::NONE &&
      !http2session_->get_allow_connect_proto()) {
    return -1;
  }

  auto &balloc = downstream_->get_block_allocator();

  auto config = get_config();
  auto &httpconf = config->http;
  auto &http2conf = config->http2;

  auto no_host_rewrite = httpconf.no_host_rewrite || config->http2_proxy ||
                         req.regular_connect_method();

  // http2session_ has already in CONNECTED state, so we can get
  // addr_idx here.

  // For HTTP/1.0 request, there is no authority in request.  In that
  // case, we use backend server's host nonetheless.
  auto authority = http2session_->get_addr()->hostport;

  if (no_host_rewrite && !req.authority.empty()) {
    authority = req.authority;
  }

  downstream_->set_request_downstream_host(authority);

  size_t num_cookies = 0;
  if (!http2conf.no_cookie_crumbling) {
    num_cookies = downstream_->count_crumble_request_cookie();
  }

  // 11 means:
  // 1. :method
  // 2. :scheme
  // 3. :path
  // 4. :authority (or host)
  // 5. :protocol (optional)
  // 6. via (optional)
  // 7. x-forwarded-for (optional)
  // 8. x-forwarded-proto (optional)
  // 9. te (optional)
  // 10. forwarded (optional)
  // 11. early-data (optional)
  auto nva = std::vector<nghttp2_nv>();
  nva.reserve(req.fs.headers().size() + 11 + num_cookies +
              httpconf.add_request_headers.size());

  if (req.connect_proto == ConnectProto::WEBSOCKET) {
    nva.push_back(http2::make_field(":method"sv, "CONNECT"sv));
    nva.push_back(http2::make_field(":protocol"sv, "websocket"sv));
  } else {
    nva.push_back(
      http2::make_field(":method"sv, http2::to_method_string(req.method)));
  }

  if (!req.regular_connect_method()) {
    assert(!req.scheme.empty());

    auto addr = http2session_->get_addr();
    assert(addr);
    // We will handle more protocol scheme upgrade in the future.
    if (addr->tls && addr->upgrade_scheme && req.scheme == "http"sv) {
      nva.push_back(http2::make_field(":scheme"sv, "https"sv));
    } else {
      nva.push_back(http2::make_field(":scheme"sv, req.scheme));
    }

    if (req.method == HTTP_OPTIONS && req.path.empty()) {
      nva.push_back(http2::make_field(":path"sv, "*"sv));
    } else {
      nva.push_back(http2::make_field(":path"sv, req.path));
    }

    if (!req.no_authority || req.connect_proto != ConnectProto::NONE) {
      nva.push_back(http2::make_field(":authority"sv, authority));
    } else {
      nva.push_back(http2::make_field("host"sv, authority));
    }
  } else {
    nva.push_back(http2::make_field(":authority"sv, authority));
  }

  auto &fwdconf = httpconf.forwarded;
  auto &xffconf = httpconf.xff;
  auto &xfpconf = httpconf.xfp;
  auto &earlydataconf = httpconf.early_data;

  uint32_t build_flags =
    (fwdconf.strip_incoming ? http2::HDOP_STRIP_FORWARDED : 0) |
    (xffconf.strip_incoming ? http2::HDOP_STRIP_X_FORWARDED_FOR : 0) |
    (xfpconf.strip_incoming ? http2::HDOP_STRIP_X_FORWARDED_PROTO : 0) |
    (earlydataconf.strip_incoming ? http2::HDOP_STRIP_EARLY_DATA : 0) |
    http2::HDOP_STRIP_SEC_WEBSOCKET_KEY;

  http2::copy_headers_to_nva_nocopy(nva, req.fs.headers(), build_flags);

  if (!http2conf.no_cookie_crumbling) {
    downstream_->crumble_request_cookie(nva);
  }

  auto upstream = downstream_->get_upstream();
  auto handler = upstream->get_client_handler();

#if defined(NGHTTP2_GENUINE_OPENSSL) ||                                        \
  defined(NGHTTP2_OPENSSL_IS_BORINGSSL) || defined(NGHTTP2_OPENSSL_IS_WOLFSSL)
  auto conn = handler->get_connection();

  if (conn->tls.ssl && !SSL_is_init_finished(conn->tls.ssl)) {
    nva.push_back(http2::make_field("early-data"sv, "1"sv));
  }
#endif // defined(NGHTTP2_GENUINE_OPENSSL) ||
       // defined(NGHTTP2_OPENSSL_IS_BORINGSSL) ||
       // defined(NGHTTP2_OPENSSL_IS_WOLFSSL)

  auto fwd =
    fwdconf.strip_incoming ? nullptr : req.fs.header(http2::HD_FORWARDED);

  if (fwdconf.params) {
    auto params = fwdconf.params;

    if (config->http2_proxy || req.regular_connect_method()) {
      params &= static_cast<uint32_t>(~FORWARDED_PROTO);
    }

    auto value = http::create_forwarded(
      balloc, params, handler->get_forwarded_by(), handler->get_forwarded_for(),
      req.authority, req.scheme);

    if (fwd || !value.empty()) {
      if (fwd) {
        if (value.empty()) {
          value = fwd->value;
        } else {
          value = concat_string_ref(balloc, fwd->value, ", "sv, value);
        }
      }

      nva.push_back(http2::make_field("forwarded"sv, value));
    }
  } else if (fwd) {
    nva.push_back(http2::make_field("forwarded"sv, fwd->value));
  }

  auto xff =
    xffconf.strip_incoming ? nullptr : req.fs.header(http2::HD_X_FORWARDED_FOR);

  if (xffconf.add) {
    std::string_view xff_value;
    const auto &addr = upstream->get_client_handler()->get_ipaddr();
    if (xff) {
      xff_value = concat_string_ref(balloc, xff->value, ", "sv, addr);
    } else {
      xff_value = addr;
    }
    nva.push_back(http2::make_field("x-forwarded-for"sv, xff_value));
  } else if (xff) {
    nva.push_back(http2::make_field("x-forwarded-for"sv, xff->value));
  }

  if (!config->http2_proxy && !req.regular_connect_method()) {
    auto xfp = xfpconf.strip_incoming
                 ? nullptr
                 : req.fs.header(http2::HD_X_FORWARDED_PROTO);

    if (xfpconf.add) {
      std::string_view xfp_value;
      // We use same protocol with :scheme header field
      if (xfp) {
        xfp_value = concat_string_ref(balloc, xfp->value, ", "sv, req.scheme);
      } else {
        xfp_value = req.scheme;
      }
      nva.push_back(http2::make_field("x-forwarded-proto"sv, xfp_value));
    } else if (xfp) {
      nva.push_back(http2::make_field("x-forwarded-proto"sv, xfp->value));
    }
  }

  auto via = req.fs.header(http2::HD_VIA);
  if (httpconf.no_via) {
    if (via) {
      nva.push_back(http2::make_field("via"sv, (*via).value));
    }
  } else {
    size_t vialen = 16;
    if (via) {
      vialen += via->value.size() + 2;
    }

    auto iov = make_byte_ref(balloc, vialen + 1);
    auto p = std::ranges::begin(iov);

    if (via) {
      p = std::ranges::copy(via->value, p).out;
      p = std::ranges::copy(", "sv, p).out;
    }
    p = http::create_via_header_value(p, req.http_major, req.http_minor);
    *p = '\0';

    nva.push_back(
      http2::make_field("via"sv, as_string_view(std::ranges::begin(iov), p)));
  }

  auto te = req.fs.header(http2::HD_TE);
  // HTTP/1 upstream request can contain keyword other than
  // "trailers".  We just forward "trailers".
  // TODO more strict handling required here.
  if (te && http2::contains_trailers(te->value)) {
    nva.push_back(http2::make_field("te"sv, "trailers"sv));
  }

  for (auto &p : httpconf.add_request_headers) {
    nva.push_back(http2::make_field(p.name, p.value));
  }

  if (LOG_ENABLED(INFO)) {
    std::stringstream ss;
    for (auto &nv : nva) {
      auto name = as_string_view(nv.name, nv.namelen);

      if ("authorization"sv == name) {
        ss << TTY_HTTP_HD << name << TTY_RST << ": <redacted>\n";
        continue;
      }
      ss << TTY_HTTP_HD << name << TTY_RST << ": "
         << as_string_view(nv.value, nv.valuelen) << "\n";
    }
    DCLOG(INFO, this) << "HTTP request headers\n" << ss.str();
  }

  auto transfer_encoding = req.fs.header(http2::HD_TRANSFER_ENCODING);

  nghttp2_data_provider2 *data_prdptr = nullptr;
  nghttp2_data_provider2 data_prd;

  // Add body as long as transfer-encoding is given even if
  // req.fs.content_length == 0 to forward trailer fields.
  if (req.method == HTTP_CONNECT || req.connect_proto != ConnectProto::NONE ||
      transfer_encoding || req.fs.content_length > 0 || req.http2_expect_body) {
    // Request-body is expected.
    data_prd = {{}, http2_data_read_callback};
    data_prdptr = &data_prd;
  }

  rv = http2session_->submit_request(this, nva.data(), nva.size(), data_prdptr);
  if (rv != 0) {
    DCLOG(FATAL, this) << "nghttp2_submit_request() failed";
    return -1;
  }

  if (data_prdptr) {
    downstream_->reset_downstream_wtimer();
  }

  http2session_->signal_write();
  return 0;
}

int Http2DownstreamConnection::push_upload_data_chunk(const uint8_t *data,
                                                      size_t datalen) {
  if (!downstream_->get_request_header_sent()) {
    auto output = downstream_->get_blocked_request_buf();
    auto &req = downstream_->request();
    output->append(data, datalen);
    req.unconsumed_body_length += datalen;
    return 0;
  }

  int rv;
  auto output = downstream_->get_request_buf();
  output->append(data, datalen);
  if (downstream_->get_downstream_stream_id() != -1) {
    rv = http2session_->resume_data(this);
    if (rv != 0) {
      return -1;
    }

    downstream_->ensure_downstream_wtimer();

    http2session_->signal_write();
  }
  return 0;
}

int Http2DownstreamConnection::end_upload_data() {
  if (!downstream_->get_request_header_sent()) {
    downstream_->set_blocked_request_data_eof(true);
    return 0;
  }

  int rv;
  if (downstream_->get_downstream_stream_id() != -1) {
    rv = http2session_->resume_data(this);
    if (rv != 0) {
      return -1;
    }

    downstream_->ensure_downstream_wtimer();

    http2session_->signal_write();
  }
  return 0;
}

int Http2DownstreamConnection::resume_read(IOCtrlReason reason,
                                           size_t consumed) {
  int rv;

  if (http2session_->get_state() != Http2SessionState::CONNECTED) {
    return 0;
  }

  if (!downstream_ || downstream_->get_downstream_stream_id() == -1) {
    return 0;
  }

  if (consumed > 0) {
    rv = http2session_->consume(
      static_cast<int32_t>(downstream_->get_downstream_stream_id()), consumed);

    if (rv != 0) {
      return -1;
    }

    auto &resp = downstream_->response();

    resp.unconsumed_body_length -= consumed;

    http2session_->signal_write();
  }

  return 0;
}

int Http2DownstreamConnection::on_read() { return 0; }

int Http2DownstreamConnection::on_write() { return 0; }

void Http2DownstreamConnection::attach_stream_data(StreamData *sd) {
  // It is possible sd->dconn is not NULL. sd is detached when
  // on_stream_close_callback. Before that, after MSG_COMPLETE is set
  // to Downstream::set_response_state(), upstream's readcb is called
  // and execution path eventually could reach here. Since the
  // response was already handled, we just detach sd.
  detach_stream_data();
  sd_ = sd;
  sd_->dconn = this;
}

StreamData *Http2DownstreamConnection::detach_stream_data() {
  if (sd_) {
    auto sd = sd_;
    sd_ = nullptr;
    sd->dconn = nullptr;
    return sd;
  }
  return nullptr;
}

int Http2DownstreamConnection::on_timeout() {
  if (!downstream_) {
    return 0;
  }

  return submit_rst_stream(downstream_, NGHTTP2_NO_ERROR);
}

const std::shared_ptr<DownstreamAddrGroup> &
Http2DownstreamConnection::get_downstream_addr_group() const {
  return http2session_->get_downstream_addr_group();
}

DownstreamAddr *Http2DownstreamConnection::get_addr() const { return nullptr; }

} // namespace shrpx
