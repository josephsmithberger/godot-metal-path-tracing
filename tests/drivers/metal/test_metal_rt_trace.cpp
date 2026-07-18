/**************************************************************************/
/*  test_metal_rt_trace.cpp                                               */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_metal_rt_trace)

#ifdef METAL_ENABLED

#include "drivers/metal/metal_objects_shared.h"

namespace TestMetalRTTrace {

static constexpr uint32_t IMAGE_WIDTH = 4;
static constexpr uint32_t IMAGE_HEIGHT = 4;
static constexpr uint64_t GOLDEN_FNV1A64 = 0x7d35dd82cbfd1de5ULL;

struct TracePixel {
	uint8_t red;
	uint8_t green;
	uint8_t blue;
	uint8_t alpha;

	bool operator==(const TracePixel &p_other) const {
		return red == p_other.red && green == p_other.green && blue == p_other.blue && alpha == p_other.alpha;
	}
};

static_assert(sizeof(TracePixel) == MDRaytracingPipeline::TRACE_PIXEL_SIZE_BYTES);

static constexpr TracePixel HIT_PIXEL = { 232, 168, 32, 255 };
static constexpr TracePixel MISS_PIXEL = { 16, 24, 40, 255 };

static uint64_t fnv1a64(const uint8_t *p_data, uint64_t p_size) {
	uint64_t hash = 14695981039346656037ULL;
	for (uint64_t i = 0; i < p_size; i++) {
		hash ^= p_data[i];
		hash *= 1099511628211ULL;
	}
	return hash;
}

static void fill_golden(TracePixel *r_pixels) {
	for (uint32_t y = 0; y < IMAGE_HEIGHT; y++) {
		for (uint32_t x = 0; x < IMAGE_WIDTH; x++) {
			r_pixels[y * IMAGE_WIDTH + x] = ((x + y) & 1u) == 0u ? HIT_PIXEL : MISS_PIXEL;
		}
	}
}

// TRACE_KERNEL deliberately reuses the exact BLAS/TLAS primitive and instance path. Keeping
// the scene local to this test makes the golden dispatch independent from the
// later RenderingDevice/SBT mapping in PIPELINE_MAPPING.
struct TraceScene {
	NS::SharedPtr<MTL::Buffer> vertex_buffer;
	NS::SharedPtr<MTL::Buffer> instance_buffer;
	NS::SharedPtr<MTL::AccelerationStructure> blas;
	NS::SharedPtr<MTL::AccelerationStructure> tlas;

