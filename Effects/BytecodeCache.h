#pragma once

#include "pch_engine.h"
#include "ShaderCompiler.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// BytecodeCache
// =============
//
// Compile-once, reuse-everywhere bytecode store. It stores and compiles
// requests; ShaderVariants decides what a variant is (MakeVariantRequest,
// BuildVariantMacros, PrecompileBindingShapes). A gpuBindable parameter
// switches its shader between cbuffer and GPU-bound builds at compile time
// through the `_SLPARAM_<name>_GPU=0|1|2` macros, so one effect has several
// variants; compiling them on the worker threads keeps that cost off the
// render thread.
//
// Identity model
// --------------
// The compile key contains everything that influences the resulting
// bytecode -- canonical source hash, entry point, target, ordered
// gpu-param signature hash, embedded-include-library hash, macro bitset.
// Effect ID / version are metadata only, which give the disk layer stable
// directory names; identical source across two renamed effects shares one
// cache entry.
//
// Diagnostics
// -----------
// Lookups return a `BytecodeCacheResult` carrying a status, bytecode and
// error message. Failed entries keep their diagnostics so a deterministic
// compile error doesn't recompile every frame.
//
// Threading
// ---------
// Worker `std::jthread`s (half the cores, clamped to 2..6) drain a FIFO
// queue. They start with the first queued job, not at construction, and
// `Shutdown` joins them; hosts call it through Engine::ShutdownEngineServices
// before the engine DLL unloads. The render thread can call `GetOrCompile`
// which:
//   1. returns immediately if the entry is `Ready`;
//   2. waits up to `timeoutMs` for an in-flight worker to finish;
//   3. falls back to inline compile on the calling thread.
// On race-completion the worker discards its result if the inline compile
// already won (`Pending -> Ready` only; never `Ready -> Failed`).
//
// Bounded memory
// --------------
// Total `Ready` + `Failed` bytecode bytes are capped (default 256 MB).
// `Pending` entries are never evicted. Eviction is LRU on insertion order;
// an evicted entry is reloaded from the disk layer when one is configured.

namespace ShaderLab::Effects
{
    // Compile identity. Two requests with equal keys must produce
    // byte-identical bytecode (assuming same engine version).
    struct BytecodeCompileKey
    {
        uint64_t    sourceHash;          // FNV1a-64 of canonical UTF-8 HLSL.
        uint64_t    paramSignatureHash;  // FNV1a-64 of "name0\0name1\0..." for ordered gpuBindable param names.
        uint64_t    includeLibraryHash;  // Hash of embedded-include library + cache schema version.
        // TWO bits per gpu-bindable parameter: bits [2i, 2i+1] hold the
        // binding mode for parameter i -- 0 = cbuffer, 1 = StructuredBuffer
        // SRV, 2 = Texture2D effect input (the only GPU-resident route a
        // Direct2D pixel shader can take). Part of the cache key, so two
        // modes of the same effect are distinct entries.
        uint32_t    macroBitset;
        // Option specialisation: 8 bits per specialised parameter, in
        // optionParamNames order. 0 = read from the cbuffer at run time,
        // v + 1 = compiled with the option fixed to index v.
        uint64_t    optionKey{ 0 };
        std::string entryPoint;          // e.g. "main".
        std::string target;              // e.g. "ps_5_0", "cs_5_0".

        bool operator==(const BytecodeCompileKey& o) const noexcept;
    };

    struct BytecodeCompileKeyHash
    {
        size_t operator()(const BytecodeCompileKey& k) const noexcept;
    };

    // Effect-level metadata stored alongside each entry. Used only by
    // disk-cache layout and diagnostics; never participates in equality.
    struct BytecodeCacheMetadata
    {
        std::wstring effectId;
        uint32_t     version{ 0 };
    };

    enum class BytecodeStatus : uint8_t
    {
        NotRequested = 0,
        Pending      = 1,
        Ready        = 2,
        Failed       = 3,
    };

    // Result handed back to callers. On success `status == Ready` and
    // `bytecode` is non-empty; on failure `status == Failed` and
    // `errorMessage` carries the D3DCompile diagnostic string.
    struct BytecodeCacheResult
    {
        BytecodeStatus       status{ BytecodeStatus::NotRequested };
        std::vector<uint8_t> bytecode;
        std::wstring         errorMessage;
        bool                 fromCache{ false };  // false if compiled inline.
    };

    // Compile request. Carries the canonical source + macros + metadata
    // so the worker can drive D3DCompile without callbacks back into
    // the data model.
    struct BytecodeCompileRequest
    {
        BytecodeCompileKey      key;
        BytecodeCacheMetadata   metadata;
        std::string             hlslSource;            // canonical UTF-8 (LF line endings).
        std::vector<std::string> gpuBindableParamNames; // index i -> bits [2i, 2i+1] of macroBitset.
        std::vector<std::string> optionParamNames;      // index i -> bits [8i, 8i+7] of optionKey.
        // SL_FIELD_<name> = <Result index> per analysis field (AnalysisFieldMacros);
        // folded into key.paramSignatureHash.
        std::vector<std::pair<std::string, std::string>> fieldMacros;
        // A source registered prebuilt (AddPrebuilt) is compiled only when
        // this is set: the evaluator sets it when its host allows runtime
        // compiles. Otherwise a build the blob lacks is refused, and the
        // evaluator falls back to one it has.
        bool allowPrebuiltCompile{ false };
    };

