#include <windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <assert.h>
#include <string>
#include <vector>
#include <fstream>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")

#define SAFE_RELEASE(p) { if (p) { (p)->Release(); (p) = nullptr; } }

const UINT DefaultWindowWidth = 1280;
const UINT DefaultWindowHeight = 720;

struct Vertex {
    float x, y, z;
    COLORREF color;
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
    HRESULT CompileShaderFromFile(const std::wstring& path, ID3DBlob** ppCode);
    HRESULT SetResourceName(ID3D11DeviceChild* pResource, const std::string& name);
    std::string WCSToMBS(const std::wstring& wstr);

    HINSTANCE m_hInstance;
    HWND m_hWnd;

    ID3D11Device* m_pDevice;
    ID3D11DeviceContext* m_pDeviceContext;
    IDXGISwapChain* m_pSwapChain;
    ID3D11RenderTargetView* m_pBackBufferRTV;

    ID3D11Buffer* m_pVertexBuffer;
    ID3D11Buffer* m_pIndexBuffer;
    ID3D11VertexShader* m_pVertexShader;
    ID3D11PixelShader* m_pPixelShader;
    ID3D11InputLayout* m_pInputLayout;

    UINT m_width;
    UINT m_height;
};

DXApp::DXApp(HINSTANCE hInstance) :
    m_hInstance(hInstance),
    m_hWnd(nullptr),
    m_pDevice(nullptr),
    m_pDeviceContext(nullptr),
    m_pSwapChain(nullptr),
    m_pBackBufferRTV(nullptr),
    m_pVertexBuffer(nullptr),
    m_pIndexBuffer(nullptr),
    m_pVertexShader(nullptr),
    m_pPixelShader(nullptr),
    m_pInputLayout(nullptr),
    m_width(DefaultWindowWidth),
    m_height(DefaultWindowHeight) {
}

DXApp::~DXApp() {
    SAFE_RELEASE(m_pInputLayout);
    SAFE_RELEASE(m_pPixelShader);
    SAFE_RELEASE(m_pVertexShader);
    SAFE_RELEASE(m_pIndexBuffer);
    SAFE_RELEASE(m_pVertexBuffer);
    SAFE_RELEASE(m_pBackBufferRTV);
    SAFE_RELEASE(m_pSwapChain);
    SAFE_RELEASE(m_pDeviceContext);
    SAFE_RELEASE(m_pDevice);
}

LRESULT CALLBACK DXApp::WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    DXApp* pApp = reinterpret_cast<DXApp*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));

    switch (message) {
    case WM_SIZE: {
        if (pApp && pApp->m_pSwapChain) {
            UINT width = LOWORD(lParam);
            UINT height = HIWORD(lParam);
            if (width > 0 && height > 0) {
                pApp->Resize(width, height);
            }
        }
        return 0;
    }
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
    HRESULT result = D3DCompile(data.data(), data.size(), WCSToMBS(path).c_str(), nullptr, nullptr,
        entryPoint.c_str(), platform.c_str(), flags1, 0, ppCode, &pErrMsg);
    if (FAILED(result) && pErrMsg) {
        OutputDebugStringA((const char*)pErrMsg->GetBufferPointer());
    }
    SAFE_RELEASE(pErrMsg);
    return result;
}

