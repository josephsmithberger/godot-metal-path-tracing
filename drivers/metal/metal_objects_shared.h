/**************************************************************************/
/*  metal_objects_shared.h                                                */
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

#include "drivers/metal/metal_device_properties.h"
#include "drivers/metal/metal_utils.h"
#include "drivers/metal/pixel_formats.h"
#include "drivers/metal/sha256_digest.h"

#include <CoreFoundation/CoreFoundation.h>

#include <memory>
#include <optional>

class RenderingDeviceDriverMetal;
class MDAccelerationStructure;
struct MDAccelerationStructureInstance;

using RDC = RenderingDeviceCommons;

enum ShaderStageUsage : uint32_t {
	None = 0,
	Vertex = RDD::SHADER_STAGE_VERTEX_BIT,
	Fragment = RDD::SHADER_STAGE_FRAGMENT_BIT,
	TesselationControl = RDD::SHADER_STAGE_TESSELATION_CONTROL_BIT,
	TesselationEvaluation = RDD::SHADER_STAGE_TESSELATION_EVALUATION_BIT,
	Compute = RDD::SHADER_STAGE_COMPUTE_BIT,
};

_FORCE_INLINE_ ShaderStageUsage &operator|=(ShaderStageUsage &p_a, int p_b) {
	p_a = ShaderStageUsage(uint32_t(p_a) | uint32_t(p_b));
	return p_a;
}

struct ClearAttKey {
	const static uint32_t COLOR_COUNT = MAX_COLOR_ATTACHMENT_COUNT;
	const static uint32_t DEPTH_INDEX = COLOR_COUNT;
	const static uint32_t STENCIL_INDEX = DEPTH_INDEX + 1;
	const static uint32_t ATTACHMENT_COUNT = STENCIL_INDEX + 1;

	enum Flags : uint16_t {
		CLEAR_FLAGS_NONE = 0,
		CLEAR_FLAGS_LAYERED = 1 << 0,
	};

	Flags flags = CLEAR_FLAGS_NONE;
	uint16_t sample_count = 0;
	uint16_t pixel_formats[ATTACHMENT_COUNT] = { 0 };

	_FORCE_INLINE_ void set_color_format(uint32_t p_idx, MTL::PixelFormat p_fmt) { pixel_formats[p_idx] = p_fmt; }
	_FORCE_INLINE_ void set_depth_format(MTL::PixelFormat p_fmt) { pixel_formats[DEPTH_INDEX] = p_fmt; }
	_FORCE_INLINE_ void set_stencil_format(MTL::PixelFormat p_fmt) { pixel_formats[STENCIL_INDEX] = p_fmt; }
	_FORCE_INLINE_ MTL::PixelFormat depth_format() const { return (MTL::PixelFormat)pixel_formats[DEPTH_INDEX]; }
	_FORCE_INLINE_ MTL::PixelFormat stencil_format() const { return (MTL::PixelFormat)pixel_formats[STENCIL_INDEX]; }
	_FORCE_INLINE_ void enable_layered_rendering() { flags::set(flags, CLEAR_FLAGS_LAYERED); }

	_FORCE_INLINE_ bool is_enabled(uint32_t p_idx) const { return pixel_formats[p_idx] != 0; }
	_FORCE_INLINE_ bool is_depth_enabled() const { return pixel_formats[DEPTH_INDEX] != 0; }
	_FORCE_INLINE_ bool is_stencil_enabled() const { return pixel_formats[STENCIL_INDEX] != 0; }
	_FORCE_INLINE_ bool is_layered_rendering_enabled() const { return flags::any(flags, CLEAR_FLAGS_LAYERED); }

	_FORCE_INLINE_ bool operator==(const ClearAttKey &p_rhs) const {
		return memcmp(this, &p_rhs, sizeof(ClearAttKey)) == 0;
	}

	uint32_t hash() const {
		uint32_t h = hash_murmur3_one_32(flags);
		h = hash_murmur3_one_32(sample_count, h);
		h = hash_murmur3_buffer(pixel_formats, ATTACHMENT_COUNT * sizeof(pixel_formats[0]), h);
		return hash_fmix32(h);
	}
};

#pragma mark - Ring Buffer

/// A ring buffer backed by MTLBuffer instances for transient GPU allocations.
/// Allocations are 16-byte aligned with a minimum size of 16 bytes.
/// When the current buffer is exhausted, a new buffer is allocated.
class API_AVAILABLE(macos(11.0), ios(14.0), tvos(14.0), visionos(2.0)) MDRingBuffer {
public:
	static constexpr uint32_t DEFAULT_BUFFER_SIZE = 512 * 1024;
	static constexpr uint32_t MIN_BLOCK_SIZE = 16;
	static constexpr uint32_t ALIGNMENT = 16;

	struct Allocation {
		void *ptr = nullptr;
		MTL::Buffer *buffer = nullptr;
		uint64_t gpu_address = 0;
		uint32_t offset = 0;

		_FORCE_INLINE_ bool is_valid() const { return ptr != nullptr; }
	};

private:
	MTL::Device *device = nullptr;
	LocalVector<MTL::Buffer *> buffers;
	LocalVector<uint32_t> heads;
	uint32_t current_segment = 0;
	uint32_t buffer_size = DEFAULT_BUFFER_SIZE;
	bool changed = false;

	_FORCE_INLINE_ uint32_t alloc_segment() {
		MTL::Buffer *buffer = device->newBuffer(buffer_size, MTL::ResourceStorageModeShared | MTL::ResourceHazardTrackingModeUntracked);
		buffers.push_back(buffer);
		heads.push_back(0);
		changed = true;

		return buffers.size() - 1;
	}

public:
	MDRingBuffer() = default;

	MDRingBuffer(MTL::Device *p_device, uint32_t p_buffer_size = DEFAULT_BUFFER_SIZE) :
			device(p_device), buffer_size(p_buffer_size) {}

	~MDRingBuffer() {
		for (MTL::Buffer *buffer : buffers) {
			buffer->release();
		}
	}

	/// Allocates a block of memory from the ring buffer.
	/// Returns an Allocation with the pointer, buffer, and offset.
	_FORCE_INLINE_ Allocation allocate(uint32_t p_size) {
		p_size = MAX(p_size, MIN_BLOCK_SIZE);
		p_size = (p_size + ALIGNMENT - 1) & ~(ALIGNMENT - 1);

		if (buffers.is_empty()) {
			alloc_segment();
		}

		uint32_t aligned_head = (heads[current_segment] + ALIGNMENT - 1) & ~(ALIGNMENT - 1);

		if (aligned_head + p_size > buffer_size) {
			// Current segment exhausted, try to find one with space or allocate new.
			bool found = false;
			for (uint32_t i = 0; i < buffers.size(); i++) {
				uint32_t ah = (heads[i] + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
				if (ah + p_size <= buffer_size) {
					current_segment = i;
					aligned_head = ah;
					found = true;
					break;
				}
			}

			if (!found) {
				current_segment = alloc_segment();
				aligned_head = 0;
			}
		}

		MTL::Buffer *buffer = buffers[current_segment];
		Allocation alloc;
		alloc.buffer = buffer;
		alloc.offset = aligned_head;
		alloc.ptr = static_cast<uint8_t *>(buffer->contents()) + aligned_head;
		if (__builtin_available(macOS 13.0, iOS 16.0, tvOS 16.0, *)) {
			alloc.gpu_address = buffer->gpuAddress() + aligned_head;
		}
		heads[current_segment] = aligned_head + p_size;

		return alloc;
	}

	/// Resets all segments for reuse. Call at frame boundaries when GPU work is complete.
	_FORCE_INLINE_ void reset() {
		for (uint32_t &head : heads) {
			head = 0;
		}
		current_segment = 0;
	}

	/// Returns true if buffers were added or removed since last clear_changed().
	_FORCE_INLINE_ bool is_changed() const { return changed; }

	/// Clears the changed flag.
	_FORCE_INLINE_ void clear_changed() { changed = false; }

	/// Returns a Span of all backing buffers.
	_FORCE_INLINE_ Span<MTL::Buffer *const> get_buffers() const {
		return Span<MTL::Buffer *const>(buffers.ptr(), buffers.size());
	}

	/// Returns the number of buffer segments currently allocated.
	_FORCE_INLINE_ uint32_t get_segment_count() const {
		return buffers.size();
	}
};

#pragma mark - Resource Factory

class API_AVAILABLE(macos(11.0), ios(14.0), tvos(14.0), visionos(2.0)) MDResourceFactory {
private:
	MTL::Device *device;
	PixelFormats &pixel_formats;
	uint32_t max_buffer_count;

	NS::SharedPtr<MTL::Function> new_func(NS::String *p_source, NS::String *p_name, NS::Error **p_error);
	NS::SharedPtr<MTL::Function> new_clear_vert_func(ClearAttKey &p_key);
	NS::SharedPtr<MTL::Function> new_clear_frag_func(ClearAttKey &p_key);
	const char *get_format_type_string(MTL::PixelFormat p_fmt) const;

