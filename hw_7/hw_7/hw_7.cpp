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
#include <cstdio>
#include <cmath>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")

#define SAFE_RELEASE(p) { if (p) { (p)->Release(); (p) = nullptr; } }

const UINT DefaultWindowWidth = 1280;
const UINT DefaultWindowHeight = 720;

using namespace DirectX;

struct Point2f {
    float x, y;
    Point2f() : x(0), y(0) {}
    Point2f(float _x, float _y) : x(_x), y(_y) {}
};

struct Point3f {
    float x, y, z;
    Point3f() : x(0), y(0), z(0) {}
    Point3f(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}
};

struct Point4f {
    float x, y, z, w;
    Point4f() : x(0), y(0), z(0), w(0) {}
    Point4f(float _x, float _y, float _z, float _w) : x(_x), y(_y), z(_z), w(_w) {}
};

struct Point4i {
    int x, y, z, w;
    Point4i() : x(0), y(0), z(0), w(0) {}
    Point4i(int _x, int _y, int _z, int _w) : x(_x), y(_y), z(_z), w(_w) {}
};

struct Matrix4f {
    Point4f r[4];
};

static Point4f StorePoint4f(const XMVECTOR& v) {
    return Point4f(XMVectorGetX(v), XMVectorGetY(v), XMVectorGetZ(v), XMVectorGetW(v));
}

static Matrix4f StoreMatrix(const XMMATRIX& m) {
    Matrix4f out = {};
    out.r[0] = StorePoint4f(m.r[0]);
    out.r[1] = StorePoint4f(m.r[1]);
    out.r[2] = StorePoint4f(m.r[2]);
    out.r[3] = StorePoint4f(m.r[3]);
    return out;
}



struct TextureNormalVertex {
    float x, y, z;
    float nx, ny, nz;
    float tx, ty, tz;
    float u, v;
};

struct SkyVertex {
    float x, y, z;
};

struct MipData {
    UINT32 pitch;
    UINT32 dataSize;
    size_t offset;
};

struct TextureDesc {
    UINT32 mipmapsCount = 1;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    UINT32 width = 0;
    UINT32 height = 0;
    std::vector<uint8_t> storage;
    std::vector<MipData> mips;
};

struct Light {
    Point4f pos;
    Point4f color;
};

struct SceneBuffer {
    Matrix4f vp;
    Point4f cameraPos;
    Point4i lightCount;     // x = light count
    Light lights[10];
    Point4f ambientColor;
};

struct GeomBuffer {
    Matrix4f model;
    Matrix4f normalMatrix;
    Point4f color;
    Point4f params;       // x = shininess, y = useNormalMap, z/w reserved
};

struct RenderItem {
    Point3f position;
    float scale = 1.0f;
    float rotationSpeed = 0.0f;
    Point4f color;
    float sortKey = 0.0f;
    float shininess = 32.0f;
    bool useNormalMap = true;
};

static const UINT MaxInstances = 100;

struct GeomBufferInstData {
    Matrix4f model;
    Matrix4f norm;
    Point4f shineSpeedTexIdNM; // x=shininess, y=rotSpeed, z=texId, w=useNormalMap
    Point4f posAngle;          // xyz=position, w=currentAngle
};

struct GeomBufferInstVis {
    Point4i ids[MaxInstances]; // x = реальный индекс инстанса
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
    FILE* file = nullptr;
    if (_wfopen_s(&file, fileName, L"rb") != 0 || !file)
        return false;

    _fseeki64(file, 0, SEEK_END);
    long long fileSize = _ftelli64(file);
    _fseeki64(file, 0, SEEK_SET);

    if (fileSize < (long long)(sizeof(uint32_t) + sizeof(DDS_HEADER))) {
        fclose(file);
        return false;
    }

    std::vector<uint8_t> bytes((size_t)fileSize);
    size_t readCount = fread(bytes.data(), 1, bytes.size(), file);
    fclose(file);
    if (readCount != bytes.size())
        return false;

    const uint32_t* magic = reinterpret_cast<const uint32_t*>(bytes.data());
    if (*magic != DDS_MAGIC)
        return false;

    const DDS_HEADER* header = reinterpret_cast<const DDS_HEADER*>(bytes.data() + sizeof(uint32_t));
    if (header->size != 124 || header->ddspf.size != 32)
        return false;

    DXGI_FORMAT fmt = DDSFormatFromPixelFormat(header->ddspf);
    if (fmt == DXGI_FORMAT_UNKNOWN)
        return false;

    UINT mipLevels = (header->flags & DDSD_MIPMAPCOUNT) && header->mipMapCount > 0
        ? header->mipMapCount : 1;
    if (topMipOnly)
        mipLevels = 1;

    textureDesc.fmt = fmt;
    textureDesc.width = header->width;
    textureDesc.height = header->height;
    textureDesc.mipmapsCount = mipLevels;
    textureDesc.mips.resize(mipLevels);

    size_t totalSize = 0;
    UINT w = header->width, h = header->height;
    for (UINT i = 0; i < mipLevels; i++) {
        UINT pitch = 0, mipSize = 0;
        if (IsBlockCompressed(fmt)) {
            UINT blockBytes = GetBytesPerBlock(fmt);
            UINT bw = (std::max)(1u, (w + 3u) / 4u);
            UINT bh = (std::max)(1u, (h + 3u) / 4u);
            pitch = bw * blockBytes;
            mipSize = pitch * bh;
        }
        else {
            UINT bpp = GetBytesPerPixel(fmt);
            if (bpp == 0) return false;
            pitch = w * bpp;
            mipSize = pitch * h;
        }
        textureDesc.mips[i] = { pitch, mipSize, totalSize };
        totalSize += mipSize;
        w = (std::max)(1u, w / 2u);
        h = (std::max)(1u, h / 2u);
    }

    const uint8_t* src = bytes.data() + sizeof(uint32_t) + sizeof(DDS_HEADER);
    size_t remaining = bytes.size() - (sizeof(uint32_t) + sizeof(DDS_HEADER));
    if (remaining < totalSize)
        return false;

    textureDesc.storage.resize(totalSize);
    std::memcpy(textureDesc.storage.data(), src, totalSize);

    return true;
}

