#include "log/log.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <sstream>
#include <thread>
#include <array>
#include <windows.h>

namespace mullion {
Logger Logger::s_instance("logger.log");
}

int main(int argc, char **argv) {
  using namespace mullion;
  if (argc != 2)
    return 1;
  char temp[MAX_PATH], name[MAX_PATH];
  if (!GetTempPathA(MAX_PATH, temp) || !GetTempFileNameA(temp, "log", 0, name))
    return 1;
  DeleteFileA(name);
  const std::filesystem::path directory(name);
  std::filesystem::create_directory(directory);
  SetEnvironmentVariableA("MULLION_LOG_PATH", name);
  const std::string mode(argv[1]);
  if (mode == "trace") {
    std::ostringstream captured;
    auto *previous = std::cerr.rdbuf(captured.rdbuf());
    int evaluated = 0;
    DEBUG(++evaluated);
    Logger::info("trace info");
    Logger::warn("trace warning");
    std::cerr.rdbuf(previous);
    const bool pass = evaluated == 1 &&
        captured.str().ends_with("debug: 1\ninfo:  trace info\nwarn:  trace warning\n") &&
        std::filesystem::is_empty(directory);
    std::filesystem::remove(directory);
    std::cout << (pass ? "passed: " : "failed: ") << "trace forwarding with normal logging disabled\n";
    return pass ? 0 : 1;
  }
  int evaluated = 0;
  DEBUG(++evaluated);
  Logger::log(LogLevel::None, "must not appear");
  Logger::info("first\nsecond\n");
  const std::string long_line(5000, 'x');
  Logger::warn(long_line);
  std::array<std::jthread, 4> workers;
  for (auto &worker : workers)
    worker = std::jthread([] { Logger::err("thread"); });
  for (auto &worker : workers)
    worker.join();
  std::string output;
  for (const auto &entry : std::filesystem::directory_iterator(directory)) {
    std::ifstream file(entry.path(), std::ios::binary);
    output.append(std::istreambuf_iterator<char>(file), {});
  }
  const std::string expected = mode == "none" ? "" :
      (mode == "info" ? std::string("info:  first\ninfo:  second\n") : std::string()) +
      "warn:  " + long_line + "\nerr:   thread\nerr:   thread\nerr:   thread\nerr:   thread\n";
  const bool pass = evaluated == 0 && output == expected;
  // The logger keeps its file open until DLL/process teardown on Windows.
  // Remove it with delete sharing at process shutdown where possible.
  for (const auto &entry : std::filesystem::directory_iterator(directory))
    DeleteFileW(entry.path().c_str());
  RemoveDirectoryW(directory.c_str());
  std::cout << (pass ? "passed: " : "failed: ") << "logger " << mode << '\n';
  return pass ? 0 : 1;
}