	_FORCE_INLINE_ uint32_t get_vertex_buffer_index(uint32_t p_binding) {
		return (max_buffer_count - 1) - p_binding;
	}

public:
	NS::SharedPtr<MTL::RenderPipelineState> new_clear_pipeline_state(ClearAttKey &p_key, NS::Error **p_error);
	NS::SharedPtr<MTL::RenderPipelineState> new_empty_draw_pipeline_state(ClearAttKey &p_key, NS::Error **p_error);
	NS::SharedPtr<MTL::DepthStencilState> new_depth_stencil_state(bool p_use_depth, bool p_use_stencil);

	MDResourceFactory(MTL::Device *p_device, PixelFormats &p_pixel_formats, uint32_t p_max_buffer_count) :
			device(p_device), pixel_formats(p_pixel_formats), max_buffer_count(p_max_buffer_count) {}
	~MDResourceFactory() = default;
};

class API_AVAILABLE(macos(11.0), ios(14.0), tvos(14.0), visionos(2.0)) MDResourceCache {
private:
	typedef HashMap<ClearAttKey, NS::SharedPtr<MTL::RenderPipelineState>> HashMap;
	std::unique_ptr<MDResourceFactory> resource_factory;
	HashMap clear_states;
	HashMap empty_draw_states;

	struct {
		NS::SharedPtr<MTL::DepthStencilState> all;
		NS::SharedPtr<MTL::DepthStencilState> depth_only;
		NS::SharedPtr<MTL::DepthStencilState> stencil_only;
		NS::SharedPtr<MTL::DepthStencilState> none;
	} clear_depth_stencil_state;

public:
	MTL::RenderPipelineState *get_clear_render_pipeline_state(ClearAttKey &p_key, NS::Error **p_error);
	MTL::RenderPipelineState *get_empty_draw_pipeline_state(ClearAttKey &p_key, NS::Error **p_error);
	MTL::DepthStencilState *get_depth_stencil_state(bool p_use_depth, bool p_use_stencil);

	explicit MDResourceCache(MTL::Device *p_device, PixelFormats &p_pixel_formats, uint32_t p_max_buffer_count) :
			resource_factory(new MDResourceFactory(p_device, p_pixel_formats, p_max_buffer_count)) {}
	~MDResourceCache() = default;
};

/**
 * Returns an index that can be used to map a shader stage to an index in a fixed-size array that is used for
 * a single pipeline type.
 */
_FORCE_INLINE_ static uint32_t to_index(RDD::ShaderStage p_s) {
	switch (p_s) {
		case RenderingDeviceCommons::SHADER_STAGE_VERTEX:
		case RenderingDeviceCommons::SHADER_STAGE_TESSELATION_CONTROL:
		case RenderingDeviceCommons::SHADER_STAGE_TESSELATION_EVALUATION:
		case RenderingDeviceCommons::SHADER_STAGE_COMPUTE:
		default:
			return 0;
		case RenderingDeviceCommons::SHADER_STAGE_FRAGMENT:
			return 1;
	}
}

class API_AVAILABLE(macos(11.0), ios(14.0), tvos(14.0), visionos(2.0)) MDFrameBuffer {
	Vector<MTL::Texture *> textures;

public:
	Size2i size;
	MDFrameBuffer(Vector<MTL::Texture *> p_textures, Size2i p_size) :
			textures(p_textures), size(p_size) {}
	MDFrameBuffer() {}

	/// Returns the texture at the given index.
	_ALWAYS_INLINE_ MTL::Texture *get_texture(uint32_t p_idx) const {
		return textures[p_idx];
	}

	/// Returns true if the texture at the given index is not nil.
	_ALWAYS_INLINE_ bool has_texture(uint32_t p_idx) const {
		return textures[p_idx] != nullptr;
	}

	/// Set the texture at the given index.
	_ALWAYS_INLINE_ void set_texture(uint32_t p_idx, MTL::Texture *p_texture) {
		textures.write[p_idx] = p_texture;
	}

	/// Unset or nil the texture at the given index.
	_ALWAYS_INLINE_ void unset_texture(uint32_t p_idx) {
		textures.write[p_idx] = nullptr;
	}

	/// Resizes buffers to the specified size.
	_ALWAYS_INLINE_ void set_texture_count(uint32_t p_size) {
		textures.resize(p_size);
	}

	virtual ~MDFrameBuffer() = default;
};

template <>
struct HashMapComparatorDefault<RDD::ShaderID> {
	static bool compare(const RDD::ShaderID &p_lhs, const RDD::ShaderID &p_rhs) {
		return p_lhs.id == p_rhs.id;
	}
};

template <>
struct HashMapComparatorDefault<RDD::BufferID> {
	static bool compare(const RDD::BufferID &p_lhs, const RDD::BufferID &p_rhs) {
		return p_lhs.id == p_rhs.id;
	}
};

template <>
struct HashMapComparatorDefault<RDD::TextureID> {
	static bool compare(const RDD::TextureID &p_lhs, const RDD::TextureID &p_rhs) {
		return p_lhs.id == p_rhs.id;
	}
};

template <>
struct HashMapHasherDefaultImpl<RDD::BufferID> {
	static _FORCE_INLINE_ uint32_t hash(const RDD::BufferID &p_value) {
		return HashMapHasherDefaultImpl<uint64_t>::hash(p_value.id);
	}
};

template <>
struct HashMapHasherDefaultImpl<RDD::TextureID> {
	static _FORCE_INLINE_ uint32_t hash(const RDD::TextureID &p_value) {
		return HashMapHasherDefaultImpl<uint64_t>::hash(p_value.id);
	}
};

namespace rid {

template <typename T>
_FORCE_INLINE_ T *get(RDD::ID p_id) {
	return reinterpret_cast<T *>(p_id.id);
}

template <typename T>
_FORCE_INLINE_ T *get(uint64_t p_id) {
	return reinterpret_cast<T *>(p_id);
}

} // namespace rid

#pragma mark - Render Pass Types

class MDRenderPass;

enum class MDAttachmentType : uint8_t {
	None = 0,
	Color = 1 << 0,
	Depth = 1 << 1,
	Stencil = 1 << 2,
};

_FORCE_INLINE_ MDAttachmentType &operator|=(MDAttachmentType &p_a, MDAttachmentType p_b) {
	flags::set(p_a, p_b);
	return p_a;
}

_FORCE_INLINE_ bool operator&(MDAttachmentType p_a, MDAttachmentType p_b) {
	return uint8_t(p_a) & uint8_t(p_b);
}

struct MDSubpass {
	uint32_t subpass_index = 0;
	uint32_t view_count = 0;
	LocalVector<RDD::AttachmentReference> input_references;
	LocalVector<RDD::AttachmentReference> color_references;
	RDD::AttachmentReference depth_stencil_reference;
	LocalVector<RDD::AttachmentReference> resolve_references;

	MTLFmtCaps getRequiredFmtCapsForAttachmentAt(uint32_t p_index) const;
};

struct API_AVAILABLE(macos(11.0), ios(14.0), tvos(14.0), visionos(2.0)) MDAttachment {
private:
	uint32_t index = 0;
	uint32_t firstUseSubpassIndex = 0;
	uint32_t lastUseSubpassIndex = 0;

public:
	MTL::PixelFormat format = MTL::PixelFormatInvalid;
	MDAttachmentType type = MDAttachmentType::None;
	MTL::LoadAction loadAction = MTL::LoadActionDontCare;
	MTL::StoreAction storeAction = MTL::StoreActionDontCare;
	MTL::LoadAction stencilLoadAction = MTL::LoadActionDontCare;
	MTL::StoreAction stencilStoreAction = MTL::StoreActionDontCare;
	uint32_t samples = 1;

	/*!
	 * @brief Returns true if this attachment is first used in the given subpass.
	 * @param p_subpass
	 * @return
	 */
	_FORCE_INLINE_ bool isFirstUseOf(MDSubpass const &p_subpass) const {
		return p_subpass.subpass_index == firstUseSubpassIndex;
	}

	/*!
	 * @brief Returns true if this attachment is last used in the given subpass.
	 * @param p_subpass
	 * @return
	 */
	_FORCE_INLINE_ bool isLastUseOf(MDSubpass const &p_subpass) const {
		return p_subpass.subpass_index == lastUseSubpassIndex;
	}

	void linkToSubpass(MDRenderPass const &p_pass);