class D3DInclude final : public ID3DInclude {
public:
    STDMETHOD(Open)(D3D_INCLUDE_TYPE IncludeType, LPCSTR pFileName, LPCVOID, LPCVOID* ppData, UINT* pBytes) override {
        (void)IncludeType;

        FILE* file = nullptr;
        if (fopen_s(&file, pFileName, "rb") != 0 || !file) {
            return E_FAIL;
        }

        fseek(file, 0, SEEK_END);
        long size = ftell(file);
        fseek(file, 0, SEEK_SET);

        if (size <= 0) {
            fclose(file);
            return E_FAIL;
        }

        void* data = malloc((size_t)size);
        if (!data) {
            fclose(file);
            return E_OUTOFMEMORY;
        }

        size_t readCount = fread(data, 1, (size_t)size, file);
        fclose(file);

        if (readCount != (size_t)size) {
            free(data);
            return E_FAIL;
        }

        *ppData = data;
        *pBytes = (UINT)size;
        return S_OK;
    }

    STDMETHOD(Close)(LPCVOID pData) override {
        free(const_cast<void*>(pData));
        return S_OK;
    }
};

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

    HRESULT CompileShaderFromFile(const std::wstring& path, ID3DBlob** ppCode, const std::vector<D3D_SHADER_MACRO>& defines = {});
    HRESULT SetResourceName(ID3D11DeviceChild* pResource, const std::string& name);
    std::string WCSToMBS(const std::wstring& wstr);

    bool CreateBackBufferRTV();
    bool CreateDepthStencil();
    bool LoadTextureFromDDS(const std::wstring& fileName, ID3D11Texture2D** ppTexture, ID3D11ShaderResourceView** ppView);
    bool CreateFlatNormalMap();
    bool LoadCubemap();

    bool LoadTextureArray();

    void RenderCube(const XMMATRIX& model, const Point4f& color, float shininess, bool useNormalMap, bool transparent, float emissive = 0.0f, float emissiveIntensity = 0.0f);
    void RenderLightMarkers(const SceneBuffer& scene);
    void RenderSkybox();

    HINSTANCE m_hInstance;
    HWND m_hWnd;

    ID3D11Device* m_pDevice;
    ID3D11DeviceContext* m_pDeviceContext;
    IDXGISwapChain* m_pSwapChain;
    ID3D11RenderTargetView* m_pBackBufferRTV;
    ID3D11DepthStencilState* m_pNoDepthState;
    ID3D11PixelShader* m_pPixelShaderNormalMap; 
    ID3D11PixelShader* m_pPixelShaderEmissive;  
    ID3D11VertexShader* m_pInstVS;
    ID3D11PixelShader* m_pInstPS;

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

    ID3D11Texture2D* m_pNormalTexture;
    ID3D11ShaderResourceView* m_pNormalTextureView;

    ID3D11Texture2D* m_pCubemapTexture;
    ID3D11ShaderResourceView* m_pCubemapView;

    ID3D11SamplerState* m_pSampler;
    ID3D11DepthStencilState* m_pOpaqueDepthState;
    ID3D11DepthStencilState* m_pSkyDepthState;
    ID3D11DepthStencilState* m_pTransparentDepthState;
    ID3D11BlendState* m_pAlphaBlendState;
    ID3D11BlendState* m_pAdditiveBlendState;
    ID3D11RasterizerState* m_pNoCullRS;
    ID3D11RasterizerState* m_pBackCullRS;

    ID3D11Buffer* m_pGeomBufferInst;    // данные всех инстансов
    ID3D11Buffer* m_pGeomBufferInstVis; // индексы видимых инстансов
    ID3D11Texture2D* m_pTextureArray;
    ID3D11ShaderResourceView* m_pTextureArrayView;

    std::vector<GeomBufferInstData> m_instances;

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
    m_pNormalTexture(nullptr),
    m_pNormalTextureView(nullptr),
    m_pCubemapTexture(nullptr),
    m_pCubemapView(nullptr),
    m_pSampler(nullptr),
    m_pOpaqueDepthState(nullptr),
    m_pSkyDepthState(nullptr),
    m_pTransparentDepthState(nullptr),
    m_pAlphaBlendState(nullptr),
    m_pAdditiveBlendState(nullptr),
    m_pNoCullRS(nullptr),
    m_pBackCullRS(nullptr),
    m_width(DefaultWindowWidth),
    m_height(DefaultWindowHeight),
    m_pPixelShaderEmissive(nullptr),
    m_pPixelShaderNormalMap(nullptr),
    m_pNoDepthState(nullptr),
    m_pGeomBufferInst(nullptr),
    m_pGeomBufferInstVis(nullptr),
    m_pTextureArray(nullptr),
    m_pTextureArrayView(nullptr),
    m_pInstVS(nullptr),
    m_pInstPS(nullptr),
    m_yaw(3.0f),
    m_pitch(0.0f),
    m_radius(4.0f),
    m_time(0.0f) {
    ZeroMemory(m_keys, sizeof(m_keys));
    QueryPerformanceFrequency(&m_freq);
    QueryPerformanceCounter(&m_prev);
}

DXApp::~DXApp() {
    SAFE_RELEASE(m_pBackCullRS);
    SAFE_RELEASE(m_pNoCullRS);
    SAFE_RELEASE(m_pAlphaBlendState);
    SAFE_RELEASE(m_pAdditiveBlendState);
    SAFE_RELEASE(m_pTransparentDepthState);
    SAFE_RELEASE(m_pSkyDepthState);
    SAFE_RELEASE(m_pOpaqueDepthState);
    SAFE_RELEASE(m_pSampler);

    SAFE_RELEASE(m_pCubemapView);
    SAFE_RELEASE(m_pCubemapTexture);
    SAFE_RELEASE(m_pNormalTextureView);
    SAFE_RELEASE(m_pNormalTexture);
    SAFE_RELEASE(m_pTextureView);
    SAFE_RELEASE(m_pTexture);

    SAFE_RELEASE(m_pGeomBuffer);
    SAFE_RELEASE(m_pSceneBuffer);

    SAFE_RELEASE(m_pSkyInputLayout);
    SAFE_RELEASE(m_pSkyPS);
    SAFE_RELEASE(m_pSkyVS);

    SAFE_RELEASE(m_pInputLayout);
    SAFE_RELEASE(m_pPixelShaderEmissive);
    SAFE_RELEASE(m_pPixelShaderNormalMap);
    SAFE_RELEASE(m_pPixelShader);
    SAFE_RELEASE(m_pVertexShader);

    SAFE_RELEASE(m_pSkyIndexBuffer);
    SAFE_RELEASE(m_pSkyVertexBuffer);
    SAFE_RELEASE(m_pIndexBuffer);
    SAFE_RELEASE(m_pVertexBuffer);

    SAFE_RELEASE(m_pDepthStencilView);
    SAFE_RELEASE(m_pDepthStencilTex);
    SAFE_RELEASE(m_pBackBufferRTV);
    SAFE_RELEASE(m_pNoDepthState);

    SAFE_RELEASE(m_pSwapChain);
    SAFE_RELEASE(m_pDeviceContext);
    SAFE_RELEASE(m_pDevice);
    SAFE_RELEASE(m_pInstVS);
    SAFE_RELEASE(m_pInstPS);

    SAFE_RELEASE(m_pGeomBufferInst);
    SAFE_RELEASE(m_pGeomBufferInstVis);
    SAFE_RELEASE(m_pTextureArray);
    SAFE_RELEASE(m_pTextureArrayView);
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
    if (!pResource) return E_FAIL;
    return pResource->SetPrivateData(WKPDID_D3DDebugObjectName, (UINT)name.length(), name.c_str());
}

