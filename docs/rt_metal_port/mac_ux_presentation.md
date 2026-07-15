# Metal path-tracing UX and presentation (C17)

Status: **Available**, with manual editor review still required for interactive
quality and temporal artifacts.

C17 makes the Mac path-tracing controls describe capabilities that actually
exist. A new `Environment` uses denoiser `None`. The inspector obtains its
choices from `RenderingServer.is_pathtracing_denoiser_supported()` and lists
only `None` on Metal. If a resource saved on Windows requests DLSS Ray
Reconstruction, loading it on an unsupported device emits one actionable
warning and changes the in-memory value to `None`; saving it then serializes
the safe value.

The Metal compute lane continues to remove the NVIDIA SER specialization flag
before shader compilation. The project setting can remain enabled for a
cross-platform project, but it has no effect on Metal.

## Native denoising decision

No native path-tracing denoiser lands in C17. Deterministic `None` is the
supported Mac strategy and native denoising remains an explicit limitation.
MetalFX spatial and temporal modes, FSR 1, and FSR 2 are presentation
upscalers; none is exposed or described as a ray-reconstruction denoiser.

## Presentation and temporal history

The C17 E0 run captures native resolution, FSR 1, FSR 2, and each MetalFX mode
reported by the current Metal device. Temporal-upscaler history is reset when
its context is created, after a viewport frame gap, after a large camera or
projection cut, or after a long frame such as pause/resume. Resizing rebuilds
the render-buffer context and therefore also starts with reset history.

The retained trace covers a camera cut, window resize, editor pause/resume,
creation and selection of a second viewport, and return to the original
viewport. The runner saves every image, the sanitized cross-platform
`Environment`, a JSON event/capability manifest, and semantic image metrics.

Run the C17 contract against the arm64 Metal editor:

```bash
python3 tests/metal_rt/run_mac_rt_tests.py \
  --stage editor-scene --binary bin/godot.macos.editor.arm64
```

The C17-specific success markers are:

```text
METAL_RT_C17_MAC_UX=passed
METAL_RT_DENOISER_DEFAULT=none
METAL_RT_SER=disabled
METAL_RT_PRESENTATION=native,fsr1,fsr2[,metalfx_spatial,metalfx_temporal]
METAL_RT_TEMPORAL_SEQUENCE=passed
METAL_RT_C17_PRESENTATION_VERIFY=passed
```

## Manual acceptance

In the E0 scene, inspect native, FSR, and available MetalFX modes at normal
editor size. Confirm that the image remains usable after resizing, an abrupt
camera move, pausing and resuming, and switching 3D viewports. In temporal
modes, specifically look for stale silhouettes, trails, a one-frame black or
garbled image, excessive shimmer, and a history flash after each transition.
Also confirm that the Environment inspector offers only `None` on Mac and that
opening the Windows DLSS-RR fixture produces one warning rather than repeated
warnings.

These perceptual and interactive judgments cannot be approved by the image
semantics checks alone.
