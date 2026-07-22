/**************************************************************************/
/*  metal_fx.cpp                                                          */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#ifdef METAL_ENABLED

#include "metal_fx.h"

#include "drivers/metal/pixel_formats.h"
#include "drivers/metal/rendering_device_driver_metal3.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

#include <MetalFX/MetalFX.hpp>

using namespace RendererRD;

#pragma mark - Spatial Scaler

MFXSpatialContext::~MFXSpatialContext() {
	if (scaler) {
		scaler->release();
	}
}

MFXSpatialEffect::MFXSpatialEffect() {
}

MFXSpatialEffect::~MFXSpatialEffect() {
}

void MFXSpatialEffect::callback(RDD *p_driver, RDD::CommandBufferID p_command_buffer, CallbackArgs *p_userdata) {
	MDCommandBufferBase *obj = (MDCommandBufferBase *)(p_command_buffer.id);
	obj->end();

	MTL::Texture *src_texture = reinterpret_cast<MTL::Texture *>(p_userdata->src.id);
	MTL::Texture *dst_texture = reinterpret_cast<MTL::Texture *>(p_userdata->dst.id);

	MTLFX::SpatialScalerBase *scaler = p_userdata->scaler;
	scaler->setColorTexture(src_texture);
	scaler->setOutputTexture(dst_texture);
	MTLFX::SpatialScaler *s = static_cast<MTLFX::SpatialScaler *>(scaler);
	MTL3::MDCommandBuffer *cmd = (MTL3::MDCommandBuffer *)(p_command_buffer.id);
	s->encodeToCommandBuffer(cmd->get_command_buffer());
	obj->retain_resource(scaler);

	CallbackArgs::free(&p_userdata);
}

void MFXSpatialEffect::ensure_context(Ref<RenderSceneBuffersRD> p_render_buffers) {
	p_render_buffers->ensure_mfx(this);
}

void MFXSpatialEffect::process(Ref<RenderSceneBuffersRD> p_render_buffers, RID p_src, RID p_dst) {
	MFXSpatialContext *ctx = p_render_buffers->get_mfx_spatial_context();
	DEV_ASSERT(ctx); // this should have been done by the caller via ensure_context

	CallbackArgs *userdata = args_allocator.alloc(
			this,
			RDD::TextureID(RD::get_singleton()->get_driver_resource(RDC::DRIVER_RESOURCE_TEXTURE, p_src)),
			RDD::TextureID(RD::get_singleton()->get_driver_resource(RDC::DRIVER_RESOURCE_TEXTURE, p_dst)),
			*ctx);
	RD::CallbackResource res[2] = {
		{ .rid = p_src, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ .rid = p_dst, .usage = RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE }
	};
	RD::get_singleton()->driver_callback_add((RDD::DriverCallback)MFXSpatialEffect::callback, userdata, VectorView<RD::CallbackResource>(res, 2));
}

MFXSpatialContext *MFXSpatialEffect::create_context(CreateParams p_params) const {
	DEV_ASSERT(RD::get_singleton()->has_feature(RD::SUPPORTS_METALFX_SPATIAL));

	RenderingDeviceDriverMetal *rdd = (RenderingDeviceDriverMetal *)RD::get_singleton()->get_device_driver();
	PixelFormats &pf = rdd->get_pixel_formats();
	MTL::Device *dev = rdd->get_device();

	NS::SharedPtr<MTLFX::SpatialScalerDescriptor> desc = NS::TransferPtr(MTLFX::SpatialScalerDescriptor::alloc()->init());
	desc->setInputWidth((NS::UInteger)p_params.input_size.width);
	desc->setInputHeight((NS::UInteger)p_params.input_size.height);

	desc->setOutputWidth((NS::UInteger)p_params.output_size.width);
	desc->setOutputHeight((NS::UInteger)p_params.output_size.height);

	desc->setColorTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.input_format));
	desc->setOutputTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.output_format));
	desc->setColorProcessingMode(MTLFX::SpatialScalerColorProcessingModeLinear);

	MFXSpatialContext *context = memnew(MFXSpatialContext);
	context->scaler = desc->newSpatialScaler(dev);

	return context;
}

