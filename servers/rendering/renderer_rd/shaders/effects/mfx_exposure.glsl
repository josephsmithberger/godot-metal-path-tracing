#[compute]

#version 450

#VERSION_DEFINES

#extension GL_EXT_samplerless_texture_functions : require

layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform texture2D adapted_luminance;
layout(r16f, set = 0, binding = 1) uniform restrict writeonly image2D exposure_output;

layout(push_constant, std430) uniform Params {
	float exposure_numerator;
}
params;

void main() {
	// MetalFX expects the multiplier applied to the input color, whereas
	// Godot stores the adapted scene luminance used as its denominator.
	float luminance = max(texelFetch(adapted_luminance, ivec2(0), 0).r, 0.0001);
	imageStore(exposure_output, ivec2(0), vec4(params.exposure_numerator / luminance));
}
