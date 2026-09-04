#include "WorkerBridge.h"
#include <iostream>

namespace {
class ScopedExclusiveLock {
public:
    explicit ScopedExclusiveLock(SRWLOCK& lock) : m_lock(&lock), m_acquired(true) {
        AcquireSRWLockExclusive(m_lock);
    }
    ~ScopedExclusiveLock() {
        if (m_acquired && m_lock) {
            ReleaseSRWLockExclusive(m_lock);
        }
    }
    void unlock() {
        if (m_acquired && m_lock) {
            ReleaseSRWLockExclusive(m_lock);
            m_acquired = false;
        }
    }
    void relock() {
        if (!m_acquired && m_lock) {
            AcquireSRWLockExclusive(m_lock);
            m_acquired = true;
        }
    }
private:
    SRWLOCK* m_lock;
    bool m_acquired;
};
}

WorkerBridge::WorkerBridge() {}

WorkerBridge::~WorkerBridge() {
    stop();
}

bool WorkerBridge::isRunning() {
    AcquireSRWLockShared(&m_lock);
    bool r = is_running;
    ReleaseSRWLockShared(&m_lock);
    return r;
}

bool WorkerBridge::start(const std::string& nvngx_path, const VideoHeader& header, SetupResponse& out_setup) {
    ScopedExclusiveLock lock(m_lock);
    if (is_running) return false;

    if (nvngx_path.empty()) return false;

    // Check if file exists using Win32 API to avoid MSVC std::filesystem ABI mismatch
    DWORD attr = GetFileAttributesA(nvngx_path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        return false;
    }

    SECURITY_ATTRIBUTES saAttr;
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;

    // Create pipes for STDOUT and STDIN
    if (!CreatePipe(&hChildStd_OUT_Rd, &hChildStd_OUT_Wr, &saAttr, 0)) return false;
    SetHandleInformation(hChildStd_OUT_Rd, HANDLE_FLAG_INHERIT, 0);

    if (!CreatePipe(&hChildStd_IN_Rd, &hChildStd_IN_Wr, &saAttr, 0)) {
        if (hChildStd_OUT_Rd) { CloseHandle(hChildStd_OUT_Rd); hChildStd_OUT_Rd = NULL; }
        if (hChildStd_OUT_Wr) { CloseHandle(hChildStd_OUT_Wr); hChildStd_OUT_Wr = NULL; }
        return false;
    }
    SetHandleInformation(hChildStd_IN_Wr, HANDLE_FLAG_INHERIT, 0);

    // Get directory of nvngx.dll to use as working directory
    std::string workingDir = "";
    size_t lastSlash = nvngx_path.find_last_of("/\\");
    if (lastSlash != std::string::npos) {
        workingDir = nvngx_path.substr(0, lastSlash);
    }

    // Redirect stderr to NUL to prevent log text from corrupting binary stdout
    HANDLE hNullStderr = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &saAttr, OPEN_EXISTING, 0, NULL);

    // Create process
    PROCESS_INFORMATION piProcInfo;
    STARTUPINFOA siStartInfo;
    ZeroMemory(&piProcInfo, sizeof(PROCESS_INFORMATION));
    ZeroMemory(&siStartInfo, sizeof(STARTUPINFOA));
    siStartInfo.cb = sizeof(STARTUPINFOA);
    siStartInfo.hStdError = (hNullStderr != INVALID_HANDLE_VALUE) ? hNullStderr : hChildStd_OUT_Wr;
    siStartInfo.hStdOutput = hChildStd_OUT_Wr;
    siStartInfo.hStdInput = hChildStd_IN_Rd;
    siStartInfo.dwFlags |= STARTF_USESTDHANDLES;

    // Create process with a mutable command line buffer (CreateProcessA modifies lpCommandLine!)
    std::string cmdArgs = "\"" + nvngx_path + "\" --video";
    std::vector<char> cmdArgsBuf(cmdArgs.begin(), cmdArgs.end());
    cmdArgsBuf.push_back('\0');
    
    BOOL bSuccess = CreateProcessA(
        NULL, 
        cmdArgsBuf.data(), 
        NULL, NULL, TRUE, 
        CREATE_NO_WINDOW, 
        NULL, 
        workingDir.empty() ? NULL : workingDir.c_str(),
        &siStartInfo, 
        &piProcInfo
    );

    if (hNullStderr != INVALID_HANDLE_VALUE) {
        CloseHandle(hNullStderr);
    }

    // Always close child ends in parent
    if (hChildStd_OUT_Wr) { CloseHandle(hChildStd_OUT_Wr); hChildStd_OUT_Wr = NULL; }
    if (hChildStd_IN_Rd)  { CloseHandle(hChildStd_IN_Rd);  hChildStd_IN_Rd = NULL; }

    if (!bSuccess) {
        if (hChildStd_OUT_Rd) { CloseHandle(hChildStd_OUT_Rd); hChildStd_OUT_Rd = NULL; }
        if (hChildStd_IN_Wr)  { CloseHandle(hChildStd_IN_Wr);  hChildStd_IN_Wr = NULL; }
        return false;
    }

    hProcess = piProcInfo.hProcess;
    CloseHandle(piProcInfo.hThread);

    // Write VideoHeader
    m_header = header;
    if (!writeExact(&header, sizeof(VideoHeader))) {
        if (hProcess) { TerminateProcess(hProcess, 0); CloseHandle(hProcess); hProcess = NULL; }
        if (hChildStd_IN_Wr)  { CloseHandle(hChildStd_IN_Wr);  hChildStd_IN_Wr = NULL; }
        if (hChildStd_OUT_Rd) { CloseHandle(hChildStd_OUT_Rd); hChildStd_OUT_Rd = NULL; }
        return false;
    }

    // Read SetupResponse
    if (!readExact(&out_setup, sizeof(SetupResponse))) {
        if (hProcess) { TerminateProcess(hProcess, 0); CloseHandle(hProcess); hProcess = NULL; }
        if (hChildStd_IN_Wr)  { CloseHandle(hChildStd_IN_Wr);  hChildStd_IN_Wr = NULL; }
        if (hChildStd_OUT_Rd) { CloseHandle(hChildStd_OUT_Rd); hChildStd_OUT_Rd = NULL; }
        return false;
    }

    if (out_setup.magic != 0x34505553 || out_setup.setup_ok != 1) {
        if (hProcess) { TerminateProcess(hProcess, 0); CloseHandle(hProcess); hProcess = NULL; }
        if (hChildStd_IN_Wr)  { CloseHandle(hChildStd_IN_Wr);  hChildStd_IN_Wr = NULL; }
        if (hChildStd_OUT_Rd) { CloseHandle(hChildStd_OUT_Rd); hChildStd_OUT_Rd = NULL; }
        return false;
    }

    is_running = true;
    return true;
}