#ifdef METAL_MFXTEMPORAL_ENABLED

#pragma mark - Temporal Scaler

MFXTemporalContext::~MFXTemporalContext() {
	if (scaler) {
		scaler->release();
	}
}

MFXTemporalEffect::MFXTemporalEffect() {
	Vector<String> modes;
	modes.push_back("");
	exposure_shader.initialize(modes);
	exposure_shader_version = exposure_shader.version_create();
	exposure_pipeline = RD::get_singleton()->compute_pipeline_create(exposure_shader.version_get_shader(exposure_shader_version, 0));
}

MFXTemporalEffect::~MFXTemporalEffect() {
	exposure_shader.version_free(exposure_shader_version);
}

RID MFXTemporalEffect::prepare_exposure(Ref<RenderSceneBuffersRD> p_render_buffers, RID p_adapted_luminance, float p_exposure_numerator) {
	ERR_FAIL_COND_V(p_render_buffers.is_null(), RID());
	ERR_FAIL_COND_V(p_adapted_luminance.is_null(), RID());

	RID exposure = p_render_buffers->create_texture(
			SNAME("MetalFX"),
			SNAME("exposure"),
			RD::DATA_FORMAT_R16_SFLOAT,
			RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT,
			RD::TEXTURE_SAMPLES_1,
			Size2i(1, 1),
			1);

	RID shader = exposure_shader.version_get_shader(exposure_shader_version, 0);
	ERR_FAIL_COND_V(shader.is_null(), RID());

	RD::Uniform u_luminance(RD::UNIFORM_TYPE_TEXTURE, 0, p_adapted_luminance);
	RD::Uniform u_exposure(RD::UNIFORM_TYPE_IMAGE, 1, exposure);
	RID uniform_set = UniformSetCacheRD::get_singleton()->get_cache(shader, 0, u_luminance, u_exposure);

	ExposurePushConstant push_constant;
	push_constant.exposure_numerator = p_exposure_numerator;

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, exposure_pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(push_constant));
	RD::get_singleton()->compute_list_dispatch(compute_list, 1, 1, 1);
	RD::get_singleton()->compute_list_end();

	return exposure;
}

MFXTemporalContext *MFXTemporalEffect::create_context(CreateParams p_params) const {
	DEV_ASSERT(RD::get_singleton()->has_feature(RD::SUPPORTS_METALFX_TEMPORAL));

	RenderingDeviceDriverMetal *rdd = (RenderingDeviceDriverMetal *)RD::get_singleton()->get_device_driver();
	PixelFormats &pf = rdd->get_pixel_formats();
	MTL::Device *dev = rdd->get_device();

	NS::SharedPtr<MTLFX::TemporalScalerDescriptor> desc = NS::TransferPtr(MTLFX::TemporalScalerDescriptor::alloc()->init());
	desc->setInputWidth((NS::UInteger)p_params.input_size.width);
	desc->setInputHeight((NS::UInteger)p_params.input_size.height);

	desc->setOutputWidth((NS::UInteger)p_params.output_size.width);
	desc->setOutputHeight((NS::UInteger)p_params.output_size.height);

	desc->setColorTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.input_format));
	desc->setDepthTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.depth_format));
	desc->setMotionTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.motion_format));
	desc->setAutoExposureEnabled(false);

	desc->setOutputTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.output_format));

	MFXTemporalContext *context = memnew(MFXTemporalContext);
	context->scaler = desc->newTemporalScaler(dev);
	context->scaler->setMotionVectorScaleX(p_params.motion_vector_scale.x);
	context->scaler->setMotionVectorScaleY(p_params.motion_vector_scale.y);
	context->scaler->setDepthReversed(true); // Godot uses reverse Z per https://github.com/godotengine/godot/pull/88328

	return context;
}

