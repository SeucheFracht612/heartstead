# Resource Lifetime and Synchronization

Status: authoritative for Renderer V2.

`transient` graph images are frame-local and allocated by the Vulkan frame-resource cache.
`external` images are imported ownership boundaries, currently the presentation image. Persistent
runtime buffers, textures, meshes, atlases, and pipelines live in their owning managers behind
generation-checked handles. The current FXAA pipeline owns no temporal image; a future temporal pass
must add a real cross-frame owner before declaring history state.

Resources are not destroyed while an in-flight submission may reference them. Logical release queues
native destruction against a submission serial and reclaims after completion. Swapchain recreation
rebuilds extent-dependent resources.

Passes declare reads, writes, sampled/storage bindings, color attachments, and depth attachments.
`RenderFramePlan::build_execution_plan()` rejects read-before-write, ambiguous access, invalid
presentation, and descriptor/access mismatches, then derives dependencies and transitions centrally.
Vulkan translates these into stage/access masks and image barriers; passes emit no ad hoc barriers.

Uploads use per-frame byte/resource budgets. Cancellation and stale-generation checks prevent
background I/O or meshing from publishing obsolete work.

Shutdown order is: stop producers, drain/cancel background work, wait for the device, release
presentation systems and caches in reverse dependency order, drain deferred destruction, then destroy
the device and window. Shutdown methods are idempotent and return the first error.
