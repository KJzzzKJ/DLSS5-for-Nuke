#include "DLSS5Live.h"
#include <algorithm>
#include <cmath>
#include <cstring>

static const char* const upscaling_modes[] = {
    "1.0x (DLAA / Native)",
    "1.5x (Quality)",
    "1.72x (Balanced)",
    "2.0x (Performance)",
    "3.0x (Ultra Performance)",
    0
};
static const char* const model_presets[] = { "Default", "J", "K", "L", "M", 0 };
static const char* const nr_styles[]     = { "Default", "Natural", "Cinematic", 0 };
static const char* const nr_presets[]    = { "Default", "Preset #1", "Preset #2", "Preset #3", 0 };
static const char* const image_transports[] = { "16-bit Half Float (Scene-Linear)", "8-bit Integer (SDR Legacy)", 0 };

static Iop* build(Node* node) { return new DLSS5Live(node); }
const Iop::Description DLSS5Live::description("DLSS5Live", "Filter/DLSS5Live", build);

int DLSS5Live::minimum_inputs() const {
    return 1;
}

int DLSS5Live::maximum_inputs() const {
    return 4;
}

int DLSS5Live::optional_input() const {
    return 1;
}

const char* DLSS5Live::input_label(int n, char*) const {
    switch (n) {
        case 0: return "";
        case 1: return "motion";
        case 2: return "depth";
        case 3: return "mask";
        default: return "";
    }
}

// Dynamic resolution for default DLSS 5 host worker
static std::string g_resolved_worker_path;
static const char* get_default_worker_path() {
    if (!g_resolved_worker_path.empty()) {
        return g_resolved_worker_path.c_str();
    }
    // 1. Environment variable
    const char* env_path = std::getenv("NUKE_DLSS5_WORKER_PATH");
    if (env_path && strlen(env_path) > 0) {
        std::string p = env_path;
        std::replace(p.begin(), p.end(), '\\', '/');
        DWORD attr = GetFileAttributesA(p.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            g_resolved_worker_path = p;
            return g_resolved_worker_path.c_str();
        }
    }
    auto check_path = [](const std::string& candidate) -> bool {
        DWORD attr = GetFileAttributesA(candidate.c_str());
        return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
    };

    // 2. Relative to this DLL module directory: <dll_dir>/runtime/DLSS_Nuke_Worker.exe or nvngx.dll
    HMODULE hMod = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&get_default_worker_path, &hMod)) {
        char modPath[MAX_PATH] = {0};
        if (GetModuleFileNameA(hMod, modPath, MAX_PATH)) {
            std::string dir = modPath;
            std::replace(dir.begin(), dir.end(), '\\', '/');
            size_t slash = dir.rfind('/');
            if (slash != std::string::npos) {
                std::string base = dir.substr(0, slash);
                const char* subpaths[] = {
                    "/runtime/DLSS_Nuke_Worker.exe",
                    "/../runtime/DLSS_Nuke_Worker.exe",
                    "/../../runtime/DLSS_Nuke_Worker.exe",
                    "/DLSS_Nuke_Worker.exe",
                    "/runtime/nvngx.dll",
                    "/../runtime/nvngx.dll",
                    "/../../runtime/nvngx.dll",
                    "/nvngx.dll"
                };
                for (const char* sub : subpaths) {
                    std::string cand = base + sub;
                    if (check_path(cand)) {
                        char fullBuf[MAX_PATH] = {0};
                        if (GetFullPathNameA(cand.c_str(), MAX_PATH, fullBuf, nullptr)) {
                            cand = fullBuf;
                            std::replace(cand.begin(), cand.end(), '\\', '/');
                        }
                        g_resolved_worker_path = cand;
                        return g_resolved_worker_path.c_str();
                    }
                }
            }
        }
    }
    // 3. ~/.nuke/DLSS5Live/runtime/
    const char* user_profile = std::getenv("USERPROFILE");
    if (user_profile) {
        std::string uBase = std::string(user_profile) + "/.nuke/DLSS5Live/runtime";
        std::replace(uBase.begin(), uBase.end(), '\\', '/');
        std::string p1Exe = uBase + "/DLSS_Nuke_Worker.exe";
        if (check_path(p1Exe)) { g_resolved_worker_path = p1Exe; return g_resolved_worker_path.c_str(); }
        std::string p1Dll = uBase + "/nvngx.dll";
        if (check_path(p1Dll)) { g_resolved_worker_path = p1Dll; return g_resolved_worker_path.c_str(); }

        // 4. ~/.nuke/runtime/ (legacy fallback)
        std::string legBase = std::string(user_profile) + "/.nuke/runtime";
        std::replace(legBase.begin(), legBase.end(), '\\', '/');
        std::string p2Exe = legBase + "/DLSS_Nuke_Worker.exe";
        if (check_path(p2Exe)) { g_resolved_worker_path = p2Exe; return g_resolved_worker_path.c_str(); }
        std::string p2Dll = legBase + "/nvngx.dll";
        if (check_path(p2Dll)) { g_resolved_worker_path = p2Dll; return g_resolved_worker_path.c_str(); }
    }
    // 5. No valid worker runtime resolved
    return "";
}

static const char* const pipeline_modes[] = {
    "Single Frame",
    "Sequence",
    "CG Multi-pass",
    0
};

static const char* const mv_sources[] = {
    "Auto Flow (OpenCV DIS)",
    "External Input 1 (motion pipe)",
    "None (Zero Motion / Pure Temporal)",
    0
};

static const char* const dis_presets[] = {
    "Fast Preview (480p)",
    "Balanced (640p - Default)",
    "High Quality (960p)",
    "Extreme (1280p)",
    "Custom...",
    0
};

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

