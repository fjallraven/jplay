#include "ProxyMode.h"

#include <mutex>

namespace jplay {

namespace {
std::mutex g_mtx;
std::string g_mode;
ProxyPathResolver g_resolver;
SlateFramesResolver g_slateResolver;
// Starts at 1 so a freshly-constructed Media (openedGeneration_ == 0) always
// treats itself as stale on its very first ensureOpen(), regardless of
// whether any mode has ever been set.
std::atomic<uint64_t> g_generation{1};
std::atomic<int64_t> g_slate{0};
} // namespace

int64_t proxySlateFrames() {
    return g_slate.load(std::memory_order_relaxed);
}

void setProxySlateFrames(int64_t n) {
    g_slate.store(n < 0 ? 0 : n, std::memory_order_relaxed);
}

void setSlateFramesResolver(SlateFramesResolver resolver) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_slateResolver = std::move(resolver);
}

int64_t pathSlateFrames(const std::string& path) {
    SlateFramesResolver resolver;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (!g_slateResolver)
            return 0;
        resolver = g_slateResolver;
    }
    const int64_t n = resolver(path);
    return n > 0 ? n : 0;
}

std::string currentProxyMode() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_mode;
}

void setProxyMode(std::string mode) {
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_mode = std::move(mode);
    }
    g_generation.fetch_add(1, std::memory_order_relaxed);
}

uint64_t proxyGeneration() {
    return g_generation.load(std::memory_order_relaxed);
}

void setProxyPathResolver(ProxyPathResolver resolver) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_resolver = std::move(resolver);
}

bool resolveProxyPath(const std::string& path, std::string& outPath) {
    std::string mode;
    ProxyPathResolver resolver;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (!g_resolver)
            return false;
        mode = g_mode;
        resolver = g_resolver;
    }
    // The empty ("Full") mode is passed through like any other: a clip's nominal
    // path is not necessarily the full-res representation — a project may have
    // been built against proxy paths — and only the resolver knows how to get
    // back. It answers false for a path that is already full-res, which is the
    // ordinary case and leaves the open exactly as it was.
    return resolver(path, mode, outPath);
}

} // namespace jplay
