#include "capture.h"
#include <d3d9.h>
namespace {
using U=uint32_t;using Ptr=unsigned long long;
constexpr U ver(size_t n,U r){return U(n)|(r<<16)|0x70000000u;}
struct Create{U version,type,maxWidth,maxHeight;void* device;void* private1;U privateSize,interfaceVersion;void* object;U adapter,sdk;void* context;void* private2;U privateSize2,reserved[55];void* pointers[27];};
struct Pid{U version,enabled,pid,reserved[47];};
struct Setup{U version,flags;void* cursorEvent;U format,reserved[61];void* pointers[31];};
struct Info{U width,height,bufferWidth,reserved;BOOL overlay,recreate,first,mouse,protectedContent;U driverError;BOOL stereo,igpu;U pid,reserved3,hdrBits,waitMode,reserved2[11];};
struct Grab{U version,flags;void* buffer;Info* info;U timeout,reserved[61];void* pointers[30];};
static_assert(sizeof(Create)==512&&sizeof(Pid)==200&&sizeof(Setup)==512&&sizeof(Info)==108&&sizeof(Grab)==512);
struct Session{virtual int __stdcall maxBuffer(U*)=0;virtual int __stdcall setup(Setup*)=0;virtual int __stdcall grab(Grab*)=0;virtual int __stdcall sleep(long long)=0;virtual int __stdcall cursor(void*)=0;virtual int __stdcall release()=0;};
// CUDA driver external resource descriptors, x64 ABI. No CUDA runtime dependency.
struct MemoryDesc{int type;struct{void* handle;const void* name;} win32;uint64_t size;U flags,reserved[16];};
struct BufferDesc{uint64_t offset,size;U flags,reserved[16];};
struct SemDesc{int type;struct{void* handle;const void* name;}win32;U flags,reserved[16];};
struct Signal{struct{uint64_t value,nvSciSync,keyedMutex;U reserved[12];}params;U flags,reserved[16];};
static_assert(sizeof(MemoryDesc)==104&&sizeof(BufferDesc)==88&&sizeof(SemDesc)==96&&sizeof(Signal)==144);
template<class T>T sym(HMODULE m,const char* s){auto p=reinterpret_cast<T>(GetProcAddress(m,s));require(p!=nullptr,s);return p;}
void cu(int r,const char* s){if(r)throw std::runtime_error(std::string(s)+": CUDA "+std::to_string(r));}
void nv(int r,const char* s){if(r)throw std::runtime_error(std::string(s)+": NvFBC "+std::to_string(r));}
}
struct Capture::Impl {
 HMODULE cuda{},fbc{};void* ctx{};int dev{};Session* session{};DWORD pid{};U bytes{},w{},h{},stride{};LUID adapterLuid{};
 Com<IDirect3D9Ex>d9;Com<IDirect3DDevice9Ex>d9device;Com<ID3D12Fence>fence;void* memory{};void* sem{};Ptr pointer{};uint64_t value{};
 int(WINAPI*setCtx)(void*){};int(WINAPI*destroyCtx)(void*){};int(WINAPI*memFree)(Ptr){};
 int(WINAPI*destroyMem)(void*){};int(WINAPI*destroySem)(void*){};
 int(WINAPI*signal)(const void*const*,const Signal*,U,void*){};
 void accept(Info i){require(!i.protectedContent,"Source is protected");require(i.pid==pid,"NvFBC returned a different process; capture stopped");require(!(i.hdrBits&1),"Use SDR for this build");require(i.width&&i.height&&i.bufferWidth>=i.width&&uint64_t(i.bufferWidth)*4*i.height<=bytes,"Invalid capture dimensions or pitch");if(w)require(w==i.width&&h==i.height&&stride==i.bufferWidth*4,"Game resolution changed. Stop and Start to rebuild capture resources.");w=i.width;h=i.height;stride=i.bufferWidth*4;}
 void acquire(){cu(setCtx(ctx),"cuCtxSetCurrent");Info info{};Grab g{};g.version=ver(sizeof(g),1);g.flags=0x10;g.buffer=reinterpret_cast<void*>(pointer);g.info=&info;g.timeout=100;nv(session->grab(&g),"NvFBCCudaGrabFrame");require(!info.recreate,"Capture session invalidated. Stop and Start to reconnect.");accept(info);}
 ~Impl(){if(ctx){setCtx(ctx);if(session)session->release();if(sem)destroySem(sem);if(pointer)memFree(pointer);if(memory)destroyMem(memory);destroyCtx(ctx);}if(fbc)FreeLibrary(fbc);if(cuda)FreeLibrary(cuda);}
};
Capture::Capture():p(std::make_unique<Impl>()){}Capture::~Capture()=default;
void Capture::open(HWND game,DWORD pid){auto& a=*p;a.pid=pid;
 a.cuda=LoadLibraryExW(L"nvcuda.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);a.fbc=LoadLibraryExW(L"NvFBC64.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);require(a.cuda&&a.fbc,"NVIDIA CUDA/NvFBC driver interface unavailable");
 a.setCtx=sym<decltype(a.setCtx)>(a.cuda,"cuCtxSetCurrent");a.destroyCtx=sym<decltype(a.destroyCtx)>(a.cuda,"cuCtxDestroy_v2");a.memFree=sym<decltype(a.memFree)>(a.cuda,"cuMemFree_v2");a.destroyMem=sym<decltype(a.destroyMem)>(a.cuda,"cuDestroyExternalMemory");a.destroySem=sym<decltype(a.destroySem)>(a.cuda,"cuDestroyExternalSemaphore");a.signal=sym<decltype(a.signal)>(a.cuda,"cuSignalExternalSemaphoresAsync");
 auto init=sym<int(WINAPI*)(U)>(a.cuda,"cuInit");cu(init(0),"cuInit");
 check(Direct3DCreate9Ex(D3D_SDK_VERSION,&a.d9),"D3D9 context adapter");UINT ordinal=UINT_MAX;auto mon=MonitorFromWindow(game,MONITOR_DEFAULTTONEAREST);for(UINT i=0;i<a.d9->GetAdapterCount();++i)if(a.d9->GetAdapterMonitor(i)==mon){ordinal=i;break;}require(ordinal!=UINT_MAX,"Capture monitor adapter unavailable");
 check(a.d9->GetAdapterLUID(ordinal,&a.adapterLuid),"Adapter LUID");D3DPRESENT_PARAMETERS pp{};pp.BackBufferWidth=16;pp.BackBufferHeight=16;pp.BackBufferFormat=D3DFMT_X8R8G8B8;pp.BackBufferCount=1;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.Windowed=TRUE;pp.hDeviceWindow=GetDesktopWindow();pp.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
 check(a.d9->CreateDeviceEx(ordinal,D3DDEVTYPE_HAL,pp.hDeviceWindow,D3DCREATE_HARDWARE_VERTEXPROCESSING|D3DCREATE_MULTITHREADED|D3DCREATE_FPU_PRESERVE,&pp,nullptr,&a.d9device),"D3D9 context device");
 auto createCtx=sym<int(WINAPI*)(void**,int*,U,IDirect3DDevice9*)>(a.cuda,"cuD3D9CtxCreate_v2");cu(createCtx(&a.ctx,&a.dev,0,a.d9device.Get()),"CUDA capture context");
 auto create=sym<int(WINAPI*)(Create*)>(a.fbc,"NvFBC_CreateEx");Create c{};c.version=ver(sizeof(c),2);c.type=0x1007;c.device=a.d9device.Get();c.context=a.ctx;c.adapter=ordinal;U key[]={0xaef57ac5,0x401d1a39,0x1b856bbe,0x9ed0ceba};c.private1=key;c.privateSize=sizeof(key);Pid target{};target.version=ver(sizeof(target),1);target.enabled=1;target.pid=pid;c.private2=&target;c.privateSize2=sizeof(target);nv(create(&c),"NvFBC CUDA PID session");require(c.object,"NvFBC returned no session");a.session=static_cast<Session*>(c.object);nv(a.session->maxBuffer(&a.bytes),"NvFBC buffer size");require(a.bytes>0,"Empty capture allocation");Setup setup{};setup.version=ver(sizeof(setup),1);setup.format=0;nv(a.session->setup(&setup),"NvFBC setup");
 // Probe before loading Streamline: establishes authoritative source dimensions.
 auto alloc=sym<int(WINAPI*)(Ptr*,size_t)>(a.cuda,"cuMemAlloc_v2");cu(alloc(&a.pointer,a.bytes),"Capture preflight allocation");a.acquire();cu(a.memFree(a.pointer),"Capture preflight free");a.pointer=0;
}
void Capture::bind(ID3D12Device* d,ID3D12Resource** out){auto& a=*p;auto luid=d->GetAdapterLuid();require(!memcmp(&luid,&a.adapterLuid,sizeof(luid)),"CUDA/D3D12 adapter mismatch");
 D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rd.Width=(uint64_t(a.bytes)+255)&~255ull;rd.Height=1;rd.DepthOrArraySize=rd.MipLevels=1;rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;check(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_SHARED,&rd,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(out)),"Shared capture buffer");
 HANDLE h{};check(d->CreateSharedHandle(*out,nullptr,GENERIC_ALL,nullptr,&h),"Capture handle");Handle handle(h);MemoryDesc md{};md.type=5;md.win32.handle=h;md.size=d->GetResourceAllocationInfo(0,1,&rd).SizeInBytes;md.flags=1;
 auto importMem=sym<int(WINAPI*)(void**,const MemoryDesc*)>(a.cuda,"cuImportExternalMemory");cu(importMem(&a.memory,&md),"CUDA import D3D12 memory");auto map=sym<int(WINAPI*)(Ptr*,void*,const BufferDesc*)>(a.cuda,"cuExternalMemoryGetMappedBuffer");BufferDesc bd{};bd.size=rd.Width;cu(map(&a.pointer,a.memory,&bd),"CUDA map D3D12 buffer");
 check(d->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&a.fence)),"Shared capture fence");HANDLE fh{};check(d->CreateSharedHandle(a.fence.Get(),nullptr,GENERIC_ALL,nullptr,&fh),"Capture fence handle");Handle fenceHandle(fh);SemDesc sd{};sd.type=4;sd.win32.handle=fh;auto importSem=sym<int(WINAPI*)(void**,const SemDesc*)>(a.cuda,"cuImportExternalSemaphore");cu(importSem(&a.sem,&sd),"CUDA import capture semaphore");
}
void Capture::grab(){auto& a=*p;a.acquire();Signal s{};s.params.value=++a.value;const void* sem=a.sem;cu(a.signal(&sem,&s,1,nullptr),"CUDA signal capture completion");}
LUID Capture::luid()const{return p->adapterLuid;}unsigned Capture::width()const{return p->w;}unsigned Capture::height()const{return p->h;}unsigned Capture::pitch()const{return p->stride;}ID3D12Fence* Capture::readyFence()const{return p->fence.Get();}uint64_t Capture::readyValue()const{return p->value;}
