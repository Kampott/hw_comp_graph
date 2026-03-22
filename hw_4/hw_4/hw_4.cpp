#include <windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <assert.h>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <float.h>
#include <algorithm>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")

#define SAFE_RELEASE(p) { if (p) { (p)->Release(); (p) = nullptr; } }

const UINT DefaultWindowWidth = 1280;
const UINT DefaultWindowHeight = 720;

using namespace DirectX;

struct TextureVertex {
    float x, y, z;
    float u, v;
};

struct SkyVertex {
    float x, y, z;
};

struct TextureDesc {
    UINT32 pitch = 0;
    UINT32 mipmapsCount = 1;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    UINT32 width = 0;
    UINT32 height = 0;
    void* pData = nullptr;

    std::vector<uint8_t> storage;
};

struct SceneBuffer {
    XMFLOAT4X4 vp;
    XMFLOAT4 cameraPos;
};

struct GeomBuffer {
    XMFLOAT4X4 model;
    XMFLOAT4 size; // x - size of sky sphere / sky cube
};

#pragma pack(push, 1)
struct DDS_PIXELFORMAT {
    uint32_t size;
    uint32_t flags;
    uint32_t fourCC;
    uint32_t RGBBitCount;
    uint32_t RBitMask;
    uint32_t GBitMask;
    uint32_t BBitMask;
    uint32_t ABitMask;
};

struct DDS_HEADER {
    uint32_t size;
    uint32_t flags;
    uint32_t height;
    uint32_t width;
    uint32_t pitchOrLinearSize;
    uint32_t depth;
    uint32_t mipMapCount;
    uint32_t reserved1[11];
    DDS_PIXELFORMAT ddspf;
    uint32_t caps;
    uint32_t caps2;
    uint32_t caps3;
    uint32_t caps4;
    uint32_t reserved2;
};
#pragma pack(pop)

static constexpr uint32_t DDS_MAGIC = 0x20534444; // "DDS "

static constexpr uint32_t DDSD_MIPMAPCOUNT = 0x20000;
static constexpr uint32_t DDPF_FOURCC = 0x4;
static constexpr uint32_t DDPF_RGB = 0x40;

static bool IsBlockCompressed(DXGI_FORMAT fmt) {
    return fmt == DXGI_FORMAT_BC1_UNORM ||
        fmt == DXGI_FORMAT_BC2_UNORM ||
        fmt == DXGI_FORMAT_BC3_UNORM;
}

static UINT GetBytesPerBlock(DXGI_FORMAT fmt) {
    switch (fmt) {
    case DXGI_FORMAT_BC1_UNORM: return 8;
    case DXGI_FORMAT_BC2_UNORM: return 16;
    case DXGI_FORMAT_BC3_UNORM: return 16;
    default: return 0;
    }
}

static UINT GetBytesPerPixel(DXGI_FORMAT fmt) {
    switch (fmt) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: return 4;
    case DXGI_FORMAT_B8G8R8A8_UNORM: return 4;
    default: return 0;
    }
}

static DXGI_FORMAT DDSFormatFromPixelFormat(const DDS_PIXELFORMAT& pf) {
    if (pf.flags & DDPF_FOURCC) {
        switch (pf.fourCC) {
        case MAKEFOURCC('D', 'X', 'T', '1'): return DXGI_FORMAT_BC1_UNORM;
        case MAKEFOURCC('D', 'X', 'T', '3'): return DXGI_FORMAT_BC2_UNORM;
        case MAKEFOURCC('D', 'X', 'T', '5'): return DXGI_FORMAT_BC3_UNORM;
        default: return DXGI_FORMAT_UNKNOWN;
        }
    }

    if (pf.flags & DDPF_RGB) {
        if (pf.RGBBitCount == 32 &&
            pf.RBitMask == 0x00ff0000 &&
            pf.GBitMask == 0x0000ff00 &&
            pf.BBitMask == 0x000000ff &&
            pf.ABitMask == 0xff000000) {
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        }

        if (pf.RGBBitCount == 32 &&
            pf.RBitMask == 0x000000ff &&
            pf.GBitMask == 0x0000ff00 &&
            pf.BBitMask == 0x00ff0000 &&
            pf.ABitMask == 0xff000000) {
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        }
    }

    return DXGI_FORMAT_UNKNOWN;
}

