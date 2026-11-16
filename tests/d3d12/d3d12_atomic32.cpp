// Each 32-bit atomic returns the previous value and leaves the expected value in
// device and threadgroup memory, for signed and unsigned integers, through DXBC and DXIL.
#include "d3d12_test.hpp"
#include <array>

static const char hlsl[] = R"hlsl(
RWStructuredBuffer<TYPE> results : register(u0);
groupshared TYPE shared_word;
#define OPERATIONS(word, base) \
  word = TYPE(7); \
  InterlockedAdd(word, TYPE(5), results[base]); \
  InterlockedAnd(word, TYPE(10), results[base + 1]); \
  InterlockedOr(word, TYPE(1), results[base + 2]); \
  InterlockedXor(word, TYPE(3), results[base + 3]); \
  InterlockedMax(word, TYPE(-5), results[base + 4]); \
  InterlockedMin(word, TYPE(3), results[base + 5]); \
  InterlockedExchange(word, TYPE(23), results[base + 6]); \
  InterlockedCompareExchange(word, TYPE(23), TYPE(31), results[base + 7]); \
  results[base + 8] = word;
[numthreads(1, 1, 1)]
void cs() {
  OPERATIONS(results[18], 0)
  OPERATIONS(shared_word, 9)
}
)hlsl";

static int run(const Compiler &compiler, bool is_signed) {
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  auto cs = compiler.compile(hlsl, "cs", "cs", {is_signed ? "TYPE=int" : "TYPE=uint"});
  if (cs.empty()) { puts("failed: HLSL did not compile"); return 1; }
  D3D12_ROOT_PARAMETER param{D3D12_ROOT_PARAMETER_TYPE_UAV};
  auto rs = root_signature(device.Get(), {1, &param});
  D3D12_COMPUTE_PIPELINE_STATE_DESC cs_desc{rs.Get(), bytecode(cs)};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateComputePipelineState(&cs_desc, IID_PPV_ARGS(&pso)));
  const UINT64 bytes = 19 * sizeof(uint32_t);
  auto results = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  list->SetComputeRootSignature(rs.Get());
  list->SetComputeRootUnorderedAccessView(0, results->GetGPUVirtualAddress());
  list->Dispatch(1, 1, 1);
  transition(list.Get(), results.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(readback.Get(), 0, results.Get(), 0, bytes);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  uint32_t *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));


  const std::array<uint32_t, 9> expected{7, 12, 8, 9, 10, is_signed ? 10u : uint32_t(-5), 3, 23, 31};
  unsigned wrong = 0;
  for (unsigned i = 0; i < 18; ++i)
    if (got[i] != expected[i % expected.size()]) {
      printf("%s word %u: %#x, expected %#x\n", is_signed ? "signed" : "unsigned", i, got[i], expected[i % expected.size()]);
      ++wrong;
    }
  printf("%s: %u wrong atomic results\n", wrong ? "failed" : "passed", wrong);
  return wrong != 0;
}

int main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) { puts("skipped: dxcompiler.dll not found"); return 77; }
  if (auto result = run(compiler, true)) return result;
  return run(compiler, false);
}