DLSS5Live::DLSS5Live(Node* node) : Iop(node) {
    inputs(4);
    k_pipeline_mode   = MODE_SINGLE_FRAME;
    k_mv_source       = MV_SRC_DIS;
    k_dis_preset      = DIS_PRESET_BALANCED;
    k_dis_custom_width = 640;
    k_dis_iterations  = 25;

    k_motion_channels = Mask_None;
    k_mv_scale_x      = 1.0f;
    k_mv_scale_y      = 1.0f;
    k_invert_mv_x     = false;
    k_invert_mv_y     = false;

    k_upscaling_mode  = 0; // Default: 1.0x (DLAA / Native)
    k_image_transport = 0;
    k_enable_hdr_range = false; // Default: OFF
    k_model_preset    = 0; // Default
    k_nr_style        = 0; // Default
    k_nr_preset       = 0; // Default
    k_auto_mask       = false;
    k_intensity       = 1.0f;
    k_local_tone      = 1.0f;
    k_local_structure = 1.0f;
    k_skin_structure  = -1.0f;
    k_hdr_range_scale = 2.0f;
    k_nvngx_path      = get_default_worker_path();
    m_last_hash       = 0;
    m_in_w            = 0;
    m_in_h            = 0;
    m_out_w           = 0;
    m_out_h           = 0;
    m_frame_computing = false;
    m_cache_valid     = false;
    m_cache_out_w     = 0;
    m_cache_out_h     = 0;
    m_frame_index     = 0;
}

DLSS5Live::~DLSS5Live() {
    m_worker.stop();
}

// ---------------------------------------------------------------------------
// Knobs
// ---------------------------------------------------------------------------

#ifndef DLSS5_VERSION_STRING
#define DLSS5_VERSION_STRING "v1.0.0"
#endif

void DLSS5Live::knobs(Knob_Callback f) {
    Named_Text_knob(f, "title_label",
                    "<font size='5' color='#76B900'><b>DLSS5 Live</b></font> "
                    "<font size='3' color='#888888'>" DLSS5_VERSION_STRING "</font><br>"
                    "<font size='2' color='#666666'>Developed by Kai (KJ) (2026)</font>");
    Divider(f);

    Enumeration_knob(f, &k_pipeline_mode, pipeline_modes, "mode", "Mode");
    Tooltip(f, "Single Frame: Independent evaluation per frame (zero motion, reset each frame). Safe for stills.\n"
               "Sequence: Temporal accumulation across sequential frames.\n"
               "CG Multi-pass: Temporal CG workflow with optional motion and guide inputs.");

    Enumeration_knob(f, &k_mv_source, mv_sources, "mv_source", "Motion Vector Source");
    Tooltip(f, "Source for 2D Motion Vectors (screen displacement dx, dy).\n"
               "- Auto Flow (OpenCV DIS): Automatically compute dense optical flow from Input 0 in background.\n"
               "- External Input 1: Read from 'motion' input pipe (e.g. VectorGenerator, CG pass).\n"
               "- None: Send zero motion (pure temporal smoothing).");

    // OpenCV DIS Knobs
    Enumeration_knob(f, &k_dis_preset, dis_presets, "dis_preset", "DIS Quality");
    Tooltip(f, "OpenCV DIS Optical Flow preset:\n"
               "- Fast Preview: 480p downscale, fastest for 24fps interactive playback (~2ms).\n"
               "- Balanced: 640p downscale, Visual Enhancer v3.0 standard (~6ms).\n"
               "- High Quality: 960p downscale, fine details (~15ms).\n"
               "- Extreme: 1280p downscale, offline rendering (~40ms).\n"
               "- Custom: Manually specify width and iterations.");

    Int_knob(f, &k_dis_custom_width, "dis_custom_width", "Flow Width");
    Tooltip(f, "Internal flow calculation width in pixels.");

    Int_knob(f, &k_dis_iterations, "dis_iterations", "Iterations");
    Tooltip(f, "Inverse compositional iterations per patch (e.g. 12 - 48).");

    // External Input 1 Knobs
    Input_ChannelSet_knob(f, &k_motion_channels, 1, "motion_channels", "MV Channels");
    Tooltip(f, "Select 2 channels for U (X) and V (Y) motion displacement from Input 1 (e.g. forward, motion).");

    Float_knob(f, &k_mv_scale_x, "mv_scale_x", "MV Scale X");
    SetRange(f, -5.0, 5.0);
    Float_knob(f, &k_mv_scale_y, "mv_scale_y", "MV Scale Y");
    SetRange(f, -5.0, 5.0);
    Bool_knob(f, &k_invert_mv_x, "invert_mv_x", "Invert X");
    Bool_knob(f, &k_invert_mv_y, "invert_mv_y", "Invert Y");

    Divider(f, "Color & Dynamic Range");

    Enumeration_knob(f, &k_image_transport, image_transports, "color_bit_depth", "Color Bit Depth");
    Tooltip(f, "16-bit Half Float preserves full scene-linear HDR dynamic range through IPC.\n"
               "8-bit Integer reproduces legacy compatibility path: RGB is clamped to 0-1 and quantized before Feature 18.");

    Bool_knob(f, &k_enable_hdr_range, "enable_hdr_range", "Enable HDR Range");
    Tooltip(f, "Compress values above 1.0 before neural reconstruction to protect extreme highlights from clipping, then restore them afterward.");

    Float_knob(f, &k_hdr_range_scale, "hdr_range_scale", "HDR Range Scale");
    SetRange(f, 1.0, 64.0);
    Tooltip(f, "Highlight dynamic range compression ratio (default: 2.0).\n"
               "For example, 2.0 maps an input value of 2.0 to 1.0 for Feature 18 and restores it to 2.0 afterward. "
               "Protects bright lights, sun, and fire highlights from being clamped by the neural model.");

    Divider(f, "DLSS Resolution & Model");

    Enumeration_knob(f, &k_upscaling_mode, upscaling_modes, "upscaling_mode", "Upscaling Mode");
    Tooltip(f, "1.0x (DLAA): Pure Neural Reconstruction (NR) at native resolution without upscaling.\n"
               "1.5x - 3.0x: Neural Reconstruction + AI Super Resolution.");

    Enumeration_knob(f, &k_model_preset, model_presets, "model_preset", "DLSS Model Preset");
    Tooltip(f, "DLSS 5 model architecture preset. 'J' is optimized for neural reconstruction.");

    Enumeration_knob(f, &k_nr_style, nr_styles, "nr_style", "NR Style");
    Tooltip(f, "Neural Reconstruction style:\n"
               "- Default: Balanced neural enhancement.\n"
               "- Natural: Organic, soft detail retention.\n"
               "- Cinematic: Film-grade texture and grain preservation.");

    Enumeration_knob(f, &k_nr_preset, nr_presets, "nr_preset", "NR Preset");
    Tooltip(f, "Neural Reconstruction tuning preset (Default, Preset #1, #2, #3).");

    Bool_knob(f, &k_auto_mask, "auto_mask", "Automatic Mask");
    Tooltip(f, "Automatically mask out non-neural UI / static elements.");

    Divider(f, "Neural Tuning");

    Float_knob(f, &k_intensity, "intensity", "Intensity");
    SetRange(f, 0.0, 2.0);
    Tooltip(f, "Global Neural Reconstruction strength (0.0 - 2.0; 1.0 is default).");

    Float_knob(f, &k_local_tone, "local_tone", "Local Tone");
    SetRange(f, 0.0, 2.0);
    Tooltip(f, "Local tone and dynamic contrast adjustment (0.0 - 2.0; 1.0 is default).");

    Float_knob(f, &k_local_structure, "local_structure", "Local Structure");
    SetRange(f, 0.0, 2.0);
    Tooltip(f, "Local detail and structural texture enhancement (0.0 - 2.0; 1.0 is default).");

    Float_knob(f, &k_skin_structure, "skin_structure", "Skin Structure");
    SetRange(f, -1.0, 2.0);
    Tooltip(f, "Skin-specific detail enhancement (-1.0 - 2.0). -1.0 is the native default (inherits from Local Structure).");

    Divider(f);
    File_knob(f, &k_nvngx_path, "nvngx_path", "nvngx.dll (Worker) Path");
}

