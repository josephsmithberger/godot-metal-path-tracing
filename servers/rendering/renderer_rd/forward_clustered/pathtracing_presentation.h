/**************************************************************************/
/*  pathtracing_presentation.h                                            */
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

#pragma once

#include "core/math/projection.h"
#include "core/math/transform_3d.h"

namespace RendererSceneRenderImplementation {

enum PathtracingPresentationHistoryReset : uint32_t {
	PT_PRESENTATION_HISTORY_RESET_NONE = 0,
	PT_PRESENTATION_HISTORY_RESET_CONTEXT = 1 << 0,
	PT_PRESENTATION_HISTORY_RESET_FRAME_GAP = 1 << 1,
	PT_PRESENTATION_HISTORY_RESET_CAMERA_CUT = 1 << 2,
	PT_PRESENTATION_HISTORY_RESET_PROJECTION_CUT = 1 << 3,
	PT_PRESENTATION_HISTORY_RESET_LONG_FRAME = 1 << 4,
};

struct PathtracingPresentationHistory {
	static constexpr real_t CAMERA_CUT_DISTANCE = 4.0;
	static constexpr real_t CAMERA_CUT_ANGLE = Math::PI / 4.0;
	static constexpr real_t PROJECTION_CUT_FOV_DEGREES = 5.0;
	static constexpr real_t PROJECTION_CUT_ASPECT = 0.2;
	static constexpr double LONG_FRAME_SECONDS = 0.25;

	bool valid = false;
	uint64_t last_frame = 0;

	static bool is_camera_cut(const Transform3D &p_current, const Transform3D &p_previous) {
		if (p_current.origin.distance_to(p_previous.origin) > CAMERA_CUT_DISTANCE) {
			return true;
		}
		const Vector3 current_forward = -p_current.basis.get_column(2).normalized();
		const Vector3 previous_forward = -p_previous.basis.get_column(2).normalized();
		return current_forward.angle_to(previous_forward) > CAMERA_CUT_ANGLE;
	}

	static bool is_projection_cut(const Projection &p_current, const Projection &p_previous) {
		if (p_current.is_orthogonal() != p_previous.is_orthogonal()) {
			return true;
		}
		return Math::abs(p_current.get_fov() - p_previous.get_fov()) > PROJECTION_CUT_FOV_DEGREES ||
				Math::abs(p_current.get_aspect() - p_previous.get_aspect()) > PROJECTION_CUT_ASPECT;
	}

	uint32_t begin_frame(uint64_t p_frame, double p_frame_step, const Transform3D &p_current_camera, const Transform3D &p_previous_camera, const Projection &p_current_projection, const Projection &p_previous_projection) {
		uint32_t reasons = PT_PRESENTATION_HISTORY_RESET_NONE;
		if (!valid) {
			reasons |= PT_PRESENTATION_HISTORY_RESET_CONTEXT;
		} else if (p_frame > last_frame + 1) {
			reasons |= PT_PRESENTATION_HISTORY_RESET_FRAME_GAP;
		}
		if (is_camera_cut(p_current_camera, p_previous_camera)) {
			reasons |= PT_PRESENTATION_HISTORY_RESET_CAMERA_CUT;
		}
		if (is_projection_cut(p_current_projection, p_previous_projection)) {
			reasons |= PT_PRESENTATION_HISTORY_RESET_PROJECTION_CUT;
		}
		if (p_frame_step > LONG_FRAME_SECONDS) {
			reasons |= PT_PRESENTATION_HISTORY_RESET_LONG_FRAME;
		}
		valid = true;
		last_frame = p_frame;
		return reasons;
	}
};

} // namespace RendererSceneRenderImplementation