bool DXApp::Init() {
    //Register window class
    WNDCLASSEX wcex = { 0 };
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

    //Create window
    RECT rc = { 0, 0, (LONG)m_width, (LONG)m_height };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    m_hWnd = CreateWindow(L"DXAppClass", L"Computer Graphics HW2", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, m_hInstance, nullptr);
    if (!m_hWnd) {
        MessageBox(nullptr, L"CreateWindow failed", L"Error", MB_OK);
        return false;
    }


    SetWindowLongPtr(m_hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    ShowWindow(m_hWnd, SW_SHOW);
    UpdateWindow(m_hWnd);

    //Initialize directx
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
    result = D3D11CreateDevice(pSelectedAdapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        flags, levels, 1, D3D11_SDK_VERSION, &m_pDevice, &level, &m_pDeviceContext);
    SAFE_RELEASE(pSelectedAdapter);
    if (FAILED(result) || level != D3D_FEATURE_LEVEL_11_0) {
        MessageBox(nullptr, L"D3D11CreateDevice failed", L"Error", MB_OK);
        SAFE_RELEASE(pFactory);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC swapChainDesc = { 0 };
    swapChainDesc.BufferCount = 2;
    swapChainDesc.BufferDesc.Width = m_width;
    swapChainDesc.BufferDesc.Height = m_height;
    swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferDesc.RefreshRate.Numerator = 0;
    swapChainDesc.BufferDesc.RefreshRate.Denominator = 1;
    swapChainDesc.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
    swapChainDesc.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.OutputWindow = m_hWnd;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SampleDesc.Quality = 0;
    swapChainDesc.Windowed = TRUE;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.Flags = 0;

    result = pFactory->CreateSwapChain(m_pDevice, &swapChainDesc, &m_pSwapChain);
    SAFE_RELEASE(pFactory);
    if (FAILED(result)) {
        MessageBox(nullptr, L"CreateSwapChain failed", L"Error", MB_OK);
        return false;
    }

    //Create rtv
    ID3D11Texture2D* pBackBuffer = nullptr;
    result = m_pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer);
    if (FAILED(result)) {
        MessageBox(nullptr, L"GetBuffer failed", L"Error", MB_OK);
        return false;
    }

    result = m_pDevice->CreateRenderTargetView(pBackBuffer, nullptr, &m_pBackBufferRTV);
    SAFE_RELEASE(pBackBuffer);
    if (FAILED(result)) {
        MessageBox(nullptr, L"CreateRenderTargetView failed", L"Error", MB_OK);
        return false;
    }

    //Create vertex buffer
    static const Vertex Vertices[] = {
        {-0.5f, -0.5f, 0.0f, RGB(255, 0, 0)},
        { 0.5f, -0.5f, 0.0f, RGB(0, 255, 0)},
        { 0.0f,  0.5f, 0.0f, RGB(0, 0, 255)}
    };

    D3D11_BUFFER_DESC vbDesc = { 0 };
    vbDesc.ByteWidth = sizeof(Vertices);
    vbDesc.Usage = D3D11_USAGE_IMMUTABLE;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA vbData = { 0 };
    vbData.pSysMem = Vertices;

    result = m_pDevice->CreateBuffer(&vbDesc, &vbData, &m_pVertexBuffer);
    if (FAILED(result)) {
        MessageBox(nullptr, L"CreateBuffer VertexBuffer failed", L"Error", MB_OK);
        return false;
    }
    SetResourceName(m_pVertexBuffer, "VertexBuffer");

    //Create index buffer
    static const USHORT Indices[] = { 0, 2, 1 };

    D3D11_BUFFER_DESC ibDesc = { 0 };
    ibDesc.ByteWidth = sizeof(Indices);
    ibDesc.Usage = D3D11_USAGE_IMMUTABLE;
    ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;

    D3D11_SUBRESOURCE_DATA ibData = { 0 };
    ibData.pSysMem = Indices;

    result = m_pDevice->CreateBuffer(&ibDesc, &ibData, &m_pIndexBuffer);
    if (FAILED(result)) {
        MessageBox(nullptr, L"CreateBuffer IndexBuffer failed", L"Error", MB_OK);
        return false;
    }
    SetResourceName(m_pIndexBuffer, "IndexBuffer");

    //Shaders
    ID3DBlob* pVSCode = nullptr;
    result = CompileShaderFromFile(L"vertex.vs", &pVSCode);
    if (FAILED(result)) return false;

    result = m_pDevice->CreateVertexShader(pVSCode->GetBufferPointer(), pVSCode->GetBufferSize(), nullptr, &m_pVertexShader);
    if (FAILED(result)) {
        SAFE_RELEASE(pVSCode);
        MessageBox(nullptr, L"CreateVertexShader failed", L"Error", MB_OK);
        return false;
    }
    SetResourceName(m_pVertexShader, "vertex.vs");

    ID3DBlob* pPSCode = nullptr;
    result = CompileShaderFromFile(L"pixel.ps", &pPSCode);
    if (FAILED(result)) {
        SAFE_RELEASE(pVSCode);
        MessageBox(nullptr, L"CompileShaderFromFile failed", L"Error", MB_OK);
        return false;
    }

    result = m_pDevice->CreatePixelShader(pPSCode->GetBufferPointer(), pPSCode->GetBufferSize(), nullptr, &m_pPixelShader);
    SAFE_RELEASE(pPSCode);
    if (FAILED(result)) {
        SAFE_RELEASE(pVSCode);
        MessageBox(nullptr, L"CreatePixelShader failed", L"Error", MB_OK);
        return false;
    }
    SetResourceName(m_pPixelShader, "pixel.ps");

    //Input layout
    static const D3D11_INPUT_ELEMENT_DESC InputDesc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };

    result = m_pDevice->CreateInputLayout(InputDesc, ARRAYSIZE(InputDesc), pVSCode->GetBufferPointer(), pVSCode->GetBufferSize(), &m_pInputLayout);
    SAFE_RELEASE(pVSCode);
    if (FAILED(result)) {
        MessageBox(nullptr, L"CreateInputLayout failed", L"Error", MB_OK);
        return false;
    }
    SetResourceName(m_pInputLayout, "InputLayout");

    return true;
}