void DLSS5Live::update_ui_visibility() {
    bool is_single  = (k_pipeline_mode == MODE_SINGLE_FRAME);
    bool is_cg      = (k_pipeline_mode == MODE_CG);
    bool is_seq     = (k_pipeline_mode == MODE_SEQUENCE);
    bool has_temporal_motion = is_seq || is_cg;
    bool is_dis     = (has_temporal_motion && k_mv_source == MV_SRC_DIS);
    bool is_ext     = (has_temporal_motion && k_mv_source == MV_SRC_INPUT1);
    bool is_custom  = (is_dis && k_dis_preset == DIS_PRESET_CUSTOM);

    auto set_vis = [](Knob* k, bool show) {
        if (!k) return;
        k->visible(show);
        k->set_flag(Knob::HIDDEN, !show);
    };

    set_vis(knob("mv_source"), has_temporal_motion);

    // DIS knobs
    set_vis(knob("dis_preset"), is_dis);
    set_vis(knob("dis_custom_width"), is_custom);
    set_vis(knob("dis_iterations"), is_custom);

    // External Input 1 knobs
    set_vis(knob("motion_channels"), is_ext);
    set_vis(knob("mv_scale_x"), is_ext);
    set_vis(knob("mv_scale_y"), is_ext);
    set_vis(knob("invert_mv_x"), is_ext);
    set_vis(knob("invert_mv_y"), is_ext);

    bool is_16bit = (k_image_transport == 0);
    set_vis(knob("enable_hdr_range"), is_16bit);
    set_vis(knob("hdr_range_scale"), is_16bit && k_enable_hdr_range);
}

