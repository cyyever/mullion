/*
 * This file is part of DXMT, Copyright (c) 2023 Feifan He
 *
 * Derived from a part of DXVK (originally under zlib License),
 * Copyright (c) 2017 Philip Rebohle
 * Copyright (c) 2019 Joshua Ashton
 *
 * See <https://github.com/doitsujin/dxvk/blob/master/LICENSE>
 */

#include <spdlog/logger.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <array>
#include <string_view>
#include <utility>

#include "log.hpp"
#include "trace.hpp"

#include "../util_env.hpp"
#include "util_string.hpp"

namespace mullion {

Logger::Logger(std::string fileName)
    : m_minLevel(getMinLogLevel()), m_fileName(std::move(fileName)) {}

Logger::~Logger() = default;

void Logger::trace(const std::string &message) {
  s_instance.emitMsg(LogLevel::Trace, message);
}

void Logger::debug(const std::string &message) {
  s_instance.emitMsg(LogLevel::Debug, message);
}

void Logger::info(const std::string &message) {
  s_instance.emitMsg(LogLevel::Info, message);
}

void Logger::warn(const std::string &message) {
  s_instance.emitMsg(LogLevel::Warn, message);
}

void Logger::err(const std::string &message) {
  s_instance.emitMsg(LogLevel::Error, message);
}

void Logger::log(LogLevel level, const std::string &message) {
  s_instance.emitMsg(level, message);
}

// the bytes of a line that fit Wine's 1020 with a newline and the terminator
constexpr size_t kWineLine = 1018;

// MULLION_TRACE_<CATEGORY>: the trace log has the warnings and errors whatever MULLION_LOG_LEVEL is (a game's own output
// may go nowhere), MULLION_TRACE_API its info and debug lines too, and MULLION_TRACE_API=heavy its trace lines
static bool Traced(LogLevel level) {
  if (!trace::Any())
    return false;
  if (level >= LogLevel::Warn)
    return true;
  return trace::On(trace::Api) && (level >= LogLevel::Debug || trace::Heavy(trace::Api));
}

namespace {
constexpr std::array<std::string_view, 5> prefixes = {"trace: ", "debug: ", "info:  ", "warn:  ", "err:   "};
constexpr std::array<spdlog::level::level_enum, 5> levels = {
    spdlog::level::trace, spdlog::level::debug, spdlog::level::info, spdlog::level::warn, spdlog::level::err};

// The owning Logger serializes these sinks, including lazy initialization.
class WineSink final : public spdlog::sinks::base_sink<spdlog::details::null_mutex> {
public:
  explicit WineSink(PFN_wineLogOutput output) : output_(output) {}

private:
  PFN_wineLogOutput output_;
  void sink_it_(const spdlog::details::log_msg &message) override {
    spdlog::memory_buf_t formatted;
    formatter_->format(message, formatted);
    const std::string_view bytes(formatted.data(), formatted.size());
    for (size_t at = 0; at < bytes.size(); at += kWineLine) {
      std::string chunk(bytes.substr(at, kWineLine));
      if (at + kWineLine < bytes.size())
        chunk += '\n';
      output_(chunk.c_str());
    }
  }
  void flush_() override {}
};

template <typename F> void Lines(std::string_view text, F &&consume) {
  while (!text.empty()) {
    const auto end = text.find('\n');
    consume(text.substr(0, end));
    if (end == std::string_view::npos)
      break;
    text.remove_prefix(end + 1);
  }
}
} // namespace

bool Logger::enabled(LogLevel level) {
  return level < LogLevel::None && (level >= s_instance.m_minLevel || Traced(level));
}

void Logger::emitMsg(LogLevel level, const std::string &message) {
  if (level >= LogLevel::None)
    return;
  const auto index = static_cast<size_t>(level);
  const auto prefix = prefixes[index];
  if (Traced(level)) {
    std::string traced;
    Lines(message, [&](std::string_view line) {
      traced.append(prefix).append(line).push_back('\n');
    });
    trace::WriteLine(traced);
  }
  if (level < m_minLevel)
    return;

  std::scoped_lock lock(m_mutex);
  std::string unwritten;
  if (!m_initialized) {
#ifdef _WIN32
    if (HMODULE ntdll = GetModuleHandleA("ntdll.dll"))
      m_wineLogOutput = reinterpret_cast<PFN_wineLogOutput>(GetProcAddress(ntdll, "__wine_dbg_output"));
#endif
    std::vector<spdlog::sink_ptr> sinks;
    if (m_wineLogOutput)
      sinks.push_back(std::make_shared<WineSink>(m_wineLogOutput));
    else
      sinks.push_back(std::make_shared<spdlog::sinks::stderr_sink_st>());
    const auto path = getFileName(m_fileName);
    if (!path.empty()) {
      try {
        sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_st>(str::topath(path.c_str()), true));
      } catch (const spdlog::spdlog_ex &) {
        unwritten = "cannot write the log file " + path;
      }
    }
    auto backend = std::make_unique<spdlog::logger>(m_fileName, sinks.begin(), sinks.end());
    backend->set_level(levels[static_cast<size_t>(m_minLevel)]);
    // Keep the existing text format and newline on both Windows and native builds.
    backend->set_formatter(std::make_unique<spdlog::pattern_formatter>("%v", spdlog::pattern_time_type::local, "\n"));
    backend->flush_on(spdlog::level::trace);
    m_backend = std::move(backend);
    m_initialized = true;
  }
  const auto write = [&](std::string_view line) {
    m_backend->log(levels[index], "{}{}", prefix, line);
  };
  if (!unwritten.empty())
    write(unwritten);
  Lines(message, write);
}

std::string Logger::getFileName(const std::string &base) {
  std::string path = env::getEnvVar("MULLION_LOG_PATH");

  if (path == "none")
    return {};

  // Don't create a log file if we're writing to wine's console output
  if (path.empty() && m_wineLogOutput)
    return {};

  if (!path.empty() && *path.rbegin() != '/')
    path += '/';

  std::string exeName = env::getExeBaseName();
  path += exeName + "_" + base;
  return path;
}

LogLevel Logger::getMinLogLevel() {
  const std::array<std::pair<const char *, LogLevel>, 6> logLevels = {{
      {"trace", LogLevel::Trace},
      {"debug", LogLevel::Debug},
      {"info", LogLevel::Info},
      {"warn", LogLevel::Warn},
      {"error", LogLevel::Error},
      {"none", LogLevel::None},
  }};

  const std::string logLevelStr = env::getEnvVar("MULLION_LOG_LEVEL");

  for (const auto &pair : logLevels) {
    if (logLevelStr == pair.first)
      return pair.second;
  }

  return LogLevel::Warn;
}

} // namespace mullion
