/**************************************************************************/
/*  metal_rt_geometry.cpp                                                 */
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

#include "metal_rt_geometry.h"

bool MetalRTGeometryLayout::validate(const RDD::AccelerationStructureGeometry &p_geometry, bool p_extended_vertex_formats, MetalRTGeometryLayout &r_layout, String &r_error) {
	r_layout = MetalRTGeometryLayout();
	r_layout.type = p_geometry.type;
	r_error = String();

	switch (p_geometry.type) {
		case RDD::AccelerationStructureGeometry::TYPE_TRIANGLES: {
			const RDD::AccelerationStructureGeometry::Triangles &triangles = p_geometry.geometry.triangles;
			if (!triangles.vertex_buffer) {
				r_error = "Triangle geometry requires a valid vertex buffer.";
				return false;
			}

			uint32_t format_size = 0;
			uint32_t format_alignment = 0;
			switch (triangles.vertex_format) {
				case RDD::DATA_FORMAT_R32G32B32_SFLOAT:
					r_layout.vertex_format = MTL::AttributeFormatFloat3;
					format_size = 12;
					format_alignment = 4;
					break;
				case RDD::DATA_FORMAT_R32G32_SFLOAT:
					r_layout.vertex_format = MTL::AttributeFormatFloat2;
					format_size = 8;
					format_alignment = 4;
					break;
				case RDD::DATA_FORMAT_R16G16B16A16_UNORM:
					r_layout.vertex_format = MTL::AttributeFormatUShort4Normalized;
					format_size = 8;
					format_alignment = 2;
					break;
				default:
					r_error = vformat("Unsupported Metal acceleration-structure vertex format (%d). Convert positions to R32G32B32_SFLOAT, R32G32_SFLOAT, or R16G16B16A16_UNORM.", triangles.vertex_format);
					return false;
			}

			if (r_layout.vertex_format != MTL::AttributeFormatFloat3 && !p_extended_vertex_formats) {
				r_error = "Metal acceleration-structure float2 and compressed UNORM16x4 positions require macOS 13.0 / iOS 16.0; convert positions to R32G32B32_SFLOAT on older systems.";
				return false;
			}
			if (triangles.vertex_count == 0) {
				r_error = "Triangle geometry requires at least three vertices.";
				return false;
			}
			if (triangles.vertex_stride < format_size || (triangles.vertex_stride % format_alignment) != 0) {
				r_error = vformat("Metal acceleration-structure vertex stride %d is invalid for the selected %d-byte position format.", triangles.vertex_stride, format_size);
				return false;
			}
			if ((triangles.vertex_offset % format_alignment) != 0) {
				r_error = "Metal acceleration-structure vertex offset is not aligned to the selected position format.";
				return false;
			}

			r_layout.indexed = bool(triangles.index_buffer);
			if (r_layout.indexed) {
				if (triangles.index_count == 0 || (triangles.index_count % 3) != 0) {
					r_error = "Indexed triangle geometry requires a nonzero index count that is a multiple of three.";
					return false;
				}
				uint32_t index_size = 0;
				switch (triangles.index_format) {
					case RDD::INDEX_BUFFER_FORMAT_UINT16:
						r_layout.index_type = MTL::IndexTypeUInt16;
						index_size = 2;
						break;
					case RDD::INDEX_BUFFER_FORMAT_UINT32:
						r_layout.index_type = MTL::IndexTypeUInt32;
						index_size = 4;
						break;
					default:
						r_error = "Metal acceleration structures support only uint16 or uint32 triangle indices.";
						return false;
				}
				if ((triangles.index_offset % index_size) != 0) {
					r_error = "Metal acceleration-structure index offset is not aligned to the index type.";
					return false;
				}
				r_layout.primitive_count = triangles.index_count / 3;
			} else {
				if ((triangles.vertex_count % 3) != 0) {
					r_error = "Non-indexed triangle geometry requires a vertex count that is a multiple of three.";
					return false;
				}
				r_layout.primitive_count = triangles.vertex_count / 3;
			}
		} break;

		case RDD::AccelerationStructureGeometry::TYPE_AABBS: {
			const RDD::AccelerationStructureGeometry::Aabbs &aabbs = p_geometry.geometry.aabbs;
			if (!aabbs.buffer) {
				r_error = "AABB geometry requires a valid bounds buffer.";
				return false;
			}
			if (aabbs.count == 0) {
				r_error = "AABB geometry requires at least one bounds record.";
				return false;
			}
			if (aabbs.stride < 24 || (aabbs.stride % 4) != 0) {
				r_error = "Metal AABB stride must be at least 24 bytes and a multiple of four.";
				return false;
			}
			if ((aabbs.offset % 4) != 0) {
				r_error = "Metal AABB offset must be four-byte aligned.";
				return false;
			}
			r_layout.primitive_count = aabbs.count;
		} break;
	}

	return true;
}