int DLSS5Live::knob_changed(Knob* k) {
    if (k == &Knob::showPanel ||
        k->is("mode") ||
        k->is("pipeline_mode") ||
        k->is("mv_source") ||
        k->is("dis_preset") ||
        k->is("color_bit_depth") ||
        k->is("image_transport") ||
        k->is("enable_hdr_range")) {
        update_ui_visibility();
        return 1;
    }
    m_last_hash = 0; // Force rebuild on next _validate(for_real=true)
    return Iop::knob_changed(k);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

float DLSS5Live::getScaleFactor() const {
    switch (k_upscaling_mode) {
        case 0: return 1.0f;        // 1.0x (DLAA / Native)
        case 1: return 1.5f;        // 1.5x (Quality)
        case 2: return 1.7241379f;  // 1.72x (Balanced)
        case 3: return 2.0f;        // 2.0x (Performance)
        case 4: return 3.0f;        // 3.0x (Ultra Performance)
        default: return 1.0f;
    }
}

uint32_t DLSS5Live::getPerfQuality() const {
    switch (k_upscaling_mode) {
        case 0: return 5; // DLAA (1.0x native)
        case 1: return 2; // Quality (1.5x)
        case 2: return 1; // Balanced (1.72x)
        case 3: return 0; // Performance (2.0x)
        case 4: return 3; // Ultra Performance (3.0x)
        default: return 5;
    }
}

uint32_t DLSS5Live::getModelPreset() const {
    switch (k_model_preset) {
        case 0: return 0;  // Default
        case 1: return 10; // J
        case 2: return 11; // K
        case 3: return 12; // L
        case 4: return 13; // M
        default: return 0;
    }
}

size_t DLSS5Live::computeSettingsHash() const {
    size_t h = (size_t)(k_pipeline_mode + 1) * 10007u;
    h ^= (size_t)(k_mv_source + 1) * 20011u;
    h ^= (size_t)(k_dis_preset + 1) * 330017u;
    h ^= (size_t)(k_dis_custom_width) * 350029u;
    h ^= (size_t)(k_dis_iterations) * 370043u;

    h ^= (size_t)(k_upscaling_mode + 1) * 100003u;
    h ^= (size_t)(k_image_transport + 1) * 150001u;
    h ^= (size_t)(k_model_preset + 1) * 200003u;
    h ^= (size_t)(k_nr_style + 1) * 250007u;
    h ^= (size_t)(k_nr_preset + 1) * 270011u;
    h ^= (size_t)(k_auto_mask ? 1 : 0) * 290017u;
    h ^= (size_t)(m_in_w) * 300007u;
    h ^= (size_t)(m_in_h) * 400009u;
    uint32_t ibits;
    std::memcpy(&ibits, &k_intensity, 4);       h ^= (size_t)ibits * 500011u;
    std::memcpy(&ibits, &k_local_tone, 4);      h ^= (size_t)ibits * 600013u;
    std::memcpy(&ibits, &k_local_structure, 4); h ^= (size_t)ibits * 700019u;
    std::memcpy(&ibits, &k_skin_structure, 4);  h ^= (size_t)ibits * 800021u;
    h ^= (size_t)(k_enable_hdr_range ? 1 : 0) * 820023u;
    std::memcpy(&ibits, &k_hdr_range_scale, 4); h ^= (size_t)ibits * 850019u;
    std::memcpy(&ibits, &k_mv_scale_x, 4);      h ^= (size_t)ibits * 900037u;
    std::memcpy(&ibits, &k_mv_scale_y, 4);      h ^= (size_t)ibits * 900053u;
    h ^= (size_t)(k_invert_mv_x ? 1 : 0) * 950009u;
    h ^= (size_t)(k_invert_mv_y ? 1 : 0) * 950021u;
    if (k_nvngx_path) {
        for (const char* p = k_nvngx_path; *p; p++)
            h = h * 31u + (unsigned char)*p;
    }
    return h ? h : 1u;
}

void DLSS5Live::rebuildWorker() {
    m_worker.stop();

    if (!k_nvngx_path || k_nvngx_path[0] == '\0' || m_in_w <= 0 || m_in_h <= 0) {
        fprintf(stderr, "[DLSS5Live] rebuildWorker aborted: path or dim invalid (in=%dx%d)\n", m_in_w, m_in_h);
        return;
    }

    std::string path = k_nvngx_path;
    std::replace(path.begin(), path.end(), '\\', '/');

    // Auto-heal if Nuke knob stripped backslashes (e.g. C:Users -> C:/Users)
    if (path.rfind("C:Users", 0) == 0) {
        path = "C:/Users" + path.substr(7);
    }

    DWORD attr = GetFileAttributesA(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        // Fallback to auto-detected default worker path
        const char* def_path = get_default_worker_path();
        if (def_path) {
            path = def_path;
            attr = GetFileAttributesA(path.c_str());
        }
    }

    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        fprintf(stderr, "[DLSS5Live] rebuildWorker aborted: file does not exist: %s\n", path.c_str());
        return;
    }

    VideoHeader header;
    header.magic             = k_image_transport == 1 ? 0x34563544u : 0x35563544u;
    header.input_width       = (uint32_t)m_in_w;
    header.input_height      = (uint32_t)m_in_h;
    header.output_width      = (uint32_t)m_out_w;
    header.output_height     = (uint32_t)m_out_h;
    header.warmup_frames     = 1;
    header.frame_count       = 100000;
    header.perf_quality      = getPerfQuality();
    header.dlss_model_preset = getModelPreset();
    header.profile           = 0;
    header.preset            = (uint32_t)k_nr_preset;
    header.style             = (uint32_t)k_nr_style;
    header.auto_mask         = k_auto_mask ? 1 : 0;
    header.ui_correction     = 0;
    header.intensity         = k_intensity;
    header.local_tone        = k_local_tone;
    header.local_structure   = k_local_structure;
    header.skin_structure    = k_skin_structure;

    // MV and DIS configuration
    if (k_pipeline_mode == MODE_SINGLE_FRAME) {
        header.mv_mode = 0; // None (Zero Motion)
    } else {
        if (k_mv_source == MV_SRC_DIS) {
            header.mv_mode = 2; // Auto DIS
        } else if (k_mv_source == MV_SRC_INPUT1) {
            header.mv_mode = 1; // External Buffer
        } else {
            header.mv_mode = 0; // None
        }
    }

    header.dis_preset = (uint32_t)k_dis_preset;
    switch (k_dis_preset) {
        case DIS_PRESET_FAST:     header.dis_flow_width = 480;  header.dis_iterations = 12; break;
        case DIS_PRESET_BALANCED: header.dis_flow_width = 640;  header.dis_iterations = 25; break;
        case DIS_PRESET_HIGH:     header.dis_flow_width = 960;  header.dis_iterations = 32; break;
        case DIS_PRESET_EXTREME:  header.dis_flow_width = 1280; header.dis_iterations = 48; break;
        case DIS_PRESET_CUSTOM:
        default:
            header.dis_flow_width = (uint32_t)(k_dis_custom_width > 0 ? k_dis_custom_width : 640);
            header.dis_iterations = (uint32_t)(k_dis_iterations > 0 ? k_dis_iterations : 25);
            break;
    }
    header._reserved_scene_cut = 0;
    header._reserved_thresh = 0.0f;

    bool ok = m_worker.start(path, header, m_setup_info);
    fprintf(stderr, "[DLSS5Live] rebuildWorker start (%s) returned: %d (worker_running=%d)\n", path.c_str(), (int)ok, (int)m_worker.isRunning());
}

// ---------------------------------------------------------------------------
// Validate
// ---------------------------------------------------------------------------