static bool LoadDDS(const wchar_t* fileName, TextureDesc& textureDesc, bool topMipOnly = true) {
    std::ifstream file(fileName, std::ios::binary);
    if (!file) return false;

    file.seekg(0, std::ios::end);
    std::streamoff fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    if (fileSize < (std::streamoff)(sizeof(uint32_t) + sizeof(DDS_HEADER))) {
        return false;
    }

    std::vector<uint8_t> bytes((size_t)fileSize);
    file.read(reinterpret_cast<char*>(bytes.data()), fileSize);
    if (!file) return false;

    const uint32_t* magic = reinterpret_cast<const uint32_t*>(bytes.data());
    if (*magic != DDS_MAGIC) {
        return false;
    }

    const DDS_HEADER* header = reinterpret_cast<const DDS_HEADER*>(bytes.data() + sizeof(uint32_t));
    if (header->size != 124 || header->ddspf.size != 32) {
        return false;
    }

    DXGI_FORMAT fmt = DDSFormatFromPixelFormat(header->ddspf);
    if (fmt == DXGI_FORMAT_UNKNOWN) {
        return false;
    }

    UINT width = header->width;
    UINT height = header->height;
    UINT mipLevels = (header->flags & DDSD_MIPMAPCOUNT) && header->mipMapCount > 0 ? header->mipMapCount : 1;
    if (topMipOnly) {
        mipLevels = 1;
    }

    UINT pitch = 0;
    UINT topMipSize = 0;

    if (IsBlockCompressed(fmt)) {
        UINT blockBytes = GetBytesPerBlock(fmt);
        UINT blockWidth = (std::max)(1u, (width + 3u) / 4u);
        UINT blockHeight = (std::max)(1u, (height + 3u) / 4u);
        pitch = blockWidth * blockBytes;
        topMipSize = pitch * blockHeight;
    }
    else {
        UINT bpp = GetBytesPerPixel(fmt);
        if (bpp == 0) return false;
        pitch = width * bpp;
        topMipSize = pitch * height;
    }

    const uint8_t* src = bytes.data() + sizeof(uint32_t) + sizeof(DDS_HEADER);
    size_t remaining = bytes.size() - (sizeof(uint32_t) + sizeof(DDS_HEADER));
    if (remaining < topMipSize) {
        return false;
    }

    textureDesc.width = width;
    textureDesc.height = height;
    textureDesc.mipmapsCount = mipLevels;
    textureDesc.fmt = fmt;
    textureDesc.pitch = pitch;
    textureDesc.storage.resize(topMipSize);
    std::memcpy(textureDesc.storage.data(), src, topMipSize);
    textureDesc.pData = textureDesc.storage.data();

    return true;
}

class DXApp {
public:
    DXApp(HINSTANCE hInstance);
    ~DXApp();

    bool Init();
    void Run();

    static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);

private:
    void Render();
    void Resize(UINT width, UINT height);

    HRESULT CompileShaderFromFile(const std::wstring& path, ID3DBlob** ppCode);
    HRESULT SetResourceName(ID3D11DeviceChild* pResource, const std::string& name);
    std::string WCSToMBS(const std::wstring& wstr);

    bool CreateBackBufferRTV();
    bool CreateDepthStencil();
    bool LoadTexture(const std::wstring& TextureName);
    bool LoadCubemap();

    HINSTANCE m_hInstance;
    HWND m_hWnd;

    ID3D11Device* m_pDevice;
    ID3D11DeviceContext* m_pDeviceContext;
    IDXGISwapChain* m_pSwapChain;
    ID3D11RenderTargetView* m_pBackBufferRTV;

    ID3D11Texture2D* m_pDepthStencilTex;
    ID3D11DepthStencilView* m_pDepthStencilView;

    ID3D11Buffer* m_pVertexBuffer;
    ID3D11Buffer* m_pIndexBuffer;
    ID3D11Buffer* m_pSkyVertexBuffer;
    ID3D11Buffer* m_pSkyIndexBuffer;

    ID3D11VertexShader* m_pVertexShader;
    ID3D11PixelShader* m_pPixelShader;
    ID3D11InputLayout* m_pInputLayout;

    ID3D11VertexShader* m_pSkyVS;
    ID3D11PixelShader* m_pSkyPS;
    ID3D11InputLayout* m_pSkyInputLayout;

    ID3D11Buffer* m_pSceneBuffer;
    ID3D11Buffer* m_pGeomBuffer;

    ID3D11Texture2D* m_pTexture;
    ID3D11ShaderResourceView* m_pTextureView;

    ID3D11Texture2D* m_pCubemapTexture;
    ID3D11ShaderResourceView* m_pCubemapView;

    ID3D11SamplerState* m_pSampler;
    ID3D11DepthStencilState* m_pSkyDepthState;
    ID3D11RasterizerState* m_pNoCullRS;

    UINT m_width;
    UINT m_height;

    float m_yaw;
    float m_pitch;
    float m_radius;
    float m_time;

    bool m_keys[256];
    LARGE_INTEGER m_freq;
    LARGE_INTEGER m_prev;
};

DXApp::DXApp(HINSTANCE hInstance) :
    m_hInstance(hInstance),
    m_hWnd(nullptr),
    m_pDevice(nullptr),
    m_pDeviceContext(nullptr),
    m_pSwapChain(nullptr),
    m_pBackBufferRTV(nullptr),
    m_pDepthStencilTex(nullptr),
    m_pDepthStencilView(nullptr),
    m_pVertexBuffer(nullptr),
    m_pIndexBuffer(nullptr),
    m_pSkyVertexBuffer(nullptr),
    m_pSkyIndexBuffer(nullptr),
    m_pVertexShader(nullptr),
    m_pPixelShader(nullptr),
    m_pInputLayout(nullptr),
    m_pSkyVS(nullptr),
    m_pSkyPS(nullptr),
    m_pSkyInputLayout(nullptr),
    m_pSceneBuffer(nullptr),
    m_pGeomBuffer(nullptr),
    m_pTexture(nullptr),
    m_pTextureView(nullptr),
    m_pCubemapTexture(nullptr),
    m_pCubemapView(nullptr),
    m_pSampler(nullptr),
    m_pSkyDepthState(nullptr),
    m_pNoCullRS(nullptr),
    m_width(DefaultWindowWidth),
    m_height(DefaultWindowHeight),
    m_yaw(3.0f),
    m_pitch(0.0f),
    m_radius(4.0f),
    m_time(0.0f) {
    ZeroMemory(m_keys, sizeof(m_keys));
    QueryPerformanceFrequency(&m_freq);
    QueryPerformanceCounter(&m_prev);
}

