// Execute the pinned Microsoft DXC integer cases with their original shaders and expected values.
#include "d3d12_test.hpp"
#include <dxgi1_4.h>
#include <cstdint>
#include "dxc_official_cases.hpp"

int main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) return 77;
  ComPtr<IDXGIFactory4> factory;
  ComPtr<IDXGIAdapter1> adapter;
  CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  CHECK(factory->EnumAdapters1(0, &adapter));
  DXGI_ADAPTER_DESC1 desc{};
  CHECK(adapter->GetDesc1(&desc));
  if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || !wcsstr(desc.Description, L"Apple")) {
    printf("failed: expected the native Apple GPU, got %ls\n", desc.Description);
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  if (!GetModuleHandleW(L"winemetal.dll")) {
    printf("failed: Mullion Metal bridge was not loaded\n");
    return 1;
  }
  printf("adapter: %ls\n", desc.Description);
  D3D12_ROOT_PARAMETER parameter{D3D12_ROOT_PARAMETER_TYPE_UAV};
  auto signature = root_signature(device.Get(), {1, &parameter});
  if (!signature) return 1;
  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  unsigned wrong = 0;
  for (auto &test : dxc_cases) {
    step("Microsoft DXC %s", test.name);
    auto shader = test.compile(compiler);
    if (shader.empty()) return 1;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{signature.Get(), bytecode(shader)};
    ComPtr<ID3D12PipelineState> pipeline;
    CHECK(device->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&pipeline)));
    struct Element { int32_t input, output; } data[64]{};
    for (unsigned i = 0; i < 64; ++i) data[i] = {i < 9 ? test.input[i] : 0, 0x12345678};
    auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(data), D3D12_RESOURCE_STATE_GENERIC_READ);
    auto result = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, sizeof(data), D3D12_RESOURCE_STATE_COPY_DEST,
                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, sizeof(data), D3D12_RESOURCE_STATE_COPY_DEST);
    void *mapped;
    CHECK(upload->Map(0, nullptr, &mapped));
    memcpy(mapped, data, sizeof(data));
    upload->Unmap(0, nullptr);
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(), IID_PPV_ARGS(&list)));
    list->CopyResource(result.Get(), upload.Get());
    transition(list.Get(), result.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->SetComputeRootSignature(signature.Get());
    list->SetComputeRootUnorderedAccessView(0, result->GetGPUVirtualAddress());
    list->Dispatch(1, 1, 1);
    transition(list.Get(), result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyResource(readback.Get(), result.Get());
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    Element *output;
    CHECK(readback->Map(0, nullptr, reinterpret_cast<void **>(&output)));
    for (unsigned i = 0; i < 9; ++i)
      if (output[i].input != test.input[i] || output[i].output != test.expected[i]) {
        printf("wrong: %s input %d: got %d, want %d\n", test.name, test.input[i], output[i].output, test.expected[i]);
        ++wrong;
      }
    readback->Unmap(0, nullptr);
  }
  printf("%s: %u wrong results in 4 official DXC cases (36 values)\n", wrong ? "failed" : "passed", wrong);
  return wrong != 0;
}