void DLSS5Live::_validate(bool for_real) {
    update_ui_visibility();
    copy_info();

    if (!node_input(0) || !input(0)) {
        fprintf(stderr, "[DLSS5Live] _validate: no input 0\n");
        return;
    }

    float factor = getScaleFactor();

    int in_w = info_.w();
    int in_h = info_.h();
    if (in_w <= 0 || in_h <= 0) {
        fprintf(stderr, "[DLSS5Live] _validate: in_w or in_h <= 0 (%dx%d)\n", in_w, in_h);
        return;
    }

    int out_w = static_cast<int>(in_w * factor);
    int out_h = static_cast<int>(in_h * factor);
    double pa = info_.format().pixel_aspect();
    if (pa <= 0.0) pa = 1.0;

    Format* fmt = Format::findExisting(out_w, out_h, pa);
    if (!fmt) {
        Format* new_fmt = new Format(out_w, out_h, pa);
        new_fmt->add(nullptr);
        fmt = new_fmt;
    }
    info_.format(*fmt);
    info_.full_size_format(*fmt);
    info_.set(0, 0, out_w, out_h);

    m_in_w = in_w;
    m_in_h = in_h;
    m_out_w = out_w;
    m_out_h = out_h;

    // Invalidate frame cache safely under lock
    AcquireSRWLockExclusive(&m_cache_lock);
    m_cache_valid = false;
    ReleaseSRWLockExclusive(&m_cache_lock);

    // Rebuild DLSS worker only when settings change, and only on for_real pass
    if (for_real) {
        size_t new_hash = computeSettingsHash();
        if (new_hash != m_last_hash) {
            m_last_hash = new_hash;
            rebuildWorker();
        }
    }
}

// ---------------------------------------------------------------------------
// Request
// ---------------------------------------------------------------------------

void DLSS5Live::_request(int x, int y, int r, int t, ChannelMask channels, int count) {
    if (!node_input(0) || !input(0)) return;

    int in_w = input0().info().w();
    int in_h = input0().info().h();
    if (in_w <= 0 || in_h <= 0) return;

    if (m_worker.isRunning()) {
        // DLSS needs full frame RGBA from input 0
        input0().request(0, 0, in_w, in_h, Mask_RGBA, count);

        // Request Motion Vectors if enabled
        if (k_pipeline_mode != MODE_SINGLE_FRAME) {
            bool need_mv = (k_mv_source == MV_SRC_INPUT1);
            if (need_mv && node_input(1) && input(1)) {
                int mv_w = input(1)->info().w();
                int mv_h = input(1)->info().h();
                if (mv_w > 0 && mv_h > 0) {
                    ChannelSet req_ch = k_motion_channels;
                    if (req_ch.empty()) req_ch = Mask_RGBA;
                    input(1)->request(0, 0, mv_w, mv_h, req_ch, count);
                }
            }
        }

        // Request the guide buffers supported by the current DLSSNR runtime.
        if (k_pipeline_mode == MODE_CG) {
            if (node_input(2) && input(2)) {
                int dw = input(2)->info().w(), dh = input(2)->info().h();
                if (dw > 0 && dh > 0) {
                    input(2)->request(0, 0, dw, dh, input(2)->info().channels(), count);
                }
            }
            if (node_input(3) && input(3)) {
                int mw = input(3)->info().w(), mh = input(3)->info().h();
                if (mw > 0 && mh > 0) {
                    input(3)->request(0, 0, mw, mh, input(3)->info().channels(), count);
                }
            }
        }
    } else {
        // Fallback: request scaled sub-region
        float factor = getScaleFactor();
        int in_x = std::clamp(static_cast<int>(x / factor) - 1, 0, in_w);
        int in_r = std::clamp(static_cast<int>(std::ceil(r / factor)) + 2, 0, in_w);
        int in_y = std::clamp(static_cast<int>(y / factor) - 1, 0, in_h);
        int in_t = std::clamp(static_cast<int>(std::ceil(t / factor)) + 2, 0, in_h);
        if (in_x < in_r && in_y < in_t) {
            input0().request(in_x, in_y, in_r, in_t, channels, count);
        }
    }
}

// ---------------------------------------------------------------------------
// Frame cache computation & Half-float helper
// ---------------------------------------------------------------------------

static inline uint16_t floatToHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000;
    int32_t exp = ((x >> 23) & 0xff) - 127;
    uint32_t mant = x & 0x7fffff;

    if (exp > 15) {
        return (uint16_t)(sign | 0x7c00);
    } else if (exp < -14) {
        if (exp < -24) return (uint16_t)sign;
        mant |= 0x800000;
        return (uint16_t)(sign | (mant >> (-exp - 14 + 13)));
    } else {
        return (uint16_t)(sign | ((exp + 15) << 10) | (mant >> 13));
    }
}

