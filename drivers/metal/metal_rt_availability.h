/**************************************************************************/
/*  metal_rt_availability.h                                               */
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

#pragma once

#include "core/string/ustring.h"

enum MetalRTBlocker : uint32_t {
	METAL_RT_BLOCKER_NONE = 0,
	METAL_RT_BLOCKER_PROJECT_DISABLED = 1 << 0,
	METAL_RT_BLOCKER_FORCED_DISABLED = 1 << 1,
	METAL_RT_BLOCKER_RAYTRACING = 1 << 2,
	METAL_RT_BLOCKER_FUNCTION_POINTERS = 1 << 3,
	METAL_RT_BLOCKER_USER_ID_INSTANCES = 1 << 4,
	METAL_RT_BLOCKER_GPU_ADDRESS = 1 << 5,
	METAL_RT_BLOCKER_ARGUMENT_BUFFERS = 1 << 6,
	METAL_RT_BLOCKER_MSL_2_3 = 1 << 7,
	METAL_RT_BLOCKER_UNSUPPORTED_PLATFORM = 1 << 8,
};

struct MetalRTGateInputs {
	bool supported_platform = true;
	bool project_enabled = true;
	bool force_disabled = false;
	bool supports_raytracing = false;
	bool supports_function_pointers = false;
	bool supports_user_id_instances = false;
	bool supports_gpu_address = false;
	bool argument_buffers_enabled = false;
	bool supports_msl_2_3 = false;
};

struct MetalRTGateResult {
	uint32_t blockers = METAL_RT_BLOCKER_NONE;

	_FORCE_INLINE_ bool is_enabled() const {
		return blockers == METAL_RT_BLOCKER_NONE;
	}

	String get_reason_codes() const;
	String get_description() const;
};

MetalRTGateResult metal_rt_evaluate_gate(const MetalRTGateInputs &p_inputs);
