#pragma once

#include "engine/core/result.hpp"
#include "engine/jobs/job_system.hpp"
#include "engine/renderer/rhi/render_device.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace heartstead::renderer {

enum class ResidencyResourceClass : std::uint8_t {
    texture,
    mesh,
};

enum class ResidencyState : std::uint8_t {
    fallback,
    queued,
    loading,
    upload_pending,
    resident,
    failed,
    cancelled,
};

struct ResidencyRequest {
    std::string id;
    ResidencyResourceClass resource_class = ResidencyResourceClass::texture;
    // Zero is the highest-detail mip/LOD. A lower requested value upgrades an existing resident.
    std::uint32_t detail_level = 0;
    float priority = 0.0F;
    std::size_t estimated_gpu_bytes = 0;
    bool pinned = false;
};

struct ResidencyPayload {
    std::string id;
    ResidencyResourceClass resource_class = ResidencyResourceClass::texture;
    std::uint32_t detail_level = 0;
    std::vector<std::byte> bytes;
    std::size_t estimated_gpu_bytes = 0;
};

struct ResidencyGpuResource {
    rhi::RenderResourceHandle handle;
    std::size_t gpu_bytes = 0;
};

using ResidencyLoadFunction = std::function<core::Result<ResidencyPayload>(
    const ResidencyRequest&, const std::atomic_bool& cancellation_requested)>;
using ResidencyUploadFunction =
    std::function<core::Result<ResidencyGpuResource>(ResidencyPayload&&)>;
// Runs on the render thread. Vulkan implementations should enqueue destruction using the
// resource's last submission serial rather than immediately destroying an in-flight object.
using ResidencyReleaseFunction = std::function<void(ResidencyGpuResource)>;

struct StreamingResidencyConfig {
    jobs::JobBackend job_backend = jobs::JobBackend::thread_pool;
    std::uint32_t worker_count = 2;
    std::uint32_t maximum_in_flight_loads = 8;
    std::size_t upload_budget_bytes = 16U * 1024U * 1024U;
    std::uint32_t upload_budget_resources = 8;
    std::size_t resident_budget_bytes = 512U * 1024U * 1024U;
    float reported_heap_budget_fraction = 0.85F;
    rhi::RenderResourceHandle texture_fallback;
    rhi::RenderResourceHandle mesh_fallback;
};

struct StreamingResidencyStats {
    std::uint64_t frame_index = 0;
    std::size_t tracked_resources = 0;
    std::size_t queued_resources = 0;
    std::size_t in_flight_loads = 0;
    std::size_t upload_pending_resources = 0;
    std::size_t resident_resources = 0;
    std::size_t failed_resources = 0;
    std::size_t resident_bytes = 0;
    std::size_t pending_upload_bytes = 0;
    std::size_t uploaded_bytes_this_frame = 0;
    std::uint32_t uploaded_resources_this_frame = 0;
    std::uint64_t stale_loads_discarded = 0;
    std::uint64_t cancelled_loads = 0;
    std::uint64_t evicted_resources = 0;
    std::uint64_t failed_loads = 0;
    std::uint64_t failed_uploads = 0;
};

struct ResidencyRecordView {
    std::string id;
    ResidencyState state = ResidencyState::fallback;
    std::uint32_t requested_detail_level = 0;
    std::optional<std::uint32_t> resident_detail_level;
    float priority = 0.0F;
    std::size_t resident_bytes = 0;
    std::uint64_t last_requested_frame = 0;
};

class StreamingResidencyManager {
  public:
    [[nodiscard]] static core::Result<std::unique_ptr<StreamingResidencyManager>>
    create(StreamingResidencyConfig config, ResidencyLoadFunction loader);
    ~StreamingResidencyManager();

    StreamingResidencyManager(const StreamingResidencyManager&) = delete;
    StreamingResidencyManager& operator=(const StreamingResidencyManager&) = delete;

    void begin_frame(std::uint64_t frame_index) noexcept;
    [[nodiscard]] core::Status request(ResidencyRequest request);
    void cancel(std::string_view id);
    void set_reported_heap_budget(std::size_t bytes) noexcept;
    [[nodiscard]] core::Status process(const ResidencyUploadFunction& uploader,
                                       const ResidencyReleaseFunction& releaser);
    void shutdown(const ResidencyReleaseFunction& releaser);

    [[nodiscard]] rhi::RenderResourceHandle resolve(std::string_view id,
                                                    ResidencyResourceClass resource_class) const;
    [[nodiscard]] ResidencyState state(std::string_view id) const noexcept;
    [[nodiscard]] std::vector<ResidencyRecordView> records() const;
    [[nodiscard]] const StreamingResidencyStats& stats() const noexcept;

  private:
    struct Record {
        ResidencyRequest request;
        ResidencyState state = ResidencyState::fallback;
        std::uint64_t generation = 1;
        std::uint64_t last_requested_frame = 0;
        bool requested = true;
        bool load_in_flight = false;
        std::shared_ptr<std::atomic_bool> cancellation;
        std::optional<ResidencyPayload> pending_payload;
        std::optional<ResidencyGpuResource> resident;
        std::optional<std::uint32_t> resident_detail_level;
        std::string error_code;
        std::string error_message;
    };

    struct CompletedLoad {
        std::string id;
        std::uint64_t generation = 0;
        bool cancelled = false;
        std::optional<ResidencyPayload> payload;
        std::string error_code;
        std::string error_message;
    };

    struct SharedCompletions {
        std::mutex mutex;
        std::vector<CompletedLoad> values;
    };

    StreamingResidencyManager(StreamingResidencyConfig config, ResidencyLoadFunction loader,
                              std::unique_ptr<jobs::IJobSystem> jobs);
    void schedule_loads();
    void harvest_loads();
    [[nodiscard]] core::Status upload_ready(const ResidencyUploadFunction& uploader,
                                            const ResidencyReleaseFunction& releaser);
    void enforce_budget(const ResidencyReleaseFunction& releaser);
    void refresh_stats();
    [[nodiscard]] std::size_t effective_resident_budget() const noexcept;
    [[nodiscard]] rhi::RenderResourceHandle
    fallback_for(ResidencyResourceClass resource_class) const noexcept;

    StreamingResidencyConfig config_;
    ResidencyLoadFunction loader_;
    std::unique_ptr<jobs::IJobSystem> jobs_;
    std::shared_ptr<SharedCompletions> completions_ = std::make_shared<SharedCompletions>();
    std::unordered_map<std::string, Record> records_;
    StreamingResidencyStats stats_;
    std::size_t reported_heap_budget_bytes_ = 0;
    bool shutdown_ = false;
};

} // namespace heartstead::renderer