static inline float HalfToFloat(uint16_t h) {
    uint32_t sign = (h >> 15) & 1;
    uint32_t exponent = (h >> 10) & 0x1f;
    uint32_t mantissa = h & 0x3ff;
    uint32_t bits;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign << 31;
        } else {
            exponent = 1;
            while (!(mantissa & 0x400)) { mantissa <<= 1; --exponent; }
            mantissa &= 0x3ff;
            bits = (sign << 31) | ((exponent + 112) << 23) | (mantissa << 13);
        }
    } else if (exponent == 0x1f) {
        bits = (sign << 31) | 0x7f800000u | (mantissa << 13);
    } else {
        bits = (sign << 31) | ((exponent + 112) << 23) | (mantissa << 13);
    }
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool DLSS5Live::computeFrameCache() {
    if (!node_input(0) || !input(0)) return false;

    int in_w = m_in_w, in_h = m_in_h;
    int out_w = m_out_w, out_h = m_out_h;
    if (in_w <= 0 || in_h <= 0 || out_w <= 0 || out_h <= 0) return false;

    // Input contract: preserve Nuke's upstream numeric values as linear RGBA16F.
    // HDR Range Scale optionally compresses RGB around Feature 18 and restores it on output.
    const bool legacy_rgba8 = k_image_transport == 1;
    const int target_rw = (m_setup_info.render_width > 0) ? (int)m_setup_info.render_width : in_w;
    const int target_rh = (m_setup_info.render_height > 0) ? (int)m_setup_info.render_height : in_h;

    std::vector<uint8_t> payload_in_8;
    std::vector<uint16_t> payload_in_16;

    if (legacy_rgba8) {
        // Direct uint8 packing (Legacy Compatibility path)
        std::vector<uint8_t> raw_in((size_t)in_w * in_h * 4, 0);
        for (int ry = 0; ry < in_h; ry++) {
            Row row(0, in_w);
            row.get(input0(), ry, 0, in_w, Mask_RGBA);
            const float* rp = row[Chan_Red];
            const float* gp = row[Chan_Green];
            const float* bp = row[Chan_Blue];
            const float* ap = row[Chan_Alpha];

            int dy = in_h - 1 - ry;
            for (int px = 0; px < in_w; px++) {
                size_t idx = ((size_t)dy * in_w + px) * 4;
                auto cvt = [](const float* p, int i, uint8_t def) -> uint8_t {
                    if (!p) return def;
                    return (uint8_t)std::clamp((int)(p[i] * 255.0f + 0.5f), 0, 255);
                };
                raw_in[idx + 0] = cvt(rp, px, 0);
                raw_in[idx + 1] = cvt(gp, px, 0);
                raw_in[idx + 2] = cvt(bp, px, 0);
                raw_in[idx + 3] = cvt(ap, px, 255);
            }
        }

        if (target_rw == in_w && target_rh == in_h) {
            payload_in_8 = std::move(raw_in);
        } else {
            payload_in_8.resize((size_t)target_rw * target_rh * 4, 0);
            for (int y = 0; y < target_rh; y++) {
                int src_y = std::clamp((y * in_h) / target_rh, 0, in_h - 1);
                for (int x = 0; x < target_rw; x++) {
                    int src_x = std::clamp((x * in_w) / target_rw, 0, in_w - 1);
                    size_t src_idx = ((size_t)src_y * in_w + src_x) * 4;
                    size_t dst_idx = ((size_t)y * target_rw + x) * 4;
                    payload_in_8[dst_idx + 0] = raw_in[src_idx + 0];
                    payload_in_8[dst_idx + 1] = raw_in[src_idx + 1];
                    payload_in_8[dst_idx + 2] = raw_in[src_idx + 2];
                    payload_in_8[dst_idx + 3] = raw_in[src_idx + 3];
                }
            }
        }
    } else {
        // Direct half-float packing (Linear RGBA16F path)
        const float hdr_range_scale = k_enable_hdr_range ? std::max(k_hdr_range_scale, 1.0f) : 1.0f;
        std::vector<uint16_t> raw_in((size_t)in_w * in_h * 4, 0);
        for (int ry = 0; ry < in_h; ry++) {
            Row row(0, in_w);
            row.get(input0(), ry, 0, in_w, Mask_RGBA);
            const float* rp = row[Chan_Red];
            const float* gp = row[Chan_Green];
            const float* bp = row[Chan_Blue];
            const float* ap = row[Chan_Alpha];

            int dy = in_h - 1 - ry;
            for (int px = 0; px < in_w; px++) {
                size_t idx = ((size_t)dy * in_w + px) * 4;
                auto cvt_rgb = [hdr_range_scale](const float* p, int i) -> uint16_t {
                    return floatToHalf((p ? p[i] : 0.0f) / hdr_range_scale);
                };
                raw_in[idx + 0] = cvt_rgb(rp, px);
                raw_in[idx + 1] = cvt_rgb(gp, px);
                raw_in[idx + 2] = cvt_rgb(bp, px);
                raw_in[idx + 3] = floatToHalf(ap ? ap[px] : 1.0f);
            }
        }

        if (target_rw == in_w && target_rh == in_h) {
            payload_in_16 = std::move(raw_in);
        } else {
            payload_in_16.resize((size_t)target_rw * target_rh * 4, 0);
            for (int y = 0; y < target_rh; y++) {
                int src_y = std::clamp((y * in_h) / target_rh, 0, in_h - 1);
                for (int x = 0; x < target_rw; x++) {
                    int src_x = std::clamp((x * in_w) / target_rw, 0, in_w - 1);
                    size_t src_idx = ((size_t)src_y * in_w + src_x) * 4;
                    size_t dst_idx = ((size_t)y * target_rw + x) * 4;
                    payload_in_16[dst_idx + 0] = raw_in[src_idx + 0];
                    payload_in_16[dst_idx + 1] = raw_in[src_idx + 1];
                    payload_in_16[dst_idx + 2] = raw_in[src_idx + 2];
                    payload_in_16[dst_idx + 3] = raw_in[src_idx + 3];
                }
            }
        }
    }

    // Determine Motion Vector buffer
    Iop* mv_iop = nullptr;
    if (k_pipeline_mode != MODE_SINGLE_FRAME) {
        if (k_mv_source == MV_SRC_INPUT1 && node_input(1) && input(1)) {
            mv_iop = input(1);
        }
    }

    std::vector<uint16_t> mv_payload;
    if (mv_iop) {
        Channel u_chan = Chan_Black;
        Channel v_chan = Chan_Black;

        if (!k_motion_channels.empty()) {
            int count = 0;
            foreach(c, k_motion_channels) {
                if (count == 0) u_chan = c;
                else if (count == 1) v_chan = c;
                count++;
                if (count >= 2) break;
            }
        }
        if (u_chan == Chan_Black) {
            u_chan = channel("forward.u");
            v_chan = channel("forward.v");
        }
        if (u_chan == Chan_Black) {
            u_chan = channel("motion.u");
            v_chan = channel("motion.v");
        }
        if (u_chan == Chan_Black) {
            // Also supports RG or RGB passes (e.g. Arnold / Redshift / V-Ray vector AOVs)
            u_chan = Chan_Red;
            v_chan = Chan_Green;
        }

        int mv_w = mv_iop->info().w();
        int mv_h = mv_iop->info().h();
        if (mv_w > 0 && mv_h > 0 && u_chan != Chan_Black && v_chan != Chan_Black) {
            ChannelSet fetch_ch;
            fetch_ch += u_chan;
            fetch_ch += v_chan;

            mv_payload.resize((size_t)target_rw * target_rh * 2, 0);

            for (int ry = 0; ry < target_rh; ry++) {
                int src_y = std::clamp((ry * mv_h) / target_rh, 0, mv_h - 1);
                Row mv_row(0, mv_w);
                mv_row.get(*mv_iop, src_y, 0, mv_w, fetch_ch);

                const float* up = mv_row[u_chan];
                const float* vp = mv_row[v_chan];

                int dy = target_rh - 1 - ry;
                for (int rx = 0; rx < target_rw; rx++) {
                    int src_x = std::clamp((rx * mv_w) / target_rw, 0, mv_w - 1);
                    float u = up ? up[src_x] : 0.0f;
                    float v = vp ? vp[src_x] : 0.0f;

                    u *= k_mv_scale_x;
                    v *= k_mv_scale_y;
                    if (k_invert_mv_x) u = -u;
                    if (k_invert_mv_y) v = -v;

                    size_t idx = ((size_t)dy * target_rw + rx) * 2;
                    mv_payload[idx + 0] = floatToHalf(u);
                    mv_payload[idx + 1] = floatToHalf(v);
                }
            }
        }
    }

    auto pack_scalar_guide = [&](int input_index, bool is_depth, std::vector<float>& payload) {
        if (k_pipeline_mode != MODE_CG || !node_input(input_index) || !input(input_index)) return;

        Iop* guide = input(input_index);
        const ChannelSet available = guide->info().channels();
        Channel selected = Chan_Black;
        if (is_depth) {
            Channel depth_z = channel("depth.Z");
            if (depth_z != Chan_Black && available.contains(depth_z)) selected = depth_z;
            else if (available.contains(Chan_Red)) selected = Chan_Red;
            else if (available.contains(Chan_Alpha)) selected = Chan_Alpha;
        } else {
            if (available.contains(Chan_Alpha)) selected = Chan_Alpha;
            else if (available.contains(Chan_Red)) selected = Chan_Red;
        }
        if (selected == Chan_Black) return;

        int guide_w = guide->info().w();
        int guide_h = guide->info().h();
        if (guide_w <= 0 || guide_h <= 0) return;

        ChannelSet fetch_ch;
        fetch_ch += selected;
        payload.resize((size_t)target_rw * target_rh, 0.0f);
        for (int ry = 0; ry < target_rh; ++ry) {
            int src_y = std::clamp((ry * guide_h) / target_rh, 0, guide_h - 1);
            Row guide_row(0, guide_w);
            guide_row.get(*guide, src_y, 0, guide_w, fetch_ch);
            const float* values = guide_row[selected];
            int dy = target_rh - 1 - ry;
            for (int rx = 0; rx < target_rw; ++rx) {
                int src_x = std::clamp((rx * guide_w) / target_rw, 0, guide_w - 1);
                float value = values ? values[src_x] : 0.0f;
                if (!std::isfinite(value)) value = 0.0f;
                if (!is_depth) value = std::clamp(value, 0.0f, 1.0f);
                payload[(size_t)dy * target_rw + rx] = value;
            }
        }
    };

    std::vector<float> depth_payload;
    std::vector<float> control_mask_payload;
    pack_scalar_guide(2, true, depth_payload);
    pack_scalar_guide(3, false, control_mask_payload);

    // Reset logic:
    // In Single Frame mode: force reset every frame for independent evaluation
    // In Sequence / CG Multi-pass mode: sequential forward playback retains temporal history
    int current_frame = (int)outputContext().frame();
    bool reset = true;

    if (k_pipeline_mode == MODE_SEQUENCE || k_pipeline_mode == MODE_CG) {
        if (m_frame_index != 0 && current_frame == (int)m_frame_index + 1) {
            reset = false;
        } else {
            reset = true;
        }
    } else {
        reset = true;
    }
    m_frame_index = (uint32_t)current_frame;

    // Send to DLSS worker
    std::vector<uint8_t> out_rgba;
    bool has_ext_mv = (k_pipeline_mode != MODE_SINGLE_FRAME && k_mv_source == MV_SRC_INPUT1 && !mv_payload.empty());
    bool ok = m_worker.processFrame(
        (uint32_t)current_frame,
        reset,
        (int64_t)current_frame * 33333,
        legacy_rgba8 ? payload_in_8.data() : reinterpret_cast<const uint8_t*>(payload_in_16.data()),
        legacy_rgba8 ? payload_in_8.size() : payload_in_16.size() * sizeof(uint16_t),
        has_ext_mv ? (const uint8_t*)mv_payload.data() : nullptr,
        has_ext_mv ? mv_payload.size() * sizeof(uint16_t) : 0,
        depth_payload.empty() ? nullptr : (const uint8_t*)depth_payload.data(),
        depth_payload.size() * sizeof(float),
        control_mask_payload.empty() ? nullptr : (const uint8_t*)control_mask_payload.data(),
        control_mask_payload.size() * sizeof(float),
        out_rgba
    );

    int worker_out_w = (m_setup_info.output_width > 0) ? (int)m_setup_info.output_width : out_w;
    int worker_out_h = (m_setup_info.output_height > 0) ? (int)m_setup_info.output_height : out_h;

    if (!ok || out_rgba.size() != (size_t)(worker_out_w * worker_out_h * 4) * (legacy_rgba8 ? 1u : 2u)) {
        return false;
    }

    // Convert worker output back to Nuke's float cache.
    // Worker output is in standard top-down layout (row 0 at top).
    // Nuke's m_cache_rgba is indexed by Nuke's scanline y (y=0 at bottom).
    m_cache_rgba.resize((size_t)out_w * out_h * 4);
    if (legacy_rgba8) {
        for (int y = 0; y < out_h; y++) {
            int dlss_y = out_h - 1 - y;
            int src_y = (worker_out_w == out_w && worker_out_h == out_h)
                        ? dlss_y
                        : std::clamp((dlss_y * worker_out_h) / out_h, 0, worker_out_h - 1);
            for (int x = 0; x < out_w; x++) {
                int src_x = (worker_out_w == out_w && worker_out_h == out_h)
                            ? x
                            : std::clamp((x * worker_out_w) / out_w, 0, worker_out_w - 1);
                size_t src_idx = ((size_t)src_y * worker_out_w + src_x) * 4;
                size_t dst_idx = ((size_t)y * out_w + x) * 4;
                m_cache_rgba[dst_idx + 0] = out_rgba[src_idx + 0] / 255.0f;
                m_cache_rgba[dst_idx + 1] = out_rgba[src_idx + 1] / 255.0f;
                m_cache_rgba[dst_idx + 2] = out_rgba[src_idx + 2] / 255.0f;
                m_cache_rgba[dst_idx + 3] = out_rgba[src_idx + 3] / 255.0f;
            }
        }
    } else {
        const uint16_t* out16 = reinterpret_cast<const uint16_t*>(out_rgba.data());
        for (int y = 0; y < out_h; y++) {
            int dlss_y = out_h - 1 - y;
            int src_y = (worker_out_w == out_w && worker_out_h == out_h)
                        ? dlss_y
                        : std::clamp((dlss_y * worker_out_h) / out_h, 0, worker_out_h - 1);
            for (int x = 0; x < out_w; x++) {
                int src_x = (worker_out_w == out_w && worker_out_h == out_h)
                            ? x
                            : std::clamp((x * worker_out_w) / out_w, 0, worker_out_w - 1);
                size_t src_idx = ((size_t)src_y * worker_out_w + src_x) * 4;
                size_t dst_idx = ((size_t)y * out_w + x) * 4;
                m_cache_rgba[dst_idx + 0] = HalfToFloat(out16[src_idx + 0]);
                m_cache_rgba[dst_idx + 1] = HalfToFloat(out16[src_idx + 1]);
                m_cache_rgba[dst_idx + 2] = HalfToFloat(out16[src_idx + 2]);
                m_cache_rgba[dst_idx + 3] = HalfToFloat(out16[src_idx + 3]);
            }
        }

        const float hdr_range_scale = k_enable_hdr_range ? std::max(k_hdr_range_scale, 1.0f) : 1.0f;
        if (hdr_range_scale != 1.0f) {
            for (size_t i = 0; i < m_cache_rgba.size(); i += 4) {
                m_cache_rgba[i + 0] *= hdr_range_scale;
                m_cache_rgba[i + 1] *= hdr_range_scale;
                m_cache_rgba[i + 2] *= hdr_range_scale;
            }
        }
    }

    m_cache_out_w = out_w;
    m_cache_out_h = out_h;
    return true;
}