	MTL::StoreAction getMTLStoreAction(MDSubpass const &p_subpass,
			bool p_is_rendering_entire_area,
			bool p_has_resolve,
			bool p_can_resolve,
			bool p_is_stencil) const;
	bool configureDescriptor(MTL::RenderPassAttachmentDescriptor *p_desc,
			PixelFormats &p_pf,
			MDSubpass const &p_subpass,
			MTL::Texture *p_attachment,
			bool p_is_rendering_entire_area,
			bool p_has_resolve,
			bool p_can_resolve,
			bool p_is_stencil) const {
		p_desc->setTexture(p_attachment);

		MTL::LoadAction load;
		if (!p_is_rendering_entire_area || !isFirstUseOf(p_subpass)) {
			load = MTL::LoadActionLoad;
		} else {
			load = p_is_stencil ? (MTL::LoadAction)stencilLoadAction : (MTL::LoadAction)loadAction;
		}

		p_desc->setLoadAction(load);

		MTL::PixelFormat mtlFmt = p_attachment->pixelFormat();
		bool isDepthFormat = p_pf.isDepthFormat(mtlFmt);
		bool isStencilFormat = p_pf.isStencilFormat(mtlFmt);
		if (isStencilFormat && !p_is_stencil && !isDepthFormat) {
			p_desc->setStoreAction(MTL::StoreActionDontCare);
		} else {
			p_desc->setStoreAction(getMTLStoreAction(p_subpass, p_is_rendering_entire_area, p_has_resolve, p_can_resolve, p_is_stencil));
		}

		return load == MTL::LoadActionClear;
	}

	/** Returns whether this attachment should be cleared in the subpass. */
	bool shouldClear(MDSubpass const &p_subpass, bool p_is_stencil) const;
};

class API_AVAILABLE(macos(11.0), ios(14.0), tvos(14.0), visionos(2.0)) MDRenderPass {
public:
	Vector<MDAttachment> attachments;
	Vector<MDSubpass> subpasses;

	uint32_t get_sample_count() const {
		return attachments.is_empty() ? 1 : attachments[0].samples;
	}

