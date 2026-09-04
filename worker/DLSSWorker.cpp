#include "DLSSWorker.h"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <vector>
#include <string>

using InitExtFn = NGXResult(__cdecl*)(unsigned long long, const wchar_t*, ID3D12Device*, int, const void*);
using InitProjectIdFn = NGXResult(__cdecl*)(const char*, int, const char*, const wchar_t*, ID3D12Device*, int, const void*);
using AllocParamsFn = NGXResult(__cdecl*)(NGXParameter**);
using DestroyParamsFn = NGXResult(__cdecl*)(NGXParameter*);
using CreateFeatureFn = NGXResult(__cdecl*)(ID3D12GraphicsCommandList*, int, NGXParameter*, NGXHandle**);
using EvaluateFeatureFn = NGXResult(__cdecl*)(ID3D12GraphicsCommandList*, const NGXHandle*, const NGXParameter*, void*);
using ReleaseFeatureFn = NGXResult(__cdecl*)(NGXHandle*);
using ShutdownFn = NGXResult(__cdecl*)();

using SnippetInitFn = NGXResult(__cdecl*)(unsigned long long, const wchar_t*, ID3D12Device*, const void*, int);

using ShimInitFn = NGXResult(__cdecl*)(void*, unsigned long long, const wchar_t*, ID3D12Device*, int, const void*);
using ShimCreateFn = NGXResult(__cdecl*)(void*, ID3D12GraphicsCommandList*, int, NGXParameter*, NGXHandle**);
using ShimEvaluateFn = NGXResult(__cdecl*)(void*, ID3D12GraphicsCommandList*, const NGXHandle*, const NGXParameter*, void*);
using ShimReleaseFn = NGXResult(__cdecl*)(void*, NGXHandle*);
using ShimShutdownFn = NGXResult(__cdecl*)(void*);

static constexpr unsigned long long APP_ID = 141959980ULL;
static constexpr const char* PROJECT_ID = "53f803cc-a12f-4d69-90d5-19b7599cad19";
static constexpr int NR_FEATURE_ID = 18;

struct NGXPathListInfo {
    wchar_t const* const* Path;
    unsigned int Length;
};
enum NGXLoggingLevel { NGX_LOG_OFF = 0, NGX_LOG_ON = 1, NGX_LOG_VERBOSE = 2 };
using NGXLogCallback = void(__cdecl*)(const char*, NGXLoggingLevel, int);
struct NGXLoggingInfo {
    NGXLoggingLevel LoggingLevel;
    NGXLogCallback Callback;
    void* UserData;
    bool DisableOtherLoggingSinks;
};
struct NGXFeatureCommonInfoInternal;
struct NGXFeatureCommonInfo {
    NGXPathListInfo PathListInfo;
    NGXFeatureCommonInfoInternal* InternalData;
    NGXLoggingInfo LoggingInfo;
};

static inline UINT AlignUp(UINT val, UINT alignment) {
    return (val + alignment - 1) & ~(alignment - 1);
}

static inline uint16_t FloatToHalf(float f) {
    uint32_t x; memcpy(&x, &f, sizeof(x));
    uint32_t s = (x >> 16) & 0x8000u;
    int32_t e = static_cast<int32_t>((x >> 23) & 0xff) - 127 + 15;
    uint32_t m = x & 0x7fffffu;
    if (e <= 0) {
        if (e < -10) return static_cast<uint16_t>(s);
        m = (m | 0x800000u) >> (1 - e);
        return static_cast<uint16_t>(s | (m >> 13));
    }
    if (e >= 31) return static_cast<uint16_t>(s | 0x7c00u);
    return static_cast<uint16_t>(s | (static_cast<uint32_t>(e) << 10) | (m >> 13));
}

static inline float HalfToFloat(uint16_t h) {
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1f, m = h & 0x3ff, x;
    if (e == 0) {
        if (m == 0) x = s << 31;
        else {
            e = 1;
            while (!(m & 0x400)) { m <<= 1; --e; }
            m &= 0x3ff;
            x = (s << 31) | ((e + 112) << 23) | (m << 13);
        }
    } else if (e == 0x1f) x = (s << 31) | 0x7f800000u | (m << 13);
    else x = (s << 31) | ((e + 112) << 23) | (m << 13);
    float f; memcpy(&f, &x, sizeof(f)); return f;
}

static inline D3D12_RESOURCE_BARRIER Barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

