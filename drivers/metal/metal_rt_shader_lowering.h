/**************************************************************************/
/*  metal_rt_shader_lowering.h                                            */
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

#pragma once

#include "core/string/ustring.h"
#include "core/templates/vector.h"
#include "servers/rendering/rendering_device_commons.h"

// Chunk C7 shader-lowering lane for ray-tracing SPIR-V.
//
// This is the probe half of the shader strategy documented in
// docs/rt_metal_port/shader_strategy.md: it runs a single SPIR-V stage
// through the same SPIRV-Cross CompilerMSL configuration used by
// RenderingShaderContainerMetal and reports the outcome instead of failing
// the container. It supports every RenderingDeviceCommons::ShaderStage,
// including the ray-tracing pipeline stages the container does not consume
// yet, so tests and future backend code can measure exactly which stages the
// SPIRV-Cross lane can lower.

class MetalRTShaderLowering {
public:
	struct Result {
		bool ok = false;
		String error;
		String entry_point;
		String msl_source;
	};

	// Lowers one SPIR-V stage to MSL.
	//
	// Every reflected resource is identity-mapped to the MSL index equal to
	// its SPIR-V binding, so callers can bind resources deterministically
	// without the full container reflection pipeline. Acceleration structures
	// count as buffer bindings.
	static Result lower_spirv(RenderingDeviceCommons::ShaderStage p_stage, const Vector<uint8_t> &p_spirv,
			uint32_t p_msl_major, uint32_t p_msl_minor, bool p_argument_buffers, bool p_pad_argument_buffer_resources = true);
};
