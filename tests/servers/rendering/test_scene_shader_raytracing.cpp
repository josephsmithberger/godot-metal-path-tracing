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

#include "servers/rendering/renderer_rd/forward_clustered/pathtracing_presentation.h"
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
					SceneShader::RT_FLAG_DENOISER_GUIDES_ENABLED |
					SceneShader::RT_FLAG_FOG_ENABLED |
					SceneShader::RT_FLAG_SER_ENABLED,
			4, 3);
	uint32_t sanitized = SceneShader::sanitize_compute_rt_flags(flags);
	CHECK((sanitized & SceneShader::RT_FLAG_DEBUG_VIS_ENABLED) == 0);
	CHECK((sanitized & SceneShader::RT_FLAG_DENOISER_GUIDES_ENABLED) != 0);
	CHECK((sanitized & SceneShader::RT_FLAG_FOG_ENABLED) == 0);
	CHECK((sanitized & SceneShader::RT_FLAG_SER_ENABLED) == 0);
	CHECK(((sanitized >> SceneShader::RT_SAMPLE_COUNT_SHIFT) & SceneShader::RT_SAMPLE_COUNT_MASK) == 4);
	CHECK(((sanitized >> SceneShader::RT_MAX_BOUNCES_SHIFT) & SceneShader::RT_MAX_BOUNCES_MASK) == 2);
}

TEST_CASE("[MetalRT] C17 resets temporal presentation history on discontinuities") {
	using namespace RendererSceneRenderImplementation;
	PathtracingPresentationHistory history;
	const Transform3D camera;
	const Projection projection = Projection::create_perspective(70.0, 1.0, 0.05, 100.0);

	uint32_t reasons = history.begin_frame(10, 1.0 / 60.0, camera, camera, projection, projection);
	CHECK((reasons & PT_PRESENTATION_HISTORY_RESET_CONTEXT) != 0);
	CHECK((reasons & ~PT_PRESENTATION_HISTORY_RESET_CONTEXT) == 0);

	reasons = history.begin_frame(11, 1.0 / 60.0, camera, camera, projection, projection);
	CHECK(reasons == PT_PRESENTATION_HISTORY_RESET_NONE);

	reasons = history.begin_frame(14, 1.0 / 60.0, camera, camera, projection, projection);
	CHECK((reasons & PT_PRESENTATION_HISTORY_RESET_FRAME_GAP) != 0);

	Transform3D cut_camera;
	cut_camera.origin = Vector3(10.0, 0.0, 0.0);
	reasons = history.begin_frame(15, 1.0 / 60.0, cut_camera, camera, projection, projection);
	CHECK((reasons & PT_PRESENTATION_HISTORY_RESET_CAMERA_CUT) != 0);

	const Projection cut_projection = Projection::create_perspective(50.0, 1.0, 0.05, 100.0);
	reasons = history.begin_frame(16, 1.0 / 60.0, cut_camera, cut_camera, cut_projection, projection);
	CHECK((reasons & PT_PRESENTATION_HISTORY_RESET_PROJECTION_CUT) != 0);

	reasons = history.begin_frame(17, 0.5, cut_camera, cut_camera, cut_projection, cut_projection);
	CHECK((reasons & PT_PRESENTATION_HISTORY_RESET_LONG_FRAME) != 0);
}

TEST_CASE("[MetalRT] C17 keeps NVIDIA SER disabled on the Metal compute lane") {
	const uint32_t requested = SceneShader::rt_flags_pack(SceneShader::RT_FLAG_SER_ENABLED, 1, 1);
	const uint32_t sanitized = SceneShader::sanitize_compute_rt_flags(requested);
	CHECK((sanitized & SceneShader::RT_FLAG_SER_ENABLED) == 0);
}