static ComPtr<ID3D12Resource> CreateTexture(ID3D12Device* dev, UINT w, UINT h, DXGI_FORMAT fmt, D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = flags;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> r;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

static ComPtr<ID3D12Resource> CreateLinearBuffer(ID3D12Device* dev, UINT64 bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = type;
    ComPtr<ID3D12Resource> r;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

static bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static HMODULE LoadCoreNGX(const std::wstring& runtime) {
    std::wstring local = runtime + L"\\_nvngx.dll";
    if (FileExists(local)) {
        if (HMODULE m = LoadLibraryW(local.c_str())) return m;
    }
    if (HMODULE m = LoadLibraryW(L"_nvngx.dll")) return m;

    wchar_t winDir[MAX_PATH] = {};
    GetWindowsDirectoryW(winDir, MAX_PATH);
    std::wstring repo = std::wstring(winDir) + L"\\System32\\DriverStore\\FileRepository";
    std::wstring pat = repo + L"\\nv*.inf_*";

    struct Cand { std::wstring path; ULARGE_INTEGER stamp; };
    std::vector<Cand> cands;

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            std::wstring candidate = repo + L"\\" + fd.cFileName + L"\\_nvngx.dll";
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (GetFileAttributesExW(candidate.c_str(), GetFileExInfoStandard, &fad) &&
                !(fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                ULARGE_INTEGER u; u.LowPart = fad.ftLastWriteTime.dwLowDateTime; u.HighPart = fad.ftLastWriteTime.dwHighDateTime;
                cands.push_back({candidate, u});
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        return a.stamp.QuadPart > b.stamp.QuadPart;
    });

    for (const auto& c : cands) {
        if (HMODULE m = LoadLibraryW(c.path.c_str())) return m;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// DLSSWorker Implementation
// ---------------------------------------------------------------------------

bool DLSSWorker::setupD3D12() {
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || desc.VendorId != 0x10DE) continue;
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device))))
            break;
    }
    if (!m_device) return false;

    D3D12_COMMAND_QUEUE_DESC qDesc{};
    qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(m_device->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&m_queue)))) return false;

    if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_cmdAlloc)))) return false;
    if (FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_cmdAlloc.Get(), nullptr, IID_PPV_ARGS(&m_cmdList)))) return false;

    if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)))) return false;
    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent) return false;

    return true;
}

bool DLSSWorker::executeAndWait() {
    if (FAILED(m_cmdList->Close())) return false;
    ID3D12CommandList* lists[] = { m_cmdList.Get() };
    m_queue->ExecuteCommandLists(1, lists);
    waitGPU();
    m_cmdAlloc->Reset();
    m_cmdList->Reset(m_cmdAlloc.Get(), nullptr);
    return true;
}