DXApp::~DXApp() {
    SAFE_RELEASE(m_pNoCullRS);
    SAFE_RELEASE(m_pSkyDepthState);
    SAFE_RELEASE(m_pSampler);

    SAFE_RELEASE(m_pCubemapView);
    SAFE_RELEASE(m_pCubemapTexture);
    SAFE_RELEASE(m_pTextureView);
    SAFE_RELEASE(m_pTexture);

    SAFE_RELEASE(m_pGeomBuffer);
    SAFE_RELEASE(m_pSceneBuffer);

    SAFE_RELEASE(m_pSkyInputLayout);
    SAFE_RELEASE(m_pSkyPS);
    SAFE_RELEASE(m_pSkyVS);

    SAFE_RELEASE(m_pInputLayout);
    SAFE_RELEASE(m_pPixelShader);
    SAFE_RELEASE(m_pVertexShader);

    SAFE_RELEASE(m_pSkyIndexBuffer);
    SAFE_RELEASE(m_pSkyVertexBuffer);
    SAFE_RELEASE(m_pIndexBuffer);
    SAFE_RELEASE(m_pVertexBuffer);

    SAFE_RELEASE(m_pDepthStencilView);
    SAFE_RELEASE(m_pDepthStencilTex);
    SAFE_RELEASE(m_pBackBufferRTV);

    SAFE_RELEASE(m_pSwapChain);
    SAFE_RELEASE(m_pDeviceContext);
    SAFE_RELEASE(m_pDevice);
}

LRESULT CALLBACK DXApp::WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    DXApp* pApp = reinterpret_cast<DXApp*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));

    switch (message) {
    case WM_SIZE:
        if (pApp && pApp->m_pSwapChain) {
            UINT width = LOWORD(lParam);
            UINT height = HIWORD(lParam);
            if (width > 0 && height > 0) {
                pApp->Resize(width, height);
            }
        }
        return 0;

    case WM_KEYDOWN:
        if (pApp && wParam < 256) {
            pApp->m_keys[wParam] = true;
        }
        return 0;

    case WM_KEYUP:
        if (pApp && wParam < 256) {
            pApp->m_keys[wParam] = false;
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
}

std::string DXApp::WCSToMBS(const std::wstring& wstr) {
    int len = WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string str(len, 0);
    WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), -1, &str[0], len, nullptr, nullptr);
    return str;
}

HRESULT DXApp::SetResourceName(ID3D11DeviceChild* pResource, const std::string& name) {
    return pResource->SetPrivateData(WKPDID_D3DDebugObjectName, (UINT)name.length(), name.c_str());
}

HRESULT DXApp::CompileShaderFromFile(const std::wstring& path, ID3DBlob** ppCode) {
    FILE* pFile = nullptr;
    _wfopen_s(&pFile, path.c_str(), L"rb");
    if (!pFile) return E_FAIL;

    fseek(pFile, 0, SEEK_END);
    long size = ftell(pFile);
    fseek(pFile, 0, SEEK_SET);

    std::vector<char> data(size);
    fread(data.data(), 1, size, pFile);
    fclose(pFile);

    std::wstring ext = path.substr(path.find_last_of(L".") + 1);
    std::string entryPoint;
    std::string platform;
    if (ext == L"vs") {
        entryPoint = "vs";
        platform = "vs_5_0";
    }
    else if (ext == L"ps") {
        entryPoint = "ps";
        platform = "ps_5_0";
    }
    else {
        return E_FAIL;
    }

    UINT flags1 = 0;
#ifdef _DEBUG
    flags1 |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ID3DBlob* pErrMsg = nullptr;
    HRESULT result = D3DCompile(
        data.data(), data.size(), WCSToMBS(path).c_str(),
        nullptr, nullptr,
        entryPoint.c_str(), platform.c_str(),
        flags1, 0, ppCode, &pErrMsg
    );

    if (FAILED(result) && pErrMsg) {
        OutputDebugStringA((const char*)pErrMsg->GetBufferPointer());
    }

    SAFE_RELEASE(pErrMsg);
    return result;
}

bool DXApp::CreateBackBufferRTV() {
    SAFE_RELEASE(m_pBackBufferRTV);

    ID3D11Texture2D* pBackBuffer = nullptr;
    HRESULT hr = m_pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBackBuffer);
    if (FAILED(hr)) return false;

    hr = m_pDevice->CreateRenderTargetView(pBackBuffer, nullptr, &m_pBackBufferRTV);
    SAFE_RELEASE(pBackBuffer);

    return SUCCEEDED(hr);
}

bool DXApp::CreateDepthStencil() {
    SAFE_RELEASE(m_pDepthStencilView);
    SAFE_RELEASE(m_pDepthStencilTex);

    D3D11_TEXTURE2D_DESC depthDesc = {};
    depthDesc.Width = m_width;
    depthDesc.Height = m_height;
    depthDesc.MipLevels = 1;
    depthDesc.ArraySize = 1;
    depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.SampleDesc.Quality = 0;
    depthDesc.Usage = D3D11_USAGE_DEFAULT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    HRESULT hr = m_pDevice->CreateTexture2D(&depthDesc, nullptr, &m_pDepthStencilTex);
    if (FAILED(hr)) return false;

    hr = m_pDevice->CreateDepthStencilView(m_pDepthStencilTex, nullptr, &m_pDepthStencilView);
    return SUCCEEDED(hr);
}

