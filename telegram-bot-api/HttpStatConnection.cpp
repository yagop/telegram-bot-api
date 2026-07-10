//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2026
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "telegram-bot-api/HttpStatConnection.h"

#include "td/net/HttpHeaderCreator.h"

#include "td/utils/common.h"
#include "td/utils/Promise.h"

namespace telegram_bot_api {

void HttpStatConnection::handle(td::unique_ptr<td::HttpQuery> http_query,
                                td::ActorOwn<td::HttpInboundConnection> connection) {
  CHECK(connection_.empty());
  connection_ = std::move(connection);

  auto promise = td::PromiseCreator::lambda([actor_id = actor_id(this)](td::Result<td::BufferSlice> result) {
    send_closure(actor_id, &HttpStatConnection::on_result, std::move(result));
  });
  send_closure(client_manager_, &ClientManager::get_stats, std::move(promise), http_query->get_args(), false);
}

void HttpStatConnection::on_result(td::Result<td::BufferSlice> result) {
  if (result.is_error()) {
    send_closure(connection_.release(), &td::HttpInboundConnection::write_error,
                 td::Status::Error(500, "Internal Server Error: closing"));
    return;
  }

  auto content = result.move_as_ok();
  td::HttpHeaderCreator hc;
  hc.init_status_line(200);
  hc.set_keep_alive();
  hc.set_content_type("text/plain");
  hc.set_content_size(content.size());

  auto r_header = hc.finish();
  if (r_header.is_error()) {
    send_closure(connection_.release(), &td::HttpInboundConnection::write_error, r_header.move_as_error());
    return;
  }
  send_closure(connection_, &td::HttpInboundConnection::write_next_noflush, td::BufferSlice(r_header.ok()));
  send_closure(connection_, &td::HttpInboundConnection::write_next_noflush, std::move(content));
  send_closure(connection_.release(), &td::HttpInboundConnection::write_ok);
}

}  // namespace telegram_bot_api
