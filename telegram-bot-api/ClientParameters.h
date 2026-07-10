//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2026
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "td/db/KeyValueSyncInterface.h"
#include "td/db/TQueue.h"

#include "td/net/GetHostByNameActor.h"

#include "td/actor/actor.h"

#include "td/utils/common.h"
#include "td/utils/List.h"
#include "td/utils/port/IPAddress.h"

#include <atomic>
#include <limits>
#include <memory>

namespace td {
class NetQueryStats;
}  // namespace td

namespace telegram_bot_api {

struct SharedData {
  std::atomic<td::uint64> query_count_{0};
  std::atomic<size_t> query_list_size_{0};
  std::atomic<int> next_verbosity_level_{-1};

  // not thread-safe, must be used from a single thread
  td::ListNode query_list_;
  td::unique_ptr<td::KeyValueSyncInterface> webhook_db_;
  td::unique_ptr<td::TQueue> tqueue_;

  double unix_time_difference_{-1e100};

  static constexpr size_t TQUEUE_EVENT_BUFFER_SIZE = 1000;
  td::TQueue::Event event_buffer_[TQUEUE_EVENT_BUFFER_SIZE];

  td::int32 get_unix_time(double now) const {
    auto result = unix_time_difference_ + now;
    if (result <= 0) {
      return 0;
    }
    if (result >= std::numeric_limits<td::int32>::max()) {
      return std::numeric_limits<td::int32>::max();
    }
    return static_cast<td::int32>(result);
  }

  static td::int32 get_file_gc_scheduler_id() {
    // the same scheduler as for file GC in Td
    return 2;
  }

  static td::int32 get_client_scheduler_id() {
    // the thread for ClientManager and all Clients
    return 4;
  }

  static td::int32 get_watchdog_scheduler_id() {
    // the thread for watchdogs
    return 5;
  }

  static td::int32 get_slow_incoming_http_scheduler_id() {
    // the thread for slow incoming HTTP connections
    return 6;
  }

  static td::int32 get_slow_outgoing_http_scheduler_id() {
    // the thread for slow outgoing HTTP connections
    return 7;
  }

  static td::int32 get_dns_resolver_scheduler_id() {
    // the thread for DNS resolving
    return 8;
  }

  static td::int32 get_binlog_scheduler_id() {
    // the thread for TQueue and webhook binlogs
    return 9;
  }

  static td::int32 get_webhook_certificate_scheduler_id() {
    // the thread for webhook certificate processing
    return 10;
  }

  static td::int32 get_statistics_thread_id() {
    // the thread for CPU usage updating
    return 11;
  }

  static td::int32 get_thread_count() {
    return 12;
  }
};

struct ClientParameters {
  td::string working_directory_;
  td::string files_directory_;
  bool allow_colon_in_filenames_ = true;

  bool local_mode_ = false;

  td::int32 api_id_ = 0;
  td::string api_hash_;

  td::string version_;

  td::int32 storage_max_time_from_last_access_ = -1;

  td::vector<td::int64> allowed_bot_user_ids_;  // empty means that all bots are allowed

  td::int32 default_max_webhook_connections_ = 0;
  td::IPAddress webhook_proxy_ip_address_;

  double start_time_ = 0;

  td::ActorId<td::GetHostByNameActor> get_host_by_name_actor_id_;

  std::shared_ptr<SharedData> shared_data_;

  std::shared_ptr<td::NetQueryStats> net_query_stats_;
};

}  // namespace telegram_bot_api
