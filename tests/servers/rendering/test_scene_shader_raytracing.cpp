/**************************************************************************/
/*  test_scene_shader_raytracing.cpp                                      */
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
/* "Software"), to deal in the Software without restriction, including  */
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

#include "servers/rendering/renderer_rd/forward_clustered/render_raytracing.h"
#include "servers/rendering/renderer_rd/forward_clustered/scene_shader_raytracing.h"
#include "tests/test_macros.h"

#include <cstddef>

TEST_FORCE_LINK(test_scene_shader_raytracing)

namespace TestSceneShaderRaytracing {

using SceneShader = RendererSceneRenderImplementation::SceneShaderRaytracing;

TEST_CASE("[MetalRT] C13 selects the editor route only when its scene shader is ready") {
	CHECK(SceneShader::select_scene_route(true, false, false) == SceneShader::SceneRoute::RAYTRACING_PIPELINE);
	CHECK(SceneShader::select_scene_route(true, true, false) == SceneShader::SceneRoute::RAYTRACING_PIPELINE);
	CHECK(SceneShader::select_scene_route(false, true, true) == SceneShader::SceneRoute::COMPUTE_RAY_QUERY);
	CHECK(SceneShader::select_scene_route(false, true, false) == SceneShader::SceneRoute::UNAVAILABLE);
	CHECK(SceneShader::select_scene_route(false, false, true) == SceneShader::SceneRoute::UNAVAILABLE);
}

TEST_CASE("[MetalRT] C13 masks unsupported Metal scene variants") {
	uint32_t flags = SceneShader::rt_flags_pack(
			SceneShader::RT_FLAG_DEBUG_VIS_ENABLED |
					SceneShader::RT_FLAG_DLSS_RR_ENABLED |
					SceneShader::RT_FLAG_FOG_ENABLED |
					SceneShader::RT_FLAG_SER_ENABLED,
			4, 3);
	uint32_t sanitized = SceneShader::sanitize_compute_rt_flags(flags);
	CHECK((sanitized & SceneShader::RT_FLAG_DEBUG_VIS_ENABLED) == 0);
	CHECK((sanitized & SceneShader::RT_FLAG_DLSS_RR_ENABLED) == 0);
	CHECK((sanitized & SceneShader::RT_FLAG_FOG_ENABLED) == 0);
	CHECK((sanitized & SceneShader::RT_FLAG_SER_ENABLED) == 0);
	CHECK(((sanitized >> SceneShader::RT_SAMPLE_COUNT_SHIFT) & SceneShader::RT_SAMPLE_COUNT_MASK) == 4);
	CHECK(((sanitized >> SceneShader::RT_MAX_BOUNCES_SHIFT) & SceneShader::RT_MAX_BOUNCES_MASK) == 2);
}

TEST_CASE("[MetalRT] C14 preserves front-face winding across mirrored transforms") {
	using namespace RendererSceneRenderImplementation;
	const uint32_t flip = RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT;
	const uint32_t opaque = RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT;

	Transform3D regular;
	CHECK(rt_instance_flags_apply_transform_winding(flip | opaque, regular) == (flip | opaque));

	Transform3D mirrored;
	mirrored.basis.scale(Vector3(-1.0, 1.0, 1.0));
	CHECK(rt_instance_flags_apply_transform_winding(flip | opaque, mirrored) == opaque);
	CHECK(rt_instance_flags_apply_transform_winding(opaque, mirrored) == (flip | opaque));
}

TEST_CASE("[MetalRT] C15 material ABI carries dispatch, alpha, identity, and custom uniforms") {
	using namespace RendererSceneRenderImplementation;
	CHECK(sizeof(RT_MaterialData) == 112);
	CHECK(offsetof(RT_MaterialData, uniform_address) == 88);
	CHECK(offsetof(RT_MaterialData, alpha_scissor_threshold) == 96);
	CHECK(offsetof(RT_MaterialData, dispatch_index) == 100);
	CHECK(offsetof(RT_MaterialData, material_id) == 104);
	CHECK((RT_MAT_FLAG_ALPHA_SCISSOR & RT_MAT_FLAG_CUSTOM_SHADER) == 0);

	RT_MaterialData material = {};
	material.uniform_address = 0x1020304050607080ULL;
	material.alpha_scissor_threshold = 0.47f;
	material.dispatch_index = 19;
	material.material_id = 73;
	material.flags = RT_MAT_FLAG_ALPHA_SCISSOR | RT_MAT_FLAG_CUSTOM_SHADER;
	CHECK(material.uniform_address == 0x1020304050607080ULL);
	CHECK(material.alpha_scissor_threshold == doctest::Approx(0.47f));
	CHECK(material.dispatch_index == 19);
	CHECK(material.material_id == 73);
}

TEST_CASE("[MetalRT] C15 material cache key invalidates on RID, parameters, and shader generation") {
	using namespace RendererSceneRenderImplementation;
	CHECK(rt_material_cache_needs_refresh(false, 7, 11, 7, 11));
	CHECK_FALSE(rt_material_cache_needs_refresh(true, 7, 11, 7, 11));
	CHECK(rt_material_cache_needs_refresh(true, 7, 11, 8, 11));
	CHECK(rt_material_cache_needs_refresh(true, 7, 11, 7, 12));

	uint32_t requested_flags = SceneShader::rt_flags_pack(
			SceneShader::RT_FLAG_DEBUG_VIS_ENABLED | SceneShader::RT_FLAG_FOG_ENABLED, 4, 2);
	SceneShader::ComputeMaterialVariantKey first = SceneShader::make_compute_material_variant_key(requested_flags, 3);
	SceneShader::ComputeMaterialVariantKey cached = SceneShader::make_compute_material_variant_key(
			SceneShader::rt_flags_pack(SceneShader::RT_FLAG_NONE, 4, 2), 3);
	SceneShader::ComputeMaterialVariantKey reloaded = SceneShader::make_compute_material_variant_key(requested_flags, 4);
	CHECK(first == cached);
	CHECK_FALSE(first == reloaded);
}

TEST_CASE("[MetalRT] C15 custom uniform and bindless texture writes stay within their BDA record") {
	using namespace RendererSceneRenderImplementation;
	CHECK(rt_material_buffer_write_fits(0, 16, 32));
	CHECK(rt_material_buffer_write_fits(28, 4, 32));
	CHECK_FALSE(rt_material_buffer_write_fits(29, 4, 32));
	CHECK_FALSE(rt_material_buffer_write_fits(UINT32_MAX, 4, 32));
}

} // namespace TestSceneShaderRaytracing