// ---------------------------------------------------------------------------
// Fallback Scaler
// ---------------------------------------------------------------------------

void DLSS5Live::fallbackScale(int y, int x, int r, ChannelMask channels, Row& out_row) {
    if (!node_input(0) || !input(0)) return;

    float factor = getScaleFactor();
    int in_w = input0().info().w();
    int in_h = input0().info().h();
    if (in_w <= 0 || in_h <= 0) return;

    float src_yf = static_cast<float>(y) / factor;
    int src_y = std::clamp(static_cast<int>(src_yf), 0, in_h - 1);

    int in_x = std::clamp(static_cast<int>(x / factor) - 1, 0, in_w);
    int in_r = std::clamp(static_cast<int>(std::ceil(r / factor)) + 2, 0, in_w);

    if (in_x >= in_r) return;

    Row in_row(in_x, in_r);
    in_row.get(input0(), src_y, in_x, in_r, channels);

    foreach(z, channels) {
        const float* in_ptr = in_row[z];
        float* out_ptr = out_row.writable(z);
        if (!in_ptr || !out_ptr) continue;
        for (int cur_x = x; cur_x < r; ++cur_x) {
            float src_xf = static_cast<float>(cur_x) / factor;
            int sx = std::clamp(static_cast<int>(src_xf), in_x, in_r - 1);
            out_ptr[cur_x] = in_ptr[sx];
        }
    }
}

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

