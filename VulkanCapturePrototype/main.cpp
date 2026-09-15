#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <vulkan/vulkan.h>
#include <vector>
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <cstring>
#include <cstdint>
using Microsoft::WRL::ComPtr;
static void hr(HRESULT r,const char* s){if(FAILED(r))throw std::runtime_error(s);}
static void vk(VkResult r,const char* s){if(r!=VK_SUCCESS)throw std::runtime_error(s);}
static RECT client(HWND h){RECT r;GetClientRect(h,&r);POINT p{r.left,r.top},q{r.right,r.bottom};ClientToScreen(h,&p);ClientToScreen(h,&q);return {p.x,p.y,q.x,q.y};}
static bool overlap(RECT a,RECT b){RECT c;return IntersectRect(&c,&a,&b)!=FALSE;}
static std::vector<HWND> windows;
static BOOL CALLBACK enumerate(HWND h,LPARAM){wchar_t t[256];GetWindowTextW(h,t,256);RECT r=client(h);if(IsWindowVisible(h)&&t[0]&&r.right>r.left&&r.bottom>r.top){windows.push_back(h);std::wcout<<windows.size()<<L": "<<t<<L"\n";}return TRUE;}
static LRESULT CALLBACK proc(HWND h,UINT m,WPARAM w,LPARAM l){if(m==WM_CLOSE){DestroyWindow(h);return 0;}if(m==WM_DESTROY){PostQuitMessage(0);return 0;}if(m==WM_NCHITTEST)return HTTRANSPARENT;return DefWindowProcW(h,m,w,l);}
struct Capture {
 ComPtr<ID3D11Device> dev;ComPtr<ID3D11DeviceContext> ctx;ComPtr<IDXGIOutputDuplication> dup;ComPtr<ID3D11Texture2D> stage;DXGI_OUTPUT_DESC output{};
 void init(HWND target){
  ComPtr<IDXGIFactory1> factory;hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI factory");HMONITOR monitor=MonitorFromWindow(target,MONITOR_DEFAULTTONEAREST);
  for(UINT a=0;;++a){ComPtr<IDXGIAdapter1> adapter;if(factory->EnumAdapters1(a,&adapter)==DXGI_ERROR_NOT_FOUND)break;
   for(UINT o=0;;++o){ComPtr<IDXGIOutput> out;if(adapter->EnumOutputs(o,&out)==DXGI_ERROR_NOT_FOUND)break;DXGI_OUTPUT_DESC d{};hr(out->GetDesc(&d),"Output description");if(d.Monitor!=monitor)continue;
    if(d.Rotation!=DXGI_MODE_ROTATION_IDENTITY)throw std::runtime_error("Use an unrotated landscape monitor.");output=d;
    hr(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&dev,nullptr,&ctx),"D3D11 device");
    ComPtr<IDXGIOutput1> out1;hr(out.As(&out1),"Output1");hr(out1->DuplicateOutput(dev.Get(),&dup),"Desktop duplication unavailable");return;
   }
  }throw std::runtime_error("Target monitor not found");
 }
 bool frame(HWND target,void* dst,unsigned width,unsigned height,bool rgba){
  RECT r=client(target),bounds=output.DesktopCoordinates;
  if(IsIconic(target)||r.left<bounds.left||r.top<bounds.top||r.right>bounds.right||r.bottom>bounds.bottom||r.right<=r.left||r.bottom<=r.top)throw std::runtime_error("Keep target visible and entirely on its original monitor.");
  DXGI_OUTDUPL_FRAME_INFO info{};ComPtr<IDXGIResource> resource;HRESULT result=dup->AcquireNextFrame(16,&info,&resource);
  if(result==DXGI_ERROR_WAIT_TIMEOUT)return false;
  hr(result,"Capture lost: restart prototype after display/mode changes");
  struct Release{IDXGIOutputDuplication* d;~Release(){d->ReleaseFrame();}} release{dup.Get()};
  ComPtr<ID3D11Texture2D> tex;hr(resource.As(&tex),"Capture texture");D3D11_TEXTURE2D_DESC td{};tex->GetDesc(&td);
  if(!stage){td.Usage=D3D11_USAGE_STAGING;td.BindFlags=0;td.MiscFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;hr(dev->CreateTexture2D(&td,nullptr,&stage),"Readback texture");}
  ctx->CopyResource(stage.Get(),tex.Get());D3D11_MAPPED_SUBRESOURCE map{};hr(ctx->Map(stage.Get(),0,D3D11_MAP_READ,0,&map),"Readback map");
  auto* outputBytes=static_cast<unsigned char*>(dst);unsigned sw=r.right-r.left,sh=r.bottom-r.top;
  for(unsigned y=0;y<height;++y){unsigned sy=(unsigned)((uint64_t)y*sh/height)+r.top-bounds.top;auto* row=static_cast<unsigned char*>(map.pData)+(size_t)sy*map.RowPitch;
   for(unsigned x=0;x<width;++x){unsigned sx=(unsigned)((uint64_t)x*sw/width)+r.left-bounds.left;auto* p=row+4*sx;auto* q=outputBytes+4*((size_t)y*width+x);q[0]=p[rgba?2:0];q[1]=p[1];q[2]=p[rgba?0:2];q[3]=255;}
  }ctx->Unmap(stage.Get(),0);return true;
 }
};
struct Presenter {
 VkInstance instance{};VkSurfaceKHR surface{};VkPhysicalDevice physical{};VkDevice device{};VkQueue queue{};uint32_t family{};
 VkSwapchainKHR swapchain{};std::vector<VkImage> images;VkExtent2D extent{};VkFormat format{};
 VkBuffer buffer{};VkDeviceMemory memory{};void* mapped{};VkCommandPool pool{};VkCommandBuffer command{};VkSemaphore available{},done{};
 ~Presenter(){if(device){vkDeviceWaitIdle(device);if(mapped)vkUnmapMemory(device,memory);if(buffer)vkDestroyBuffer(device,buffer,nullptr);if(memory)vkFreeMemory(device,memory,nullptr);if(available)vkDestroySemaphore(device,available,nullptr);if(done)vkDestroySemaphore(device,done,nullptr);if(pool)vkDestroyCommandPool(device,pool,nullptr);if(swapchain)vkDestroySwapchainKHR(device,swapchain,nullptr);vkDestroyDevice(device,nullptr);}if(surface)vkDestroySurfaceKHR(instance,surface,nullptr);if(instance)vkDestroyInstance(instance,nullptr);}
 void init(HWND window){
  const char* exts[]={VK_KHR_SURFACE_EXTENSION_NAME,VK_KHR_WIN32_SURFACE_EXTENSION_NAME};VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="VulkanCapturePrototype";app.apiVersion=VK_API_VERSION_1_0;
  VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ci.pApplicationInfo=&app;ci.enabledExtensionCount=2;ci.ppEnabledExtensionNames=exts;vk(vkCreateInstance(&ci,nullptr,&instance),"Vulkan instance");
  VkWin32SurfaceCreateInfoKHR wi{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};wi.hinstance=GetModuleHandleW(nullptr);wi.hwnd=window;vk(vkCreateWin32SurfaceKHR(instance,&wi,nullptr,&surface),"Win32 Vulkan surface");
  uint32_t n=0;vk(vkEnumeratePhysicalDevices(instance,&n,nullptr),"GPU enumeration");std::vector<VkPhysicalDevice> gpus(n);vk(vkEnumeratePhysicalDevices(instance,&n,gpus.data()),"GPUs");
  for(auto g:gpus){VkPhysicalDeviceProperties props;vkGetPhysicalDeviceProperties(g,&props);if(props.vendorID!=0x10de)continue;uint32_t count=0;vkGetPhysicalDeviceQueueFamilyProperties(g,&count,nullptr);std::vector<VkQueueFamilyProperties> families(count);vkGetPhysicalDeviceQueueFamilyProperties(g,&count,families.data());for(uint32_t f=0;f<count;++f){VkBool32 supported=0;vk(vkGetPhysicalDeviceSurfaceSupportKHR(g,f,surface,&supported),"Surface support");if(supported&&(families[f].queueFlags&VK_QUEUE_GRAPHICS_BIT)){physical=g;family=f;break;}}if(physical)break;}
  if(!physical)throw std::runtime_error("No NVIDIA Vulkan graphics/presentation device found");
  float priority=1;VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qi.queueFamilyIndex=family;qi.queueCount=1;qi.pQueuePriorities=&priority;
  const char* de=VK_KHR_SWAPCHAIN_EXTENSION_NAME;VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};di.queueCreateInfoCount=1;di.pQueueCreateInfos=&qi;di.enabledExtensionCount=1;di.ppEnabledExtensionNames=&de;vk(vkCreateDevice(physical,&di,nullptr,&device),"Vulkan device");vkGetDeviceQueue(device,family,0,&queue);
  VkSurfaceCapabilitiesKHR caps;vk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical,surface,&caps),"Surface capabilities");
  if(!(caps.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_DST_BIT))throw std::runtime_error("Surface does not support transfer output");
  vk(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&n,nullptr),"Formats");std::vector<VkSurfaceFormatKHR> formats(n);vk(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&n,formats.data()),"Formats");VkSurfaceFormatKHR chosen{};
  for(auto f:formats)if(f.format==VK_FORMAT_B8G8R8A8_UNORM||f.format==VK_FORMAT_R8G8B8A8_UNORM){chosen=f;break;}
  if(formats.size()==1&&formats[0].format==VK_FORMAT_UNDEFINED)chosen={VK_FORMAT_B8G8R8A8_UNORM,formats[0].colorSpace};
  if(chosen.format==VK_FORMAT_UNDEFINED)throw std::runtime_error("An 8-bit UNORM surface format is required");format=chosen.format;
  RECT cr{};GetClientRect(window,&cr);extent=caps.currentExtent;if(extent.width==UINT32_MAX)extent={std::clamp((uint32_t)cr.right,caps.minImageExtent.width,caps.maxImageExtent.width),std::clamp((uint32_t)cr.bottom,caps.minImageExtent.height,caps.maxImageExtent.height)};
  vk(vkGetPhysicalDeviceSurfacePresentModesKHR(physical,surface,&n,nullptr),"Present modes");std::vector<VkPresentModeKHR> modes(n);vk(vkGetPhysicalDeviceSurfacePresentModesKHR(physical,surface,&n,modes.data()),"Modes");VkPresentModeKHR mode=VK_PRESENT_MODE_FIFO_KHR;for(auto m:modes)if(m==VK_PRESENT_MODE_IMMEDIATE_KHR)mode=m;
  VkCompositeAlphaFlagBitsKHR alpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;for(unsigned b=1;b<=8;b<<=1)if(caps.supportedCompositeAlpha&b){alpha=(VkCompositeAlphaFlagBitsKHR)b;break;}
  uint32_t count=std::max(2u,caps.minImageCount);if(caps.maxImageCount)count=std::min(count,caps.maxImageCount);
  VkSwapchainCreateInfoKHR sc{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};sc.surface=surface;sc.minImageCount=count;sc.imageFormat=format;sc.imageColorSpace=chosen.colorSpace;sc.imageExtent=extent;sc.imageArrayLayers=1;sc.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT;sc.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;sc.preTransform=caps.currentTransform;sc.compositeAlpha=alpha;sc.presentMode=mode;sc.clipped=VK_TRUE;vk(vkCreateSwapchainKHR(device,&sc,nullptr,&swapchain),"Swapchain");
  vk(vkGetSwapchainImagesKHR(device,swapchain,&n,nullptr),"Swapchain images");images.resize(n);vk(vkGetSwapchainImagesKHR(device,swapchain,&n,images.data()),"Images");
  VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bc.size=(VkDeviceSize)extent.width*extent.height*4;bc.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT;bc.sharingMode=VK_SHARING_MODE_EXCLUSIVE;vk(vkCreateBuffer(device,&bc,nullptr,&buffer),"Upload buffer");VkMemoryRequirements req;vkGetBufferMemoryRequirements(device,buffer,&req);VkPhysicalDeviceMemoryProperties mp;vkGetPhysicalDeviceMemoryProperties(physical,&mp);uint32_t mt=UINT32_MAX;
  for(uint32_t i=0;i<mp.memoryTypeCount;++i)if((req.memoryTypeBits&(1u<<i))&&(mp.memoryTypes[i].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){mt=i;break;}
  if(mt==UINT32_MAX)throw std::runtime_error("No coherent upload memory");VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ma.allocationSize=req.size;ma.memoryTypeIndex=mt;vk(vkAllocateMemory(device,&ma,nullptr,&memory),"Upload memory");vk(vkBindBufferMemory(device,buffer,memory,0),"Bind upload");vk(vkMapMemory(device,memory,0,VK_WHOLE_SIZE,0,&mapped),"Map upload");
  VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pc.queueFamilyIndex=family;pc.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;vk(vkCreateCommandPool(device,&pc,nullptr,&pool),"Command pool");VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;vk(vkAllocateCommandBuffers(device,&ca,&command),"Command buffer");VkSemaphoreCreateInfo se{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};vk(vkCreateSemaphore(device,&se,nullptr,&available),"Acquire semaphore");vk(vkCreateSemaphore(device,&se,nullptr,&done),"Present semaphore");std::cout<<"Native NVIDIA Vulkan swapchain ready: "<<extent.width<<"x"<<extent.height<<"\n";
 }
 void present(){
  uint32_t index;VkResult result=vkAcquireNextImageKHR(device,swapchain,UINT64_MAX,available,VK_NULL_HANDLE,&index);if(result!=VK_SUCCESS&&result!=VK_SUBOPTIMAL_KHR)throw std::runtime_error("Swapchain changed: restart prototype");
  vk(vkResetCommandBuffer(command,0),"Reset command");VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;vk(vkBeginCommandBuffer(command,&begin),"Begin command");
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.image=images[index];barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
  VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={extent.width,extent.height,1};vkCmdCopyBufferToImage(command,buffer,images[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
  barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=0;vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&barrier);vk(vkEndCommandBuffer(command),"End command");
  VkPipelineStageFlags wait=VK_PIPELINE_STAGE_TRANSFER_BIT;VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.waitSemaphoreCount=1;submit.pWaitSemaphores=&available;submit.pWaitDstStageMask=&wait;submit.commandBufferCount=1;submit.pCommandBuffers=&command;submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&done;vk(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE),"Submit");
  VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};pi.waitSemaphoreCount=1;pi.pWaitSemaphores=&done;pi.swapchainCount=1;pi.pSwapchains=&swapchain;pi.pImageIndices=&index;result=vkQueuePresentKHR(queue,&pi);if(result!=VK_SUCCESS&&result!=VK_SUBOPTIMAL_KHR)throw std::runtime_error("Presentation changed: restart prototype");
  vk(vkQueueWaitIdle(queue),"Wait for upload reuse");
 }
};
int main(){try{
 SetProcessDPIAware();EnumWindows(enumerate,0);std::cout<<"Select source window number: ";size_t selected=0;std::cin>>selected;if(!selected||selected>windows.size())throw std::runtime_error("Invalid selection");HWND target=windows[selected-1];Capture capture;capture.init(target);
 std::cout<<"Output desktop X Y (must not overlap source, e.g. second monitor): ";int x,y;std::cin>>x>>y;if(!std::cin)throw std::runtime_error("Invalid coordinates");
 WNDCLASSW wc{};wc.lpfnWndProc=proc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"VulkanCapturePrototype";RegisterClassW(&wc);
 HWND output=CreateWindowExW(WS_EX_NOACTIVATE|WS_EX_TRANSPARENT|WS_EX_TOPMOST,wc.lpszClassName,L"Vulkan Capture Output",WS_POPUP,x,y,960,540,nullptr,nullptr,wc.hInstance,nullptr);if(!output)throw std::runtime_error("Output window creation failed");
 RECT outRect{x,y,x+960,y+540};if(overlap(outRect,client(target)))throw std::runtime_error("Output overlaps source. Choose another monitor or non-overlapping coordinates.");ShowWindow(output,SW_SHOWNOACTIVATE);Presenter presenter;presenter.init(output);
 std::cout<<"Output is click-through. Ctrl+Alt+Q exits. Keep original source focused.\n";RegisterHotKey(output,1,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,'Q');bool running=true;unsigned frames=0;ULONGLONG start=GetTickCount64();
 while(running){MSG message;while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){if(message.message==WM_QUIT||message.message==WM_HOTKEY)running=false;TranslateMessage(&message);DispatchMessageW(&message);}if(!running)break;if(!IsWindow(target))throw std::runtime_error("Source closed");if(overlap(outRect,client(target)))throw std::runtime_error("Source moved under output; capture stopped to prevent feedback");if(capture.frame(target,presenter.mapped,presenter.extent.width,presenter.extent.height,presenter.format==VK_FORMAT_R8G8B8A8_UNORM)){presenter.present();++frames;}if(GetTickCount64()-start>=2000){std::cout<<"Captured/presented fps: "<<(frames*1000.0/(GetTickCount64()-start))<<"\n";frames=0;start=GetTickCount64();}}
 UnregisterHotKey(output,1);return 0;
 }catch(const std::exception& e){std::cerr<<"Error: "<<e.what()<<"\n";std::cout<<"Press Enter to close.\n";std::cin.clear();std::cin.ignore(10000,'\n');std::cin.get();return 1;}}
