// Compare identical ordinary linear sampling with the converter's minmax
// fallback present/absent. Optional argv[1]: original game's DXBC (kept outside
// Git). The generated VS supplies its COLOR/UV inputs. GPU timestamps exclude
// warmup/compilation; every measured frame is read back and hashed.
#include "../d3d11/d3d11_test.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <thread>

static const char source[] = R"(
struct V { float4 pos:SV_Position; float4 color:COLOR; float2 uv:TEXCOORD0;
           float4 unused1:TEXCOORD1; float4 unused2:TEXCOORD2; };
V vs(uint id:SV_VertexID) {
 V v; v.uv=float2(id==1?2:0,id==2?2:0); v.pos=float4(v.uv*float2(2,-2)+float2(-1,1),0,1);
 v.color=float4(1,1,1,0.5); v.unused1=v.unused2=0; return v;
}
Texture2D image:register(t0); SamplerState smp:register(s0);
cbuffer Params:register(b0) { float4 padding[3]; float4 add; };
float4 ps(V v):SV_Target {
 float4 c=(image.Sample(smp,v.uv)+add)*float4(v.color.rgb,round(v.color.a*255)/255);
 return float4(c.rgb*c.a,c.a);
})";

int main(int argc, char **argv) {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> ctx;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                          nullptr, 0, D3D11_SDK_VERSION, &device, nullptr,
                          &ctx));
  auto vs_code = compile(source, "vs", "vs"),
       ps_code = compile(source, "ps", "ps");
  if (!vs_code || !ps_code)
    return 1;
  std::vector<char> original;
  if (argc > 1) {
    std::ifstream f(argv[1], std::ios::binary);
    original.assign(std::istreambuf_iterator<char>(f), {});
    if (original.empty())
      return 1;
  }
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  CHECK(device->CreateVertexShader(vs_code->GetBufferPointer(),
                                   vs_code->GetBufferSize(), nullptr, &vs));
  CHECK(device->CreatePixelShader(
      original.empty() ? ps_code->GetBufferPointer() : original.data(),
      original.empty() ? ps_code->GetBufferSize() : original.size(), nullptr,
      &ps));
  D3D11_TEXTURE2D_DESC td{};
  td.Width = 1920;
  td.Height = 1080;
  td.MipLevels = td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.BindFlags = D3D11_BIND_RENDER_TARGET;
  ComPtr<ID3D11Texture2D> target, staging, input;
  ComPtr<ID3D11RenderTargetView> rtv;
  CHECK(device->CreateTexture2D(&td, nullptr, &target));
  CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
  td.BindFlags = 0;
  td.Usage = D3D11_USAGE_STAGING;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  CHECK(device->CreateTexture2D(&td, nullptr, &staging));
  td.Width = td.Height = 64;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.CPUAccessFlags = 0;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  std::vector<unsigned> pixels(64 * 64);
  for (unsigned i = 0; i < pixels.size(); ++i)
    pixels[i] = 0xff000000u | ((i * 7919) & 0xffffff);
  D3D11_SUBRESOURCE_DATA data{pixels.data(), 64 * 4};
  CHECK(device->CreateTexture2D(&td, &data, &input));
  ComPtr<ID3D11ShaderResourceView> srv;
  CHECK(device->CreateShaderResourceView(input.Get(), nullptr, &srv));
  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  ComPtr<ID3D11SamplerState> sampler;
  CHECK(device->CreateSamplerState(&sd, &sampler));
  D3D11_BLEND_DESC bd{};
  auto &b = bd.RenderTarget[0];
  b.BlendEnable = TRUE;
  b.SrcBlend = b.SrcBlendAlpha = D3D11_BLEND_ONE;
  b.DestBlend = b.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  b.BlendOp = b.BlendOpAlpha = D3D11_BLEND_OP_ADD;
  b.RenderTargetWriteMask = 15;
  ComPtr<ID3D11BlendState> blend;
  CHECK(device->CreateBlendState(&bd, &blend));
  auto cb = buffer(device.Get(), 64, D3D11_BIND_CONSTANT_BUFFER);
  if (!cb)
    return 1;
  ctx->VSSetShader(vs.Get(), nullptr, 0);
  ctx->PSSetShader(ps.Get(), nullptr, 0);
  ctx->PSSetShaderResources(0, 1, srv.GetAddressOf());
  ctx->PSSetSamplers(0, 1, sampler.GetAddressOf());
  ctx->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
  ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
  ctx->OMSetBlendState(blend.Get(), nullptr, ~0u);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  D3D11_VIEWPORT vp{0, 0, 1920, 1080, 0, 1};
  ctx->RSSetViewports(1, &vp);
  ComPtr<ID3D11Query> begin, end, disjoint;
  D3D11_QUERY_DESC q{D3D11_QUERY_TIMESTAMP, 0};
  CHECK(device->CreateQuery(&q, &begin));
  CHECK(device->CreateQuery(&q, &end));
  q.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
  CHECK(device->CreateQuery(&q, &disjoint));
  auto wait = [&](ID3D11Query *query, void *value, UINT bytes) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    HRESULT hr;
    while ((hr = ctx->GetData(query, value, bytes, 0)) == S_FALSE) {
      if (std::chrono::steady_clock::now() > deadline)
        return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return hr == S_OK;
  };
  std::vector<double> times;
  uint64_t baseline = 0;
  for (unsigned frame = 0; frame < 25; ++frame) {
    float clear[4] = {};
    ctx->ClearRenderTargetView(rtv.Get(), clear);
    ctx->Begin(disjoint.Get());
    ctx->Draw(3, 0);
    ctx->End(begin.Get());
    auto submitted = std::chrono::steady_clock::now();
    for (unsigned draw = 0; draw < 64; ++draw)
      ctx->Draw(3, 0);
    ctx->End(end.Get());
    ctx->End(disjoint.Get());
    ctx->Flush();
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT clock{};
    UINT64 a = 0, z = 0;
    if (!wait(disjoint.Get(), &clock, sizeof(clock)) ||
        !wait(begin.Get(), &a, sizeof(a)) || !wait(end.Get(), &z, sizeof(z)) ||
        clock.Disjoint || !clock.Frequency || z <= a)
      return 1;
    ctx->CopyResource(staging.Get(), target.Get());
    D3D11_MAPPED_SUBRESOURCE mapped;
    CHECK(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    uint64_t hash = 14695981039346656037ull;
    for (unsigned y = 0; y < 1080; ++y)
      for (unsigned x = 0; x < 1920 * 4; ++x)
        hash =
            (hash ^ ((unsigned char *)mapped.pData)[y * mapped.RowPitch + x]) *
            1099511628211ull;
    ctx->Unmap(staging.Get(), 0);
    if (frame && hash != baseline)
      return 1;
    baseline = hash;
    double ms = double(z - a) * 1000 / clock.Frequency;
    double cpu_ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - submitted)
                        .count();
    if (!a || ms > cpu_ms + 1) {
      printf("invalid timestamps: begin=%llu end=%llu frequency=%llu gpu_ms=%f "
             "cpu_ms=%f\n",
             (unsigned long long)a, (unsigned long long)z,
             (unsigned long long)clock.Frequency, ms, cpu_ms);
      return 1;
    }
    if (frame >= 5)
      times.push_back(ms);
  }
  std::sort(times.begin(), times.end());
  printf("gpu_median_ms=%.6f gpu_p95_ms=%.6f pixel_hash=%016llx samples=%zu "
         "draws=64\n",
         times[times.size() / 2], times[times.size() * 95 / 100],
         (unsigned long long)baseline, times.size());
  return 0;
}
