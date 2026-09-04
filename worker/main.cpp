#include "Protocol.h"
#include "DLSSWorker.h"
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <cstdio>
#include <cstring>
#include <vector>

// ---- Binary pipe I/O ----
static bool readAll(HANDLE h, void* buf, DWORD n) {
    DWORD total = 0;
    while (total < n) {
        DWORD got = 0;
        if (!ReadFile(h, (char*)buf + total, n - total, &got, nullptr) || got == 0)
            return false;
        total += got;
    }
    return true;
}

static bool writeAll(HANDLE h, const void* buf, DWORD n) {
    DWORD total = 0;
    while (total < n) {
        DWORD wrote = 0;
        if (!WriteFile(h, (const char*)buf + total, n - total, &wrote, nullptr) || wrote == 0)
            return false;
        total += wrote;
    }
    return true;
}

static SetupResponse makeFailSetup(uint32_t resultCode = 0) {
    SetupResponse r = {};
    r.magic        = 0x34505553u;
    r.setup_ok     = 0;
    r.setup_result = resultCode;
    return r;
}

int main() {
    // Switch stdin/stdout to binary (no \r\n translation)
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);

    HANDLE hIn  = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);

    // ---- 1. Read VideoHeader ----
    VideoHeader hdr = {};
    if (!readAll(hIn, &hdr, sizeof(hdr))) return 1;
    if (hdr.magic != 0x34563544u && hdr.magic != 0x35563544u) return 1;

    // ---- 2. Initialize DLSS worker ----
    DLSSWorker worker;
    bool ok = worker.init(hdr);

    // ---- 3. Send SetupResponse ----
    SetupResponse setup = ok ? worker.getSetup() : makeFailSetup();
    if (!writeAll(hOut, &setup, sizeof(setup))) return 1;
    if (!ok) return 1;

    // ---- 4. Frame loop ----
    const uint32_t pixelBytes  = hdr.magic == 0x34563544u ? 4u : 8u;
    const uint32_t inBytes     = hdr.input_width  * hdr.input_height  * pixelBytes;
    const uint32_t outBytes    = hdr.output_width * hdr.output_height * pixelBytes;
    const uint32_t motionBytes = hdr.input_width  * hdr.input_height  * 2 * sizeof(uint16_t);

    std::vector<uint8_t> frameIn(inBytes);
    std::vector<uint8_t> motionIn(motionBytes); // RG16F motion buffer
    std::vector<float> depthIn((size_t)hdr.input_width * hdr.input_height);
    std::vector<float> controlMaskIn((size_t)hdr.input_width * hdr.input_height);
    std::vector<uint8_t> frameOut;

    while (true) {
        // Read frame header
        FrameHeader fhdr = {};
        if (!readAll(hIn, &fhdr, sizeof(fhdr))) break;
        if (fhdr.magic != 0x314D5246u) break; // bad magic

        // Read frame pixel data selected by VideoHeader magic.
        if (!readAll(hIn, frameIn.data(), inBytes)) break;

        // Read motion vector data (RG16F) only if mv_mode == 1 (EXTERNAL_BUFFER)
        uint8_t* motionPtr = nullptr;
        if (hdr.mv_mode == 1) {
            if (!readAll(hIn, motionIn.data(), motionBytes)) break;
            motionPtr = motionIn.data();
        }

        const float* depthPtr = nullptr;
        if (fhdr.guide_flags & GUIDE_DEPTH) {
            if (!readAll(hIn, depthIn.data(), depthIn.size() * sizeof(float))) break;
            depthPtr = depthIn.data();
        }

        const float* controlMaskPtr = nullptr;
        if (fhdr.guide_flags & GUIDE_CONTROL_MASK) {
            if (!readAll(hIn, controlMaskIn.data(), controlMaskIn.size() * sizeof(float))) break;
            controlMaskPtr = controlMaskIn.data();
        }

        // Process frame with both RGBA and Motion Vector buffers
        bool frameOk = worker.processFrame(
            fhdr.index, fhdr.reset != 0, fhdr.pts,
            frameIn.data(), motionPtr, depthPtr, controlMaskPtr, frameOut);

        // Send FrameResponse
        FrameResponse fresp = {};
        fresp.magic      = 0x3154554Fu; // 'OUT1'
        fresp.out_index  = fhdr.index;
        fresp.ok         = frameOk ? 1u : 0u;
        fresp.byte_count = frameOk ? outBytes : 0u;
        fresp.out_pts    = fhdr.pts;

        if (!writeAll(hOut, &fresp, sizeof(fresp))) break;

        // Send pixel data (only on success)
        if (frameOk) {
            if (!writeAll(hOut, frameOut.data(), outBytes)) break;
        }
    }

    return 0;
}
