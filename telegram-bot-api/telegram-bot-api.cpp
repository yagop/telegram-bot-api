//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2026
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "telegram-bot-api/ClientManager.h"
#include "telegram-bot-api/ClientParameters.h"
#include "telegram-bot-api/HttpConnection.h"
#include "telegram-bot-api/HttpServer.h"
#include "telegram-bot-api/HttpStatConnection.h"
#include "telegram-bot-api/Stats.h"
#include "telegram-bot-api/Watchdog.h"

#include "td/db/binlog/Binlog.h"

#include "td/net/GetHostByNameActor.h"
#include "td/net/HttpInboundConnection.h"

#include "td/actor/actor.h"
#include "td/actor/ConcurrentScheduler.h"

#include "td/utils/AsyncFileLog.h"
#include "td/utils/CombinedLog.h"
#include "td/utils/common.h"
#include "td/utils/crypto.h"
#include "td/utils/ExitGuard.h"
//#include "td/utils/GitInfo.h"
#include "td/utils/logging.h"
#include "td/utils/MemoryLog.h"
#include "td/utils/misc.h"
#include "td/utils/OptionParser.h"
#include "td/utils/PathView.h"
#include "td/utils/port/detail/ThreadIdGuard.h"
#include "td/utils/port/IPAddress.h"
#include "td/utils/port/path.h"
#include "td/utils/port/rlimit.h"
#include "td/utils/port/signals.h"
#include "td/utils/port/stacktrace.h"
#include "td/utils/port/thread.h"
#include "td/utils/port/user.h"
#include "td/utils/Promise.h"
#include "td/utils/Slice.h"
#include "td/utils/SliceBuilder.h"
#include "td/utils/Status.h"
#include "td/utils/Time.h"

#include <atomic>
#include <cstdlib>
#include <memory>
#include <tuple>

