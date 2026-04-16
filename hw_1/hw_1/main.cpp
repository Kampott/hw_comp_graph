#include <windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <dxgi.h>
#include <assert.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

#define SAFE_RELEASE(p) { if (p) { (p)->Release(); (p) = nullptr; } }

const UINT DefaultWindowWidth = 1280;
const UINT DefaultWindowHeight = 720;

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

    HINSTANCE m_hInstance;
    HWND m_hWnd;

    ID3D11Device* m_pDevice;
    ID3D11DeviceContext* m_pDeviceContext;
    IDXGISwapChain* m_pSwapChain;
    ID3D11RenderTargetView* m_pBackBufferRTV;

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
    m_width(DefaultWindowWidth),
    m_height(DefaultWindowHeight) {
}

DXApp::~DXApp() {
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

bool DXApp::Init() {
    //Register window class
    WNDCLASSEX wcex = { 0 };
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = DXApp::WndProc;
    wcex.hInstance = m_hInstance;
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.lpszClassName = L"DXAppClass";
    if (!RegisterClassEx(&wcex)) return false;

    //Create window
    RECT rc = { 0, 0, (LONG)m_width, (LONG)m_height };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    m_hWnd = CreateWindow(L"DXAppClass", L"Computer Graphics Homework 1", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, m_hInstance, nullptr);
    if (!m_hWnd) return false;

    SetWindowLongPtr(m_hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    ShowWindow(m_hWnd, SW_SHOW);
    UpdateWindow(m_hWnd);

    //Initialize directx
    HRESULT result;
    IDXGIFactory* pFactory = nullptr;
    result = CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&pFactory);
    if (FAILED(result)) return false;

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
    if (FAILED(result)) return false;

    //Create rtv
    ID3D11Texture2D* pBackBuffer = nullptr;
    result = m_pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer);
    if (FAILED(result)) return false;

    result = m_pDevice->CreateRenderTargetView(pBackBuffer, nullptr, &m_pBackBufferRTV);
    SAFE_RELEASE(pBackBuffer);
    if (FAILED(result)) return false;

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

    static const FLOAT BackColor[4] = { 0, 0, 0, 0 }; 
    m_pDeviceContext->ClearRenderTargetView(m_pBackBufferRTV, BackColor);

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

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    DXApp app(hInstance);
    if (!app.Init()) {
        return 1;
    }
    app.Run(); 
    return 0;
}