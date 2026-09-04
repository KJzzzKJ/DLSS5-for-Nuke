#pragma once
#include "Protocol.h"
#include <wrl/client.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <vector>
#include <string>
#include <cstdint>
#include "DISOpticalFlow.h"

using Microsoft::WRL::ComPtr;

using NGXResult = int;
struct NGXHandle { unsigned int Id; };

struct NGXParameter {
    virtual void Set(const char*, unsigned long long) = 0;
    virtual void Set(const char*, float) = 0;
    virtual void Set(const char*, double) = 0;
    virtual void Set(const char*, unsigned int) = 0;
    virtual void Set(const char*, int) = 0;
    virtual void Set(const char*, ID3D11Resource*) = 0;
    virtual void Set(const char*, ID3D12Resource*) = 0;
    virtual void Set(const char*, void*) = 0;
    virtual NGXResult Get(const char*, unsigned long long*) const = 0;
    virtual NGXResult Get(const char*, float*) const = 0;
    virtual NGXResult Get(const char*, double*) const = 0;
    virtual NGXResult Get(const char*, unsigned int*) const = 0;
    virtual NGXResult Get(const char*, int*) const = 0;
    virtual NGXResult Get(const char*, ID3D11Resource**) const = 0;
    virtual NGXResult Get(const char*, ID3D12Resource**) const = 0;
    virtual NGXResult Get(const char*, void**) const = 0;
    virtual void Reset() = 0;
};

class DLSSWorker {
public:
    DLSSWorker()  = default;
    ~DLSSWorker() { shutdown(); }

    // Initialize D3D12 + NGX Feature 18 (DLSS-NR)
    bool init(const VideoHeader& hdr);

    // Process one frame with RGBA8 legacy or linear RGBA16F input and RG16F motion vectors.
    bool processFrame(uint32_t idx, bool reset, int64_t pts,
                      const uint8_t* rgba_in, const uint8_t* motion_in,
                      const float* depth_in, const float* control_mask_in,
                      std::vector<uint8_t>& rgba_out);

    const SetupResponse& getSetup() const { return m_setup; }
    const std::string& getLastError() const { return m_lastError; }

private:
    void shutdown();
    bool setupD3D12();
    bool allocateResources(uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH);
    bool initNGX(const VideoHeader& hdr);
    bool executeAndWait();
    void waitGPU();
    void setCommonParams(bool reset);

    // D3D12 core
    ComPtr<ID3D12Device>              m_device;
    ComPtr<ID3D12CommandQueue>        m_queue;
    ComPtr<ID3D12CommandAllocator>    m_cmdAlloc;
    ComPtr<ID3D12GraphicsCommandList> m_cmdList;
    ComPtr<ID3D12Fence>               m_fence;
    HANDLE                            m_fenceEvent = nullptr;
    UINT64                            m_fenceVal   = 0;

    // GPU Textures
    ComPtr<ID3D12Resource>            m_colorTex;    // RGBA16F (inW x inH)
    ComPtr<ID3D12Resource>            m_outputTex;   // RGBA16F (outW x outH)
    ComPtr<ID3D12Resource>            m_mvTex;       // RG16F   (inW x inH)
    ComPtr<ID3D12Resource>            m_depthTex;    // R32F    (inW x inH)
    ComPtr<ID3D12Resource>            m_controlMaskTex; // R32F (inW x inH)

    // CPU<->GPU Staging Buffers
    ComPtr<ID3D12Resource>            m_uploadBuf;   // CPU->GPU RGBA16F
    ComPtr<ID3D12Resource>            m_mvUploadBuf; // CPU->GPU RG16F
    ComPtr<ID3D12Resource>            m_depthUploadBuf; // CPU->GPU R32F
    ComPtr<ID3D12Resource>            m_controlMaskUploadBuf; // CPU->GPU R32F
    ComPtr<ID3D12Resource>            m_readbackBuf; // GPU->CPU RGBA16F

    UINT   m_colorPitch    = 0;
    UINT   m_mvPitch       = 0;
    UINT   m_guidePitch    = 0;
    UINT   m_readbackPitch = 0;
    UINT64 m_uploadBytes   = 0;
    UINT64 m_mvBytes       = 0;
    UINT64 m_guideBytes    = 0;
    UINT64 m_readbackBytes = 0;

    // NGX Snippet & Shim Function Pointers
    HMODULE m_coreMod  = nullptr;
    HMODULE m_nrMod    = nullptr;
    HMODULE m_shimMod  = nullptr;

    // Feature 18 (DLSS-NR)
    NGXParameter* m_params  = nullptr;
    NGXHandle*    m_feature = nullptr;

    // Feature 1 (DLSS-SR: Super Resolution) for Two-Pass Pipeline
    NGXParameter*          m_srParams    = nullptr;
    NGXHandle*             m_srFeature   = nullptr;
    ComPtr<ID3D12Resource> m_intermediateTex; // RGBA16F (inW x inH) for Pass 1 output
    bool                   m_needUpscale = false;

    // Dimensions and State
    uint32_t      m_inW  = 0;
    uint32_t      m_inH  = 0;
    uint32_t      m_outW = 0;
    uint32_t      m_outH = 0;
    VideoHeader   m_hdr  = {};
    SetupResponse m_setup = {};
    std::string   m_lastError;
    bool          m_initialized = false;
    bool          m_legacyRgba8 = false;

    // Optical Flow (DIS) State
    DISOpticalFlow       m_dis;
    std::vector<uint8_t> m_prev_gray;
    int                  m_flow_width = 640;
    int                  m_flow_height = 360;
};
