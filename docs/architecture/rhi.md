# Renderer RHI

Status: authoritative for Renderer V2.

The renderer-facing hardware interface is `renderer::rhi::IRenderDevice`. Game and presentation
code never include Vulkan headers and never retain native handles. `Renderer` owns the device and
all resource managers; the owner-thread wrapper enforces that creation, submission, resize, and
destruction happen on the render thread.

## Device and resources

The Vulkan backend uses Vulkan 1.3 dynamic rendering and explicit barriers, selects graphics/present
queues, records optional features, and exposes `VK_EXT_memory_budget` data where available. Hardware
ray tracing is not required. The deterministic headless backend validates frame/resource contracts
without producing pixels.

The RHI supports vertex, index, uniform, storage, indirect, and storage-indirect buffers. Images
carry explicit format, extent, mip and array counts, cubemap compatibility, sampled/storage/color/
depth/transfer usage, and color-space metadata. Samplers support anisotropy, comparison sampling,
address modes, and mip ranges. Pipeline layouts declare named descriptors and push constants before
pipeline creation.

`RenderFrameSubmission` combines a validated graph, camera/environment/exposure state, and commands
keyed by stable pass index. Draws support indexed/non-indexed, instanced, and indirect rendering;
compute dispatch is first-class. Transfer work uses budgeted upload managers.

Debug Vulkan runs enable validation, `VK_EXT_debug_utils` object/pass labels, and timestamp queries.
Pipeline variants are prewarmed before the cache is sealed; unexpected runtime creation is an error.

Presentation-completion timing is a diagnostic RHI capability, not a default pacing mechanism.
`RenderDeviceCapabilities::supports_presentation_completion_timing` reports combined surface,
`VK_KHR_present_id`, `VK_KHR_present_wait`, and feature support. A
`RenderDeviceDesc::measure_presentation_completion` request requires that capability, assigns one
strictly increasing ID per swapchain present, and returns validity, ID, and host-observed
queue-call-to-completion wait in `RenderFrameStats`. Unsupported or timed-out requests fail closed.
The opt-in wait serializes presentation and does not claim a precise presentation timestamp,
physical scan-out, or input-to-photon latency; ordinary devices do not enable or execute it.

See [Rendering](rendering.md), [Resource lifetime and synchronization](resource_lifetime_and_synchronization.md),
and [Large-world rendering](large_world_rendering.md).