void MFXTemporalEffect::process(RendererRD::MFXTemporalContext *p_ctx, RendererRD::MFXTemporalEffect::Params p_params) {
	ERR_FAIL_NULL(p_ctx);
	ERR_FAIL_NULL(p_ctx->scaler);

	CallbackArgs *userdata = args_allocator.alloc(
			this,
			RDD::TextureID(RD::get_singleton()->get_driver_resource(RDC::DRIVER_RESOURCE_TEXTURE, p_params.src)),
			RDD::TextureID(RD::get_singleton()->get_driver_resource(RDC::DRIVER_RESOURCE_TEXTURE, p_params.depth)),
			RDD::TextureID(RD::get_singleton()->get_driver_resource(RDC::DRIVER_RESOURCE_TEXTURE, p_params.motion)),
			p_params.exposure.is_valid() ? RDD::TextureID(RD::get_singleton()->get_driver_resource(RDC::DRIVER_RESOURCE_TEXTURE, p_params.exposure)) : RDD::TextureID(),
			p_params.jitter_offset,
			RDD::TextureID(RD::get_singleton()->get_driver_resource(RDC::DRIVER_RESOURCE_TEXTURE, p_params.dst)),
			*p_ctx,
			p_params.reset);
	RD::CallbackResource res[5] = {
		{ .rid = p_params.src, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ .rid = p_params.depth, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ .rid = p_params.motion, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ .rid = p_params.dst, .usage = RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE },
	};
	uint32_t resource_count = 4;
	if (p_params.exposure.is_valid()) {
		res[resource_count++] = { .rid = p_params.exposure, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE };
	}
	RD::get_singleton()->driver_callback_add((RDD::DriverCallback)MFXTemporalEffect::callback, userdata, VectorView<RD::CallbackResource>(res, resource_count));
}

void MFXTemporalEffect::callback(RDD *p_driver, RDD::CommandBufferID p_command_buffer, CallbackArgs *p_userdata) {
	MDCommandBufferBase *obj = (MDCommandBufferBase *)(p_command_buffer.id);
	obj->end();

	MTL::Texture *src_texture = reinterpret_cast<MTL::Texture *>(p_userdata->src.id);
	MTL::Texture *depth = reinterpret_cast<MTL::Texture *>(p_userdata->depth.id);
	MTL::Texture *motion = reinterpret_cast<MTL::Texture *>(p_userdata->motion.id);
	MTL::Texture *exposure = reinterpret_cast<MTL::Texture *>(p_userdata->exposure.id);

	MTL::Texture *dst_texture = reinterpret_cast<MTL::Texture *>(p_userdata->dst.id);

	MTLFX::TemporalScalerBase *scaler = p_userdata->scaler;
	scaler->setReset(p_userdata->reset);
	scaler->setColorTexture(src_texture);
	scaler->setDepthTexture(depth);
	scaler->setMotionTexture(motion);
	scaler->setExposureTexture(exposure);
	scaler->setJitterOffsetX(p_userdata->jitter_offset.x);
	scaler->setJitterOffsetY(p_userdata->jitter_offset.y);
	scaler->setOutputTexture(dst_texture);
	MTLFX::TemporalScaler *s = static_cast<MTLFX::TemporalScaler *>(scaler);
	MTL3::MDCommandBuffer *cmd = (MTL3::MDCommandBuffer *)(p_command_buffer.id);
	s->encodeToCommandBuffer(cmd->get_command_buffer());
	obj->retain_resource(scaler);

	CallbackArgs::free(&p_userdata);
}

#endif

#ifdef METAL_MFXDENOISED_ENABLED

#pragma mark - Temporal Denoised Scaler