void DLSSWorker::waitGPU() {
    ++m_fenceVal;
    m_queue->Signal(m_fence.Get(), m_fenceVal);
    if (m_fence->GetCompletedValue() < m_fenceVal) {
        m_fence->SetEventOnCompletion(m_fenceVal, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

bool DLSSWorker::allocateResources(uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH) {
    m_inW = inW; m_inH = inH;
    m_outW = outW; m_outH = outH;
    m_needUpscale = (outW > inW || outH > inH);

    // Color texture: RGBA16F (inW x inH)
    m_colorTex = CreateTexture(m_device.Get(), inW, inH, DXGI_FORMAT_R16G16B16A16_FLOAT,
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    // Intermediate texture for Two-Pass pipeline (Pass 1 NR output / Pass 2 SR input)
    if (m_needUpscale) {
        m_intermediateTex = CreateTexture(m_device.Get(), inW, inH, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (!m_intermediateTex) return false;
    } else {
        m_intermediateTex.Reset();
    }

    m_depthTex = CreateTexture(m_device.Get(), inW, inH, DXGI_FORMAT_R32_FLOAT,
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    m_controlMaskTex = CreateTexture(m_device.Get(), inW, inH, DXGI_FORMAT_R32_FLOAT,
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    // Output texture: RGBA16F (outW x outH)
    m_outputTex = CreateTexture(m_device.Get(), outW, outH, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    // Motion Vector texture: RG16F (inW x inH)
    m_mvTex = CreateTexture(m_device.Get(), inW, inH, DXGI_FORMAT_R16G16_FLOAT,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    if (!m_colorTex || !m_outputTex || !m_mvTex || !m_depthTex || !m_controlMaskTex) return false;

    // Row pitch aligned to 256 bytes for D3D12 copy operations
    m_colorPitch    = AlignUp(inW * 8u, 256u);   // RGBA16F = 8 bytes/pixel
    m_mvPitch       = AlignUp(inW * 4u, 256u);   // RG16F = 4 bytes/pixel
    m_guidePitch    = AlignUp(inW * 4u, 256u);   // R32F = 4 bytes/pixel
    m_readbackPitch = AlignUp(outW * 8u, 256u);  // RGBA16F = 8 bytes/pixel

    m_uploadBytes   = (UINT64)m_colorPitch * inH;
    m_mvBytes       = (UINT64)m_mvPitch * inH;
    m_guideBytes    = (UINT64)m_guidePitch * inH;
    m_readbackBytes = (UINT64)m_readbackPitch * outH;

    m_uploadBuf   = CreateLinearBuffer(m_device.Get(), m_uploadBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    m_mvUploadBuf = CreateLinearBuffer(m_device.Get(), m_mvBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    m_depthUploadBuf = CreateLinearBuffer(m_device.Get(), m_guideBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    m_controlMaskUploadBuf = CreateLinearBuffer(m_device.Get(), m_guideBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    m_readbackBuf = CreateLinearBuffer(m_device.Get(), m_readbackBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

    if (!m_uploadBuf || !m_mvUploadBuf || !m_depthUploadBuf || !m_controlMaskUploadBuf || !m_readbackBuf) return false;

    return true;
}

void DLSSWorker::setCommonParams(bool reset) {
    if (!m_params) return;
    ID3D12Resource* nrOutput = m_needUpscale ? m_intermediateTex.Get() : m_outputTex.Get();

    m_params->Set("DLSSNR.Width", m_inW);
    m_params->Set("DLSSNR.Height", m_inH);
    m_params->Set("DLSSNR.Enabled", 1);
    m_params->Set("DLSSNR.Reset", reset ? 1 : 0);
    m_params->Set("DLSSNR.Style", (int)m_hdr.style);
    m_params->Set("DLSSNR.Hint.Render.Preset", (int)m_hdr.preset);
    m_params->Set("DLSSNR.Intensity", m_hdr.intensity);
    m_params->Set("DLSSNR.LocalToneStrength", m_hdr.local_tone);
    m_params->Set("DLSSNR.LocalStructureStrength", m_hdr.local_structure);
    m_params->Set("DLSSNR.SkinStructureStrength", m_hdr.skin_structure);
    m_params->Set("DLSSNR.UseAutoMask", (int)m_hdr.auto_mask);
    m_params->Set("DLSSNR.UICorrection", 0);
    m_params->Set("DLSSNR.DepthInverted", 1);
    m_params->Set("DLSSNR.ScalingRatio", 1.0f); // NR Pass is ALWAYS 1.0x native
    m_params->Set("DLSSNR.MVecScaleX", 1.0f);
    m_params->Set("DLSSNR.MVecScaleY", 1.0f);
    m_params->Set("DLSSNR.Color", m_colorTex.Get());
    m_params->Set("DLSSNR.Output", nrOutput);
    m_params->Set("DLSSNR.Backbuffer", nrOutput);
    m_params->Set("DLSSNR.MVec", m_mvTex.Get());
    m_params->Set("DLSSNR.ColorSubrectBaseX", 0);
    m_params->Set("DLSSNR.ColorSubrectBaseY", 0);
    m_params->Set("DLSSNR.ColorSubrectWidth", m_inW);
    m_params->Set("DLSSNR.ColorSubrectHeight", m_inH);
    m_params->Set("DLSSNR.OutputSubrectBaseX", 0);
    m_params->Set("DLSSNR.OutputSubrectBaseY", 0);
    m_params->Set("DLSSNR.OutputSubrectWidth", m_inW);
    m_params->Set("DLSSNR.OutputSubrectHeight", m_inH);
    m_params->Set("DLSSNR.MVecSubrectBaseX", 0);
    m_params->Set("DLSSNR.MVecSubrectBaseY", 0);
    m_params->Set("DLSSNR.MVecSubrectWidth", m_inW);
    m_params->Set("DLSSNR.MVecSubrectHeight", m_inH);
}

bool DLSSWorker::initNGX(const VideoHeader& hdr) {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring runtimeDir = exePath;
    size_t lastSlash = runtimeDir.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) runtimeDir = runtimeDir.substr(0, lastSlash);

    m_coreMod = LoadCoreNGX(runtimeDir);
    if (!m_coreMod) {
        m_lastError = "Could not load NVIDIA NGX core _nvngx.dll";
        return false;
    }

    std::wstring nrPath = runtimeDir + L"\\nvngx_dlssnr.dll";
    if (!FileExists(nrPath)) {
        m_lastError = "nvngx_dlssnr.dll not found in runtime directory";
        return false;
    }
    m_nrMod = LoadLibraryW(nrPath.c_str());
    if (!m_nrMod) {
        m_lastError = "LoadLibrary(nvngx_dlssnr.dll) failed";
        return false;
    }

    std::wstring shimPath = runtimeDir + L"\\nvngx.dll";
    if (!FileExists(shimPath)) {
        m_lastError = "Caller shim nvngx.dll not found";
        return false;
    }
    m_shimMod = LoadLibraryW(shimPath.c_str());
    if (!m_shimMod) {
        m_lastError = "LoadLibrary(nvngx.dll shim) failed";
        return false;
    }

    auto coreInitExt     = reinterpret_cast<InitExtFn>(GetProcAddress(m_coreMod, "NVSDK_NGX_D3D12_Init_Ext"));
    auto coreInitProject = reinterpret_cast<InitProjectIdFn>(GetProcAddress(m_coreMod, "NVSDK_NGX_D3D12_Init_ProjectID"));
    auto allocParams     = reinterpret_cast<AllocParamsFn>(GetProcAddress(m_coreMod, "NVSDK_NGX_D3D12_AllocateParameters"));

    auto nrInit          = reinterpret_cast<SnippetInitFn>(GetProcAddress(m_nrMod, "NVSDK_NGX_D3D12_Init_Ext"));
    auto nrCreate        = reinterpret_cast<CreateFeatureFn>(GetProcAddress(m_nrMod, "NVSDK_NGX_D3D12_CreateFeature"));

    auto shimInit        = reinterpret_cast<ShimInitFn>(GetProcAddress(m_shimMod, "DLSSNR_CallInit"));
    auto shimCreate      = reinterpret_cast<ShimCreateFn>(GetProcAddress(m_shimMod, "DLSSNR_CallCreate"));

    if (!allocParams || !nrInit || !nrCreate || !shimInit || !shimCreate) {
        m_lastError = "Failed to resolve required NGX / Snippet / Shim entry points";
        return false;
    }

    NGXFeatureCommonInfo fci{};
    fci.LoggingInfo.LoggingLevel = NGX_LOG_OFF;
    fci.LoggingInfo.DisableOtherLoggingSinks = true;

    bool coreOk = false;
    if (coreInitProject) {
        for (int ver = 0x13; ver <= 0x20 && !coreOk; ++ver) {
            NGXResult r = coreInitProject(PROJECT_ID, 0, "1.0.0", runtimeDir.c_str(), m_device.Get(), ver, nullptr);
            coreOk = (r == 1);
        }
    }
    if (!coreOk && coreInitExt) {
        for (int ver = 0x13; ver <= 0x20 && !coreOk; ++ver) {
            NGXResult r = coreInitExt(APP_ID, runtimeDir.c_str(), m_device.Get(), ver, &fci);
            coreOk = (r == 1);
        }
    }
    if (!coreOk) {
        m_lastError = "NGX Core initialization failed";
        return false;
    }

    NGXResult sr = shimInit(reinterpret_cast<void*>(nrInit), APP_ID, runtimeDir.c_str(), m_device.Get(), 0x15, &fci);
    if (sr != 1) {
        m_lastError = "DLSSNR Snippet initialization via caller shim failed";
        return false;
    }

    if (allocParams(&m_params) != 1 || !m_params) {
        m_lastError = "AllocateParameters failed";
        return false;
    }

    setCommonParams(true);

    NGXResult cr = shimCreate(reinterpret_cast<void*>(nrCreate), m_cmdList.Get(), NR_FEATURE_ID, m_params, &m_feature);
    if (cr != 1 || !m_feature) {
        m_lastError = "CreateFeature(18: DLSS-NR) failed";
        return false;
    }

    // If upscaling requested (>1.0x), create Feature 1 (DLSS-SR) for Pass 2
    if (m_needUpscale) {
        if (allocParams(&m_srParams) != 1 || !m_srParams) {
            m_lastError = "AllocateParameters for DLSS-SR failed";
            return false;
        }

        auto coreCreate = reinterpret_cast<CreateFeatureFn>(GetProcAddress(m_coreMod, "NVSDK_NGX_D3D12_CreateFeature"));
        if (!coreCreate) {
            m_lastError = "NVSDK_NGX_D3D12_CreateFeature not found in core";
            return false;
        }

        m_srParams->Set("Width", m_inW);
        m_srParams->Set("Height", m_inH);
        m_srParams->Set("OutWidth", m_outW);
        m_srParams->Set("OutHeight", m_outH);
        m_srParams->Set("PerfQualityValue", (int)m_hdr.perf_quality);
        m_srParams->Set("DLSS.Feature.Create.Flags", (unsigned int)(1 | 8)); // IsHDR (1) | DepthInverted (8)
        m_srParams->Set("DLSS.Hint.Render.Preset", (int)m_hdr.dlss_model_preset);

        NGXResult srCr = shimCreate(reinterpret_cast<void*>(coreCreate), m_cmdList.Get(), 1, m_srParams, &m_srFeature);
        if (srCr != 1 || !m_srFeature) {
            m_lastError = "CreateFeature(1: DLSS-SR) failed (code=" + std::to_string(srCr) + ")";
            return false;
        }
    }

    if (!executeAndWait()) return false;

    return true;
}

bool DLSSWorker::init(const VideoHeader& hdr) {
    m_legacyRgba8 = hdr.magic == 0x34563544u;
    m_hdr = hdr;
    if (!setupD3D12()) return false;
    if (!allocateResources(hdr.input_width, hdr.input_height, hdr.output_width, hdr.output_height)) return false;
    if (!initNGX(hdr)) return false;

    m_setup.magic                = 0x34505553u; // 'SUP4'
    m_setup.setup_ok             = 1;
    m_setup.setup_result         = 1;
    m_setup.render_width         = hdr.input_width;
    m_setup.render_height        = hdr.input_height;
    m_setup.output_width         = hdr.output_width;
    m_setup.output_height        = hdr.output_height;
    m_setup.min_width            = hdr.input_width;
    m_setup.min_height           = hdr.input_height;
    m_setup.max_width            = hdr.input_width;
    m_setup.max_height           = hdr.input_height;
    m_setup.applied_model_preset = hdr.dlss_model_preset;

    // Initialize DIS optical flow resolution & state
    m_flow_width = (hdr.dis_flow_width > 0) ? (int)hdr.dis_flow_width : 640;
    if (m_flow_width > (int)hdr.input_width) m_flow_width = (int)hdr.input_width;
    float scale = (float)m_flow_width / (float)hdr.input_width;
    m_flow_height = std::max(16, (int)std::round(((float)hdr.input_height * scale) / 2.0f) * 2);
    m_prev_gray.clear();

    m_initialized = true;
    return true;
}

bool DLSSWorker::processFrame(
    uint32_t idx, bool reset, int64_t pts,
    const uint8_t* rgba_in, const uint8_t* motion_in,
    const float* depth_in, const float* control_mask_in,
    std::vector<uint8_t>& rgba_out)
{
    if (!m_initialized || !rgba_in) return false;

    // 1. Upload legacy RGBA8 or linear RGBA16F into the RGBA16F GPU texture.
    {
        void* mapped = nullptr;
        if (FAILED(m_uploadBuf->Map(0, nullptr, &mapped)) || !mapped) return false;
        auto* dstBase = static_cast<uint8_t*>(mapped);
        for (uint32_t y = 0; y < m_inH; ++y) {
            auto* dstRow = reinterpret_cast<uint16_t*>(dstBase + (size_t)y * m_colorPitch);
            if (m_legacyRgba8) {
                const uint8_t* srcRow = rgba_in + (size_t)y * m_inW * 4;
                for (uint32_t x = 0; x < m_inW; ++x) {
                    dstRow[x * 4 + 0] = FloatToHalf(srcRow[x * 4 + 0] / 255.0f);
                    dstRow[x * 4 + 1] = FloatToHalf(srcRow[x * 4 + 1] / 255.0f);
                    dstRow[x * 4 + 2] = FloatToHalf(srcRow[x * 4 + 2] / 255.0f);
                    dstRow[x * 4 + 3] = FloatToHalf(srcRow[x * 4 + 3] / 255.0f);
                }
            } else {
                const uint16_t* srcRow = reinterpret_cast<const uint16_t*>(rgba_in + (size_t)y * m_inW * 8);
                memcpy(dstRow, srcRow, (size_t)m_inW * 8);
            }
        }
        m_uploadBuf->Unmap(0, nullptr);
    }

    // 2. Optical Flow / Motion Vector Processing
    std::vector<float> flowU, flowV;
    if (m_hdr.mv_mode == 2) { // Auto DIS Optical Flow
        std::vector<uint8_t> rgba8ForFlow((size_t)m_inW * m_inH * 4);
        if (m_legacyRgba8) {
            rgba8ForFlow.assign(rgba_in, rgba_in + rgba8ForFlow.size());
        } else {
            for (size_t i = 0; i < rgba8ForFlow.size(); ++i) {
                rgba8ForFlow[i] = static_cast<uint8_t>(std::clamp(
                    static_cast<int>(HalfToFloat(reinterpret_cast<const uint16_t*>(rgba_in)[i]) * 255.0f + 0.5f), 0, 255));
            }
        }
        std::vector<uint8_t> currGray((size_t)m_flow_width * m_flow_height);
        DISOpticalFlow::downscaleRgbaToGray(rgba8ForFlow.data(), m_inW, m_inH, currGray.data(), m_flow_width, m_flow_height);

        if (m_prev_gray.empty()) {
            reset = true;
        } else if (!reset) {
            DISParams params;
            params.flow_width = m_flow_width;
            params.iterations = m_hdr.dis_iterations > 0 ? (int)m_hdr.dis_iterations : 25;

            switch (m_hdr.dis_preset) {
                case 0: // Fast Preview
                    params.finest_scale = 2;
                    params.patch_stride = 4;
                    break;
                case 1: // Balanced
                    params.finest_scale = 1;
                    params.patch_stride = 4;
                    break;
                case 2: // High Quality
                    params.finest_scale = 0;
                    params.patch_stride = 2;
                    break;
                case 3: // Extreme
                    params.finest_scale = 0;
                    params.patch_stride = 1;
                    break;
                case 4: // Custom
                default:
                    params.finest_scale = 1;
                    params.patch_stride = 4;
                    break;
            }

            m_dis.compute(currGray.data(), m_prev_gray.data(),
                          m_flow_width, m_flow_height,
                          m_inW, m_inH,
                          params, flowU, flowV);
        }
        m_prev_gray = std::move(currGray);
    }

    // 3. Upload Motion Vectors (float16 U, V)
    {
        void* mapped = nullptr;
        if (FAILED(m_mvUploadBuf->Map(0, nullptr, &mapped)) || !mapped) return false;
        auto* dstBase = static_cast<uint8_t*>(mapped);

        if (m_hdr.mv_mode == 2 && !flowU.empty() && !reset) {
            for (uint32_t y = 0; y < m_inH; ++y) {
                auto* dstRow = reinterpret_cast<uint16_t*>(dstBase + (size_t)y * m_mvPitch);
                size_t rowOffset = (size_t)y * m_inW;
                for (uint32_t x = 0; x < m_inW; ++x) {
                    dstRow[x * 2 + 0] = FloatToHalf(flowU[rowOffset + x]);
                    dstRow[x * 2 + 1] = FloatToHalf(flowV[rowOffset + x]);
                }
            }
        } else if (m_hdr.mv_mode == 1 && motion_in && !reset) {
            for (uint32_t y = 0; y < m_inH; ++y) {
                auto* dstRow = dstBase + (size_t)y * m_mvPitch;
                const uint8_t* srcRow = motion_in + (size_t)y * m_inW * 4; // 2 float16 = 4 bytes
                memcpy(dstRow, srcRow, m_inW * 4);
            }
        } else {
            for (uint32_t y = 0; y < m_inH; ++y) {
                auto* dstRow = dstBase + (size_t)y * m_mvPitch;
                memset(dstRow, 0, m_inW * 4);
            }
        }
        m_mvUploadBuf->Unmap(0, nullptr);
    }

    // 3.5. Upload optional CG guides if present
    if (depth_in) {
        void* mapped = nullptr;
        if (FAILED(m_depthUploadBuf->Map(0, nullptr, &mapped)) || !mapped) return false;
        auto* dstBase = static_cast<uint8_t*>(mapped);
        for (uint32_t y = 0; y < m_inH; ++y) {
            float* dstRow = reinterpret_cast<float*>(dstBase + (size_t)y * m_guidePitch);
            memcpy(dstRow, depth_in + (size_t)y * m_inW, (size_t)m_inW * sizeof(float));
        }
        m_depthUploadBuf->Unmap(0, nullptr);
    }
    if (control_mask_in) {
        void* mapped = nullptr;
        if (FAILED(m_controlMaskUploadBuf->Map(0, nullptr, &mapped)) || !mapped) return false;
        auto* dstBase = static_cast<uint8_t*>(mapped);
        for (uint32_t y = 0; y < m_inH; ++y) {
            float* dstRow = reinterpret_cast<float*>(dstBase + (size_t)y * m_guidePitch);
            memcpy(dstRow, control_mask_in + (size_t)y * m_inW, (size_t)m_inW * sizeof(float));
        }
        m_controlMaskUploadBuf->Unmap(0, nullptr);
    }

    // 3. Transition textures to COPY_DEST
    std::vector<D3D12_RESOURCE_BARRIER> preBarriers;
    preBarriers.push_back(Barrier(m_colorTex.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST));
    preBarriers.push_back(Barrier(m_mvTex.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST));
    if (depth_in) {
        preBarriers.push_back(Barrier(m_depthTex.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST));
    }
    if (control_mask_in) {
        preBarriers.push_back(Barrier(m_controlMaskTex.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST));
    }
    m_cmdList->ResourceBarrier((UINT)preBarriers.size(), preBarriers.data());

    // Copy upload buffers to textures
    D3D12_TEXTURE_COPY_LOCATION dstCol{}, srcCol{};
    dstCol.pResource = m_colorTex.Get(); dstCol.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcCol.pResource = m_uploadBuf.Get(); srcCol.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcCol.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srcCol.PlacedFootprint.Footprint.Width = m_inW; srcCol.PlacedFootprint.Footprint.Height = m_inH;
    srcCol.PlacedFootprint.Footprint.Depth = 1; srcCol.PlacedFootprint.Footprint.RowPitch = m_colorPitch;
    m_cmdList->CopyTextureRegion(&dstCol, 0, 0, 0, &srcCol, nullptr);

    D3D12_TEXTURE_COPY_LOCATION dstMv{}, srcMv{};
    dstMv.pResource = m_mvTex.Get(); dstMv.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcMv.pResource = m_mvUploadBuf.Get(); srcMv.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcMv.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16_FLOAT;
    srcMv.PlacedFootprint.Footprint.Width = m_inW; srcMv.PlacedFootprint.Footprint.Height = m_inH;
    srcMv.PlacedFootprint.Footprint.Depth = 1; srcMv.PlacedFootprint.Footprint.RowPitch = m_mvPitch;
    m_cmdList->CopyTextureRegion(&dstMv, 0, 0, 0, &srcMv, nullptr);

    if (depth_in) {
        D3D12_TEXTURE_COPY_LOCATION dstDepth{}, srcDepth{};
        dstDepth.pResource = m_depthTex.Get(); dstDepth.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcDepth.pResource = m_depthUploadBuf.Get(); srcDepth.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcDepth.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
        srcDepth.PlacedFootprint.Footprint.Width = m_inW; srcDepth.PlacedFootprint.Footprint.Height = m_inH;
        srcDepth.PlacedFootprint.Footprint.Depth = 1; srcDepth.PlacedFootprint.Footprint.RowPitch = m_guidePitch;
        m_cmdList->CopyTextureRegion(&dstDepth, 0, 0, 0, &srcDepth, nullptr);
    }

    if (control_mask_in) {
        D3D12_TEXTURE_COPY_LOCATION dstMask{}, srcMask{};
        dstMask.pResource = m_controlMaskTex.Get(); dstMask.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcMask.pResource = m_controlMaskUploadBuf.Get(); srcMask.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcMask.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
        srcMask.PlacedFootprint.Footprint.Width = m_inW; srcMask.PlacedFootprint.Footprint.Height = m_inH;
        srcMask.PlacedFootprint.Footprint.Depth = 1; srcMask.PlacedFootprint.Footprint.RowPitch = m_guidePitch;
        m_cmdList->CopyTextureRegion(&dstMask, 0, 0, 0, &srcMask, nullptr);
    }

    // Transition back to SHADER_RESOURCE
    std::vector<D3D12_RESOURCE_BARRIER> postBarriers;
    postBarriers.push_back(Barrier(m_colorTex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    postBarriers.push_back(Barrier(m_mvTex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    if (depth_in) {
        postBarriers.push_back(Barrier(m_depthTex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    }
    if (control_mask_in) {
        postBarriers.push_back(Barrier(m_controlMaskTex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    }
    m_cmdList->ResourceBarrier((UINT)postBarriers.size(), postBarriers.data());

    // 4. Evaluate Feature 18 (DLSS-NR Pass 1)
    m_params->Set("DLSSNR.Reset", reset ? 1 : 0);
    if (depth_in) {
        m_params->Set("DLSSNR.Depth", m_depthTex.Get());
        m_params->Set("DLSSNR.DepthSubrectBaseX", 0);
        m_params->Set("DLSSNR.DepthSubrectBaseY", 0);
        m_params->Set("DLSSNR.DepthSubrectWidth", m_inW);
        m_params->Set("DLSSNR.DepthSubrectHeight", m_inH);
    } else {
        m_params->Set("DLSSNR.Depth", (ID3D12Resource*)nullptr);
    }
    if (control_mask_in) {
        m_params->Set("DLSSNR.ControlMask", m_controlMaskTex.Get());
        m_params->Set("DLSSNR.ControlMaskSubrectBaseX", 0);
        m_params->Set("DLSSNR.ControlMaskSubrectBaseY", 0);
        m_params->Set("DLSSNR.ControlMaskSubrectWidth", m_inW);
        m_params->Set("DLSSNR.ControlMaskSubrectHeight", m_inH);
    } else {
        m_params->Set("DLSSNR.ControlMask", (ID3D12Resource*)nullptr);
    }

    auto nrEval = reinterpret_cast<EvaluateFeatureFn>(GetProcAddress(m_nrMod, "NVSDK_NGX_D3D12_EvaluateFeature"));
    auto shimEval = reinterpret_cast<ShimEvaluateFn>(GetProcAddress(m_shimMod, "DLSSNR_CallEvaluate"));
    if (!nrEval || !shimEval) return false;

    NGXResult er = shimEval(reinterpret_cast<void*>(nrEval), m_cmdList.Get(), m_feature, m_params, nullptr);
    if (er != 1) {
        fprintf(stderr, "[DEBUG] Pass 1 (Feature 18 NR) Evaluate failed: 0x%08X\n", er);
        fflush(stderr);
        return false;
    }

    // 4.5. If upscaling, Evaluate Feature 1 (DLSS-SR Pass 2)
    if (m_needUpscale && m_srFeature && m_srParams) {
        auto bInter1 = Barrier(m_intermediateTex.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_cmdList->ResourceBarrier(1, &bInter1);

        m_srParams->Set("Color", m_intermediateTex.Get());
        m_srParams->Set("Output", m_outputTex.Get());
        m_srParams->Set("MotionVectors", m_mvTex.Get());
        m_srParams->Set("Depth", m_depthTex.Get());
        m_srParams->Set("Reset", reset ? 1 : 0);
        m_srParams->Set("Jitter.Offset.X", 0.0f);
        m_srParams->Set("Jitter.Offset.Y", 0.0f);
        m_srParams->Set("MV.Scale.X", 1.0f);
        m_srParams->Set("MV.Scale.Y", 1.0f);
        m_srParams->Set("DLSS.Render.Subrect.Dimensions.Width", (unsigned int)m_inW);
        m_srParams->Set("DLSS.Render.Subrect.Dimensions.Height", (unsigned int)m_inH);
        m_srParams->Set("DLSS.Pre.Exposure", 1.0f);
        m_srParams->Set("DLSS.Exposure.Scale", 1.0f);

        auto coreEval = reinterpret_cast<EvaluateFeatureFn>(GetProcAddress(m_coreMod, "NVSDK_NGX_D3D12_EvaluateFeature"));
        if (!coreEval) {
            fprintf(stderr, "[DEBUG] coreEval function pointer not found!\n");
            fflush(stderr);
            return false;
        }

        NGXResult srEr = shimEval(reinterpret_cast<void*>(coreEval), m_cmdList.Get(), m_srFeature, m_srParams, nullptr);
        if (srEr != 1) {
            fprintf(stderr, "[DEBUG] Pass 2 (Feature 1 SR) Evaluate failed: 0x%08X\n", srEr);
            fflush(stderr);
            return false;
        }

        auto bInter2 = Barrier(m_intermediateTex.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_cmdList->ResourceBarrier(1, &bInter2);
    }

    // 5. Readback result
    auto b5 = Barrier(m_outputTex.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    m_cmdList->ResourceBarrier(1, &b5);

    D3D12_TEXTURE_COPY_LOCATION dstRb{}, srcOut{};
    dstRb.pResource = m_readbackBuf.Get(); dstRb.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstRb.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    dstRb.PlacedFootprint.Footprint.Width = m_outW; dstRb.PlacedFootprint.Footprint.Height = m_outH;
    dstRb.PlacedFootprint.Footprint.Depth = 1; dstRb.PlacedFootprint.Footprint.RowPitch = m_readbackPitch;
    srcOut.pResource = m_outputTex.Get(); srcOut.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    m_cmdList->CopyTextureRegion(&dstRb, 0, 0, 0, &srcOut, nullptr);

    auto b6 = Barrier(m_outputTex.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_cmdList->ResourceBarrier(1, &b6);

    if (!executeAndWait()) return false;

    // 6. Map readback buffer & convert
    void* rmap = nullptr;
    if (FAILED(m_readbackBuf->Map(0, nullptr, &rmap)) || !rmap) return false;
    const auto* base = static_cast<const uint8_t*>(rmap);

    rgba_out.resize((size_t)m_outW * m_outH * 4 * (m_legacyRgba8 ? 1 : 2));

    // Channel order robustness: compare first few samples against source mean color to verify R/B order
    double sumR = 0, sumB = 0;
    double srcSumR = 0, srcSumB = 0;
    for (uint32_t y = 0; y < std::min(m_inH, 32u); ++y) {
        const uint16_t* sRow16 = m_legacyRgba8 ? nullptr : reinterpret_cast<const uint16_t*>(rgba_in + (size_t)y * m_inW * 8);
        const uint8_t* sRow8 = m_legacyRgba8 ? rgba_in + (size_t)y * m_inW * 4 : nullptr;
        const auto* rRow = reinterpret_cast<const uint16_t*>(base + (size_t)y * m_readbackPitch);
        for (uint32_t x = 0; x < std::min(m_inW, 32u); ++x) {
            srcSumR += m_legacyRgba8 ? sRow8[x * 4 + 0] / 255.0 : HalfToFloat(sRow16[x * 4 + 0]);
            srcSumB += m_legacyRgba8 ? sRow8[x * 4 + 2] / 255.0 : HalfToFloat(sRow16[x * 4 + 2]);
            sumR += HalfToFloat(rRow[x * 4 + 0]);
            sumB += HalfToFloat(rRow[x * 4 + 2]);
        }
    }
    bool swapRB = (std::abs(sumR - srcSumR) + std::abs(sumB - srcSumB)) >
                  (std::abs(sumB - srcSumR) + std::abs(sumR - srcSumB));

    for (uint32_t y = 0; y < m_outH; ++y) {
        const auto* row = reinterpret_cast<const uint16_t*>(base + (size_t)y * m_readbackPitch);
        uint8_t* dst = rgba_out.data() + (size_t)y * m_outW * 4 * (m_legacyRgba8 ? 1 : 2);
        for (uint32_t x = 0; x < m_outW; ++x) {
            if (m_legacyRgba8) {
                dst[x * 4 + 0] = static_cast<uint8_t>(std::clamp(static_cast<int>(HalfToFloat(row[x * 4 + (swapRB ? 2 : 0)]) * 255.0f + 0.5f), 0, 255));
                dst[x * 4 + 1] = static_cast<uint8_t>(std::clamp(static_cast<int>(HalfToFloat(row[x * 4 + 1]) * 255.0f + 0.5f), 0, 255));
                dst[x * 4 + 2] = static_cast<uint8_t>(std::clamp(static_cast<int>(HalfToFloat(row[x * 4 + (swapRB ? 0 : 2)]) * 255.0f + 0.5f), 0, 255));
                dst[x * 4 + 3] = static_cast<uint8_t>(std::clamp(static_cast<int>(HalfToFloat(row[x * 4 + 3]) * 255.0f + 0.5f), 0, 255));
            } else {
                uint16_t* dst16 = reinterpret_cast<uint16_t*>(dst);
                dst16[x * 4 + 0] = row[x * 4 + (swapRB ? 2 : 0)];
                dst16[x * 4 + 1] = row[x * 4 + 1];
                dst16[x * 4 + 2] = row[x * 4 + (swapRB ? 0 : 2)];
                dst16[x * 4 + 3] = row[x * 4 + 3];
            }
        }
    }
    m_readbackBuf->Unmap(0, nullptr);

    return true;
}

void DLSSWorker::shutdown() {
    if (m_srFeature) {
        auto coreRelease = reinterpret_cast<ReleaseFeatureFn>(GetProcAddress(m_coreMod, "NVSDK_NGX_D3D12_ReleaseFeature"));
        auto shimRelease = reinterpret_cast<ShimReleaseFn>(GetProcAddress(m_shimMod, "DLSSNR_CallRelease"));
        if (coreRelease && shimRelease) shimRelease(reinterpret_cast<void*>(coreRelease), m_srFeature);
        m_srFeature = nullptr;
    }
    if (m_srParams) {
        auto destroyParams = reinterpret_cast<DestroyParamsFn>(GetProcAddress(m_coreMod, "NVSDK_NGX_D3D12_DestroyParameters"));
        if (destroyParams) destroyParams(m_srParams);
        m_srParams = nullptr;
    }
    if (m_feature) {
        auto nrRelease = reinterpret_cast<ReleaseFeatureFn>(GetProcAddress(m_nrMod, "NVSDK_NGX_D3D12_ReleaseFeature"));
        auto shimRelease = reinterpret_cast<ShimReleaseFn>(GetProcAddress(m_shimMod, "DLSSNR_CallRelease"));
        if (nrRelease && shimRelease) shimRelease(reinterpret_cast<void*>(nrRelease), m_feature);
        m_feature = nullptr;
    }
    if (m_coreMod) {
        auto coreShutdown = reinterpret_cast<ShutdownFn>(GetProcAddress(m_coreMod, "NVSDK_NGX_D3D12_Shutdown"));
        if (coreShutdown) coreShutdown();
    }
    m_params = nullptr;
    m_colorTex.Reset();
    m_intermediateTex.Reset();
    m_depthTex.Reset();
    m_controlMaskTex.Reset();
    m_outputTex.Reset();
    m_mvTex.Reset();
    m_uploadBuf.Reset();
    m_mvUploadBuf.Reset();
    m_depthUploadBuf.Reset();
    m_controlMaskUploadBuf.Reset();
    m_readbackBuf.Reset();
    m_cmdList.Reset();
    m_cmdAlloc.Reset();
    m_queue.Reset();
    m_fence.Reset();
    if (m_fenceEvent) { CloseHandle(m_fenceEvent); m_fenceEvent = nullptr; }
    m_device.Reset();

    if (m_shimMod) { FreeLibrary(m_shimMod); m_shimMod = nullptr; }
    if (m_nrMod) { FreeLibrary(m_nrMod); m_nrMod = nullptr; }
    if (m_coreMod) { FreeLibrary(m_coreMod); m_coreMod = nullptr; }

    m_initialized = false;
}
