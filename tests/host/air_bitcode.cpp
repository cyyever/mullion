// AIR must retain typed pointers even when the optimizer uses opaque pointers.
// Unsupported optimization promises must be removed before attribute groups are
// enumerated, including groups containing only one promise on functions/calls.
// Loading a library alone misses this regression: the driver upgrades bitcode
// when it creates the pipeline.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../../src/airconv/metallib_writer.hpp"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

static int testPipeline(llvm::Attribute::AttrKind promise) {
  @autoreleasepool {
    auto device = MTLCreateSystemDefaultDevice();
    if (!device || ![device supportsFamily:MTLGPUFamilyMetal4])
      return 1;
    llvm::LLVMContext context;
    llvm::SMDiagnostic diagnostic;
    auto module = llvm::parseAssemblyString(R"(
      target datalayout = "e-p:64:64:64-i64:64:64-n8:16:32"
      target triple = "air64_v28-apple-macosx26.0.0"
      %A = type { float }
      %B = type { float }
      declare i32 @llvm.ctlz.i32(i32, i1 immarg)
      declare void @llvm.lifetime.start.p0(ptr captures(none))
      declare void @llvm.lifetime.end.p0(ptr captures(none))
      define internal %A @repacked(float %value) {
        %result = insertvalue %A zeroinitializer, float %value, 0
        ret %A %result
      }
      define internal float @identity(float %value) {
        ret float %value
      }
      define <4 x float> @fragment_main(ptr addrspace(2) nofree %buffer) nocallback nocreateundeforpoison {
        %loaded = load i32, ptr addrspace(2) %buffer, align 4
        %bits = freeze i32 %loaded
        %slot = alloca i32, align 4
        call void @llvm.lifetime.start.p0(ptr %slot)
        store i32 %bits, ptr %slot, align 4
        %float = load float, ptr %slot, align 4
        call void @llvm.lifetime.end.p0(ptr %slot)
        %packed = call %B @repacked(float %float)
        %unpacked = extractvalue %B %packed, 0
        %value = call float @identity(float %unpacked)
        %neg = fneg float %value
        %zeros = call i32 @llvm.ctlz.i32(i32 %bits, i1 false)
        %integer = uitofp i32 %zeros to float
        %a = insertelement <4 x float> zeroinitializer, float %neg, i32 0
        %b = insertelement <4 x float> %a, float %integer, i32 1
        %scaled = fmul reassoc nsz arcp contract afn <4 x float> %b, <float 0.5, float 0.5, float 0.5, float 0.5>
        ret <4 x float> %scaled
      }
      !air.version = !{!0}
      !air.language_version = !{!1}
      !air.fragment = !{!2}
      !0 = !{i32 2, i32 8, i32 0}
      !1 = !{!"Metal", i32 4, i32 0, i32 0}
      !2 = !{ptr @fragment_main, !3, !5}
      !3 = !{!4}
      !4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
      !5 = !{!6}
      !6 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1,
             !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4,
             !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"buffer"}
    )",
                                            diagnostic, context);
    if (!module) {
      diagnostic.print("air_bitcode", llvm::errs());
      return 1;
    }
    auto *identity = module->getFunction("identity");
    identity->addFnAttr(promise);
    for (auto &block : *module->getFunction("fragment_main"))
      for (auto &inst : block)
        if (auto *call = llvm::dyn_cast<llvm::CallBase>(&inst))
          if (promise != llvm::Attribute::Speculatable)
            call->addFnAttr(promise);
    llvm::SmallVector<char, 0> bytes;
    llvm::raw_svector_ostream output(bytes);
    if (auto error = mullion::metallib::write(*module, output)) {
      llvm::errs() << llvm::toString(std::move(error)) << "\n";
      return 1;
    }
    if (!identity->hasFnAttribute(promise) ||
        !module->getFunction("llvm.ctlz.i32")->hasParamAttribute(1, llvm::Attribute::ImmArg))
      return 1;
    auto data = dispatch_data_create(bytes.data(), bytes.size(), nullptr,
                                     DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NSError *error = nil;
    auto library = [device newLibraryWithData:data error:&error];
    if (!library) {
      NSLog(@"AIR library: %@", error);
      return 1;
    }
    auto options = [MTLCompileOptions new];
    options.languageVersion = MTLLanguageVersion4_0;
    auto vertexLibrary = [device
        newLibraryWithSource:
            @"#include <metal_stdlib>\nusing namespace metal; vertex float4 "
            @"v(uint i [[vertex_id]]) { float2 p = float2((i << 1) & 2, i & 2); return float4(p * 2 - 1, 0, 1); }"
                     options:options
                       error:&error];
    if (!vertexLibrary) {
      NSLog(@"vertex: %@", error);
      return 1;
    }
    auto descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = [vertexLibrary newFunctionWithName:@"v"];
    descriptor.fragmentFunction =
        [library newFunctionWithName:@"fragment_main"];
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
    auto pipeline =
        [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (!pipeline) {
      NSLog(@"AIR pipeline: %@", error);
      return 1;
    }
    auto textureDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                        width:1 height:1 mipmapped:NO];
    textureDesc.storageMode = MTLStorageModeShared;
    textureDesc.usage = MTLTextureUsageRenderTarget;
    auto texture = [device newTextureWithDescriptor:textureDesc];
    auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(-1, -1, -1, -1);
    auto queue = [device newCommandQueue];
    auto command = [queue commandBuffer];
    auto encoder = [command renderCommandEncoderWithDescriptor:pass];
    uint32_t zero = 0;
    [encoder setRenderPipelineState:pipeline];
    [encoder setFragmentBytes:&zero length:sizeof(zero) atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    [command commit];
    [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted) {
      NSLog(@"AIR rendering: %@", command.error);
      return 1;
    }
    float pixel[4];
    [texture getBytes:pixel bytesPerRow:sizeof(pixel) fromRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0];
    if (pixel[0] != 0 || pixel[1] != 16 || pixel[2] != 0 || pixel[3] != 0) {
      fprintf(stderr, "wrong AIR pixel: %g %g %g %g\n", pixel[0], pixel[1], pixel[2], pixel[3]);
      return 1;
    }
    puts("passed: Metal 4 AIR pipeline and GPU pixel readback");
    return 0;
  }
}

int main() {
  for (auto promise : {llvm::Attribute::Speculatable,
                       llvm::Attribute::NoCallback,
                       llvm::Attribute::NoCreateUndefOrPoison,
                       llvm::Attribute::NoDivergenceSource,
                       llvm::Attribute::MustProgress,
                       llvm::Attribute::NoFree,
                       llvm::Attribute::NoSync,
                       llvm::Attribute::WillReturn}) {
    llvm::outs() << "Testing " << llvm::Attribute::getNameFromAttrKind(promise) << "\n";
    llvm::outs().flush();
    if (testPipeline(promise)) return 1;
  }
  return 0;
}
