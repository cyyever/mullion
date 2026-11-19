// contract: D3D11.3 15.4: "Clipping of the Z coordinates can be disabled by setting the DepthClipEnable"
// (the quotation ends before its state-object cross-reference). D3D12_RASTERIZER_DESC specifies the same clip
// volume. ../raster.hpp supplies independent homogeneous clipping, fixed-point edge and viewport expectations.
// direct, bundle and ExecuteIndirect draws must produce identical pixels, including SV_IsFrontFace and depth.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_rasterizer_desc
#include "d3d12_test.hpp"
#include "../raster.hpp"
#include "../raster_conservative.hpp"
#include <cstddef>
#include <bit>
#include <map>

int main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  if (!raster::reference_contract() || !raster_conservative::reference_contract())
    return 1;
  step("compile raster shaders before device creation");
  std::string codes[raster::ModeCount], gs_code[raster::ModeCount][raster::Triangle + 1];

  for (unsigned mode = raster::None; mode < raster::ModeCount; mode++) {
    codes[mode] = compiler.compile(raster::hlsl, "vs", "vs", {"DISTANCES=" + std::to_string(mode)});
    if (codes[mode].empty())
      return 1;
    const char *entries[] = {"gs_point", "gs_line", "gs_triangle", "gs_line_strips"};
    for (unsigned shape = 0; shape < std::size(entries); shape++) {
      gs_code[mode][shape] =
          compiler.compile(raster::hlsl, entries[shape], "gs", {"DISTANCES=" + std::to_string(mode)});
      if (gs_code[mode][shape].empty())
        return 1;
    }
  }
  auto ps = compiler.compile(raster::hlsl, "ps", "ps", {"DISTANCES=0"});
  auto inner = compiler.compile(raster_conservative::hlsl, "ps_inner", "ps");
  if (ps.empty() || inner.empty())
    return 1;
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
  auto rs = root_signature(device.Get(),
                           {0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT});
  if (!expect(!!rs, "root signature was not created"))
    return verdict();
  auto all = raster::cases();
  const auto original_cases = all.size();
  for (size_t i = 0; i < original_cases; i++) {
    if (all[i].geometry || all[i].name != "post-snap degeneracy, ties and face culling")
      continue;
    for (bool horizontal : {false, true}) {
      auto c = all[i];
      c.name = "wireframe ties with translated viewport and homogeneous scale";
      c.viewport.x = 0.25f;
      c.viewport.y = 0.25f;
      for (auto &v : c.vertices) {
        if (horizontal) std::swap(v.position[0], v.position[1]);
        for (auto &coordinate : v.position) coordinate *= 2;
      }
      all.push_back(c);
      c.geometry = true;
      c.name += " through GS";
      all.push_back(std::move(c));
    }
  }
  auto boundaries = raster::viewport_boundaries(D3D12_VIEWPORT_BOUNDS_MIN, D3D12_VIEWPORT_BOUNDS_MAX);
  all.insert(all.end(), boundaries.begin(), boundaries.end());
  auto conservative = raster_conservative::cases(options.ConservativeRasterizationTier);
  all.insert(all.end(), conservative.begin(), conservative.end());
  const auto alias_begin = all.size();
  for (auto weights : {std::array{1.0f, 1.0f}, std::array{0.0625f, 16.0f}, std::array{16.0f, 0.0625f}})
    for (bool reverse : {false, true}) {
      auto c = raster::base("aliased line position interpolation with unequal homogeneous weights");
      c.shape = raster::Line;
      c.viewport.low = 0.125f;
      c.viewport.high = 0.875f;
      c.vertices = {raster::vertex(2.25f, 3.25f, 0.2f, weights[0]),
                    raster::vertex(12.25f, 9.25f, 0.8f, weights[1])};
      if (reverse) std::swap(c.vertices[0], c.vertices[1]);
      all.push_back(c);
    }
  for (auto viewport : {raster::Viewport{0, 0, 16384, 16384}, raster::Viewport{-16384, -8192, 32768, 16384},
                        raster::Viewport{-32768, -32768, 65535, 65535}}) {
    auto c = raster::base("aliased line coverage in a large viewport");
    c.shape = raster::Line;
    c.viewport = viewport;
    auto vertex = [&](float x, float y) {
      return raster::Vertex{{(x - viewport.x) * 2 / viewport.w - 1,
                             1 - (y - viewport.y) * 2 / viewport.h, 0.5f, 1}, {}};
    };
    c.vertices = {vertex(2.25f, 3.25f), vertex(12.25f, 9.25f)};
    all.push_back(c);
  }
  const auto alias_end = all.size();
  for (auto i = alias_begin; i < alias_end; i++) {
    auto c = all[i];
    c.geometry = true;
    c.name += " through GS";
    all.push_back(std::move(c));
  }
  const auto strip_begin = all.size();
  for (auto mode : {raster::None, raster::Clip, raster::Cull})
    for (bool tall : {false, true}) {
      auto c = raster::base("GS line strips retain adjacent vertices and RestartStrip boundaries");
      c.shape = raster::Line;
      c.geometry = c.multisample = c.additive = true;
      c.mode = mode;
      (tall ? c.viewport.w : c.viewport.h) /= 2;
      auto a = raster::vertex(2.25f, 3.25f), b = raster::vertex(12.25f, 9.25f);
      a.distance.fill(-1);
      b.distance.fill(mode == raster::Cull ? -1 : 1);
      c.vertices = {a, b, b, a, a, b};
      all.push_back(c);
      if (mode == raster::None) {
        c.multisample = false;
        c.name = "aliased GS line strips retain adjacent vertices and RestartStrip boundaries";
        all.push_back(std::move(c));
      }
    }
  const auto capacity = std::max_element(all.begin(), all.end(), [](const auto &a, const auto &b) {
                          return a.vertices.size() < b.vertices.size();
                        })->vertices.size();
  const UINT vertex_bytes = sizeof(raster::Vertex) * capacity;
  auto vb = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, vertex_bytes * all.size(), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto args = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(D3D12_DRAW_ARGUMENTS) * all.size(),
                     D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(vb && args, "upload buffers were not created"))
    return verdict();
  char *vertices;
  D3D12_DRAW_ARGUMENTS *arguments;
  CHECK(vb->Map(0, nullptr, reinterpret_cast<void **>(&vertices)));
  CHECK(args->Map(0, nullptr, reinterpret_cast<void **>(&arguments)));
  for (size_t i = 0; i < all.size(); i++) {
    memset(vertices + i * vertex_bytes, 0, vertex_bytes);
    if (!all[i].vertices.empty())
      memcpy(vertices + i * vertex_bytes, all[i].vertices.data(), all[i].vertices.size() * sizeof(raster::Vertex));
    arguments[i] = {UINT(i >= strip_begin ? 2 : all[i].vertices.size()), all[i].instances, 0, 0};
  }
  vb->Unmap(0, nullptr);
  args->Unmap(0, nullptr);
  D3D12_INDIRECT_ARGUMENT_DESC draw{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW};
  D3D12_COMMAND_SIGNATURE_DESC signature_desc{sizeof(D3D12_DRAW_ARGUMENTS), 1, &draw};
  ComPtr<ID3D12CommandSignature> signature;
  CHECK(device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&signature)));
  D3D12_INPUT_ELEMENT_DESC elements[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(raster::Vertex, position),
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"DISTANCE", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(raster::Vertex, distance),
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"DISTANCE", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(raster::Vertex, distance) + sizeof(raster::Color),
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
  step("rasterizer PSO enum validation and conservative capability");
  D3D12_GRAPHICS_PIPELINE_STATE_DESC validation{};
  validation.pRootSignature = rs.Get();
  validation.VS = bytecode(codes[raster::None]);
  validation.PS = bytecode(ps);
  validation.InputLayout = {elements, UINT(std::size(elements))};
  validation.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  validation.RasterizerState.DepthClipEnable = TRUE;
  validation.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
  validation.SampleDesc = {1, 0};
  validation.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  validation.NumRenderTargets = 1;
  validation.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
  validation.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  enum Field { Fill, Culling, Forced, Conservative };
  for (auto field : {Fill, Culling, Forced, Conservative})
    for (UINT value : {0u, 1u, 2u, 3u, 4u, 17u, ~0u}) {
      auto desc = validation;
      if (field == Fill) {
        if (value == D3D12_FILL_MODE_SOLID || value == D3D12_FILL_MODE_WIREFRAME)
          continue;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE(value);
      } else if (field == Culling) {
        if (value >= D3D12_CULL_MODE_NONE && value <= D3D12_CULL_MODE_BACK)
          continue;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE(value);
      } else if (field == Forced) {
        if (value == 0 || value == 1 || value == 4 || value == 8 || value == 16)
          continue;
        desc.RasterizerState.ForcedSampleCount = value;
      } else {
        if (value == D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF || value == D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON)
          continue;
        desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE(value);
      }
      ComPtr<ID3D12PipelineState> state;
      HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&state));
      expect(hr == E_INVALIDARG && !state, "invalid rasterizer field %u value %#x returned %#lx", field, value, hr);
    }
  for (auto fill : {D3D12_FILL_MODE_SOLID, D3D12_FILL_MODE_WIREFRAME}) {
    auto desc = validation;
    desc.RasterizerState.FillMode = fill;
    desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON;
    ComPtr<ID3D12PipelineState> state;
    HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&state));
    bool supported = options.ConservativeRasterizationTier != D3D12_CONSERVATIVE_RASTERIZATION_TIER_NOT_SUPPORTED &&
                     fill == D3D12_FILL_MODE_SOLID;
    expect(supported ? hr == S_OK && state : FAILED(hr) && !state, "conservative tier %u fill %u returned %#lx",
           options.ConservativeRasterizationTier, fill, hr);
  }
  {
    auto desc = validation;
    desc.PS = bytecode(inner);
    desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON;
    ComPtr<ID3D12PipelineState> state;
    HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&state));
    expect(options.ConservativeRasterizationTier >= D3D12_CONSERVATIVE_RASTERIZATION_TIER_3 ? hr == S_OK && state
                                                                                            : FAILED(hr) && !state,
           "InnerCoverage PSO at tier%u returned %#lx", options.ConservativeRasterizationTier, hr);
  }
  D3D12_RESOURCE_DESC td{D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                         0,
                         raster::width,
                         raster::height,
                         1,
                         1,
                         DXGI_FORMAT_R32G32B32A32_FLOAT,
                         {1, 0},
                         D3D12_TEXTURE_LAYOUT_UNKNOWN,
                         D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  ComPtr<ID3D12Resource> target;
  CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                        IID_PPV_ARGS(&target)));
  D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  ComPtr<ID3D12DescriptorHeap> descriptors;
  CHECK(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&descriptors)));
  auto rtv = descriptors->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 bytes;
  device->GetCopyableFootprints(&td, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  const UINT64 slot = (bytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) / D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT *
                      D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
  constexpr size_t batch_size = 256;
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, slot * batch_size, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(!!readback, "readback buffer was not created"))
    return verdict();
  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator, bundle_allocator;
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&bundle_allocator)));
  std::map<std::array<unsigned, 14>, ComPtr<ID3D12PipelineState>> pipelines;
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  for (unsigned path = 0; path < 3; path++) {
    for (size_t first = 0; first < all.size(); first += batch_size) {
      const size_t last = std::min(first + batch_size, all.size());
      if (path || first) {
        if (path == 1)
          CHECK(bundle_allocator->Reset());
        CHECK(allocator->Reset());
        CHECK(list->Reset(allocator.Get(), nullptr));
      }
      CHECK(forget(readback.Get()));
      std::vector<ComPtr<ID3D12GraphicsCommandList>> bundles;
      for (size_t i = first; i < last; i++) {
        const auto &c = all[i];
        step("path %u case %zu: %s", path, i, c.name.c_str());
        std::array<unsigned, 14> key{c.mode,
                                     c.shape,
                                     c.cull,
                                     c.ccw,
                                     c.wire,
                                     c.depth_clip,
                                     c.multisample,
                                     c.aa,
                                     c.additive,
                                     unsigned(c.geometry) + unsigned(i >= strip_begin),
                                     c.conservative,
                                     c.inner,
                                     std::bit_cast<unsigned>(c.bias_scale),
                                     std::bit_cast<unsigned>(c.bias_clamp)};
        auto &pso = pipelines[key];
        if (!pso) {
          D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
          pd.pRootSignature = rs.Get();
          pd.VS = bytecode(codes[c.mode]);
          if (c.geometry)
            pd.GS = bytecode(gs_code[c.mode][i >= strip_begin ? raster::Triangle : c.shape - 1]);
          pd.PS = bytecode(c.inner ? inner : ps);
          pd.InputLayout = {elements, UINT(std::size(elements))};
          pd.RasterizerState = {c.wire ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID,
                                c.cull == raster::NoCull      ? D3D12_CULL_MODE_NONE
                                : c.cull == raster::CullFront ? D3D12_CULL_MODE_FRONT
                                                              : D3D12_CULL_MODE_BACK};
          pd.RasterizerState.FrontCounterClockwise = c.ccw;
          pd.RasterizerState.DepthClipEnable = c.depth_clip;
          pd.RasterizerState.MultisampleEnable = c.multisample;
          pd.RasterizerState.AntialiasedLineEnable = c.aa;
          pd.RasterizerState.SlopeScaledDepthBias = c.bias_scale;
          pd.RasterizerState.DepthBiasClamp = c.bias_clamp;
          pd.RasterizerState.ConservativeRaster =
              c.conservative ? D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON : D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
          if (c.additive) {
            auto &blend = pd.BlendState.RenderTarget[0];
            blend.BlendEnable = TRUE;
            blend.SrcBlend = blend.DestBlend = blend.SrcBlendAlpha = blend.DestBlendAlpha = D3D12_BLEND_ONE;
            blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
          }
          pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
          pd.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
          pd.SampleDesc = {1, 0};
          pd.PrimitiveTopologyType = c.shape == raster::Point  ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT
                                     : c.shape == raster::Line ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE
                                                               : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
          pd.NumRenderTargets = 1;
          pd.RTVFormats[0] = td.Format;
          CHECK(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)));
        }
        D3D12_VIEWPORT vp{c.viewport.x, c.viewport.y, c.viewport.w, c.viewport.h, c.viewport.low, c.viewport.high};
        D3D12_RECT rect{c.rect.left, c.rect.top, c.rect.right, c.rect.bottom};
        // D3D12 always scissors; emulate the D3D10/11 disabled setting with the target's full rectangle.
        if (!c.scissor)
          rect = {0, 0, raster::width, raster::height};
        list->RSSetViewports(1, &vp);
        list->RSSetScissorRects(1, &rect);
        list->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const float clear[raster::lanes] = {};
        list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        ComPtr<ID3D12GraphicsCommandList> bundle;
        if (path == 1)
          CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, bundle_allocator.Get(), pso.Get(),
                                          IID_PPV_ARGS(&bundle)));
        auto commands = path == 1 ? bundle.Get() : list.Get();
        commands->SetGraphicsRootSignature(rs.Get());
        commands->SetPipelineState(pso.Get());
        D3D12_VERTEX_BUFFER_VIEW view{vb->GetGPUVirtualAddress() + i * vertex_bytes, vertex_bytes,
                                      sizeof(raster::Vertex)};
        commands->IASetVertexBuffers(0, 1, &view);
        commands->IASetPrimitiveTopology(
            c.shape == raster::Point ? D3D_PRIMITIVE_TOPOLOGY_POINTLIST
            : c.shape == raster::Line
                ? (c.strip ? D3D_PRIMITIVE_TOPOLOGY_LINESTRIP : D3D_PRIMITIVE_TOPOLOGY_LINELIST)
                : (c.strip ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST));
        if (path == 2)
          commands->ExecuteIndirect(signature.Get(), 1, args.Get(), i * sizeof(D3D12_DRAW_ARGUMENTS), nullptr, 0);
        else
          commands->DrawInstanced(i >= strip_begin ? 2 : c.vertices.size(), c.instances, 0, 0);
        if (bundle) {
          CHECK(bundle->Close());
          list->ExecuteBundle(bundle.Get());
          bundles.push_back(bundle);
        }
        transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION from{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
            into{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
        into.PlacedFootprint.Offset = (i - first) * slot;
        list->CopyTextureRegion(&into, 0, 0, 0, &from, nullptr);
        transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
      }
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      char *mapped;
      CHECK(readback->Map(0, nullptr, reinterpret_cast<void **>(&mapped)));
      for (size_t i = first; i < last; i++) {
        step("read path %u case %zu: %s", path, i, all[i].name.c_str());
        if (all[i].conservative)
          raster_conservative::compare(all[i], options.ConservativeRasterizationTier, mapped + (i - first) * slot,
                                       footprint.Footprint.RowPitch);
        else
          raster::compare(all[i], mapped + (i - first) * slot, footprint.Footprint.RowPitch, false, true);
      }
      readback->Unmap(0, nullptr);
    }
  }
  return verdict();
}
