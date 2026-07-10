//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2026
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "telegram-bot-api/Stats.h"

#include "td/utils/common.h"
#include "td/utils/logging.h"
#include "td/utils/port/config.h"
#include "td/utils/port/thread.h"
#include "td/utils/SliceBuilder.h"
#include "td/utils/StringBuilder.h"

#if TD_PORT_POSIX
#include <sys/resource.h>
#endif

namespace telegram_bot_api {

static td::Result<td::CpuStat> get_cpu_stat(double now) {
  auto r_cpu_stat = td::cpu_stat();
  if (r_cpu_stat.is_ok() && r_cpu_stat.ok().total_ticks_ > 0) {
    return r_cpu_stat;
  }
#if TD_PORT_POSIX
  // fallback for sandboxed kernels (e.g. gVisor) where /proc/stat is unavailable or static:
  // derive process ticks from getrusage and total ticks from the wall clock
  struct rusage usage;
  if (getrusage(RUSAGE_SELF, &usage) == 0) {
    td::CpuStat stat;
    stat.process_user_ticks_ =
        static_cast<td::uint64>(usage.ru_utime.tv_sec) * 1000000 + static_cast<td::uint64>(usage.ru_utime.tv_usec);
    stat.process_system_ticks_ =
        static_cast<td::uint64>(usage.ru_stime.tv_sec) * 1000000 + static_cast<td::uint64>(usage.ru_stime.tv_usec);
    auto cpu_count = td::thread::hardware_concurrency() ? td::thread::hardware_concurrency() : 1;
    stat.total_ticks_ = static_cast<td::uint64>(now * 1e6) * cpu_count;
    return stat;
  }
#endif
  return r_cpu_stat;
}

ServerCpuStat::ServerCpuStat() {
  for (std::size_t i = 1; i < SIZE; i++) {
    stat_[i] = td::TimedStat<CpuStat>(DURATIONS[i], td::Time::now());
  }
}

void ServerCpuStat::update(double now) {
  auto r_cpu_stat = get_cpu_stat(now);
  if (r_cpu_stat.is_error()) {
    if (r_cpu_stat.error().message() != "Not supported") {
      LOG(ERROR) << "Failed to get CPU statistics: " << r_cpu_stat.error();
    }
    return;
  }

  auto &cpu_stat = instance();
  std::lock_guard<std::mutex> guard(cpu_stat.mutex_);
  for (auto &stat : cpu_stat.stat_) {
    stat.add_event(r_cpu_stat.ok(), now);
  }
  static double next_log_time = 0.0;
  if (now >= next_log_time) {
    next_log_time = now + 60.0;
    LOG(WARNING) << "CPU usage: " << cpu_stat.stat_[1].get_stat(now).as_vector()[0].value_;
  }
}

td::string ServerCpuStat::get_description() {
  td::string res = "DURATION";
  for (auto &descr : DESCR) {
    res += '\t';
    res += descr;
  }
  return res;
}

static td::string to_percentage(td::uint64 ticks, td::uint64 total_ticks) {
  static double multiplier = 100.0 * (td::thread::hardware_concurrency() ? td::thread::hardware_concurrency() : 1);
  return PSTRING() << (static_cast<double>(ticks) / static_cast<double>(total_ticks) * multiplier) << '%';
}

td::vector<StatItem> CpuStat::as_vector() const {
  td::vector<StatItem> res;
  if (cnt_ < 2 || first_.total_ticks_ >= last_.total_ticks_) {
    res.push_back({"total_cpu", "UNKNOWN"});
    res.push_back({"user_cpu", "UNKNOWN"});
    res.push_back({"system_cpu", "UNKNOWN"});
  } else {
    auto total_ticks = last_.total_ticks_ - first_.total_ticks_;
    auto user_ticks = last_.process_user_ticks_ - first_.process_user_ticks_;
    auto system_ticks = last_.process_system_ticks_ - first_.process_system_ticks_;
    res.push_back({"total_cpu", to_percentage(user_ticks + system_ticks, total_ticks)});
    res.push_back({"user_cpu", to_percentage(user_ticks, total_ticks)});
    res.push_back({"system_cpu", to_percentage(system_ticks, total_ticks)});
  }
  return res;
}

td::vector<StatItem> ServerCpuStat::as_vector(double now) {
  std::lock_guard<std::mutex> guard(mutex_);

  td::vector<StatItem> res = stat_[0].get_stat(now).as_vector();
  for (std::size_t i = 1; i < SIZE; i++) {
    auto other = stat_[i].get_stat(now).as_vector();
    CHECK(other.size() == res.size());
    for (size_t j = 0; j < res.size(); j++) {
      res[j].value_ += "\t";
      res[j].value_ += other[j].value_;
    }
  }
  return res;
}

void ServerBotStat::normalize(double duration) {
  if (duration == 0) {
    return;
  }
  request_count_ /= duration;
  request_bytes_ /= duration;
  request_file_count_ /= duration;
  request_files_bytes_ /= duration;
  response_count_ /= duration;
  response_count_ok_ /= duration;
  response_count_error_ /= duration;
  response_bytes_ /= duration;
  update_count_ /= duration;
}

void ServerBotStat::add(const ServerBotStat &stat) {
  request_count_ += stat.request_count_;
  request_bytes_ += stat.request_bytes_;
  request_file_count_ += stat.request_file_count_;
  request_files_bytes_ += stat.request_files_bytes_;
  request_files_max_bytes_ = td::max(request_files_max_bytes_, stat.request_files_max_bytes_);

  response_count_ += stat.response_count_;
  response_count_ok_ += stat.response_count_ok_;
  response_count_error_ += stat.response_count_error_;
  response_bytes_ += stat.response_bytes_;

  update_count_ += stat.update_count_;
}

td::vector<StatItem> ServerBotStat::as_vector() const {
  td::vector<StatItem> res;
  auto add_item = [&res](td::string name, auto value) {
    res.push_back({std::move(name), td::to_string(value)});
  };
  add_item("request_count", request_count_);
  add_item("request_bytes", request_bytes_);
  add_item("request_file_count", request_file_count_);
  add_item("request_files_bytes", request_files_bytes_);
  add_item("request_max_bytes", request_files_max_bytes_);
  add_item("response_count", response_count_);
  add_item("response_count_ok", response_count_ok_);
  add_item("response_count_error", response_count_error_);
  add_item("response_bytes", response_bytes_);
  add_item("update_count", update_count_);
  return res;
}

td::vector<StatItem> BotStatActor::as_vector(double now) {
  auto first_sd = stat_[0].stat_duration(now);
  first_sd.first.normalize(first_sd.second);
  td::vector<StatItem> res = first_sd.first.as_vector();
  for (std::size_t i = 1; i < SIZE; i++) {
    auto next_sd = stat_[i].stat_duration(now);
    next_sd.first.normalize(next_sd.second);
    auto other = next_sd.first.as_vector();
    CHECK(other.size() == res.size());
    for (size_t j = 0; j < res.size(); j++) {
      res[j].value_ += "\t";
      res[j].value_ += other[j].value_;
    }
  }
  return res;
}

td::string BotStatActor::get_description() {
  td::string res = "DURATION";
  for (auto &descr : DESCR) {
    res += "\t";
    res += descr;
  }
  return res;
}

double BotStatActor::get_score(double now) {
  auto minute_stat = stat_[2].stat_duration(now);
  double minute_score = minute_stat.first.request_count_ + minute_stat.first.update_count_;
  if (minute_stat.second != 0) {
    minute_score /= minute_stat.second;
  }
  auto all_time_stat = stat_[0].stat_duration(now);
  double all_time_score = 0.01 * (all_time_stat.first.request_count_ + all_time_stat.first.update_count_);
  if (all_time_stat.second != 0) {
    all_time_score /= all_time_stat.second;
  }
  auto active_request_score = static_cast<double>(td::max(get_active_request_count() - 10, static_cast<td::int64>(0)));
  auto active_file_upload_score = static_cast<double>(get_active_file_upload_bytes()) * 1e-8;
  return minute_score + all_time_score + active_request_score + active_file_upload_score;
}

double BotStatActor::get_minute_update_count(double now) {
  auto minute_stat = stat_[2].stat_duration(now);
  double result = minute_stat.first.update_count_;
  if (minute_stat.second != 0) {
    result /= minute_stat.second;
  }
  return result;
}

td::int64 BotStatActor::get_active_request_count() const {
  return active_request_count_;
}

td::int64 BotStatActor::get_active_file_upload_bytes() const {
  return active_file_upload_bytes_;
}

td::int64 BotStatActor::get_active_file_upload_count() const {
  return active_file_upload_count_;
}

bool BotStatActor::is_active(double now) const {
  return last_activity_timestamp_ > now - 86400;
}

}  // namespace telegram_bot_api
