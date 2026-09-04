#include "DISOpticalFlow.h"
#include <cstring>
#include <immintrin.h>

void DISOpticalFlow::downscaleRgbaToGray(
    const uint8_t* rgba, int inW, int inH,
    uint8_t* gray, int flowW, int flowH)
{
    if (!rgba || !gray || inW <= 0 || inH <= 0 || flowW <= 0 || flowH <= 0) return;

    for (int dy = 0; dy < flowH; ++dy) {
        int sy = std::clamp((dy * inH) / flowH, 0, inH - 1);
        const uint8_t* srcRow = rgba + (size_t)sy * inW * 4;
        uint8_t* dstRow = gray + (size_t)dy * flowW;

        for (int dx = 0; dx < flowW; ++dx) {
            int sx = std::clamp((dx * inW) / flowW, 0, inW - 1);
            const uint8_t* p = srcRow + sx * 4;
            // Standard BT.601 luma: 0.299 R + 0.587 G + 0.114 B
            uint32_t y = (299u * p[0] + 587u * p[1] + 114u * p[2] + 500u) / 1000u;
            dstRow[dx] = (uint8_t)std::min(y, 255u);
        }
    }
}


static inline float sampleBilinear(const uint8_t* img, int w, int h, float x, float y) {
    x = std::clamp(x, 0.0f, (float)(w - 1));
    y = std::clamp(y, 0.0f, (float)(h - 1));
    int x0 = (int)x;
    int y0 = (int)y;
    int x1 = std::min(x0 + 1, w - 1);
    int y1 = std::min(y0 + 1, h - 1);
    float fx = x - x0;
    float fy = y - y0;

    float v00 = (float)img[y0 * w + x0];
    float v10 = (float)img[y0 * w + x1];
    float v01 = (float)img[y1 * w + x0];
    float v11 = (float)img[y1 * w + x1];

    float top = v00 * (1.0f - fx) + v10 * fx;
    float bot = v01 * (1.0f - fx) + v11 * fx;
    return top * (1.0f - fy) + bot * fy;
}

void DISOpticalFlow::buildPyramid(
    const uint8_t* I0, const uint8_t* I1,
    int w, int h, int maxLevels,
    std::vector<Level>& pyr)
{
    pyr.clear();
    Level base;
    base.w = w;
    base.h = h;
    base.I0.assign(I0, I0 + (size_t)w * h);
    base.I1.assign(I1, I1 + (size_t)w * h);
    base.u.assign((size_t)w * h, 0.0f);
    base.v.assign((size_t)w * h, 0.0f);
    pyr.push_back(std::move(base));

    int currW = w, currH = h;
    for (int l = 1; l < maxLevels; ++l) {
        int nextW = currW / 2;
        int nextH = currH / 2;
        if (nextW < 16 || nextH < 16) break;

        Level lvl;
        lvl.w = nextW;
        lvl.h = nextH;
        lvl.I0.resize((size_t)nextW * nextH);
        lvl.I1.resize((size_t)nextW * nextH);
        lvl.u.assign((size_t)nextW * nextH, 0.0f);
        lvl.v.assign((size_t)nextW * nextH, 0.0f);

        const uint8_t* prevI0 = pyr.back().I0.data();
        const uint8_t* prevI1 = pyr.back().I1.data();

        // 2x2 box downsampling
        for (int y = 0; y < nextH; ++y) {
            int sy0 = y * 2;
            int sy1 = std::min(sy0 + 1, currH - 1);
            for (int x = 0; x < nextW; ++x) {
                int sx0 = x * 2;
                int sx1 = std::min(sx0 + 1, currW - 1);

                uint32_t sum0 = prevI0[sy0 * currW + sx0] + prevI0[sy0 * currW + sx1] +
                                prevI0[sy1 * currW + sx0] + prevI0[sy1 * currW + sx1];
                uint32_t sum1 = prevI1[sy0 * currW + sx0] + prevI1[sy0 * currW + sx1] +
                                prevI1[sy1 * currW + sx0] + prevI1[sy1 * currW + sx1];

                lvl.I0[y * nextW + x] = (uint8_t)(sum0 / 4);
                lvl.I1[y * nextW + x] = (uint8_t)(sum1 / 4);
            }
        }

        pyr.push_back(std::move(lvl));
        currW = nextW;
        currH = nextH;
    }
}

void DISOpticalFlow::computeGradients(Level& lvl) {
    int w = lvl.w, h = lvl.h;
    lvl.gradX.resize((size_t)w * h);
    lvl.gradY.resize((size_t)w * h);

    const uint8_t* img = lvl.I0.data();
    float* gx = lvl.gradX.data();
    float* gy = lvl.gradY.data();

    for (int y = 0; y < h; ++y) {
        int y_prev = (y > 0) ? (y - 1) * w : 0;
        int y_curr = y * w;
        int y_next = (y < h - 1) ? (y + 1) * w : (h - 1) * w;

        for (int x = 0; x < w; ++x) {
            int x_prev = (x > 0) ? (x - 1) : 0;
            int x_next = (x < w - 1) ? (x + 1) : (w - 1);

            gx[y_curr + x] = ((float)img[y_curr + x_next] - (float)img[y_curr + x_prev]) * 0.5f;
            gy[y_curr + x] = ((float)img[y_next + x] - (float)img[y_prev + x]) * 0.5f;
        }
    }
}

