/**************************************************************************/
/*  test_metal_rt.cpp                                                     */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_metal_rt)

#ifdef METAL_ENABLED

#include "drivers/metal/metal_objects_shared.h"

#include <limits>

namespace TestMetalRT {

TEST_CASE("[MetalRT] Acceleration structure metadata maps flags and scratch sizes") {
	MTL::AccelerationStructureSizes sizes = {};
	sizes.accelerationStructureSize = 4096;
	sizes.buildScratchBufferSize = 512;
	sizes.refitScratchBufferSize = 1024;

	BitField<RDD::AccelerationStructureFlagBits> flags = {};
	CHECK(MDAccelerationStructure::usage_from_flags(flags) == MTL::AccelerationStructureUsageNone);
	CHECK(MDAccelerationStructure::required_scratch_size(sizes, flags) == 512);

	flags.set_flag(RDD::ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT);
	CHECK(MDAccelerationStructure::usage_from_flags(flags) == MTL::AccelerationStructureUsageRefit);
	CHECK(MDAccelerationStructure::required_scratch_size(sizes, flags) == 1024);

	flags.set_flag(RDD::ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT);
	CHECK(MDAccelerationStructure::usage_from_flags(flags) == (MTL::AccelerationStructureUsageRefit | MTL::AccelerationStructureUsagePreferFastBuild));

	sizes.buildScratchBufferSize = 2048;
	sizes.refitScratchBufferSize = 1024;
	CHECK(MDAccelerationStructure::required_scratch_size(sizes, flags) == 2048);

	BitField<RDD::AccelerationStructureFlagBits> deferred_flags = {};
	deferred_flags.set_flag(RDD::ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT);
	deferred_flags.set_flag(RDD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT);
	deferred_flags.set_flag(RDD::ACCELERATION_STRUCTURE_LOW_MEMORY_BIT);
	CHECK(MDAccelerationStructure::usage_from_flags(deferred_flags) == MTL::AccelerationStructureUsageNone);
}

TEST_CASE("[MetalRT] Packs and validates TLAS instance records") {
	MTL::AccelerationStructureSizes sizes = {};
	MDAccelerationStructure blas(MDAccelerationStructure::Type::BLAS, NS::SharedPtr<MTL::AccelerationStructureDescriptor>(), sizes, {});
	MDAccelerationStructure tlas(MDAccelerationStructure::Type::TLAS, NS::SharedPtr<MTL::AccelerationStructureDescriptor>(), sizes, {}, 1);

	RDD::AccelerationStructureInstance instance;
	instance.transform.basis.rows[0] = Vector3(1.0, 2.0, 3.0);
	instance.transform.basis.rows[1] = Vector3(4.0, 5.0, 6.0);
	instance.transform.basis.rows[2] = Vector3(7.0, 8.0, 9.0);
	instance.transform.origin = Vector3(10.0, 11.0, 12.0);
	instance.id = 0x12345678;
	instance.mask = 0xa5;
	instance.hit_sbt_offset = 37;
	instance.flags.set_flag(RDD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT);
	instance.flags.set_flag(RDD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT);
	instance.blas = RDD::AccelerationStructureID(&blas);

	MDAccelerationStructureInstance metal_instance;
	REQUIRE(metal_instance.write(instance));
	CHECK(sizeof(metal_instance) == 128);
	CHECK(metal_instance.transformation_matrix[0] == 1.0f);
	CHECK(metal_instance.transformation_matrix[1] == 4.0f);
	CHECK(metal_instance.transformation_matrix[2] == 7.0f);
	CHECK(metal_instance.transformation_matrix[3] == 2.0f);
	CHECK(metal_instance.transformation_matrix[4] == 5.0f);
	CHECK(metal_instance.transformation_matrix[5] == 8.0f);
	CHECK(metal_instance.transformation_matrix[6] == 3.0f);
	CHECK(metal_instance.transformation_matrix[7] == 6.0f);
	CHECK(metal_instance.transformation_matrix[8] == 9.0f);
	CHECK(metal_instance.transformation_matrix[9] == 10.0f);
	CHECK(metal_instance.transformation_matrix[10] == 11.0f);
	CHECK(metal_instance.transformation_matrix[11] == 12.0f);
	CHECK(metal_instance.options == (MTL::AccelerationStructureInstanceOptionDisableTriangleCulling | MTL::AccelerationStructureInstanceOptionOpaque));
	CHECK(metal_instance.mask == 0xa5);
	CHECK(metal_instance.requested_mask == 0xa5);
	CHECK(metal_instance.intersection_function_table_offset == 37);
	CHECK(metal_instance.acceleration_structure_index == 0);
	CHECK(metal_instance.blas == &blas);
	CHECK(metal_instance.user_id == 0x12345678);

	instance.transform.origin.x = std::numeric_limits<real_t>::quiet_NaN();
	CHECK_FALSE(metal_instance.write(instance));
	instance.transform.origin.x = 10.0;
	instance.blas = RDD::AccelerationStructureID(&tlas);
	CHECK_FALSE(metal_instance.write(instance));
	instance.blas = RDD::AccelerationStructureID(&blas);
	instance.flags.set_flag(RDD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_NO_OPAQUE_BIT);
	CHECK_FALSE(metal_instance.write(instance));
}

TEST_CASE("[MetalRT] Backend placeholders own descriptors and preserve metadata") {
	MTL::AccelerationStructureSizes sizes = {};
	sizes.accelerationStructureSize = 8192;
	sizes.buildScratchBufferSize = 768;
	sizes.refitScratchBufferSize = 1536;

	BitField<RDD::AccelerationStructureFlagBits> flags = {};
	flags.set_flag(RDD::ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT);
	flags.set_flag(RDD::ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT);

	NS::SharedPtr<MTL::PrimitiveAccelerationStructureDescriptor> blas_descriptor = NS::TransferPtr(MTL::PrimitiveAccelerationStructureDescriptor::alloc()->init());
	MTL::PrimitiveAccelerationStructureDescriptor *raw_blas_descriptor = blas_descriptor.get();
	MDAccelerationStructure blas(MDAccelerationStructure::Type::BLAS, blas_descriptor, sizes, flags);
	blas_descriptor.reset();

	CHECK(blas.type == MDAccelerationStructure::Type::BLAS);
	CHECK(blas.descriptor.get() == raw_blas_descriptor);
	CHECK_FALSE(blas.accel);
	CHECK(blas.acceleration_structure_size == 8192);
	CHECK(blas.build_scratch_size == 768);
	CHECK(blas.refit_scratch_size == 1536);
	CHECK(blas.scratch_size == 1536);
	CHECK_FALSE(blas.compacted_size_buffer);
	CHECK(blas.get_compacted_size() == 0);
	CHECK(blas.flags.has_flag(RDD::ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT));
	CHECK(blas.flags.has_flag(RDD::ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT));
	CHECK(blas.max_instance_count == 0);

	NS::SharedPtr<MTL::InstanceAccelerationStructureDescriptor> tlas_descriptor = NS::TransferPtr(MTL::InstanceAccelerationStructureDescriptor::alloc()->init());
	MTL::InstanceAccelerationStructureDescriptor *raw_tlas_descriptor = tlas_descriptor.get();
	MDAccelerationStructure tlas(MDAccelerationStructure::Type::TLAS, tlas_descriptor, sizes, flags, 7);
	tlas_descriptor.reset();

	CHECK(tlas.type == MDAccelerationStructure::Type::TLAS);
	CHECK(tlas.descriptor.get() == raw_tlas_descriptor);
	CHECK_FALSE(tlas.accel);
	CHECK(tlas.max_instance_count == 7);

	MDPipeline *pipeline = new MDRaytracingPipeline;
	CHECK(pipeline->type == MDPipelineType::Raytracing);
	CHECK_FALSE(static_cast<MDRaytracingPipeline *>(pipeline)->state);
	delete pipeline;
}

TEST_CASE("[MetalRT] Sizes unbounded argument buffers from the bound descriptor count") {
	UniformSet shader_set;
	shader_set.uniforms.resize(1);
	shader_set.uniforms[0].arrayLength = UINT32_MAX;
	shader_set.uniforms[0].arg_buffer.texture = 3;
	shader_set.buffer_size = 4 * sizeof(uint64_t); // Three fixed entries plus the reserved runtime entry.
	shader_set.has_unbounded_array = true;

	Vector<RDD::BoundUniform> bound_uniforms;
	bound_uniforms.resize(1);
	bound_uniforms.write[0].type = RDD::UNIFORM_TYPE_TEXTURE;
	CHECK(shader_set.argument_buffer_size(bound_uniforms) == 4 * sizeof(uint64_t));

	bound_uniforms.write[0].ids.push_back(RDD::ID());
	bound_uniforms.write[0].ids.push_back(RDD::ID());
	CHECK(shader_set.argument_buffer_size(bound_uniforms) == 5 * sizeof(uint64_t));

	shader_set.has_unbounded_array = false;
	CHECK(shader_set.argument_buffer_size(bound_uniforms) == shader_set.buffer_size);
}

TEST_CASE_PENDING("[MetalRT][GPU] Builds, queries, and refits a single-triangle BLAS") {
	NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
	NS::SharedPtr<MTL::Device> device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
	if (!device || !device->supportsRaytracing()) {
		MESSAGE("SKIP_REASON=missing_metal_rt_feature");
		return;
	}

	struct TriangleVertex {
		float x;
		float y;
		float z;
	};
	const TriangleVertex vertices[] = {
		{ -1.0f, -1.0f, 0.0f },
		{ 1.0f, -1.0f, 0.0f },
		{ 0.0f, 1.0f, 0.0f },
	};
	NS::SharedPtr<MTL::Buffer> vertex_buffer = NS::TransferPtr(device->newBuffer(vertices, sizeof(vertices), MTL::ResourceStorageModeShared));
	REQUIRE(vertex_buffer);

	NS::SharedPtr<MTL::AccelerationStructureTriangleGeometryDescriptor> geometry = NS::TransferPtr(MTL::AccelerationStructureTriangleGeometryDescriptor::alloc()->init());
	geometry->setVertexBuffer(vertex_buffer.get());
	geometry->setVertexBufferOffset(0);
	geometry->setVertexStride(sizeof(TriangleVertex));
	geometry->setTriangleCount(1);
	geometry->setOpaque(true);

	NS::Object *geometry_object = geometry.get();
	NS::SharedPtr<NS::Array> geometries = NS::TransferPtr(NS::Array::array(&geometry_object, 1)->retain());
	NS::SharedPtr<MTL::PrimitiveAccelerationStructureDescriptor> descriptor = NS::TransferPtr(MTL::PrimitiveAccelerationStructureDescriptor::alloc()->init());
	descriptor->setGeometryDescriptors(geometries.get());

	BitField<RDD::AccelerationStructureFlagBits> flags = {};
	flags.set_flag(RDD::ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT);
	flags.set_flag(RDD::ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT);
	descriptor->setUsage(MDAccelerationStructure::usage_from_flags(flags));

	MTL::AccelerationStructureSizes sizes = device->accelerationStructureSizes(descriptor.get());
	REQUIRE(sizes.accelerationStructureSize > 0);
	REQUIRE(sizes.buildScratchBufferSize > 0);

	NS::SharedPtr<MTL::CommandQueue> queue = NS::TransferPtr(device->newCommandQueue());
	REQUIRE(queue);

	for (uint32_t iteration = 0; iteration < 3; iteration++) {
		MDAccelerationStructure blas(MDAccelerationStructure::Type::BLAS, descriptor, sizes, flags);
		REQUIRE(blas.allocate(device.get()));
		REQUIRE(blas.accel);
		REQUIRE(blas.compacted_size_buffer);

		NS::SharedPtr<MTL::Buffer> scratch = NS::TransferPtr(device->newBuffer(blas.scratch_size, MTL::ResourceStorageModePrivate));
		REQUIRE(scratch);

		NS::SharedPtr<MTL::CommandBuffer> build_command = NS::RetainPtr(queue->commandBuffer());
		NS::SharedPtr<MTL::AccelerationStructureCommandEncoder> build_encoder = NS::RetainPtr(build_command->accelerationStructureCommandEncoder());
		REQUIRE(build_encoder);
		blas.encode_build(build_encoder.get(), scratch.get());
		build_encoder->endEncoding();
		build_command->commit();
		build_command->waitUntilCompleted();

		REQUIRE(build_command->status() == MTL::CommandBufferStatusCompleted);
		CHECK(build_command->error() == nullptr);
		CHECK(blas.build_encoded);
		const uint64_t compacted_size = blas.get_compacted_size();
		CHECK(compacted_size > 0);
		CHECK(compacted_size <= blas.acceleration_structure_size);
		static_cast<TriangleVertex *>(vertex_buffer->contents())[2].y = 1.0f + (0.125f * (iteration + 1));

		NS::SharedPtr<MTL::CommandBuffer> refit_command = NS::RetainPtr(queue->commandBuffer());
		NS::SharedPtr<MTL::AccelerationStructureCommandEncoder> refit_encoder = NS::RetainPtr(refit_command->accelerationStructureCommandEncoder());
		REQUIRE(refit_encoder);
		blas.encode_refit(refit_encoder.get(), scratch.get());
		refit_encoder->endEncoding();
		refit_command->commit();
		refit_command->waitUntilCompleted();

		REQUIRE(refit_command->status() == MTL::CommandBufferStatusCompleted);
		CHECK(refit_command->error() == nullptr);
		print_line(vformat("MetalRT C5 BLAS smoke: device=\"%s\" iteration=%d build_size=%d compacted_size=%d build_scratch_size=%d refit_scratch_size=%d", device->name()->utf8String(), iteration + 1, blas.acceleration_structure_size, compacted_size, blas.build_scratch_size, blas.refit_scratch_size));
	}
}

TEST_CASE_PENDING("[MetalRT][GPU] Builds a one-instance TLAS referencing a triangle BLAS") {
	NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
	NS::SharedPtr<MTL::Device> device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
	if (!device || !device->supportsRaytracing()) {
		MESSAGE("SKIP_REASON=missing_metal_rt_feature");
		return;
	}

	struct TriangleVertex {
		float x;
		float y;
		float z;
	};
	const TriangleVertex vertices[] = {
		{ -1.0f, -1.0f, 0.0f },
		{ 1.0f, -1.0f, 0.0f },
		{ 0.0f, 1.0f, 0.0f },
	};
	NS::SharedPtr<MTL::Buffer> vertex_buffer = NS::TransferPtr(device->newBuffer(vertices, sizeof(vertices), MTL::ResourceStorageModeShared));
	REQUIRE(vertex_buffer);

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

	MTL::AccelerationStructureSizes blas_sizes = device->accelerationStructureSizes(blas_descriptor.get());
	REQUIRE(blas_sizes.accelerationStructureSize > 0);
	REQUIRE(blas_sizes.buildScratchBufferSize > 0);

	NS::SharedPtr<MTL::CommandQueue> queue = NS::TransferPtr(device->newCommandQueue());
	REQUIRE(queue);

	for (uint32_t iteration = 0; iteration < 3; iteration++) {
		MDAccelerationStructure blas(MDAccelerationStructure::Type::BLAS, blas_descriptor, blas_sizes, {});
		REQUIRE(blas.allocate(device.get()));
		NS::SharedPtr<MTL::Buffer> blas_scratch = NS::TransferPtr(device->newBuffer(blas.build_scratch_size, MTL::ResourceStorageModePrivate));
		REQUIRE(blas_scratch);

		NS::SharedPtr<MTL::CommandBuffer> blas_command = NS::RetainPtr(queue->commandBuffer());
		NS::SharedPtr<MTL::AccelerationStructureCommandEncoder> blas_encoder = NS::RetainPtr(blas_command->accelerationStructureCommandEncoder());
		REQUIRE(blas_encoder);
		blas.encode_build(blas_encoder.get(), blas_scratch.get());
		blas_encoder->endEncoding();
		blas_command->commit();
		blas_command->waitUntilCompleted();
		REQUIRE(blas_command->status() == MTL::CommandBufferStatusCompleted);
		CHECK(blas_command->error() == nullptr);

		RDD::AccelerationStructureInstance instance;
		instance.transform.origin = Vector3(0.25 * iteration, -0.5, 1.0);
		instance.id = 0x10203040 + iteration;
		instance.mask = 0xa5;
		instance.hit_sbt_offset = 3;
		instance.flags.set_flag(RDD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT);
		instance.flags.set_flag(RDD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT);
		instance.blas = RDD::AccelerationStructureID(&blas);

		MDAccelerationStructureInstance metal_instance;
		REQUIRE(metal_instance.write(instance));
		NS::SharedPtr<MTL::Buffer> instance_buffer = NS::TransferPtr(device->newBuffer(&metal_instance, sizeof(metal_instance), MTL::ResourceStorageModeShared));
		REQUIRE(instance_buffer);

		NS::SharedPtr<MTL::InstanceAccelerationStructureDescriptor> tlas_descriptor = NS::TransferPtr(MTL::InstanceAccelerationStructureDescriptor::alloc()->init());
		tlas_descriptor->setInstanceCount(1);
		tlas_descriptor->setInstanceDescriptorStride(sizeof(MDAccelerationStructureInstance));
		MTL::AccelerationStructureSizes tlas_sizes = device->accelerationStructureSizes(tlas_descriptor.get());
		REQUIRE(tlas_sizes.accelerationStructureSize > 0);
		REQUIRE(tlas_sizes.buildScratchBufferSize > 0);

		MDAccelerationStructure tlas(MDAccelerationStructure::Type::TLAS, tlas_descriptor, tlas_sizes, {}, 1);
		REQUIRE(tlas.allocate(device.get()));
		REQUIRE(tlas.prepare_tlas_build(instance_buffer.get(), 0, 1));
		CHECK(tlas_descriptor->instanceCount() == 1);
		CHECK(tlas_descriptor->instanceDescriptorBuffer() == instance_buffer.get());
		CHECK(tlas_descriptor->instanceDescriptorBufferOffset() == 0);
		CHECK(tlas_descriptor->instanceDescriptorStride() == sizeof(MDAccelerationStructureInstance));
		REQUIRE(tlas_descriptor->instancedAccelerationStructures());
		CHECK(tlas_descriptor->instancedAccelerationStructures()->count() == 1);
		CHECK(tlas_descriptor->instancedAccelerationStructures()->object(0) == blas.accel.get());

		NS::SharedPtr<MTL::Buffer> tlas_scratch = NS::TransferPtr(device->newBuffer(tlas.build_scratch_size, MTL::ResourceStorageModePrivate));
		REQUIRE(tlas_scratch);
		NS::SharedPtr<MTL::CommandBuffer> tlas_command = NS::RetainPtr(queue->commandBuffer());
		NS::SharedPtr<MTL::AccelerationStructureCommandEncoder> tlas_encoder = NS::RetainPtr(tlas_command->accelerationStructureCommandEncoder());
		REQUIRE(tlas_encoder);
		tlas.encode_build(tlas_encoder.get(), tlas_scratch.get());
		tlas_encoder->endEncoding();
		tlas_command->commit();
		tlas_command->waitUntilCompleted();

		REQUIRE(tlas_command->status() == MTL::CommandBufferStatusCompleted);
		CHECK(tlas_command->error() == nullptr);
		MDAccelerationStructureInstance round_trip;
		memcpy(&round_trip, instance_buffer->contents(), sizeof(round_trip));
		CHECK(round_trip.transformation_matrix[9] == doctest::Approx(float(instance.transform.origin.x)));
		CHECK(round_trip.transformation_matrix[10] == doctest::Approx(float(instance.transform.origin.y)));
		CHECK(round_trip.transformation_matrix[11] == doctest::Approx(float(instance.transform.origin.z)));
		CHECK(round_trip.user_id == instance.id);
		CHECK(round_trip.requested_mask == instance.mask);
		CHECK(round_trip.mask == instance.mask);
		CHECK(round_trip.intersection_function_table_offset == instance.hit_sbt_offset);
		CHECK(round_trip.blas == &blas);
		CHECK(round_trip.acceleration_structure_index == 0);
		print_line(vformat("MetalRT C6 TLAS smoke: device=\"%s\" iteration=%d blas_size=%d tlas_size=%d tlas_scratch_size=%d instance_id=%d mask=%d hit_sbt_offset=%d", device->name()->utf8String(), iteration + 1, blas.acceleration_structure_size, tlas.acceleration_structure_size, tlas.build_scratch_size, round_trip.user_id, round_trip.requested_mask, round_trip.intersection_function_table_offset));
	}
}

} // namespace TestMetalRT

#endif // METAL_ENABLED
