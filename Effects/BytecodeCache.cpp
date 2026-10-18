#include "pch_engine.h"
#include "BytecodeCache.h"
#include "ShaderCompiler.h"
#include "ShaderIncludes.h"
#include "ShaderVariants.h"
#include "../Engine/Utf8.h"

#include <algorithm>
#include <chrono>

namespace ShaderLab::Effects
{
    // -----------------------------------------------------------------------
    // Free helpers (canonicalization + hashing)
    // -----------------------------------------------------------------------

    uint64_t FNV1a64(const void* data, size_t size)
    {
        // Standard 64-bit FNV-1a.
        constexpr uint64_t kOffset = 0xcbf29ce484222325ull;
        constexpr uint64_t kPrime  = 0x100000001b3ull;
        const auto* p = static_cast<const uint8_t*>(data);
        uint64_t h = kOffset;
        for (size_t i = 0; i < size; ++i)
        {
            h ^= p[i];
            h *= kPrime;
        }
        return h;
    }

    uint64_t HashCanonicalSource(std::string_view canonical)
    {
        return FNV1a64(canonical.data(), canonical.size());
    }

    uint64_t HashParamSignature(const std::vector<std::string>& orderedNames)
    {
        // Pack as "name0\0name1\0..." so order matters and "Foo|FooBar"
        // doesn't collide with "FooFooBar".
        std::string buf;
        buf.reserve(64);
        for (const auto& n : orderedNames)
        {
            buf.append(n);
            buf.push_back('\0');
        }
        return FNV1a64(buf.data(), buf.size());
    }

    uint64_t HashParamSignature(const std::vector<std::string>& gpuNames,
                                const std::vector<std::string>& optionNames)
    {
        if (optionNames.empty()) return HashParamSignature(gpuNames);
        // Pack as "gpu0\0gpu1\0\x01opt0\0opt1\0". The separator keeps a name
        // that moves from one list to the other from hashing the same.
        std::string buf;
        for (const auto& name : gpuNames)    { buf.append(name); buf.push_back('\0'); }
        buf.push_back('\x01');
        for (const auto& name : optionNames) { buf.append(name); buf.push_back('\0'); }
        return FNV1a64(buf.data(), buf.size());
    }

    uint64_t IncludeLibraryHashOf(std::string_view paramsText, std::string_view colorMathText,
                                  std::string_view gamutText)
    {
        // Cache schema version: bump when the cache layout or key fields
        // change in a way that should invalidate existing entries (in-memory
        // and on disk). Edits to the embedded includes or the compile flags
        // change the hash on their own.
        constexpr uint32_t kCacheSchemaVersion = 5;
        std::string buffer;
        buffer.reserve(paramsText.size() + colorMathText.size() + gamutText.size() + 32);
        auto appendSized = [&buffer](std::string_view text)
        {
            const uint64_t size = text.size();
            buffer.append(reinterpret_cast<const char*>(&size), sizeof(size));
            buffer.append(text);
        };
        buffer.append(reinterpret_cast<const char*>(&kCacheSchemaVersion), sizeof(kCacheSchemaVersion));
        buffer.append(reinterpret_cast<const char*>(&cShaderCompileFlags), sizeof(cShaderCompileFlags));
        appendSized(paramsText);
        appendSized(colorMathText);
        appendSized(gamutText);
        return FNV1a64(buffer.data(), buffer.size());
    }

    uint64_t IncludeLibraryHash()
    {
        // The includes are compiled into the binary, so hash them once.
        static const uint64_t sHash = []
        {
            const auto includes = EmbeddedIncludes();
            return IncludeLibraryHashOf(includes[0].text, includes[1].text, includes[2].text);
        }();
        return sHash;
    }

    static std::string CanonicalizeImpl(const char* src, size_t len)
    {
        // Normalize CRLF / CR-only line endings to LF. Don't strip
        // trailing whitespace or comments -- D3DCompile is sensitive
        // to source content via #line directives and cumulative output.
        std::string out;
        out.reserve(len);
        for (size_t i = 0; i < len; ++i)
        {
            char c = src[i];
            if (c == '\r')
            {
                out.push_back('\n');
                if (i + 1 < len && src[i + 1] == '\n') ++i;
            }
            else
            {
                out.push_back(c);
            }
        }
        return out;
    }

    std::string CanonicalizeHlslSource(std::string_view source)
    {
        return CanonicalizeImpl(source.data(), source.size());
    }

    std::string CanonicalizeHlslSource(std::wstring_view source)
    {
        const std::string utf8 = WideToUtf8(source);
        return CanonicalizeImpl(utf8.data(), utf8.size());
    }

    // -----------------------------------------------------------------------
    // BytecodeCompileKey
    // -----------------------------------------------------------------------

    bool BytecodeCompileKey::operator==(const BytecodeCompileKey& o) const noexcept
    {
        return sourceHash         == o.sourceHash
            && paramSignatureHash == o.paramSignatureHash
            && includeLibraryHash == o.includeLibraryHash
            && macroBitset        == o.macroBitset
            && optionKey          == o.optionKey
            && entryPoint         == o.entryPoint
            && target             == o.target;
    }

