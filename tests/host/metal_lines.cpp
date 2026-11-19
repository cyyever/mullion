// Native coverage control and GPU arithmetic checks for diamond-exit lines.
// Endpoints are clipped on the CPU. Large viewport tests evaluate the
// transform, coverage and interpolation in compute; hardware large-viewport
// rasterization is separate.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
bool expect(bool, const char *, ...);
#include "../raster.hpp"
#include <cstdio>
#include <cstring>
int main() {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    NSString *source = @R"metal(
#include <metal_stdlib>
using namespace metal;
struct Vertex { float4 p [[position]]; };
vertex Vertex native_line(uint id [[vertex_id]], constant float4 *ends [[buffer(0)]]) {
  return {ends[id]};
}
vertex Vertex rectangle(uint id [[vertex_id]], constant float4 *ends [[buffer(0)]], constant float4 &viewport [[buffer(1)]]) {
  float2 a = ends[0].xy / ends[0].w, b = ends[1].xy / ends[1].w;
  float2 lo = min(a,b) - (2.0/viewport.zw), hi = max(a,b) + (2.0/viewport.zw);
  uint2 corner[6] = {uint2(0,0),uint2(1,0),uint2(0,1),uint2(0,1),uint2(1,0),uint2(1,1)};
  return {float4(select(lo,hi,bool2(corner[id])),0.5,1)};
}
fragment float4 solid(Vertex v [[stage_in]]) { return float4(1,0.5,1,1); }
bool covered(float2 a, float2 b, float2 pixel) {
  a = rint(a*256)/256; b = rint(b*256)/256;
  long2 delta=long2(rint((b-a)*256)), start=long2(rint((a-pixel)*256));
  long2 lo=long2(0,1),hi=long2(1,1);
  bool live=any(delta!=0);
  for (int sx=-1;sx<=1;sx+=2) for (int sy=-1;sy<=1;sy+=2) {
    long d=sx*start.x+sy*start.y-128, slope=sx*delta.x+sy*delta.y;
    if(slope==0) live &= !(d>0 || (d==0 && sy<0));
    else if(slope>0) {long2 t=long2(-d,slope); if(t.x*hi.y<hi.x*t.y) hi=t;}
    else {long2 t=long2(d,-slope); if(t.x*lo.y>lo.x*t.y) lo=t;}
  }
  long2 last=start+delta, point=start*lo.y+delta*lo.x;
  long e=abs(last.x)+abs(last.y), m=abs(point.x)+abs(point.y);
  bool major=abs(delta.y)>abs(delta.x);
  bool end_inside=e<128 || (e==128 && (last.y>0 || (last.x==128 && major)));
  bool point_inside=m<128*lo.y || (m==128*lo.y && (point.y>0 || (point.x==128*lo.y && major)));
  bool nonempty=lo.x*hi.y<hi.x*lo.y || (lo.x*hi.y==hi.x*lo.y && point_inside);
  return live && nonempty && hi.x>=0 && (hi.x<hi.y || !end_inside);
}
fragment float4 diamond(Vertex v [[stage_in]], constant float4 *ends [[buffer(0)]], constant float4 &viewport [[buffer(1)]]) {
  float2 a = (ends[0].xy/ends[0].w * float2(1,-1) + 1) * viewport.zw * 0.5 + viewport.xy;
  float2 b = (ends[1].xy/ends[1].w * float2(1,-1) + 1) * viewport.zw * 0.5 + viewport.xy;
  if (!covered(a,b,v.p.xy)) discard_fragment();
  return float4(1,0.5,1,1);
}
struct Probe {float4 ends[2]; float4 viewport; float4 pixel_depth; float4 attributes[2];};
struct Result {float4 attributes; float4 status;};
Result evaluate_line(Probe p) {
  float2 a=rint(((p.ends[0].xy/p.ends[0].w*float2(1,-1)+1)*p.viewport.zw*0.5+p.viewport.xy)*256)/256;
  float2 b=rint(((p.ends[1].xy/p.ends[1].w*float2(1,-1)+1)*p.viewport.zw*0.5+p.viewport.xy)*256)/256;
  float2 delta=b-a;
  float t=dot(p.pixel_depth.xy-a,delta)/dot(delta,delta);
  float2 weights=float2(1-t,t)/float2(p.ends[0].w,p.ends[1].w);
  float inverse_w=weights.x+weights.y;
  float perspective=dot(weights,float2(p.attributes[0].x,p.attributes[1].x))/inverse_w;
  float linear=(1-t)*p.attributes[0].y+t*p.attributes[1].y;
  float z=(1-t)*p.ends[0].z/p.ends[0].w+t*p.ends[1].z/p.ends[1].w;
  z=p.pixel_depth.z+z*(p.pixel_depth.w-p.pixel_depth.z);
  return {float4(perspective,linear,p.attributes[0].z,z),float4(covered(a,b,p.pixel_depth.xy),1/inverse_w,t,0)};
}

kernel void probe_lines(device const Probe *probes [[buffer(0)]], device Result *results [[buffer(1)]], uint id [[thread_position_in_grid]]) {
  results[id]=evaluate_line(probes[id]);
}
vertex Vertex clipped_rectangle(uint id [[vertex_id]], constant Probe &p [[buffer(2)]]) {
  float2 a=(p.ends[0].xy/p.ends[0].w*float2(1,-1)+1)*p.viewport.zw*0.5+p.viewport.xy;
  float2 b=(p.ends[1].xy/p.ends[1].w*float2(1,-1)+1)*p.viewport.zw*0.5+p.viewport.xy;
  float2 origin=floor(p.pixel_depth.xy);
  float2 lo=clamp(min(a,b)-1-origin,float2(0),float2(1));
  float2 hi=clamp(max(a,b)+1-origin,float2(0),float2(1));
  uint2 corners[6]={uint2(0,0),uint2(1,0),uint2(0,1),uint2(0,1),uint2(1,0),uint2(1,1)};
  float2 screen=select(lo,hi,bool2(corners[id]));
  return {float4(screen*float2(2,-2)+float2(-1,1),0.5,1)};
}
struct FragmentResult {float4 attributes [[color(0)]]; float4 status [[color(1)]];};
fragment FragmentResult interpolated_line(Vertex input [[stage_in]], constant Probe &probe [[buffer(2)]]) {
  Probe p=probe;
  p.pixel_depth.xy=input.p.xy+floor(probe.pixel_depth.xy);
  auto result=evaluate_line(p);
  if(!result.status.x) discard_fragment();
  return {result.attributes,result.status};
}
)metal";
    NSError *error = nil;
    MTLCompileOptions *options = [MTLCompileOptions new];
    options.mathMode = MTLMathModeSafe;
    id<MTLLibrary> library =
        [device newLibraryWithSource:source options:options error:&error];
    if (!library) {
      fprintf(stderr, "%s\n", error.description.UTF8String);
      return 1;
    }
    id<MTLRenderPipelineState> pipelines[2];
    for (unsigned i = 0; i < 2; i++) {
      MTLRenderPipelineDescriptor *p = [MTLRenderPipelineDescriptor new];
      p.vertexFunction =
          [library newFunctionWithName:i ? @"rectangle" : @"native_line"];
      p.fragmentFunction =
          [library newFunctionWithName:i ? @"diamond" : @"solid"];
      p.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
      pipelines[i] =
          [device newRenderPipelineStateWithDescriptor:p error:&error];
      if (!pipelines[i]) {
        fprintf(stderr, "%s\n", error.description.UTF8String);
        return 1;
      }
    }
    auto desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                     width:16
                                    height:16
                                 mipmapped:NO];
    desc.usage = MTLTextureUsageRenderTarget;
    desc.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [device newTextureWithDescriptor:desc];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    unsigned cases = 0, mismatches[2] = {};
    auto scenes = raster::cases();
    const auto large_begin = scenes.size();
    for (auto viewport : {raster::Viewport{0, 0, 16384, 16384},
                          raster::Viewport{-16384, -8192, 32768, 16384},
                          raster::Viewport{-32768, -32768, 65535, 65535},
                          raster::Viewport{0.25f, 0.75f, 16383.5f, 8191.5f}}) {
      auto c = raster::base("large native viewport");
      c.shape = raster::Line;
      c.viewport = viewport;
      auto position = [&](float x, float y) {
        return raster::Vertex{{(x - viewport.x) * 2 / viewport.w - 1,
                               1 - (y - viewport.y) * 2 / viewport.h, 0.5f, 1},
                              {}};
      };
      c.vertices = {position(2.25f, 3.25f), position(12.25f, 9.25f)};
      scenes.push_back(c);
    }
    unsigned large_errors = 0;
    for (auto &c : scenes) {
      if (c.shape != raster::Line || c.multisample || c.geometry || c.strip ||
          c.mode != raster::None || c.vertices.size() != 2 || c.default_state ||
          !c.depth_clip)
        continue;
      auto a = c.vertices[0], b = c.vertices[1];
      if (!raster::clip_segment(c, a, b))
        continue;
      float ends[8];
      memcpy(ends, a.position.data(), 16);
      memcpy(ends + 4, b.position.data(), 16);
      auto expected = raster::expected(c);
      for (unsigned path = 0; path < 2; path++) {
        auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        auto command = [queue commandBuffer];
        auto encoder = [command renderCommandEncoderWithDescriptor:pass];
        [encoder setRenderPipelineState:pipelines[path]];
        [encoder setViewport:MTLViewport{c.viewport.x, c.viewport.y,
                                         c.viewport.w, c.viewport.h,
                                         c.viewport.low, c.viewport.high}];
        float viewport[] = {c.viewport.x, c.viewport.y, c.viewport.w,
                            c.viewport.h};
        [encoder setVertexBytes:viewport length:sizeof(viewport) atIndex:1];
        [encoder setFragmentBytes:viewport length:sizeof(viewport) atIndex:1];
        [encoder setVertexBytes:ends length:sizeof(ends) atIndex:0];
        [encoder setFragmentBytes:ends length:sizeof(ends) atIndex:0];
        [encoder drawPrimitives:path ? MTLPrimitiveTypeTriangle
                                     : MTLPrimitiveTypeLine
                    vertexStart:0
                    vertexCount:path ? 6 : 2];
        [encoder endEncoding];
        [command commit];
        [command waitUntilCompleted];
        if (command.status == MTLCommandBufferStatusError) {
          fprintf(stderr, "%s\n", command.error.description.UTF8String);
          return 1;
        }
        float pixels[16 * 16 * 4];
        [texture getBytes:pixels
              bytesPerRow:16 * 4 * sizeof(float)
               fromRegion:MTLRegionMake2D(0, 0, 16, 16)
              mipmapLevel:0];
        for (unsigned pixel = 0; pixel < 256; pixel++)
          if ((pixels[4 * pixel] != 0) != (expected[pixel][0] != 0)) {
            if (path && mismatches[path] < 12)
              printf("%s pixel %u,%u got %g want %g ends %g,%g -> %g,%g\n",
                     c.name.c_str(), pixel % 16, pixel / 16, pixels[4 * pixel],
                     expected[pixel][0], ends[0], ends[1], ends[4], ends[5]);
            if (path && &c >= scenes.data() + large_begin)
              large_errors++;
            mismatches[path]++;
          }
      }
      cases++;
    }
    printf("large native viewport candidate mismatches: %u\n", large_errors);
    // The largest intermediate is a product of two sums of 16.8 coordinates.
    constexpr int64_t coordinate_bound = int64_t(1) << 25;
    static_assert(8 * coordinate_bound * coordinate_bound < INT64_MAX);
    struct Probe {
      raster::Color ends[2], viewport, pixel_depth, attributes[2];
    };
    struct Result {
      raster::Color attributes, status;
    };
    static_assert(sizeof(Probe) == 96 && sizeof(Result) == 32);
    std::vector<Probe> probes;
    for (auto viewport :
         {raster::Color{0, 0, 16, 16}, raster::Color{0, 0, 16384, 16384},
          raster::Color{-16384, -8192, 32768, 16384},
          raster::Color{-32768, -32768, 65535, 65535},
          raster::Color{0.25f, 0.75f, 16383.5f, 8191.5f}})
      for (auto direction :
           {raster::Color{-1, 0, 1, 0}, raster::Color{0, -1, 0, 1},
            raster::Color{-1, -1, 1, 1}, raster::Color{-1, 0.5f, 1, -0.5f},
            raster::Color{-0.0001f, -0.0002f, 0.0001f, 0.0002f}})
        for (bool reverse : {false, true})
          for (auto ws : {std::array{1.0f, 1.0f}, std::array{0.0625f, 16.0f},
                          std::array{16.0f, 0.0625f}})
            for (float sample : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f})
              for (float offset : {-1.0f, 0.0f, 1.0f}) {
                Probe p{};
                p.viewport = viewport;
                p.ends[0] = {direction[0] * ws[0], direction[1] * ws[0],
                             0.2f * ws[0], ws[0]};
                p.ends[1] = {direction[2] * ws[1], direction[3] * ws[1],
                             0.8f * ws[1], ws[1]};
                p.attributes[0] = {-4, -1.25f, 17, 0};
                p.attributes[1] = {3, 8, -23, 0};
                if (reverse) {
                  std::swap(p.ends[0], p.ends[1]);
                  std::swap(p.attributes[0], p.attributes[1]);
                }
                raster::Viewport vp{viewport[0], viewport[1], viewport[2],
                                    viewport[3], 0.125f,      0.875f};
                auto a = raster::project({p.ends[0], {}}, vp),
                     b = raster::project({p.ends[1], {}}, vp);
                if (a.x == b.x && a.y == b.y)
                  continue;
                p.pixel_depth = {
                    float(std::floor(a.x + (b.x - a.x) * sample) + 0.5),
                    float(std::floor(a.y + (b.y - a.y) * sample) + 0.5 +
                          offset),
                    vp.low, vp.high};
                probes.push_back(p);
              }
    auto kernel = [library newFunctionWithName:@"probe_lines"];
    auto compute =
        [device newComputePipelineStateWithFunction:kernel error:&error];
    if (!compute) {
      fprintf(stderr, "%s\n", error.description.UTF8String);
      return 1;
    }
    auto input = [device newBufferWithBytes:probes.data()
                                     length:probes.size() * sizeof(Probe)
                                    options:MTLResourceStorageModeShared];
    auto output = [device newBufferWithLength:probes.size() * sizeof(Result)
                                      options:MTLResourceStorageModeShared];
    auto command = [queue commandBuffer];
    auto encoder = [command computeCommandEncoder];
    [encoder setComputePipelineState:compute];
    [encoder setBuffer:input offset:0 atIndex:0];
    [encoder setBuffer:output offset:0 atIndex:1];
    [encoder dispatchThreads:MTLSizeMake(probes.size(), 1, 1)
        threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [encoder endEncoding];
    [command commit];
    [command waitUntilCompleted];
    if (command.status == MTLCommandBufferStatusError) {
      fprintf(stderr, "%s\n", command.error.description.UTF8String);
      return 1;
    }
    auto results = static_cast<const Result *>(output.contents);
    unsigned coverage_errors = 0, interpolation_errors = 0, interpolated = 0;
    double largest_error = 0;
    for (size_t i = 0; i < probes.size(); i++) {
      auto &p = probes[i];
      auto &v = p.viewport;
      raster::Viewport vp{
          v[0], v[1], v[2], v[3], p.pixel_depth[2], p.pixel_depth[3]};
      auto a = raster::project({p.ends[0], {}}, vp),
           b = raster::project({p.ends[1], {}}, vp);
      bool covered = raster::diamond(a, b, p.pixel_depth[0], p.pixel_depth[1]);
      if (covered != bool(results[i].status[0]))
        printf("coverage probe %zu: a %.9g,%.9g b %.9g,%.9g pixel %.9g,%.9g "
               "got %g want %d\n",
               i, a.x, a.y, b.x, b.y, p.pixel_depth[0], p.pixel_depth[1],
               results[i].status[0], covered);
      coverage_errors += covered != bool(results[i].status[0]);
      if (!covered)
        continue;
      interpolated++;
      double dx = b.x - a.x, dy = b.y - a.y;
      double t =
          ((p.pixel_depth[0] - a.x) * dx + (p.pixel_depth[1] - a.y) * dy) /
          (dx * dx + dy * dy);
      double inverse_w = (1 - t) / p.ends[0][3] + t / p.ends[1][3];
      double expected[] = {
          ((1 - t) * p.attributes[0][0] / p.ends[0][3] +
           t * p.attributes[1][0] / p.ends[1][3]) /
              inverse_w,
          (1 - t) * p.attributes[0][1] + t * p.attributes[1][1],
          p.attributes[0][2], (1 - t) * a.z + t * b.z, 1 / inverse_w};
      for (unsigned lane = 0; lane < 5; lane++) {
        double actual =
            lane < 4 ? results[i].attributes[lane] : results[i].status[1];
        double error = std::abs(actual - expected[lane]);
        largest_error = std::max(largest_error, error);
        if (!std::isfinite(actual) ||
            error > 2e-5 * std::max(1.0, std::abs(expected[lane]))) {
          if (interpolation_errors < 8)
            printf("probe %zu lane %u got %.9g want %.9g\n", i, lane, actual,
                   expected[lane]);
          interpolation_errors++;
        }
      }
    }
    MTLRenderPipelineDescriptor *fragment_desc =
        [MTLRenderPipelineDescriptor new];
    fragment_desc.vertexFunction =
        [library newFunctionWithName:@"clipped_rectangle"];
    fragment_desc.fragmentFunction =
        [library newFunctionWithName:@"interpolated_line"];
    for (unsigned attachment = 0; attachment < 2; attachment++)
      fragment_desc.colorAttachments[attachment].pixelFormat =
          MTLPixelFormatRGBA32Float;
    auto fragment_pipeline =
        [device newRenderPipelineStateWithDescriptor:fragment_desc
                                               error:&error];
    if (!fragment_pipeline) {
      fprintf(stderr, "%s\n", error.description.UTF8String);
      return 1;
    }
    auto pixel_desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                     width:1
                                    height:1
                                 mipmapped:NO];
    pixel_desc.usage = MTLTextureUsageRenderTarget;
    pixel_desc.storageMode = MTLStorageModeShared;
    id<MTLTexture> pixel_targets[2] = {
        [device newTextureWithDescriptor:pixel_desc],
        [device newTextureWithDescriptor:pixel_desc]};
    unsigned raster_probes = 0, raster_coverage_errors = 0,
             raster_attribute_errors = 0;
    for (size_t i = 0; i < probes.size(); i++) {
      auto &p = probes[i];
      auto &vp = p.viewport;
      if (p.pixel_depth[0] < vp[0] || p.pixel_depth[0] >= vp[0] + vp[2] ||
          p.pixel_depth[1] < vp[1] || p.pixel_depth[1] >= vp[1] + vp[3])
        continue;
      auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
      for (unsigned attachment = 0; attachment < 2; attachment++) {
        pass.colorAttachments[attachment].texture = pixel_targets[attachment];
        pass.colorAttachments[attachment].loadAction = MTLLoadActionClear;
        pass.colorAttachments[attachment].storeAction = MTLStoreActionStore;
      }
      auto draw = [queue commandBuffer];
      auto render = [draw renderCommandEncoderWithDescriptor:pass];
      [render setRenderPipelineState:fragment_pipeline];
      [render setViewport:MTLViewport{0, 0, 1, 1, 0, 1}];
      [render setVertexBytes:&p length:sizeof(p) atIndex:2];
      [render setVertexBytes:p.ends length:sizeof(p.ends) atIndex:0];
      [render setVertexBytes:&vp length:sizeof(vp) atIndex:1];
      [render setFragmentBytes:&p length:sizeof(p) atIndex:2];
      [render drawPrimitives:MTLPrimitiveTypeTriangle
                 vertexStart:0
                 vertexCount:6];
      [render endEncoding];
      [draw commit];
      [draw waitUntilCompleted];
      if (draw.status == MTLCommandBufferStatusError) {
        fprintf(stderr, "%s\n", draw.error.description.UTF8String);
        return 1;
      }
      Result actual{};
      [pixel_targets[0] getBytes:actual.attributes.data()
                     bytesPerRow:16
                      fromRegion:MTLRegionMake2D(0, 0, 1, 1)
                     mipmapLevel:0];
      [pixel_targets[1] getBytes:actual.status.data()
                     bytesPerRow:16
                      fromRegion:MTLRegionMake2D(0, 0, 1, 1)
                     mipmapLevel:0];
      bool covered = results[i].status[0] != 0;
      if (covered != bool(actual.status[0])) {
        if (raster_coverage_errors < 8)
          printf("fragment coverage probe %zu got %g want %g\n", i,
                 actual.status[0], results[i].status[0]);
        raster_coverage_errors++;
      }
      if (covered && actual.status[0])
        for (unsigned lane = 0; lane < 5; lane++) {
          float got = lane < 4 ? actual.attributes[lane] : actual.status[1];
          float want =
              lane < 4 ? results[i].attributes[lane] : results[i].status[1];
          raster_attribute_errors +=
              !std::isfinite(got) ||
              std::abs(got - want) > 2e-5f * std::max(1.0f, std::abs(want));
        }
      raster_probes++;
    }
    printf("%u fragment probes: %u coverage errors, %u attribute errors\n",
           raster_probes, raster_coverage_errors, raster_attribute_errors);
    printf("%zu viewport probes, %u covered: %u coverage errors, %u "
           "interpolation errors, maximum absolute error %.9g\n",
           probes.size(), interpolated, coverage_errors, interpolation_errors,
           largest_error);
    printf("%u cases: native Metal %u coverage mismatches; diamond shader %u "
           "coverage mismatches\n",
           cases, mismatches[0], mismatches[1]);
    return mismatches[1] || coverage_errors || interpolation_errors ||
                   raster_coverage_errors || raster_attribute_errors
               ? 1
               : 0;
  }
}
