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
    bool captureOverlays = false;  // include 3rd-party overlays (Discord/Steam) in the capture?
    uint32_t frames = 0;
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
    Capture(const std::string& windowName, bool captureOverlays = false);

    void attach();
    void shutdown();

    // Toggle overlay capture. Set pre-attach for the initial mode; if attached,
    // pushes to the live hook info and re-signals the hook to re-read it.
    void setCaptureOverlays(bool enabled);
    bool captureOverlays() const { return config.captureOverlays; }
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
    bool waitHookReady();
};

} // namespace obsc