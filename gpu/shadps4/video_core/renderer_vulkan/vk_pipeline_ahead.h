// bbport: the pipelines the ahead-of-time compile builds. The game chooses a pipeline's state at
// draw time; the planner knows the engine's rules for it (a model pass's targets, the formats and
// blends of the other passes, the vertex layouts) and adds what this player's sessions have drawn,
// kept in their cache. The pairs of shaders come from the game's files. No backend's objects here:
// the same plan serves any backend. Nothing here is shipped.
#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "video_core/renderer_vulkan/vk_pipeline_key.h"
#include "video_core/renderer_vulkan/vk_shader_scan.h"

namespace Vulkan {

/// One pipeline by its programs (0: no pixel shader). `key`'s stage hashes are not used, except
/// a tessellated draw's: its hull and domain programs (`vertex` is then its local program).
struct AheadPipeline {
    u64 vertex{};
    u64 pixel{};
    GraphicsPipelineKey key{};
};

/// A tessellated draw's programs, as the file names pair them.
struct TessStages {
    u64 local{}, hull{}, domain{};
    auto operator<=>(const TessStages&) const = default;
};
/// Every local, hull and domain program set the game's files give.
std::vector<TessStages> TessellationSets(const ShaderScan& scan);

/// Adds a pipeline a session built or loaded to the player's list, once.
void NotePipeline(const std::filesystem::path& path, const AheadPipeline& pipeline);
/// The player's list; empty when it is missing or from another version.
std::vector<AheadPipeline> ReadNotedPipelines(const std::filesystem::path& path);
/// How many different states `seen` holds.
u32 CountStates(std::span<const AheadPipeline> seen);
/// Every pair the files give, with the rules' states and formats and those `seen` shows.
std::vector<AheadPipeline> PlanPipelines(const ShaderScan& scan, std::span<const AheadPipeline> seen);
/// Prints how many of `seen` the rules alone plan (BB_PIPELINE_PLAN_CHECK).
void CheckPlan(const ShaderScan& scan, std::span<const AheadPipeline> seen);

/// The system the compile step built its pipelines for (the macOS build and the Mac model).
/// Metal's pipeline archive is unusable after a macOS update or on another Mac and is started
/// over (mtl_pso_cache.mm), so the step runs again when this changes. Empty off macOS.
std::string PipelineSystem();

/// A later session's pipelines bring the step back only with this many new states: a run takes
/// minutes, and most new pipelines reuse states the step has already built.
constexpr u32 NewStatesForARun = 4;

/// Why the compile step is or is not due.
enum class CompileReason {
    NeverBuilt,   ///< the step has not finished here (no record of it)
    OtherVersion, ///< finished by another version of the shader binary format
    OtherSystem,  ///< finished on another macOS build or Mac
    NewStates,    ///< finished, but play has met enough new states since
    UpToDate,     ///< finished and nothing new: not due
};
const char* CompileReasonName(CompileReason reason);

struct CompileStatus {
    bool due{};
    CompileReason reason{CompileReason::NeverBuilt};
    u32 built_states{};          ///< the states the finished step built (pipelines-ahead.complete)
    u32 seen_states{};           ///< the states play has noted (pipelines-seen.bin)
    u32 new_states_for_a_run{NewStatesForARun};
    std::string built_for;       ///< the system recorded by the finished step (empty: none)
    std::string system;          ///< the system now
    u32 version{};               ///< the shader binary version now
    u32 built_version{};         ///< the version recorded by the finished step (0: none)
    bool migrated{};             ///< built_for was empty with states built (0.9.5): taken as `system`
};

/// The one rule of whether the compile step is due, read-only: the folder is the user's cache
/// folder, `system` the current PipelineSystem(). Due unless shaders-ahead.txt.complete holds this
/// version, pipelines-ahead.complete this system, and the session notes hold fewer than
/// NewStatesForARun states over those built. A record from 0.9.5 (states, no system) is taken as
/// this system (`migrated`); rewriting it is the caller's, not done here.
CompileStatus EvaluateCompile(const std::filesystem::path& cache, const std::string& system);

} // namespace Vulkan
