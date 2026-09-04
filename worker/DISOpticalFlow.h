#pragma once
#include <cstdint>
#include <vector>
#include <cmath>
#include <algorithm>

struct DISParams {
    int flow_width   = 640;
    int finest_scale = 1;      // 0 = level 0, 1 = level 1, 2 = level 2
    int iterations   = 25;     // 12 for ultrafast, 25 for medium, 32 for slow, 48 for extreme
    int patch_stride = 4;      // 3 or 4
    int patch_size   = 8;
};

class DISOpticalFlow {
public:
    DISOpticalFlow() = default;
    ~DISOpticalFlow() = default;

    // Convert RGBA8 (inW x inH) to Grayscale (flowW x flowH)
    static void downscaleRgbaToGray(
        const uint8_t* rgba, int inW, int inH,
        uint8_t* gray, int flowW, int flowH);

    // Compute dense optical flow pointing from current (I0) to previous (I1)
    // Output: flowU, flowV of size (inW x inH)
    void compute(
        const uint8_t* currentGray,
        const uint8_t* prevGray,
        int flowW, int flowH,
        int inW, int inH,
        const DISParams& params,
        std::vector<float>& flowU,
        std::vector<float>& flowV
    );

private:
    struct Level {
        int w = 0, h = 0;
        std::vector<uint8_t> I0;
        std::vector<uint8_t> I1;
        std::vector<float> gradX;
        std::vector<float> gradY;
        std::vector<float> u;
        std::vector<float> v;
    };

    void buildPyramid(
        const uint8_t* I0, const uint8_t* I1,
        int w, int h, int maxLevels,
        std::vector<Level>& pyr);

    void computeGradients(Level& lvl);

    void solveLevel(
        Level& lvl,
        const DISParams& params);

    void upsampleFlow(
        const std::vector<float>& srcU, const std::vector<float>& srcV,
        int srcW, int srcH,
        std::vector<float>& dstU, std::vector<float>& dstV,
        int dstW, int dstH,
        float scale);
};