    size_t BytecodeCompileKeyHash::operator()(const BytecodeCompileKey& k) const noexcept
    {
        // Fold the four 64-bit hashes + macroBitset + entry/target into
        // a 64-bit then return as size_t. Cheap mixing; collisions
        // resolved by full equality.
        uint64_t h = k.sourceHash;
        auto mix = [&h](uint64_t v) {
            h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        };
        mix(k.paramSignatureHash);
        mix(k.includeLibraryHash);
        mix(static_cast<uint64_t>(k.macroBitset));
        mix(k.optionKey);
        mix(FNV1a64(k.entryPoint.data(), k.entryPoint.size()));
        mix(FNV1a64(k.target.data(),     k.target.size()));
        return static_cast<size_t>(h);
    }

    // Thread-local sentinel: set while a worker is processing a job.
    // Used by GetOrCompile to detect re-entrancy and skip waiting.
    // File-scope (not a class static) because TLS can't have dllexport.
    static thread_local bool t_isWorkerThread = false;

    // -----------------------------------------------------------------------
    // Lifetime
    // -----------------------------------------------------------------------

    BytecodeCache& BytecodeCache::Instance()
    {
        // Function-local static -- thread-safe initialization since C++11.
        static BytecodeCache instance;
        return instance;
    }

    BytecodeCache::BytecodeCache() = default;

    // Hosts call Shutdown first, so this normally finds no workers. A host
    // that did not reaches here at process exit, when Windows has already
    // ended the other threads and the join returns at once.
    BytecodeCache::~BytecodeCache()
    {
        Shutdown();
    }

    size_t BytecodeCache::WorkerCount() const
    {
        std::lock_guard<std::mutex> g(m_mutex);
        return m_workers.size();
    }

    void BytecodeCache::StartWorkers_NoLock()
    {
        if (!m_workers.empty() || m_shutdown.load()) return;
        // Half the cores, clamped to 2..6. Option variants are precompiled in
        // parallel here; the cap bounds D3DCompile's memory use and leaves a
        // core for the render thread.
        const size_t coreCount = (std::max)(std::thread::hardware_concurrency(), 1u);
        const size_t workerCount = (std::min)((std::max)(coreCount / 2, size_t{ 2 }), size_t{ 6 });
        m_workers.reserve(workerCount);
        for (size_t i = 0; i < workerCount; ++i)
        {
            m_workers.emplace_back([this](std::stop_token stop) {
                t_isWorkerThread = true;
                WorkerLoop(stop);
            });
        }
    }

    void BytecodeCache::Shutdown()
    {
        if (m_shutdown.exchange(true)) return;

        std::vector<std::jthread> workers;
        {
            std::lock_guard<std::mutex> g(m_mutex);
            // A dropped compile job's placeholder would read as compiling forever.
            // A queued key can also be compiling right now (inline in
            // GetOrCompile, or on a worker from a duplicate job); DoCompile holds
            // a reference to that entry with the lock dropped, so it stays and
            // the compile in flight completes it.
            for (const PendingWork& work : m_workQueue)
            {
                if (work.task) continue;
                auto it = m_entries.find(work.request.key);
                if (it != m_entries.end() &&
                    it->second.status == BytecodeStatus::Pending &&
                    it->second.compilesInFlight == 0)
                    m_entries.erase(it);
            }
            m_workQueue.clear();
            workers.swap(m_workers);
        }
        for (auto& w : workers)
            w.request_stop();
        m_workCv.notify_all();
        m_readyCv.notify_all();
        // Each jthread joins as `workers` goes out of scope.
    }

    // -----------------------------------------------------------------------
    // Lookup
    // -----------------------------------------------------------------------

    BytecodeCacheResult BytecodeCache::TryGet(const BytecodeCompileKey& key)
    {
        BytecodeCacheResult r;
        {
            std::lock_guard<std::mutex> g(m_mutex);
            auto it = m_entries.find(key);
            if (it != m_entries.end())
            {
                Materialize_NoLock(it->second);
                r.status       = it->second.status;
                r.bytecode     = it->second.bytecode;
                r.errorMessage = it->second.errorMessage;
                r.fromCache    = (r.status == BytecodeStatus::Ready);
                return r;
            }
        }
        // Disk fallback: hydrate the in-memory map if the file exists.
        std::vector<uint8_t> fromDisk;
        if (TryLoadFromDisk(GetDiskCacheRoot(), key, fromDisk))
        {
            std::lock_guard<std::mutex> g(m_mutex);
            // Re-check after acquiring the lock (another thread may have
            // populated the entry while we read the file).
            auto it = m_entries.find(key);
            if (it == m_entries.end())
            {
                Entry& e = m_entries[key];
                e.status         = BytecodeStatus::Ready;
                e.bytecode       = fromDisk;
                e.insertionOrder = m_nextInsertionOrder++;
                m_currentBytes += e.bytecode.size();
            }
            r.status    = BytecodeStatus::Ready;
            r.bytecode  = std::move(fromDisk);
            r.fromCache = true;
            return r;
        }
        r.status = BytecodeStatus::NotRequested;
        return r;
    }

