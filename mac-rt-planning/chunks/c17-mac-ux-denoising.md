# C17: Mac UX, denoising, and presentation

**Status:** Planned

## Goal

Make path-tracing settings truthful and usable on Mac, and validate the image
presentation path without conflating upscaling with denoising.

## Required implementation

1. Make `None` the safe path-tracing denoiser default on platforms where DLSS
   Ray Reconstruction is unavailable.
2. Filter or disable inspector choices according to runtime capability and
   provide one actionable warning when loading an unsupported saved choice.
3. Mask the NVIDIA SER path on Metal.
4. Decide and document a native denoising strategy. If none lands in this
   chunk, retain deterministic `None` behavior and name native denoising as an
   explicit remaining limitation.
5. Validate any MetalFX/FSR presentation use separately. MetalFX upscaling must
   not be presented as a ray-reconstruction denoiser.
6. Check resize, camera cuts, editor pause/resume, viewport switching, and
   history reset behavior.

## Required tests

- L3 platform default, property hint, capability filtering, and serialization.
- L5-E0/E2 captures with denoiser `None`.
- Presentation matrix at native resolution and each enabled upscaler.
- Camera-cut and resize sequences with history/debug records.
- Project created on Windows with DLSS-RR selected, then opened on Mac.
- Unsupported-device and forced-disabled fallback behavior.

## Acceptance markers

- `METAL_RT_C17_MAC_UX=passed`
- `METAL_RT_DENOISER_DEFAULT=none`
- `METAL_RT_SER=disabled`
- `METAL_RT_PRESENTATION=<native|validated_mode>`

## Artifacts

Inspector screenshots, serialized environment fixture, warning/fallback logs,
presentation images/diffs/metrics, and temporal reset trace.
