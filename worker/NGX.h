#pragma once
// Minimal NGX/DLSS type definitions for dynamic loading.
// Matches DLSS SDK 3.x public API - no SDK headers required.

#include <windows.h>
#include <d3d12.h>
#include <cstdint>

struct ID3D11Resource;

// ---- Result codes --------------------------------------------------------
typedef uint32_t NVSDK_NGX_Result;
#define NVSDK_NGX_Result_Success          ((NVSDK_NGX_Result)0x1)
#define NVSDK_NGX_Result_FAIL             ((NVSDK_NGX_Result)0x0)
#define NVSDK_NGX_SUCCEED(r)              ((r) == NVSDK_NGX_Result_Success)

// ---- Feature IDs ---------------------------------------------------------
#define NVSDK_NGX_Feature_SuperSampling   1   // DLSS SR

// ---- Feature creation flags (bitmask) ------------------------------------
#define NVSDK_NGX_DLSS_Feature_Flags_None          0
#define NVSDK_NGX_DLSS_Feature_Flags_IsHDR         (1 << 0)
#define NVSDK_NGX_DLSS_Feature_Flags_MVLowRes      (1 << 1)
#define NVSDK_NGX_DLSS_Feature_Flags_MVJittered    (1 << 2)
#define NVSDK_NGX_DLSS_Feature_Flags_DepthInverted (1 << 3)
#define NVSDK_NGX_DLSS_Feature_Flags_DoSharpening  (1 << 5)
#define NVSDK_NGX_DLSS_Feature_Flags_AutoExposure  (1 << 6)

// ---- Performance / quality levels ----------------------------------------
// Matches NVSDK_NGX_PerfQuality_Value enum
#define NVSDK_NGX_PerfQuality_MaxPerf           0   // Performance
#define NVSDK_NGX_PerfQuality_Balanced          1
#define NVSDK_NGX_PerfQuality_MaxQuality        2   // Quality
#define NVSDK_NGX_PerfQuality_UltraPerformance  3
#define NVSDK_NGX_PerfQuality_UltraQuality      4
#define NVSDK_NGX_PerfQuality_DLAA              5

// ---- Parameter name string constants -------------------------------------
#define NGX_P_COLOR          "Color"
#define NGX_P_OUTPUT         "Output"
#define NGX_P_MV             "MotionVectors"
#define NGX_P_DEPTH          "Depth"
#define NGX_P_RESET          "Reset"
#define NGX_P_WIDTH          "Width"
#define NGX_P_HEIGHT         "Height"
#define NGX_P_OUTWIDTH       "OutWidth"
#define NGX_P_OUTHEIGHT      "OutHeight"
#define NGX_P_PERFQUALITY    "PerfQualityValue"
#define NGX_P_SHARPNESS      "Sharpness"
#define NGX_P_MV_SCALE_X     "MV.Scale.X"
#define NGX_P_MV_SCALE_Y     "MV.Scale.Y"
#define NGX_P_JITTER_X       "Jitter.Offset.X"
#define NGX_P_JITTER_Y       "Jitter.Offset.Y"
#define NGX_P_DLSS_FLAGS     "DLSS.Feature.Create.Flags"
#define NGX_P_SUBRECT_W      "DLSS.Render.Subrect.Dimensions.Width"
#define NGX_P_SUBRECT_H      "DLSS.Render.Subrect.Dimensions.Height"
#define NGX_P_PRE_EXPOSURE   "DLSS.Pre.Exposure"
#define NGX_P_EXPOSURE_SCALE "DLSS.Exposure.Scale"
#define NGX_P_DLSS_GET_OPTW  "DLSS.Render.Subrect.Dimensions.Width"

// ---- Handle --------------------------------------------------------------
struct NVSDK_NGX_Handle { uint32_t Id; };

// ---- Parameter interface (vtable must match nvngx.dll exactly) -----------
// Derived from official DLSS SDK 3.x nvsdk_ngx.h (Apache 2.0 license)
struct NVSDK_NGX_Parameter {
    virtual void Set(const char* n, unsigned long long v) = 0;
    virtual void Set(const char* n, float v)              = 0;
    virtual void Set(const char* n, double v)             = 0;
    virtual void Set(const char* n, unsigned int v)       = 0;
    virtual void Set(const char* n, int v)                = 0;
    virtual void Set(const char* n, ID3D11Resource* v)    = 0;
    virtual void Set(const char* n, ID3D12Resource* v)    = 0;
    virtual void Set(const char* n, void* v)              = 0;
    virtual NVSDK_NGX_Result Get(const char* n, unsigned long long* v) const = 0;
    virtual NVSDK_NGX_Result Get(const char* n, float* v)              const = 0;
    virtual NVSDK_NGX_Result Get(const char* n, double* v)             const = 0;
    virtual NVSDK_NGX_Result Get(const char* n, unsigned int* v)       const = 0;
    virtual NVSDK_NGX_Result Get(const char* n, int* v)                const = 0;
    virtual NVSDK_NGX_Result Get(const char* n, ID3D11Resource** v)    const = 0;
    virtual NVSDK_NGX_Result Get(const char* n, ID3D12Resource** v)    const = 0;
    virtual NVSDK_NGX_Result Get(const char* n, void** v)              const = 0;
    virtual void Reset() = 0;
};