    BytecodeStatus BytecodeCache::GetStatus(const BytecodeCompileKey& key) const
    {
        std::lock_guard<std::mutex> g(m_mutex);
        auto it = m_entries.find(key);
        return (it == m_entries.end()) ? BytecodeStatus::NotRequested : it->second.status;
    }

    // -----------------------------------------------------------------------
    // Enqueue
    // -----------------------------------------------------------------------

    void BytecodeCache::RequestCompile(BytecodeCompileRequest request, bool urgent)
    {
        if (m_shutdown.load()) return;

        {
            std::lock_guard<std::mutex> g(m_mutex);
            if (m_shutdown.load()) return;
            if (!request.allowPrebuiltCompile && m_prebuiltSources.contains(request.key.sourceHash))
                return;
            auto it = m_entries.find(request.key);
            if (it != m_entries.end())
            {
                // Already known -- no-op for Ready/Failed. Failure retry
                // requires explicit Invalidate. An urgent request for a
                // queued compile moves it to the front.
                if (urgent && it->second.status == BytecodeStatus::Pending)
                {
                    auto queued = std::find_if(m_workQueue.begin(), m_workQueue.end(),
                        [&](const PendingWork& work) { return work.request.key == request.key; });
                    if (queued != m_workQueue.end() && queued != m_workQueue.begin())
                    {
                        PendingWork work = std::move(*queued);
                        m_workQueue.erase(queued);
                        m_workQueue.push_front(std::move(work));
                    }
                }
                return;
            }
            // Insert a Pending placeholder so a subsequent identical
            // request from another thread short-circuits cleanly.
            Entry& e = m_entries[request.key];
            e.status         = BytecodeStatus::Pending;
            e.insertionOrder = m_nextInsertionOrder++;
            if (urgent) m_workQueue.push_front({ std::move(request), {} });
            else        m_workQueue.push_back({ std::move(request), {} });
            StartWorkers_NoLock();
        }
        m_workCv.notify_one();
    }

    void BytecodeCache::QueueReapDisk(uint64_t staleThresholdSec)
    {
        {
            std::lock_guard<std::mutex> g(m_mutex);
            if (m_shutdown.load()) return;
            m_workQueue.push_back({ {}, [this, staleThresholdSec] {
                const ReapResult result = ReapDisk(staleThresholdSec);
                std::lock_guard<std::mutex> resultLock(m_mutex);
                m_lastReap = result;
            } });
            StartWorkers_NoLock();
        }
        m_workCv.notify_one();
    }

    std::optional<BytecodeCache::ReapResult> BytecodeCache::LastReapResult() const
    {
        std::lock_guard<std::mutex> g(m_mutex);
        return m_lastReap;
    }

    std::optional<size_t> BytecodeCache::QueuePosition(const BytecodeCompileKey& key) const
    {
        std::lock_guard<std::mutex> g(m_mutex);
        for (size_t index = 0; index < m_workQueue.size(); ++index)
            if (m_workQueue[index].request.key == key) return index;
        return std::nullopt;
    }

    // -----------------------------------------------------------------------
    // Synchronous fetch-or-compile
    // -----------------------------------------------------------------------