void DISOpticalFlow::solveLevel(Level& lvl, const DISParams& params) {
    computeGradients(lvl);

    int w = lvl.w, h = lvl.h;
    int P = params.patch_size;
    int S = params.patch_stride;
    int pHalf = P / 2;

    const uint8_t* I0 = lvl.I0.data();
    const uint8_t* I1 = lvl.I1.data();
    const float* gx = lvl.gradX.data();
    const float* gy = lvl.gradY.data();

    std::vector<float>& u = lvl.u;
    std::vector<float>& v = lvl.v;

    // Iterate over patch centers with step S
    for (int cy = pHalf; cy < h - pHalf; cy += S) {
        for (int cx = pHalf; cx < w - pHalf; cx += S) {
            // 1. Precompute inverse Hessian over patch in I0
            float h00 = 0.0f, h01 = 0.0f, h11 = 0.0f;
            for (int py = -pHalf; py < pHalf; ++py) {
                int iy = cy + py;
                for (int px = -pHalf; px < pHalf; ++px) {
                    int ix = cx + px;
                    float dx = gx[iy * w + ix];
                    float dy = gy[iy * w + ix];
                    h00 += dx * dx;
                    h01 += dx * dy;
                    h11 += dy * dy;
                }
            }

            // Regularization
            h00 += 0.001f;
            h11 += 0.001f;
            float det = h00 * h11 - h01 * h01;
            if (std::abs(det) < 1e-6f) continue;
            float invDet = 1.0f / det;
            float invH00 = h11 * invDet;
            float invH01 = -h01 * invDet;
            float invH11 = h00 * invDet;

            // 2. Spatial Propagation: test candidate vectors from neighbors
            float curU = u[cy * w + cx];
            float curV = v[cy * w + cx];

            auto testSSD = [&](float tu, float tv) -> float {
                float ssd = 0.0f;
                for (int py = -pHalf; py < pHalf; ++py) {
                    int iy = cy + py;
                    for (int px = -pHalf; px < pHalf; ++px) {
                        int ix = cx + px;
                        float val0 = (float)I0[iy * w + ix];
                        float val1 = sampleBilinear(I1, w, h, (float)ix + tu, (float)iy + tv);
                        float diff = val1 - val0;
                        ssd += diff * diff;
                    }
                }
                return ssd;
            };

            float bestSSD = testSSD(curU, curV);

            // Candidate from left
            if (cx - S >= pHalf) {
                float lu = u[cy * w + (cx - S)];
                float lv = v[cy * w + (cx - S)];
                float s = testSSD(lu, lv);
                if (s < bestSSD) { bestSSD = s; curU = lu; curV = lv; }
            }
            // Candidate from top
            if (cy - S >= pHalf) {
                float tu = u[(cy - S) * w + cx];
                float tv = v[(cy - S) * w + cx];
                float s = testSSD(tu, tv);
                if (s < bestSSD) { bestSSD = s; curU = tu; curV = tv; }
            }

            // 3. Inverse Compositional Iterations
            for (int it = 0; it < params.iterations; ++it) {
                float bx = 0.0f, by = 0.0f;
                for (int py = -pHalf; py < pHalf; ++py) {
                    int iy = cy + py;
                    for (int px = -pHalf; px < pHalf; ++px) {
                        int ix = cx + px;
                        float val0 = (float)I0[iy * w + ix];
                        float val1 = sampleBilinear(I1, w, h, (float)ix + curU, (float)iy + curV);
                        float diff = val1 - val0;
                        bx += gx[iy * w + ix] * diff;
                        by += gy[iy * w + ix] * diff;
                    }
                }

                float du = -(invH00 * bx + invH01 * by);
                float dv = -(invH01 * bx + invH11 * by);

                curU += du;
                curV += dv;

                if (du * du + dv * dv < 0.0025f) break;
            }

            // 4. Splat to local patch footprint in dense grid
            for (int py = -pHalf; py < pHalf; ++py) {
                int iy = cy + py;
                if (iy < 0 || iy >= h) continue;
                for (int px = -pHalf; px < pHalf; ++px) {
                    int ix = cx + px;
                    if (ix < 0 || ix >= w) continue;
                    u[iy * w + ix] = curU;
                    v[iy * w + ix] = curV;
                }
            }
        }
    }
}