static simd::float4x4 _projection_to_simd(const Projection &p_projection) {
	return simd::float4x4(
			(simd::float4){ p_projection.columns[0].x, p_projection.columns[0].y, p_projection.columns[0].z, p_projection.columns[0].w },
			(simd::float4){ p_projection.columns[1].x, p_projection.columns[1].y, p_projection.columns[1].z, p_projection.columns[1].w },
			(simd::float4){ p_projection.columns[2].x, p_projection.columns[2].y, p_projection.columns[2].z, p_projection.columns[2].w },
			(simd::float4){ p_projection.columns[3].x, p_projection.columns[3].y, p_projection.columns[3].z, p_projection.columns[3].w });
}

MFXDenoisedContext::~MFXDenoisedContext() {
	if (scaler) {
		scaler->release();
	}
}

MFXDenoisedEffect::MFXDenoisedEffect() {}
MFXDenoisedEffect::~MFXDenoisedEffect() {}

MFXDenoisedContext *MFXDenoisedEffect::create_context(CreateParams p_params) const {
	DEV_ASSERT(RD::get_singleton()->has_feature(RD::SUPPORTS_METALFX_DENOISED));

	RenderingDeviceDriverMetal *rdd = (RenderingDeviceDriverMetal *)RD::get_singleton()->get_device_driver();
	PixelFormats &pf = rdd->get_pixel_formats();
	MTL::Device *dev = rdd->get_device();

	NS::SharedPtr<MTLFX::TemporalDenoisedScalerDescriptor> desc = NS::TransferPtr(MTLFX::TemporalDenoisedScalerDescriptor::alloc()->init());
	desc->setInputWidth((NS::UInteger)p_params.input_size.width);
	desc->setInputHeight((NS::UInteger)p_params.input_size.height);
	desc->setOutputWidth((NS::UInteger)p_params.output_size.width);
	desc->setOutputHeight((NS::UInteger)p_params.output_size.height);

	desc->setColorTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.input_format));
	desc->setDepthTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.depth_format));
	desc->setMotionTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.motion_format));
	desc->setDiffuseAlbedoTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.diffuse_albedo_format));
	desc->setSpecularAlbedoTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.specular_albedo_format));
	desc->setNormalTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.normal_format));
	desc->setRoughnessTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.roughness_format));
	desc->setSpecularHitDistanceTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.specular_hit_distance_format));
	desc->setSpecularHitDistanceTextureEnabled(true);
	desc->setDenoiseStrengthMaskTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.denoise_strength_format));
	desc->setDenoiseStrengthMaskTextureEnabled(true);
	desc->setTransparencyOverlayTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.transparency_overlay_format));
	desc->setTransparencyOverlayTextureEnabled(true);
	desc->setAutoExposureEnabled(false);
	desc->setOutputTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.output_format));

	MFXDenoisedContext *context = memnew(MFXDenoisedContext);
	context->scaler = desc->newTemporalDenoisedScaler(dev);
	if (!context->scaler) {
		memdelete(context);
		return nullptr;
	}

	context->scaler->setMotionVectorScaleX(p_params.motion_vector_scale.x);
	context->scaler->setMotionVectorScaleY(p_params.motion_vector_scale.y);
	context->scaler->setDepthReversed(true);
	return context;
}

