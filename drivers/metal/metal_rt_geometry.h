/**************************************************************************/
/*  metal_rt_geometry.h                                                   */
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
#include "servers/rendering/rendering_device_driver.h"

#include <Metal/Metal.hpp>

/// Fully validated Metal layout for one RenderingDevice BLAS geometry.
///
/// Validation is deliberately separate from native descriptor allocation:
/// `blas_create()` validates every entry first, then creates the Metal
/// descriptors. This prevents an unsupported surface in a multi-geometry BLAS
/// from leaving a partially initialized descriptor graph.
struct MetalRTGeometryLayout {
	RDD::AccelerationStructureGeometry::Type type = RDD::AccelerationStructureGeometry::TYPE_TRIANGLES;

	// Triangle geometry.
	MTL::AttributeFormat vertex_format = MTL::AttributeFormatInvalid;
	MTL::IndexType index_type = MTL::IndexTypeUInt16;
	uint32_t primitive_count = 0;
	bool indexed = false;

	static bool validate(const RDD::AccelerationStructureGeometry &p_geometry, bool p_extended_vertex_formats, MetalRTGeometryLayout &r_layout, String &r_error);
};