bool DXApp::LoadTexture(const std::wstring& TextureName) {
    TextureDesc textureDesc;
    if (!LoadDDS(TextureName.c_str(), textureDesc, true)) {
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Format = textureDesc.fmt;
    desc.ArraySize = 1;
    desc.MipLevels = textureDesc.mipmapsCount;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Height = textureDesc.height;
    desc.Width = textureDesc.width;

    D3D11_SUBRESOURCE_DATA data = {};
    data.pSysMem = textureDesc.pData;
    data.SysMemPitch = textureDesc.pitch;
    data.SysMemSlicePitch = 0;

    HRESULT result = m_pDevice->CreateTexture2D(&desc, &data, &m_pTexture);
    if (FAILED(result)) return false;

    SetResourceName(m_pTexture, "Texture2D");

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = textureDesc.fmt;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = textureDesc.mipmapsCount;
    srvDesc.Texture2D.MostDetailedMip = 0;

    result = m_pDevice->CreateShaderResourceView(m_pTexture, &srvDesc, &m_pTextureView);
    if (FAILED(result)) return false;

    SetResourceName(m_pTextureView, "TextureView");
    return true;
}

bool DXApp::LoadCubemap() {
    const std::wstring TextureNames[6] = {
        L"sky_posx.dds",
        L"sky_negx.dds",
        L"sky_posy.dds",
        L"sky_negy.dds",
        L"sky_posz.dds",
        L"sky_negz.dds"
    };

    TextureDesc texDescs[6];
    bool ddsRes = true;
    for (int i = 0; i < 6 && ddsRes; i++) {
        ddsRes = LoadDDS(TextureNames[i].c_str(), texDescs[i], true);
    }
    if (!ddsRes) return false;

    DXGI_FORMAT textureFmt = texDescs[0].fmt;
    for (int i = 1; i < 6; i++) {
        if (texDescs[i].fmt != textureFmt ||
            texDescs[i].width != texDescs[0].width ||
            texDescs[i].height != texDescs[0].height) {
            return false;
        }
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Format = textureFmt;
    desc.ArraySize = 6;
    desc.MipLevels = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Height = texDescs[0].height;
    desc.Width = texDescs[0].width;

    D3D11_SUBRESOURCE_DATA data[6];
    for (int i = 0; i < 6; i++) {
        data[i].pSysMem = texDescs[i].pData;
        data[i].SysMemPitch = texDescs[i].pitch;
        data[i].SysMemSlicePitch = 0;
    }

    HRESULT result = m_pDevice->CreateTexture2D(&desc, data, &m_pCubemapTexture);
    if (FAILED(result)) return false;

    SetResourceName(m_pCubemapTexture, "CubemapTexture");

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = textureFmt;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.TextureCube.MipLevels = 1;
    srvDesc.TextureCube.MostDetailedMip = 0;

    result = m_pDevice->CreateShaderResourceView(m_pCubemapTexture, &srvDesc, &m_pCubemapView);
    if (FAILED(result)) return false;

    SetResourceName(m_pCubemapView, "CubemapView");
    return true;
}

bool DXApp::Init() {
    WNDCLASSEX wcex = {};
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = DXApp::WndProc;
    wcex.hInstance = m_hInstance;
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.lpszClassName = L"DXAppClass";
    if (!RegisterClassEx(&wcex)) {
        MessageBox(nullptr, L"RegisterClassEx failed", L"Error", MB_OK);
        return false;
    }

    RECT rc = { 0, 0, (LONG)m_width, (LONG)m_height };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    m_hWnd = CreateWindow(
        L"DXAppClass", L"Computer Graphics HW4",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, m_hInstance, nullptr
    );

    if (!m_hWnd) {
        MessageBox(nullptr, L"CreateWindow failed", L"Error", MB_OK);
        return false;
    }

    SetWindowLongPtr(m_hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    ShowWindow(m_hWnd, SW_SHOW);
    UpdateWindow(m_hWnd);

    HRESULT result;
    IDXGIFactory* pFactory = nullptr;
    result = CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&pFactory);
    if (FAILED(result)) {
        MessageBox(nullptr, L"CreateDXGIFactory failed", L"Error", MB_OK);
        return false;
    }

    IDXGIAdapter* pSelectedAdapter = nullptr;
    IDXGIAdapter* pAdapter = nullptr;
    UINT adapterIdx = 0;
    while (SUCCEEDED(pFactory->EnumAdapters(adapterIdx, &pAdapter))) {
        DXGI_ADAPTER_DESC desc;
        pAdapter->GetDesc(&desc);
        if (wcscmp(desc.Description, L"Microsoft Basic Render Driver") != 0) {
            pSelectedAdapter = pAdapter;
            break;
        }
        SAFE_RELEASE(pAdapter);
        adapterIdx++;
    }

    if (!pSelectedAdapter) {
        MessageBox(nullptr, L"No suitable adapter found", L"Error", MB_OK);
        SAFE_RELEASE(pFactory);
        return false;
    }

    D3D_FEATURE_LEVEL level;
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    result = D3D11CreateDevice(
        pSelectedAdapter,
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        flags,
        levels, 1,
        D3D11_SDK_VERSION,
        &m_pDevice,
        &level,
        &m_pDeviceContext
    );

    SAFE_RELEASE(pSelectedAdapter);
    if (FAILED(result) || level != D3D_FEATURE_LEVEL_11_0) {
        MessageBox(nullptr, L"D3D11CreateDevice failed", L"Error", MB_OK);
        SAFE_RELEASE(pFactory);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
    swapChainDesc.BufferCount = 1;
    swapChainDesc.BufferDesc.Width = m_width;
    swapChainDesc.BufferDesc.Height = m_height;
    swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferDesc.RefreshRate.Numerator = 60;
    swapChainDesc.BufferDesc.RefreshRate.Denominator = 1;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.OutputWindow = m_hWnd;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SampleDesc.Quality = 0;
    swapChainDesc.Windowed = TRUE;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    swapChainDesc.Flags = 0;

    result = pFactory->CreateSwapChain(m_pDevice, &swapChainDesc, &m_pSwapChain);
    SAFE_RELEASE(pFactory);
    if (FAILED(result)) {
        MessageBox(nullptr, L"CreateSwapChain failed", L"Error", MB_OK);
        return false;
    }

    if (!CreateBackBufferRTV()) {
        MessageBox(nullptr, L"CreateBackBufferRTV failed", L"Error", MB_OK);
        return false;
    }

    if (!CreateDepthStencil()) {
        MessageBox(nullptr, L"CreateDepthStencil failed", L"Error", MB_OK);
        return false;
    }

    // Geometry for cube, 24 vertices as in the slides
    static const TextureVertex Vertices[] = {
        // Bottom
        {-0.5f, -0.5f,  0.5f, 0.0f, 1.0f},
        { 0.5f, -0.5f,  0.5f, 1.0f, 1.0f},
        { 0.5f, -0.5f, -0.5f, 1.0f, 0.0f},
        {-0.5f, -0.5f, -0.5f, 0.0f, 0.0f},

        // Top
        {-0.5f,  0.5f,  0.5f, 0.0f, 1.0f},
        { 0.5f,  0.5f,  0.5f, 1.0f, 1.0f},
        { 0.5f,  0.5f, -0.5f, 1.0f, 0.0f},
        {-0.5f,  0.5f, -0.5f, 0.0f, 0.0f},

        // Front
        {-0.5f, -0.5f,  0.5f, 0.0f, 1.0f},
        { 0.5f, -0.5f,  0.5f, 1.0f, 1.0f},
        { 0.5f,  0.5f,  0.5f, 1.0f, 0.0f},
        {-0.5f,  0.5f,  0.5f, 0.0f, 0.0f},

        // Back
        { 0.5f, -0.5f, -0.5f, 0.0f, 1.0f},
        {-0.5f, -0.5f, -0.5f, 1.0f, 1.0f},
        {-0.5f,  0.5f, -0.5f, 1.0f, 0.0f},
        { 0.5f,  0.5f, -0.5f, 0.0f, 0.0f},

        // Left
        {-0.5f, -0.5f, -0.5f, 0.0f, 1.0f},
        {-0.5f, -0.5f,  0.5f, 1.0f, 1.0f},
        {-0.5f,  0.5f,  0.5f, 1.0f, 0.0f},
        {-0.5f,  0.5f, -0.5f, 0.0f, 0.0f},

        // Right
        { 0.5f, -0.5f,  0.5f, 0.0f, 1.0f},
        { 0.5f, -0.5f, -0.5f, 1.0f, 1.0f},
        { 0.5f,  0.5f, -0.5f, 1.0f, 0.0f},
        { 0.5f,  0.5f,  0.5f, 0.0f, 0.0f}
    };

    static const USHORT Indices[] = {
        0, 2, 1, 0, 3, 2,
        4, 6, 5, 4, 7, 6,
        8, 10, 9, 8, 11, 10,
        12, 14, 13, 12, 15, 14,
        16, 18, 17, 16, 19, 18,
        20, 22, 21, 20, 23, 22
    };

    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.ByteWidth = sizeof(Vertices);
    vbDesc.Usage = D3D11_USAGE_IMMUTABLE;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA vbData = {};
    vbData.pSysMem = Vertices;

    result = m_pDevice->CreateBuffer(&vbDesc, &vbData, &m_pVertexBuffer);
    if (FAILED(result)) {
        MessageBox(nullptr, L"CreateBuffer VertexBuffer failed", L"Error", MB_OK);
        return false;
    }
    SetResourceName(m_pVertexBuffer, "VertexBuffer");

    D3D11_BUFFER_DESC ibDesc = {};
    ibDesc.ByteWidth = sizeof(Indices);
    ibDesc.Usage = D3D11_USAGE_IMMUTABLE;
    ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;

    D3D11_SUBRESOURCE_DATA ibData = {};
    ibData.pSysMem = Indices;

    result = m_pDevice->CreateBuffer(&ibDesc, &ibData, &m_pIndexBuffer);
    if (FAILED(result)) {
        MessageBox(nullptr, L"CreateBuffer IndexBuffer failed", L"Error", MB_OK);
        return false;
    }
    SetResourceName(m_pIndexBuffer, "IndexBuffer");

    // Skybox geometry: simple cube around the camera
    static const SkyVertex SkyVertices[] = {
        {-1.0f, -1.0f, -1.0f},
        {-1.0f,  1.0f, -1.0f},
        { 1.0f,  1.0f, -1.0f},
        { 1.0f, -1.0f, -1.0f},
        {-1.0f, -1.0f,  1.0f},
        {-1.0f,  1.0f,  1.0f},
        { 1.0f,  1.0f,  1.0f},
        { 1.0f, -1.0f,  1.0f}
    };

    static const USHORT SkyIndices[] = {
        0, 1, 2, 0, 2, 3,
        4, 6, 5, 4, 7, 6,
        4, 5, 1, 4, 1, 0,
        3, 2, 6, 3, 6, 7,
        1, 5, 6, 1, 6, 2,
        4, 0, 3, 4, 3, 7
    };

    vbDesc.ByteWidth = sizeof(SkyVertices);
    vbData.pSysMem = SkyVertices;
    result = m_pDevice->CreateBuffer(&vbDesc, &vbData, &m_pSkyVertexBuffer);
    if (FAILED(result)) {
        MessageBox(nullptr, L"CreateBuffer SkyVertexBuffer failed", L"Error", MB_OK);
        return false;
    }

    ibDesc.ByteWidth = sizeof(SkyIndices);
    ibData.pSysMem = SkyIndices;
    result = m_pDevice->CreateBuffer(&ibDesc, &ibData, &m_pSkyIndexBuffer);
    if (FAILED(result)) {
        MessageBox(nullptr, L"CreateBuffer SkyIndexBuffer failed", L"Error", MB_OK);
        return false;
    }

    // Буфер сцены: vp + cameraPos
    {
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = sizeof(SceneBuffer);
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        desc.MiscFlags = 0;
        desc.StructureByteStride = 0;

        result = m_pDevice->CreateBuffer(&desc, nullptr, &m_pSceneBuffer);
        if (FAILED(result)) {
            MessageBox(nullptr, L"CreateBuffer SceneBuffer failed", L"Error", MB_OK);
            return false;
        }
        SetResourceName(m_pSceneBuffer, "SceneBuffer");
    }

    // Буфер геометрии: model + size
    {
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = sizeof(GeomBuffer);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;
        desc.StructureByteStride = 0;

        result = m_pDevice->CreateBuffer(&desc, nullptr, &m_pGeomBuffer);
        if (FAILED(result)) {
            MessageBox(nullptr, L"CreateBuffer GeomBuffer failed", L"Error", MB_OK);
            return false;
        }
        SetResourceName(m_pGeomBuffer, "GeomBuffer");
    }

    // Shaders for cube
    ID3DBlob* pVSCode = nullptr;
    result = CompileShaderFromFile(L"cube.vs", &pVSCode);
    if (FAILED(result)) {
        MessageBox(nullptr, L"CompileShaderFromFile cube.vs failed", L"Error", MB_OK);
        return false;
    }

    result = m_pDevice->CreateVertexShader(pVSCode->GetBufferPointer(), pVSCode->GetBufferSize(), nullptr, &m_pVertexShader);
    if (FAILED(result)) {
        SAFE_RELEASE(pVSCode);
        MessageBox(nullptr, L"CreateVertexShader cube failed", L"Error", MB_OK);
        return false;
    }
    SetResourceName(m_pVertexShader, "cube.vs");

    ID3DBlob* pPSCode = nullptr;
    result = CompileShaderFromFile(L"cube.ps", &pPSCode);
    if (FAILED(result)) {
        SAFE_RELEASE(pVSCode);
        MessageBox(nullptr, L"CompileShaderFromFile cube.ps failed", L"Error", MB_OK);
        return false;
    }

    result = m_pDevice->CreatePixelShader(pPSCode->GetBufferPointer(), pPSCode->GetBufferSize(), nullptr, &m_pPixelShader);
    SAFE_RELEASE(pPSCode);
    if (FAILED(result)) {
        SAFE_RELEASE(pVSCode);
        MessageBox(nullptr, L"CreatePixelShader cube failed", L"Error", MB_OK);
        return false;
    }
    SetResourceName(m_pPixelShader, "cube.ps");

    static const D3D11_INPUT_ELEMENT_DESC InputDesc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };

    result = m_pDevice->CreateInputLayout(InputDesc, ARRAYSIZE(InputDesc), pVSCode->GetBufferPointer(), pVSCode->GetBufferSize(), &m_pInputLayout);
    SAFE_RELEASE(pVSCode);
    if (FAILED(result)) {
        MessageBox(nullptr, L"CreateInputLayout cube failed", L"Error", MB_OK);
        return false;
    }
    SetResourceName(m_pInputLayout, "CubeInputLayout");

    // Shaders for skybox
    ID3DBlob* pSkyVSCode = nullptr;
    result = CompileShaderFromFile(L"skybox.vs", &pSkyVSCode);
    if (FAILED(result)) {
        MessageBox(nullptr, L"CompileShaderFromFile skybox.vs failed", L"Error", MB_OK);
        return false;
    }

    result = m_pDevice->CreateVertexShader(pSkyVSCode->GetBufferPointer(), pSkyVSCode->GetBufferSize(), nullptr, &m_pSkyVS);
    if (FAILED(result)) {
        SAFE_RELEASE(pSkyVSCode);
        MessageBox(nullptr, L"CreateVertexShader skybox failed", L"Error", MB_OK);
        return false;
    }
    SetResourceName(m_pSkyVS, "skybox.vs");

    ID3DBlob* pSkyPSCode = nullptr;
    result = CompileShaderFromFile(L"skybox.ps", &pSkyPSCode);
    if (FAILED(result)) {
        SAFE_RELEASE(pSkyVSCode);
        MessageBox(nullptr, L"CompileShaderFromFile skybox.ps failed", L"Error", MB_OK);
        return false;
    }

    result = m_pDevice->CreatePixelShader(pSkyPSCode->GetBufferPointer(), pSkyPSCode->GetBufferSize(), nullptr, &m_pSkyPS);
    SAFE_RELEASE(pSkyPSCode);
    if (FAILED(result)) {
        SAFE_RELEASE(pSkyVSCode);
        MessageBox(nullptr, L"CreatePixelShader skybox failed", L"Error", MB_OK);
        return false;
    }
    SetResourceName(m_pSkyPS, "skybox.ps");

    static const D3D11_INPUT_ELEMENT_DESC SkyInputDesc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };

    result = m_pDevice->CreateInputLayout(SkyInputDesc, ARRAYSIZE(SkyInputDesc), pSkyVSCode->GetBufferPointer(), pSkyVSCode->GetBufferSize(), &m_pSkyInputLayout);
    SAFE_RELEASE(pSkyVSCode);
    if (FAILED(result)) {
        MessageBox(nullptr, L"CreateInputLayout skybox failed", L"Error", MB_OK);
        return false;
    }
    SetResourceName(m_pSkyInputLayout, "SkyInputLayout");

    // Textures
    if (!LoadTexture(L"cube.dds")) {
        MessageBox(nullptr, L"Failed to load cube.dds", L"Error", MB_OK);
        return false;
    }

    if (!LoadCubemap()) {
        MessageBox(nullptr, L"Failed to load skybox cubemap DDS faces", L"Error", MB_OK);
        return false;
    }

    // Sampler
    {
        D3D11_SAMPLER_DESC desc = {};
        desc.Filter = D3D11_FILTER_ANISOTROPIC;
        desc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        desc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
        desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        desc.MinLOD = -FLT_MAX;
        desc.MaxLOD = FLT_MAX;
        desc.MipLODBias = 0.0f;
        desc.MaxAnisotropy = 16;
        desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        desc.BorderColor[0] = 1.0f;
        desc.BorderColor[1] = 1.0f;
        desc.BorderColor[2] = 1.0f;
        desc.BorderColor[3] = 1.0f;

        result = m_pDevice->CreateSamplerState(&desc, &m_pSampler);
        if (FAILED(result)) {
            MessageBox(nullptr, L"CreateSamplerState failed", L"Error", MB_OK);
            return false;
        }
        SetResourceName(m_pSampler, "Sampler");
    }

    // Depth state for skybox
    {
        D3D11_DEPTH_STENCIL_DESC desc = {};
        desc.DepthEnable = TRUE;
        desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        desc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        desc.StencilEnable = FALSE;

        result = m_pDevice->CreateDepthStencilState(&desc, &m_pSkyDepthState);
        if (FAILED(result)) {
            MessageBox(nullptr, L"CreateDepthStencilState failed", L"Error", MB_OK);
            return false;
        }
    }

    // Rasterizer state without culling
    {
        D3D11_RASTERIZER_DESC desc = {};
        desc.FillMode = D3D11_FILL_SOLID;
        desc.CullMode = D3D11_CULL_NONE;
        desc.DepthClipEnable = TRUE;
        desc.ScissorEnable = TRUE;

        result = m_pDevice->CreateRasterizerState(&desc, &m_pNoCullRS);
        if (FAILED(result)) {
            MessageBox(nullptr, L"CreateRasterizerState failed", L"Error", MB_OK);
            return false;
        }
    }

    return true;
}

void DXApp::Resize(UINT width, UINT height) {
    if (width == 0 || height == 0) return;
    if (!m_pSwapChain) return;

    m_width = width;
    m_height = height;

    m_pDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);

    SAFE_RELEASE(m_pBackBufferRTV);
    SAFE_RELEASE(m_pDepthStencilView);
    SAFE_RELEASE(m_pDepthStencilTex);

    HRESULT result = m_pSwapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    assert(SUCCEEDED(result));

    if (!CreateBackBufferRTV()) {
        assert(false);
        return;
    }

    if (!CreateDepthStencil()) {
        assert(false);
        return;
    }
}

