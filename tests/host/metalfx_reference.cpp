// The Apple MetalFX denoiser is the image oracle for the D3D12/NGX bridge.
// It need not preserve a constant image exactly: the M2 model has a one-pixel
// deviation even without Mullion. Compare the entire native image instead.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdlib>
int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: metalfx_reference <output.rgba>\n");
    return 1;
  }
  @autoreleasepool {
    const NSUInteger width = 256, height = 144, output_width = width * 2,
                     output_height = height * 2;
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    auto descriptor = [MTLFXTemporalDenoisedScalerDescriptor new];
    descriptor.colorTextureFormat = MTLPixelFormatRGBA8Unorm;
    descriptor.outputTextureFormat = MTLPixelFormatRGBA8Unorm;
    descriptor.depthTextureFormat = MTLPixelFormatR32Float;
    descriptor.motionTextureFormat = MTLPixelFormatRG16Float;
    descriptor.diffuseAlbedoTextureFormat =
        descriptor.specularAlbedoTextureFormat =
            descriptor.normalTextureFormat = MTLPixelFormatRGBA16Float;
    descriptor.roughnessTextureFormat = MTLPixelFormatR16Float;
    descriptor.inputWidth = width;
    descriptor.inputHeight = height;
    descriptor.outputWidth = output_width;
    descriptor.outputHeight = output_height;
    id<MTLFXTemporalDenoisedScaler> scaler =
        [descriptor newTemporalDenoisedScalerWithDevice:device];
    if (!scaler) {
      NSLog(@"Native MetalFX denoiser unavailable");
      return 2;
    }
    auto make_texture = [&](MTLPixelFormat f, NSUInteger width,
                            NSUInteger height, MTLTextureUsage usage,
                            bool output = false) {
      auto texture =
          [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f
                                                             width:width
                                                            height:height
                                                         mipmapped:NO];
      texture.storageMode =
          output ? MTLStorageModePrivate : MTLStorageModeShared;
      texture.usage = usage;
      return [device newTextureWithDescriptor:texture];
    };
    auto fill_texture = [&](id<MTLTexture> texture, const void *pixel,
                            size_t size) {
      std::vector<unsigned char> data(texture.width * texture.height * size);
      for (size_t i = 0; i < data.size(); i += size)
        memcpy(data.data() + i, pixel, size);
      [texture
          replaceRegion:MTLRegionMake2D(0, 0, texture.width, texture.height)
            mipmapLevel:0
              withBytes:data.data()
            bytesPerRow:texture.width * size];
    };
    scaler.colorTexture = make_texture(descriptor.colorTextureFormat, width,
                                       height, scaler.colorTextureUsage);
    scaler.outputTexture =
        make_texture(descriptor.outputTextureFormat, output_width,
                     output_height, scaler.outputTextureUsage, true);
    scaler.depthTexture = make_texture(descriptor.depthTextureFormat, width,
                                       height, scaler.depthTextureUsage);
    scaler.motionTexture = make_texture(descriptor.motionTextureFormat, width,
                                        height, scaler.motionTextureUsage);
    scaler.diffuseAlbedoTexture =
        make_texture(descriptor.diffuseAlbedoTextureFormat, width, height,
                     scaler.diffuseAlbedoTextureUsage);
    scaler.specularAlbedoTexture =
        make_texture(descriptor.specularAlbedoTextureFormat, width, height,
                     scaler.specularAlbedoTextureUsage);
    scaler.normalTexture = make_texture(descriptor.normalTextureFormat, width,
                                        height, scaler.normalTextureUsage);
    scaler.roughnessTexture =
        make_texture(descriptor.roughnessTextureFormat, width, height,
                     scaler.roughnessTextureUsage);
    scaler.exposureTexture =
        make_texture(MTLPixelFormatR32Float, 1, 1, MTLTextureUsageShaderRead);
    unsigned char color[] = {64, 128, 192, 255};
    float depth = .5f, exposure = 1;
    uint16_t motion[2] = {}, white[4] = {0x3c00, 0x3c00, 0x3c00, 0x3c00},
             black[4] = {}, normal[4] = {0, 0, 0x3c00, 0x3800}, rough = 0x3800;
    fill_texture(scaler.colorTexture, color, 4);
    fill_texture(scaler.depthTexture, &depth, 4);
    fill_texture(scaler.motionTexture, motion, 4);
    fill_texture(scaler.diffuseAlbedoTexture, white, 8);
    fill_texture(scaler.specularAlbedoTexture, black, 8);
    fill_texture(scaler.normalTexture, normal, 8);
    fill_texture(scaler.roughnessTexture, &rough, 2);
    fill_texture(scaler.exposureTexture, &exposure, 4);
    scaler.shouldResetHistory = YES;
    scaler.depthReversed = NO;
    scaler.preExposure = 1;
    scaler.motionVectorScaleX = scaler.motionVectorScaleY = 1;
    scaler.worldToViewMatrix = matrix_identity_float4x4;
    scaler.viewToClipMatrix = matrix_identity_float4x4;
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    [scaler encodeToCommandBuffer:commands];
    id<MTLBuffer> read =
        [device newBufferWithLength:output_width * output_height * 4
                            options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    [blit copyFromTexture:scaler.outputTexture
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(output_width, output_height, 1)
                        toBuffer:read
               destinationOffset:0
          destinationBytesPerRow:output_width * 4
        destinationBytesPerImage:output_width * output_height * 4];
    [blit endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    if (commands.status != MTLCommandBufferStatusCompleted) {
      NSLog(@"%@", commands.error);
      return 3;
    }
    FILE *output = fopen(argv[1], "wb");
    if (!output)
      return 4;
    const bool wrote =
        fwrite(read.contents, 1, output_width * output_height * 4, output) ==
        output_width * output_height * 4;
    const bool closed = fclose(output) == 0;
    if (!wrote || !closed)
      return 5;
    printf("passed: native MetalFX denoiser reference (%lux%lu)\n",
           output_width, output_height);

    return 0;
  }
}
