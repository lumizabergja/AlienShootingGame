#pragma once
#include <windows.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <stdexcept>
#include <vector>
#include <functional>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <cmath>
template<class T> using Com = Microsoft::WRL::ComPtr<T>;
inline void check(HRESULT r,const char* what){if(FAILED(r)){char b[128];sprintf_s(b,"%s (0x%08X)",what,unsigned(r));throw std::runtime_error(b);}}
inline void require(bool ok,const char* msg){if(!ok)throw std::runtime_error(msg);}
inline std::wstring widen(const std::string& s){int n=MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,nullptr,0);std::wstring w(n,L'\0');MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,w.data(),n);if(n)w.pop_back();return w;}
inline std::wstring appDir(){wchar_t p[32768];GetModuleFileNameW(nullptr,p,32768);std::wstring s(p);return s.substr(0,s.find_last_of(L"\\/"));}
struct Handle{HANDLE h{};Handle()=default;explicit Handle(HANDLE v):h(v){};~Handle(){if(h&&h!=INVALID_HANDLE_VALUE)CloseHandle(h);}Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;};
inline void barrier(ID3D12GraphicsCommandList* c,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){if(a==b)return;D3D12_RESOURCE_BARRIER x{};x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;x.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};c->ResourceBarrier(1,&x);}
struct Config{DWORD pid{};HWND game{};unsigned multiplier=2;};
using Status=std::function<void(const std::wstring&)>;
void runEngine(Config,std::atomic_bool&,Status);
