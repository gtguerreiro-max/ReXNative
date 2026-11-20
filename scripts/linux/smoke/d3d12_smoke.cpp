// A D3D12 program on Linux without Wine: an SDL3 window, a device from vkd3d-proton
// (libvkd3d-proton-d3d12.so) and a swap chain from DXVK's DXGI (libdxvk_dxgi.so), which takes the
// SDL_Window* as its HWND. Clears the back buffer to a changing colour and presents for a few
// seconds, then prints the frames per second. The check that ReXNative's D3D12 code can run on
// Linux natively, before it is ported.
//   d3d12_smoke [seconds]
#include <SDL3/SDL.h>
#include <dlfcn.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

// DXVK Native's Windows and DirectX headers (mingw-w64's, with __uuidof and IID_PPV_ARGS).
// vkd3d-proton returns structures (GetCPUDescriptorHandleForHeapStart, GetDesc) the way MSVC
// does, through a pointer: the headers' methods must do the same.
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

using PFN_CreateDXGIFactory2 = HRESULT (*)(UINT, REFIID, void**);
using PFN_D3D12CreateDevice = HRESULT (*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

static void* Load(const char* lib, const char* sym) {
  void* h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
  if (!h) { std::fprintf(stderr, "dlopen %s: %s\n", lib, dlerror()); return nullptr; }
  void* f = dlsym(h, sym);
  if (!f) std::fprintf(stderr, "dlsym %s in %s: %s\n", sym, lib, dlerror());
  return f;
}

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { std::fprintf(stderr, "%s failed: %08x\n", #x, unsigned(hr_)); return 1; } } while (0)

int main(int argc, char** argv) {
  const double seconds = argc > 1 ? std::atof(argv[1]) : 5.0;
  if (!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
  SDL_Window* window = SDL_CreateWindow("d3d12 smoke", 1280, 800, SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN);
  if (!window) { std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
  std::printf("SDL video driver: %s\n", SDL_GetCurrentVideoDriver());

  auto create_factory = reinterpret_cast<PFN_CreateDXGIFactory2>(Load("libdxvk_dxgi.so", "CreateDXGIFactory2"));
  auto create_device = reinterpret_cast<PFN_D3D12CreateDevice>(Load("libvkd3d-proton-d3d12.so", "D3D12CreateDevice"));
  if (!create_factory || !create_device) return 1;

  IDXGIFactory4* factory = nullptr;
  CHECK(create_factory(0, IID_PPV_ARGS(&factory)));
  IDXGIAdapter1* adapter = nullptr;
  CHECK(factory->EnumAdapters1(0, &adapter));
  DXGI_ADAPTER_DESC1 ad = {};
  adapter->GetDesc1(&ad);
  char name[128] = {};
  for (int i = 0; i < 127 && ad.Description[i]; ++i) name[i] = char(ad.Description[i]);
  std::printf("adapter: %s\n", name);

  ID3D12Device* device = nullptr;
  CHECK(create_device(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_COMMAND_QUEUE_DESC qd = {};
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  ID3D12CommandQueue* queue = nullptr;
  CHECK(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));

  constexpr UINT kBuffers = 3;
  DXGI_SWAP_CHAIN_DESC1 sd = {};
  sd.Width = 1280; sd.Height = 800;
  sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = kBuffers;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  sd.Scaling = DXGI_SCALING_STRETCH;
  IDXGISwapChain1* sc1 = nullptr;
  CHECK(factory->CreateSwapChainForHwnd(queue, reinterpret_cast<HWND>(window), &sd, nullptr, nullptr, &sc1));
  IDXGISwapChain3* swapchain = nullptr;
  CHECK(sc1->QueryInterface(IID_PPV_ARGS(&swapchain)));

  D3D12_DESCRIPTOR_HEAP_DESC hd = {};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = kBuffers;
  ID3D12DescriptorHeap* rtv_heap = nullptr;
  CHECK(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap)));
  const UINT stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  ID3D12Resource* buffers[kBuffers] = {};
  D3D12_CPU_DESCRIPTOR_HANDLE rtv[kBuffers];
  for (UINT i = 0; i < kBuffers; ++i) {
    CHECK(swapchain->GetBuffer(i, IID_PPV_ARGS(&buffers[i])));
    rtv[i] = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv[i].ptr += SIZE_T(i) * stride;
    device->CreateRenderTargetView(buffers[i], nullptr, rtv[i]);
  }
  ID3D12CommandAllocator* alloc[kBuffers] = {};
  for (auto& a : alloc) CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)));
  ID3D12GraphicsCommandList* list = nullptr;
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0], nullptr, IID_PPV_ARGS(&list)));
  list->Close();
  ID3D12Fence* fence = nullptr;
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
  UINT64 fence_values[kBuffers] = {}, next = 1;

  const auto start = std::chrono::steady_clock::now();
  unsigned frames = 0;
  bool quit = false;
  while (!quit) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) if (e.type == SDL_EVENT_QUIT) quit = true;
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (t >= seconds) break;
    const UINT i = swapchain->GetCurrentBackBufferIndex();
    if (fence->GetCompletedValue() < fence_values[i]) fence->SetEventOnCompletion(fence_values[i], nullptr);
    alloc[i]->Reset();
    list->Reset(alloc[i], nullptr);
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = buffers[i];
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    list->ResourceBarrier(1, &b);
    const float c[4] = {float(0.5 + 0.5 * std::sin(t * 2.0)), 0.2f, float(0.5 + 0.5 * std::cos(t * 3.0)), 1.0f};
    list->ClearRenderTargetView(rtv[i], c, 0, nullptr);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    list->ResourceBarrier(1, &b);
    list->Close();
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    if (FAILED(swapchain->Present(1, 0))) { std::fprintf(stderr, "Present failed\n"); break; }
    queue->Signal(fence, next);
    fence_values[i] = next++;
    ++frames;
  }
  const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  std::printf("frames %u in %.2f s: %.1f FPS\n", frames, t, frames / t);
  queue->Signal(fence, next);
  fence->SetEventOnCompletion(next, nullptr);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