void DXApp::Render() {
    if (!m_pDeviceContext || !m_pSwapChain || !m_pBackBufferRTV || !m_pDepthStencilView) return;
    if (m_width == 0 || m_height == 0) return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = float(now.QuadPart - m_prev.QuadPart) / float(m_freq.QuadPart);
    m_prev = now;
    m_time += dt;

   // const float cameraSpeed = 1.5f;
    //if (m_keys[VK_LEFT])  m_yaw -= cameraSpeed * dt;
    //if (m_keys[VK_RIGHT]) m_yaw += cameraSpeed * dt;
    //if (m_keys[VK_UP])    m_pitch += cameraSpeed * dt;
    //if (m_keys[VK_DOWN])  m_pitch -= cameraSpeed * dt;

    //const float pitchLimit = XM_PIDIV2 - 0.05f;
    //if (m_pitch > pitchLimit) m_pitch = pitchLimit;
    //if (m_pitch < -pitchLimit) m_pitch = -pitchLimit;

    float cy = cosf(m_yaw);
    float sy = sinf(m_yaw);
    float cp = cosf(m_pitch);
    float sp = sinf(m_pitch);

    XMVECTOR eye = XMVectorSet(
        m_radius * cp * sy,
        m_radius * sp,
        m_radius * cp * cy,
        1.0f
    );

    XMVECTOR target = XMVectorZero();
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    XMMATRIX v = XMMatrixLookAtLH(eye, target, up);
    XMMATRIX p = XMMatrixPerspectiveFovLH(XM_PIDIV4, float(m_width) / float(m_height), 0.1f, 100.0f);

    SceneBuffer sceneBuffer = {};
    XMStoreFloat4x4(&sceneBuffer.vp, XMMatrixTranspose(XMMatrixMultiply(v, p)));
    XMStoreFloat4(&sceneBuffer.cameraPos, eye);

    D3D11_MAPPED_SUBRESOURCE subresource;
    HRESULT result = m_pDeviceContext->Map(m_pSceneBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &subresource);
    assert(SUCCEEDED(result));
    if (SUCCEEDED(result)) {
        SceneBuffer& scene = *reinterpret_cast<SceneBuffer*>(subresource.pData);
        scene = sceneBuffer;
        m_pDeviceContext->Unmap(m_pSceneBuffer, 0);
    }

    m_pDeviceContext->ClearState();

    ID3D11RenderTargetView* views[] = { m_pBackBufferRTV };
    m_pDeviceContext->OMSetRenderTargets(1, views, m_pDepthStencilView);

    static const FLOAT BackColor[4] = { 0.20f, 0.20f, 0.24f, 1.0f };
    m_pDeviceContext->ClearRenderTargetView(m_pBackBufferRTV, BackColor);
    m_pDeviceContext->ClearDepthStencilView(m_pDepthStencilView, D3D11_CLEAR_DEPTH, 1.0f, 0);

    D3D11_VIEWPORT viewport = {};
    viewport.Width = (FLOAT)m_width;
    viewport.Height = (FLOAT)m_height;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    m_pDeviceContext->RSSetViewports(1, &viewport);

    D3D11_RECT rect = { 0, 0, (LONG)m_width, (LONG)m_height };
    m_pDeviceContext->RSSetScissorRects(1, &rect);

    m_pDeviceContext->RSSetState(m_pNoCullRS);

    // ----- Draw cubemap skybox -----
    {
        GeomBuffer geomBuffer = {};
        XMMATRIX model = XMMatrixIdentity();
        XMStoreFloat4x4(&geomBuffer.model, XMMatrixTranspose(model));
        geomBuffer.size = XMFLOAT4(25.0f, 25.0f, 25.0f, 1.0f);

        m_pDeviceContext->UpdateSubresource(m_pGeomBuffer, 0, nullptr, &geomBuffer, 0, 0);

        ID3D11Buffer* cbs[] = { m_pSceneBuffer, m_pGeomBuffer };
        m_pDeviceContext->VSSetConstantBuffers(0, 2, cbs);

        ID3D11Buffer* vbs[] = { m_pSkyVertexBuffer };
        UINT strides[] = { sizeof(SkyVertex) };
        UINT offsets[] = { 0 };
        m_pDeviceContext->IASetVertexBuffers(0, 1, vbs, strides, offsets);
        m_pDeviceContext->IASetIndexBuffer(m_pSkyIndexBuffer, DXGI_FORMAT_R16_UINT, 0);
        m_pDeviceContext->IASetInputLayout(m_pSkyInputLayout);
        m_pDeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        m_pDeviceContext->VSSetShader(m_pSkyVS, nullptr, 0);
        m_pDeviceContext->PSSetShader(m_pSkyPS, nullptr, 0);

        ID3D11ShaderResourceView* resources[] = { m_pCubemapView };
        m_pDeviceContext->PSSetShaderResources(0, 1, resources);

        ID3D11SamplerState* samplers[] = { m_pSampler };
        m_pDeviceContext->PSSetSamplers(0, 1, samplers);

        m_pDeviceContext->OMSetDepthStencilState(m_pSkyDepthState, 0);
        m_pDeviceContext->DrawIndexed(36, 0, 0);
    }

    // ----- Draw textured cube -----
    {
        GeomBuffer geomBuffer = {};
        XMMATRIX model = XMMatrixRotationY(m_time) * XMMatrixRotationX(m_time * 0.5f);
        XMStoreFloat4x4(&geomBuffer.model, XMMatrixTranspose(model));
        geomBuffer.size = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);

        m_pDeviceContext->UpdateSubresource(m_pGeomBuffer, 0, nullptr, &geomBuffer, 0, 0);

        ID3D11Buffer* cbs[] = { m_pSceneBuffer, m_pGeomBuffer };
        m_pDeviceContext->VSSetConstantBuffers(0, 2, cbs);

        ID3D11Buffer* vbs[] = { m_pVertexBuffer };
        UINT strides[] = { sizeof(TextureVertex) };
        UINT offsets[] = { 0 };
        m_pDeviceContext->IASetVertexBuffers(0, 1, vbs, strides, offsets);
        m_pDeviceContext->IASetIndexBuffer(m_pIndexBuffer, DXGI_FORMAT_R16_UINT, 0);
        m_pDeviceContext->IASetInputLayout(m_pInputLayout);
        m_pDeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        m_pDeviceContext->VSSetShader(m_pVertexShader, nullptr, 0);
        m_pDeviceContext->PSSetShader(m_pPixelShader, nullptr, 0);

        ID3D11ShaderResourceView* resources[] = { m_pTextureView };
        m_pDeviceContext->PSSetShaderResources(0, 1, resources);

        ID3D11SamplerState* samplers[] = { m_pSampler };
        m_pDeviceContext->PSSetSamplers(0, 1, samplers);

        m_pDeviceContext->OMSetDepthStencilState(nullptr, 0);
        m_pDeviceContext->DrawIndexed(36, 0, 0);
    }
    wchar_t title[256];
    swprintf_s(title, L"HW4 | yaw: %.2f  pitch: %.2f  radius: %.2f", m_yaw, m_pitch, m_radius);
    SetWindowText(m_hWnd, title);

    result = m_pSwapChain->Present(1, 0);
    assert(SUCCEEDED(result));
}

void DXApp::Run() {
    MSG msg = {};
    bool exit = false;

    while (!exit) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) {
                exit = true;
            }
        }
        else {
            Render();
        }
    }
}

int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nCmdShow) {
    DXApp app(hInstance);
    if (!app.Init()) {
        MessageBox(nullptr, L"Init failed", L"Error", MB_OK);
        return 1;
    }
    app.Run();
    return 0;
}