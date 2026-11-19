// Native coverage control for D3D diamond-exit lines. The CPU clips endpoints
// before both GPU paths. The integer candidate is tested at 16x16 here; this
// does not validate general interpolation or large viewports.
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
vertex Vertex rectangle(uint id [[vertex_id]], constant float4 *ends [[buffer(0)]]) {
  float2 a = ends[0].xy / ends[0].w, b = ends[1].xy / ends[1].w;
  float2 lo = min(a,b) - float2(2.0/16), hi = max(a,b) + float2(2.0/16);
  uint2 corner[6] = {uint2(0,0),uint2(1,0),uint2(0,1),uint2(0,1),uint2(1,0),uint2(1,1)};
  return {float4(select(lo,hi,bool2(corner[id])),0.5,1)};
}
fragment float4 solid(Vertex v [[stage_in]]) { return float4(1,0.5,1,1); }
fragment float4 diamond(Vertex v [[stage_in]], constant float4 *ends [[buffer(0)]]) {
  float2 a = (ends[0].xy/ends[0].w * float2(1,-1) + 1) * 8;
  float2 b = (ends[1].xy/ends[1].w * float2(1,-1) + 1) * 8;
  a = rint(a*256)/256; b = rint(b*256)/256;
  long2 delta=long2(rint((b-a)*256)), start=long2(rint((a-v.p.xy)*256));
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
  if(!(live && nonempty && hi.x>=0 && (hi.x<hi.y || !end_inside))) discard_fragment();
  return float4(1,0.5,1,1);
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
    for (auto &c : raster::cases()) {
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
            mismatches[path]++;
          }
      }
      cases++;
    }
    printf("%u cases: native Metal %u coverage mismatches; diamond shader %u "
           "coverage mismatches\n",
           cases, mismatches[0], mismatches[1]);
    return mismatches[1] ? 1 : 0;
  }
}
