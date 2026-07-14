/**************************************************************************/
/*  metal_rt_availability.cpp                                             */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
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

#include "metal_rt_availability.h"

MetalRTGateResult metal_rt_evaluate_gate(const MetalRTGateInputs &p_inputs) {
	MetalRTGateResult result;

	// An explicit opt-out takes precedence over capability diagnostics. This
	// keeps the forced fallback log stable on both supported and unsupported
	// machines.
	if (p_inputs.force_disabled) {
		result.blockers = METAL_RT_BLOCKER_FORCED_DISABLED;
		return result;
	}
	if (!p_inputs.project_enabled) {
		result.blockers = METAL_RT_BLOCKER_PROJECT_DISABLED;
		return result;
	}

	if (!p_inputs.supports_raytracing) {
		result.blockers |= METAL_RT_BLOCKER_RAYTRACING;
	}
	if (!p_inputs.supports_function_pointers) {
		result.blockers |= METAL_RT_BLOCKER_FUNCTION_POINTERS;
	}
	if (!p_inputs.supports_user_id_instances) {
		result.blockers |= METAL_RT_BLOCKER_USER_ID_INSTANCES;
	}
	if (!p_inputs.supports_gpu_address) {
		result.blockers |= METAL_RT_BLOCKER_GPU_ADDRESS;
	}
	if (!p_inputs.argument_buffers_enabled) {
		result.blockers |= METAL_RT_BLOCKER_ARGUMENT_BUFFERS;
	}
	if (!p_inputs.supports_msl_2_3) {
		result.blockers |= METAL_RT_BLOCKER_MSL_2_3;
	}
	if (!p_inputs.supported_platform) {
		result.blockers |= METAL_RT_BLOCKER_UNSUPPORTED_PLATFORM;
	}

	return result;
}

String MetalRTGateResult::get_reason_codes() const {
	Vector<String> reasons;
	if (blockers & METAL_RT_BLOCKER_PROJECT_DISABLED) {
		reasons.push_back("project_disabled");
	}
	if (blockers & METAL_RT_BLOCKER_FORCED_DISABLED) {
		reasons.push_back("forced_disabled");
	}
	if (blockers & METAL_RT_BLOCKER_RAYTRACING) {
		reasons.push_back("missing_raytracing");
	}
	if (blockers & METAL_RT_BLOCKER_FUNCTION_POINTERS) {
		reasons.push_back("missing_function_pointers");
	}
	if (blockers & METAL_RT_BLOCKER_USER_ID_INSTANCES) {
		reasons.push_back("missing_user_id_instances");
	}
	if (blockers & METAL_RT_BLOCKER_GPU_ADDRESS) {
		reasons.push_back("missing_gpu_address");
	}
	if (blockers & METAL_RT_BLOCKER_ARGUMENT_BUFFERS) {
		reasons.push_back("missing_argument_buffers");
	}
	if (blockers & METAL_RT_BLOCKER_MSL_2_3) {
		reasons.push_back("missing_msl_2_3");
	}
	if (blockers & METAL_RT_BLOCKER_UNSUPPORTED_PLATFORM) {
		reasons.push_back("unsupported_platform");
	}
	return String(",").join(reasons);
}

String MetalRTGateResult::get_description() const {
	Vector<String> reasons;
	if (blockers & METAL_RT_BLOCKER_PROJECT_DISABLED) {
		reasons.push_back("the project setting rendering/pathtracer/metal_ray_query_backend is false");
	}
	if (blockers & METAL_RT_BLOCKER_FORCED_DISABLED) {
		reasons.push_back("GODOT_MTL_DISABLE_RAYTRACING=1");
	}
	if (blockers & METAL_RT_BLOCKER_RAYTRACING) {
		reasons.push_back("the device does not report supportsRaytracing");
	}
	if (blockers & METAL_RT_BLOCKER_FUNCTION_POINTERS) {
		reasons.push_back("the device does not report supportsFunctionPointers");
	}
	if (blockers & METAL_RT_BLOCKER_USER_ID_INSTANCES) {
		reasons.push_back("shader-visible UserID instance descriptors are unavailable");
	}
	if (blockers & METAL_RT_BLOCKER_GPU_ADDRESS) {
		reasons.push_back("GPU buffer addresses are unavailable");
	}
	if (blockers & METAL_RT_BLOCKER_ARGUMENT_BUFFERS) {
		reasons.push_back("encoder-free tier-2 argument buffers are unavailable or disabled");
	}
	if (blockers & METAL_RT_BLOCKER_MSL_2_3) {
		reasons.push_back("the selected Metal Shading Language target is older than 2.3");
	}
	if (blockers & METAL_RT_BLOCKER_UNSUPPORTED_PLATFORM) {
		reasons.push_back("the native Metal RT path is currently supported on macOS arm64 only");
	}
	return String("; ").join(reasons);
}