	MDRenderPass(Vector<MDAttachment> &p_attachments, Vector<MDSubpass> &p_subpasses);
};

#pragma mark - Command Buffer Helpers

_FORCE_INLINE_ static MTL::Size MTLSizeFromVector3i(Vector3i p_size) {
	return MTL::Size{ (NS::UInteger)p_size.x, (NS::UInteger)p_size.y, (NS::UInteger)p_size.z };
}

_FORCE_INLINE_ static MTL::Origin MTLOriginFromVector3i(Vector3i p_origin) {
	return MTL::Origin{ (NS::UInteger)p_origin.x, (NS::UInteger)p_origin.y, (NS::UInteger)p_origin.z };
}

// Clamps the size so that the sum of the origin and size do not exceed the maximum size.
_FORCE_INLINE_ static MTL::Size clampMTLSize(MTL::Size p_size, MTL::Origin p_origin, MTL::Size p_max_size) {
	MTL::Size clamped;
	clamped.width = MIN(p_size.width, p_max_size.width - p_origin.x);
	clamped.height = MIN(p_size.height, p_max_size.height - p_origin.y);
	clamped.depth = MIN(p_size.depth, p_max_size.depth - p_origin.z);
	return clamped;
}

API_AVAILABLE(macos(11.0), ios(14.0), tvos(14.0))
_FORCE_INLINE_ static bool isArrayTexture(MTL::TextureType p_type) {
	return (p_type == MTL::TextureType3D ||
			p_type == MTL::TextureType2DArray ||
			p_type == MTL::TextureType2DMultisampleArray ||
			p_type == MTL::TextureType1DArray);
}

_FORCE_INLINE_ static bool operator==(MTL::Size p_a, MTL::Size p_b) {
	return p_a.width == p_b.width && p_a.height == p_b.height && p_a.depth == p_b.depth;
}

#pragma mark - Pipeline Stage Conversion

GODOT_CLANG_WARNING_PUSH_AND_IGNORE("-Wunguarded-availability")

_FORCE_INLINE_ static MTL::Stages convert_src_pipeline_stages_to_metal(BitField<RDD::PipelineStageBits> p_stages) {
	p_stages.clear_flag(RDD::PIPELINE_STAGE_TOP_OF_PIPE_BIT);

	// BOTTOM_OF_PIPE or ALL_COMMANDS means "all prior work must complete".
	if (p_stages & (RDD::PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT | RDD::PIPELINE_STAGE_ALL_COMMANDS_BIT)) {
		return MTL::StageAll;
	}

	MTL::Stages mtlStages = 0;

	// Vertex stage mappings.
	if (p_stages & (RDD::PIPELINE_STAGE_DRAW_INDIRECT_BIT | RDD::PIPELINE_STAGE_VERTEX_INPUT_BIT | RDD::PIPELINE_STAGE_VERTEX_SHADER_BIT | RDD::PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT | RDD::PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT | RDD::PIPELINE_STAGE_GEOMETRY_SHADER_BIT)) {
		mtlStages |= MTL::StageVertex;
	}

	// Fragment stage mappings.
	// Includes resolve and clear_storage, which on Metal use the render pipeline.
	if (p_stages & (RDD::PIPELINE_STAGE_FRAGMENT_SHADER_BIT | RDD::PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | RDD::PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | RDD::PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | RDD::PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT | RDD::PIPELINE_STAGE_FRAGMENT_DENSITY_PROCESS_BIT | RDD::PIPELINE_STAGE_RESOLVE_BIT | RDD::PIPELINE_STAGE_CLEAR_STORAGE_BIT)) {
		mtlStages |= MTL::StageFragment;
	}

	// Compute stage.
	// Raytracing executes through a compute encoder on Metal, so it maps to the
	// dispatch stage as well.
	if (p_stages & (RDD::PIPELINE_STAGE_COMPUTE_SHADER_BIT | RDD::PIPELINE_STAGE_RAY_TRACING_SHADER_BIT)) {
		mtlStages |= MTL::StageDispatch;
	}

	// Acceleration structure builds use a dedicated encoder and stage.
	if (p_stages & RDD::PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT) {
		mtlStages |= MTL::StageAccelerationStructure;
	}

	// Blit stage (transfer operations).
	if (p_stages & RDD::PIPELINE_STAGE_COPY_BIT) {
		mtlStages |= MTL::StageBlit;
	}

	// ALL_GRAPHICS_BIT special case.
	if (p_stages & RDD::PIPELINE_STAGE_ALL_GRAPHICS_BIT) {
		mtlStages |= (MTL::StageVertex | MTL::StageFragment);
	}

	return mtlStages;
}

_FORCE_INLINE_ static MTL::Stages convert_dst_pipeline_stages_to_metal(BitField<RDD::PipelineStageBits> p_stages) {
	p_stages.clear_flag(RDD::PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

	// TOP_OF_PIPE or ALL_COMMANDS means "wait before any work starts".
	if (p_stages & (RDD::PIPELINE_STAGE_ALL_COMMANDS_BIT | RDD::PIPELINE_STAGE_TOP_OF_PIPE_BIT)) {
		return MTL::StageAll;
	}

	MTL::Stages mtlStages = 0;

	// Vertex stage mappings.
	if (p_stages & (RDD::PIPELINE_STAGE_DRAW_INDIRECT_BIT | RDD::PIPELINE_STAGE_VERTEX_INPUT_BIT | RDD::PIPELINE_STAGE_VERTEX_SHADER_BIT | RDD::PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT | RDD::PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT | RDD::PIPELINE_STAGE_GEOMETRY_SHADER_BIT)) {
		mtlStages |= MTL::StageVertex;
	}

	// Fragment stage mappings.
	// Includes resolve and clear_storage, which on Metal use the render pipeline.
	if (p_stages & (RDD::PIPELINE_STAGE_FRAGMENT_SHADER_BIT | RDD::PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | RDD::PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | RDD::PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | RDD::PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT | RDD::PIPELINE_STAGE_FRAGMENT_DENSITY_PROCESS_BIT | RDD::PIPELINE_STAGE_RESOLVE_BIT | RDD::PIPELINE_STAGE_CLEAR_STORAGE_BIT)) {
		mtlStages |= MTL::StageFragment;
	}

	// Compute stage.
	// Raytracing executes through a compute encoder on Metal, so it maps to the
	// dispatch stage as well.
	if (p_stages & (RDD::PIPELINE_STAGE_COMPUTE_SHADER_BIT | RDD::PIPELINE_STAGE_RAY_TRACING_SHADER_BIT)) {
		mtlStages |= MTL::StageDispatch;
	}

	// Acceleration structure builds use a dedicated encoder and stage.
	if (p_stages & RDD::PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT) {
		mtlStages |= MTL::StageAccelerationStructure;
	}

	// Blit stage (transfer operations).
	if (p_stages & RDD::PIPELINE_STAGE_COPY_BIT) {
		mtlStages |= MTL::StageBlit;
	}

	// ALL_GRAPHICS_BIT special case.
	if (p_stages & RDD::PIPELINE_STAGE_ALL_GRAPHICS_BIT) {
		mtlStages |= (MTL::StageVertex | MTL::StageFragment);
	}

	return mtlStages;
}

GODOT_CLANG_WARNING_POP

#pragma mark - Command Buffer Base

enum class MDCommandBufferStateType {
	None,
	Render,
	Compute,
	Blit, // Only used by Metal 3
};

/// Base struct for render state shared between MTL3 and MTL4 implementations.
struct RenderStateBase {
	LocalVector<MTL::Viewport> viewports;
	LocalVector<MTL::ScissorRect> scissors;
	std::optional<Color> blend_constants;

	// clang-format off
	enum DirtyFlag : uint16_t {
		DIRTY_NONE     = 0,
		DIRTY_PIPELINE = 1 << 0,
		DIRTY_UNIFORMS = 1 << 1,
		DIRTY_PUSH     = 1 << 2,
		DIRTY_DEPTH    = 1 << 3,
		DIRTY_VERTEX   = 1 << 4,
		DIRTY_VIEWPORT = 1 << 5,
		DIRTY_SCISSOR  = 1 << 6,
		DIRTY_BLEND    = 1 << 7,
		DIRTY_RASTER   = 1 << 8,
		DIRTY_ALL      = (1 << 9) - 1,
	};
	// clang-format on
	BitField<DirtyFlag> dirty = DIRTY_NONE;
};

/// Abstract base class for Metal command buffers, shared between MTL3 and MTL4 implementations.
class API_AVAILABLE(macos(11.0), ios(14.0), tvos(14.0), visionos(2.0)) MDCommandBufferBase {
	LocalVector<CFTypeRef> _retained_resources;

protected:
	// From RenderingDevice
	static constexpr uint32_t MAX_PUSH_CONSTANT_SIZE = 128;

	MDCommandBufferStateType type = MDCommandBufferStateType::None;

	uint8_t push_constant_data[MAX_PUSH_CONSTANT_SIZE];
	uint32_t push_constant_data_len = 0;
	uint32_t push_constant_binding = UINT32_MAX;

	::RenderingDeviceDriverMetal *device_driver = nullptr;

	void release_resources();

	/// Called when push constants are modified to mark the appropriate dirty flags.
	virtual void mark_push_constants_dirty() = 0;

	/// Returns a reference to the render state base for viewport/scissor/blend operations.
	virtual RenderStateBase &get_render_state_base() = 0;

	/// Returns the view count for the current subpass.
	virtual uint32_t get_current_view_count() const = 0;

	/// Accessors for render pass state.
	virtual MDRenderPass *get_render_pass() const = 0;
	virtual MDFrameBuffer *get_frame_buffer() const = 0;
	virtual const MDSubpass &get_current_subpass() const = 0;
	virtual LocalVector<RDD::RenderPassClearValue> &get_clear_values() = 0;
	virtual const Rect2i &get_render_area() const = 0;
	virtual void end_render_encoding() = 0;

	void _populate_vertices(simd::float4 *p_vertices, Size2i p_fb_size, VectorView<Rect2i> p_rects);
	uint32_t _populate_vertices(simd::float4 *p_vertices, uint32_t p_index, Rect2i const &p_rect, Size2i p_fb_size);
	void _end_render_pass();
	void _render_clear_render_area();

public:
	virtual ~MDCommandBufferBase() { release_resources(); }

	virtual void begin() = 0;
	virtual void commit() = 0;
	virtual void end() = 0;

	virtual void bind_pipeline(RDD::PipelineID p_pipeline) = 0;
	void encode_push_constant_data(RDD::ShaderID p_shader, VectorView<uint32_t> p_data);

	void retain_resource(CFTypeRef p_resource);

#pragma mark - Render Commands

	virtual void render_bind_uniform_sets(VectorView<RDD::UniformSetID> p_uniform_sets, RDD::ShaderID p_shader, uint32_t p_first_set_index, uint32_t p_set_count, uint32_t p_dynamic_offsets) = 0;
	virtual void render_clear_attachments(VectorView<RDD::AttachmentClear> p_attachment_clears, VectorView<Rect2i> p_rects) = 0;
	void render_set_viewport(VectorView<Rect2i> p_viewports);
	void render_set_scissor(VectorView<Rect2i> p_scissors);
	void render_set_blend_constants(const Color &p_constants);
	virtual void render_begin_pass(RDD::RenderPassID p_render_pass,
			RDD::FramebufferID p_frameBuffer,
			RDD::CommandBufferType p_cmd_buffer_type,
			const Rect2i &p_rect,
			VectorView<RDD::RenderPassClearValue> p_clear_values) = 0;
	virtual void render_next_subpass() = 0;
	virtual void render_draw(uint32_t p_vertex_count,
			uint32_t p_instance_count,
			uint32_t p_base_vertex,
			uint32_t p_first_instance) = 0;
	virtual void render_bind_vertex_buffers(uint32_t p_binding_count, const RDD::BufferID *p_buffers, const uint64_t *p_offsets, uint64_t p_dynamic_offsets) = 0;
	virtual void render_bind_index_buffer(RDD::BufferID p_buffer, RDD::IndexBufferFormat p_format, uint64_t p_offset) = 0;

	virtual void render_draw_indexed(uint32_t p_index_count,
			uint32_t p_instance_count,
			uint32_t p_first_index,
			int32_t p_vertex_offset,
			uint32_t p_first_instance) = 0;

	virtual void render_draw_indexed_indirect(RDD::BufferID p_indirect_buffer, uint64_t p_offset, uint32_t p_draw_count, uint32_t p_stride) = 0;
	virtual void render_draw_indexed_indirect_count(RDD::BufferID p_indirect_buffer, uint64_t p_offset, RDD::BufferID p_count_buffer, uint64_t p_count_buffer_offset, uint32_t p_max_draw_count, uint32_t p_stride) = 0;
	virtual void render_draw_indirect(RDD::BufferID p_indirect_buffer, uint64_t p_offset, uint32_t p_draw_count, uint32_t p_stride) = 0;
	virtual void render_draw_indirect_count(RDD::BufferID p_indirect_buffer, uint64_t p_offset, RDD::BufferID p_count_buffer, uint64_t p_count_buffer_offset, uint32_t p_max_draw_count, uint32_t p_stride) = 0;

	virtual void render_end_pass() = 0;

#pragma mark - Compute Commands

	virtual void compute_bind_uniform_sets(VectorView<RDD::UniformSetID> p_uniform_sets, RDD::ShaderID p_shader, uint32_t p_first_set_index, uint32_t p_set_count, uint32_t p_dynamic_offsets) = 0;
	virtual void compute_dispatch(uint32_t p_x_groups, uint32_t p_y_groups, uint32_t p_z_groups) = 0;
	virtual void compute_dispatch_indirect(RDD::BufferID p_indirect_buffer, uint64_t p_offset) = 0;

#pragma mark - Acceleration Structure Commands

	virtual void acceleration_structure_build(MDAccelerationStructure *p_acceleration_structure, MTL::Buffer *p_scratch_buffer) = 0;
	virtual void acceleration_structure_refit(MDAccelerationStructure *p_acceleration_structure, MTL::Buffer *p_scratch_buffer) = 0;

#pragma mark - Raytracing Commands

	/// Dispatches the bound compute-lane raytracing pipeline over a
	/// `p_width` x `p_height` x `p_depth` pixel grid (C10). The kernel is
	/// responsible for bounds-checking because threadgroups round up.
	virtual void trace_rays(uint32_t p_width, uint32_t p_height, uint32_t p_depth) = 0;

#pragma mark - Transfer

	virtual void resolve_texture(RDD::TextureID p_src_texture, RDD::TextureLayout p_src_texture_layout, uint32_t p_src_layer, uint32_t p_src_mipmap, RDD::TextureID p_dst_texture, RDD::TextureLayout p_dst_texture_layout, uint32_t p_dst_layer, uint32_t p_dst_mipmap) = 0;
	virtual void clear_color_texture(RDD::TextureID p_texture, RDD::TextureLayout p_texture_layout, const Color &p_color, const RDD::TextureSubresourceRange &p_subresources) = 0;
	virtual void clear_depth_stencil_texture(RDD::TextureID p_texture, RDD::TextureLayout p_texture_layout, float p_depth, uint8_t p_stencil, const RDD::TextureSubresourceRange &p_subresources) = 0;
	virtual void clear_buffer(RDD::BufferID p_buffer, uint64_t p_offset, uint64_t p_size) = 0;
	virtual void copy_buffer(RDD::BufferID p_src_buffer, RDD::BufferID p_dst_buffer, VectorView<RDD::BufferCopyRegion> p_regions) = 0;
	virtual void copy_texture(RDD::TextureID p_src_texture, RDD::TextureID p_dst_texture, VectorView<RDD::TextureCopyRegion> p_regions) = 0;
	virtual void copy_buffer_to_texture(RDD::BufferID p_src_buffer, RDD::TextureID p_dst_texture, VectorView<RDD::BufferTextureCopyRegion> p_regions) = 0;
	virtual void copy_texture_to_buffer(RDD::TextureID p_src_texture, RDD::BufferID p_dst_buffer, VectorView<RDD::BufferTextureCopyRegion> p_regions) = 0;

#pragma mark - Synchronization

	virtual void pipeline_barrier(BitField<RDD::PipelineStageBits> p_src_stages,
			BitField<RDD::PipelineStageBits> p_dst_stages,
			VectorView<RDD::MemoryAccessBarrier> p_memory_barriers,
			VectorView<RDD::BufferBarrier> p_buffer_barriers,
			VectorView<RDD::TextureBarrier> p_texture_barriers,
			VectorView<RDD::AccelerationStructureBarrier> p_acceleration_structure_barriers) = 0;

#pragma mark - Debugging

	virtual void begin_label(const char *p_label_name, const Color &p_color) = 0;
	virtual void end_label() = 0;
};

#pragma mark - Uniform Types

struct API_AVAILABLE(macos(11.0), ios(14.0), tvos(14.0), visionos(2.0)) UniformInfo {
	uint32_t binding;
	BitField<RDD::ShaderStage> active_stages;
	MTL::DataType dataType = MTL::DataTypeNone;
	MTL::BindingAccess access = MTL::BindingAccessReadOnly;
	MTL::ResourceUsage usage = 0;
	MTL::TextureType textureType = MTL::TextureType2D;
	uint32_t imageFormat = 0;
	uint32_t arrayLength = 0;
	bool isMultisampled = false;

	struct Indexes {
		uint32_t buffer = UINT32_MAX;
		uint32_t texture = UINT32_MAX;
		uint32_t sampler = UINT32_MAX;
	};
	Indexes slot;
	Indexes arg_buffer;

	enum class IndexType {
		SLOT,
		ARG,
	};

	_FORCE_INLINE_ Indexes &get_indexes(IndexType p_type) {
		switch (p_type) {
			case IndexType::SLOT:
				return slot;
			case IndexType::ARG:
				return arg_buffer;
		}
	}
};

struct API_AVAILABLE(macos(11.0), ios(14.0), tvos(14.0), visionos(2.0)) UniformSet {
	LocalVector<UniformInfo> uniforms;
	LocalVector<uint32_t> dynamic_uniforms;
	uint32_t buffer_size = 0;
	/// True when the set's trailing binding is an unbounded (runtime-sized)
	/// array; the argument buffer is then sized per uniform set, from the
	/// actual descriptor count, instead of `buffer_size` alone.
	bool has_unbounded_array = false;

	_FORCE_INLINE_ uint32_t argument_buffer_size(VectorView<RDD::BoundUniform> p_uniforms) const {
		if (!has_unbounded_array) {
			return buffer_size;
		}

		DEV_ASSERT(uniforms.size() == p_uniforms.size());
		uint32_t size = buffer_size;
		for (uint32_t i = 0; i < p_uniforms.size(); i++) {
			const UniformInfo &uniform = uniforms[i];
			if (uniform.arrayLength != UINT32_MAX) {
				continue;
			}
			uint32_t descriptor_count = MAX(p_uniforms[i].ids.size(), 1u);
			size = MAX(size, (uniform.arg_buffer.texture + descriptor_count) * (uint32_t)sizeof(uint64_t));
		}
		return size;
	}
};

class API_AVAILABLE(macos(11.0), ios(14.0), tvos(14.0), visionos(2.0)) DynamicOffsetLayout {
	struct Data {
		uint8_t offset : 4;
		uint8_t count : 4;
	};

	union {
		Data data[MAX_DYNAMIC_BUFFERS];
		uint64_t _val = 0;
	};

public:
	_FORCE_INLINE_ bool is_empty() const { return _val == 0; }

	_FORCE_INLINE_ uint32_t get_count(uint32_t p_set_index) const {
		return data[p_set_index].count;
	}

	_FORCE_INLINE_ uint32_t get_offset(uint32_t p_set_index) const {
		return data[p_set_index].offset;
	}

	_FORCE_INLINE_ void set_offset_count(uint32_t p_set_index, uint8_t p_offset, uint8_t p_count) {
		data[p_set_index].offset = p_offset;
		data[p_set_index].count = p_count;
	}

	_FORCE_INLINE_ uint32_t get_offset_index_shift(uint32_t p_set_index, uint32_t p_dynamic_index = 0) const {
		return (data[p_set_index].offset + p_dynamic_index) * 4u;
	}
};

#pragma mark - Shader Types

class MDLibrary; // Forward declaration for C++ code
struct ShaderCacheEntry; // Forward declaration for C++ code

enum class ShaderLoadStrategy {
	IMMEDIATE,
	LAZY,

	/// The default strategy is to load the shader immediately.
	DEFAULT = IMMEDIATE,
};

/// A Metal shader library.
class MDLibrary : public std::enable_shared_from_this<MDLibrary> {
protected:
	ShaderCacheEntry *_entry = nullptr;
#ifdef DEV_ENABLED
	NS::SharedPtr<NS::String> _original_source = nullptr;
#endif

	MDLibrary(ShaderCacheEntry *p_entry
#ifdef DEV_ENABLED
			,
			NS::String *p_source
#endif
	);

public:
	virtual ~MDLibrary();

	virtual MTL::Library *get_library() = 0;
	virtual NS::Error *get_error() = 0;
	virtual void set_label(NS::String *p_label);
#ifdef DEV_ENABLED
	NS::String *get_original_source() const { return _original_source.get(); }
#endif

	static std::shared_ptr<MDLibrary> create(ShaderCacheEntry *p_entry,
			MTL::Device *p_device,
			NS::String *p_source,
			MTL::CompileOptions *p_options,
			ShaderLoadStrategy p_strategy);

	static std::shared_ptr<MDLibrary> create(ShaderCacheEntry *p_entry,
			MTL::Device *p_device,
#ifdef DEV_ENABLED
			NS::String *p_source,
#endif
			dispatch_data_t p_data);
};

/// A cache entry for a Metal shader library.
struct ShaderCacheEntry {
	RenderingDeviceDriverMetal &owner;
	/// A hash of the Metal shader source code.
	SHA256Digest key;
	CharString name;
	RDC::ShaderStage stage = RDC::SHADER_STAGE_VERTEX;
	/// Weak reference to the library; allows cache lookup without preventing cleanup.
	std::weak_ptr<MDLibrary> library;

	/// Notify the cache that this entry is no longer needed.
	void notify_free() const;

	ShaderCacheEntry(RenderingDeviceDriverMetal &p_owner, SHA256Digest p_key) :
			owner(p_owner), key(p_key) {
	}
	~ShaderCacheEntry() = default;
};

class API_AVAILABLE(macos(11.0), ios(14.0), tvos(14.0), visionos(2.0)) MDShader {
public:
	CharString name;
	Vector<UniformSet> sets;
	struct {
		BitField<RDD::ShaderStage> stages = {};
		uint32_t binding = UINT32_MAX;
		uint32_t size = 0;
	} push_constants;
	DynamicOffsetLayout dynamic_offset_layout;
	bool uses_argument_buffers = true;

	MDShader(CharString p_name, Vector<UniformSet> p_sets, bool p_uses_argument_buffers) :
			name(p_name), sets(p_sets), uses_argument_buffers(p_uses_argument_buffers) {}
	virtual ~MDShader() = default;
};

class API_AVAILABLE(macos(11.0), ios(14.0), tvos(14.0), visionos(2.0)) MDComputeShader final : public MDShader {
public:
	MTL::Size local = {};

	std::shared_ptr<MDLibrary> kernel;

	MDComputeShader(CharString p_name, Vector<UniformSet> p_sets, bool p_uses_argument_buffers, std::shared_ptr<MDLibrary> p_kernel);
};

class API_AVAILABLE(macos(11.0), ios(14.0), tvos(14.0), visionos(2.0)) MDRenderShader final : public MDShader {
public:
	bool needs_view_mask_buffer = false;

	std::shared_ptr<MDLibrary> vert;
	std::shared_ptr<MDLibrary> frag;

	MDRenderShader(CharString p_name,
			Vector<UniformSet> p_sets,
			bool p_needs_view_mask_buffer,
			bool p_uses_argument_buffers,
			std::shared_ptr<MDLibrary> p_vert, std::shared_ptr<MDLibrary> p_frag);
};

#pragma mark - Uniform Set

enum StageResourceUsage : uint32_t {
	ResourceUnused = 0,
	VertexRead = (MTL::ResourceUsageRead << RDD::SHADER_STAGE_VERTEX * 2),
	VertexWrite = (MTL::ResourceUsageWrite << RDD::SHADER_STAGE_VERTEX * 2),
	FragmentRead = (MTL::ResourceUsageRead << RDD::SHADER_STAGE_FRAGMENT * 2),
	FragmentWrite = (MTL::ResourceUsageWrite << RDD::SHADER_STAGE_FRAGMENT * 2),
	TesselationControlRead = (MTL::ResourceUsageRead << RDD::SHADER_STAGE_TESSELATION_CONTROL * 2),
	TesselationControlWrite = (MTL::ResourceUsageWrite << RDD::SHADER_STAGE_TESSELATION_CONTROL * 2),
	TesselationEvaluationRead = (MTL::ResourceUsageRead << RDD::SHADER_STAGE_TESSELATION_EVALUATION * 2),
	TesselationEvaluationWrite = (MTL::ResourceUsageWrite << RDD::SHADER_STAGE_TESSELATION_EVALUATION * 2),
	ComputeRead = (MTL::ResourceUsageRead << RDD::SHADER_STAGE_COMPUTE * 2),
	ComputeWrite = (MTL::ResourceUsageWrite << RDD::SHADER_STAGE_COMPUTE * 2),
};

typedef LocalVector<MTL::Resource *> ResourceVector;
typedef HashMap<StageResourceUsage, ResourceVector> ResourceUsageMap;

_FORCE_INLINE_ StageResourceUsage &operator|=(StageResourceUsage &p_a, uint32_t p_b) {
	p_a = StageResourceUsage(uint32_t(p_a) | p_b);
	return p_a;
}

_FORCE_INLINE_ StageResourceUsage stage_resource_usage(RDC::ShaderStage p_stage, MTL::ResourceUsage p_usage) {
	return StageResourceUsage(p_usage << (p_stage * 2));
}

_FORCE_INLINE_ MTL::ResourceUsage resource_usage_for_stage(StageResourceUsage p_usage, RDC::ShaderStage p_stage) {
	return MTL::ResourceUsage((p_usage >> (p_stage * 2)) & 0b11);
}

class API_AVAILABLE(macos(11.0), ios(14.0), tvos(14.0), visionos(2.0)) MDUniformSet {
public:
	NS::SharedPtr<MTL::Buffer> arg_buffer;
	Vector<uint8_t> arg_buffer_data; // Stored for dynamic uniform sets.
	ResourceUsageMap usage_to_resources; // Used by Metal 3 for resource tracking.
	Vector<RDD::BoundUniform> uniforms;
};

#pragma mark - Pipeline Types

enum class MDPipelineType {
	None,
	Render,
	Compute,
	Raytracing,
};

class API_AVAILABLE(macos(11.0), ios(14.0), tvos(14.0), visionos(2.0)) MDPipeline {
public:
	MDPipelineType type;

	virtual MTL::ComputePipelineState *get_compute_pipeline_state() const { return nullptr; }
	virtual MDShader *get_compute_shader() const { return nullptr; }
	virtual MTL::Size get_threads_per_threadgroup() const { return {}; }

	explicit MDPipeline(MDPipelineType p_type) :
			type(p_type) {}
	virtual ~MDPipeline() = default;
};

class API_AVAILABLE(macos(11.0), ios(14.0), tvos(14.0), visionos(2.0)) MDRenderPipeline final : public MDPipeline {
public:
	NS::SharedPtr<MTL::RenderPipelineState> state;
	NS::SharedPtr<MTL::DepthStencilState> depth_stencil;
	uint32_t push_constant_size = 0;
	uint32_t push_constant_stages_mask = 0;
	SampleCount sample_count = SampleCount1;

	struct {
		MTL::CullMode cull_mode = MTL::CullModeNone;
		MTL::TriangleFillMode fill_mode = MTL::TriangleFillModeFill;
		MTL::DepthClipMode clip_mode = MTL::DepthClipModeClip;
		MTL::Winding winding = MTL::WindingClockwise;
		MTL::PrimitiveType render_primitive = MTL::PrimitiveTypePoint;

		struct {
			bool enabled = false;
		} depth_test;

		struct {
			bool enabled = false;
			float depth_bias = 0.0;
			float slope_scale = 0.0;
			float clamp = 0.0;

			template <typename T>
			_FORCE_INLINE_ void apply(T *p_enc) const {
				if (!enabled) {
					return;
				}
				p_enc->setDepthBias(depth_bias, slope_scale, clamp);
			}
		} depth_bias;

		struct {
			bool enabled = false;
			uint32_t front_reference = 0;
			uint32_t back_reference = 0;

			template <typename T>
			_FORCE_INLINE_ void apply(T *p_enc) const {
				if (!enabled) {
					return;
				}
				p_enc->setStencilReferenceValues(front_reference, back_reference);
			}
		} stencil;

		struct {
			bool enabled = false;
			float r = 0.0;
			float g = 0.0;
			float b = 0.0;
			float a = 0.0;

			template <typename T>
			_FORCE_INLINE_ void apply(T *p_enc) const {
				p_enc->setBlendColor(r, g, b, a);
			}
		} blend;

		template <typename T>
		_FORCE_INLINE_ void apply(T *p_enc) const {
			p_enc->setCullMode(cull_mode);
			p_enc->setTriangleFillMode(fill_mode);
			p_enc->setDepthClipMode(clip_mode);
			p_enc->setFrontFacingWinding(winding);
			depth_bias.apply(p_enc);
			stencil.apply(p_enc);
			blend.apply(p_enc);
		}

	} raster_state;

	MDRenderShader *shader = nullptr;

	MDRenderPipeline() :
			MDPipeline(MDPipelineType::Render) {}
	~MDRenderPipeline() final = default;
};

class API_AVAILABLE(macos(11.0), ios(14.0), tvos(14.0), visionos(2.0)) MDComputePipeline final : public MDPipeline {
public:
	NS::SharedPtr<MTL::ComputePipelineState> state;
	struct {
		MTL::Size local = {};
	} compute_state;

	MDComputeShader *shader = nullptr;

	explicit MDComputePipeline(NS::SharedPtr<MTL::ComputePipelineState> p_state) :
			MDPipeline(MDPipelineType::Compute), state(std::move(p_state)) {}

	MTL::ComputePipelineState *get_compute_pipeline_state() const final { return state.get(); }
	MDShader *get_compute_shader() const final { return shader; }
	MTL::Size get_threads_per_threadgroup() const final { return compute_state.local; }
	~MDComputePipeline() final = default;
};

/*! A compute-backed ray-tracing pipeline and Godot shader-group translation.
 *
 * Metal has no dedicated ray-tracing pipeline object; tracing runs as a compute
 * dispatch whose kernel uses a ray query or the MSL intersector. Godot's Vulkan-
 * shaped shader groups are retained as small, deterministic records. Triangle
 * groups share Metal's system opaque-triangle intersection function; procedural
 * groups reserve stable table slots for the compute lowering supplied by C10.
 */
class API_AVAILABLE(macos(11.0), ios(14.0), tvos(14.0), visionos(2.0)) MDRaytracingPipeline final : public MDPipeline {
public:
	enum class ShaderGroupType : uint32_t {
		RAYGEN,
		MISS,
		TRIANGLE_HIT,
		PROCEDURAL_HIT,
		EMPTY_HIT,
	};

	enum class IntersectionFunctionType : uint32_t {
		OPAQUE_TRIANGLE,
		PROCEDURAL,
	};

	struct ShaderGroup {
		ShaderGroupType type = ShaderGroupType::EMPTY_HIT;
		uint32_t general_shader_index = UINT32_MAX;
		uint32_t closest_hit_shader_index = UINT32_MAX;
		uint32_t any_hit_shader_index = UINT32_MAX;
		uint32_t intersection_shader_index = UINT32_MAX;
		uint32_t intersection_function_table_index = UINT32_MAX;
	};

	struct IntersectionFunction {
		IntersectionFunctionType type = IntersectionFunctionType::OPAQUE_TRIANGLE;
		uint32_t shader_index = UINT32_MAX;
	};

	// Metal has no opaque Vulkan-style shader-group handle. These records are
	// copied into Godot's compatibility SBT buffers and consumed as stable table
	// indices by the compute lowering.
	struct ShaderGroupHandle {
		static constexpr uint32_t MAGIC = 0x4d525447; // "MRTG".
		uint32_t magic = MAGIC;
		uint32_t group_index = UINT32_MAX;
		uint32_t intersection_function_table_index = UINT32_MAX;
		ShaderGroupType type = ShaderGroupType::EMPTY_HIT;
	};

	static constexpr uint32_t SHADER_GROUP_HANDLE_SIZE = sizeof(ShaderGroupHandle);
	static constexpr uint32_t SHADER_GROUP_HANDLE_ALIGNMENT = alignof(ShaderGroupHandle);
	static constexpr uint32_t SHADER_GROUP_BASE_ALIGNMENT = 16;

	static constexpr uint32_t TRACE_PIXEL_SIZE_BYTES = 4;
	static constexpr uint32_t TRACE_TLAS_BUFFER_INDEX = 0;
	static constexpr uint32_t TRACE_OUTPUT_BUFFER_INDEX = 1;
	static constexpr uint32_t TRACE_CONSTANTS_BUFFER_INDEX = 2;
	static constexpr uint32_t TRACE_INTERSECTION_TABLE_BUFFER_INDEX = 3;

	/// Compute pipeline that hosts the trace kernel (raygen equivalent).
	NS::SharedPtr<MTL::ComputePipelineState> state;
	/// Pipeline-specific table. C8 installs Metal's opaque-triangle function at
	/// index zero; custom procedural intersection functions remain a later step.
	NS::SharedPtr<MTL::IntersectionFunctionTable> intersection_function_table;
	uint32_t intersection_function_count = 0;

	Vector<ShaderGroup> shader_groups;
	Vector<IntersectionFunction> intersection_functions;
	uint32_t raygen_group_count = 0;
	uint32_t miss_group_count = 0;
	uint32_t hit_group_count = 0;
	// Metal has no pipeline recursion limit. This is the software recursion
	// budget which the compute-lane kernel must enforce explicitly.
	uint32_t max_trace_recursion_depth = 0;
	// True when the ray-generation group is a re-expressed ray-query compute
	// kernel supplied by the engine (C10) rather than an RT-pipeline stage.
	bool uses_compute_lane = false;
	MDShader *shader = nullptr;
	MTL::Size threads_per_threadgroup = MTL::Size(8, 8, 1);

	bool configure_shader_groups(VectorView<RDD::PipelineShader> p_shaders, VectorView<uint32_t> p_raygen_shader_indices, VectorView<uint32_t> p_miss_shader_indices, VectorView<RDD::HitGroup> p_hit_groups, uint32_t p_max_trace_recursion_depth, String *r_error = nullptr);
	bool get_shader_group_handles(uint32_t p_group_index_offset, VectorView<uint32_t> p_group_indices, uint8_t *r_data, uint32_t p_data_stride_bytes, String *r_error = nullptr) const;

	/// Creates the backend-owned C8 kernel and its intersection-function table.
	bool create_trace_one_ray(MTL::Device *p_device, String *r_error = nullptr);
	/// Encodes a 2D image dispatch. Each output pixel is four bytes (RGBA8).
	bool encode_trace_one_ray(MTL::ComputeCommandEncoder *p_encoder, MTL::AccelerationStructure *p_tlas, MTL::Buffer *p_output_buffer, uint32_t p_width, uint32_t p_height) const;

	/// Creates the compute pipeline state for a C10 compute-lane kernel. The
	/// function is the engine's re-expressed ray-query compute entry point and
	/// `p_local` its reflected workgroup size. Ray-query kernels use no
	/// intersection-function table.
	bool create_compute_lane(MTL::Device *p_device, MTL::Function *p_function, MTL::Size p_local, String *r_error = nullptr);

	bool is_valid() const {
		if (uses_compute_lane) {
			return state.get() != nullptr;
		}
		return state && intersection_function_table && intersection_function_count > 0;
	}

	MTL::ComputePipelineState *get_compute_pipeline_state() const final { return state.get(); }
	MDShader *get_compute_shader() const final { return shader; }
	MTL::Size get_threads_per_threadgroup() const final { return threads_per_threadgroup; }

	MDRaytracingPipeline() :
			MDPipeline(MDPipelineType::Raytracing) {}
	~MDRaytracingPipeline() final = default;
};

static_assert(sizeof(MDRaytracingPipeline::ShaderGroupHandle) == 16, "Metal RT shader-group handles must remain stable 16-byte records.");

#pragma mark - Acceleration Structures

/*! Backend state for one Godot acceleration structure (BLAS or TLAS). */
class API_AVAILABLE(macos(11.0), ios(14.0), tvos(14.0), visionos(2.0)) MDAccelerationStructure {
public:
	enum class Type : uint8_t {
		BLAS,
		TLAS,
	};

	Type type;
	/// A `PrimitiveAccelerationStructureDescriptor` for a BLAS or an
	/// `InstanceAccelerationStructureDescriptor` for a TLAS. Retains any geometry
	/// buffers it references.
	NS::SharedPtr<MTL::AccelerationStructureDescriptor> descriptor;
	/// Native acceleration structure.
	NS::SharedPtr<MTL::AccelerationStructure> accel;
	/// Bytes required for the native acceleration-structure allocation.
	uint64_t acceleration_structure_size = 0;
	/// Scratch bytes required for a clean build.
	uint64_t build_scratch_size = 0;
	/// Scratch bytes required for an in-place refit.
	uint64_t refit_scratch_size = 0;
	/// Scratch bytes required to build (and refit, when allowed) this structure.
	uint64_t scratch_size = 0;
	/// Shared result buffer populated after builds that request compaction.
	NS::SharedPtr<MTL::Buffer> compacted_size_buffer;
	BitField<RDD::AccelerationStructureFlagBits> flags = {};
	/// True after a build has been encoded, allowing a later in-place refit.
	bool build_encoded = false;

	// TLAS only.
	uint32_t max_instance_count = 0;
	/// TLAS only: unique primitive structures referenced by the last prepared
	/// build. Metal requires them to be made resident (useResource) on any
	/// encoder that intersects this TLAS; the descriptor's BLAS array retains
	/// the objects these raw pointers reference.
	LocalVector<MTL::AccelerationStructure *> resident_blases;

	static MTL::AccelerationStructureUsage usage_from_flags(BitField<RDD::AccelerationStructureFlagBits> p_flags) {
		MTL::AccelerationStructureUsage usage = MTL::AccelerationStructureUsageNone;
		if (p_flags.has_flag(RDD::ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT)) {
			usage |= MTL::AccelerationStructureUsageRefit;
		}
		if (p_flags.has_flag(RDD::ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT)) {
			usage |= MTL::AccelerationStructureUsagePreferFastBuild;
		}
		return usage;
	}

	static uint64_t required_scratch_size(const MTL::AccelerationStructureSizes &p_sizes, BitField<RDD::AccelerationStructureFlagBits> p_flags) {
		uint64_t size = p_sizes.buildScratchBufferSize;
		if (p_flags.has_flag(RDD::ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT)) {
			size = MAX(size, p_sizes.refitScratchBufferSize);
		}
		return size;
	}

	bool allocate(MTL::Device *p_device) {
		accel = NS::TransferPtr(p_device->newAccelerationStructure(acceleration_structure_size));
		if (!accel) {
			return false;
		}

		if (flags.has_flag(RDD::ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT)) {
			compacted_size_buffer = NS::TransferPtr(p_device->newBuffer(sizeof(uint64_t), MTL::ResourceStorageModeShared));
			if (!compacted_size_buffer) {
				accel.reset();
				return false;
			}
			*static_cast<uint64_t *>(compacted_size_buffer->contents()) = 0;
		}

		return true;
	}

	void encode_build(MTL::AccelerationStructureCommandEncoder *p_encoder, MTL::Buffer *p_scratch_buffer) {
		p_encoder->buildAccelerationStructure(accel.get(), descriptor.get(), p_scratch_buffer, 0);
		if (compacted_size_buffer) {
			p_encoder->writeCompactedAccelerationStructureSize(accel.get(), compacted_size_buffer.get(), 0);
		}
		build_encoded = true;
	}

	void encode_refit(MTL::AccelerationStructureCommandEncoder *p_encoder, MTL::Buffer *p_scratch_buffer) {
		p_encoder->refitAccelerationStructure(accel.get(), descriptor.get(), accel.get(), p_scratch_buffer, 0);
	}

	/// Configures a TLAS descriptor from Godot's persistently mapped instance
	/// records. The first 64 bytes of each record are consumed directly by Metal;
	/// the remaining metadata resolves Godot BLAS handles to descriptor-array
	/// indices. Returns false when the record range or a referenced BLAS is invalid.
	bool prepare_tlas_build(MTL::Buffer *p_instance_buffer, uint32_t p_instance_offset, uint32_t p_instance_count);

	/// Returns zero until a compaction-size-enabled build has completed.
	uint64_t get_compacted_size() const {
		if (!compacted_size_buffer || !compacted_size_buffer->contents()) {
			return 0;
		}
		return *static_cast<const uint64_t *>(compacted_size_buffer->contents());
	}

	MDAccelerationStructure(Type p_type, NS::SharedPtr<MTL::AccelerationStructureDescriptor> p_descriptor, const MTL::AccelerationStructureSizes &p_sizes, BitField<RDD::AccelerationStructureFlagBits> p_flags, uint32_t p_max_instance_count = 0) :
			type(p_type),
			descriptor(std::move(p_descriptor)),
			acceleration_structure_size(p_sizes.accelerationStructureSize),
			build_scratch_size(p_sizes.buildScratchBufferSize),
			refit_scratch_size(p_sizes.refitScratchBufferSize),
			scratch_size(required_scratch_size(p_sizes, p_flags)),
			flags(p_flags),
			max_instance_count(p_max_instance_count) {}
};

/*! CPU-written instance record used by the Metal TLAS build path.
 *
 * Metal's macOS 11 instance descriptor identifies a BLAS by an index into an
 * NSArray supplied on the TLAS descriptor. Godot instead gives the instance
 * writer a backend BLAS handle before a particular TLAS is known. The native
 * descriptor occupies the leading bytes and backend-only metadata follows it.
 * A 128-byte stride keeps every RenderingDevice suballocation aligned to
 * Metal's required 64-byte instanceDescriptorBufferOffset.
 *
 * The record's native prefix is the 68-byte UserID descriptor
 * (`MTL::AccelerationStructureUserIDInstanceDescriptor`), whose first 64 bytes
 * are identical to the default descriptor. `user_id` carries Godot's instance
 * custom index so the C10 ray-query compute lane can read it through
 * `rayQueryGetIntersectionInstanceCustomIndexEXT` (SPIRV-Cross lowers it to
 * MSL `user_instance_id`). Consuming the field requires the TLAS descriptor
 * type to be UserID, which `tlas_create()` selects on macOS 12+; at the
 * macOS 11 floor Metal reads only the default 64-byte prefix and the field
 * stays inert metadata.
 */
struct MDAccelerationStructureInstance {
	// Binary-compatible prefix with MTL::AccelerationStructureUserIDInstanceDescriptor
	// (and, for the first 64 bytes, MTL::AccelerationStructureInstanceDescriptor).
	float transformation_matrix[12] = {};
	uint32_t options = 0;
	uint32_t mask = 0;
	uint32_t intersection_function_table_offset = 0;
	uint32_t acceleration_structure_index = 0;
	uint32_t user_id = 0;

	// Godot-to-Metal build metadata, ignored by Metal because of the stride.
	uint32_t requested_mask = 0;
	MDAccelerationStructure *blas = nullptr;
	uint32_t reserved[12] = {};

	bool write(const RDD::AccelerationStructureInstance &p_instance) {
		constexpr uint32_t valid_options =
				RDD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT |
				RDD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT |
				RDD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT |
				RDD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_NO_OPAQUE_BIT;

		const uint32_t instance_options = static_cast<uint32_t>(p_instance.flags);
		if (!p_instance.transform.is_finite() || (instance_options & ~valid_options) != 0) {
			return false;
		}
		if ((instance_options & RDD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT) != 0 &&
				(instance_options & RDD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_NO_OPAQUE_BIT) != 0) {
			return false;
		}

		MDAccelerationStructure *blas_info = reinterpret_cast<MDAccelerationStructure *>(p_instance.blas.id);
		if (blas_info != nullptr && blas_info->type != MDAccelerationStructure::Type::BLAS) {
			return false;
		}

		float packed_transform[12] = {};
		for (uint32_t column = 0; column < 3; column++) {
			for (uint32_t row = 0; row < 3; row++) {
				const float component = p_instance.transform.basis.rows[row][column];
				if (!Math::is_finite(component)) {
					return false;
				}
				packed_transform[column * 3 + row] = component;
			}
		}
		for (uint32_t row = 0; row < 3; row++) {
			const float component = p_instance.transform.origin[row];
			if (!Math::is_finite(component)) {
				return false;
			}
			packed_transform[9 + row] = component;
		}

		*this = MDAccelerationStructureInstance();
		memcpy(transformation_matrix, packed_transform, sizeof(packed_transform));
		options = instance_options;
		mask = blas_info != nullptr ? p_instance.mask : 0;
		intersection_function_table_offset = p_instance.hit_sbt_offset;
		user_id = p_instance.id;
		blas = blas_info;
		requested_mask = p_instance.mask;
		return true;
	}
};

static_assert(sizeof(MTL::AccelerationStructureInstanceDescriptor) == 64, "Unexpected native Metal instance descriptor size.");
static_assert(sizeof(MTL::AccelerationStructureUserIDInstanceDescriptor) == 68, "Unexpected native Metal user-ID instance descriptor size.");
static_assert(offsetof(MDAccelerationStructureInstance, options) == offsetof(MTL::AccelerationStructureUserIDInstanceDescriptor, options));
static_assert(offsetof(MDAccelerationStructureInstance, mask) == offsetof(MTL::AccelerationStructureUserIDInstanceDescriptor, mask));
static_assert(offsetof(MDAccelerationStructureInstance, intersection_function_table_offset) == offsetof(MTL::AccelerationStructureUserIDInstanceDescriptor, intersectionFunctionTableOffset));
static_assert(offsetof(MDAccelerationStructureInstance, acceleration_structure_index) == offsetof(MTL::AccelerationStructureUserIDInstanceDescriptor, accelerationStructureIndex));
static_assert(offsetof(MDAccelerationStructureInstance, user_id) == offsetof(MTL::AccelerationStructureUserIDInstanceDescriptor, userID));
static_assert(sizeof(MDAccelerationStructureInstance) == 128, "Metal TLAS instance records must preserve 64-byte suballocation alignment.");

inline bool MDAccelerationStructure::prepare_tlas_build(MTL::Buffer *p_instance_buffer, uint32_t p_instance_offset, uint32_t p_instance_count) {
	if (type != Type::TLAS || p_instance_buffer == nullptr || p_instance_buffer->contents() == nullptr || p_instance_count > max_instance_count) {
		return false;
	}
	if ((p_instance_offset % 64) != 0) {
		return false;
	}

	const uint64_t records_size = uint64_t(p_instance_count) * sizeof(MDAccelerationStructureInstance);
	if (p_instance_offset > p_instance_buffer->length() || records_size > p_instance_buffer->length() - p_instance_offset) {
		return false;
	}

	LocalVector<MDAccelerationStructureInstance> instances;
	instances.resize(p_instance_count);
	const uint8_t *instance_bytes = static_cast<const uint8_t *>(p_instance_buffer->contents()) + p_instance_offset;
	for (uint32_t i = 0; i < p_instance_count; i++) {
		memcpy(&instances[i], instance_bytes + (i * sizeof(MDAccelerationStructureInstance)), sizeof(MDAccelerationStructureInstance));
		MDAccelerationStructure *blas_info = instances[i].blas;
		if (instances[i].requested_mask > UINT8_MAX ||
				(blas_info != nullptr && (blas_info->type != Type::BLAS || !blas_info->accel || !blas_info->build_encoded))) {
			return false;
		}
	}

	MDAccelerationStructure *fallback_blas = nullptr;
	for (const MDAccelerationStructureInstance &instance : instances) {
		if (instance.blas != nullptr) {
			fallback_blas = instance.blas;
			break;
		}
	}

	MTL::InstanceAccelerationStructureDescriptor *tlas_descriptor = static_cast<MTL::InstanceAccelerationStructureDescriptor *>(descriptor.get());
	tlas_descriptor->setInstanceDescriptorBuffer(p_instance_buffer);
	tlas_descriptor->setInstanceDescriptorBufferOffset(p_instance_offset);
	tlas_descriptor->setInstanceDescriptorStride(sizeof(MDAccelerationStructureInstance));

	resident_blases.clear();

	if (fallback_blas == nullptr) {
		// A collection of null Godot instances is semantically an empty TLAS.
		tlas_descriptor->setInstanceCount(0);
		tlas_descriptor->setInstancedAccelerationStructures(NS::Array::array());
		return true;
	}

	LocalVector<NS::Object *> instanced_acceleration_structures;
	instanced_acceleration_structures.resize(p_instance_count);
	uint8_t *writable_instance_bytes = static_cast<uint8_t *>(p_instance_buffer->contents()) + p_instance_offset;
	for (uint32_t i = 0; i < p_instance_count; i++) {
		MDAccelerationStructureInstance &instance = instances[i];
		MDAccelerationStructure *blas_info = instance.blas != nullptr ? instance.blas : fallback_blas;
		instance.acceleration_structure_index = i;
		instance.mask = instance.blas != nullptr ? instance.requested_mask : 0;
		instanced_acceleration_structures[i] = blas_info->accel.get();
		if (!resident_blases.has(blas_info->accel.get())) {
			resident_blases.push_back(blas_info->accel.get());
		}
		memcpy(writable_instance_bytes + (i * sizeof(MDAccelerationStructureInstance)), &instance, sizeof(MDAccelerationStructureInstance));
	}

	NS::Array *blas_array = NS::Array::array(instanced_acceleration_structures.ptr(), instanced_acceleration_structures.size());
	tlas_descriptor->setInstanceCount(p_instance_count);
	tlas_descriptor->setInstancedAccelerationStructures(blas_array);
	return true;
}
