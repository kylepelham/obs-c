#include "obsc/debug.h"
#include "obsc/obsc.h"
#include "obsc/hook_info.h"
#include "obsc/constants.h"
#include "obsc/file_mapping.h"
#include "obsc/utils.h"

#include <d3d11.h>

namespace obsc {

namespace {

void unmapSurface(Context& context)
{
    if (!context.surface) return;
    context.surface->Unmap();
    context.surface.Reset();
}

D3D11_TEXTURE2D_DESC stagingDescFor(const D3D11_TEXTURE2D_DESC& source, UINT width, UINT height)
{
    D3D11_TEXTURE2D_DESC desc = source;
    desc.Width = width;
    desc.Height = height;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    return desc;
}

bool sameStagingDesc(const D3D11_TEXTURE2D_DESC& a, const D3D11_TEXTURE2D_DESC& b)
{
    return a.Width == b.Width &&
           a.Height == b.Height &&
           a.MipLevels == b.MipLevels &&
           a.ArraySize == b.ArraySize &&
           a.Format == b.Format &&
           a.SampleDesc.Count == b.SampleDesc.Count &&
           a.SampleDesc.Quality == b.SampleDesc.Quality;
}

}

Capture::Capture(const std::string& windowName, bool captureOverlays)
{
    config.windowName = windowName;
    config.captureOverlays = captureOverlays;
}

void Capture::setCaptureOverlays(bool enabled)
{
    config.captureOverlays = enabled;

    // Push to live hook info if attached + re-signal init to re-read it.
    // Pre-attach (pid 0) the open throws; value applies at next attach().
    try {
        auto hookInfo = FileMapping<HookInfo>::open(fmt::format("{}{}", SHMEM_HOOK_INFO, context.pid));
        hookInfo->capture_overlay = config.captureOverlays;
        if (context.hookInit) context.hookInit->signal();
    } catch (...) {}
}

void Capture::attach()
{
    // Get window handle
    auto hwnd = FindWindowA(nullptr, config.windowName.c_str());
    if (!hwnd) throw std::runtime_error(fmt::format("Window not found: {}", config.windowName));

    // Get window thread and process id
    DWORD threadId = GetWindowThreadProcessId(hwnd, (DWORD*)&context.pid);
    if (!threadId) throw std::runtime_error(fmt::format("Failed to get the thread id. Window: {}", config.windowName));

    context.hwnd = hwnd;
    context.threadId = threadId;
    context.is32Bit = is32BitProcess(context.pid);

    PRINTLN("Window: {}, PID: {}, TID: {}, Bitness: {}", config.windowName, context.pid, threadId, context.is32Bit ? "32" : "64");

    // Starup
    initKeepalive();
    initPipe();

    // Reuse a resident hook (ping Restart); inject only if none exists
    if (!attemptExistingHook()) {
        PRINTLN("Inject new hook. Thread ID: {}", threadId);
        injectGraphicsHook(context.threadId, true, context.is32Bit);
    }
    else
        PRINTLN("Reusing existing hook. Thread ID: {}", threadId);

    // Initialize the hook info and events
    initTextureMutexes();
    initHookInfo();
    initEvents();

    // Drive the init/ready handshake
    if (!waitHookReady())
        throw std::runtime_error("Timed out waiting for the graphics hook to become ready");

    // // Extract data from the shared memory
    auto hookInfo = FileMapping<HookInfo>::open(fmt::format("{}{}", SHMEM_HOOK_INFO, context.pid));

    auto textureData = FileMapping<ShtexData>::open(fmt::format("{}_{}_{}", SHMEM_TEXTURE, hookInfo->window, hookInfo->map_id));
    context.textureHandle = textureData->tex_handle;

    if (hookInfo->type == CaptureType::Texture) {
        // Initialize d3d11 variables
        auto [device, deviceContext] = createDevice();
        auto resource = openResource(device, context.textureHandle);
        Microsoft::WRL::ComPtr<ID3D11Texture2D> sourceTexture;
        HRESULT hr = resource.As(&sourceTexture);
        if (FAILED(hr))
            throw std::runtime_error(fmt::format("Failed to query source texture 0x{:x}", hr));

        context.device = device;
        context.deviceContext = deviceContext;
        context.resource = resource;
        context.sourceTexture = sourceTexture;
    }

    PRINTLN("Hook ready. Texture handle: {}. Capture Mode: {}", context.textureHandle, hookInfo->type == CaptureType::Memory ? "Memory" : "Texture");
}

void Capture::shutdown()
{
    // Signal Stop (hook idles) and drop handles; releasing keepalive tells the
    // hook we're gone. The hook never signals Exit, so don't wait on it.
    if (context.hookStop && !context.hookStop->signal()) PRINTLN("Failed to signal the stop event: {}", GetLastError());

    context.hookRestart.reset();
    context.hookStop.reset();
    context.hookInit.reset();
    context.hookReady.reset();
    context.hookExit.reset();

    context.keepaliveMutex.reset();
    context.pipe.reset();

    context.textureMutex1.reset();
    context.textureMutex2.reset();

    unmapSurface(context);

    context.device.Reset();
    context.deviceContext.Reset();
    context.resource.Reset();
    context.sourceTexture.Reset();
    context.readbackSurface.Reset();
    context.readbackTexture.Reset();
    context.readbackTextureDesc = {};
    context.stripSurface.Reset();
    context.stripTexture.Reset();
    context.stripTextureDesc = {};

    PRINTLN("Capture shutdown complete.");
}

std::tuple<std::vector<uint8_t>, std::pair<size_t, size_t>> Capture::captureFrame()
{
    // Hook re-initialized capture (stale handle) -- re-attach cleanly
    if (context.hookRestart->signalled()) {
        PRINTLN("The restart event has been signalled. Restarting the capture.");
        shutdown();
        attach();
    }

    // Get the frame data
    auto [mappedSurface, dimensions] = mapResource();

    // Copy the frame data
    size_t height = dimensions.second;
    size_t stride = (size_t)mappedSurface.Pitch / 4;
    size_t byteSize = stride * height * 4;

    std::vector<uint8_t> frameData(byteSize);
    std::memcpy(frameData.data(), mappedSurface.pBits, byteSize);
    unmapSurface(context);

    return {std::move(frameData), dimensions};
}

bool Capture::captureStripInto(uint8_t* out, int x, int y, int w, int h)
{
    if (!out || w <= 0 || h <= 0 || x < 0 || y < 0) return false;

    if (context.hookRestart && context.hookRestart->signalled()) {
        PRINTLN("The restart event has been signalled. Restarting the capture.");
        shutdown();
        attach();
    }

    try {
        unmapSurface(context);
        if (!context.sourceTexture) return false;

        D3D11_TEXTURE2D_DESC textureDesc{};
        context.sourceTexture->GetDesc(&textureDesc);

        const UINT left   = static_cast<UINT>(x);
        const UINT top    = static_cast<UINT>(y);
        const UINT width  = static_cast<UINT>(w);
        const UINT height = static_cast<UINT>(h);
        if (left > textureDesc.Width || top > textureDesc.Height ||
            width > textureDesc.Width - left ||
            height > textureDesc.Height - top) return false;

        D3D11_TEXTURE2D_DESC stripDesc = stagingDescFor(textureDesc, width, height);

        if (!context.stripTexture || !sameStagingDesc(context.stripTextureDesc, stripDesc)) {
            context.stripSurface.Reset();
            context.stripTexture.Reset();
            HRESULT hr = context.device->CreateTexture2D(&stripDesc, nullptr, &context.stripTexture);
            if (FAILED(hr))
                throw std::runtime_error(fmt::format("Failed to create strip texture 0x{:x}", hr));
            hr = context.stripTexture.As(&context.stripSurface);
            if (FAILED(hr))
                throw std::runtime_error(fmt::format("Failed to query strip surface 0x{:x}", hr));
            context.stripTexture->SetEvictionPriority(DXGI_RESOURCE_PRIORITY_MAXIMUM);
            context.stripTextureDesc = stripDesc;
        }

        const D3D11_BOX box{ left, top, 0, left + width, top + height, 1 };
        context.deviceContext->CopySubresourceRegion(context.stripTexture.Get(), 0, 0, 0, 0,
                                                     context.sourceTexture.Get(), 0, &box);

        DXGI_MAPPED_RECT mapped{};
        HRESULT hr = context.stripSurface->Map(&mapped, DXGI_MAP_READ);
        if (FAILED(hr))
            throw std::runtime_error(fmt::format("Failed to map strip surface 0x{:x}", hr));

        const auto* src      = static_cast<const uint8_t*>(mapped.pBits);
        const size_t pitch   = static_cast<size_t>(mapped.Pitch);
        const size_t rowSize = static_cast<size_t>(w) * 4;

        for (int row = 0; row < h; ++row)
            std::memcpy(out + row * rowSize, src + row * pitch, rowSize);

        context.stripSurface->Unmap();
    } catch (const std::exception& e) {
        PRINTLN("captureStripInto: {}", e.what());
        return false;
    }

    return true;
}

StripFrame Capture::captureStrip(int x, int y, int w, int h)
{
    StripFrame out{ {}, w, h };
    out.bgra.resize(static_cast<size_t>(w) * h * 4);
    if (!captureStripInto(out.bgra.data(), x, y, w, h))
        throw std::runtime_error("captureStrip failed");
    return out;
}

std::vector<uint8_t> Capture::getPixel(int x, int y)
{
    std::vector<uint8_t> out(4);
    if (!captureStripInto(out.data(), x, y, 1, 1)) return {};
    return out;
}

void Capture::initKeepalive()
{
    if (context.keepaliveMutex)
        throw std::runtime_error("Keepalive mutex already exists");

    char name[64];
    sprintf_s(name, "%s%lu", WINDOW_HOOK_KEEPALIVE, context.pid);

    context.keepaliveMutex = Mutex::create(name);
    if (!context.keepaliveMutex)
        throw std::runtime_error(fmt::format("Failed to create the keepalive mutex: ({})", GetLastError()));
}

void Capture::initPipe()
{
    if (context.pipe)
        throw std::runtime_error("Pipe already exists");

    char name[64];
    sprintf_s(name, "%s%lu", PIPE_NAME, context.pid);

    context.pipe = NamedPipe::create(name);
    if (!context.pipe)
        throw std::runtime_error(fmt::format("Failed to create named pipe: ({})", GetLastError()));
}

void Capture::initTextureMutexes()
{
    if (context.textureMutex1 || context.textureMutex2)
        throw std::runtime_error("Texture mutexes already exist");

    context.textureMutex1 = Mutex::create(MUTEX_TEXTURE1);
    context.textureMutex2 = Mutex::create(MUTEX_TEXTURE2);

    if (!context.textureMutex1 || !context.textureMutex2)
        throw std::runtime_error(fmt::format("Failed to create the texture mutexes: ({})", GetLastError()));
}

void Capture::initEvents()
{
    // Create the events
    context.hookRestart = std::make_unique<Event>();
    context.hookStop = std::make_unique<Event>();
    context.hookInit = std::make_unique<Event>();
    context.hookReady = std::make_unique<Event>();
    context.hookExit = std::make_unique<Event>();

    // Open the events
    char name[64];
    sprintf_s(name, "%s%lu", EVENT_CAPTURE_RESTART, context.pid);
    context.hookRestart->openInline(name);

    sprintf_s(name, "%s%lu", EVENT_CAPTURE_STOP, context.pid);
    context.hookStop->openInline(name);

    sprintf_s(name, "%s%lu", EVENT_HOOK_INIT, context.pid);
    context.hookInit->openInline(name);

    sprintf_s(name, "%s%lu", EVENT_HOOK_READY, context.pid);
    context.hookReady->openInline(name);

    sprintf_s(name, "%s%lu", EVENT_HOOK_EXIT, context.pid);
    context.hookExit->openInline(name);
}

void Capture::initHookInfo()
{
    if (context.device || context.deviceContext || context.resource)
        throw std::runtime_error("Hook info already exists");

    PRINTLN("Initializing the hook information");

    char name[64];
    sprintf_s(name, "%s%lu", SHMEM_HOOK_INFO, context.pid);

    auto hookInfo = FileMapping<HookInfo>::open(name);

    hookInfo->offsets = loadGraphicsOffsets(context.pid, context.is32Bit);
    hookInfo->capture_overlay = config.captureOverlays;
    hookInfo->force_shmem = false;
    hookInfo->UNUSED_use_scale = false;
    hookInfo->allow_srgb_alias = true;

    PRINTLN("{}", (*hookInfo).toString());
}

std::tuple<DXGI_MAPPED_RECT, std::pair<size_t, size_t>> Capture::mapResource()
{
    unmapSurface(context);
    if (!context.sourceTexture)
        throw std::runtime_error("No source texture available");

    D3D11_TEXTURE2D_DESC textureDesc{};
    context.sourceTexture->GetDesc(&textureDesc);

    D3D11_TEXTURE2D_DESC readbackDesc = stagingDescFor(textureDesc, textureDesc.Width, textureDesc.Height);

    if (!context.readbackTexture || !sameStagingDesc(context.readbackTextureDesc, readbackDesc)) {
        context.readbackSurface.Reset();
        context.readbackTexture.Reset();
        HRESULT hr = context.device->CreateTexture2D(&readbackDesc, nullptr, &context.readbackTexture);
        if (FAILED(hr))
            throw std::runtime_error(fmt::format("Failed to create the 2d texture 0x{:x}", hr));
        hr = context.readbackTexture.As(&context.readbackSurface);
        if (FAILED(hr))
            throw std::runtime_error(fmt::format("Failed to query readback surface 0x{:x}", hr));
        context.readbackTexture->SetEvictionPriority(DXGI_RESOURCE_PRIORITY_MAXIMUM);
        context.readbackTextureDesc = readbackDesc;
    }

    context.deviceContext->CopyResource(context.readbackTexture.Get(), context.sourceTexture.Get());

    DXGI_MAPPED_RECT mapped_surface;
    HRESULT hr = context.readbackSurface->Map(&mapped_surface, DXGI_MAP_READ);
    if (FAILED(hr))
        throw std::runtime_error(fmt::format("Failed to map the surface 0x{:x}", hr));

    context.surface = context.readbackSurface;

    return {mapped_surface, {textureDesc.Width, textureDesc.Height}};
}

bool Capture::waitHookReady()
{
    // Hook re-inits + fires HookReady only when its present hook sees:
    // !active + Restart signalled + keepalive alive (capture_should_init).
    constexpr DWORD kPollMs = 25;

    auto poll = [&](DWORD durationMs) -> bool {
        for (DWORD t = 0; t < durationMs; t += kPollMs) {
            if (context.hookReady->signalled()) return true;
            Sleep(kPollMs);
        }
        return context.hookReady->signalled();
    };

    auto succeed = [&]() -> bool {
        // drop leftover Restart so captureStrip doesn't spuriously re-attach
        if (context.hookRestart) context.hookRestart->reset();
        return true;
    };

    context.hookInit->signal();  // releases a fresh hook's capture_loop

    // Fast path: an inactive (cleanly stopped) hook re-inits on Restart alone
    if (context.hookRestart) context.hookRestart->signal();
    if (poll(800)) return succeed();

    // Recovery: hook stuck active (prev run never signalled Stop; our keepalive
    // masks its death). Force inactive via Stop, clear it, then Restart.
    for (int phase = 0; phase < 6; ++phase) {
        PRINTLN("HookReady stalled; forcing hook re-init (phase {}).", phase + 1);
        if (context.hookStop) context.hookStop->signal();
        Sleep(150);  // let a present process the stop
        if (context.hookStop) context.hookStop->reset();
        if (context.hookRestart) context.hookRestart->signal();
        context.hookInit->signal();
        if (poll(1200)) return succeed();
    }

    PRINTLN("HookReady not signalled; hook unresponsive.");
    return false;
}

bool Capture::attemptExistingHook()
{
    // Restart event exists => a hook is already resident; ping it to re-init
    try {
        char name[64];
        sprintf_s(name, "%s%lu", EVENT_CAPTURE_RESTART, context.pid);

        auto event = Event::open(name);
        if (!event.signal()) PRINTLN("Failed to signal the restart event: {}", GetLastError());
        return true;
    }
    catch (const std::exception&) {
        PRINTLN("Found no existing hook.");
        return false;
    }
}

} // namespace obsc