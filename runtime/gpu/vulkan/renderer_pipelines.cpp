// Translated shaders, the pipelines built from them, and the shader cache that
// rebuilds a previous run's pipelines before the title asks for them.
#include "renderer_state.h"
#include "../../diagnostics.h"
#include "../../log.h"
#include "../../report.h"
#include "../../crash.h"
#include "../../env.h"
#include "../../platform.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_set>
#include <string>
#include <thread>
#include <vector>

#ifdef MW2_HAVE_VULKAN

namespace vk::renderer::detail
{
    // A shader the title has not drawn with before here: taken from the ones
    // compiled when it was loaded (TakePrepared), or translated and compiled
    // now -- a stall, which is why the load-time path exists.
    const Shader* ShaderFor(shader::Type type, uint64_t hash, const uint32_t* code, size_t words)
    {
        if (!code || !words) return nullptr;
        auto& last = g.lastShader[type == shader::Type::Pixel];
        if (last.shader && last.hash == hash) return last.shader;
        auto found = g.shaders.find(hash);
        if (found != g.shaders.end())
        {
            if (!found->second.module) return nullptr;
            last = { hash, &found->second };
            return &found->second;
        }

        Shader entry;
        if (!TakePrepared(hash, entry))
        {
            stutters::Timed timed(stutters::kShaders);
            const auto translateStart = std::chrono::steady_clock::now();
            entry = BuildShader(type, code, words);
            const uint64_t took = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - translateStart).count());
            g.translateMicroseconds += took;
            report::Add(report::kShader, took);
            g.shadersBuiltAtDraw++;
        }
        const bool ok = entry.module != VK_NULL_HANDLE;
        if (!ok)
            LOGW("renderer: %s shader %016llX not usable: %s",
                 type == shader::Type::Vertex ? "vertex" : "pixel",
                 (unsigned long long)hash,
                 entry.translation.ok ? "driver refused the module"
                                      : entry.translation.error.c_str());
        auto& stored = g.shaders.emplace(hash, std::move(entry)).first->second;
        return ok ? &stored : nullptr;
    }

    VkCompareOp CompareOp(uint32_t function)
    {
        switch (function & 7)
        {
        case 0: return VK_COMPARE_OP_NEVER;
        case 1: return VK_COMPARE_OP_LESS;
        case 2: return VK_COMPARE_OP_EQUAL;
        case 3: return VK_COMPARE_OP_LESS_OR_EQUAL;
        case 4: return VK_COMPARE_OP_GREATER;
        case 5: return VK_COMPARE_OP_NOT_EQUAL;
        case 6: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        default: return VK_COMPARE_OP_ALWAYS;
        }
    }

    // Xenos's stencil operations are Vulkan's, in the same order.
    VkStencilOp StencilOp(uint32_t op)
    {
        constexpr VkStencilOp kOps[8] = {
            VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE,
            VK_STENCIL_OP_INCREMENT_AND_CLAMP, VK_STENCIL_OP_DECREMENT_AND_CLAMP,
            VK_STENCIL_OP_INVERT, VK_STENCIL_OP_INCREMENT_AND_WRAP, VK_STENCIL_OP_DECREMENT_AND_WRAP,
        };
        return kOps[op & 7];
    }

    VkBlendFactor BlendFactor(uint32_t factor)
    {
        switch (factor)
        {
        case 0:  return VK_BLEND_FACTOR_ZERO;
        case 1:  return VK_BLEND_FACTOR_ONE;
        case 4:  return VK_BLEND_FACTOR_SRC_COLOR;
        case 5:  return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 6:  return VK_BLEND_FACTOR_SRC_ALPHA;
        case 7:  return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 8:  return VK_BLEND_FACTOR_DST_COLOR;
        case 9:  return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 10: return VK_BLEND_FACTOR_DST_ALPHA;
        case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 12: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 14: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        case 16: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        default: return VK_BLEND_FACTOR_ONE;
        }
    }

    VkBlendOp BlendOp(uint32_t op)
    {
        switch (op & 7)
        {
        case 1:  return VK_BLEND_OP_SUBTRACT;
        case 2:  return VK_BLEND_OP_MIN;
        case 3:  return VK_BLEND_OP_MAX;
        case 4:  return VK_BLEND_OP_REVERSE_SUBTRACT;
        default: return VK_BLEND_OP_ADD;
        }
    }

    VkPrimitiveTopology TopologyFor(uint32_t primitive)
    {
        switch (primitive)
        {
        case 1:  return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case 2:  return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case 3:  return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case 4:  return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case 5:  return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        case 6:  return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        // A rectangle list is expanded into two triangles a rectangle before the
        // draw, so by here it is an ordinary triangle list.
        case 8:  return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case 12: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;      // line loop
        default: return VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
        }
    }

    DepthTests DepthTestsFor(uint32_t control)
    {
        const gpu::DepthControl depthControl{ control };
        DepthTests tests;
        tests.depthTest = depthControl.ZEnable();
        tests.depthWrite = depthControl.ZWrite();
        tests.depthCompare = CompareOp(depthControl.ZFunc());
        // The reference and the masks are set on their own. Back faces share
        // the front's operations unless the title gives them their own.
        tests.stencilTest = depthControl.StencilEnable();
        tests.front = { StencilOp(depthControl.StencilFail()), StencilOp(depthControl.StencilZPass()),
                        StencilOp(depthControl.StencilZFail()), CompareOp(depthControl.StencilFunc()),
                        0, 0, 0 };
        tests.back = tests.front;
        if (depthControl.BackfaceEnable())
            tests.back = { StencilOp(depthControl.StencilFailBack()),
                           StencilOp(depthControl.StencilZPassBack()),
                           StencilOp(depthControl.StencilZFailBack()),
                           CompareOp(depthControl.StencilFuncBack()), 0, 0, 0 };
        return tests;
    }

    void FacesFor(uint32_t modeCntl, VkCullModeFlags& cull, VkFrontFace& front)
    {
        const gpu::ModeCntl mode{ modeCntl };
        // A whole class of geometry missing and nothing else wrong is what a
        // winding read backwards looks like, so it is worth being able to take
        // the culling away in one run.
        static const bool noCull = diag::Flag("MW2_NO_CULL");
        cull = noCull ? 0u
                      : ((mode.CullFront() ? VK_CULL_MODE_FRONT_BIT : 0u) |
                         (mode.CullBack() ? VK_CULL_MODE_BACK_BIT : 0u));
        front = mode.FrontIsClockwise() ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    }

    PipelineState::PipelineState(const PipelineKey& key)
    {
        // Xenos has no fixed-function vertex input at all: the shader fetches its
        // own attributes out of set 2.
        assembly.topology = VkPrimitiveTopology(key.topology);
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;

        raster.polygonMode = VK_POLYGON_MODE_FILL;
        // Culling and winding are set per draw (FacesFor).
        raster.lineWidth = 1.0f;
        // Always on, and set per draw: a shadow map drawn without the title's
        // bias shadows the very surfaces it was rendered from.
        raster.depthBiasEnable = VK_TRUE;

        multisample.rasterizationSamples = VkSampleCountFlagBits(std::max(key.samples, 1u));
        // The tests themselves are set per draw (DepthTestsFor); `depth` stays empty.

        const gpu::BlendControl blend{ key.blendControl };
        attachment.blendEnable = !blend.IsPassThrough();
        attachment.srcColorBlendFactor = BlendFactor(blend.ColorSource());
        attachment.dstColorBlendFactor = BlendFactor(blend.ColorDestination());
        attachment.colorBlendOp = BlendOp(blend.ColorOp());
        attachment.srcAlphaBlendFactor = BlendFactor(blend.AlphaSource());
        attachment.dstAlphaBlendFactor = BlendFactor(blend.AlphaDestination());
        attachment.alphaBlendOp = BlendOp(blend.AlphaOp());
        // Four bits per target, ARGB on the console and RGBA here.
        const uint32_t mask = key.colourMask & 0xF;
        attachment.colorWriteMask =
            ((mask & 1) ? VK_COLOR_COMPONENT_R_BIT : 0u) |
            ((mask & 2) ? VK_COLOR_COMPONENT_G_BIT : 0u) |
            ((mask & 4) ? VK_COLOR_COMPONENT_B_BIT : 0u) |
            ((mask & 8) ? VK_COLOR_COMPONENT_A_BIT : 0u);
        blending.attachmentCount = 1;
        blending.pAttachments = &attachment;

        static constexpr VkDynamicState kDynamic[] = {
            VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS,
            VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
            VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_CULL_MODE_EXT,
            VK_DYNAMIC_STATE_FRONT_FACE_EXT, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE_EXT,
            VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE_EXT, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP_EXT,
            VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE_EXT, VK_DYNAMIC_STATE_STENCIL_OP_EXT };
        dynamic.dynamicStateCount = uint32_t(std::size(kDynamic));
        dynamic.pDynamicStates = kDynamic;

        // Dynamic rendering: the formats of the targets stand where a render
        // pass would.
        colourFormat = VkFormat(key.colourFormat);
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &colourFormat;
        rendering.depthAttachmentFormat = VkFormat(key.depthFormat);
        rendering.stencilAttachmentFormat = VkFormat(key.depthFormat);
    }

    // The whole pipeline in one call, both shaders compiled for it: what a
    // device without pipeline libraries gets. Any thread: it reads nothing but
    // its arguments, the device, the layout and the cache.
    VkPipeline BuildPipeline(const PipelineKey& key, VkShaderModule vertexModule, VkShaderModule pixelModule)
    {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertexModule;
        stages[0].pName = "main";
        stages[1] = stages[0];
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = pixelModule;

        const PipelineState state(key);
        VkGraphicsPipelineCreateInfo info{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        info.pNext = &state.rendering;
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &state.input;
        info.pInputAssemblyState = &state.assembly;
        info.pViewportState = &state.viewport;
        info.pRasterizationState = &state.raster;
        info.pMultisampleState = &state.multisample;
        info.pDepthStencilState = &state.depth;
        info.pColorBlendState = &state.blending;
        info.pDynamicState = &state.dynamic;
        info.layout = static_cast<VkPipelineLayout>(vk::pipeline::Layout());

        VkPipeline built = VK_NULL_HANDLE;
        if (vkCreateGraphicsPipelines(g.device, static_cast<VkPipelineCache>(vk::pipeline::Cache()),
                                      1, &info, nullptr, &built) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        return built;
    }

    VkPipeline BuildPipeline(const PipelineKey& key, const Shader& vertex, const Shader& pixel)
    {
        return BuildPipeline(key, vertex.module, pixel.module);
    }

    // Pipelines compiled off the draw. Without pipeline libraries (MoltenVK)
    // a pipeline is both shaders compiled for Metal, a quarter of a second to
    // more than one apiece on Apple's chips: a frame that met five new ones
    // stopped the game for a second, and a match's first minute met 150. So a
    // draw that needs one asks for it and is skipped, the way Xenia's
    // asynchronous shaders skip it, and the pipeline is ready a few frames
    // later -- something new on screen appears a moment late, the first time
    // only (the caches keep it). MW2_ASYNC_PIPELINES=0 compiles at the draw
    // again; =1 asks for this on a device with libraries too. On by default on
    // Apple's systems.
    bool AsyncPipelines()
    {
        static const bool on = [] {
            if (const char* value = env::Text("MW2_ASYNC_PIPELINES")) return std::strcmp(value, "0") != 0;
#ifdef __APPLE__
            return !vk::pipeline::PipelineLibraries();
#else
            return false;
#endif
        }();
        return on;
    }

    namespace
    {
        struct AsyncJob
        {
            PipelineKey key;
            VkShaderModule vertex = VK_NULL_HANDLE, pixel = VK_NULL_HANDLE;
        };
        struct AsyncCompiles
        {
            std::mutex lock;
            std::condition_variable wake;
            std::deque<AsyncJob> jobs;
            std::vector<std::pair<PipelineKey, VkPipeline>> finished;
            std::atomic<bool> anyFinished{ false };
            bool stopping = false;
            std::vector<platform::GuestThread> workers;
            // The renderer's thread's own: asked for and not taken back yet.
            std::unordered_set<PipelineKey, WordsHash> pending;
            std::atomic<uint64_t> compileMicroseconds{ 0 }, longestMicroseconds{ 0 };
            uint64_t asked = 0, skippedDraws = 0;
        } a;

        void CompileForDraws()
        {
            crash::RegisterThread("pipeline compiler");
            for (;;)
            {
                AsyncJob job;
                {
                    std::unique_lock held(a.lock);
                    a.wake.wait(held, [] { return a.stopping || !a.jobs.empty(); });
                    if (a.jobs.empty()) return;   // stopping
                    // The newest first: what was asked for last is what is on
                    // screen now.
                    job = a.jobs.back();
                    a.jobs.pop_back();
                }
                const auto start = std::chrono::steady_clock::now();
                const VkPipeline built = BuildPipeline(job.key, job.vertex, job.pixel);
                const uint64_t took = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - start).count());
                a.compileMicroseconds.fetch_add(took, std::memory_order_relaxed);
                uint64_t longest = a.longestMicroseconds.load(std::memory_order_relaxed);
                while (took > longest && !a.longestMicroseconds.compare_exchange_weak(longest, took)) {}
                std::lock_guard held(a.lock);
                a.finished.push_back({ job.key, built });
                a.anyFinished.store(true, std::memory_order_release);
            }
        }

        void TakeAsyncPipelines()
        {
            if (!a.anyFinished.load(std::memory_order_acquire)) return;
            std::vector<std::pair<PipelineKey, VkPipeline>> finished;
            {
                std::lock_guard held(a.lock);
                finished.swap(a.finished);
                a.anyFinished.store(false, std::memory_order_relaxed);
            }
            for (auto& [key, built] : finished)
            {
                a.pending.erase(key);
                if (!built)
                    LOGW("renderer: pipeline for %016llX/%016llX rejected",
                         (unsigned long long)key.vertexShader, (unsigned long long)key.pixelShader);
                g.pipelines[key] = built;   // a failure is remembered, so it fails once
            }
        }

        void AskFor(const PipelineKey& key, const Shader& vertex, const Shader& pixel)
        {
            if (a.workers.empty())
            {
                // Metal compiles a pipeline on one thread; a few at once keep
                // up with what a new area brings.
                const unsigned threads = std::clamp(std::thread::hardware_concurrency() / 3, 2u, 4u);
                for (unsigned i = 0; i < threads; i++) a.workers.emplace_back(CompileForDraws);
                LOGI("renderer: pipelines are compiled on %u threads of their own, and a draw"
                     " waiting for one is skipped (MW2_ASYNC_PIPELINES)", threads);
            }
            a.pending.insert(key);
            a.asked++;
            std::lock_guard held(a.lock);
            a.jobs.push_back({ key, vertex.module, pixel.module });
            a.wake.notify_one();
        }
    }

    void StopAsyncPipelines()
    {
        {
            std::lock_guard held(a.lock);
            a.stopping = true;
            a.jobs.clear();
        }
        a.wake.notify_all();
        for (platform::GuestThread& worker : a.workers) worker.join();
        a.workers.clear();
        TakeAsyncPipelines();
        if (a.asked)
            LOGI("renderer: %llu pipelines compiled off the draw in %llu ms (the longest %llu ms);"
                 " %llu draws waited for one",
                 (unsigned long long)a.asked, (unsigned long long)(a.compileMicroseconds.load() / 1000),
                 (unsigned long long)(a.longestMicroseconds.load() / 1000),
                 (unsigned long long)a.skippedDraws);
    }

    VkPipeline EnsurePipeline(const PipelineKey& key, const Shader& vertex, const Shader& pixel,
                              bool* made, bool* later)
    {
        TakeOptimisedPipelines();
        TakeAsyncPipelines();
        auto found = g.pipelines.find(key);
        if (found != g.pipelines.end()) return found->second;
        if (later && AsyncPipelines())
        {
            *later = true;
            a.skippedDraws++;
            if (a.pending.count(key)) return VK_NULL_HANDLE;
            if (made) *made = true;
            AskFor(key, vertex, pixel);
            return VK_NULL_HANDLE;
        }
        if (made) *made = true;

        const auto compileStart = std::chrono::steady_clock::now();
        stutters::Timed timed(stutters::kPipelines);
        // Linked from the two shaders' libraries, compiled when the title
        // loaded them, in well under a millisecond; or the whole pipeline
        // compiled here, on a device that cannot.
        VkPipeline built = vertex.library && pixel.library ? LinkPipeline(key, vertex, pixel)
                                                           : BuildPipeline(key, vertex, pixel);
        const uint64_t took = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - compileStart).count());
        g.compileMicroseconds += took;
        report::Add(report::kPipeline, took);
        if (!built)
            LOGW("renderer: pipeline for %016llX/%016llX rejected",
                 (unsigned long long)key.vertexShader, (unsigned long long)key.pixelShader);
        g.pipelines[key] = built;   // a failure is remembered, so it fails once
        return built;
    }

    // MW2_SHADER_CACHE=<file> records every pipeline a run needed and rebuilds
    // them at start-up on the next -- the "compiling shaders" pass the console
    // never needed. It cannot be built from the shaders alone: a pipeline is a
    // shader pair *and* the state it is drawn with, and a previous run is the only
    // place that state exists.
    const char* ShaderCachePath()
    {
        static const char* path = env::Text("MW2_SHADER_CACHE");
        return path;
    }

    constexpr uint32_t kCacheMagic = 0x32574D53;   // 'SMW2'
    constexpr uint32_t kCacheVersion = 3;   // 3: a vertex program ends by turning
                                            // window coordinates into clip space

    // The cache as it would be written: on the renderer's thread, which owns
    // what goes in it.
    std::vector<uint8_t> SerialiseShaderCache()
    {
        std::vector<uint8_t> out;
        auto put = [&out](const void* data, size_t bytes) {
            const auto* at = static_cast<const uint8_t*>(data);
            out.insert(out.end(), at, at + bytes);
        };
        const uint32_t header[4] = { kCacheMagic, kCacheVersion,
                                     uint32_t(g.shaders.size()), uint32_t(g.recorded.size()) };
        put(header, sizeof header);
        for (const auto& [hash, shader] : g.shaders)
        {
            const uint32_t entry[2] = { uint32_t(shader.type == shader::Type::Pixel),
                                        uint32_t(shader.code.size()) };
            put(&hash, sizeof hash);
            put(entry, sizeof entry);
            put(shader.code.data(), 4 * shader.code.size());
        }
        put(g.recorded.data(), sizeof(Recorded) * g.recorded.size());
        return out;
    }

    // Beside the file and then over it, so a run killed while it writes --
    // which is how an iPad app ends -- leaves the last whole one.
    bool WriteWhole(const char* path, const std::vector<uint8_t>& data)
    {
        const std::string partial = std::string(path) + ".partial";
        std::FILE* file = std::fopen(partial.c_str(), "wb");
        if (!file) return false;
        const bool written = std::fwrite(data.data(), 1, data.size(), file) == data.size();
        if (std::fclose(file) != 0 || !written) { std::remove(partial.c_str()); return false; }
        return std::rename(partial.c_str(), path) == 0;
    }

    void WriteShaderCache()
    {
        const char* path = ShaderCachePath();
        if (!path || g.recorded.empty()) return;
        if (WriteWhole(path, SerialiseShaderCache()))
            LOGI("renderer: wrote %zu shaders and %zu pipelines to %s",
                 g.shaders.size(), g.recorded.size(), path);
    }

    namespace
    {
        std::atomic<bool> g_saveAsked{ false };
        std::atomic<bool> g_saving{ false };
    }

    // Both caches written while the run goes on, not only at its end: a run
    // that is killed -- an iPad app put away, the usual end of one -- kept
    // nothing, and compiled every pipeline again at its first draw the next
    // time. Every 15 seconds that built something new, or when asked (the app
    // going to the background). The file is made on a thread of its own; the
    // renderer's part is copying what goes in it.
    void PersistCaches()
    {
        // What the caches held when they were read counts as kept: a start
        // that built nothing new rewrote 76 MB of pipeline cache for nothing.
        static size_t saved = g.recorded.size();
        static auto lastSave = std::chrono::steady_clock::now();
        const char* path = ShaderCachePath();
        if (!path) return;
        const auto now = std::chrono::steady_clock::now();
        const bool asked = g_saveAsked.load(std::memory_order_relaxed);
        if (g.recorded.size() == saved && !asked) return;
        if (!asked && now - lastSave < std::chrono::seconds(15)) return;
        if (g_saving.exchange(true)) return;
        g_saveAsked = false;
        saved = g.recorded.size();
        lastSave = now;
        std::thread([data = SerialiseShaderCache(), path] {
            WriteWhole(path, data);
            vk::pipeline::Persist();
            g_saving = false;
        }).detach();
    }

    // Work the draw path would otherwise do mid-frame, which is where a hitch
    // comes from -- the total is small, the moment it is paid is the problem.
    void PrewarmFromCache()
    {
        const char* path = ShaderCachePath();
        if (!path) return;
        std::FILE* file = std::fopen(path, "rb");
        if (!file) return;

        uint32_t header[4]{};
        if (std::fread(header, sizeof header, 1, file) != 1 ||
            header[0] != kCacheMagic || header[1] != kCacheVersion)
        {
            std::fclose(file);
            LOGW("renderer: %s is not a shader cache for this build, ignoring it", path);
            return;
        }

        const auto start = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < header[2]; i++)
        {
            uint64_t hash = 0;
            uint32_t entry[2]{};
            if (std::fread(&hash, sizeof hash, 1, file) != 1 ||
                std::fread(entry, sizeof entry, 1, file) != 1) { std::fclose(file); return; }
            std::vector<uint32_t> code(entry[1]);
            if (entry[1] && std::fread(code.data(), 4, code.size(), file) != code.size())
            { std::fclose(file); return; }
            ShaderFor(entry[0] ? shader::Type::Pixel : shader::Type::Vertex,
                      hash, code.data(), code.size());
        }

        std::vector<Recorded> wanted(header[3]);
        if (header[3] && std::fread(wanted.data(), sizeof(Recorded), wanted.size(), file)
                             != wanted.size())
        { std::fclose(file); return; }
        std::fclose(file);

        g.prewarmWanted = uint32_t(wanted.size());
        uint32_t last = 0;
        for (const Recorded& want : wanted)
        {
            auto vertex = g.shaders.find(want.vertexShader);
            auto pixel = g.shaders.find(want.pixelShader);
            if (vertex == g.shaders.end() || pixel == g.shaders.end() ||
                !vertex->second.module || !pixel->second.module)
                continue;
            PipelineKey key;
            key.vertexShader = want.vertexShader;
            key.pixelShader = want.pixelShader;
            key.colourFormat = want.colourFormat;
            key.depthFormat = want.depthFormat;
            key.topology = want.topology;
            key.blendControl = want.blendControl;
            key.colourMask = want.colourMask;
            key.samples = want.samples;
            if (EnsurePipeline(key, vertex->second, pixel->second)) g.prewarmed++;
            // Kept, so the cache written at shutdown is the union of this run and earlier
            // ones. Recording only what this run built would shrink it every time.
            g.recorded.push_back(want);

            // Progress, because this is the pass a player would be looking at.
            const uint32_t percent = 100 * (&want - wanted.data() + 1) / uint32_t(wanted.size());
            if (percent >= last + 25) { LOGI("compiling shaders: %u%%", percent); last = percent; }
        }
        g.prewarmMicroseconds = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count());
        LOGI("renderer: prewarmed %u of %u pipelines over %zu shaders in %llu us",
             g.prewarmed, g.prewarmWanted, g.shaders.size(),
             (unsigned long long)g.prewarmMicroseconds);
    }
}

void vk::renderer::SaveCachesSoon() { detail::g_saveAsked = true; }

#endif  // MW2_HAVE_VULKAN
