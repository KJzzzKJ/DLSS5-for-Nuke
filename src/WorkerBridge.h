#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

// Protocol Structs (Packed)
#pragma pack(push, 1)

struct VideoHeader {
    // 'D5V4' is legacy RGBA8; 'D5V5' is linear RGBA16F. Must match worker/Protocol.h.
    uint32_t magic = 0x35563544; // 'D5V5' (VIDEO_MAGIC)
    uint32_t input_width;
    uint32_t input_height;
    uint32_t output_width;
    uint32_t output_height;
    uint32_t warmup_frames;
    uint32_t frame_count = 100000;
    uint32_t perf_quality;
    uint32_t dlss_model_preset;
    uint32_t profile = 0;
    uint32_t preset = 0;
    uint32_t style = 0;
    uint32_t auto_mask = 0;
    uint32_t ui_correction = 0;
    float intensity;
    float local_tone;
    float local_structure;
    float skin_structure;
    uint32_t mv_mode = 0;             // 0=None, 1=External Buffer, 2=Auto DIS
    uint32_t dis_preset = 1;          // 0=Fast, 1=Balanced, 2=High, 3=Extreme, 4=Custom
    uint32_t dis_flow_width = 640;    // e.g. 480, 640, 960, 1280
    uint32_t dis_iterations = 25;     // e.g. 12, 25, 32, 48
    uint32_t _reserved_scene_cut = 0; // Unused (scene cut removed)
    float    _reserved_thresh    = 0.0f;
};

struct SetupResponse {
    uint32_t magic;
    uint32_t setup_ok;
    uint32_t setup_result;
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
    uint32_t magic = 0x314D5246; // 'FRM1'
    uint32_t index;
    uint32_t reset;
    uint32_t guide_flags = 0;
    int64_t pts;
};

enum FrameGuideFlags : uint32_t {
    GUIDE_DEPTH        = 1u << 0,
    GUIDE_CONTROL_MASK = 1u << 1
};

struct FrameResponse {
    uint32_t magic; // 0x3154554F 'OUT1'
    uint32_t out_index;
    uint32_t ok;
    uint32_t byte_count;
    uint32_t ngx_result;
    int64_t out_pts;
};

#pragma pack(pop)

class WorkerBridge {
public:
    WorkerBridge();
    ~WorkerBridge();

    bool start(const std::string& nvngx_path, const VideoHeader& header, SetupResponse& out_setup);
    void stop();

    // Process a single frame. Thread-safe.
    bool processFrame(
        uint32_t index, 
        bool reset, 
        int64_t pts,
        const uint8_t* rgba_data, size_t rgba_size,
        const uint8_t* motion_data, size_t motion_size,
        const uint8_t* depth_data, size_t depth_size,
        const uint8_t* control_mask_data, size_t control_mask_size,
        std::vector<uint8_t>& out_rgba
    );

    bool isRunning();

private:
    HANDLE hProcess = NULL;
    HANDLE hChildStd_IN_Rd = NULL;
    HANDLE hChildStd_IN_Wr = NULL;
    HANDLE hChildStd_OUT_Rd = NULL;
    HANDLE hChildStd_OUT_Wr = NULL;

    SRWLOCK m_lock = SRWLOCK_INIT;
    bool is_running = false;
    VideoHeader m_header = {};

    bool readExact(void* buffer, size_t size);
    bool writeExact(const void* buffer, size_t size);
};