void DLSS5Live::engine(int y, int x, int r, ChannelMask channels, Row& out_row) {
    if (!node_input(0) || !input(0)) return;

    // ---- DLSS path -------------------------------------------------------
    if (m_worker.isRunning()) {
        AcquireSRWLockExclusive(&m_cache_lock);
        if (!m_cache_valid) {
            if (!m_frame_computing) {
                m_frame_computing = true;
                ReleaseSRWLockExclusive(&m_cache_lock);
                bool ok = computeFrameCache();
                AcquireSRWLockExclusive(&m_cache_lock);
                m_cache_valid = ok;
                m_frame_computing = false;
                WakeAllConditionVariable(&m_cache_cv);
            } else {
                while (m_frame_computing) {
                    SleepConditionVariableSRW(&m_cache_cv, &m_cache_lock, INFINITE, 0);
                }
            }
        }
        bool valid = m_cache_valid;
        ReleaseSRWLockExclusive(&m_cache_lock);

        if (valid) {
            int cw = m_cache_out_w, ch = m_cache_out_h;
            foreach(z, channels) {
                int ci = (z == Chan_Red) ? 0 : (z == Chan_Green) ? 1 :
                         (z == Chan_Blue) ? 2 : (z == Chan_Alpha) ? 3 : -1;
                if (ci < 0) continue;
                float* out_ptr = out_row.writable(z);
                if (!out_ptr) continue;
                for (int cx = x; cx < r; ++cx) {
                    if (cx >= 0 && cx < cw && y >= 0 && y < ch)
                        out_ptr[cx] = m_cache_rgba[((size_t)y * cw + cx) * 4 + ci];
                    else
                        out_ptr[cx] = (ci == 3) ? 1.0f : 0.0f;
                }
            }
            return;
        }
    }

    // ---- Fallback: Nearest-neighbour ------------------------------------
    fallbackScale(y, x, r, channels, out_row);
}
