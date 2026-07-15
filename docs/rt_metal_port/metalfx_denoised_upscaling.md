# MetalFX temporal denoised upscaling

Godot's Metal path tracer supports Apple's combined temporal denoiser and
upscaler when `MTLFXTemporalDenoisedScalerDescriptor::supportsDevice()` reports
support. The runtime gate also requires macOS 26 (or iOS 18) and is exposed as
`RenderingDevice.SUPPORTS_METALFX_DENOISED`. Unsupported devices do not show
the option in the `Environment` inspector, and saved resources fall back to
`None` with one warning.

Select **MetalFX Denoised Upscaling** in the Environment's path-tracing
denoiser property. The effect owns the internal-to-target resolution step, so
it can operate at native resolution or replace the viewport's configured
presentation upscaler when the internal resolution is lower.

The Metal compute/ray-query scene lane builds a denoiser-guide shader variant
only while a denoiser is active. It emits noise-free diffuse and specular
albedo, signed world-space normals, linear roughness, and optional specular hit
distance. MetalFX also consumes the path tracer's color, reverse-Z depth, and
motion vectors. The effect runs before tone mapping and post-processing and
uses the same camera-cut, resize, frame-gap, and pause/resume history reset
contract as Godot's other temporal upscalers.

The descriptor is created with the render buffer's actual texture formats and
input/output sizes. If MetalFX rejects that combination, Godot reports one
error and presents the frame without denoising instead of encoding a null
scaler.