void MFXDenoisedEffect::process(MFXDenoisedContext *p_ctx, Params p_params) {
	ERR_FAIL_NULL(p_ctx);
	ERR_FAIL_NULL(p_ctx->scaler);

	auto texture_id = [](RID p_rid) {
		return RDD::TextureID(RD::get_singleton()->get_driver_resource(RDC::DRIVER_RESOURCE_TEXTURE, p_rid));
	};

	CallbackArgs *userdata = args_allocator.alloc(
			this,
			p_ctx->scaler,
			texture_id(p_params.src),
			texture_id(p_params.depth),
			texture_id(p_params.motion),
			p_params.exposure.is_valid() ? texture_id(p_params.exposure) : RDD::TextureID(),
			texture_id(p_params.diffuse_albedo),
			texture_id(p_params.specular_albedo),
			texture_id(p_params.normal),
			texture_id(p_params.roughness),
			texture_id(p_params.specular_hit_distance),
			texture_id(p_params.denoise_strength),
			texture_id(p_params.transparency_overlay),
			texture_id(p_params.dst),
			p_params.jitter_offset,
			p_params.camera_projection,
			p_params.camera_transform,
			p_params.reset);

	RD::CallbackResource resources[12] = {
		{ .rid = p_params.src, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ .rid = p_params.depth, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ .rid = p_params.motion, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ .rid = p_params.diffuse_albedo, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ .rid = p_params.specular_albedo, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ .rid = p_params.normal, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ .rid = p_params.roughness, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ .rid = p_params.specular_hit_distance, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ .rid = p_params.denoise_strength, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ .rid = p_params.transparency_overlay, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ .rid = p_params.dst, .usage = RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE },
	};
	uint32_t resource_count = 11;
	if (p_params.exposure.is_valid()) {
		resources[resource_count++] = { .rid = p_params.exposure, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE };
	}
	RD::get_singleton()->driver_callback_add((RDD::DriverCallback)MFXDenoisedEffect::callback, userdata, VectorView<RD::CallbackResource>(resources, resource_count));
}

void MFXDenoisedEffect::callback(RDD *p_driver, RDD::CommandBufferID p_command_buffer, CallbackArgs *p_userdata) {
	MDCommandBufferBase *obj = (MDCommandBufferBase *)(p_command_buffer.id);
	obj->end();

	MTLFX::TemporalDenoisedScalerBase *scaler = p_userdata->scaler;
	scaler->setShouldResetHistory(p_userdata->reset);
	scaler->setColorTexture(reinterpret_cast<MTL::Texture *>(p_userdata->src.id));
	scaler->setDepthTexture(reinterpret_cast<MTL::Texture *>(p_userdata->depth.id));
	scaler->setMotionTexture(reinterpret_cast<MTL::Texture *>(p_userdata->motion.id));
	scaler->setExposureTexture(reinterpret_cast<MTL::Texture *>(p_userdata->exposure.id));
	scaler->setDiffuseAlbedoTexture(reinterpret_cast<MTL::Texture *>(p_userdata->diffuse_albedo.id));
	scaler->setSpecularAlbedoTexture(reinterpret_cast<MTL::Texture *>(p_userdata->specular_albedo.id));
	scaler->setNormalTexture(reinterpret_cast<MTL::Texture *>(p_userdata->normal.id));
	scaler->setRoughnessTexture(reinterpret_cast<MTL::Texture *>(p_userdata->roughness.id));
	scaler->setSpecularHitDistanceTexture(reinterpret_cast<MTL::Texture *>(p_userdata->specular_hit_distance.id));
	scaler->setDenoiseStrengthMaskTexture(reinterpret_cast<MTL::Texture *>(p_userdata->denoise_strength.id));
	scaler->setTransparencyOverlayTexture(reinterpret_cast<MTL::Texture *>(p_userdata->transparency_overlay.id));
	scaler->setOutputTexture(reinterpret_cast<MTL::Texture *>(p_userdata->dst.id));
	scaler->setJitterOffsetX(p_userdata->jitter_offset.x);
	scaler->setJitterOffsetY(p_userdata->jitter_offset.y);
	scaler->setWorldToViewMatrix(_projection_to_simd(Projection(p_userdata->camera_transform.affine_inverse())));
	scaler->setViewToClipMatrix(_projection_to_simd(p_userdata->camera_projection));

	MTLFX::TemporalDenoisedScaler *denoised_scaler = static_cast<MTLFX::TemporalDenoisedScaler *>(scaler);
	MTL3::MDCommandBuffer *cmd = (MTL3::MDCommandBuffer *)(p_command_buffer.id);
	denoised_scaler->encodeToCommandBuffer(cmd->get_command_buffer());
	obj->retain_resource(scaler);

	CallbackArgs::free(&p_userdata);
}

#endif

#endif
