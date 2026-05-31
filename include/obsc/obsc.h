/**
 * @file obsc.h
 * @author Kyle Pelham (bonezone2001@gmail.com)
 * @brief The main header file for the obsc dll wrapper.
 * 
 * @copyright Copyright (c) 2024
*/

#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <string>
#include <memory>
#include <stdexcept>
#include <vector>
#include <tuple>

#include "obsc/mutex.h"
#include "obsc/context.h"
#include "obsc/obsc_export.hpp"

namespace obsc {

struct OBSC_EXPORT CaptureConfig {
    std::string windowName;
    bool captureOverlays;
    uint32_t frames;
};

struct OBSC_EXPORT StripFrame {
    std::vector<uint8_t> bgra;
    int width  = 0;
    int height = 0;
};

class OBSC_EXPORT Capture {
private:
    CaptureConfig config;
    Context context;

public:
    Capture(const std::string& windowName);

    void attach();
    void shutdown();
    std::tuple<std::vector<uint8_t>, std::pair<size_t, size_t>> captureFrame();

    // Sub-rect capture; cheaper memcpy than captureFrame when only a region is needed.
    StripFrame captureStrip(int x, int y, int w, int h);
    bool captureStripInto(uint8_t* out, int x, int y, int w, int h);

    // Single pixel probe. Returns {B, G, R, A} or empty on failure.
    std::vector<uint8_t> getPixel(int x, int y);

private:
    void initKeepalive();
    void initPipe();
    void initTextureMutexes();
    void initEvents();
    void initHookInfo();
    std::tuple<DXGI_MAPPED_RECT, std::pair<size_t, size_t>> mapResource();
    bool attemptExistingHook();
};

} // namespace obsc