#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <stdio.h>
int main(){
  { wchar_t me[MAX_PATH]; GetModuleFileNameW(0,me,MAX_PATH); DWORD h; GetFileVersionInfoSizeW(me,&h);}
  WNDCLASSW wc{}; wc.lpfnWndProc=DefWindowProcW; wc.lpszClassName=L"t"; RegisterClassW(&wc);
  HWND w=CreateWindowW(L"t",L"f2",WS_OVERLAPPEDWINDOW|WS_VISIBLE,0,0,320,240,0,0,0,0);
  ID3D11Device* dev; ID3D11DeviceContext* ctx; D3D11CreateDevice(0,D3D_DRIVER_TYPE_HARDWARE,0,0,0,0,D3D11_SDK_VERSION,&dev,0,&ctx);
  IDXGIFactory2* f=0; HRESULT hr=CreateDXGIFactory2(0,__uuidof(IDXGIFactory2),(void**)&f); printf("f2 %lx\n",hr);
  DXGI_SWAP_CHAIN_DESC1 d{}; d.Width=320; d.Height=240; d.Format=DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count=1; d.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount=2; d.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
  IDXGISwapChain1* sc=0; hr=f->CreateSwapChainForHwnd(dev,w,&d,0,0,&sc); printf("sc %lx\n",hr); fflush(stdout);
  for(int i=0;i<700;i++){ MSG m; while(PeekMessage(&m,0,0,0,PM_REMOVE)) DispatchMessage(&m); sc->Present(0,0); Sleep(i%100==50?150:15);}
  return 0; }
