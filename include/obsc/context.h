/**
 * @file context.h
 * @author Kyle Pelham (bonezone2001@gmail.com)
 * @brief The context structure.
 * 
 * @copyright Copyright (c) 2024
*/

#pragma once
#include <windows.h>
#include <dxgi.h>
#include <string>
#include <memory>
#include <stdexcept>
#include <wrl/client.h>

#pragma comment(lib,"d3d11.lib")
#include <d3d11.h>

#include "obsc/obsc_export.hpp"
#include "obsc/mutex.h"
#include "obsc/event.h"
#include "obsc/pipe.h"

using Microsoft::WRL::ComPtr;

namespace obsc {

struct HookInfo;
struct ShtexData;

struct OBSC_EXPORT Context {
    HWND hwnd = nullptr;
    uint32_t pid = 0;
    DWORD threadId = 0;
    uint32_t textureHandle = 0;
    uint32_t hookWindow = 0;
    uint32_t textureMapId = 0;
    bool is32Bit = false;

    HANDLE hookInfoHandle = nullptr;
    HookInfo* hookInfoView = nullptr;
    HANDLE textureDataHandle = nullptr;
    ShtexData* textureDataView = nullptr;
    
    std::unique_ptr<Mutex> keepaliveMutex;
    std::unique_ptr<Mutex> textureMutex1;
    std::unique_ptr<Mutex> textureMutex2;
    std::unique_ptr<NamedPipe> pipe;

    std::unique_ptr<Event> hookRestart;
    std::unique_ptr<Event> hookStop;
    std::unique_ptr<Event> hookInit;
    std::unique_ptr<Event> hookReady;
    std::unique_ptr<Event> hookExit;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> deviceContext;
    ComPtr<ID3D11Resource> resource;
    ComPtr<ID3D11Texture2D> sourceTexture;
    ComPtr<ID3D11Texture2D> readbackTexture;
    ComPtr<IDXGISurface1> readbackSurface;
    D3D11_TEXTURE2D_DESC readbackTextureDesc{};
    ComPtr<ID3D11Texture2D> stripTexture;
    ComPtr<IDXGISurface1> stripSurface;
    D3D11_TEXTURE2D_DESC stripTextureDesc{};

    ComPtr<IDXGISurface> surface;
};

} // namespace obsc