	bool build(MTL::Device *p_device, MTL::CommandQueue *p_queue) {
		struct TriangleVertex {
			float x, y, z;
		};
		const TriangleVertex vertices[] = {
			{ -1.0f, -1.0f, 0.0f },
			{ 1.0f, -1.0f, 0.0f },
			{ 0.0f, 1.0f, 0.0f },
		};
		vertex_buffer = NS::TransferPtr(p_device->newBuffer(vertices, sizeof(vertices), MTL::ResourceStorageModeShared));
		if (!vertex_buffer) {
			return false;
		}

		NS::SharedPtr<MTL::AccelerationStructureTriangleGeometryDescriptor> geometry = NS::TransferPtr(MTL::AccelerationStructureTriangleGeometryDescriptor::alloc()->init());
		geometry->setVertexBuffer(vertex_buffer.get());
		geometry->setVertexBufferOffset(0);
		geometry->setVertexStride(sizeof(TriangleVertex));
		geometry->setTriangleCount(1);
		geometry->setOpaque(true);

		NS::Object *geometry_object = geometry.get();
		NS::SharedPtr<NS::Array> geometries = NS::TransferPtr(NS::Array::array(&geometry_object, 1)->retain());
		NS::SharedPtr<MTL::PrimitiveAccelerationStructureDescriptor> blas_descriptor = NS::TransferPtr(MTL::PrimitiveAccelerationStructureDescriptor::alloc()->init());
		blas_descriptor->setGeometryDescriptors(geometries.get());

		const MTL::AccelerationStructureSizes blas_sizes = p_device->accelerationStructureSizes(blas_descriptor.get());
		MDAccelerationStructure blas_structure(MDAccelerationStructure::Type::BLAS, blas_descriptor, blas_sizes, {});
		if (!blas_structure.allocate(p_device)) {
			return false;
		}
		NS::SharedPtr<MTL::Buffer> blas_scratch = NS::TransferPtr(p_device->newBuffer(blas_structure.build_scratch_size, MTL::ResourceStorageModePrivate));
		if (!blas_scratch) {
			return false;
		}
		NS::SharedPtr<MTL::CommandBuffer> blas_command = NS::RetainPtr(p_queue->commandBuffer());
		NS::SharedPtr<MTL::AccelerationStructureCommandEncoder> blas_encoder = NS::RetainPtr(blas_command->accelerationStructureCommandEncoder());
		if (!blas_encoder) {
			return false;
		}
		blas_structure.encode_build(blas_encoder.get(), blas_scratch.get());
		blas_encoder->endEncoding();
		blas_command->commit();
		blas_command->waitUntilCompleted();
		if (blas_command->status() != MTL::CommandBufferStatusCompleted || blas_command->error() != nullptr) {
			return false;
		}
		blas = blas_structure.accel;

		RDD::AccelerationStructureInstance instance;
		instance.id = 42;
		instance.mask = 0xff;
		instance.hit_sbt_offset = 0;
		instance.flags.set_flag(RDD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT);
		instance.flags.set_flag(RDD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT);
		instance.blas = RDD::AccelerationStructureID(&blas_structure);

		MDAccelerationStructureInstance metal_instance;
		if (!metal_instance.write(instance)) {
			return false;
		}
		instance_buffer = NS::TransferPtr(p_device->newBuffer(&metal_instance, sizeof(metal_instance), MTL::ResourceStorageModeShared));
		if (!instance_buffer) {
			return false;
		}

		NS::SharedPtr<MTL::InstanceAccelerationStructureDescriptor> tlas_descriptor = NS::TransferPtr(MTL::InstanceAccelerationStructureDescriptor::alloc()->init());
		tlas_descriptor->setInstanceCount(1);
		tlas_descriptor->setInstanceDescriptorStride(sizeof(MDAccelerationStructureInstance));
		const MTL::AccelerationStructureSizes tlas_sizes = p_device->accelerationStructureSizes(tlas_descriptor.get());
		MDAccelerationStructure tlas_structure(MDAccelerationStructure::Type::TLAS, tlas_descriptor, tlas_sizes, {}, 1);
		if (!tlas_structure.allocate(p_device) || !tlas_structure.prepare_tlas_build(instance_buffer.get(), 0, 1)) {
			return false;
		}
		NS::SharedPtr<MTL::Buffer> tlas_scratch = NS::TransferPtr(p_device->newBuffer(tlas_structure.build_scratch_size, MTL::ResourceStorageModePrivate));
		if (!tlas_scratch) {
			return false;
		}
		NS::SharedPtr<MTL::CommandBuffer> tlas_command = NS::RetainPtr(p_queue->commandBuffer());
		NS::SharedPtr<MTL::AccelerationStructureCommandEncoder> tlas_encoder = NS::RetainPtr(tlas_command->accelerationStructureCommandEncoder());
		if (!tlas_encoder) {
			return false;
		}
		tlas_structure.encode_build(tlas_encoder.get(), tlas_scratch.get());
		tlas_encoder->endEncoding();
		tlas_command->commit();
		tlas_command->waitUntilCompleted();
		if (tlas_command->status() != MTL::CommandBufferStatusCompleted || tlas_command->error() != nullptr) {
			return false;
		}
		tlas = tlas_structure.accel;
		return true;
	}
};

TEST_CASE("[MetalRT] TRACE_KERNEL trace image golden is stable") {
	TracePixel golden[IMAGE_WIDTH * IMAGE_HEIGHT];
	fill_golden(golden);
	CHECK(fnv1a64(reinterpret_cast<const uint8_t *>(golden), sizeof(golden)) == GOLDEN_FNV1A64);
	CHECK(golden[0] == HIT_PIXEL);
	CHECK(golden[1] == MISS_PIXEL);
	CHECK(golden[IMAGE_WIDTH] == MISS_PIXEL);
	CHECK(golden[IMAGE_WIDTH + 1] == HIT_PIXEL);

	MDRaytracingPipeline invalid_pipeline;
	String error;
	CHECK_FALSE(invalid_pipeline.create_trace_one_ray(nullptr, &error));
	CHECK_FALSE(invalid_pipeline.is_valid());
	CHECK(error.contains("not supported"));
}

TEST_CASE_PENDING("[MetalRT][GPU] TRACE_KERNEL traces a deterministic RGBA8 hit/miss image") {
	NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
	NS::SharedPtr<MTL::Device> device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
	if (!device || !device->supportsRaytracing()) {
		MESSAGE("SKIP_REASON=missing_metal_rt_feature");
		return;
	}

	TracePixel golden[IMAGE_WIDTH * IMAGE_HEIGHT];
	fill_golden(golden);
	NS::SharedPtr<MTL::CommandQueue> queue = NS::TransferPtr(device->newCommandQueue());
	REQUIRE(queue);

	for (uint32_t iteration = 0; iteration < 3; iteration++) {
		TraceScene scene;
		REQUIRE(scene.build(device.get(), queue.get()));

		MDRaytracingPipeline pipeline;
		String pipeline_error;
		REQUIRE_MESSAGE(pipeline.create_trace_one_ray(device.get(), &pipeline_error), pipeline_error);
		CHECK(pipeline.state);
		CHECK(pipeline.intersection_function_table);
		CHECK(pipeline.intersection_function_count == 1);

		TracePixel actual[IMAGE_WIDTH * IMAGE_HEIGHT] = {};
		NS::SharedPtr<MTL::Buffer> output_buffer = NS::TransferPtr(device->newBuffer(actual, sizeof(actual), MTL::ResourceStorageModeShared));
		REQUIRE(output_buffer);
		NS::SharedPtr<MTL::Buffer> undersized_output = NS::TransferPtr(device->newBuffer(sizeof(actual) - 1, MTL::ResourceStorageModeShared));
		REQUIRE(undersized_output);

		NS::SharedPtr<MTL::CommandBuffer> command = NS::RetainPtr(queue->commandBuffer());
		NS::SharedPtr<MTL::ComputeCommandEncoder> encoder = NS::RetainPtr(command->computeCommandEncoder());
		REQUIRE(encoder);
		CHECK_FALSE(pipeline.encode_trace_one_ray(encoder.get(), scene.tlas.get(), undersized_output.get(), IMAGE_WIDTH, IMAGE_HEIGHT));
		CHECK_FALSE(pipeline.encode_trace_one_ray(encoder.get(), scene.tlas.get(), output_buffer.get(), 0, IMAGE_HEIGHT));
		REQUIRE(pipeline.encode_trace_one_ray(encoder.get(), scene.tlas.get(), output_buffer.get(), IMAGE_WIDTH, IMAGE_HEIGHT));
		// The TLAS references the BLAS indirectly; it must be resident for the
		// trace dispatch even though the kernel binds only the TLAS.
		encoder->useResource(scene.blas.get(), MTL::ResourceUsageRead);
		encoder->endEncoding();
		command->commit();
		command->waitUntilCompleted();
		REQUIRE(command->status() == MTL::CommandBufferStatusCompleted);
		CHECK(command->error() == nullptr);

		memcpy(actual, output_buffer->contents(), sizeof(actual));
		for (uint32_t i = 0; i < IMAGE_WIDTH * IMAGE_HEIGHT; i++) {
			CHECK(actual[i] == golden[i]);
		}
		const uint64_t actual_hash = fnv1a64(reinterpret_cast<const uint8_t *>(actual), sizeof(actual));
		CHECK(actual_hash == GOLDEN_FNV1A64);
		print_line(vformat("MetalRT TRACE_KERNEL trace smoke: device=\"%s\" iteration=%d image=%dx%d ift_entries=%d golden_fnv1a64=%s",
				device->name()->utf8String(), iteration + 1, IMAGE_WIDTH, IMAGE_HEIGHT, pipeline.intersection_function_count, String::num_uint64(actual_hash, 16).lpad(16, "0")));
	}
}

} // namespace TestMetalRTTrace

#endif // METAL_ENABLED