TEST_CASE("[MetalRT] MetalFX denoising enables path-tracing guide output") {
	float params[16] = {};
	params[RSE::PT_PARAM_SAMPLE_COUNT] = 1.0f;
	params[RSE::PT_PARAM_MAX_BOUNCES] = 1.0f;
	params[RSE::PT_PARAM_DENOISER] = (float)RSE::PT_DENOISER_METALFX;

	uint32_t flags = SceneShader::compute_rt_flags(params, false);
	CHECK((flags & SceneShader::RT_FLAG_DENOISER_GUIDES_ENABLED) != 0);
	CHECK((SceneShader::sanitize_compute_rt_flags(flags) & SceneShader::RT_FLAG_DENOISER_GUIDES_ENABLED) != 0);

	params[RSE::PT_PARAM_DENOISER] = (float)RSE::PT_DENOISER_NONE;
	flags = SceneShader::compute_rt_flags(params, false);
	CHECK((flags & SceneShader::RT_FLAG_DENOISER_GUIDES_ENABLED) == 0);
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

TEST_CASE("[MetalRT] P1 compute aggregate debounce waits for a quiet finalize pass") {
	SceneShader::ComputeCompileDebounce debounce;
	CHECK(debounce.is_ready(0));
	CHECK_FALSE(debounce.is_ready(1));
	// A sequential arrival restarts the quiet-pass window.
	CHECK_FALSE(debounce.is_ready(2));
	CHECK(debounce.is_ready(2));
	CHECK(debounce.is_ready(2));
	CHECK_FALSE(debounce.is_ready(3));
	CHECK(debounce.is_ready(3));
}

TEST_CASE("[MetalRT] P1 aggregate cache insertion never overwrites an owner") {
	HashMap<uint64_t, RID> cache;
	const RID first = RID::from_uint64(11);
	const RID duplicate = RID::from_uint64(22);
	REQUIRE(SceneShader::compute_cache_insert_if_absent(cache, 7, first));
	CHECK_FALSE(SceneShader::compute_cache_insert_if_absent(cache, 7, duplicate));
	REQUIRE(cache.getptr(7) != nullptr);
	CHECK(*cache.getptr(7) == first);
}

TEST_CASE("[MetalRT] P4 TLAS growth stays standard at the limit boundary") {
	using namespace RendererSceneRenderImplementation;
	constexpr uint64_t standard_limit = 1ull << 24;
	CHECK(rt_tlas_growth_capacity((1u << 23) + 1, standard_limit) == standard_limit);
	CHECK(rt_tlas_growth_capacity((uint32_t)standard_limit, standard_limit) == standard_limit);
	CHECK(rt_tlas_growth_capacity((uint32_t)standard_limit + 1, standard_limit) == standard_limit + 1);
}

TEST_CASE("[MetalRT] C15 custom uniform and bindless texture writes stay within their BDA record") {
	using namespace RendererSceneRenderImplementation;
	CHECK(rt_material_buffer_write_fits(0, 16, 32));
	CHECK(rt_material_buffer_write_fits(28, 4, 32));
	CHECK_FALSE(rt_material_buffer_write_fits(29, 4, 32));
	CHECK_FALSE(rt_material_buffer_write_fits(UINT32_MAX, 4, 32));
}

TEST_CASE("[MetalRT] C16 validates explicit and fallback procedural bounds") {
	using namespace RendererSceneRenderImplementation;
	const float valid_bounds[] = {
		-1.0f,
		-0.5f,
		-2.0f,
		1.0f,
		0.5f,
		2.0f,
		2.0f,
		1.0f,
		-1.0f,
		3.0f,
		4.0f,
		1.0f,
	};
	RTProceduralBoundsValidation validation;
	String error;
	REQUIRE(rt_procedural_bounds_validate(Span<const float>(valid_bounds), AABB(), validation, error));
	CHECK(error.is_empty());
	CHECK(validation.source == RTProceduralBoundsSource::EXPLICIT);
	CHECK(validation.count == 2);

	const AABB fallback(Vector3(-0.5, -1.0, -1.5), Vector3(1.0, 2.0, 3.0));
	REQUIRE(rt_procedural_bounds_validate(Span<const float>(), fallback, validation, error));
	CHECK(validation.source == RTProceduralBoundsSource::FALLBACK);
	CHECK(validation.count == 1);

	const float incomplete[] = { -1.0f, -1.0f, -1.0f, 1.0f, 1.0f };
	CHECK_FALSE(rt_procedural_bounds_validate(Span<const float>(incomplete), fallback, validation, error));
	CHECK(error.contains("complete"));

	const float flat[] = { -1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f };
	CHECK_FALSE(rt_procedural_bounds_validate(Span<const float>(flat), fallback, validation, error));
	CHECK(error.contains("strictly greater"));

	const float inverted[] = { 1.0f, -1.0f, -1.0f, -1.0f, 1.0f, 1.0f };
	CHECK_FALSE(rt_procedural_bounds_validate(Span<const float>(inverted), fallback, validation, error));
	CHECK(error.contains("AABB 0"));

	float non_finite[] = { -1.0f, -1.0f, -1.0f, Math::NaN, 1.0f, 1.0f };
	CHECK_FALSE(rt_procedural_bounds_validate(Span<const float>(non_finite), fallback, validation, error));
	CHECK(error.contains("non-finite"));

	CHECK_FALSE(rt_procedural_bounds_validate(Span<const float>(), AABB(Vector3(), Vector3(1.0, 0.0, 1.0)), validation, error));
	CHECK(error.contains("positive volume"));
}

TEST_CASE("[MetalRT] C16 procedural geometry ABI preserves IDs and hit attributes") {
	using namespace RendererSceneRenderImplementation;
	CHECK(sizeof(RT_GeometryData) == 128);
	CHECK(offsetof(RT_GeometryData, flags) == 68);
	CHECK(offsetof(RT_GeometryData, aabb_size_x) == 72);

	RT_GeometryData geometry = {};
	geometry.vertex_buffer_address = 0x0102030405060708ULL;
	geometry.primitive_count = 3;
	geometry.flags = RT_GEOM_FLAG_PROCEDURAL;
	CHECK(geometry.vertex_buffer_address == 0x0102030405060708ULL);
	CHECK(geometry.primitive_count == 3);
	CHECK((geometry.flags & RT_GEOM_FLAG_PROCEDURAL) != 0);
}

} // namespace TestSceneShaderRaytracing
