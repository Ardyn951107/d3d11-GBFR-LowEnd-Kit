#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>
int main(int argc,char**argv){
  { wchar_t me[MAX_PATH]; GetModuleFileNameW(0,me,MAX_PATH); DWORD h; GetFileVersionInfoSizeW(me,&h);} 
  const char* mode = argc>1?argv[1]:"ok";
  WNDCLASSW wc{}; wc.lpfnWndProc=DefWindowProcW; wc.lpszClassName=L"t"; RegisterClassW(&wc);
  HWND w=CreateWindowW(L"t",L"test",WS_OVERLAPPEDWINDOW|WS_VISIBLE,0,0,320,240,0,0,0,0);
  DXGI_SWAP_CHAIN_DESC sd{}; sd.BufferCount=2; sd.BufferDesc.Width=320; sd.BufferDesc.Height=240; sd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow=w; sd.SampleDesc.Count=1; sd.Windowed=TRUE; sd.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
  IDXGISwapChain* sc; ID3D11Device* dev; ID3D11DeviceContext* ctx;
  HRESULT hr=D3D11CreateDeviceAndSwapChain(0,D3D_DRIVER_TYPE_HARDWARE,0,0,0,0,D3D11_SDK_VERSION,&sd,&sc,&dev,0,&ctx);
  printf("create hr=%lx\n",hr); fflush(stdout); if(FAILED(hr)) return 1;
  const char* src="float4 main():SV_Target{return float4(1,0,0,1);}";
  ID3DBlob* b=0; hr=D3DCompile(src,strlen(src),0,0,0,"main","ps_4_0",0,0,&b,0); printf("compile %lx\n",hr);
  ID3D11PixelShader* ps=0; if(b) { hr=dev->CreatePixelShader(b->GetBufferPointer(),b->GetBufferSize(),0,&ps); printf("ps %lx\n",hr);}
  if(!strcmp(mode,"bad")){ char junk[64]; memset(junk,0x41,64); ID3D11PixelShader* p2=0; hr=dev->CreatePixelShader(junk,64,0,&p2); printf("bad ps %lx\n",hr);}
  for(int i=0;i<120;i++){ MSG m; while(PeekMessage(&m,0,0,0,PM_REMOVE)) DispatchMessage(&m); sc->Present(1,0); Sleep(10);}
  printf("presented\n"); fflush(stdout);
  if(!strcmp(mode,"hang")){ for(int i=0;i<700;i++){ MSG m; while(PeekMessage(&m,0,0,0,PM_REMOVE)) DispatchMessage(&m); Sleep(10);} }
  if(!strcmp(mode,"crash")){ *(volatile int*)0 = 5; }
  printf("done\n"); return 0;
}