namespace telegram_bot_api {

static std::atomic_flag need_reopen_log;

static void after_log_rotation_signal_handler(int sig) {
  need_reopen_log.clear();
}

static std::atomic_flag need_quit;

static void quit_signal_handler(int sig) {
  need_quit.clear();
}

static td::MemoryLog<1 << 20> memory_log;

void print_log() {
  td::LogGuard log_guard;
  auto buf = memory_log.get_buffer();
  auto pos = memory_log.get_pos();
  size_t tail_length = buf.size() - pos;
  while (tail_length > 0 && buf[pos + tail_length - 1] == ' ') {
    tail_length--;
  }
  if (tail_length + 100 >= buf.size() - pos) {
    tail_length = buf.size() - pos;
  }
  td::signal_safe_write("------- Log dump -------\n");
  td::signal_safe_write(buf.substr(pos, tail_length), false);
  td::signal_safe_write(buf.substr(0, pos), false);
  td::signal_safe_write("\n", false);
  td::signal_safe_write("------------------------\n");
}

static std::atomic_bool has_failed{false};

static std::atomic_flag need_dump_statistics;

static void dump_stacktrace_signal_handler(int sig) {
  if (has_failed) {
    return;
  }
  td::LogGuard log_guard;
  if (LOG_TAG != nullptr && *LOG_TAG) {
    td::signal_safe_write(td::Slice(LOG_TAG));
    td::signal_safe_write(td::Slice("\n"), false);
  }
  td::Stacktrace::print_to_stderr();
  need_dump_statistics.clear();
}

static void fail_signal_handler(int sig) {
  has_failed = true;
  print_log();
  {
    td::LogGuard log_guard;
    td::signal_safe_write_signal_number(sig);
    td::Stacktrace::PrintOptions options;
    options.use_gdb = true;
    td::Stacktrace::print_to_stderr(options);
  }
  _Exit(EXIT_FAILURE);
}

static std::atomic_flag need_change_verbosity_level;

static void change_verbosity_level_signal_handler(int sig) {
  need_change_verbosity_level.clear();
}

static std::atomic_flag need_dump_log;

static void dump_log_signal_handler(int sig) {
  if (has_failed) {
    return;
  }
  need_dump_log.clear();
}

static void sigsegv_signal_handler(int signum, void *addr) {
  td::signal_safe_write_pointer(addr);
  fail_signal_handler(signum);
}

int main(int argc, char *argv[]) {
  SET_VERBOSITY_LEVEL(VERBOSITY_NAME(FATAL));
  td::ExitGuard exit_guard;
  td::detail::ThreadIdGuard thread_id_guard;

  need_reopen_log.test_and_set();
  need_quit.test_and_set();
  need_change_verbosity_level.test_and_set();
  need_dump_statistics.test_and_set();
  need_dump_log.test_and_set();

  td::Stacktrace::init();

  td::setup_signals_alt_stack().ensure();
  td::set_signal_handler(td::SignalType::User, after_log_rotation_signal_handler).ensure();
  td::ignore_signal(td::SignalType::HangUp).ensure();
  td::ignore_signal(td::SignalType::Pipe).ensure();
  td::set_signal_handler(td::SignalType::Quit, quit_signal_handler).ensure();
  td::set_signal_handler(td::SignalType::Abort, fail_signal_handler).ensure();
  td::set_signal_handler(td::SignalType::Other, fail_signal_handler).ensure();
  td::set_extended_signal_handler(td::SignalType::Error, sigsegv_signal_handler).ensure();

  td::set_real_time_signal_handler(0, change_verbosity_level_signal_handler).ensure();
  td::set_real_time_signal_handler(1, dump_log_signal_handler).ensure();
  td::set_real_time_signal_handler(2, dump_stacktrace_signal_handler).ensure();

  td::init_openssl_threads();

  auto start_time = td::Time::now();
  auto shared_data = std::make_shared<SharedData>();
  auto parameters = std::make_unique<ClientParameters>();
  parameters->version_ = "10.3";
  parameters->shared_data_ = shared_data;
  parameters->start_time_ = start_time;
  auto net_query_stats = td::create_net_query_stats();
  parameters->net_query_stats_ = net_query_stats;

  td::OptionParser options;
  bool need_print_usage = false;
  bool need_print_version = false;
  int http_port = 8081;
  int http_stat_port = 0;
  td::string http_ip_address = "0.0.0.0";
  td::string http_stat_ip_address = "0.0.0.0";
  td::string log_file_path;
  int default_verbosity_level = 0;
  int memory_verbosity_level = VERBOSITY_NAME(INFO);
  td::int64 log_max_file_size = 2000000000;
  td::string working_directory = PSTRING() << "." << TD_DIR_SLASH;
  td::string files_directory;
  td::string temporary_directory;
  td::string username;
  td::string groupname;
  td::uint64 max_connections = 0;
  td::uint64 cpu_affinity = 0;
  td::uint64 main_thread_affinity = 0;
  ClientManager::TokenRange token_range{0, 1};

  parameters->api_id_ = [](auto x) -> td::int32 {
    if (x) {
      return td::to_integer<td::int32>(td::Slice(x));
    }
    return 0;
  }(std::getenv("TELEGRAM_API_ID"));
  parameters->api_hash_ = [](auto x) -> td::string {
    if (x) {
      return x;
    }
    return td::string();
  }(std::getenv("TELEGRAM_API_HASH"));

  options.set_usage(td::Slice(argv[0]), "--api-id=<arg> --api-hash=<arg> [--local] [OPTION]...");
  options.set_description("Telegram Bot API server");
  options.add_option('h', "help", "display this help text and exit", [&] { need_print_usage = true; });
  options.add_option('\0', "version", "display version number and exit", [&] { need_print_version = true; });
  options.add_option('\0', "local", "allow the Bot API server to serve local requests",
                     [&] { parameters->local_mode_ = true; });
  options.add_checked_option(
      '\0', "api-id",
      "application identifier for Telegram API access, which can be obtained at https://my.telegram.org (defaults to "
      "the value of the TELEGRAM_API_ID environment variable)",
      td::OptionParser::parse_integer(parameters->api_id_));
  options.add_option('\0', "api-hash",
                     "application identifier hash for Telegram API access, which can be obtained at "
                     "https://my.telegram.org (defaults to the value of the TELEGRAM_API_HASH environment variable)",
                     td::OptionParser::parse_string(parameters->api_hash_));
  options.add_checked_option('p', "http-port", PSLICE() << "HTTP listening port (default is " << http_port << ")",
                             td::OptionParser::parse_integer(http_port));
  options.add_checked_option('s', "http-stat-port", "HTTP statistics port",
                             td::OptionParser::parse_integer(http_stat_port));
  options.add_option('d', "dir", "server working directory", td::OptionParser::parse_string(working_directory));
  options.add_option('f', "files-dir", "directory for storing auxiliary files (defaults to the working directory)",
                     td::OptionParser::parse_string(files_directory));
  options.add_option('t', "temp-dir", "directory for storing HTTP server temporary files",
                     td::OptionParser::parse_string(temporary_directory));
  options.add_checked_option('\0', "filter",
                             "\"<remainder>/<modulo>\". Allow only bots with 'bot_user_id % modulo == remainder'",
                             [&](td::Slice rem_mod) {
                               td::Slice rem;
                               td::Slice mod;
                               std::tie(rem, mod) = td::split(rem_mod, '/');
                               TRY_RESULT(rem_i, td::to_integer_safe<td::uint64>(rem));
                               TRY_RESULT(mod_i, td::to_integer_safe<td::uint64>(mod));
                               if (rem_i >= mod_i) {
                                 return td::Status::Error("Wrong argument specified: ensure that remainder < modulo");
                               }
                               token_range = {rem_i, mod_i};
                               return td::Status::OK();
                             });
  options.add_checked_option('\0', "allowed-bot-ids",
                             "comma-separated list of bot user identifiers that are allowed to use the server. By "
                             "default, all bots are allowed",
                             [&](td::Slice ids) -> td::Status {
                               for (auto id_str : td::full_split(ids, ',')) {
                                 TRY_RESULT(user_id, td::to_integer_safe<td::int64>(id_str));
                                 if (user_id <= 0) {
                                   return td::Status::Error("Invalid bot user identifier specified");
                                 }
                                 parameters->allowed_bot_user_ids_.push_back(user_id);
                               }
                               return td::Status::OK();
                             });
  options.add_checked_option('\0', "max-webhook-connections",
                             "default value of the maximum webhook connections per bot",
                             td::OptionParser::parse_integer(parameters->default_max_webhook_connections_));
  options.add_checked_option('\0', "http-ip-address",
                             "local IP address, HTTP connections to which will be accepted. By default, connections to "
                             "any local IPv4 address are accepted",
                             [&](td::Slice ip_address) {
                               TRY_STATUS(td::IPAddress::get_ip_address(ip_address.str()));
                               http_ip_address = ip_address.str();
                               return td::Status::OK();
                             });
  options.add_checked_option('\0', "http-stat-ip-address",
                             "local IP address, HTTP statistics connections to which will be accepted. By default, "
                             "statistics connections to any local IPv4 address are accepted",
                             [&](td::Slice ip_address) {
                               TRY_STATUS(td::IPAddress::get_ip_address(ip_address.str()));
                               http_stat_ip_address = ip_address.str();
                               return td::Status::OK();
                             });

  options.add_option('l', "log", "path to the file where the log will be written",
                     td::OptionParser::parse_string(log_file_path));
  options.add_checked_option('v', "verbosity", "log verbosity level",
                             td::OptionParser::parse_integer(default_verbosity_level));
  options.add_checked_option('\0', "memory-verbosity", "memory log verbosity level; defaults to 3",
                             td::OptionParser::parse_integer(memory_verbosity_level));
  options.add_checked_option(
      '\0', "log-max-file-size",
      PSLICE() << "maximum size of the log file in bytes before it will be auto-rotated (default is "
               << log_max_file_size << ")",
      td::OptionParser::parse_integer(log_max_file_size));

  options.add_option('u', "username", "effective user name to switch to", td::OptionParser::parse_string(username));
  options.add_option('g', "groupname", "effective group name to switch to", td::OptionParser::parse_string(groupname));
  options.add_checked_option('c', "max-connections", "maximum number of open file descriptors",
                             td::OptionParser::parse_integer(max_connections));
#if TD_HAVE_THREAD_AFFINITY
  options.add_checked_option('\0', "cpu-affinity", "CPU affinity as 64-bit mask (defaults to all available CPUs)",
                             td::OptionParser::parse_integer(cpu_affinity));
  options.add_checked_option(
      '\0', "main-thread-affinity",
      "CPU affinity of the main thread as 64-bit mask (defaults to the value of the option --cpu-affinity)",
      td::OptionParser::parse_integer(main_thread_affinity));
#else
  (void)cpu_affinity;
  (void)main_thread_affinity;
#endif

  options.add_checked_option('\0', "proxy",
                             "HTTP proxy server for outgoing webhook requests in the format http://host:port",
                             [&](td::Slice address) {
                               if (td::begins_with(address, "http://")) {
                                 address.remove_prefix(7);
                               } else if (td::begins_with(address, "https://")) {
                                 address.remove_prefix(8);
                               }
                               return parameters->webhook_proxy_ip_address_.init_host_port(address.str());
                             });
  options.add_check([&] {
    if (parameters->api_id_ <= 0 || parameters->api_hash_.empty()) {
      return td::Status::Error("You must provide valid api-id and api-hash obtained at https://my.telegram.org");
    }
    return td::Status::OK();
  });
  options.add_check([&] {
    if (default_verbosity_level < 0) {
      return td::Status::Error("Wrong verbosity level specified");
    }
    return td::Status::OK();
  });
  options.add_check([&] {
    if (memory_verbosity_level < 0) {
      return td::Status::Error("Wrong memory verbosity level specified");
    }
    return td::Status::OK();
  });
  auto r_non_options = options.run(argc, argv, 0);
  if (need_print_usage) {
    LOG(PLAIN) << options;
    return 0;
  }
  if (need_print_version) {
    LOG(PLAIN) << "Bot API " << parameters->version_;
    return 0;
  }
  if (r_non_options.is_error()) {
    LOG(PLAIN) << argv[0] << ": " << r_non_options.error().message();
    LOG(PLAIN) << options;
    return 1;
  }

  td::CombinedLog log;
  log.set_first(td::default_log_interface);
  log.set_second(&memory_log);
  td::log_interface = &log;

  td::AsyncFileLog file_log;

  auto init_status = [&] {
#if TD_HAVE_THREAD_AFFINITY
    if (main_thread_affinity == 0) {
      main_thread_affinity = cpu_affinity;
    }
    if (main_thread_affinity != 0) {
      auto initial_mask = td::thread::get_affinity_mask(td::this_thread::get_id());
      if (initial_mask == 0) {
        return td::Status::Error("Failed to get current thread affinity");
      }
      if (cpu_affinity != 0) {
        TRY_STATUS_PREFIX(td::thread::set_affinity_mask(td::this_thread::get_id(), cpu_affinity),
                          "Can't set CPU affinity mask: ");
      } else {
        cpu_affinity = initial_mask;
      }
      TRY_STATUS_PREFIX(td::thread::set_affinity_mask(td::this_thread::get_id(), main_thread_affinity),
                        "Can't set main thread CPU affinity mask: ");
    }
#endif

    if (max_connections != 0) {
      TRY_STATUS_PREFIX(td::set_resource_limit(td::ResourceLimitType::NoFile, max_connections),
                        "Can't set file descriptor limit: ");
    }

    if (!username.empty()) {
      TRY_STATUS_PREFIX(td::change_user(username, groupname), "Can't change effective user: ");
    }

    {
      TRY_RESULT_PREFIX_ASSIGN(working_directory, td::realpath(working_directory, true),
                               "Invalid working directory specified: ");
      if (working_directory.empty()) {
        return td::Status::Error("Empty path specified as working directory");
      }
      if (working_directory.back() != TD_DIR_SLASH) {
        working_directory += TD_DIR_SLASH;
      }

      TRY_STATUS_PREFIX(td::mkpath(working_directory, 0750), "Failed to create working directory: ");

      auto r_temp_file = td::mkstemp(working_directory);
      if (r_temp_file.is_error()) {
        return td::Status::Error(PSLICE() << "Can't create files in the directory \"" << working_directory
                                          << "\". Use --dir option to specify a writable working directory");
      }
      r_temp_file.ok_ref().first.close();
      td::unlink(r_temp_file.ok().second).ensure();

      auto r_temp_dir = td::mkdtemp(working_directory, "1:a");
      if (r_temp_dir.is_error()) {
        parameters->allow_colon_in_filenames_ = false;
        r_temp_dir = td::mkdtemp(working_directory, "1~a");
        if (r_temp_dir.is_error()) {
          return td::Status::Error(PSLICE() << "Can't create directories in the directory \"" << working_directory
                                            << "\". Use --dir option to specify a writable working directory");
        }
      }
      td::rmdir(r_temp_dir.ok()).ensure();
    }

    if (files_directory.empty()) {
      files_directory = working_directory;
    } else {
      TRY_RESULT_PREFIX_ASSIGN(files_directory, td::realpath(files_directory, true),
                               "Invalid files directory specified: ");
      if (files_directory.empty()) {
        return td::Status::Error("Empty path specified as files directory");
      }
      if (files_directory.back() != TD_DIR_SLASH) {
        files_directory += TD_DIR_SLASH;
      }

      TRY_STATUS_PREFIX(td::mkpath(files_directory, 0750), "Failed to create files directory: ");

      auto r_temp_file = td::mkstemp(files_directory);
      if (r_temp_file.is_error()) {
        return td::Status::Error(PSLICE() << "Can't create files in the directory \"" << files_directory
                                          << "\". Use --files-dir option to specify a writable files directory");
      }
      r_temp_file.ok_ref().first.close();
      td::unlink(r_temp_file.ok().second).ensure();

      auto r_temp_dir = td::mkdtemp(files_directory, "1:a");
      if (r_temp_dir.is_error()) {
        parameters->allow_colon_in_filenames_ = false;
        r_temp_dir = td::mkdtemp(files_directory, "1~a");
        if (r_temp_dir.is_error()) {
          return td::Status::Error(PSLICE() << "Can't create directories in the directory \"" << files_directory
                                            << "\". Use --files-dir option to specify a writable files directory");
        }
      }
      td::rmdir(r_temp_dir.ok()).ensure();
    }

    if (!temporary_directory.empty()) {
      if (td::PathView(temporary_directory).is_relative()) {
        temporary_directory = files_directory + temporary_directory;
      }
      TRY_STATUS_PREFIX(td::set_temporary_dir(temporary_directory), "Can't set temporary directory: ");
    }

    {  // check temporary directory
      auto temp_dir = td::get_temporary_dir();
      if (temp_dir.empty()) {
        return td::Status::Error("Can't find directory for temporary files. Use --temp-dir option to specify it");
      }

      auto r_temp_file = td::mkstemp(temp_dir);
      if (r_temp_file.is_error()) {
        return td::Status::Error(PSLICE()
                                 << "Can't create files in the directory \"" << temp_dir
                                 << "\". Use --temp-dir option to specify another directory for temporary files");
      }
      r_temp_file.ok_ref().first.close();
      td::unlink(r_temp_file.ok().second).ensure();
    }

    if (!log_file_path.empty()) {
      if (td::PathView(log_file_path).is_relative()) {
        log_file_path = files_directory + log_file_path;
      }
      TRY_STATUS_PREFIX(file_log.init(log_file_path, log_max_file_size), "Can't open log file: ");
      log.set_first(&file_log);
    }

    return td::Status::OK();
  }();
  if (init_status.is_error()) {
    LOG(PLAIN) << init_status.message();
    LOG(PLAIN) << options;
    return 1;
  }

  parameters->working_directory_ = std::move(working_directory);
  parameters->files_directory_ = std::move(files_directory);

  if (parameters->default_max_webhook_connections_ <= 0) {
    parameters->default_max_webhook_connections_ = parameters->local_mode_ ? 100 : 40;
  }

  ::td::VERBOSITY_NAME(dns_resolver) = VERBOSITY_NAME(WARNING);

  log.set_second_verbosity_level(memory_verbosity_level);

  auto set_verbosity_level = [&log, memory_verbosity_level](int new_verbosity_level) {
    SET_VERBOSITY_LEVEL(td::max(memory_verbosity_level, new_verbosity_level));
    log.set_first_verbosity_level(new_verbosity_level);
  };
  set_verbosity_level(default_verbosity_level);

  // LOG(WARNING) << "Bot API server with commit " << td::GitInfo::commit() << ' '
  //              << (td::GitInfo::is_dirty() ? "(dirty)" : "") << " started";
  LOG(WARNING) << "Bot API " << parameters->version_ << " server started";

  td::ConcurrentScheduler sched(SharedData::get_thread_count() - 1, cpu_affinity);

  td::GetHostByNameActor::Options get_host_by_name_options;
  get_host_by_name_options.scheduler_id = SharedData::get_dns_resolver_scheduler_id();
  parameters->get_host_by_name_actor_id_ =
      sched.create_actor_unsafe<td::GetHostByNameActor>(0, "GetHostByName", std::move(get_host_by_name_options))
          .release();

  auto client_manager = sched
                            .create_actor_unsafe<ClientManager>(SharedData::get_client_scheduler_id(), "ClientManager",
                                                                std::move(parameters), token_range)
                            .release();

  sched
      .create_actor_unsafe<HttpServer>(
          SharedData::get_client_scheduler_id(), "HttpServer", http_ip_address, http_port,
          [client_manager, shared_data] {
            return td::ActorOwn<td::HttpInboundConnection::Callback>(
                td::create_actor<HttpConnection>("HttpConnection", client_manager, shared_data));
          })
      .release();

  if (http_stat_port != 0) {
    sched
        .create_actor_unsafe<HttpServer>(
            SharedData::get_client_scheduler_id(), "HttpStatsServer", http_stat_ip_address, http_stat_port,
            [client_manager] {
              return td::ActorOwn<td::HttpInboundConnection::Callback>(
                  td::create_actor<HttpStatConnection>("HttpStatConnection", client_manager));
            })
        .release();
  }

  constexpr double WATCHDOG_TIMEOUT = 0.25;
  auto watchdog_id = sched.create_actor_unsafe<Watchdog>(SharedData::get_watchdog_scheduler_id(), "Watchdog",
                                                         td::this_thread::get_id(), WATCHDOG_TIMEOUT);

  sched.start();

  double next_watchdog_kick_time = start_time;
  double next_cron_time = start_time;
  double last_dump_time = start_time - 1000.0;
  bool close_flag = false;
  std::atomic_bool can_quit{false};
  ServerCpuStat::instance();  // create ServerCpuStat instance
  while (true) {
    sched.run_main(td::min(next_cron_time, next_watchdog_kick_time) - td::Time::now());

    if (!need_reopen_log.test_and_set()) {
      td::log_interface->after_rotation();
    }

    if (!need_quit.test_and_set()) {
      if (close_flag) {
        LOG(WARNING) << "Receive stop signal again. Exit immediately...";
        std::_Exit(0);
      }

      LOG(WARNING) << "Stopping engine with uptime " << (td::Time::now() - start_time) << " seconds by a signal";
      close_flag = true;
      auto guard = sched.get_main_guard();
      watchdog_id.reset();
      send_closure(client_manager, &ClientManager::close, td::PromiseCreator::lambda([&can_quit](td::Unit) {
                     can_quit.store(true);
                     td::Scheduler::instance()->yield();
                   }));
    }
    if (can_quit.exchange(false)) {
      break;
    }

    if (!need_change_verbosity_level.test_and_set()) {
      if (log.get_first_verbosity_level() == default_verbosity_level) {
        // increase default log verbosity level
        set_verbosity_level(100);
      } else {
        // return back verbosity level
        set_verbosity_level(default_verbosity_level);
      }
    }

    auto next_verbosity_level = shared_data->next_verbosity_level_.exchange(-1);
    if (next_verbosity_level != -1) {
      set_verbosity_level(next_verbosity_level);
    }

    if (!need_dump_log.test_and_set()) {
      print_log();
      need_dump_statistics.clear();
    }

    double now = td::Time::now();
    if (now >= next_cron_time) {
      if (now >= next_cron_time + 1.0) {
        next_cron_time = now;
      }
      next_cron_time += 1.0;
      auto guard = sched.get_main_guard();
      td::Scheduler::instance()->run_on_scheduler(SharedData::get_statistics_thread_id(),
                                                  [](td::Unit) { ServerCpuStat::update(td::Time::now()); });
    }

    if (now >= start_time + 600) {
      auto guard = sched.get_main_guard();
      send_closure(watchdog_id, &Watchdog::kick);
      next_watchdog_kick_time = now + WATCHDOG_TIMEOUT / 10;
    }

    if (!need_dump_statistics.test_and_set() || now > last_dump_time + 300.0) {
      last_dump_time = now;
      auto guard = sched.get_main_guard();
      send_closure(client_manager, &ClientManager::dump_statistics);
    }
  }

  LOG(WARNING) << "--------------------FINISH ENGINE--------------------";
  if (net_query_stats.use_count() != 1) {
    LOG(ERROR) << "NetQueryStats have leaked";
  }
  net_query_stats = nullptr;
  sched.finish();
  SET_VERBOSITY_LEVEL(VERBOSITY_NAME(FATAL));
  td::log_interface = td::default_log_interface;
  return 0;
}

}  // namespace telegram_bot_api

int main(int argc, char *argv[]) {
  return telegram_bot_api::main(argc, argv);
}