    // Option specialisation limits: an option key holds eight 8-bit slots.
    inline constexpr size_t   cMaxSpecializedOptions = 8;
    inline constexpr uint32_t cMaxOptionValues       = 255;

    // The per-parameter slot of an option key: 0 = generic, v + 1 = option v.
    inline uint32_t OptionKeySlot(uint64_t optionKey, size_t index) noexcept
    {
        return index < cMaxSpecializedOptions ? static_cast<uint32_t>((optionKey >> (8u * index)) & 0xFFu) : 0u;
    }

    // Helpers (also exposed so call sites can hash without going through
    // the cache, e.g. to compute a key for `TryGet`).
    uint64_t FNV1a64(const void* data, size_t size);
    uint64_t HashCanonicalSource(std::string_view canonical);
    uint64_t HashParamSignature(const std::vector<std::string>& orderedNames);
    // Hashes the gpu-bindable and specialised option names together.
    // With no options this equals HashParamSignature(gpuNames).
    uint64_t HashParamSignature(const std::vector<std::string>& gpuNames,
                                              const std::vector<std::string>& optionNames);
    // Covers the three engine headers and the cache schema.
    uint64_t IncludeLibraryHash();
    uint64_t IncludeLibraryHashOf(std::string_view paramsText, std::string_view colorMathText,
                                                std::string_view gamutText);

    // Centralized source canonicalization. Every call site that hashes
    // source MUST go through this so a render-time hash matches the
    // precompile-time hash even if the call site stored the source as
    // wstring vs string with different line-endings.
    std::string CanonicalizeHlslSource(std::wstring_view source);
    std::string CanonicalizeHlslSource(std::string_view source);

    // The cache itself. The engine uses the process instance; tests may
    // build their own.
    class BytecodeCache
    {
    public:
        static BytecodeCache& Instance();

        BytecodeCache();
        ~BytecodeCache();

        BytecodeCache(const BytecodeCache&) = delete;
        BytecodeCache& operator=(const BytecodeCache&) = delete;

        // Non-blocking lookup. Returns a copy of cached state. `status`
        // can be NotRequested / Pending / Ready / Failed. If the entry
        // is not in memory but exists on disk, this method silently
        // hydrates the in-memory map with the disk bytecode and returns
        // a Ready result (this is why the method is non-const).
        BytecodeCacheResult TryGet(const BytecodeCompileKey& key);

        BytecodeStatus GetStatus(const BytecodeCompileKey& key) const;

        // Configure the on-disk cache root. Call once during engine
        // init (via EngineExports). If empty (default), persistence is
        // disabled and the cache is purely in-memory. Path is created
        // recursively if missing.
        void SetDiskCacheRoot(std::wstring rootPath);
        std::wstring GetDiskCacheRoot() const;

        // Idempotent enqueue. If status is already Pending or Ready,
        // no-op. If status is Failed, no-op (caller must explicitly
        // Invalidate to retry a deterministic failure). Worker thread
        // picks it up. `urgent` puts it at the front of the queue, ahead
        // of eager precompiles, and moves an already queued request there.
        void RequestCompile(BytecodeCompileRequest request, bool urgent = false);

        // Index of a request in the work queue, or nullopt when it is not
        // queued (not requested, or already taken by a worker).
        std::optional<size_t> QueuePosition(const BytecodeCompileKey& key) const;

        // Synchronous "fetch or compile". Renders should call this:
        //   1. If Ready -> return immediately, fromCache=true.
        //   2. If Pending -> wait up to timeoutMs for worker; if still
        //      pending after timeout, fall back to inline compile on the
        //      calling thread. fromCache=false on inline.
        //   3. If miss -> compile inline, insert as Ready, return.
        //   4. If Failed cached -> return cached failure (don't re-compile).
        //
        // Race-safe: a late worker result will not overwrite a Ready
        // entry produced by an inline compile.
        BytecodeCacheResult GetOrCompile(BytecodeCompileRequest request, uint32_t timeoutMs = 50);

        // Produces a prebuilt build's bytecode, decompressing it from its
        // blob; empty when it cannot.
        using PrebuiltLoader = std::function<std::vector<uint8_t>()>;

        // Adds a build made ahead of time (an effect blob) as a Ready entry
        // that is never evicted, cleared or invalidated, and records its
        // source as prebuilt (see allowPrebuiltCompile). `load` runs the
        // first time the entry is read, so a build nobody asks for never
        // takes memory.
        void AddPrebuilt(const BytecodeCompileKey& key, PrebuiltLoader load);
        bool IsPrebuiltSource(uint64_t sourceHash) const;

        // Drops the entry so the next request recompiles (the editor's
        // Ctrl+Enter after fixing a failed shader).
        void Invalidate(const BytecodeCompileKey& key);