HRESULT DXApp::CompileShaderFromFile(const std::wstring& path, ID3DBlob** ppCode, const std::vector<D3D_SHADER_MACRO>& defines) {
    FILE* pFile = nullptr;
    _wfopen_s(&pFile, path.c_str(), L"rb");
    if (!pFile) return E_FAIL;

    fseek(pFile, 0, SEEK_END);
    long size = ftell(pFile);
    fseek(pFile, 0, SEEK_SET);

    std::vector<char> data((size_t)size);
    fread(data.data(), 1, data.size(), pFile);
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

    std::vector<D3D_SHADER_MACRO> macros = defines;
    macros.push_back({ nullptr, nullptr });

    D3DInclude includeHandler;
    ID3DBlob* pErrMsg = nullptr;
    HRESULT result = D3DCompile(
        data.data(), data.size(), WCSToMBS(path).c_str(),
        macros.data(), &includeHandler,
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
    depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.SampleDesc.Quality = 0;
    depthDesc.Usage = D3D11_USAGE_DEFAULT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    HRESULT hr = m_pDevice->CreateTexture2D(&depthDesc, nullptr, &m_pDepthStencilTex);
    if (FAILED(hr)) return false;

    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Texture2D.MipSlice = 0;

    hr = m_pDevice->CreateDepthStencilView(m_pDepthStencilTex, &dsvDesc, &m_pDepthStencilView);
    return SUCCEEDED(hr);
}

bool DXApp::LoadTextureFromDDS(const std::wstring& fileName, ID3D11Texture2D** ppTexture, ID3D11ShaderResourceView** ppView) {
    if (!ppTexture || !ppView) return false;

    TextureDesc textureDesc;
    if (!LoadDDS(fileName.c_str(), textureDesc, false)) {
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

    std::vector<D3D11_SUBRESOURCE_DATA> subData(textureDesc.mipmapsCount);
    for (UINT i = 0; i < textureDesc.mipmapsCount; i++) {
        subData[i].pSysMem = textureDesc.storage.data() + textureDesc.mips[i].offset;
        subData[i].SysMemPitch = textureDesc.mips[i].pitch;
        subData[i].SysMemSlicePitch = 0;
    }


    HRESULT result = m_pDevice->CreateTexture2D(&desc, subData.data(), ppTexture);
    if (FAILED(result)) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = textureDesc.fmt;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = textureDesc.mipmapsCount;
    srvDesc.Texture2D.MostDetailedMip = 0;

    result = m_pDevice->CreateShaderResourceView(*ppTexture, &srvDesc, ppView);
    if (FAILED(result)) return false;

    return true;
}

bool DXApp::CreateFlatNormalMap() {
    SAFE_RELEASE(m_pNormalTexture);
    SAFE_RELEASE(m_pNormalTextureView);

    const uint8_t pixelRGBA[4] = { 128, 128, 255, 255 }; // flat normal in R8G8B8A8_UNORM

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = 1;
    desc.Height = 1;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = pixelRGBA;
    init.SysMemPitch = sizeof(pixelRGBA);

    HRESULT hr = m_pDevice->CreateTexture2D(&desc, &init, &m_pNormalTexture);
    if (FAILED(hr)) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = desc.Format;
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MostDetailedMip = 0;
    srv.Texture2D.MipLevels = 1;

    hr = m_pDevice->CreateShaderResourceView(m_pNormalTexture, &srv, &m_pNormalTextureView);
    return SUCCEEDED(hr);
}

bool DXApp::LoadTextureArray() {
    const std::wstring names[] = { L"cube.dds", L"cube2.dds" };
    const UINT count = 2;

    TextureDesc descs[2];
    for (UINT i = 0; i < count; i++) {
        if (!LoadDDS(names[i].c_str(), descs[i], false))
            return false;
    }

    for (UINT i = 1; i < count; i++) {
        if (GetBytesPerBlock(descs[i].fmt) == 0) {
            MessageBox(nullptr, L"TextureArray: unsupported format", L"Error", MB_OK);
            return false;
        }
        if (descs[i].width != descs[0].width || descs[i].height != descs[0].height) {
            MessageBox(nullptr, L"TextureArray: size mismatch", L"Error", MB_OK);
            return false;
        }
        if (descs[i].fmt != descs[0].fmt) {
            MessageBox(nullptr, L"TextureArray: format mismatch", L"Error", MB_OK);
            return false;
        }
    }

    UINT mipLevels = descs[0].mipmapsCount;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Format = descs[0].fmt;
    desc.ArraySize = count;
    desc.MipLevels = mipLevels;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.SampleDesc.Count = 1;
    desc.Width = descs[0].width;
    desc.Height = descs[0].height;


    std::vector<D3D11_SUBRESOURCE_DATA> data(mipLevels * count);
    for (UINT j = 0; j < count; j++) {
        for (UINT i = 0; i < mipLevels; i++) {
            data[j * mipLevels + i].pSysMem = descs[j].storage.data() + descs[j].mips[i].offset;
            data[j * mipLevels + i].SysMemPitch = descs[j].mips[i].pitch;
            data[j * mipLevels + i].SysMemSlicePitch = 0;
        }
    }

    HRESULT hr = m_pDevice->CreateTexture2D(&desc, data.data(), &m_pTextureArray);
    if (FAILED(hr)) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = descs[0].fmt;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDesc.Texture2DArray.ArraySize = count;
    srvDesc.Texture2DArray.FirstArraySlice = 0;
    srvDesc.Texture2DArray.MipLevels = mipLevels;
    srvDesc.Texture2DArray.MostDetailedMip = 0;

    hr = m_pDevice->CreateShaderResourceView(m_pTextureArray, &srvDesc, &m_pTextureArrayView);
    return SUCCEEDED(hr);
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
        data[i].pSysMem = texDescs[i].storage.data() + texDescs[i].mips[0].offset;
        data[i].SysMemPitch = texDescs[i].mips[0].pitch;
        data[i].SysMemSlicePitch = 0;
    }

    HRESULT result = m_pDevice->CreateTexture2D(&desc, data, &m_pCubemapTexture);
    if (FAILED(result)) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = textureFmt;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.TextureCube.MipLevels = 1;
    srvDesc.TextureCube.MostDetailedMip = 0;

    result = m_pDevice->CreateShaderResourceView(m_pCubemapTexture, &srvDesc, &m_pCubemapView);
    if (FAILED(result)) return false;

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
        L"DXAppClass", L"Computer Graphics HW5 - Lighting + Normal Map",
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

    static const TextureNormalVertex Vertices[] = {
        // Bottom face (0, -1, 0), tangent +X
        { -0.5f, -0.5f,  0.5f,   0.0f, -1.0f, 0.0f,   1.0f, 0.0f, 0.0f,   0.0f, 1.0f },
        {  0.5f, -0.5f,  0.5f,    0.0f, -1.0f, 0.0f,   1.0f, 0.0f, 0.0f,   1.0f, 1.0f },
        {  0.5f, -0.5f, -0.5f,    0.0f, -1.0f, 0.0f,   1.0f, 0.0f, 0.0f,   1.0f, 0.0f },
        { -0.5f, -0.5f, -0.5f,    0.0f, -1.0f, 0.0f,   1.0f, 0.0f, 0.0f,   0.0f, 0.0f },

        // Top face (0, +1, 0), tangent +X
        { -0.5f,  0.5f,  0.5f,    0.0f, 1.0f, 0.0f,    1.0f, 0.0f, 0.0f,   0.0f, 1.0f },
        {  0.5f,  0.5f,  0.5f,    0.0f, 1.0f, 0.0f,    1.0f, 0.0f, 0.0f,   1.0f, 1.0f },
        {  0.5f,  0.5f, -0.5f,    0.0f, 1.0f, 0.0f,    1.0f, 0.0f, 0.0f,   1.0f, 0.0f },
        { -0.5f,  0.5f, -0.5f,    0.0f, 1.0f, 0.0f,    1.0f, 0.0f, 0.0f,   0.0f, 0.0f },

        // Front face (0, 0, +1), tangent +X
        { -0.5f, -0.5f,  0.5f,    0.0f, 0.0f, 1.0f,    1.0f, 0.0f, 0.0f,   0.0f, 1.0f },
        {  0.5f, -0.5f,  0.5f,    0.0f, 0.0f, 1.0f,    1.0f, 0.0f, 0.0f,   1.0f, 1.0f },
        {  0.5f,  0.5f,  0.5f,    0.0f, 0.0f, 1.0f,    1.0f, 0.0f, 0.0f,   1.0f, 0.0f },
        { -0.5f,  0.5f,  0.5f,    0.0f, 0.0f, 1.0f,    1.0f, 0.0f, 0.0f,   0.0f, 0.0f },

        // Back face (0, 0, -1), tangent -X to keep UV orientation intuitive
        {  0.5f, -0.5f, -0.5f,    0.0f, 0.0f, -1.0f,   -1.0f, 0.0f, 0.0f,   0.0f, 1.0f },
        { -0.5f, -0.5f, -0.5f,    0.0f, 0.0f, -1.0f,   -1.0f, 0.0f, 0.0f,   1.0f, 1.0f },
        { -0.5f,  0.5f, -0.5f,    0.0f, 0.0f, -1.0f,   -1.0f, 0.0f, 0.0f,   1.0f, 0.0f },
        {  0.5f,  0.5f, -0.5f,    0.0f, 0.0f, -1.0f,   -1.0f, 0.0f, 0.0f,   0.0f, 0.0f },

        // Left face (-1, 0, 0), tangent +Z
        { -0.5f, -0.5f, -0.5f,   -1.0f, 0.0f, 0.0f,    0.0f, 0.0f, 1.0f,   0.0f, 1.0f },
        { -0.5f, -0.5f,  0.5f,   -1.0f, 0.0f, 0.0f,    0.0f, 0.0f, 1.0f,   1.0f, 1.0f },
        { -0.5f,  0.5f,  0.5f,   -1.0f, 0.0f, 0.0f,    0.0f, 0.0f, 1.0f,   1.0f, 0.0f },
        { -0.5f,  0.5f, -0.5f,   -1.0f, 0.0f, 0.0f,    0.0f, 0.0f, 1.0f,   0.0f, 0.0f },

        // Right face (+1, 0, 0), tangent -Z
        {  0.5f, -0.5f,  0.5f,    1.0f, 0.0f, 0.0f,    0.0f, 0.0f, -1.0f,   0.0f, 1.0f },
        {  0.5f, -0.5f, -0.5f,    1.0f, 0.0f, 0.0f,    0.0f, 0.0f, -1.0f,   1.0f, 1.0f },
        {  0.5f,  0.5f, -0.5f,    1.0f, 0.0f, 0.0f,    0.0f, 0.0f, -1.0f,   1.0f, 0.0f },
        {  0.5f,  0.5f,  0.5f,    1.0f, 0.0f, 0.0f,    0.0f, 0.0f, -1.0f,   0.0f, 0.0f }
    };

    static const USHORT Indices[] = {
        0, 2, 1, 0, 3, 2,
        4, 5, 6, 4, 6, 7,
        8, 9, 10, 8, 10, 11,
        12, 13, 14, 12, 14, 15,
        16, 17, 18, 16, 18, 19,
        20, 21, 22, 20, 22, 23
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
    SetResourceName(m_pSkyVertexBuffer, "SkyVertexBuffer");

    ibDesc.ByteWidth = sizeof(SkyIndices);
    ibData.pSysMem = SkyIndices;
    result = m_pDevice->CreateBuffer(&ibDesc, &ibData, &m_pSkyIndexBuffer);
    if (FAILED(result)) {
        MessageBox(nullptr, L"CreateBuffer SkyIndexBuffer failed", L"Error", MB_OK);
        return false;
    }
    SetResourceName(m_pSkyIndexBuffer, "SkyIndexBuffer");

    {
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = sizeof(SceneBuffer);
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        result = m_pDevice->CreateBuffer(&desc, nullptr, &m_pSceneBuffer);
        if (FAILED(result)) {
            MessageBox(nullptr, L"CreateBuffer SceneBuffer failed", L"Error", MB_OK);
            return false;
        }
        SetResourceName(m_pSceneBuffer, "SceneBuffer");
    }

    {
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = sizeof(GeomBuffer);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

        result = m_pDevice->CreateBuffer(&desc, nullptr, &m_pGeomBuffer);
        if (FAILED(result)) {
            MessageBox(nullptr, L"CreateBuffer GeomBuffer failed", L"Error", MB_OK);
            return false;
        }
        SetResourceName(m_pGeomBuffer, "GeomBuffer");
    }

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

    // Обычный (без normal map)
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

    // С normal map
    ID3DBlob* pPSNMCode = nullptr;
    result = CompileShaderFromFile(L"cube.ps", &pPSNMCode, { {"USE_NORMAL_MAP", "1"} });
    if (FAILED(result)) {
        SAFE_RELEASE(pVSCode);
        MessageBox(nullptr, L"CompileShaderFromFile cube.ps USE_NORMAL_MAP failed", L"Error", MB_OK);
        return false;
    }
    result = m_pDevice->CreatePixelShader(pPSNMCode->GetBufferPointer(), pPSNMCode->GetBufferSize(), nullptr, &m_pPixelShaderNormalMap);
    SAFE_RELEASE(pPSNMCode);
    if (FAILED(result)) {
        SAFE_RELEASE(pVSCode);
        MessageBox(nullptr, L"CreatePixelShader NormalMap failed", L"Error", MB_OK);
        return false;
    }
    SetResourceName(m_pPixelShaderNormalMap, "cube.ps[USE_NORMAL_MAP]");

    // Эмиссивный
    ID3DBlob* pPSEmCode = nullptr;
    result = CompileShaderFromFile(L"cube.ps", &pPSEmCode, { {"EMISSIVE", "1"} });
    if (FAILED(result)) {
        SAFE_RELEASE(pVSCode);
        MessageBox(nullptr, L"CompileShaderFromFile cube.ps EMISSIVE failed", L"Error", MB_OK);
        return false;
    }
    result = m_pDevice->CreatePixelShader(pPSEmCode->GetBufferPointer(), pPSEmCode->GetBufferSize(), nullptr, &m_pPixelShaderEmissive);
    SAFE_RELEASE(pPSEmCode);
    if (FAILED(result)) {
        SAFE_RELEASE(pVSCode);
        MessageBox(nullptr, L"CreatePixelShader Emissive failed", L"Error", MB_OK);
        return false;
    }
    SetResourceName(m_pPixelShaderEmissive, "cube.ps[EMISSIVE]");

    static const D3D11_INPUT_ELEMENT_DESC InputDesc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TANGENT",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 36, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };

    result = m_pDevice->CreateInputLayout(InputDesc, ARRAYSIZE(InputDesc), pVSCode->GetBufferPointer(), pVSCode->GetBufferSize(), &m_pInputLayout);
    SAFE_RELEASE(pVSCode);
    if (FAILED(result)) {
        MessageBox(nullptr, L"CreateInputLayout cube failed", L"Error", MB_OK);
        return false;
    }
    SetResourceName(m_pInputLayout, "CubeInputLayout");

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

    // ---- Instancing shaders ----
    ID3DBlob* pInstVSCode = nullptr;
    result = CompileShaderFromFile(L"inst.vs", &pInstVSCode);
    if (FAILED(result)) {
        MessageBox(nullptr, L"CompileShaderFromFile inst.vs failed", L"Error", MB_OK);
        return false;
    }
    result = m_pDevice->CreateVertexShader(
        pInstVSCode->GetBufferPointer(), pInstVSCode->GetBufferSize(),
        nullptr, &m_pInstVS);
    SAFE_RELEASE(pInstVSCode);
    if (FAILED(result)) {
        MessageBox(nullptr, L"CreateVertexShader inst failed", L"Error", MB_OK);
        return false;
    }
    SetResourceName(m_pInstVS, "inst.vs");

    ID3DBlob* pInstPSCode = nullptr;
    result = CompileShaderFromFile(L"inst.ps", &pInstPSCode);
    if (FAILED(result)) {
        MessageBox(nullptr, L"CompileShaderFromFile inst.ps failed", L"Error", MB_OK);
        return false;
    }
    result = m_pDevice->CreatePixelShader(
        pInstPSCode->GetBufferPointer(), pInstPSCode->GetBufferSize(),
        nullptr, &m_pInstPS);
    SAFE_RELEASE(pInstPSCode);
    if (FAILED(result)) {
        MessageBox(nullptr, L"CreatePixelShader inst failed", L"Error", MB_OK);
        return false;
    }
    SetResourceName(m_pInstPS, "inst.ps");

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

    if (!LoadTextureFromDDS(L"cube.dds", &m_pTexture, &m_pTextureView)) {
        MessageBox(nullptr, L"Failed to load cube.dds", L"Error", MB_OK);
        return false;
    }

    if (!LoadTextureFromDDS(L"cube_normal.dds", &m_pNormalTexture, &m_pNormalTextureView)) {
        if (!CreateFlatNormalMap()) {
            MessageBox(nullptr, L"Failed to create fallback normal map", L"Error", MB_OK);
            return false;
        }
    }

    if (!LoadTextureArray()) {
        MessageBox(nullptr, L"Failed to load texture array", L"Error", MB_OK);
        return false;
    }
    if (!LoadCubemap()) {
        MessageBox(nullptr, L"Failed to load skybox cubemap DDS faces", L"Error", MB_OK);
        return false;
    }
    // Буфер данных всех инстансов
    {
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = sizeof(GeomBufferInstData) * MaxInstances;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        result = m_pDevice->CreateBuffer(&desc, nullptr, &m_pGeomBufferInst);
    }

    // Буфер видимых индексов
    {
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = sizeof(GeomBufferInstVis);
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        result = m_pDevice->CreateBuffer(&desc, nullptr, &m_pGeomBufferInstVis);
    }

    struct InstDesc { float x, y, z, rotSpeed, shininess, texId; };
    InstDesc descs4[4] = {
        { 0.5f, 0.0f,  0.0f,  0.8f,  48.0f, 0.0f }, // старая текстура
        {  0.0f, 0.0f,  2.5f, -0.5f,  32.0f, 0.0f }, // старая текстура
        {  2.0f, 0.0f,  0.0f,  1.1f,  2.0f, 1.0f }, // новая текстура
        {  0.0f, 0.0f, -2.5f, -0.9f,  10.0f, 1.0f }, // новая текстура
    };
    for (int i = 0; i < 4; i++) {
        GeomBufferInstData inst = {};
        inst.posAngle = Point4f(descs4[i].x, descs4[i].y, descs4[i].z, 0.0f);
        inst.shineSpeedTexIdNM = Point4f(descs4[i].shininess, descs4[i].rotSpeed,
            descs4[i].texId, 1.0f); // w=1 = normal map вкл
        m_instances.push_back(inst);
    }
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

    {
        D3D11_DEPTH_STENCIL_DESC desc = {};
        desc.DepthEnable = TRUE;
        desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        desc.DepthFunc = D3D11_COMPARISON_GREATER;
        desc.StencilEnable = FALSE;

        result = m_pDevice->CreateDepthStencilState(&desc, &m_pOpaqueDepthState);
        if (FAILED(result)) {
            MessageBox(nullptr, L"CreateDepthStencilState opaque failed", L"Error", MB_OK);
            return false;
        }
    }

    {
        D3D11_DEPTH_STENCIL_DESC desc = {};
        desc.DepthEnable = TRUE;
        desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        desc.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
        desc.StencilEnable = FALSE;

        result = m_pDevice->CreateDepthStencilState(&desc, &m_pTransparentDepthState);
        if (FAILED(result)) {
            MessageBox(nullptr, L"CreateDepthStencilState transparent failed", L"Error", MB_OK);
            return false;
        }
    }

    {
        D3D11_DEPTH_STENCIL_DESC desc = {};
        desc.DepthEnable = TRUE;
        desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        desc.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
        desc.StencilEnable = FALSE;

        result = m_pDevice->CreateDepthStencilState(&desc, &m_pSkyDepthState);
        if (FAILED(result)) {
            MessageBox(nullptr, L"CreateDepthStencilState sky failed", L"Error", MB_OK);
            return false;
        }
    }

    {
        D3D11_BLEND_DESC desc = {};
        desc.AlphaToCoverageEnable = FALSE;
        desc.IndependentBlendEnable = FALSE;
        desc.RenderTarget[0].BlendEnable = TRUE;
        desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

        result = m_pDevice->CreateBlendState(&desc, &m_pAlphaBlendState);
        if (FAILED(result)) {
            MessageBox(nullptr, L"CreateBlendState failed", L"Error", MB_OK);
            return false;
        }
    }

    {
        D3D11_BLEND_DESC desc = {};
        desc.AlphaToCoverageEnable = FALSE;
        desc.IndependentBlendEnable = FALSE;
        desc.RenderTarget[0].BlendEnable = TRUE;
        desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        desc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

        result = m_pDevice->CreateBlendState(&desc, &m_pAdditiveBlendState);
        if (FAILED(result)) {
            MessageBox(nullptr, L"CreateBlendState additive failed", L"Error", MB_OK);
            return false;
        }
    }

    {
        D3D11_RASTERIZER_DESC desc = {};
        desc.FillMode = D3D11_FILL_SOLID;
        desc.CullMode = D3D11_CULL_NONE;
        desc.DepthClipEnable = TRUE;
        desc.ScissorEnable = TRUE;

        result = m_pDevice->CreateRasterizerState(&desc, &m_pNoCullRS);
        if (FAILED(result)) {
            MessageBox(nullptr, L"CreateRasterizerState (no cull) failed", L"Error", MB_OK);
            return false;
        }
    }

    {
        D3D11_RASTERIZER_DESC desc = {};
        desc.FillMode = D3D11_FILL_SOLID;
        desc.CullMode = D3D11_CULL_BACK;
        desc.FrontCounterClockwise = FALSE;
        desc.DepthClipEnable = TRUE;
        desc.ScissorEnable = TRUE;

        result = m_pDevice->CreateRasterizerState(&desc, &m_pBackCullRS);
        if (FAILED(result)) {
            MessageBox(nullptr, L"CreateRasterizerState (back cull) failed", L"Error", MB_OK);
            return false;
        }
    }
    {
        D3D11_DEPTH_STENCIL_DESC desc = {};
        desc.DepthEnable = FALSE;
        desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        desc.StencilEnable = FALSE;

        result = m_pDevice->CreateDepthStencilState(&desc, &m_pNoDepthState);
        if (FAILED(result)) {
            MessageBox(nullptr, L"CreateDepthStencilState no-depth failed", L"Error", MB_OK);
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

void DXApp::RenderCube(const XMMATRIX& model, const Point4f& color, float shininess, bool useNormalMap, bool transparent, float emissive, float emissiveIntensity) {
    GeomBuffer geomBuffer = {};
    geomBuffer.model = StoreMatrix(model);
    XMMATRIX normalMat = XMMatrixTranspose(XMMatrixInverse(nullptr, model));
    geomBuffer.normalMatrix = StoreMatrix(normalMat);
    geomBuffer.color = color;
    geomBuffer.params = Point4f(shininess, useNormalMap ? 1.0f : 0.0f, emissive, emissiveIntensity);

    m_pDeviceContext->UpdateSubresource(m_pGeomBuffer, 0, nullptr, &geomBuffer, 0, 0);

    ID3D11Buffer* cbs[] = { m_pSceneBuffer, m_pGeomBuffer };
    m_pDeviceContext->VSSetConstantBuffers(0, 2, cbs);
    m_pDeviceContext->PSSetConstantBuffers(0, 2, cbs);

    ID3D11Buffer* vbs[] = { m_pVertexBuffer };
    UINT strides[] = { sizeof(TextureNormalVertex) };
    UINT offsets[] = { 0 };
    m_pDeviceContext->IASetVertexBuffers(0, 1, vbs, strides, offsets);
    m_pDeviceContext->IASetIndexBuffer(m_pIndexBuffer, DXGI_FORMAT_R16_UINT, 0);
    m_pDeviceContext->IASetInputLayout(m_pInputLayout);
    m_pDeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    m_pDeviceContext->VSSetShader(m_pVertexShader, nullptr, 0);
    if (emissive > 0.5f)
        m_pDeviceContext->PSSetShader(m_pPixelShaderEmissive, nullptr, 0);
    else if (useNormalMap)
        m_pDeviceContext->PSSetShader(m_pPixelShaderNormalMap, nullptr, 0);
    else
        m_pDeviceContext->PSSetShader(m_pPixelShader, nullptr, 0);

    ID3D11ShaderResourceView* resources[2] = { m_pTextureView, m_pNormalTextureView };
    m_pDeviceContext->PSSetShaderResources(0, 2, resources);

    ID3D11SamplerState* samplers[] = { m_pSampler };
    m_pDeviceContext->PSSetSamplers(0, 1, samplers);

    const FLOAT blendFactor[4] = { 0.f, 0.f, 0.f, 0.f };

    if (emissive > 0.5f) {
        m_pDeviceContext->OMSetBlendState(m_pAdditiveBlendState, blendFactor, 0xFFFFFFFF);
        m_pDeviceContext->OMSetDepthStencilState(m_pTransparentDepthState, 0);
    }
    else if (transparent) {
        m_pDeviceContext->OMSetBlendState(m_pAlphaBlendState, blendFactor, 0xFFFFFFFF);
        m_pDeviceContext->OMSetDepthStencilState(m_pTransparentDepthState, 0);
    }
    else {
        m_pDeviceContext->OMSetBlendState(nullptr, blendFactor, 0xFFFFFFFF);
        m_pDeviceContext->OMSetDepthStencilState(m_pOpaqueDepthState, 0);
    }

    m_pDeviceContext->RSSetState(m_pBackCullRS);
    m_pDeviceContext->DrawIndexed(36, 0, 0);
}

void DXApp::RenderLightMarkers(const SceneBuffer& scene) {
    const FLOAT blendFactor[4] = { 0.f, 0.f, 0.f, 0.f };

    m_pDeviceContext->OMSetBlendState(nullptr, blendFactor, 0xFFFFFFFF);
    m_pDeviceContext->OMSetDepthStencilState(m_pOpaqueDepthState, 0);

    for (int i = 0; i < scene.lightCount.x && i < 10; ++i) {
        const Light& light = scene.lights[i];

        XMMATRIX model =
            XMMatrixScaling(0.30f, 0.30f, 0.30f) *
            XMMatrixTranslation(light.pos.x, light.pos.y, light.pos.z);

        RenderCube(
            model,
            Point4f(1.0f, 1.0f, 1.0f, 1.0f),
            0.0f,
            false,
            false,
            1.0f,
            0.0f
        );
    }
}

void DXApp::RenderSkybox() {
    GeomBuffer geomBuffer = {};
    geomBuffer.model = StoreMatrix(XMMatrixIdentity());
    geomBuffer.normalMatrix = StoreMatrix(XMMatrixIdentity());
    geomBuffer.color = Point4f(1.f, 1.f, 1.f, 1.f);
    geomBuffer.params = Point4f(25.0f, 0.0f, 0.0f, 0.0f);

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

    const FLOAT blendFactor[4] = { 0.f, 0.f, 0.f, 0.f };
    m_pDeviceContext->OMSetBlendState(nullptr, blendFactor, 0xFFFFFFFF);
    m_pDeviceContext->OMSetDepthStencilState(m_pSkyDepthState, 0);
    m_pDeviceContext->RSSetState(m_pNoCullRS);

    m_pDeviceContext->DrawIndexed(36, 0, 0);
}

void DXApp::Render() {
    if (!m_pDeviceContext || !m_pSwapChain || !m_pBackBufferRTV || !m_pDepthStencilView) return;
    if (m_width == 0 || m_height == 0) return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = float(now.QuadPart - m_prev.QuadPart) / float(m_freq.QuadPart);
    m_prev = now;
    m_time += dt;

    const float cameraSpeed = 1.5f;
    if (m_keys[VK_LEFT])  m_yaw -= cameraSpeed * dt;
    if (m_keys[VK_RIGHT]) m_yaw += cameraSpeed * dt;
    if (m_keys[VK_UP])    m_pitch += cameraSpeed * dt;
    if (m_keys[VK_DOWN])  m_pitch -= cameraSpeed * dt;

    constexpr float pitchLimit = XMConvertToRadians(75.0f);
    if (m_pitch > pitchLimit) m_pitch = pitchLimit;
    if (m_pitch < -pitchLimit) m_pitch = -pitchLimit;

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
    XMMATRIX p = XMMatrixPerspectiveFovLH(XM_PIDIV4, float(m_width) / float(m_height), 100.0f, 0.1f);

    XMMATRIX vSky = v;
    vSky.r[3] = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);

    SceneBuffer sceneBuffer = {};
    sceneBuffer.vp = StoreMatrix(XMMatrixMultiply(v, p));
    sceneBuffer.cameraPos = StorePoint4f(eye);
    sceneBuffer.lightCount = Point4i(3, 1, 0, 0);
    sceneBuffer.ambientColor = Point4f(0.14f, 0.14f, 0.16f, 1.0f);

    const float t = m_time;
    sceneBuffer.lights[0].pos = Point4f(2.5f, 2.5f, 0.0f, 1.0f);
    sceneBuffer.lights[0].color = Point4f(3.0f, 3.0f, 3.0f, 1.0f);

    sceneBuffer.lights[1].pos = Point4f(-2.5f, -1.0f, 0.0f, 1.0f);
    sceneBuffer.lights[1].color = Point4f(3.0f, 3.0f, 3.0f, 1.0f);

    sceneBuffer.lights[2].pos = Point4f(0.0f, 2.5f, 0.0f, 1.0f);
    sceneBuffer.lights[2].color = Point4f(3.0f, 3.0f, 3.0f, 1.0f);

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

    static const FLOAT BackColor[4] = { 0.18f, 0.18f, 0.22f, 1.0f };
    m_pDeviceContext->ClearRenderTargetView(m_pBackBufferRTV, BackColor);
    m_pDeviceContext->ClearDepthStencilView(m_pDepthStencilView, D3D11_CLEAR_DEPTH, 0.0f, 0);

    D3D11_VIEWPORT viewport = {};
    viewport.Width = (FLOAT)m_width;
    viewport.Height = (FLOAT)m_height;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    m_pDeviceContext->RSSetViewports(1, &viewport);

    D3D11_RECT rect = { 0, 0, (LONG)m_width, (LONG)m_height };
    m_pDeviceContext->RSSetScissorRects(1, &rect);

    RenderSkybox();

    XMMATRIX vp = XMMatrixMultiply(v, p);

    // Транспонируем чтобы удобно брать строки
    XMMATRIX vpT = XMMatrixTranspose(vp);

    XMVECTOR planes[6];
    planes[0] = XMVectorAdd(vpT.r[3], vpT.r[2]);        // near:   row3 + row2
    planes[1] = XMVectorSubtract(vpT.r[3], vpT.r[2]);   // far:    row3 - row2
    planes[2] = XMVectorAdd(vpT.r[3], vpT.r[0]);        // left:   row3 + row0
    planes[3] = XMVectorSubtract(vpT.r[3], vpT.r[0]);   // right:  row3 - row0
    planes[4] = XMVectorSubtract(vpT.r[3], vpT.r[1]);   // top:    row3 - row1
    planes[5] = XMVectorAdd(vpT.r[3], vpT.r[1]);        // bottom: row3 + row1

    // Нормализуем плоскости
    for (int i = 0; i < 6; i++) {
        float len = XMVectorGetX(XMVector3Length(planes[i]));
        if (len > 0.0f)
            planes[i] = XMVectorScale(planes[i], 1.0f / len);
    }

    auto IsBoxInside = [&](XMVECTOR bbMin, XMVECTOR bbMax) -> bool {
        for (int i = 0; i < 6; i++) {
            float nx = XMVectorGetX(planes[i]);
            float ny = XMVectorGetY(planes[i]);
            float nz = XMVectorGetZ(planes[i]);
            XMVECTOR p = XMVectorSet(
                nx < 0 ? XMVectorGetX(bbMin) : XMVectorGetX(bbMax),
                ny < 0 ? XMVectorGetY(bbMin) : XMVectorGetY(bbMax),
                nz < 0 ? XMVectorGetZ(bbMin) : XMVectorGetZ(bbMax),
                1.0f);
            float s = XMVectorGetX(XMVector4Dot(p, planes[i]));
            if (s < 0.0f) return false;
        }
        return true;
        };


    std::vector<GeomBufferInstData> instData(MaxInstances);
    GeomBufferInstVis visData = {};
    UINT visCount = 0;

    for (UINT i = 0; i < (UINT)m_instances.size(); i++) {
        auto& inst = m_instances[i];
        inst.posAngle.w += inst.shineSpeedTexIdNM.y * dt; // обновляем угол

        float x = inst.posAngle.x, y = inst.posAngle.y, z = inst.posAngle.z;
        XMMATRIX model =
            XMMatrixRotationY(inst.posAngle.w) *
            XMMatrixTranslation(x, y, z);
        XMMATRIX normMat = XMMatrixTranspose(XMMatrixInverse(nullptr, model));
        instData[i].model = StoreMatrix(model);
        instData[i].norm = StoreMatrix(normMat);
        instData[i].posAngle = inst.posAngle;
        instData[i].shineSpeedTexIdNM = inst.shineSpeedTexIdNM;

        XMVECTOR bbMin = XMVectorSet(x - 0.5f, y - 0.5f, z - 0.5f, 1.0f);
        XMVECTOR bbMax = XMVectorSet(x + 0.5f, y + 0.5f, z + 0.5f, 1.0f);

        if (IsBoxInside(bbMin, bbMax) && visCount < MaxInstances) {
            visData.ids[visCount] = Point4i((int)i, 0, 0, 0);
            visCount++;
        }
    }

    m_pDeviceContext->UpdateSubresource(m_pGeomBufferInst, 0, nullptr, instData.data(), 0, 0);

    D3D11_MAPPED_SUBRESOURCE ms;
    m_pDeviceContext->Map(m_pGeomBufferInstVis, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    memcpy(ms.pData, &visData, sizeof(visData));
    m_pDeviceContext->Unmap(m_pGeomBufferInstVis, 0);

    // Draw instanced cubes
    {
        ID3D11Buffer* instCbs[] = { m_pSceneBuffer, m_pGeomBufferInst, m_pGeomBufferInstVis };
        m_pDeviceContext->VSSetConstantBuffers(0, 3, instCbs);
        m_pDeviceContext->PSSetConstantBuffers(0, 3, instCbs);

        ID3D11ShaderResourceView* srvs[] = { m_pTextureArrayView, m_pNormalTextureView };
        m_pDeviceContext->PSSetShaderResources(0, 2, srvs);

        ID3D11SamplerState* samplers[] = { m_pSampler };
        m_pDeviceContext->PSSetSamplers(0, 1, samplers);

        ID3D11Buffer* vbs[] = { m_pVertexBuffer };
        UINT strides[] = { sizeof(TextureNormalVertex) };
        UINT offsets[] = { 0 };
        m_pDeviceContext->IASetVertexBuffers(0, 1, vbs, strides, offsets);
        m_pDeviceContext->IASetIndexBuffer(m_pIndexBuffer, DXGI_FORMAT_R16_UINT, 0);
        m_pDeviceContext->IASetInputLayout(m_pInputLayout);
        m_pDeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // Шейдеры для инстансинга
        m_pDeviceContext->VSSetShader(m_pInstVS, nullptr, 0);
        m_pDeviceContext->PSSetShader(m_pInstPS, nullptr, 0);

        // Непрозрачный рендер
        const FLOAT blendFactor[4] = { 0.f, 0.f, 0.f, 0.f };
        m_pDeviceContext->OMSetBlendState(nullptr, blendFactor, 0xFFFFFFFF);
        m_pDeviceContext->OMSetDepthStencilState(m_pOpaqueDepthState, 0);
        m_pDeviceContext->RSSetState(m_pBackCullRS);

        m_pDeviceContext->DrawIndexedInstanced(36, visCount, 0, 0, 0);
    }
    RenderLightMarkers(sceneBuffer);
    wchar_t title[256];
    swprintf_s(title, L"HW5 | lighting + normal map | yaw: %.2f  pitch: %.2f  radius: %.2f", m_yaw, m_pitch, m_radius);
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
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    DXApp app(hInstance);
    if (!app.Init()) {
        MessageBox(nullptr, L"Init failed", L"Error", MB_OK);
        return 1;
    }
    app.Run();
    return 0;
}

