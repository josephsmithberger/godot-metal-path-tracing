/**************************************************************************/
/*  test_metal_rt_geometry.cpp                                            */
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

TEST_FORCE_LINK(test_metal_rt_geometry)

#ifdef METAL_ENABLED

#include "drivers/metal/metal_objects_shared.h"

namespace TestMetalRTGeometry {

static bool submit_build(MTL::CommandQueue *p_queue, MDAccelerationStructure &p_structure, MTL::Buffer *p_scratch) {
	NS::SharedPtr<MTL::CommandBuffer> command = NS::RetainPtr(p_queue->commandBuffer());
	if (!command) {
		return false;
	}
	NS::SharedPtr<MTL::AccelerationStructureCommandEncoder> encoder = NS::RetainPtr(command->accelerationStructureCommandEncoder());
	if (!encoder) {
		return false;
	}
	const uint64_t build_generation = p_structure.encode_build(encoder.get(), p_scratch);
	encoder->endEncoding();
	command->commit();
	command->waitUntilCompleted();
	const bool completed = command->status() == MTL::CommandBufferStatusCompleted && command->error() == nullptr;
	if (completed) {
		p_structure.completion_state->completed_build.store(build_generation, std::memory_order_release);
	}
	return completed;
}

static bool submit_refit(MTL::CommandQueue *p_queue, MDAccelerationStructure &p_structure, MTL::Buffer *p_scratch) {
	NS::SharedPtr<MTL::CommandBuffer> command = NS::RetainPtr(p_queue->commandBuffer());
	if (!command) {
		return false;
	}
	NS::SharedPtr<MTL::AccelerationStructureCommandEncoder> encoder = NS::RetainPtr(command->accelerationStructureCommandEncoder());
	if (!encoder) {
		return false;
	}
	p_structure.encode_refit(encoder.get(), p_scratch);
	encoder->endEncoding();
	command->commit();
	command->waitUntilCompleted();
	return command->status() == MTL::CommandBufferStatusCompleted && command->error() == nullptr;
}

static NS::SharedPtr<MTL::PrimitiveAccelerationStructureDescriptor> make_blas_descriptor(MTL::AccelerationStructureGeometryDescriptor *p_geometry, MTL::AccelerationStructureUsage p_usage) {
	NS::Object *geometry_object = p_geometry;
	NS::SharedPtr<NS::Array> geometries = NS::TransferPtr(NS::Array::array(&geometry_object, 1)->retain());
	NS::SharedPtr<MTL::PrimitiveAccelerationStructureDescriptor> descriptor = NS::TransferPtr(MTL::PrimitiveAccelerationStructureDescriptor::alloc()->init());
	descriptor->setGeometryDescriptors(geometries.get());
	descriptor->setUsage(p_usage);
	return descriptor;
}

TEST_CASE_PENDING("[MetalRT][GPU] Rebuilds real scene geometry through twenty lifetime iterations") {
	NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
	NS::SharedPtr<MTL::Device> device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
	if (!device || !device->supportsRaytracing()) {
		MESSAGE("SKIP_REASON=missing_metal_rt_feature");
		return;
	}
	if (!__builtin_available(macOS 13.0, iOS 16.0, tvOS 16.0, *)) {
		MESSAGE("SKIP_REASON=missing_metal_rt_feature");
		return;
	}

	NS::SharedPtr<MTL::CommandQueue> queue = NS::TransferPtr(device->newCommandQueue());
	REQUIRE(queue);

	struct Float3 {
		float x;
		float y;
		float z;
	};
	struct UNorm16x4 {
		uint16_t x;
		uint16_t y;
		uint16_t z;
		uint16_t w;
	};

	const Float3 indexed_vertices[] = {
		{ -1.0f, -1.0f, 0.0f },
		{ 1.0f, -1.0f, 0.0f },
		{ 1.0f, 1.0f, 0.0f },
		{ -1.0f, 1.0f, 0.0f },
	};
	const uint16_t indexed_indices[] = { 0, 2, 1, 0, 3, 2 };
	const UNorm16x4 compressed_vertices[] = {
		{ 0, 0, 0, UINT16_MAX },
		{ UINT16_MAX, 0, 0, UINT16_MAX },
		{ 0, UINT16_MAX, 0, UINT16_MAX },
	};
	const Float3 initial_deformed_vertices[] = {
		{ -0.5f, -0.5f, 0.0f },
		{ 0.5f, -0.5f, 0.0f },
		{ 0.0f, 0.5f, 0.0f },
	};

	uint32_t as_rebuild_count = 0;
	uint32_t as_refit_count = 0;
	uint64_t peak_as_bytes = 0;

	for (uint32_t iteration = 0; iteration < 20; iteration++) {
		CAPTURE(iteration);
		NS::SharedPtr<MTL::Buffer> indexed_vertex_buffer = NS::TransferPtr(device->newBuffer(indexed_vertices, sizeof(indexed_vertices), MTL::ResourceStorageModeShared));
		NS::SharedPtr<MTL::Buffer> indexed_index_buffer = NS::TransferPtr(device->newBuffer(indexed_indices, sizeof(indexed_indices), MTL::ResourceStorageModeShared));
		NS::SharedPtr<MTL::Buffer> compressed_vertex_buffer = NS::TransferPtr(device->newBuffer(compressed_vertices, sizeof(compressed_vertices), MTL::ResourceStorageModeShared));
		NS::SharedPtr<MTL::Buffer> deformed_vertex_buffer = NS::TransferPtr(device->newBuffer(initial_deformed_vertices, sizeof(initial_deformed_vertices), MTL::ResourceStorageModeShared));
		REQUIRE(indexed_vertex_buffer);
		REQUIRE(indexed_index_buffer);
		REQUIRE(compressed_vertex_buffer);
		REQUIRE(deformed_vertex_buffer);

		NS::SharedPtr<MTL::AccelerationStructureTriangleGeometryDescriptor> indexed_geometry = NS::TransferPtr(MTL::AccelerationStructureTriangleGeometryDescriptor::alloc()->init());
		indexed_geometry->setVertexBuffer(indexed_vertex_buffer.get());
		indexed_geometry->setVertexStride(sizeof(Float3));
		indexed_geometry->setIndexBuffer(indexed_index_buffer.get());
		indexed_geometry->setIndexType(MTL::IndexTypeUInt16);
		indexed_geometry->setTriangleCount(2);
		indexed_geometry->setOpaque(true);

		NS::SharedPtr<MTL::AccelerationStructureTriangleGeometryDescriptor> compressed_geometry = NS::TransferPtr(MTL::AccelerationStructureTriangleGeometryDescriptor::alloc()->init());
		compressed_geometry->setVertexBuffer(compressed_vertex_buffer.get());
		compressed_geometry->setVertexStride(sizeof(UNorm16x4));
		compressed_geometry->setVertexFormat(MTL::AttributeFormatUShort4Normalized);
		compressed_geometry->setTriangleCount(1);
		compressed_geometry->setOpaque(true);

		NS::SharedPtr<MTL::AccelerationStructureTriangleGeometryDescriptor> deformed_geometry = NS::TransferPtr(MTL::AccelerationStructureTriangleGeometryDescriptor::alloc()->init());
		deformed_geometry->setVertexBuffer(deformed_vertex_buffer.get());
		deformed_geometry->setVertexStride(sizeof(Float3));
		deformed_geometry->setTriangleCount(1);
		deformed_geometry->setOpaque(true);

		BitField<RDD::AccelerationStructureFlagBits> static_flags;
		static_flags.set_flag(RDD::ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT);
		BitField<RDD::AccelerationStructureFlagBits> deformed_flags;
		deformed_flags.set_flag(RDD::ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT);
		deformed_flags.set_flag(RDD::ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT);

		NS::SharedPtr<MTL::PrimitiveAccelerationStructureDescriptor> indexed_descriptor = make_blas_descriptor(indexed_geometry.get(), MDAccelerationStructure::usage_from_flags(static_flags));
		NS::SharedPtr<MTL::PrimitiveAccelerationStructureDescriptor> compressed_descriptor = make_blas_descriptor(compressed_geometry.get(), MDAccelerationStructure::usage_from_flags(static_flags));
		NS::SharedPtr<MTL::PrimitiveAccelerationStructureDescriptor> deformed_descriptor = make_blas_descriptor(deformed_geometry.get(), MDAccelerationStructure::usage_from_flags(deformed_flags));

		const MTL::AccelerationStructureSizes indexed_sizes = device->accelerationStructureSizes(indexed_descriptor.get());
		const MTL::AccelerationStructureSizes compressed_sizes = device->accelerationStructureSizes(compressed_descriptor.get());
		const MTL::AccelerationStructureSizes deformed_sizes = device->accelerationStructureSizes(deformed_descriptor.get());
		MDAccelerationStructure indexed_blas(MDAccelerationStructure::Type::BLAS, indexed_descriptor, indexed_sizes, static_flags);
		MDAccelerationStructure compressed_blas(MDAccelerationStructure::Type::BLAS, compressed_descriptor, compressed_sizes, static_flags);
		MDAccelerationStructure deformed_blas(MDAccelerationStructure::Type::BLAS, deformed_descriptor, deformed_sizes, deformed_flags);
		REQUIRE(indexed_blas.allocate(device.get()));
		REQUIRE(compressed_blas.allocate(device.get()));
		REQUIRE(deformed_blas.allocate(device.get()));

		NS::SharedPtr<MTL::Buffer> indexed_scratch = NS::TransferPtr(device->newBuffer(MAX(uint64_t(1), indexed_blas.scratch_size), MTL::ResourceStorageModePrivate));
		NS::SharedPtr<MTL::Buffer> compressed_scratch = NS::TransferPtr(device->newBuffer(MAX(uint64_t(1), compressed_blas.scratch_size), MTL::ResourceStorageModePrivate));
		NS::SharedPtr<MTL::Buffer> deformed_scratch = NS::TransferPtr(device->newBuffer(MAX(uint64_t(1), deformed_blas.scratch_size), MTL::ResourceStorageModePrivate));
		REQUIRE(submit_build(queue.get(), indexed_blas, indexed_scratch.get()));
		REQUIRE(submit_build(queue.get(), compressed_blas, compressed_scratch.get()));
		REQUIRE(submit_build(queue.get(), deformed_blas, deformed_scratch.get()));
		as_rebuild_count += 3;

		Float3 *deformed_vertices = static_cast<Float3 *>(deformed_vertex_buffer->contents());
		deformed_vertices[2].y = 0.5f + 0.025f * float(iteration + 1);
		REQUIRE(submit_refit(queue.get(), deformed_blas, deformed_scratch.get()));
		as_refit_count++;

		NS::SharedPtr<MTL::InstanceAccelerationStructureDescriptor> tlas_descriptor = NS::TransferPtr(MTL::InstanceAccelerationStructureDescriptor::alloc()->init());
		tlas_descriptor->setInstanceCount(4);
		tlas_descriptor->setInstanceDescriptorStride(sizeof(MDAccelerationStructureInstance));
		tlas_descriptor->setInstanceDescriptorType(MTL::AccelerationStructureInstanceDescriptorTypeUserID);
		const MTL::AccelerationStructureSizes tlas_sizes = device->accelerationStructureSizes(tlas_descriptor.get());
		MDAccelerationStructure tlas(MDAccelerationStructure::Type::TLAS, tlas_descriptor, tlas_sizes, {}, 4);
		REQUIRE(tlas.allocate(device.get()));
		NS::SharedPtr<MTL::Buffer> tlas_scratch = NS::TransferPtr(device->newBuffer(MAX(uint64_t(1), tlas.scratch_size), MTL::ResourceStorageModePrivate));
		NS::SharedPtr<MTL::Buffer> instance_buffer = NS::TransferPtr(device->newBuffer(4 * sizeof(MDAccelerationStructureInstance), MTL::ResourceStorageModeShared));
		REQUIRE(tlas_scratch);
		REQUIRE(instance_buffer);

		RDD::AccelerationStructureInstance source_instances[4];
		source_instances[0].id = 100;
		source_instances[0].mask = 0xFF;
		source_instances[0].hit_sbt_offset = 1;
		source_instances[0].flags.set_flag(RDD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT);
		source_instances[0].blas = RDD::AccelerationStructureID(&indexed_blas);

		source_instances[1] = source_instances[0];
		source_instances[1].id = 101;
		source_instances[1].mask = 0x0F;
		source_instances[1].transform.basis.scale(Vector3(-1.0, 1.0, 1.0));
		source_instances[1].transform.origin.x = 2.0;
		source_instances[1].flags.set_flag(RDD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT);

		source_instances[2].id = 200;
		source_instances[2].mask = 0xF0;
		source_instances[2].hit_sbt_offset = 1;
		source_instances[2].transform.origin.x = -2.0;
		source_instances[2].blas = RDD::AccelerationStructureID(&compressed_blas);

		source_instances[3].id = 300;
		source_instances[3].mask = 0xA5;
		source_instances[3].hit_sbt_offset = 1;
		source_instances[3].transform.origin.y = 1.0;
		source_instances[3].blas = RDD::AccelerationStructureID(&deformed_blas);

		auto write_instances = [&](uint32_t p_count) {
			for (uint32_t i = 0; i < p_count; i++) {
				MDAccelerationStructureInstance record;
				REQUIRE(record.write(source_instances[i]));
				memcpy(static_cast<uint8_t *>(instance_buffer->contents()) + i * sizeof(record), &record, sizeof(record));
			}
		};

		// Build, remove the deformed fixture, then re-add it without recreating
		// the TLAS. Each build rewrites the descriptor array and masks.
		write_instances(4);
		REQUIRE(tlas.prepare_tlas_build(instance_buffer.get(), 0, 4));
		REQUIRE(submit_build(queue.get(), tlas, tlas_scratch.get()));
		as_rebuild_count++;
		write_instances(3);
		REQUIRE(tlas.prepare_tlas_build(instance_buffer.get(), 0, 3));
		REQUIRE(submit_build(queue.get(), tlas, tlas_scratch.get()));
		as_rebuild_count++;
		write_instances(4);
		REQUIRE(tlas.prepare_tlas_build(instance_buffer.get(), 0, 4));
		REQUIRE(submit_build(queue.get(), tlas, tlas_scratch.get()));
		as_rebuild_count++;

		MDAccelerationStructureInstance round_trip[4];
		memcpy(round_trip, instance_buffer->contents(), sizeof(round_trip));
		CHECK(round_trip[0].user_id == 100);
		CHECK(round_trip[1].user_id == 101);
		CHECK(round_trip[2].user_id == 200);
		CHECK(round_trip[3].user_id == 300);
		CHECK(round_trip[0].mask == 0xFF);
		CHECK(round_trip[1].mask == 0x0F);
		CHECK(round_trip[2].mask == 0xF0);
		CHECK(round_trip[3].mask == 0xA5);
		CHECK((round_trip[1].options & MTL::AccelerationStructureInstanceOptionTriangleFrontFacingWindingCounterClockwise) != 0);
		CHECK(indexed_blas.get_compacted_size() > 0);
		CHECK(compressed_blas.get_compacted_size() > 0);

		const uint64_t iteration_as_bytes = indexed_blas.acceleration_structure_size + compressed_blas.acceleration_structure_size + deformed_blas.acceleration_structure_size + tlas.acceleration_structure_size;
		peak_as_bytes = MAX(peak_as_bytes, iteration_as_bytes);
	}

	CHECK(as_rebuild_count == 120);
	CHECK(as_refit_count == 20);
	print_line(vformat("MetalRT scene geometry: iterations=20 rebuilds=%d refits=%d peak_as_bytes=%d",
			as_rebuild_count, as_refit_count, peak_as_bytes));
}

TEST_CASE_PENDING("[MetalRT][GPU] Refits procedural AABBs in mixed geometry through twenty lifetime iterations") {
	NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
	NS::SharedPtr<MTL::Device> device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
	if (!device || !device->supportsRaytracing()) {
		MESSAGE("SKIP_REASON=missing_metal_rt_feature");
		return;
	}
	if (!__builtin_available(macOS 13.0, iOS 16.0, tvOS 16.0, *)) {
		MESSAGE("SKIP_REASON=missing_metal_rt_feature");
		return;
	}

	NS::SharedPtr<MTL::CommandQueue> queue = NS::TransferPtr(device->newCommandQueue());
	REQUIRE(queue);

	struct Float3 {
		float x;
		float y;
		float z;
	};
	struct AABBRecord {
		Float3 min;
		Float3 max;
	};
	static_assert(sizeof(AABBRecord) == 24);

	const Float3 triangle_vertices[] = {
		{ -1.0f, -1.0f, 0.0f },
		{ 1.0f, -1.0f, 0.0f },
		{ 0.0f, 1.0f, 0.0f },
	};
	const AABBRecord initial_bounds[] = {
		{ { -0.8f, -0.8f, -0.8f }, { 0.0f, 0.8f, 0.8f } },
		{ { 0.0f, -0.8f, -0.8f }, { 0.8f, 0.8f, 0.8f } },
	};

	uint32_t procedural_build_count = 0;
	uint32_t procedural_refit_count = 0;
	uint32_t mixed_tlas_build_count = 0;
	uint64_t peak_as_bytes = 0;

	for (uint32_t iteration = 0; iteration < 20; iteration++) {
		CAPTURE(iteration);
		NS::SharedPtr<MTL::Buffer> triangle_buffer = NS::TransferPtr(device->newBuffer(triangle_vertices, sizeof(triangle_vertices), MTL::ResourceStorageModeShared));
		NS::SharedPtr<MTL::Buffer> bounds_buffer = NS::TransferPtr(device->newBuffer(initial_bounds, sizeof(initial_bounds), MTL::ResourceStorageModeShared));
		REQUIRE(triangle_buffer);
		REQUIRE(bounds_buffer);

		NS::SharedPtr<MTL::AccelerationStructureTriangleGeometryDescriptor> triangle_geometry = NS::TransferPtr(MTL::AccelerationStructureTriangleGeometryDescriptor::alloc()->init());
		triangle_geometry->setVertexBuffer(triangle_buffer.get());
		triangle_geometry->setVertexStride(sizeof(Float3));
		triangle_geometry->setTriangleCount(1);
		triangle_geometry->setOpaque(true);

		NS::SharedPtr<MTL::AccelerationStructureBoundingBoxGeometryDescriptor> procedural_geometry = NS::TransferPtr(MTL::AccelerationStructureBoundingBoxGeometryDescriptor::alloc()->init());
		procedural_geometry->setBoundingBoxBuffer(bounds_buffer.get());
		procedural_geometry->setBoundingBoxStride(sizeof(AABBRecord));
		procedural_geometry->setBoundingBoxCount(2);
		procedural_geometry->setOpaque(false);

		BitField<RDD::AccelerationStructureFlagBits> procedural_flags;
		procedural_flags.set_flag(RDD::ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT);
		procedural_flags.set_flag(RDD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT);
		NS::SharedPtr<MTL::PrimitiveAccelerationStructureDescriptor> triangle_descriptor = make_blas_descriptor(triangle_geometry.get(), MTL::AccelerationStructureUsageNone);
		NS::SharedPtr<MTL::PrimitiveAccelerationStructureDescriptor> procedural_descriptor = make_blas_descriptor(procedural_geometry.get(), MDAccelerationStructure::usage_from_flags(procedural_flags));

		const MTL::AccelerationStructureSizes triangle_sizes = device->accelerationStructureSizes(triangle_descriptor.get());
		const MTL::AccelerationStructureSizes procedural_sizes = device->accelerationStructureSizes(procedural_descriptor.get());
		MDAccelerationStructure triangle_blas(MDAccelerationStructure::Type::BLAS, triangle_descriptor, triangle_sizes, {});
		MDAccelerationStructure procedural_blas(MDAccelerationStructure::Type::BLAS, procedural_descriptor, procedural_sizes, procedural_flags);
		REQUIRE(triangle_blas.allocate(device.get()));
		REQUIRE(procedural_blas.allocate(device.get()));

		NS::SharedPtr<MTL::Buffer> triangle_scratch = NS::TransferPtr(device->newBuffer(MAX(uint64_t(1), triangle_blas.scratch_size), MTL::ResourceStorageModePrivate));
		NS::SharedPtr<MTL::Buffer> procedural_scratch = NS::TransferPtr(device->newBuffer(MAX(uint64_t(1), procedural_blas.scratch_size), MTL::ResourceStorageModePrivate));
		REQUIRE(submit_build(queue.get(), triangle_blas, triangle_scratch.get()));
		REQUIRE(submit_build(queue.get(), procedural_blas, procedural_scratch.get()));
		procedural_build_count++;

		AABBRecord *updated_bounds = static_cast<AABBRecord *>(bounds_buffer->contents());
		updated_bounds[0].min.x -= 0.025f * float(iteration + 1);
		updated_bounds[0].max.x += 0.025f * float(iteration + 1);
		REQUIRE(submit_refit(queue.get(), procedural_blas, procedural_scratch.get()));
		procedural_refit_count++;

		NS::SharedPtr<MTL::InstanceAccelerationStructureDescriptor> tlas_descriptor = NS::TransferPtr(MTL::InstanceAccelerationStructureDescriptor::alloc()->init());
		tlas_descriptor->setInstanceCount(2);
		tlas_descriptor->setInstanceDescriptorStride(sizeof(MDAccelerationStructureInstance));
		tlas_descriptor->setInstanceDescriptorType(MTL::AccelerationStructureInstanceDescriptorTypeUserID);
		const MTL::AccelerationStructureSizes tlas_sizes = device->accelerationStructureSizes(tlas_descriptor.get());
		MDAccelerationStructure tlas(MDAccelerationStructure::Type::TLAS, tlas_descriptor, tlas_sizes, {}, 2);
		REQUIRE(tlas.allocate(device.get()));
		NS::SharedPtr<MTL::Buffer> tlas_scratch = NS::TransferPtr(device->newBuffer(MAX(uint64_t(1), tlas.scratch_size), MTL::ResourceStorageModePrivate));
		NS::SharedPtr<MTL::Buffer> instance_buffer = NS::TransferPtr(device->newBuffer(2 * sizeof(MDAccelerationStructureInstance), MTL::ResourceStorageModeShared));
		REQUIRE(tlas_scratch);
		REQUIRE(instance_buffer);

		RDD::AccelerationStructureInstance instances[2];
		instances[0].id = 0x12345;
		instances[0].mask = 0xFF;
		instances[0].hit_sbt_offset = 3;
		instances[0].blas = RDD::AccelerationStructureID(&triangle_blas);
		instances[1].id = 0x23456;
		instances[1].mask = 0x7F;
		instances[1].hit_sbt_offset = 9;
		instances[1].transform.origin.x = 1.5;
		instances[1].blas = RDD::AccelerationStructureID(&procedural_blas);

		auto write_instances = [&](uint32_t p_count) {
			for (uint32_t i = 0; i < p_count; i++) {
				MDAccelerationStructureInstance record;
				REQUIRE(record.write(instances[i]));
				memcpy(static_cast<uint8_t *>(instance_buffer->contents()) + i * sizeof(record), &record, sizeof(record));
			}
		};

		write_instances(2);
		REQUIRE(tlas.prepare_tlas_build(instance_buffer.get(), 0, 2));
		REQUIRE(submit_build(queue.get(), tlas, tlas_scratch.get()));
		mixed_tlas_build_count++;

		MDAccelerationStructureInstance records[2];
		memcpy(records, instance_buffer->contents(), sizeof(records));
		CHECK(records[0].user_id == 0x12345);
		CHECK(records[1].user_id == 0x23456);
		CHECK(records[0].intersection_function_table_offset == 3);
		CHECK(records[1].intersection_function_table_offset == 9);

		// Remove the procedural instance while retaining the valid triangle.
		write_instances(1);
		REQUIRE(tlas.prepare_tlas_build(instance_buffer.get(), 0, 1));
		REQUIRE(submit_build(queue.get(), tlas, tlas_scratch.get()));
		mixed_tlas_build_count++;
		memcpy(records, instance_buffer->contents(), sizeof(MDAccelerationStructureInstance));
		CHECK(records[0].user_id == 0x12345);

		peak_as_bytes = MAX(peak_as_bytes, triangle_blas.acceleration_structure_size + procedural_blas.acceleration_structure_size + tlas.acceleration_structure_size);
	}

	CHECK(procedural_build_count == 20);
	CHECK(procedural_refit_count == 20);
	CHECK(mixed_tlas_build_count == 40);
	print_line(vformat("MetalRT procedural geometry: iterations=20 builds=%d refits=%d mixed_tlas_builds=%d peak_as_bytes=%d",
			procedural_build_count, procedural_refit_count, mixed_tlas_build_count, peak_as_bytes));
}

} // namespace TestMetalRTGeometry

#endif // METAL_ENABLED
