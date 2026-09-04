#pragma once

#include <windows.h>
#include "DDImage/Iop.h"
#include "DDImage/Row.h"
#include "DDImage/Knobs.h"
#include "DDImage/Format.h"
#include "DDImage/ChannelSet.h"
#include "WorkerBridge.h"
#include <string>
#include <vector>
#include <cstdint>

using namespace DD::Image;

class DLSS5Live : public Iop {
public:
    DLSS5Live(Node* node);
    virtual ~DLSS5Live();

    // Nuke Iop Overrides (img, motion, depth, mask; inputs >= 1 are optional)
    int minimum_inputs() const override;
    int maximum_inputs() const override;
    int optional_input() const override;
    const char* input_label(int n, char*) const override;

    void knobs(Knob_Callback) override;
    int knob_changed(Knob* k) override;
    void _validate(bool) override;
    void _request(int x, int y, int r, int t, ChannelMask channels, int count) override;
    void engine(int y, int x, int r, ChannelMask channels, Row& out_row) override;

    // Node information
    const char* Class() const override { return "DLSS5Live"; }
    const char* node_help() const override { return "NVIDIA DLSS 5 Native Live Node"; }

    static const Iop::Description description;

    enum WorkflowMode {
        MODE_SINGLE_FRAME = 0,
        MODE_SEQUENCE     = 1,
        MODE_CG           = 2
    };

    enum MVSource {
        MV_SRC_DIS = 0,
        MV_SRC_INPUT1 = 1,
        MV_SRC_NONE = 2
    };

    enum DISPreset {
        DIS_PRESET_FAST = 0,
        DIS_PRESET_BALANCED = 1,
        DIS_PRESET_HIGH = 2,
        DIS_PRESET_EXTREME = 3,
        DIS_PRESET_CUSTOM = 4
    };

private:
    float getScaleFactor() const;
    uint32_t getPerfQuality() const;
    uint32_t getModelPreset() const;
    size_t computeSettingsHash() const;
    bool computeFrameCache();
    void fallbackScale(int y, int x, int r, ChannelMask channels, Row& out_row);
    void rebuildWorker();
    void update_ui_visibility();

    // Workflow & Guide Buffer Knobs
    int k_pipeline_mode;
    int k_mv_source;
    int k_dis_preset;
    int k_dis_custom_width;
    int k_dis_iterations;

    ChannelSet k_motion_channels;
    float k_mv_scale_x;
    float k_mv_scale_y;
    bool k_invert_mv_x;
    bool k_invert_mv_y;

    // DLSS Core Knobs
    int k_upscaling_mode;
    int k_image_transport;
    int k_model_preset;
    int k_nr_style;
    int k_nr_preset;
    bool k_auto_mask;
    float k_intensity;
    float k_local_tone;
    float k_local_structure;
    float k_skin_structure;
    bool k_enable_hdr_range;
    float k_hdr_range_scale;
    const char* k_nvngx_path;

    WorkerBridge m_worker;
    SetupResponse m_setup_info;

    // Cached dimensions from last _validate
    int m_in_w = 0, m_in_h = 0;
    int m_out_w = 0, m_out_h = 0;

    // State hashing (force rebuild when settings change)
    size_t m_last_hash;

    // Frame cache synchronization using Windows native primitives (no MSVCP ABI issues)
    SRWLOCK m_cache_lock = SRWLOCK_INIT;
    CONDITION_VARIABLE m_cache_cv = CONDITION_VARIABLE_INIT;
    bool m_frame_computing = false;
    std::vector<float> m_cache_rgba; // out_w * out_h * 4, interleaved RGBA float [0,1]
    int m_cache_out_w = 0;
    int m_cache_out_h = 0;
    bool m_cache_valid = false;
    uint32_t m_frame_index = 0;
};

