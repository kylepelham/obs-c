#include "obsc/debug.h"
#include "obsc/obsc.h"
#include "obsc/hook_info.h"
#include "obsc/constants.h"
#include "obsc/file_mapping.h"
#include "obsc/utils.h"

#include <d3d11.h>

namespace obsc {

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

    // Initialize the hook
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

    // Create and signal the hook init event
    if (!context.hookInit->signal()) throw std::runtime_error(fmt::format("Failed to signal the hook init event ({})", GetLastError()));

    // Wait for the hook to signal the ready event
    if (!context.hookReady->signalled()) context.hookReady->wait();

    // // Extract data from the shared memory
    auto hookInfo = FileMapping<HookInfo>::open(fmt::format("{}{}", SHMEM_HOOK_INFO, context.pid));

    auto textureData = FileMapping<ShtexData>::open(fmt::format("{}_{}_{}", SHMEM_TEXTURE, hookInfo->window, hookInfo->map_id));
    context.textureHandle = textureData->tex_handle;

    if (hookInfo->type == CaptureType::Texture) {
        // Initialize d3d11 variables
        auto [device, deviceContext] = createDevice();
        auto resource = openResource(device, context.textureHandle);

        context.device = device;
        context.deviceContext = deviceContext;
        context.resource = resource;
    }

    PRINTLN("Hook ready. Texture handle: {}. Capture Mode: {}", context.textureHandle, hookInfo->type == CaptureType::Memory ? "Memory" : "Texture");
}

void Capture::shutdown()
{
    // Signal the stop event & wait for the hook to exit
    if (context.hookStop && !context.hookStop->signal()) PRINTLN("Failed to signal the stop event: {}", GetLastError());
    if (context.hookExit) context.hookExit->wait();

    // Cleanup
    context.hookRestart.reset();
    context.hookStop.reset();
    context.hookInit.reset();
    context.hookReady.reset();
    context.hookExit.reset();

    context.keepaliveMutex.reset();
    context.pipe.reset();

    context.textureMutex1.reset();
    context.textureMutex2.reset();

    context.device.Reset();
    context.deviceContext.Reset();
    context.resource.Reset();

    context.surface.Reset();

    PRINTLN("Capture shutdown complete.");
}

std::tuple<std::vector<uint8_t>, std::pair<size_t, size_t>> Capture::captureFrame()
{
    // Restart the capture if the event is signalled
    if (context.hookRestart->signalled()) {
        PRINTLN("The restart event has been signalled. Restarting the capture.");
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

    return {std::move(frameData), dimensions};
}

bool Capture::captureStripInto(uint8_t* out, int x, int y, int w, int h)
{
    if (!out || w <= 0 || h <= 0 || x < 0 || y < 0) return false;

    if (context.hookRestart && context.hookRestart->signalled()) {
        PRINTLN("The restart event has been signalled. Restarting the capture.");
        attach();
    }

    DXGI_MAPPED_RECT mapped{};
    std::pair<size_t, size_t> dim{};
    try {
        auto r = mapResource();
        mapped = std::get<0>(r);
        dim    = std::get<1>(r);
    } catch (const std::exception& e) {
        PRINTLN("captureStripInto: {}", e.what());
        return false;
    }

    if (static_cast<size_t>(x + w) > dim.first ||
        static_cast<size_t>(y + h) > dim.second) return false;

    const auto* src      = static_cast<const uint8_t*>(mapped.pBits);
    const size_t pitch   = static_cast<size_t>(mapped.Pitch);
    const size_t rowSize = static_cast<size_t>(w) * 4;

    for (int row = 0; row < h; ++row)
        std::memcpy(out + row * rowSize, src + (y + row) * pitch + x * 4, rowSize);

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
    // Unmap the surface if it exists
    if (context.surface) {
        context.surface->Unmap();
        context.surface.Reset();
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> frameTexture;
    context.resource.As(&frameTexture);

    D3D11_TEXTURE2D_DESC textureDesc;
    frameTexture->GetDesc(&textureDesc);

    textureDesc.Usage = D3D11_USAGE_STAGING;
    textureDesc.BindFlags = 0;
    textureDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    textureDesc.MiscFlags = 0;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> readableTexture;
    HRESULT hr = context.device->CreateTexture2D(&textureDesc, nullptr, &readableTexture);
    if (FAILED(hr))
        throw std::runtime_error(fmt::format("Failed to create the 2d texture 0x{:x}", hr));

    readableTexture->SetEvictionPriority(DXGI_RESOURCE_PRIORITY_MAXIMUM);
    Microsoft::WRL::ComPtr<ID3D11Resource> readableSurface;
    readableTexture.As(&readableSurface);

    context.deviceContext->CopyResource(readableSurface.Get(), frameTexture.Get());

    Microsoft::WRL::ComPtr<IDXGISurface1> frameSurface;
    readableSurface.As(&frameSurface);

    DXGI_MAPPED_RECT mapped_surface;
    hr = frameSurface->Map(&mapped_surface, DXGI_MAP_READ);
    if (FAILED(hr))
        throw std::runtime_error(fmt::format("Failed to map the surface 0x{:x}", hr));

    context.surface = frameSurface;

    return {mapped_surface, {textureDesc.Width, textureDesc.Height}};
}

bool Capture::attemptExistingHook()
{
    try {
        char name[64];
        sprintf_s(name, "%s%lu", EVENT_CAPTURE_RESTART, context.pid);

        auto event = Event::open(name);
        if (!event.signal()) PRINTLN("Failed to signal the event: {}", GetLastError());
        return true;
    }
    catch(const std::exception&) {
        PRINTLN("Found no existing hook.");
        return false;
    }
}

} // namespace obsc