    BytecodeCacheResult BytecodeCache::GetOrCompile(
        BytecodeCompileRequest request, uint32_t timeoutMs)
    {
        // Fast path: existing Ready/Failed entry in memory.
        {
            std::lock_guard<std::mutex> g(m_mutex);
            auto it = m_entries.find(request.key);
            if (it != m_entries.end())
            {
                Materialize_NoLock(it->second);
                if (it->second.status == BytecodeStatus::Ready)
                {
                    BytecodeCacheResult r;
                    r.status       = BytecodeStatus::Ready;
                    r.bytecode     = it->second.bytecode;
                    r.fromCache    = true;
                    m_cacheHits.fetch_add(1, std::memory_order_relaxed);
                    return r;
                }
                if (it->second.status == BytecodeStatus::Failed)
                {
                    BytecodeCacheResult r;
                    r.status       = BytecodeStatus::Failed;
                    r.errorMessage = it->second.errorMessage;
                    r.fromCache    = true;
                    m_cacheHits.fetch_add(1, std::memory_order_relaxed);
                    return r;
                }
                // Pending: fall through to wait/inline-compile path.
            }
        }

        // A build a prebuilt effect's blob lacks is not compiled here; the
        // evaluator falls back to one the blob has.
        if (!request.allowPrebuiltCompile && IsPrebuiltSource(request.key.sourceHash))
        {
            BytecodeCacheResult refused;
            refused.status = BytecodeStatus::NotRequested;
            refused.errorMessage = L"This build is not in the effect's prebuilt blob, and runtime compiles are off.";
            return refused;
        }

        // Disk fallback BEFORE inline compile -- a previous-session
        // bytecode is much cheaper than re-running D3DCompile.
        {
            std::vector<uint8_t> fromDisk;
            if (TryLoadFromDisk(GetDiskCacheRoot(), request.key, fromDisk))
            {
                std::lock_guard<std::mutex> g(m_mutex);
                auto it = m_entries.find(request.key);
                if (it == m_entries.end() ||
                    it->second.status == BytecodeStatus::Pending)
                {
                    Entry& e = m_entries[request.key];
                    if (e.insertionOrder == 0)
                        e.insertionOrder = m_nextInsertionOrder++;
                    if (e.bytecode.size() != fromDisk.size())
                        m_currentBytes += fromDisk.size() - e.bytecode.size();
                    e.bytecode = std::move(fromDisk);
                    e.status   = BytecodeStatus::Ready;
                    e.errorMessage.clear();
                }
                BytecodeCacheResult r;
                r.status    = BytecodeStatus::Ready;
                r.bytecode  = m_entries[request.key].bytecode;
                r.fromCache = true;
                m_cacheHits.fetch_add(1, std::memory_order_relaxed);
                m_readyCv.notify_all();
                return r;
            }
        }

        m_cacheMisses.fetch_add(1, std::memory_order_relaxed);

        // Wait briefly for an in-flight worker compile to finish, but
        // not if we ARE a worker (would deadlock if a compile callback
        // re-entered the cache).
        if (timeoutMs > 0 && !t_isWorkerThread)
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(timeoutMs);
            while (true)
            {
                auto it = m_entries.find(request.key);
                if (it != m_entries.end() &&
                    (it->second.status == BytecodeStatus::Ready ||
                     it->second.status == BytecodeStatus::Failed))
                {
                    BytecodeCacheResult r;
                    r.status       = it->second.status;
                    r.bytecode     = it->second.bytecode;
                    r.errorMessage = it->second.errorMessage;
                    r.fromCache    = true;
                    return r;
                }
                if (m_readyCv.wait_until(lock, deadline) == std::cv_status::timeout)
                    break;
            }
        }

