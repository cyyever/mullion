// Measures Apple Silicon fragment throughput vs live-register pressure (low live regs, batched 32-reg hoist window,
// excessive 192-reg live pressure that spills, and half4 fp16 accumulation) on a 1920x1080 32-tap fragment shader.
import Metal

let device = MTLCreateSystemDefaultDevice()!
let queue = device.makeCommandQueue()!

let source = """
#include <metal_stdlib>
using namespace metal;

struct VOut { float4 pos [[position]]; float2 uv; };

vertex VOut vs_tri(uint id [[vertex_id]]) {
  float2 p = float2((id << 1) & 2, id & 2);
  VOut o;
  o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
  o.uv = p;
  return o;
}

// Serial accumulation (<= 8 live float regs)
fragment float4 fs_low_reg(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler s [[sampler(0)]]) {
  float4 acc = 0.0;
  for (int i = 0; i < 32; i++) {
    float2 off = float2(float(i & 7) * 0.001, float(i >> 3) * 0.001);
    acc += tex.sample(s, in.uv + off) * (1.0 / 32.0);
  }
  return acc;
}

// Batched 8x float4 (32 regs live at once, matching HoistLoadsPass window=32)
fragment float4 fs_win32(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler s [[sampler(0)]]) {
  float4 acc = 0.0;
  for (int g = 0; g < 4; g++) {
    float4 v[8];
    for (int i = 0; i < 8; i++) {
      int idx = g * 8 + i;
      v[i] = tex.sample(s, in.uv + float2(float(idx & 7) * 0.001, float(idx >> 3) * 0.001));
    }
    for (int i = 0; i < 8; i++) acc += v[i] * (1.0 / 32.0);
  }
  return acc;
}

// Excessive live pressure: keep all 32 float4 taps (128+ regs) live simultaneously before dependent cross-combine
fragment float4 fs_spill128(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler s [[sampler(0)]]) {
  float4 v[32];
  for (int i = 0; i < 32; i++) {
    v[i] = tex.sample(s, in.uv + float2(float(i & 7) * 0.001, float(i >> 3) * 0.001));
  }
  float4 acc = 0.0;
  for (int i = 0; i < 32; i++) {
    acc += v[i] * v[31 - i] * (1.0 / 32.0);
  }
  return acc;
}

// Half-precision (fp16) accumulation: half the register footprint on Apple Silicon
fragment half4 fs_half_reg(VOut in [[stage_in]], texture2d<half> tex [[texture(0)]], sampler s [[sampler(0)]]) {
  half4 acc = half4(0.0h);
  for (int i = 0; i < 32; i++) {
    float2 off = float2(float(i & 7) * 0.001, float(i >> 3) * 0.001);
    acc += tex.sample(s, in.uv + off) * half(1.0 / 32.0);
  }
  return acc;
}
"""

let lib = try device.makeLibrary(source: source, options: nil)
let vs = lib.makeFunction(name: "vs_tri")!

let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: 1920, height: 1080, mipmapped: false)
td.storageMode = .private; td.usage = [.shaderRead, .renderTarget]
let srcTex = device.makeTexture(descriptor: td)!
let dstTex = device.makeTexture(descriptor: td)!
let sd = MTLSamplerDescriptor(); sd.minFilter = .linear; sd.magFilter = .linear
let samp = device.makeSamplerState(descriptor: sd)!

func bench(_ name: String) throws {
  let pd = MTLRenderPipelineDescriptor()
  pd.vertexFunction = vs
  pd.fragmentFunction = lib.makeFunction(name: name)!
  pd.colorAttachments[0].pixelFormat = .rgba8Unorm
  let pso = try device.makeRenderPipelineState(descriptor: pd)
  let rpd = MTLRenderPassDescriptor()
  rpd.colorAttachments[0].texture = dstTex
  rpd.colorAttachments[0].loadAction = .dontCare
  rpd.colorAttachments[0].storeAction = .store

  var times: [Double] = []
  for iter in 0..<25 {
    let cb = queue.makeCommandBuffer()!
    let enc = cb.makeRenderCommandEncoder(descriptor: rpd)!
    enc.setRenderPipelineState(pso)
    enc.setFragmentTexture(srcTex, index: 0)
    enc.setFragmentSamplerState(samp, index: 0)
    enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
    enc.endEncoding()
    cb.commit()
    cb.waitUntilCompleted()
    if iter >= 5 { times.append((cb.gpuEndTime - cb.gpuStartTime) * 1000.0) }
  }
  times.sort()
  let med = times[times.count / 2]
  print(String(format: "%-14s  median %.3f ms  min %.3f ms", (name as NSString).utf8String!, med, times.first!))
}

print("GPU:", device.name, "(1920x1080, 32-tap fragment)")
try bench("fs_low_reg")
try bench("fs_win32")
try bench("fs_spill128")
try bench("fs_half_reg")
