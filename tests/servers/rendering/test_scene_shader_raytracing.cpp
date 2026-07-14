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

#include "servers/rendering/renderer_rd/forward_clustered/scene_shader_raytracing.h"
#include "tests/test_macros.h"

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

} // namespace TestSceneShaderRaytracing