        // Inline compile fallback. Place a Pending placeholder if not
        // already present, then compile on this thread.
        Entry localEntry;
        BytecodeStatus finalStatus;
        BytecodeCacheMetadata metaCopy;
        BytecodeCompileKey    keyCopy;
        std::vector<uint8_t>  bytecodeForDisk;
        std::wstring          diskRoot;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            auto& e = m_entries[request.key];
            if (e.status == BytecodeStatus::Ready ||
                e.status == BytecodeStatus::Failed)
            {
                BytecodeCacheResult r;
                r.status       = e.status;
                r.bytecode     = e.bytecode;
                r.errorMessage = e.errorMessage;
                r.fromCache    = true;
                return r;
            }
            if (e.insertionOrder == 0)
                e.insertionOrder = m_nextInsertionOrder++;
            finalStatus      = DoCompile(request, lock, e);
            localEntry       = e;
            keyCopy          = request.key;
            metaCopy         = request.metadata;
            if (finalStatus == BytecodeStatus::Ready)
                bytecodeForDisk = e.bytecode;
            diskRoot         = m_diskRoot;
            EnforceByteLimit_NoLock();
        }
        m_inlineCompiles.fetch_add(1, std::memory_order_relaxed);
        m_readyCv.notify_all();

        // Persist without the lock (best-effort).
        if (!bytecodeForDisk.empty())
            WriteToDisk(diskRoot, keyCopy, metaCopy, bytecodeForDisk);

        BytecodeCacheResult r;
        r.status       = finalStatus;
        r.bytecode     = std::move(localEntry.bytecode);
        r.errorMessage = std::move(localEntry.errorMessage);
        r.fromCache    = false;
        return r;
    }

    // -----------------------------------------------------------------------
    // Invalidate / Clear / Stats
    // -----------------------------------------------------------------------

    void BytecodeCache::AddPrebuilt(const BytecodeCompileKey& key, PrebuiltLoader load)
    {
        std::lock_guard<std::mutex> g(m_mutex);
        m_prebuiltSources.insert(key.sourceHash);
        auto& entry = m_entries[key];
        if (entry.compilesInFlight > 0) return;   // a worker is filling it; its result stands
        if (entry.status == BytecodeStatus::Ready && entry.pinned) return;   // registered already
        m_currentBytes -= entry.bytecode.size();
        entry.status = BytecodeStatus::Ready;
        entry.bytecode.clear();
        entry.errorMessage.clear();
        entry.pinned = true;
        entry.load = std::move(load);
        entry.insertionOrder = m_nextInsertionOrder++;
    }

    void BytecodeCache::Materialize_NoLock(Entry& entry)
    {
        if (!entry.load) return;
        entry.bytecode = entry.load();
        entry.load = nullptr;
        if (entry.bytecode.empty())
        {
            entry.status = BytecodeStatus::Failed;
            entry.errorMessage = L"A prebuilt build in the effect's blob does not decompress.";
            return;
        }
        m_currentBytes += entry.bytecode.size();
    }

    bool BytecodeCache::IsPrebuiltSource(uint64_t sourceHash) const
    {
        std::lock_guard<std::mutex> g(m_mutex);
        return m_prebuiltSources.contains(sourceHash);
    }

    void BytecodeCache::Invalidate(const BytecodeCompileKey& key)
    {
        std::lock_guard<std::mutex> g(m_mutex);
        auto it = m_entries.find(key);
        if (it == m_entries.end()) return;
        if (it->second.status == BytecodeStatus::Pending || it->second.pinned)
        {
            // Don't yank an in-flight entry out from under a worker, or a
            // prebuilt one that nothing could rebuild.
            return;
        }
        m_currentBytes -= it->second.bytecode.size();
        m_entries.erase(it);
    }

    void BytecodeCache::Clear()
    {
        std::lock_guard<std::mutex> g(m_mutex);
        // Don't drop Pending entries -- workers may be mid-compile -- or
        // prebuilt ones.
        for (auto it = m_entries.begin(); it != m_entries.end(); )
        {
            if (it->second.status == BytecodeStatus::Pending || it->second.pinned)
            {
                ++it;
            }
            else
            {
                m_currentBytes -= it->second.bytecode.size();
                it = m_entries.erase(it);
            }
        }
    }

    // -----------------------------------------------------------------------
    // Disk persistence
    // -----------------------------------------------------------------------
    //
    // Layout: <root>\<effectIdSafe>\<version>\<keyHashHex>.cso
    //
    // The keyHash combines all bytecode-determining fields into a single
    // 64-bit value: source + paramSig + includeLib + macroBitset + entry +
    // target. Two metadata fields (effectIdSafe, version) live in the
    // path purely so a human can inspect the cache and so the reaper can
    // find stale-version directories cheaply.

    namespace {
        std::wstring SanitizeForFilesystem(std::wstring_view in)
        {
            std::wstring out;
            out.reserve(in.size());
            for (wchar_t c : in)
            {
                if ((c >= L'A' && c <= L'Z') ||
                    (c >= L'a' && c <= L'z') ||
                    (c >= L'0' && c <= L'9') ||
                    c == L'_' || c == L'-' || c == L'.')
                    out.push_back(c);
                else
                    out.push_back(L'_');
            }
            // Cap length to keep paths well under MAX_PATH even for
            // deep cache trees. 64 chars is plenty for human-readable
            // effect names; collisions resolved by keyHash.
            if (out.size() > 64) out.resize(64);
            if (out.empty()) out = L"_unnamed";
            return out;
        }

        std::wstring HexFormat(uint64_t v)
        {
            wchar_t buf[17];
            swprintf_s(buf, L"%016llx", static_cast<unsigned long long>(v));
            return buf;
        }

        bool EnsureDirectoryRecursive(const std::wstring& path)
        {
            // CreateDirectoryW fails if the path doesn't exist or any
            // parent is missing. Walk up and create as we go.
            if (path.empty()) return false;
            // Quick check: already exists?
            DWORD attrs = ::GetFileAttributesW(path.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES &&
                (attrs & FILE_ATTRIBUTE_DIRECTORY))
                return true;

            // Walk parent.
            size_t slash = path.find_last_of(L"\\/");
            if (slash != std::wstring::npos && slash > 2)  // skip drive letter
            {
                if (!EnsureDirectoryRecursive(path.substr(0, slash)))
                    return false;
            }
            BOOL ok = ::CreateDirectoryW(path.c_str(), nullptr);
            if (!ok)
            {
                DWORD err = ::GetLastError();
                if (err == ERROR_ALREADY_EXISTS) return true;
                return false;
            }
            return true;
        }
    }

    void BytecodeCache::SetDiskCacheRoot(std::wstring rootPath)
    {
        if (!rootPath.empty())
            EnsureDirectoryRecursive(rootPath);
        std::lock_guard<std::mutex> g(m_mutex);
        m_diskRoot = std::move(rootPath);
    }

    std::wstring BytecodeCache::GetDiskCacheRoot() const
    {
        std::lock_guard<std::mutex> g(m_mutex);
        return m_diskRoot;
    }

    std::wstring BytecodeCache::KeyToDiskPath(
        const std::wstring& root,
        const BytecodeCompileKey& key,
        const BytecodeCacheMetadata& meta)
    {
        if (root.empty()) return {};

        // Single 64-bit hash of the full key for the filename.
        BytecodeCompileKeyHash hasher;
        const uint64_t keyHash = static_cast<uint64_t>(hasher(key));

        std::wstring effectIdSafe = SanitizeForFilesystem(
            meta.effectId.empty() ? L"_unknown" : meta.effectId);

        std::wstring path = root;
        if (!path.empty() && path.back() != L'\\') path.push_back(L'\\');
        path += effectIdSafe;
        path.push_back(L'\\');
        path += std::to_wstring(meta.version);
        path.push_back(L'\\');
        path += HexFormat(keyHash);
        path += L".cso";
        return path;
    }

    bool BytecodeCache::TryLoadFromDisk(
        const std::wstring& root,
        const BytecodeCompileKey& key,
        std::vector<uint8_t>& outBytecode)
    {
        // The on-disk filename is keyed by the FULL hash of all
        // bytecode-determining fields. We don't have effectId/version
        // for keys not in memory -- the disk layout uses metadata for
        // human-readable hierarchy, but lookup is by hash so we need
        // to know the directory. To keep `TryGet` cheap, we walk all
        // immediate `<effectIdSafe>/<version>/` children looking for
        // a file named `<keyHashHex>.cso`. With a typical cache of
        // 5-50 effects, that's a fast operation.
        if (root.empty()) return false;
        BytecodeCompileKeyHash hasher;
        std::wstring fname = HexFormat(static_cast<uint64_t>(hasher(key))) + L".cso";

        WIN32_FIND_DATAW fdEffect{};
        std::wstring effectGlob = root;
        if (!effectGlob.empty() && effectGlob.back() != L'\\') effectGlob.push_back(L'\\');
        effectGlob += L"*";
        HANDLE hEffect = ::FindFirstFileW(effectGlob.c_str(), &fdEffect);
        if (hEffect == INVALID_HANDLE_VALUE) return false;
        bool found = false;
        std::wstring foundPath;
        do {
            if (!(fdEffect.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (fdEffect.cFileName[0] == L'.') continue;
            std::wstring effectDir = root;
            if (effectDir.back() != L'\\') effectDir.push_back(L'\\');
            effectDir += fdEffect.cFileName;

            WIN32_FIND_DATAW fdVer{};
            std::wstring verGlob = effectDir + L"\\*";
            HANDLE hVer = ::FindFirstFileW(verGlob.c_str(), &fdVer);
            if (hVer == INVALID_HANDLE_VALUE) continue;
            do {
                if (!(fdVer.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (fdVer.cFileName[0] == L'.') continue;
                std::wstring candidate = effectDir + L"\\" + fdVer.cFileName + L"\\" + fname;
                DWORD a = ::GetFileAttributesW(candidate.c_str());
                if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY))
                {
                    foundPath = std::move(candidate);
                    found = true;
                    break;
                }
            } while (::FindNextFileW(hVer, &fdVer));
            ::FindClose(hVer);
            if (found) break;
        } while (::FindNextFileW(hEffect, &fdEffect));
        ::FindClose(hEffect);

        if (!found) return false;

        // Read the file.
        HANDLE hFile = ::CreateFileW(foundPath.c_str(), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER sz{};
        if (!::GetFileSizeEx(hFile, &sz) || sz.QuadPart <= 0 ||
            sz.QuadPart > 16ull * 1024 * 1024)  // 16 MB sanity cap
        {
            ::CloseHandle(hFile);
            return false;
        }
        outBytecode.resize(static_cast<size_t>(sz.QuadPart));
        DWORD got = 0;
        BOOL ok = ::ReadFile(hFile, outBytecode.data(),
            static_cast<DWORD>(outBytecode.size()), &got, nullptr);
        ::CloseHandle(hFile);
        if (!ok || got != outBytecode.size())
        {
            outBytecode.clear();
            return false;
        }
        // Touch atime so the reaper sees the load. Use SetFileTime with
        // current FILETIME on the LastAccessTime field.
        FILETIME now{};
        ::GetSystemTimeAsFileTime(&now);
        HANDLE hTouch = ::CreateFileW(foundPath.c_str(), FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hTouch != INVALID_HANDLE_VALUE)
        {
            ::SetFileTime(hTouch, nullptr, &now, &now);
            ::CloseHandle(hTouch);
        }
        return true;
    }

    void BytecodeCache::WriteToDisk(
        const std::wstring& root,
        const BytecodeCompileKey& key,
        const BytecodeCacheMetadata& meta,
        const std::vector<uint8_t>& bytecode)
    {
        if (root.empty() || bytecode.empty()) return;
        std::wstring path = KeyToDiskPath(root, key, meta);
        if (path.empty()) return;

        // Create parent dir.
        size_t slash = path.find_last_of(L'\\');
        if (slash == std::wstring::npos) return;
        if (!EnsureDirectoryRecursive(path.substr(0, slash))) return;

        // Atomic write: temp file in parent dir + MoveFileEx replace. The temp
        // name is per thread: two threads can persist the same key at once.
        std::wstring tmp = path + L"." + std::to_wstring(::GetCurrentThreadId()) + L".tmp";
        HANDLE hFile = ::CreateFileW(tmp.c_str(), GENERIC_WRITE,
            0, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        BOOL ok = ::WriteFile(hFile, bytecode.data(),
            static_cast<DWORD>(bytecode.size()), &written, nullptr);
        ::CloseHandle(hFile);
        if (!ok || written != bytecode.size())
        {
            ::DeleteFileW(tmp.c_str());
            return;
        }
        if (!::MoveFileExW(tmp.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            ::DeleteFileW(tmp.c_str());
        }
    }

    BytecodeCache::ReapResult BytecodeCache::ReapDisk(uint64_t staleThresholdSec)
    {
        ReapResult r{};
        std::wstring root;
        {
            std::lock_guard<std::mutex> g(m_mutex);
            root = m_diskRoot;
        }
        if (root.empty()) return r;

        // FILETIME = 100ns since 1601. Threshold: now - staleThresholdSec.
        // Special case: staleThresholdSec == 0 means "reap everything"
        // (force threshold above all possible atimes).
        FILETIME nowFt{};
        ::GetSystemTimeAsFileTime(&nowFt);
        ULARGE_INTEGER nowUli; nowUli.LowPart = nowFt.dwLowDateTime;
        nowUli.HighPart = nowFt.dwHighDateTime;
        ULONGLONG thresholdUli;
        const bool reapAll = (staleThresholdSec == 0);
        if (reapAll)
            thresholdUli = ~0ull;  // any atime is < this; everything reaped.
        else
            thresholdUli = nowUli.QuadPart -
                (static_cast<ULONGLONG>(staleThresholdSec) * 10'000'000ull);

        // Walk root\effectIdSafe\version\*.cso
        WIN32_FIND_DATAW fdE{};
        std::wstring effectGlob = root;
        if (effectGlob.back() != L'\\') effectGlob.push_back(L'\\');
        effectGlob += L"*";
        HANDLE hE = ::FindFirstFileW(effectGlob.c_str(), &fdE);
        if (hE == INVALID_HANDLE_VALUE) return r;
        do {
            if (!(fdE.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (fdE.cFileName[0] == L'.') continue;
            std::wstring effectDir = root;
            if (effectDir.back() != L'\\') effectDir.push_back(L'\\');
            effectDir += fdE.cFileName;

            WIN32_FIND_DATAW fdV{};
            std::wstring vGlob = effectDir + L"\\*";
            HANDLE hV = ::FindFirstFileW(vGlob.c_str(), &fdV);
            if (hV == INVALID_HANDLE_VALUE) continue;
            do {
                if (!(fdV.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (fdV.cFileName[0] == L'.') continue;
                std::wstring vDir = effectDir + L"\\" + fdV.cFileName;
                WIN32_FIND_DATAW fdF{};
                std::wstring fGlob = vDir + L"\\*.cso";
                HANDLE hF = ::FindFirstFileW(fGlob.c_str(), &fdF);
                if (hF == INVALID_HANDLE_VALUE) continue;
                do {
                    ULARGE_INTEGER atime;
                    atime.LowPart  = fdF.ftLastAccessTime.dwLowDateTime;
                    atime.HighPart = fdF.ftLastAccessTime.dwHighDateTime;
                    if (atime.QuadPart >= thresholdUli) continue;

                    std::wstring full = vDir + L"\\" + fdF.cFileName;
                    LARGE_INTEGER sz{};
                    sz.LowPart  = fdF.nFileSizeLow;
                    sz.HighPart = fdF.nFileSizeHigh;
                    if (::DeleteFileW(full.c_str()))
                    {
                        ++r.filesDeleted;
                        r.bytesFreed += static_cast<size_t>(sz.QuadPart);
                    }
                    else
                    {
                        ++r.errors;
                    }
                } while (::FindNextFileW(hF, &fdF));
                ::FindClose(hF);
                // Try to rmdir empty version dir (best-effort).
                ::RemoveDirectoryW(vDir.c_str());
            } while (::FindNextFileW(hV, &fdV));
            ::FindClose(hV);
            ::RemoveDirectoryW(effectDir.c_str());
        } while (::FindNextFileW(hE, &fdE));
        ::FindClose(hE);
        return r;
    }

    BytecodeCache::ReapResult BytecodeCache::ClearDisk()
    {
        // staleThresholdSec=0 is the "reap everything" sentinel.
        return ReapDisk(0);
    }

    BytecodeCache::Stats BytecodeCache::GetStats() const
    {
        Stats s{};
        {
            std::lock_guard<std::mutex> g(m_mutex);
            s.entries            = m_entries.size();
            s.totalBytecodeBytes = m_currentBytes;
            for (const auto& [k, e] : m_entries)
            {
                if (e.status == BytecodeStatus::Pending) ++s.pendingCount;
                else if (e.status == BytecodeStatus::Failed) ++s.failedCount;
            }
        }
        s.inlineCompiles = m_inlineCompiles.load(std::memory_order_relaxed);
        s.workerCompiles = m_workerCompiles.load(std::memory_order_relaxed);
        s.cacheHits      = m_cacheHits.load(std::memory_order_relaxed);
        s.cacheMisses    = m_cacheMisses.load(std::memory_order_relaxed);
        return s;
    }

    // -----------------------------------------------------------------------
    // Worker loop + compile core
    // -----------------------------------------------------------------------

    void BytecodeCache::WorkerLoop(std::stop_token stop)
    {
        while (!stop.stop_requested() && !m_shutdown.load())
        {
            PendingWork work;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_workCv.wait(lock, [&] {
                    return stop.stop_requested() || m_shutdown.load() || !m_workQueue.empty();
                });
                if (stop.stop_requested() || m_shutdown.load()) return;
                if (m_workQueue.empty()) continue;
                work = std::move(m_workQueue.front());
                m_workQueue.pop_front();
            }
            if (work.task)
            {
                work.task();
                continue;
            }
            const BytecodeCompileRequest& req = work.request;

            // Compile outside the queue mutex.
            std::unique_lock<std::mutex> lock(m_mutex);
            auto it = m_entries.find(req.key);
            if (it == m_entries.end())
            {
                // Got Invalidated between enqueue and dequeue -- skip.
                continue;
            }
            // If the inline-compile path already filled this entry while
            // we were dequeueing, leave it alone.
            if (it->second.status == BytecodeStatus::Ready ||
                it->second.status == BytecodeStatus::Failed)
            {
                continue;
            }

            // A reference, not the iterator: DoCompile drops the lock, and an
            // insert meanwhile can rehash, which moves no node but invalidates
            // iterators.
            Entry& entry = it->second;
            DoCompile(req, lock, entry);
            m_workerCompiles.fetch_add(1, std::memory_order_relaxed);

            // Copy what to persist before the byte limit can evict the entry,
            // then write without the lock (best-effort).
            std::vector<uint8_t> bytecodeForDisk;
            if (entry.status == BytecodeStatus::Ready && !m_diskRoot.empty())
                bytecodeForDisk = entry.bytecode;
            const std::wstring diskRoot = m_diskRoot;
            EnforceByteLimit_NoLock();

            lock.unlock();
            m_readyCv.notify_all();
            if (!bytecodeForDisk.empty())
                WriteToDisk(diskRoot, req.key, req.metadata, bytecodeForDisk);
        }
    }

    BytecodeStatus BytecodeCache::DoCompile(
        const BytecodeCompileRequest& req,
        std::unique_lock<std::mutex>& lock,
        Entry& outEntry)
    {
        // ShaderVariants owns the macro spellings. The strings must outlive D3DCompile.
        std::vector<MacroPair> macroPairs = BuildVariantMacros(
            req.gpuBindableParamNames, req.key.macroBitset, req.optionParamNames, req.key.optionKey);
        macroPairs.insert(macroPairs.end(), req.fieldMacros.begin(), req.fieldMacros.end());
        std::vector<ShaderCompiler::MacroDef> macros;
        macros.reserve(macroPairs.size());
        for (const auto& [name, value] : macroPairs)
            macros.push_back({ name.c_str(), value.c_str() });

        // Drop the lock around D3DCompile -- it can take 50-300ms in
        // debug. Other readers can still TryGet the (Pending) entry,
        // and workers can pick up other queue items.
        std::string hlsl = req.hlslSource;
        std::string entry = req.key.entryPoint;
        std::string target = req.key.target;
        ++outEntry.compilesInFlight;
        lock.unlock();

        ShaderCompileResult result = ShaderCompiler::CompileFromString(
            hlsl, "BytecodeCache", entry, target, macros);

        lock.lock();
        --outEntry.compilesInFlight;
        // Race check: if the entry was wiped or completed under us,
        // discard our result. Specifically, NEVER overwrite a Ready
        // entry with a Failed result (or any worker result with a
        // Ready inline-compile result that beat us).
        // outEntry is a reference into m_entries[req.key]; the entry's
        // status drives the merge.
        if (outEntry.status == BytecodeStatus::Ready ||
            outEntry.status == BytecodeStatus::Failed)
        {
            return outEntry.status;
        }

        if (result.bytecode)
        {
            const uint8_t* p = static_cast<const uint8_t*>(result.bytecode->GetBufferPointer());
            size_t sz = result.bytecode->GetBufferSize();
            outEntry.bytecode.assign(p, p + sz);
            outEntry.status = BytecodeStatus::Ready;
            outEntry.errorMessage.clear();
            m_currentBytes += sz;
            return BytecodeStatus::Ready;
        }

        outEntry.status = BytecodeStatus::Failed;
        outEntry.errorMessage = result.ErrorMessage();
        outEntry.bytecode.clear();
        if (outEntry.errorMessage.empty())
            outEntry.errorMessage = L"D3DCompile failed (no diagnostic).";
        return BytecodeStatus::Failed;
    }

    void BytecodeCache::EnforceByteLimit_NoLock()
    {
        if (m_currentBytes <= m_maxBytes) return;

        // Naive LRU on insertion order. Build a vector of
        // (insertionOrder, key) for non-Pending entries, sort by
        // order ascending, evict from front until we're back under cap.
        struct Victim { uint64_t order; BytecodeCompileKey key; };
        std::vector<Victim> victims;
        victims.reserve(m_entries.size());
        for (auto& [k, e] : m_entries)
        {
            if (e.status == BytecodeStatus::Pending || e.pinned) continue;
            victims.push_back({ e.insertionOrder, k });
        }
        std::sort(victims.begin(), victims.end(),
            [](const Victim& a, const Victim& b) { return a.order < b.order; });

        for (auto& v : victims)
        {
            if (m_currentBytes <= m_maxBytes) break;
            auto it = m_entries.find(v.key);
            if (it == m_entries.end()) continue;
            m_currentBytes -= it->second.bytecode.size();
            m_entries.erase(it);
        }
    }
}
