/*
 * Copyright 2026 Feifan He for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "Metal.hpp"
#include "airconv_public.h"
#include "DXBCParser/d3d12tokenizedprogramformat.hpp"
#include "d3d11_device.hpp"
#include "d3d11_pipeline.hpp"
#include "log/log.hpp"

namespace mullion {

class MTLCompiledGeometryPipelineImpl
    : public MTLCompiledGeometryPipeline {
public:
  MTLCompiledGeometryPipelineImpl(MTLD3D11Device *pDevice,
                              const MTL_GRAPHICS_PIPELINE_DESC *pDesc)
      : num_rtvs(pDesc->NumColorAttachments),
        depth_stencil_format(pDesc->DepthStencilFormat), device_(pDevice),
        pBlendState(pDesc->BlendState),
        RasterizationEnabled(pDesc->RasterizationEnabled),
        SampleCount(pDesc->SampleCount) {
    if (pDesc->RasterizationEnabled && !pDesc->PixelShader)
      num_rtvs = 0;
    uint32_t unorm_output_reg_mask = 0;
    for (unsigned i = 0; i < num_rtvs; i++) {
      rtv_formats[i] = pDesc->ColorAttachmentFormats[i];
      unorm_output_reg_mask |= (uint32_t(IsUnorm8RenderTargetFormat(
                                    pDesc->ColorAttachmentFormats[i]))
                                << i);
    }
    // the mesh stage is the geometry shader's, or for stream output alone a variant of the vertex shader, whose
    // primitives are then the topology's
    static_assert(
        WMTPrimitiveTopologyClassPoint == (int)microsoft::D3D10_SB_PRIMITIVE_POINT &&
        WMTPrimitiveTopologyClassLine == (int)microsoft::D3D10_SB_PRIMITIVE_LINE &&
        WMTPrimitiveTopologyClassTriangle == (int)microsoft::D3D10_SB_PRIMITIVE_TRIANGLE
    );
    auto owner = pDesc->GeometryShader ? pDesc->GeometryShader : pDesc->VertexShader;
    GeometryInstances = pDesc->GeometryShader ? pDesc->GeometryShader->reflection().GeometryShader.InstanceCount : 1;
    MeshInstances = pDesc->GeometryShader ? GeometryInstances : pDesc->PassThrough;
    // a geometry shader's invocations share mesh threadgroups where its pipeline rasterizes them as they are
    MeshInvocations = pDesc->GeometryShader ? SM50GeometryMeshInvocations(
                                                  pDesc->GeometryShader->reflection().GeometryShader.MeshInvocations,
                                                  false, pDesc->SOLayout != nullptr
                                              )
                                            : 1;
    RasterizationEnabled = RasterizationEnabled && (pDesc->GeometryShader || pDesc->PassThrough);
    for (bool counting : {false, true}) {
      if (counting && !pDesc->SOLayout)
        break;
      // the counting pass draws nothing
      ShaderVariantStreamOutput so{
          .layout=pDesc->SOLayout, .counting=counting, .input_primitive=pDesc->InputPrimitive, .pass_through=pDesc->PassThrough && !counting, .quad_lines=pDesc->QuadLines && !counting
      };
      VertexShader[counting] = pDesc->VertexShader->get_shader(ShaderVariantGeometryVertex{
          .input_layout_handle=pDesc->InputLayout, .geometry_shader_handle=pDesc->GeometryShader, .index_buffer_format=pDesc->IndexBufferFormat, .strip_topology=pDesc->GSStripTopology, .stream_output=so});
      GeometryShader[counting] =
          owner->get_shader(ShaderVariantGeometry{
          .vertex_shader_handle=pDesc->VertexShader, .strip_topology=pDesc->GSStripTopology, .stream_output=so, .pipeline_statistics=pDesc->PipelineStatistics, .clip_distance_varyings=pDesc->ClipDistances != 0});
    }

    if (pDesc->PixelShader) {
      PixelShader = pDesc->PixelShader->get_pixel_shader(ShaderVariantPixel{
          .sample_mask=pDesc->SampleMask, .dual_source_blending=pDesc->BlendState->IsDualSourceBlending(),
          .disable_depth_output=depth_stencil_format == WMTPixelFormatInvalid,
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
    if (statistics && (description->PixelShader || description->GeometryShader) && !description->PipelineStatistics) {
      auto desc = *description;
      desc.PipelineStatistics = true;
      MTLCompiledGeometryPipeline *pipeline;
      device_->CreateGeometryPipeline(&desc, &pipeline);
      return pipeline->GetPipeline(pPipeline);
    }
    ready_.wait(false, std::memory_order_acquire);
    *pPipeline = {.PipelineState=state_mesh_[0], .StreamOutputCount=state_mesh_[1], .GeometryInstances=GeometryInstances, .MeshInstances=MeshInstances, .MeshInvocations=MeshInvocations};
  }

  ThreadpoolWork *RunThreadpoolWork() override {

    WMT::Reference<WMT::Error> err;
    MTL_COMPILED_SHADER vs[2] = {}, gs[2] = {}, ps;

    for (int counting = 0; counting < 2 && VertexShader[counting]; counting++) {
      if (!VertexShader[counting]->GetShader(&vs[counting])) {
        return VertexShader[counting];
      }
      if (!vs[counting].Function) {
        ERR("Failed to create mesh PSO: Invalid vertex shader.");
        return this;
      }
      if (!GeometryShader[counting]->GetShader(&gs[counting])) {
        return GeometryShader[counting];
      }
      if (!gs[counting].Function) {
        ERR("Failed to create mesh PSO: Invalid geometry shader.");
        return this;
      }
    }
    if (PixelShader) {
      if (!PixelShader->GetShader(&ps)) {
        return PixelShader;
      }
      if (!ps.Function) {
        ERR("Failed to create mesh PSO: Invalid pixel shader.");
        return this;
      }
    }

    WMTMeshRenderPipelineInfo info;
    WMT::InitializeMeshRenderPipelineInfo(info);

    info.object_function = vs[0].Function;
    info.mesh_function = gs[0].Function;
    info.payload_memory_length = SM50GeometryPayloadSize(description->VertexShader->reflection().NumOutputElement);

    info.immutable_object_buffers = (1 << 16)  | (1 << 21) | (1 << 29) | (1 << 30);
    info.immutable_mesh_buffers = (1 << 29) | (1 << 30);
    info.immutable_fragment_buffers = (1 << 29) | (1 << 30);

    if (PixelShader && RasterizationEnabled) {
      info.fragment_function = ps.Function;
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

    info.raster_sample_count = SampleCount;

    state_mesh_[0] = device_->GetMTLDevice().newRenderPipelineState(info, err);

    if (state_mesh_[0] == nullptr) {
      ERR("Failed to create mesh PSO: ", err.description().getUTF8String());
      return this;
    }
    if (vs[1].Function) {
      // stream output's counting pass draws nothing
      info.object_function = vs[1].Function;
      info.mesh_function = gs[1].Function;
      info.fragment_function = {};
      info.rasterization_enabled = false;
      state_mesh_[1] = device_->GetMTLDevice().newRenderPipelineState(info, err);
      if (state_mesh_[1] == nullptr)
        ERR("Failed to create mesh PSO: ", err.description().getUTF8String());
    }
    TraceGraphicsPipeline(state_mesh_[0].handle, *description);
    TraceGraphicsPipeline(state_mesh_[1].handle, *description);
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
  MTLD3D11Device *device_;
  std::atomic_bool ready_;
  IMTLD3D11BlendState *pBlendState;
  // the pipeline, and with stream output [1] its counting pass
  WMT::Reference<WMT::RenderPipelineState> state_mesh_[2];
  bool RasterizationEnabled;
  UINT SampleCount;
  uint32_t GeometryInstances;
  uint32_t MeshInstances;
  uint32_t MeshInvocations;

  CompiledShader *VertexShader[2] = {};
  CompiledShader *PixelShader;
  CompiledShader *GeometryShader[2] = {};
};

std::unique_ptr<MTLCompiledGeometryPipeline>
CreateGeometryPipeline(MTLD3D11Device *pDevice,
                       MTL_GRAPHICS_PIPELINE_DESC *pDesc) {
  return std::make_unique<MTLCompiledGeometryPipelineImpl>(pDevice, pDesc);
}

} // namespace mullion