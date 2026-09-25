#pragma once

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
    }
};
