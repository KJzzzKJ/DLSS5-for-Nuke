#pragma once
#include <cstdint>

#pragma pack(push, 1)

struct VideoHeader {
    // 'D5V4': legacy RGBA8; 'D5V5': linear RGBA16F. This must match src/WorkerBridge.h.
    uint32_t magic        = 0x35563544; // 'D5V5'
    uint32_t input_width;
    uint32_t input_height;
    uint32_t output_width;
    uint32_t output_height;
    uint32_t warmup_frames;
    uint32_t frame_count  = 100000;
    uint32_t perf_quality;       // 0=DLAA, 1=Quality, 2=Balanced, 3=Performance, 4=UltraPerf
    uint32_t dlss_model_preset;  // 0=Default, 1=J, 2=K, 3=L, 4=M
    uint32_t profile      = 0;
    uint32_t preset       = 0;
    uint32_t style        = 0;
    uint32_t auto_mask    = 0;
    uint32_t ui_correction = 0;
    float    intensity;
    float    local_tone;
    float    local_structure;
    float    skin_structure;
    uint32_t mv_mode;             // 0=None, 1=External Buffer, 2=Auto DIS
    uint32_t dis_preset;          // 0=Fast, 1=Balanced, 2=High, 3=Extreme, 4=Custom
    uint32_t dis_flow_width;      // e.g. 480, 640, 960, 1280
    uint32_t dis_iterations;      // e.g. 12, 25, 32, 48
    uint32_t _reserved_scene_cut = 0; // Unused (scene cut removed)
    float    _reserved_thresh    = 0.0f;
};

struct SetupResponse {
    uint32_t magic;          // 0x34505553 'SUP4'
    uint32_t setup_ok;       // 1 = success
    uint32_t setup_result;   // NGX result code
    uint32_t render_width;
    uint32_t render_height;
    uint32_t output_width;
    uint32_t output_height;
    uint32_t min_width;
    uint32_t min_height;
    uint32_t max_width;
    uint32_t max_height;
    uint32_t applied_model_preset;
};

struct FrameHeader {
    uint32_t magic  = 0x314D5246; // 'FRM1'
    uint32_t index;
    uint32_t reset;
    uint32_t guide_flags = 0;
    int64_t  pts;
};

enum FrameGuideFlags : uint32_t {
    GUIDE_DEPTH        = 1u << 0,
    GUIDE_CONTROL_MASK = 1u << 1
};

struct FrameResponse {
    uint32_t magic;      // 0x3154554F 'OUT1'
    uint32_t out_index;
    uint32_t ok;         // 1 = success
    uint32_t byte_count;
    uint32_t ngx_result;
    int64_t  out_pts;
};

#pragma pack(pop)