        // Drops every in-memory entry except Pending ones (a worker may be
        // mid-compile).
        void Clear();

        // Reap stale on-disk entries. Files not modified within
        // `staleThresholdSec` are deleted. Returns counts.
        struct ReapResult { size_t filesDeleted; size_t bytesFreed; size_t errors; };
        ReapResult ReapDisk(uint64_t staleThresholdSec);

        // ReapDisk as a job on the worker queue, behind any queued compiles.
        // Its counts land in LastReapResult. Dropped if Shutdown comes first.
        void QueueReapDisk(uint64_t staleThresholdSec);
        std::optional<ReapResult> LastReapResult() const;

        // Delete everything under the disk cache root. In-memory
        // entries are preserved (next render still works without
        // re-compile), but any future restart will recompile.
        ReapResult ClearDisk();

        struct Stats
        {
            size_t entries;
            size_t totalBytecodeBytes;
            size_t pendingCount;
            size_t failedCount;
            size_t inlineCompiles;   // total inline-fallback compiles since startup.
            size_t workerCompiles;   // total worker compiles since startup.
            size_t cacheHits;
            size_t cacheMisses;
        };
        Stats GetStats() const;

        // Stops accepting queued work, discards what is queued and joins the
        // workers (a compile in flight finishes first). Idempotent. Lookups
        // and GetOrCompile keep working afterwards, compiling inline.
        void Shutdown();

        // Worker threads running now: 0 until the first queued job and after
        // Shutdown.
        size_t WorkerCount() const;

    private:

        struct Entry
        {
            BytecodeStatus       status{ BytecodeStatus::Pending };
            std::vector<uint8_t> bytecode;
            std::wstring         errorMessage;
            uint64_t             insertionOrder{ 0 };  // for naive LRU eviction.
            // DoCompile calls holding a reference to this entry with the lock
            // dropped. Nothing may erase the entry while it is non-zero.
            uint32_t             compilesInFlight{ 0 };
            bool                 pinned{ false };   // AddPrebuilt
            PrebuiltLoader       load;              // until the first read
        };

        // Runs a prebuilt entry's loader on its first read. Caller holds m_mutex.
        void Materialize_NoLock(Entry& entry);

        // A queued job: a compile request, or a maintenance task when
        // `task` is set. The key is a copy so the entry pointer need not be
        // stable across the workers and the entry map.
        struct PendingWork
        {
            BytecodeCompileRequest request;
            std::function<void()>  task;
        };

        // Run a D3DCompile in-place and mutate the entry. Caller holds
        // m_mutex on entry; this method releases it during the compile
        // and re-acquires before mutating the map. Returns the final
        // status (Ready or Failed).
        BytecodeStatus DoCompile(
            const BytecodeCompileRequest& req,
            std::unique_lock<std::mutex>& lock,
            Entry& outEntry);

        void WorkerLoop(std::stop_token stop);
        // Starts the workers if none are running. Caller holds m_mutex.
        void StartWorkers_NoLock();

        // Evict oldest Ready/Failed entries until total bytecode bytes
        // fits under m_maxBytes. Caller holds m_mutex. Pending entries
        // are never evicted.
        void EnforceByteLimit_NoLock();

        mutable std::mutex                                                              m_mutex;
        std::condition_variable                                                          m_workCv;
        std::condition_variable                                                          m_readyCv;
        std::unordered_map<BytecodeCompileKey, Entry, BytecodeCompileKeyHash>           m_entries;
        std::deque<PendingWork>                                                          m_workQueue;
        std::vector<std::jthread>                                                        m_workers;
        std::atomic<bool>                                                                m_shutdown{ false };
        std::unordered_set<uint64_t>                                                     m_prebuiltSources;   // guarded by m_mutex
        uint64_t                                                                         m_nextInsertionOrder{ 1 };
        size_t                                                                           m_currentBytes{ 0 };

        // Tunables.
        size_t       m_maxBytes{ 256ull * 1024 * 1024 };  // 256 MB.
        std::wstring m_diskRoot;                          // empty = disabled; guarded by m_mutex.
        std::optional<ReapResult> m_lastReap;             // guarded by m_mutex.

        // Disk helpers. They take the root as a snapshot taken under m_mutex
        // and do their file I/O without holding it.
        static std::wstring KeyToDiskPath(
            const std::wstring& root,
            const BytecodeCompileKey& key,
            const BytecodeCacheMetadata& meta);
        static bool TryLoadFromDisk(
            const std::wstring& root,
            const BytecodeCompileKey& key,
            std::vector<uint8_t>& outBytecode);
        static void WriteToDisk(
            const std::wstring& root,
            const BytecodeCompileKey& key,
            const BytecodeCacheMetadata& meta,
            const std::vector<uint8_t>& bytecode);

        // Stats (atomic so GetStats is lock-free for the counters).
        mutable std::atomic<size_t> m_inlineCompiles{ 0 };
        mutable std::atomic<size_t> m_workerCompiles{ 0 };
        mutable std::atomic<size_t> m_cacheHits{ 0 };
        mutable std::atomic<size_t> m_cacheMisses{ 0 };
    };
}