void WorkerBridge::stop() {
    ScopedExclusiveLock lock(m_lock);
    if (hProcess) {
        TerminateProcess(hProcess, 0);
        CloseHandle(hProcess);
        hProcess = NULL;
    }
    if (hChildStd_IN_Wr) { CloseHandle(hChildStd_IN_Wr); hChildStd_IN_Wr = NULL; }
    if (hChildStd_IN_Rd) { CloseHandle(hChildStd_IN_Rd); hChildStd_IN_Rd = NULL; }
    if (hChildStd_OUT_Wr) { CloseHandle(hChildStd_OUT_Wr); hChildStd_OUT_Wr = NULL; }
    if (hChildStd_OUT_Rd) { CloseHandle(hChildStd_OUT_Rd); hChildStd_OUT_Rd = NULL; }
    is_running = false;
}

bool WorkerBridge::processFrame(
    uint32_t index, bool reset, int64_t pts,
    const uint8_t* rgba_data, size_t rgba_size,
    const uint8_t* motion_data, size_t motion_size,
    const uint8_t* depth_data, size_t depth_size,
    const uint8_t* control_mask_data, size_t control_mask_size,
    std::vector<uint8_t>& out_rgba)
{
    ScopedExclusiveLock lock(m_lock);
    if (!is_running) return false;

    FrameHeader header;
    header.index = index;
    header.reset = reset ? 1 : 0;
    header.pts = pts;

    const size_t guide_size = (size_t)m_header.input_width * m_header.input_height * sizeof(float);
    if (depth_data && depth_size == guide_size) {
        header.guide_flags |= GUIDE_DEPTH;
    }
    if (control_mask_data && control_mask_size == guide_size) {
        header.guide_flags |= GUIDE_CONTROL_MASK;
    }

    if (!writeExact(&header, sizeof(FrameHeader))) {
        if (hProcess) { TerminateProcess(hProcess, 0); CloseHandle(hProcess); hProcess = NULL; }
        if (hChildStd_IN_Wr)  { CloseHandle(hChildStd_IN_Wr);  hChildStd_IN_Wr = NULL; }
        if (hChildStd_OUT_Rd) { CloseHandle(hChildStd_OUT_Rd); hChildStd_OUT_Rd = NULL; }
        is_running = false;
        return false;
    }
    if (!writeExact(rgba_data, rgba_size)) {
        if (hProcess) { TerminateProcess(hProcess, 0); CloseHandle(hProcess); hProcess = NULL; }
        if (hChildStd_IN_Wr)  { CloseHandle(hChildStd_IN_Wr);  hChildStd_IN_Wr = NULL; }
        if (hChildStd_OUT_Rd) { CloseHandle(hChildStd_OUT_Rd); hChildStd_OUT_Rd = NULL; }
        is_running = false;
        return false;
    }
    
    // Write motion vector buffer only if mv_mode == 1 (EXTERNAL_BUFFER)
    if (m_header.mv_mode == 1) {
        if (motion_size > 0 && motion_data != nullptr) {
            if (!writeExact(motion_data, motion_size)) {
                if (hProcess) { TerminateProcess(hProcess, 0); CloseHandle(hProcess); hProcess = NULL; }
                if (hChildStd_IN_Wr)  { CloseHandle(hChildStd_IN_Wr);  hChildStd_IN_Wr = NULL; }
                if (hChildStd_OUT_Rd) { CloseHandle(hChildStd_OUT_Rd); hChildStd_OUT_Rd = NULL; }
                is_running = false;
                return false;
            }
        } else {
            const size_t motion_bytes = (size_t)m_header.input_width * m_header.input_height * 2 * sizeof(uint16_t);
            std::vector<uint8_t> zeroMotion(motion_bytes, 0);
            if (!writeExact(zeroMotion.data(), zeroMotion.size())) {
                if (hProcess) { TerminateProcess(hProcess, 0); CloseHandle(hProcess); hProcess = NULL; }
                if (hChildStd_IN_Wr)  { CloseHandle(hChildStd_IN_Wr);  hChildStd_IN_Wr = NULL; }
                if (hChildStd_OUT_Rd) { CloseHandle(hChildStd_OUT_Rd); hChildStd_OUT_Rd = NULL; }
                is_running = false;
                return false;
            }
        }
    }

    if ((header.guide_flags & GUIDE_DEPTH) && !writeExact(depth_data, depth_size)) {
        if (hProcess) { TerminateProcess(hProcess, 0); CloseHandle(hProcess); hProcess = NULL; }
        if (hChildStd_IN_Wr)  { CloseHandle(hChildStd_IN_Wr);  hChildStd_IN_Wr = NULL; }
        if (hChildStd_OUT_Rd) { CloseHandle(hChildStd_OUT_Rd); hChildStd_OUT_Rd = NULL; }
        is_running = false;
        return false;
    }
    if ((header.guide_flags & GUIDE_CONTROL_MASK) && !writeExact(control_mask_data, control_mask_size)) {
        if (hProcess) { TerminateProcess(hProcess, 0); CloseHandle(hProcess); hProcess = NULL; }
        if (hChildStd_IN_Wr)  { CloseHandle(hChildStd_IN_Wr);  hChildStd_IN_Wr = NULL; }
        if (hChildStd_OUT_Rd) { CloseHandle(hChildStd_OUT_Rd); hChildStd_OUT_Rd = NULL; }
        is_running = false;
        return false;
    }

    FrameResponse resp;
    if (!readExact(&resp, sizeof(FrameResponse))) {
        if (hProcess) { TerminateProcess(hProcess, 0); CloseHandle(hProcess); hProcess = NULL; }
        if (hChildStd_IN_Wr)  { CloseHandle(hChildStd_IN_Wr);  hChildStd_IN_Wr = NULL; }
        if (hChildStd_OUT_Rd) { CloseHandle(hChildStd_OUT_Rd); hChildStd_OUT_Rd = NULL; }
        is_running = false;
        return false;
    }

    if (resp.magic != 0x3154554F || resp.ok != 1 || resp.out_index != index) {
        return false;
    }

    out_rgba.resize(resp.byte_count);
    if (!readExact(out_rgba.data(), resp.byte_count)) {
        if (hProcess) { TerminateProcess(hProcess, 0); CloseHandle(hProcess); hProcess = NULL; }
        if (hChildStd_IN_Wr)  { CloseHandle(hChildStd_IN_Wr);  hChildStd_IN_Wr = NULL; }
        if (hChildStd_OUT_Rd) { CloseHandle(hChildStd_OUT_Rd); hChildStd_OUT_Rd = NULL; }
        is_running = false;
        return false;
    }

    return true;
}

bool WorkerBridge::readExact(void* buffer, size_t size) {
    if (!hChildStd_OUT_Rd) return false;
    DWORD dwRead;
    size_t totalRead = 0;
    char* ptr = (char*)buffer;
    while (totalRead < size) {
        if (!ReadFile(hChildStd_OUT_Rd, ptr + totalRead, (DWORD)(size - totalRead), &dwRead, NULL) || dwRead == 0) {
            return false;
        }
        totalRead += dwRead;
    }
    return true;
}

bool WorkerBridge::writeExact(const void* buffer, size_t size) {
    if (!hChildStd_IN_Wr) return false;
    DWORD dwWritten;
    size_t totalWritten = 0;
    const char* ptr = (const char*)buffer;
    while (totalWritten < size) {
        if (!WriteFile(hChildStd_IN_Wr, ptr + totalWritten, (DWORD)(size - totalWritten), &dwWritten, NULL) || dwWritten == 0) {
            return false;
        }
        totalWritten += dwWritten;
    }
    return true;
}
