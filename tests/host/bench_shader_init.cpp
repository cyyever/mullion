// Usage: bench_shader_init shader1.dxbc shader2.dxbc ... shader6.dxbc
#include "airconv_public.h"
#include <llvm/Support/MemoryBuffer.h>
#include <algorithm>
#include <array>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <print>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <thread>
#include <vector>

constexpr int workers = 6, rounds = 40;
using Inputs = std::array<std::unique_ptr<llvm::MemoryBuffer>, workers>;

static sm50_shader_t
initialize(const llvm::MemoryBuffer &bytes) {
  sm50_shader_t shader{};
  sm50_error_t error{};
  MTL_SHADER_REFLECTION reflection{};
  if (SM50Initialize(bytes.getBufferStart(), bytes.getBufferSize(), &shader, &reflection, &error)) {
    std::println(stderr, "{}: {}", bytes.getBufferIdentifier().str(),
                 error ? SM50GetErrorMessageString(error) : "initialization failed");
    if (error)
      SM50FreeError(error);
    std::exit(1);
  }
  return shader;
}

enum class Workload : std::uint8_t { Single, SharedCold, SharedWarm, Distinct };

static void
measure(const Inputs &inputs, Workload workload, const char *name, int sample) {
  const int count = workload == Workload::Single ? 1 : workers;
  std::barrier gate(count + 1);
  std::array<sm50_shader_t, workers> handles{};
  std::vector<std::thread> threads;
  threads.reserve(count);
  for (int i = 0; i < count; ++i) {
    threads.emplace_back([&, i] {
      for (int round = 0; round < rounds; ++round) {
        gate.arrive_and_wait();
        handles[i] = initialize(*inputs[workload == Workload::Distinct ? i : 0]);
        gate.arrive_and_wait();
        gate.arrive_and_wait();
      }
    });
  }

  std::array<double, rounds> wall{}, cpu{};
  for (int round = 0; round < rounds; ++round) {
    auto held = workload == Workload::SharedWarm ? initialize(*inputs[0]) : nullptr;
    auto cpu_start = std::clock();
    auto start = std::chrono::steady_clock::now();
    gate.arrive_and_wait();
    gate.arrive_and_wait();
    wall[round] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    cpu[round] = 1000.0 * (std::clock() - cpu_start) / CLOCKS_PER_SEC;

    for (int i = 0; i < count; ++i) {
      if (workload != Workload::Distinct && handles[i] != handles[0]) {
        std::println(stderr, "{}: concurrent callers received different handles", name);
        std::exit(1);
      }
    }
    for (int i = 0; i < count; ++i)
      SM50Destroy(handles[i]);
    if (held)
      SM50Destroy(held);
    gate.arrive_and_wait();
  }
  for (auto &thread : threads)
    thread.join();
  std::ranges::sort(wall);
  std::ranges::sort(cpu);
  std::println("sample={} mode={} wall_median_ms={:.3f} cpu_median_ms={:.3f} rounds={}", sample, name,
              (wall[rounds / 2 - 1] + wall[rounds / 2]) / 2, (cpu[rounds / 2 - 1] + cpu[rounds / 2]) / 2, rounds);
}

int
main(int argc, char **argv) {
  if (argc != workers + 1) {
    std::println(stderr, "usage: {} <six distinct shareable DXBC shader files>", argv[0]);
    return 1;
  }
  Inputs inputs;
  for (int i = 0; i < workers; ++i) {
    auto file = llvm::MemoryBuffer::getFile(argv[i + 1], false, false);
    if (!file) {
      std::println(stderr, "{}: {}", argv[i + 1], file.getError().message());
      return 1;
    }
    inputs[i] = std::move(*file);
    for (int j = 0; j < i; ++j) {
      if (inputs[j]->getBuffer() == inputs[i]->getBuffer()) {
        std::println(stderr, "{} and {} contain identical bytecode", argv[j + 1], argv[i + 1]);
        return 1;
      }
    }
    SM50Destroy(initialize(*inputs[i]));
  }

  for (int sample = 1; sample <= 3; ++sample) {
    measure(inputs, Workload::Single, "single-cold", sample);
    measure(inputs, Workload::SharedCold, "same-cold-6", sample);
    measure(inputs, Workload::SharedWarm, "same-warm-6", sample);
    measure(inputs, Workload::Distinct, "different-cold-6", sample);
  }
}