// ---- Function pointer types for dynamic loading --------------------------
typedef NVSDK_NGX_Result(__cdecl* PFN_NGX_D3D12_Init)(
    unsigned long long appId,
    const wchar_t*     dataPath,
    ID3D12Device*      device,
    const void*        featureInfo, // NVSDK_NGX_FeatureCommonInfo* (can be nullptr)
    unsigned int       sdkVersion   // pass 0 for auto-detect
);

typedef NVSDK_NGX_Result(__cdecl* PFN_NGX_D3D12_Shutdown1)(
    ID3D12Device* device
);

typedef NVSDK_NGX_Result(__cdecl* PFN_NGX_D3D12_AllocateParameters)(
    NVSDK_NGX_Parameter** outParams
);

typedef NVSDK_NGX_Result(__cdecl* PFN_NGX_D3D12_DestroyParameters)(
    NVSDK_NGX_Parameter* params
);

typedef NVSDK_NGX_Result(__cdecl* PFN_NGX_D3D12_GetCapabilityParameters)(
    NVSDK_NGX_Parameter** outParams
);

typedef NVSDK_NGX_Result(__cdecl* PFN_NGX_D3D12_CreateFeature)(
    ID3D12GraphicsCommandList* cmdList,
    unsigned int               featureId,
    NVSDK_NGX_Parameter*       params,
    NVSDK_NGX_Handle**         outHandle
);

typedef NVSDK_NGX_Result(__cdecl* PFN_NGX_D3D12_ReleaseFeature)(
    NVSDK_NGX_Handle* handle
);

typedef NVSDK_NGX_Result(__cdecl* PFN_NGX_D3D12_EvaluateFeature)(
    ID3D12GraphicsCommandList* cmdList,
    const NVSDK_NGX_Handle*    handle,
    const NVSDK_NGX_Parameter* params,
    void*                      callback // PFN_NVSDK_NGX_ProgressCallback, can be nullptr
);

// ---- Dynamic loader -------------------------------------------------------
struct NGXLoader {
    HMODULE hDll = nullptr;

    PFN_NGX_D3D12_Init                  Init                  = nullptr;
    PFN_NGX_D3D12_Shutdown1             Shutdown1             = nullptr;
    PFN_NGX_D3D12_AllocateParameters    AllocateParameters    = nullptr;
    PFN_NGX_D3D12_DestroyParameters     DestroyParameters     = nullptr;
    PFN_NGX_D3D12_GetCapabilityParameters GetCapabilityParameters = nullptr;
    PFN_NGX_D3D12_CreateFeature         CreateFeature         = nullptr;
    PFN_NGX_D3D12_ReleaseFeature        ReleaseFeature        = nullptr;
    PFN_NGX_D3D12_EvaluateFeature       EvaluateFeature       = nullptr;

    bool load(const wchar_t* nvngxPath = L"nvngx.dll") {
        hDll = LoadLibraryW(nvngxPath);
        if (!hDll) return false;

        auto get = [&](const char* name) { return GetProcAddress(hDll, name); };

#define LOAD(fn, sym) fn = reinterpret_cast<decltype(fn)>(get(sym)); if (!fn) { unload(); return false; }
        LOAD(Init,                   "NVSDK_NGX_D3D12_Init")
        LOAD(Shutdown1,              "NVSDK_NGX_D3D12_Shutdown1")
        LOAD(AllocateParameters,     "NVSDK_NGX_D3D12_AllocateParameters")
        LOAD(DestroyParameters,      "NVSDK_NGX_D3D12_DestroyParameters")
        LOAD(GetCapabilityParameters,"NVSDK_NGX_D3D12_GetCapabilityParameters")
        LOAD(CreateFeature,          "NVSDK_NGX_D3D12_CreateFeature")
        LOAD(ReleaseFeature,         "NVSDK_NGX_D3D12_ReleaseFeature")
        LOAD(EvaluateFeature,        "NVSDK_NGX_D3D12_EvaluateFeature")
#undef LOAD
        return true;
    }

    void unload() {
        if (hDll) { FreeLibrary(hDll); hDll = nullptr; }
        Init = nullptr; Shutdown1 = nullptr; AllocateParameters = nullptr;
        DestroyParameters = nullptr; GetCapabilityParameters = nullptr;
        CreateFeature = nullptr; ReleaseFeature = nullptr; EvaluateFeature = nullptr;
    }

    ~NGXLoader() { unload(); }
};
