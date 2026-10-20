#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <sstream>
#include <chrono>

namespace fs=std::filesystem;
static constexpr int GAMES=101,REFRESH=102,START=103,STOP=104,FOV=105,WGC=106,HUD=107,CROSS=108,RADIUS=109,STATUS=110,CURSOR=111,VSYNC=112;
static const wchar_t* STOP_EVENT=L"Local\\FrameWarpCustomCoreStop";
static HWND mainWindow{}, gameBox{}, statusText{}, startButton{}, stopButton{};
static PROCESS_INFORMATION core{};
static HANDLE stopEvent{};
static HFONT font{};
static std::vector<HWND> windows;
static std::wstring baseDir;
static bool active=false;
static std::wstring selectedTitle;

static std::wstring Dir(){
 wchar_t path[MAX_PATH]={};GetModuleFileNameW(nullptr,path,MAX_PATH);
 return fs::path(path).parent_path().wstring();
}
static HWND Add(const wchar_t* cls,const wchar_t* text,DWORD style,int x,int y,int w,int h,HWND parent,int id=0,DWORD ex=0){
 HWND n=CreateWindowExW(ex,cls,text,style|WS_CHILD|WS_VISIBLE,x,y,w,h,parent,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
 if(font)SendMessageW(n,WM_SETFONT,(WPARAM)font,TRUE);
 return n;
}
static std::wstring WindowTitle(HWND h){
 wchar_t b[512]{};GetWindowTextW(h,b,511);return b;
}
static BOOL CALLBACK EnumGames(HWND h,LPARAM){
 if(!IsWindowVisible(h)||IsIconic(h)||GetWindowTextLengthW(h)<2)return TRUE;
 if((GetWindowLongPtrW(h,GWL_EXSTYLE)&WS_EX_TOOLWINDOW)||GetWindow(h,GW_OWNER))return TRUE;
 DWORD p=0;GetWindowThreadProcessId(h,&p);
 if(p==GetCurrentProcessId())return TRUE;
 DWORD cloaked=0;
 if(SUCCEEDED(DwmGetWindowAttribute(h,DWMWA_CLOAKED,&cloaked,sizeof(cloaked)))&&cloaked)return TRUE;
 RECT r{};GetClientRect(h,&r);
 if(r.right<320||r.bottom<200)return TRUE;
 windows.push_back(h);return TRUE;
}
static void RefreshWindows(){
 HWND last=nullptr;int selected=(int)SendMessageW(gameBox,CB_GETCURSEL,0,0);
 if(selected>=0&&selected<(int)windows.size())last=windows[selected];
 windows.clear();
 SendMessageW(gameBox,CB_RESETCONTENT,0,0);
 EnumWindows(EnumGames,0);
 int select=0;
 for(size_t i=0;i<windows.size();++i){
   const std::wstring label=WindowTitle(windows[i])+L"  [PID "+[&](){DWORD id=0;GetWindowThreadProcessId(windows[i],&id);return std::to_wstring(id);}()+L"]";
   SendMessageW(gameBox,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
   if(windows[i]==last)select=(int)i;
   if(!last && label.find(L"Counter-Strike")!=std::wstring::npos)select=(int)i;
 }
 if(!windows.empty())SendMessageW(gameBox,CB_SETCURSEL,select,0);
}
static void Status(const std::wstring& message){if(statusText)SetWindowTextW(statusText,message.c_str());}
static int IntFrom(HWND h,int id,int def,int min,int max){
 wchar_t b[32]{};GetWindowTextW(GetDlgItem(h,id),b,31);
 int v=_wtoi(b);if(v<min||v>max)v=def;return v;
}
static bool Checked(HWND h,int id){return SendDlgItemMessageW(h,id,BM_GETCHECK,0,0)==BST_CHECKED;}
static std::wstring IniSafe(std::wstring s){
 // Reject INI injection through window titles.
 for(auto& c:s)if(c==L'\r'||c==L'\n'||c==L'['||c==L']'||c==L'='||c==L';')c=L' ';
 return s;
}
static void WriteUTF16Ini(const fs::path& p,const std::wstring& data){
 // GetPrivateProfileStringW understands little-endian UTF-16 INI.
 std::ofstream f(p,std::ios::binary|std::ios::trunc);
 if(!f)throw std::runtime_error("Cannot create configuration file");
 const unsigned char bom[2]={0xff,0xfe};f.write((const char*)bom,2);
 f.write((const char*)data.data(),(std::streamsize)(data.size()*sizeof(wchar_t)));
 if(!f)throw std::runtime_error("Cannot write configuration");
}
static void WriteConfiguration(HWND window,HWND target){
 fs::create_directories(fs::path(baseDir)/L"profiles");
 wchar_t fovText[48]{},radiusText[48]{};
 const int vfov=IntFrom(window,FOV,60,30,120);
 const bool hud=Checked(window,HUD),cursor=Checked(window,CURSOR);
 const bool cross=Checked(window,CROSS),vsync=Checked(window,VSYNC);
 const int radius=IntFrom(window,RADIUS,32,8,128);
 const auto app=std::wstring(L"[app]\nprofile=none\n[gpu]\nadapter=-1\n[nr]\nenabled=0\nparam_block=1\n[filters]\nenabled=0\n[fg]\nenabled=1\n[ui]\n")+
     L"hud="+(hud?std::wstring(L"1"):std::wstring(L"0"))+L"\n"+
     L"toast=0\n[overlay]\nmode=composed\nexclude_from_capture=0\n[hotkeys]\nquit=Ctrl+F12\n[log]\nselftest=0\nstats_every=180\n";
 WriteUTF16Ini(fs::path(baseDir)/L"justflow.ini",app);
 std::wstring name=IniSafe(WindowTitle(target));
 RECT rect{};GetClientRect(target,&rect);
 int w=std::max(640L,rect.right),h=std::max(480L,rect.bottom);
 auto profile=std::wstring(L"[capture]\nmode=wgc\nwindow_title=")+name+
  L"\nwindow_class=\ncursor="+(cursor?std::wstring(L"1"):std::wstring(L"0"))+
  L"\nborder=0\ndesktop=0\n[nr]\nwork=50%\n[ofa]\ninput=960x540\ngrid=auto\nperf=fast\n[fg]\nengine=latewarp\nmultiplier=2\nlw_fov="+std::to_wstring(vfov)+
  L"\npacing="+(vsync?std::wstring(L"vblank"):std::wstring(L"timer"))+
  L"\nmin_gain=0\nmax_input_fps=0\nphase_ms=0\n[ui]\nmask=1\nfeather=4\n";
 if(cross){
   const int x=w/2,y=h/2;
   profile+=L"rect1="+std::to_wstring(x-radius)+L","+std::to_wstring(y-radius)+L","+
       std::to_wstring(x+radius)+L","+std::to_wstring(y+radius)+L"\n";
 }
 profile+=L"[log]\nfile=FrameWarpCustom.log\n";
 WriteUTF16Ini(fs::path(baseDir)/L"profiles"/L"custom.ini",profile);
 selectedTitle=name;
}
static void CloseHandles(){
 if(core.hThread){CloseHandle(core.hThread);core.hThread=nullptr;}
 if(core.hProcess){CloseHandle(core.hProcess);core.hProcess=nullptr;}
 if(stopEvent){CloseHandle(stopEvent);stopEvent=nullptr;}
 active=false;
}
static void Stop(){
 if(!active){CloseHandles();return;}
 Status(L"Stopping Frame Warp and releasing capture...");
 if(stopEvent)SetEvent(stopEvent);
 if(core.hProcess && WaitForSingleObject(core.hProcess,6000)==WAIT_TIMEOUT){
   // Fail-safe: prevent the old process holding a fullscreen overlay forever.
   TerminateProcess(core.hProcess,1);
   WaitForSingleObject(core.hProcess,2000);
 }
 CloseHandles();
 EnableWindow(startButton,TRUE);EnableWindow(stopButton,FALSE);
 Status(L"Stopped. Select a game and press Start.");
}
static void Start(HWND h){
 if(active)return;
 int idx=(int)SendMessageW(gameBox,CB_GETCURSEL,0,0);
 if(idx<0||idx>=(int)windows.size()||!IsWindow(windows[idx])){
  Status(L"Select a running game or press Refresh.");return;
 }
 auto exe=(fs::path(baseDir)/L"FrameWarpCore.exe").wstring();
 if(!fs::exists(exe)){Status(L"FrameWarpCore.exe is missing from the ZIP.");return;}
 if(!fs::exists(fs::path(baseDir)/L"nvngx_latewarp.dll")){Status(L"Missing NVIDIA-signed nvngx_latewarp.dll.");return;}
 try{WriteConfiguration(h,windows[idx]);}
 catch(const std::exception&){Status(L"Could not write the Frame Warp configuration. Check folder permissions.");return;}
 stopEvent=CreateEventW(nullptr,TRUE,FALSE,STOP_EVENT);
 if(!stopEvent){Status(L"Cannot create Frame Warp stop event.");return;}
 std::wstring cmd=L"\""+exe+L"\" --ini profiles\\custom.ini";
 STARTUPINFOW si{};si.cb=sizeof(si);
 ZeroMemory(&core,sizeof(core));
 if(!CreateProcessW(exe.c_str(),cmd.data(),nullptr,nullptr,FALSE,
                    CREATE_NO_WINDOW,nullptr,baseDir.c_str(),&si,&core)){
   DWORD error=GetLastError();CloseHandles();
   Status(L"Start failed. Windows error "+std::to_wstring(error));return;
 }
 active=true;EnableWindow(startButton,FALSE);EnableWindow(stopButton,TRUE);
 Status(L"Starting calibrated NVIDIA Frame Warp for "+selectedTitle+L"...\nCalibration begins when you move the camera.");
}
static std::wstring ReadStatus(){
 fs::path p=fs::path(baseDir)/L"FrameWarpCustom.log";
 std::ifstream f(p,std::ios::binary);
 if(!f)return L"Starting NVIDIA Frame Warp. Waiting for game capture...";
 f.seekg(0,std::ios::end);auto len=f.tellg();
 if(len<0)return L"Reading status...";
 size_t n=(size_t)len;
 size_t start=n>8000?n-8000:0;
 f.seekg((std::streamoff)start,std::ios::beg);
 std::string s(n-start,'\0');f.read(s.data(),(std::streamsize)s.size());
 if(s.find("[fg] disabled")!=std::string::npos||s.find("latewarp evaluate")!=std::string::npos)
   return L"Frame Warp failed. See FrameWarpCustom.log (same folder).";
 if(s.find("[latewarp] feature created")!=std::string::npos){
    if(s.find("[mouse] model:")!=std::string::npos)return L"NVIDIA Frame Warp running; mouse calibration progressing. See FrameWarpCustom.log.";
    return L"NVIDIA Frame Warp initialized. Move camera to let optical-flow mouse calibration learn.";
 }
 return L"Capturing "+selectedTitle+L". Waiting for NVIDIA Frame Warp initialization...";
}
static LRESULT CALLBACK Proc(HWND h,UINT m,WPARAM w,LPARAM l){
 switch(m){
  case WM_COMMAND:
    switch(LOWORD(w)){
     case REFRESH:RefreshWindows();return 0;
     case START:Start(h);return 0;
     case STOP:Stop();return 0;
     default:break;
    }break;
  case WM_TIMER:
    if(w==1){
     if(active){
      if(!core.hProcess||WaitForSingleObject(core.hProcess,0)==WAIT_OBJECT_0){
       DWORD exitCode=0;if(core.hProcess)GetExitCodeProcess(core.hProcess,&exitCode);
       CloseHandles();EnableWindow(startButton,TRUE);EnableWindow(stopButton,FALSE);
       Status(L"Frame Warp stopped (code "+std::to_wstring(exitCode)+L"). Check FrameWarpCustom.log.");
      }else Status(ReadStatus());
     }
     return 0;
    }break;
  case WM_CLOSE:Stop();DestroyWindow(h);return 0;
  case WM_DESTROY:PostQuitMessage(0);return 0;
 }
 return DefWindowProcW(h,m,w,l);
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int show){
 SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
 HANDLE singleton=CreateMutexW(nullptr,TRUE,L"Local\\FrameWarpCustomController20261020");
 if(!singleton||GetLastError()==ERROR_ALREADY_EXISTS){
  MessageBoxW(nullptr,L"FrameWarp Custom is already open.",L"FrameWarp Custom",MB_OK|MB_ICONINFORMATION);
  if(singleton)CloseHandle(singleton);return 0;
 }
 baseDir=Dir();
 font=(HFONT)GetStockObject(DEFAULT_GUI_FONT);
 WNDCLASSW c{};c.hInstance=instance;c.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
 c.hCursor=LoadCursor(nullptr,IDC_ARROW);c.lpfnWndProc=Proc;c.lpszClassName=L"FrameWarpCustomController";
 RegisterClassW(&c);
 mainWindow=CreateWindowW(c.lpszClassName,L"FrameWarp Custom — Calibrated NVIDIA Frame Warp",
     WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,CW_USEDEFAULT,CW_USEDEFAULT,570,450,
     nullptr,nullptr,instance,nullptr);
 Add(L"STATIC",L"Game / Window",WS_CHILD,20,17,300,20,mainWindow);
 gameBox=Add(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL,20,41,413,180,mainWindow,GAMES);
 Add(L"BUTTON",L"Refresh",BS_PUSHBUTTON,440,40,104,28,mainWindow,REFRESH);
 Add(L"STATIC",L"Vertical FOV",WS_CHILD,20,92,200,20,mainWindow);
 Add(L"EDIT",L"60",WS_BORDER|ES_NUMBER,20,116,100,25,mainWindow,FOV);
 Add(L"STATIC",L"Crosshair radius (px)",WS_CHILD,160,92,200,20,mainWindow);
 Add(L"EDIT",L"32",WS_BORDER|ES_NUMBER,160,116,100,25,mainWindow,RADIUS);
 Add(L"BUTTON",L"Protect crosshair (native center)",BS_AUTOCHECKBOX,20,158,326,24,mainWindow,CROSS);
 SendDlgItemMessageW(mainWindow,CROSS,BM_SETCHECK,BST_CHECKED,0);
 Add(L"BUTTON",L"Capture cursor",BS_AUTOCHECKBOX,20,187,150,24,mainWindow,CURSOR);
 Add(L"BUTTON",L"Show backend HUD",BS_AUTOCHECKBOX,205,187,190,24,mainWindow,HUD);
 Add(L"BUTTON",L"VBlank pacing (vs timer)",BS_AUTOCHECKBOX,20,217,260,24,mainWindow,VSYNC);
 SendDlgItemMessageW(mainWindow,VSYNC,BM_SETCHECK,BST_CHECKED,0);
 startButton=Add(L"BUTTON",L"Start NVIDIA Frame Warp",BS_DEFPUSHBUTTON,20,256,260,38,mainWindow,START);
 stopButton=Add(L"BUTTON",L"Stop",BS_PUSHBUTTON,289,256,254,38,mainWindow,STOP);
 EnableWindow(stopButton,FALSE);
 statusText=Add(L"STATIC",L"Select a running game and press Start.",WS_CHILD,20,306,510,47,mainWindow,STATUS);
 Add(L"STATIC",L"Auto mouse calibration from GPU optical flow • WGC • DX12 • Click-through",WS_CHILD,20,367,540,29,mainWindow);
 RefreshWindows();
 SetTimer(mainWindow,1,1000,nullptr);
 ShowWindow(mainWindow,show);UpdateWindow(mainWindow);
 MSG msg{};
 while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
 Stop();
 ReleaseMutex(singleton);CloseHandle(singleton);
 return (int)msg.wParam;
}