void DISOpticalFlow::upsampleFlow(
    const std::vector<float>& srcU, const std::vector<float>& srcV,
    int srcW, int srcH,
    std::vector<float>& dstU, std::vector<float>& dstV,
    int dstW, int dstH,
    float scale)
{
    dstU.resize((size_t)dstW * dstH);
    dstV.resize((size_t)dstW * dstH);

    float scaleX = (float)srcW / (float)dstW;
    float scaleY = (float)srcH / (float)dstH;

    for (int dy = 0; dy < dstH; ++dy) {
        float sy = dy * scaleY;
        int y0 = (int)sy;
        int y1 = std::min(y0 + 1, srcH - 1);
        float fy = sy - y0;

        for (int dx = 0; dx < dstW; ++dx) {
            float sx = dx * scaleX;
            int x0 = (int)sx;
            int x1 = std::min(x0 + 1, srcW - 1);
            float fx = sx - x0;

            float u00 = srcU[y0 * srcW + x0]; float u10 = srcU[y0 * srcW + x1];
            float u01 = srcU[y1 * srcW + x0]; float u11 = srcU[y1 * srcW + x1];
            float v00 = srcV[y0 * srcW + x0]; float v10 = srcV[y0 * srcW + x1];
            float v01 = srcV[y1 * srcW + x0]; float v11 = srcV[y1 * srcW + x1];

            float ut = u00 * (1.0f - fx) + u10 * fx;
            float ub = u01 * (1.0f - fx) + u11 * fx;
            float vt = v00 * (1.0f - fx) + v10 * fx;
            float vb = v01 * (1.0f - fx) + v11 * fx;

            dstU[dy * dstW + dx] = (ut * (1.0f - fy) + ub * fy) * scale;
            dstV[dy * dstW + dx] = (vt * (1.0f - fy) + vb * fy) * scale;
        }
    }
}

void DISOpticalFlow::compute(
    const uint8_t* currentGray,
    const uint8_t* prevGray,
    int flowW, int flowH,
    int inW, int inH,
    const DISParams& params,
    std::vector<float>& flowU,
    std::vector<float>& flowV)
{
    if (!currentGray || !prevGray || flowW <= 0 || flowH <= 0 || inW <= 0 || inH <= 0) return;

    // 1. Build Multi-Scale Pyramids (typically 4 levels)
    std::vector<Level> pyr;
    buildPyramid(currentGray, prevGray, flowW, flowH, 4, pyr);
    int numLevels = (int)pyr.size();
    if (numLevels == 0) return;

    int finest = std::clamp(params.finest_scale, 0, numLevels - 1);

    // 2. Coarse-to-Fine Optical Flow Solving
    for (int l = numLevels - 1; l >= finest; --l) {
        if (l < numLevels - 1) {
            // Upsample flow from (l + 1) to l
            upsampleFlow(pyr[l + 1].u, pyr[l + 1].v, pyr[l + 1].w, pyr[l + 1].h,
                         pyr[l].u, pyr[l].v, pyr[l].w, pyr[l].h, 2.0f);
        }
        solveLevel(pyr[l], params);
    }

    // 3. Upsample to base flow resolution (Level 0) if finest > 0
    std::vector<float> baseU, baseV;
    if (finest > 0) {
        float factor = (float)(1 << finest);
        upsampleFlow(pyr[finest].u, pyr[finest].v, pyr[finest].w, pyr[finest].h,
                     baseU, baseV, flowW, flowH, factor);
    } else {
        baseU = std::move(pyr[0].u);
        baseV = std::move(pyr[0].v);
    }

    // 4. Final Upsample from flow resolution (flowW, flowH) to input frame resolution (inW, inH)
    float scaleX = (float)inW / (float)flowW;
    float scaleY = (float)inH / (float)flowH;

    flowU.resize((size_t)inW * inH);
    flowV.resize((size_t)inW * inH);

    for (int y = 0; y < inH; ++y) {
        float sy = (float)y * ((float)flowH / (float)inH);
        int y0 = (int)sy;
        int y1 = std::min(y0 + 1, flowH - 1);
        float fy = sy - y0;

        for (int x = 0; x < inW; ++x) {
            float sx = (float)x * ((float)flowW / (float)inW);
            int x0 = (int)sx;
            int x1 = std::min(x0 + 1, flowW - 1);
            float fx = sx - x0;

            float u00 = baseU[y0 * flowW + x0]; float u10 = baseU[y0 * flowW + x1];
            float u01 = baseU[y1 * flowW + x0]; float u11 = baseU[y1 * flowW + x1];
            float v00 = baseV[y0 * flowW + x0]; float v10 = baseV[y0 * flowW + x1];
            float v01 = baseV[y1 * flowW + x0]; float v11 = baseV[y1 * flowW + x1];

            float ut = u00 * (1.0f - fx) + u10 * fx;
            float ub = u01 * (1.0f - fx) + u11 * fx;
            float vt = v00 * (1.0f - fx) + v10 * fx;
            float vb = v01 * (1.0f - fx) + v11 * fx;

            flowU[(size_t)y * inW + x] = (ut * (1.0f - fy) + ub * fy) * scaleX;
            flowV[(size_t)y * inW + x] = (vt * (1.0f - fy) + vb * fy) * scaleY;
        }
    }
}
