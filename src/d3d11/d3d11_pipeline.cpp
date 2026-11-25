#include "Metal.hpp"
#include "d3d11_private.h"
#include "d3d11_pipeline.hpp"
#include "d3d11_device.hpp"
#include "d3d11_shader.hpp"
#include "log/log.hpp"
#include "airconv_public.h"
#include <atomic>
#include <mutex>
#include <unordered_map>

namespace mullion {

WMT::Reference<WMT::Function>
GetFragmentDepthBiasFunction(WMT::Device device) {
  static std::mutex mutex;
  static std::unordered_map<uint64_t, WMT::Reference<WMT::Function>> cache;
  std::scoped_lock lock(mutex);
  auto key = device.handle;
  if (auto it = cache.find(key); it != cache.end())
    return it->second;
  std::string source =
      "#include <metal_stdlib>\nusing namespace metal;\n"
      "struct In { float4 position [[position]]; };\n"
      "struct Out { float z [[depth(any)]]; };\n"
      "fragment Out fragment_depth_bias(In in [[stage_in]], uint viewport [[viewport_array_index]], "
      "constant float *ranges [[buffer(" + std::to_string(SM50_BINDING_INDEX_DEPTH_RANGES) + ")]]) {\n"
      "  Out out;\n"
      "  constant float *bias = ranges + " + std::to_string(2 * SM50_VIEWPORTS + 4) + ";\n"
      "  float z = in.position.z;\n"
      "  float slope = max(abs(dfdx(z)), abs(dfdy(z)));\n"
      "  float offset = bias[1] != 0 ? bias[1] * slope : 0;\n"
      "  if (bias[3] != 0) offset += bias[0] / bias[3];\n"
      "  if (bias[2] > 0) offset = min(offset, bias[2]);\n"
      "  if (bias[2] < 0) offset = max(offset, bias[2]);\n"
      "  uint range = viewport < " + std::to_string(SM50_VIEWPORTS) + "u ? 2 * viewport : 0;\n"
      "  float biased = clamp(z + offset, ranges[range], ranges[range + 1]);\n"
      "  out.z = bias[3] != 0 ? precise::divide(round(biased * bias[3]), bias[3]) : biased;\n"
      "  return out;\n"
      "}\n";
  WMT::Reference<WMT::Error> err;
  auto lib = device.newLibraryWithSource(source, err);
  if (!lib) {
    ERR("Failed to compile D3D11 fragment_depth_bias shader: ", err.description().getUTF8String());
    return {};
  }
  auto fn = lib.newFunction("fragment_depth_bias");
  cache.emplace(key, fn);
  return fn;
}

class MTLCompiledGraphicsPipelineImpl
    : public MTLCompiledGraphicsPipeline {
public:
  MTLCompiledGraphicsPipelineImpl(MTLD3D11Device *pDevice,
                              MTL_GRAPHICS_PIPELINE_DESC *pDesc)
      : num_rtvs(pDesc->NumColorAttachments),
        depth_stencil_format(pDesc->DepthStencilFormat),
        topology_class(pDesc->TopologyClass), device_(pDevice),
        pBlendState(pDesc->BlendState),
        RasterizationEnabled(pDesc->RasterizationEnabled),
        FragmentDepthBias(pDesc->FragmentDepthBias),
        SampleCount(pDesc->SampleCount) {
    uint32_t unorm_output_reg_mask = 0;
    for (unsigned i = 0; i < num_rtvs; i++) {
      rtv_formats[i] = pDesc->ColorAttachmentFormats[i];
      unorm_output_reg_mask |= (uint32_t(IsUnorm8RenderTargetFormat(pDesc->ColorAttachmentFormats[i])) << i);
    }

    VertexShader = pDesc->VertexShader->get_shader(ShaderVariantVertex{
        .input_layout_handle=pDesc->InputLayout, .gs_passthrough=pDesc->GSPassthrough, .rasterization_disabled=!pDesc->RasterizationEnabled, .clip_distance_varyings=pDesc->ClipDistances != 0});

    if (pDesc->PixelShader) {
      PixelShader = pDesc->PixelShader->get_pixel_shader(ShaderVariantPixel{
          .sample_mask=pDesc->SampleMask, .dual_source_blending=pDesc->BlendState->IsDualSourceBlending(),
          .disable_depth_output=depth_stencil_format == WMTPixelFormatInvalid,
          .quantize_d24=pDesc->QuantizeD24,
          .unorm_output_reg_mask=unorm_output_reg_mask, .pipeline_statistics=pDesc->PipelineStatistics, .alpha_to_coverage=pDesc->BlendState->IsAlphaToCoverage(),
          .centered_samples=pDesc->CenteredSamples, .single_raster_sample=pDesc->SingleRasterSample, .paired_samples=pDesc->PairedSamples, .clip_distances=pDesc->ClipDistances},
          {rtv_formats, num_rtvs}, pDesc->BlendState, pDesc->PixelMinMaxFiltering);
      ps_valid_render_targets = pDesc->PixelShader->reflection().PSValidRenderTargets;
    } else {
      PixelShader = nullptr;
      ps_valid_render_targets = 0;
    }
  }

  void GetPipeline(MTL_COMPILED_GRAPHICS_PIPELINE *pPipeline, bool statistics) final {
    if (statistics && description->PixelShader && !description->PipelineStatistics) {
      auto desc = *description;
      desc.PipelineStatistics = true;
      MTLCompiledGraphicsPipeline *pipeline;
      device_->CreateGraphicsPipeline(&desc, &pipeline);
      return pipeline->GetPipeline(pPipeline);
    }
    ready_.wait(false, std::memory_order_acquire);
    *pPipeline = {.PipelineState=state_};
  }

  ThreadpoolWork *RunThreadpoolWork() override {


    WMT::Reference<WMT::Error> err;
    MTL_COMPILED_SHADER vs, ps;
    if (!VertexShader->GetShader(&vs)) {
      return VertexShader;
    }
    if (PixelShader && !PixelShader->GetShader(&ps)) {
      return PixelShader;
    }

    WMTRenderPipelineInfo info;
    WMT::InitializeRenderPipelineInfo(info);

    info.vertex_function = vs.Function;

    if (PixelShader) {
      info.fragment_function = ps.Function;
    } else if (FragmentDepthBias) {
      info.fragment_function = GetFragmentDepthBiasFunction(device_->GetMTLDevice());
    }
    info.rasterization_enabled = RasterizationEnabled;

    for (unsigned i = 0; i < num_rtvs; i++) {
      if (rtv_formats[i] == WMTPixelFormatInvalid)
        continue;
      info.colors[i].pixel_format = rtv_formats[i];
    }

    if (depth_stencil_format != WMTPixelFormatInvalid) {
      info.depth_pixel_format = depth_stencil_format;
    }
    if (DepthStencilPlanarFlags(depth_stencil_format) & 2) {
      info.stencil_pixel_format = depth_stencil_format;
    }

    if (pBlendState) {
      pBlendState->SetupMetalPipelineDescriptor((WMTRenderPipelineBlendInfo *)&info, num_rtvs, ps_valid_render_targets);
    }
    // explicit shader coverage disables A2C (D3D11.3 17.18)
    if (description->PixelShader && description->PixelShader->reflection().PixelShader.HasCoverageOutput)
      info.alpha_to_coverage_enabled = false;

    info.input_primitive_topology = topology_class;
    info.raster_sample_count = SampleCount;
    info.immutable_vertex_buffers = (1 << 16) | (1 << 29) | (1 << 30);
    info.immutable_fragment_buffers = (1 << 29) | (1 << 30);

    state_ = device_->GetMTLDevice().newRenderPipelineState(info, err);

    if (state_ == nullptr) {
      ERR("Failed to create PSO: ", err.description().getUTF8String());
      return this;
    }

    TraceGraphicsPipeline(state_.handle, *description);


    return this;
  }

  bool GetIsDone() override { return ready_; }

  void SetIsDone(bool state) override {
    ready_.store(state);
    ready_.notify_all();
  }

private:
  UINT num_rtvs;
  UINT ps_valid_render_targets;
  WMTPixelFormat rtv_formats[8];
  WMTPixelFormat depth_stencil_format;
  WMTPrimitiveTopologyClass topology_class;
  MTLD3D11Device *device_;
  std::atomic_bool ready_;
  CompiledShader *VertexShader;
  CompiledShader *PixelShader;
  IMTLD3D11BlendState *pBlendState;
  WMT::Reference<WMT::RenderPipelineState> state_;
  bool RasterizationEnabled;
  bool FragmentDepthBias;
  UINT SampleCount;
};

std::unique_ptr<MTLCompiledGraphicsPipeline>
CreateGraphicsPipeline(MTLD3D11Device *pDevice,
                       MTL_GRAPHICS_PIPELINE_DESC *pDesc) {
  return std::make_unique<MTLCompiledGraphicsPipelineImpl>(pDevice, pDesc);
}

class MTLCompiledComputePipelineImpl
    : public MTLCompiledComputePipeline {
public:
  MTLCompiledComputePipelineImpl(MTLD3D11Device *pDevice, ManagedShader shader)
      : device_(pDevice), shader_hash_(shader->sha1()) {
    ComputeShader = shader->get_shader(ShaderVariantDefault{});
    uint32_t total_tgsize = shader->reflection().ThreadgroupSize[0] *
                            shader->reflection().ThreadgroupSize[1] *
                            shader->reflection().ThreadgroupSize[2];
    // FIXME: might be different on AMD GPU, if it's ever supported
    tgsize_is_multiple_of_sgwidth = (total_tgsize % 32) == 0;
  }

  void GetPipeline(MTL_COMPILED_COMPUTE_PIPELINE *pPipeline) final {
    ready_.wait(false, std::memory_order_acquire);
    *pPipeline = {state_};
  }

  ThreadpoolWork *RunThreadpoolWork() override {


    WMT::Reference<WMT::Error> err;
    MTL_COMPILED_SHADER cs;
    if (!ComputeShader->GetShader(&cs)) {
      return ComputeShader;
    }

    WMTComputePipelineInfo info;
    WMT::InitializeComputePipelineInfo(info);

    info.compute_function = cs.Function;
    info.tgsize_is_multiple_of_sgwidth = tgsize_is_multiple_of_sgwidth;
    info.immutable_buffers = (1 << 29) | (1 << 30);

    state_ = device_->GetMTLDevice().newComputePipelineState(info, err);

    if (!state_) {
      ERR("Failed to create compute PSO: ", err.description().getUTF8String());
      return this;
    }

    TRACE_IN(Shader, "D3D11 Compute:", state_.handle, " CS=", shader_hash_.string());


    return this;
  }

  bool GetIsDone() override { return ready_; }

  void SetIsDone(bool state) override {
    ready_.store(state);
    ready_.notify_all();
  }

private:
  MTLD3D11Device *device_;
  std::atomic_bool ready_;
  CompiledShader *ComputeShader;
  Sha1Digest shader_hash_;
  WMT::Reference<WMT::ComputePipelineState> state_;
  bool tgsize_is_multiple_of_sgwidth;
};

std::unique_ptr<MTLCompiledComputePipeline>
CreateComputePipeline(MTLD3D11Device *pDevice, ManagedShader ComputeShader) {
  return std::make_unique<MTLCompiledComputePipelineImpl>(pDevice, ComputeShader);
}

} // namespace mullion