void DXApp::Resize(UINT width, UINT height) {
    if (width == 0 || height == 0) return;

    SAFE_RELEASE(m_pBackBufferRTV);

    HRESULT result = m_pSwapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    assert(SUCCEEDED(result));

    ID3D11Texture2D* pBackBuffer = nullptr;
    result = m_pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer);
    assert(SUCCEEDED(result));

    result = m_pDevice->CreateRenderTargetView(pBackBuffer, nullptr, &m_pBackBufferRTV);
    SAFE_RELEASE(pBackBuffer);
    assert(SUCCEEDED(result));

    m_width = width;
    m_height = height;
}

void DXApp::Render() {
    m_pDeviceContext->ClearState();

    ID3D11RenderTargetView* views[] = { m_pBackBufferRTV };
    m_pDeviceContext->OMSetRenderTargets(1, views, nullptr);

    static const FLOAT BackColor[4] = { 0.25f, 0.25f, 0.25f, 1.0f };
    m_pDeviceContext->ClearRenderTargetView(m_pBackBufferRTV, BackColor);

    D3D11_VIEWPORT viewport = { 0 };
    viewport.Width = (FLOAT)m_width;
    viewport.Height = (FLOAT)m_height;
    viewport.MaxDepth = 1.0f;
    m_pDeviceContext->RSSetViewports(1, &viewport);

    D3D11_RECT rect = { 0 };
    rect.right = m_width;
    rect.bottom = m_height;
    m_pDeviceContext->RSSetScissorRects(1, &rect);

    m_pDeviceContext->IASetIndexBuffer(m_pIndexBuffer, DXGI_FORMAT_R16_UINT, 0);

    ID3D11Buffer* vertexBuffers[] = { m_pVertexBuffer };
    UINT strides[] = { sizeof(Vertex) };
    UINT offsets[] = { 0 };
    m_pDeviceContext->IASetVertexBuffers(0, 1, vertexBuffers, strides, offsets);

    m_pDeviceContext->IASetInputLayout(m_pInputLayout);
    m_pDeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    m_pDeviceContext->VSSetShader(m_pVertexShader, nullptr, 0);
    m_pDeviceContext->PSSetShader(m_pPixelShader, nullptr, 0);

    m_pDeviceContext->DrawIndexed(3, 0, 0);

    HRESULT result = m_pSwapChain->Present(0, 0);
    assert(SUCCEEDED(result));
}

void DXApp::Run() {
    MSG msg = { 0 };
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