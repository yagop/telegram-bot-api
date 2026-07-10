//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2026
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "telegram-bot-api/HttpConnection.h"

#include "telegram-bot-api/Query.h"

#include "td/net/HttpHeaderCreator.h"

#include "td/utils/common.h"
#include "td/utils/JsonBuilder.h"
#include "td/utils/logging.h"
#include "td/utils/Parser.h"
#include "td/utils/Promise.h"
#include "td/utils/SliceBuilder.h"

namespace telegram_bot_api {

void HttpConnection::handle(td::unique_ptr<td::HttpQuery> http_query,
                            td::ActorOwn<td::HttpInboundConnection> connection) {
  CHECK(connection_.empty());
  connection_ = std::move(connection);

  LOG(DEBUG) << "Handle " << *http_query;
  td::Parser url_path_parser(http_query->url_path_);
  if (url_path_parser.peek_char() != '/') {
    return send_http_error(404, "Not Found: absolute URI is specified in the Request-Line");
  }

  if (http_query->url_path_ == "/stats") {
    auto promise = td::PromiseCreator::lambda([actor_id = actor_id(this)](td::Result<td::BufferSlice> result) {
      send_closure(actor_id, &HttpConnection::on_stats_result, std::move(result));
    });
    send_closure(client_manager_, &ClientManager::get_stats, std::move(promise), http_query->get_args(), true);
    return;
  }

  if (!url_path_parser.try_skip("/bot")) {
    return send_http_error(404, "Not Found");
  }

  auto token = url_path_parser.read_till('/');
  bool is_test_dc = false;
  if (url_path_parser.try_skip("/test")) {
    is_test_dc = true;
  }
  url_path_parser.skip('/');
  if (url_path_parser.status().is_error()) {
    return send_http_error(404, "Not Found");
  }

  auto method = url_path_parser.data();
  auto query = td::make_unique<Query>(std::move(http_query->container_), token, is_test_dc, method,
                                      std::move(http_query->args_), std::move(http_query->headers_),
                                      std::move(http_query->files_), shared_data_, http_query->peer_address_, false);

  auto promise = td::PromiseCreator::lambda([actor_id = actor_id(this)](td::Result<td::unique_ptr<Query>> r_query) {
    send_closure(actor_id, &HttpConnection::on_query_finished, std::move(r_query));
  });
  auto promised_query = PromisedQueryPtr(query.release(), PromiseDeleter(std::move(promise)));
  send_closure(client_manager_, &ClientManager::send, std::move(promised_query));
}

void HttpConnection::on_query_finished(td::Result<td::unique_ptr<Query>> r_query) {
  LOG_CHECK(r_query.is_ok()) << r_query.error();

  auto query = r_query.move_as_ok();
  send_response(query->http_status_code(), std::move(query->answer()), query->retry_after());
}

void HttpConnection::on_stats_result(td::Result<td::BufferSlice> result) {
  if (result.is_error()) {
    return send_http_error(500, "Internal Server Error");
  }
  send_response(200, result.move_as_ok(), 0);
}

void HttpConnection::send_response(int http_status_code, td::BufferSlice &&content, int retry_after) {
  td::HttpHeaderCreator hc;
  hc.init_status_line(http_status_code);
  hc.set_keep_alive();
  hc.set_content_type("application/json");
  if (retry_after > 0) {
    hc.add_header("Retry-After", PSLICE() << retry_after);
  }
  hc.set_content_size(content.size());

  auto r_header = hc.finish();
  LOG(DEBUG) << "Response headers: " << r_header.ok();
  if (r_header.is_error()) {
    LOG(ERROR) << "Bad response headers";
    send_closure(std::move(connection_), &td::HttpInboundConnection::write_error, r_header.move_as_error());
    return;
  }
  LOG(DEBUG) << "Send result: " << content;

  send_closure(connection_, &td::HttpInboundConnection::write_next_noflush, td::BufferSlice(r_header.ok()));
  send_closure(connection_, &td::HttpInboundConnection::write_next_noflush, std::move(content));
  send_closure(std::move(connection_), &td::HttpInboundConnection::write_ok);
}

void HttpConnection::send_http_error(int http_status_code, td::CSlice description) {
  send_response(http_status_code, td::json_encode<td::BufferSlice>(JsonQueryError(http_status_code, description)), 0);
}

}  // namespace telegram_bot_api
