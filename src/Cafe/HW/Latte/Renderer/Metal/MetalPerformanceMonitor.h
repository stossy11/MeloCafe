#pragma once

#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"

// These measure CPU time on the render thread, not GPU execution time.
class MetalScopedTimer
{
public:
    MetalScopedTimer(LattePerfStatTimer& timer, bool enabled) : m_timer(timer), m_enabled(enabled)
    {
        if (m_enabled)
            m_timer.beginMeasuring();
    }

    ~MetalScopedTimer()
    {
        if (m_enabled)
            m_timer.endMeasuring();
    }

    MetalScopedTimer(const MetalScopedTimer&) = delete;
    MetalScopedTimer& operator=(const MetalScopedTimer&) = delete;

private:
    LattePerfStatTimer& m_timer;
    bool m_enabled;
};

class MetalPerformanceMonitor
{
public:
    // Per frame data
    uint32 m_commandBuffers = 0;
    uint32 m_renderPasses = 0;
    uint32 m_clears = 0;
    uint32 m_manualVertexFetchDraws = 0;
    uint32 m_meshDraws = 0;
    uint32 m_triangleFans = 0;
    uint64 m_snapshotBytes = 0;
    uint32 m_snapshotReuses = 0;
    uint32 m_argumentBufferEncodes = 0;
    uint32 m_argumentBufferReuses = 0;
    uint32 m_residencyDeclarations = 0;
    uint32 m_residencySkips = 0;
    uint32 m_textureUploads = 0;
    uint64 m_textureUploadBytes = 0;
    uint32 m_queryCount = 0;
    uint32 m_readbackCount = 0;
    LattePerfStatTimer m_queryWait;
    LattePerfStatTimer m_flushWait;

    MetalPerformanceMonitor() = default;
    ~MetalPerformanceMonitor() = default;

    void ResetPerFrameData()
    {
        m_commandBuffers = 0;
        m_renderPasses = 0;
        m_clears = 0;
        m_manualVertexFetchDraws = 0;
        m_meshDraws = 0;
        m_triangleFans = 0;
        m_snapshotBytes = 0;
        m_snapshotReuses = 0;
        m_argumentBufferEncodes = 0;
        m_argumentBufferReuses = 0;
        m_residencyDeclarations = 0;
        m_residencySkips = 0;
        m_textureUploads = 0;
        m_textureUploadBytes = 0;
        m_queryCount = 0;
        m_readbackCount = 0;
    }
};
