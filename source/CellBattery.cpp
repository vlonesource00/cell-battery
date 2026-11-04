// Cell: native, event-driven, portable Windows battery widget. C++17, Windows 10+.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <windowsx.h>
#include <winioctl.h>
#include <setupapi.h>
#include <poclass.h>
#include <powrprof.h>
#include <wtsapi32.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <sddl.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

using namespace Gdiplus;
using std::wstring;
// One card for every tab (logical px, scaled only by DPI): header, tab bar, content.
constexpr int CW=240, CH=386;
constexpr float CHART_X=12, CHART_Y=304, CHART_W=216, CHART_H=52, LEGEND_Y=364;
constexpr uint64_t DAY=86400, HISTORY_DAYS=7;
constexpr size_t MAX_HISTORY=12000, MAX_ENERGY=16384;
constexpr uint64_t UNATTRIBUTED_ID=UINT64_MAX;
constexpr UINT WM_TRAY=WM_APP+1, WM_IMPORTED=WM_APP+2, WM_RECREATE=WM_APP+3;
constexpr UINT_PTR SAMPLE_TIMER=1, DESKTOP_RETRY_TIMER=2, ANIM_TIMER=3, PILL_TIMER=4;
// PILL_INTERVAL_MS keeps the taskbar pill current while the card itself is hidden.
constexpr UINT LIVE_INTERVAL_MS=2000, PILL_INTERVAL_MS=15000, HIDDEN_INTERVAL_MS=300000;
const wchar_t* WINDOW_CLASS=L"CellBatteryWidget1";
const wchar_t* CONTROL_CLASS=L"CellBatteryControl1";
enum Source : uint8_t { Battery=0, Charging=1, Plugged=2, SourceUnknown=3 };
enum Action { Expand=100, Menu, Refresh, FilterBattery, FilterPlugged, FilterAll,
              ClearHour, PreviousPage, NextPage, Show=200, Topmost, Startup,
              Settings, DataFolder, Hide, Quit, BatteryReadings, DesktopPin, FilterLive=300,
              TabBattery=400, TabApps, TabNow, TabWeek, TaskbarPill=500, AlertUnplug, AlertLow, AlertDrain, SizeSmall=520, SizeMedium, SizeLarge };
struct Power {
    bool valid=false, present=false, saver=false;
    int percent=-1;
    Source source=SourceUnknown;
    uint32_t seconds=UINT32_MAX;
    bool windowsEta=false;
    double watts=0;
    double reportedWatts=0;
    bool recentDrain=false;
    uint32_t drainSpan=0;
    uint32_t capacityMWh=UINT32_MAX;
    uint32_t fullSeconds=UINT32_MAX;
    uint32_t remainingMWh=UINT32_MAX,fullMWh=UINT32_MAX;
    bool drainCapable=false;
};
// Static-ish battery facts. Queried rarely (startup, resume, source change, hourly).
struct BatteryFacts {
    bool valid=false;
    uint32_t designMWh=0,fullMWh=0,cycles=0;
    uint64_t readAt=0;
    int HealthPercent() const {return valid&&designMWh&&fullMWh?int(std::lround(100.0*fullMWh/designMWh)):-1;}
};
// Bounded, in-memory fuel-gauge trend. Never carry a trend across charging,
// capacity recalibration, battery replacement, or a pause in observations.
struct DrainTrend {
    struct Point { uint64_t tick; uint32_t capacity; };
    std::array<Point,80> points{};
    size_t count=0;
    void Reset(){count=0;}
    bool Add(uint64_t tick,uint32_t capacity,double& watts,uint32_t& span){
        watts=0;span=0;
        if(capacity==UINT32_MAX||capacity==0){Reset();return false;}
        if(count){
            auto last=points[count-1];
            if(tick<last.tick||tick-last.tick>10000||capacity>last.capacity)Reset();
            else if(tick-last.tick<1000)return Estimate(watts,span);
        }
        size_t expired=0;
        while(expired<count&&tick-points[expired].tick>120000)++expired;
        if(expired){std::move(points.begin()+expired,points.begin()+count,points.begin());count-=expired;}
        if(count==points.size()){std::move(points.begin()+1,points.end(),points.begin());--count;}
        points[count++]={tick,capacity};return Estimate(watts,span);
    }
    bool Estimate(double& watts,uint32_t& span) const {
        watts=0;span=0;if(count<3)return false;
        const auto& first=points[0];const auto& last=points[count-1];
        uint64_t duration=last.tick-first.tick;
        if(duration<60000||first.capacity-last.capacity<30)return false;
        unsigned changes=0;double sx=0,sy=0,sxx=0,sxy=0;
        for(size_t i=0;i<count;i++){
            if(i&&points[i].capacity<points[i-1].capacity)++changes;
            double x=(points[i].tick-first.tick)/1000.0,y=double(points[i].capacity)-first.capacity;
            sx+=x;sy+=y;sxx+=x*x;sxy+=x*y;
        }
        if(changes<4)return false; // A coarse, unchanged counter cannot resolve live draw.
        double divisor=count*sxx-sx*sx;if(divisor<=0)return false;
        double rate=-3.6*(count*sxy-sx*sy)/divisor;
        if(!std::isfinite(rate)||rate<0.2||rate>250)return false;
        watts=rate;span=uint32_t(duration/1000);return true;
    }
};
struct BatteryMeter {
    HANDLE device=INVALID_HANDLE_VALUE;ULONG tag=0;uint64_t retryAt=0;
    DrainTrend trend;
    void Close(){if(device!=INVALID_HANDLE_VALUE)CloseHandle(device);device=INVALID_HANDLE_VALUE;tag=0;trend.Reset();}
    ~BatteryMeter(){Close();}
    bool Open(uint64_t tick){
        if(device!=INVALID_HANDLE_VALUE)return true;if(tick<retryAt)return false;
        retryAt=tick+300000;
        const GUID cls={0x72631e54,0x78a4,0x11d0,{0xbc,0xf7,0,0xaa,0,0xb7,0xb3,0x2a}};
        HDEVINFO set=SetupDiGetClassDevsW(&cls,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
        if(set==INVALID_HANDLE_VALUE)return false;
        unsigned batteries=0;bool incomplete=false;
        for(DWORD i=0;i<16;i++){
            SP_DEVICE_INTERFACE_DATA iface{};iface.cbSize=sizeof(iface);
            if(!SetupDiEnumDeviceInterfaces(set,nullptr,&cls,i,&iface)){if(GetLastError()!=ERROR_NO_MORE_ITEMS)incomplete=true;break;}
            DWORD size=0;SetupDiGetDeviceInterfaceDetailW(set,&iface,nullptr,0,&size,nullptr);
            if(size<sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)||size>65536){incomplete=true;continue;}
            std::vector<BYTE> buffer(size);auto* detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());detail->cbSize=sizeof(*detail);
            if(!SetupDiGetDeviceInterfaceDetailW(set,&iface,detail,size,nullptr,nullptr)){incomplete=true;continue;}
            HANDLE h=CreateFileW(detail->DevicePath,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(h==INVALID_HANDLE_VALUE){incomplete=true;continue;}
            ULONG t=0;DWORD wait=0,returned=0;BATTERY_INFORMATION info{};BATTERY_QUERY_INFORMATION query{};
            bool ok=DeviceIoControl(h,IOCTL_BATTERY_QUERY_TAG,&wait,sizeof(wait),&t,sizeof(t),&returned,nullptr)&&t;
            query.BatteryTag=t;query.InformationLevel=BatteryInformation;
            ok=ok&&DeviceIoControl(h,IOCTL_BATTERY_QUERY_INFORMATION,&query,sizeof(query),&info,sizeof(info),&returned,nullptr);
            if(!ok)incomplete=true;
            else if(info.Capabilities&BATTERY_SYSTEM_BATTERY){
                ++batteries;
                if(!(info.Capabilities&BATTERY_CAPACITY_RELATIVE)&&device==INVALID_HANDLE_VALUE){device=h;tag=t;h=INVALID_HANDLE_VALUE;}
            }
            if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);
        }
        SetupDiDestroyDeviceInfoList(set);
        // This estimator needs one complete absolute-capacity system battery.
        // Other configurations keep Windows' aggregate reading without guessing.
        if(incomplete||batteries!=1)Close();return device!=INVALID_HANDLE_VALUE;
    }
    bool Capacity(uint64_t tick,uint32_t& capacity){
        if(!Open(tick))return false;
        BATTERY_WAIT_STATUS wait{};wait.BatteryTag=tag;BATTERY_STATUS status{};DWORD returned=0;
        if(!DeviceIoControl(device,IOCTL_BATTERY_QUERY_STATUS,&wait,sizeof(wait),&status,sizeof(status),&returned,nullptr)){
            Close();retryAt=tick+60000;return false;
        }
        if(!(status.PowerState&BATTERY_DISCHARGING)||(status.PowerState&(BATTERY_CHARGING|BATTERY_POWER_ON_LINE))||status.Capacity==UINT32_MAX){trend.Reset();return false;}
        capacity=status.Capacity;return true;
    }
} meter;
// Sums design and full-charge capacity across absolute-capacity system batteries.
// Opens and closes handles immediately; nothing stays resident.
BatteryFacts ReadBatteryFacts(){
    BatteryFacts facts;
    const GUID cls={0x72631e54,0x78a4,0x11d0,{0xbc,0xf7,0,0xaa,0,0xb7,0xb3,0x2a}};
    HDEVINFO set=SetupDiGetClassDevsW(&cls,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
    if(set==INVALID_HANDLE_VALUE)return facts;
    for(DWORD i=0;i<16;i++){
        SP_DEVICE_INTERFACE_DATA iface{};iface.cbSize=sizeof(iface);
        if(!SetupDiEnumDeviceInterfaces(set,nullptr,&cls,i,&iface))break;
        DWORD size=0;SetupDiGetDeviceInterfaceDetailW(set,&iface,nullptr,0,&size,nullptr);
        if(size<sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)||size>65536)continue;
        std::vector<BYTE> buffer(size);auto* detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());detail->cbSize=sizeof(*detail);
        if(!SetupDiGetDeviceInterfaceDetailW(set,&iface,detail,size,nullptr,nullptr))continue;
        HANDLE h=CreateFileW(detail->DevicePath,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(h==INVALID_HANDLE_VALUE)continue;
        ULONG t=0;DWORD wait=0,returned=0;BATTERY_INFORMATION info{};BATTERY_QUERY_INFORMATION query{};
        bool ok=DeviceIoControl(h,IOCTL_BATTERY_QUERY_TAG,&wait,sizeof(wait),&t,sizeof(t),&returned,nullptr)&&t;
        query.BatteryTag=t;query.InformationLevel=BatteryInformation;
        ok=ok&&DeviceIoControl(h,IOCTL_BATTERY_QUERY_INFORMATION,&query,sizeof(query),&info,sizeof(info),&returned,nullptr);
        CloseHandle(h);
        if(!ok||!(info.Capabilities&BATTERY_SYSTEM_BATTERY)||(info.Capabilities&BATTERY_CAPACITY_RELATIVE))continue;
        if(info.DesignedCapacity==0||info.DesignedCapacity==UINT32_MAX||info.FullChargedCapacity==UINT32_MAX)continue;
        facts.designMWh+=info.DesignedCapacity;facts.fullMWh+=info.FullChargedCapacity;
        facts.cycles=std::max<uint32_t>(facts.cycles,info.CycleCount);facts.valid=true;
    }
    SetupDiDestroyDeviceInfoList(set);
    return facts;
}
#pragma pack(push,1)
struct Sample { uint64_t utc=0; int16_t percent=-1; uint8_t source=SourceUnknown, gap=0; };
// Energy components Windows reports per app (millijoules). Grouped for display.
enum Part { PartCpu, PartGpu, PartDisplay, PartNetwork, PartDisk, PartOther, PART_COUNT };
struct LegacyEnergy { uint64_t hour=0,id=0; double mj=0; uint8_t source=SourceUnknown; wchar_t name[64]{}; };
struct Energy { uint64_t hour=0,id=0; double mj=0; uint8_t source=SourceUnknown; wchar_t name[64]{}; float parts[PART_COUNT]{}; };
struct EnergyHeader { uint32_t magic=0x31454342,version=2,count=0; uint64_t updated=0,latest=0; };
// Battery energy actually lost per clock hour, measured from the battery's own capacity counter.
struct DrainHour { uint64_t hour=0; float mwh=0; uint32_t seconds=0; };
struct DrainHeader { uint32_t magic=0x31444342,count=0; };
struct HistoryHeader { uint32_t magic=0x31484342,count=0; };
struct LegacyPrefs { uint32_t magic=0x31504342; int32_t x=INT32_MIN,y=INT32_MIN; uint8_t top=0,expanded=0; };
struct PrefsV2 { uint32_t magic=0x32504342; int32_t x=INT32_MIN,y=INT32_MIN; uint8_t top=0,expanded=0,desktop=0; };
// alerts: 1 = unplug at 80%, 2 = low battery, 4 = unusual drain. All opt-in.
struct Prefs { uint32_t magic=0x33504342; int32_t x=INT32_MIN,y=INT32_MIN; uint8_t top=0,tab=0,desktop=0,taskbar=1,alerts=0,size=0,reserved[2]{}; }; // size: 0 medium, 1 small, 2 large
// apps.bin: Windows' identity (path or package id) per app, used only to find icons.
// Kept apart from energy.bin so older readers and the hourly helper stay compatible.
struct AppPath { uint64_t id=0; wchar_t identity[260]{}; };
struct AppsHeader { uint32_t magic=0x31414342,count=0; };
#pragma pack(pop)
constexpr size_t MAX_APP_PATHS=2048;
constexpr int APP_ROWS=5;
struct AppTotal { uint64_t id=0; wstring name; double mj=0,idMj=0; std::array<double,PART_COUNT> parts{}; };
struct UsageView {
    std::array<double,24> battery{},plugged{},unknown{},app{},measured{}; // measured: mJ lost by the battery
    std::array<uint64_t,24> hours{};
    std::vector<AppTotal> apps;
    double total=0,unassigned=0,day=0,measuredTotal=0,attributedOnBattery=0;
    int Pages() const {return std::max(1,(int(apps.size())+APP_ROWS-1)/APP_ROWS);}
};
struct State {
    HWND window=nullptr,tooltip=nullptr,controller=nullptr,desktopHost=nullptr;
    float scale=1;
    bool expanded=false,top=false,hidden=false,displayOn=true,locked=false,suspended=false;
    bool refreshing=false,timer=false,dirty=false,readOnly=false,historyGap=true;
    bool desktop=false,desktopWanted=false,quitting=false,dragging=false;
    bool hourlyAvailable=false,scheduledPending=false;
    unsigned desktopRetries=0;
    POINT dragStart{};RECT dragRect{};
    UINT timerInterval=0;
    uint64_t powerReads=0;
    Power power;
    BatteryFacts facts;
    bool hovering=false,tracking=false;float hoverX=0;
    int tab=0;float ringShown=0;bool animating=false,lightApps=false;
    HWND pill=nullptr,peek=nullptr;bool pillHover=false,pillTracking=false,pillTwoLines=false;wstring pillKey;
    uint64_t lastDrainAlert=0;unsigned alertsSent=0;
    HICON trayIcon=nullptr;uint32_t trayKey=UINT32_MAX;bool lightTaskbar=false;
    Prefs prefs;
    std::vector<Sample> history;
    std::vector<Energy> energy;
    EnergyHeader energyHeader;
    UsageView view;
    int filter=0,selected=-1,page=0;
    bool appSelected=false;wstring appName; // Selection is by display name, so merged identities stay together.
    std::vector<DrainHour> drain;uint32_t drainBase=UINT32_MAX;uint64_t drainBaseUtc=0;
    // Live view: CPU-time snapshots by app, kept only while the Now tab is open.
    struct LiveSnapshot { uint64_t tick=0; uint64_t busy=0; std::map<wstring,uint64_t> cpu; std::map<wstring,uint32_t> pid; };
    struct LiveApp { wstring name; double cpu=0,share=0; uint32_t pid=0; };
    std::vector<LiveSnapshot> liveSnaps;std::vector<LiveApp> live;std::vector<BYTE> liveBuffer;
    std::array<float,150> watts{};size_t wattsCount=0;double liveBusy=0;
    std::map<wstring,wstring> livePaths;
    int hoverRow=-1,hoverHour=-1;
    std::map<uint64_t,wstring> appPaths;
    std::map<uint64_t,std::unique_ptr<Bitmap>> icons;
    uint64_t lastSave=0,lastSample=0,viewHour=0;
    uint64_t cacheCheck=0,scheduledRequest=0;
    FILETIME scheduledStamp{},scheduledStatusStamp{};
    wstring scheduledFolder,scheduledTask;
    wstring notice;
    std::unique_ptr<Bitmap> buffer;
    std::array<std::unique_ptr<Font>,8> fonts;
    std::vector<HWND> buttons;
    std::vector<HPOWERNOTIFY> notifications;
    NOTIFYICONDATAW tray{};
    HICON icon=nullptr;
} g;
wstring root,data;
// Theme colours follow the Windows app mode (light or dark); see ApplyTheme.
Color BG(255,20,25,26), PANEL(255,27,33,34), LINE(255,50,60,59), FG(255,236,243,236), MUTED(255,160,176,170), GREEN(255,183,235,171),
    GOLD(255,237,192,121), BLUE(255,144,173,189), TRACK(255,35,44,42), RED(255,240,134,118), SEL(255,58,73,68);
void ApplyTheme(bool light){
    g.lightApps=light;
    if(light){BG=Color(255,250,250,248);PANEL=Color(255,242,243,240);LINE=Color(255,218,222,218);FG=Color(255,24,30,28);MUTED=Color(255,92,104,99);
        GREEN=Color(255,46,128,66);GOLD=Color(255,178,108,16);BLUE=Color(255,40,104,146);TRACK=Color(255,234,237,233);RED=Color(255,196,58,44);SEL=Color(255,255,255,255);}
    else{BG=Color(255,20,25,26);PANEL=Color(255,27,33,34);LINE=Color(255,50,60,59);FG=Color(255,236,243,236);MUTED=Color(255,160,176,170);
        GREEN=Color(255,183,235,171);GOLD=Color(255,237,192,121);BLUE=Color(255,144,173,189);TRACK=Color(255,35,44,42);RED=Color(255,240,134,118);SEL=Color(255,58,73,68);}
}
bool LightApps(){
    DWORD value=1,size=sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",L"AppsUseLightTheme",RRF_RT_REG_DWORD,nullptr,&value,&size)!=ERROR_SUCCESS||value!=0;
}
constexpr int LOW_PERCENT=20;

uint64_t Now() { FILETIME ft; GetSystemTimeAsFileTime(&ft); ULARGE_INTEGER u;u.LowPart=ft.dwLowDateTime;u.HighPart=ft.dwHighDateTime;return u.QuadPart/10000000ULL-11644473600ULL; }
FILETIME FileTime(uint64_t utc) { ULARGE_INTEGER u;u.QuadPart=(utc+11644473600ULL)*10000000ULL;return FILETIME{u.LowPart,u.HighPart}; }
SYSTEMTIME LocalTime(uint64_t utc) { FILETIME f=FileTime(utc);SYSTEMTIME s{},l{};FileTimeToSystemTime(&f,&s);SystemTimeToTzSpecificLocalTime(nullptr,&s,&l);return l; }
wstring Clock(uint64_t utc) { auto s=LocalTime(utc);wchar_t b[24];swprintf(b,24,L"%02u:%02u",s.wHour,s.wMinute);return b; }
wstring ExePath() { wchar_t p[32768];DWORD n=GetModuleFileNameW(nullptr,p,32768);return n&&n<32768?wstring(p,n):L""; }
bool ExactRead(HANDLE f,void* p,DWORD size) { DWORD n=0;return ReadFile(f,p,size,&n,nullptr)&&n==size; }
bool ExactWrite(HANDLE f,const void* p,DWORD size) { DWORD n=0;return WriteFile(f,p,size,&n,nullptr)&&n==size; }
bool AtomicWrite(const wstring& path,const void* header,DWORD hs,const void* body,DWORD bs) {
    wstring tmp=path+L".new";
    HANDLE f=CreateFileW(tmp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE)return false;
    bool ok=ExactWrite(f,header,hs)&&(!bs||ExactWrite(f,body,bs))&&FlushFileBuffers(f);
    CloseHandle(f);
    if(ok)ok=MoveFileExW(tmp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
    if(!ok)DeleteFileW(tmp.c_str());
    return ok;
}
void InitPaths(const wstring& overrideData=L"") {
    root=ExePath();root=root.substr(0,root.find_last_of(L"\\/"));data=root+L"\\data";
    if(!overrideData.empty())data=overrideData;
    if(!CreateDirectoryW(data.c_str(),nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS)g.readOnly=true;
}
void InitScheduledPaths(){
    HANDLE token=nullptr;DWORD size=0;LPWSTR sid=nullptr;PWSTR base=nullptr;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return;
    GetTokenInformation(token,TokenUser,nullptr,0,&size);std::vector<BYTE> bytes(size);
    if(size&&GetTokenInformation(token,TokenUser,bytes.data(),size,&size)&&ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid,&sid)){
        g.scheduledTask=L"CellBatteryHistory-"+wstring(sid);
        if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData,0,nullptr,&base))){g.scheduledFolder=wstring(base)+L"\\CellBattery\\History-"+sid;CoTaskMemFree(base);}
        LocalFree(sid);
    }
    CloseHandle(token);
}
wstring EtaText(uint32_t seconds) {
    if(seconds==UINT32_MAX||seconds==0)return L"Estimating";
    auto m=std::max(1u,(seconds+30)/60);
    if(m>7*24*60)return L"> 7 days";
    wchar_t b[40];
    if(m>=60)swprintf(b,40,L"%uh %02um",m/60,m%60);else swprintf(b,40,L"%um",m);
    return b;
}
uint64_t EtaDisplayMinute(uint32_t seconds){return seconds==UINT32_MAX||seconds==0?UINT64_MAX:std::max<uint64_t>(1,(uint64_t(seconds)+30)/60);}
void ApplyRecentDrain(Power& p,uint32_t capacity,double watts,uint32_t span){
    p.recentDrain=true;p.watts=watts;p.drainSpan=span;p.capacityMWh=capacity;
    if(!p.windowsEta){double estimate=3.6*capacity/watts;if(estimate>0&&estimate<7*86400)p.seconds=uint32_t(estimate);else p.seconds=UINT32_MAX;}
}
Power ReadPower() {
    Power p;SYSTEM_POWER_STATUS s{};
    if(!GetSystemPowerStatus(&s)){meter.trend.Reset();return p;}
    p.valid=true;p.present=s.BatteryFlag!=255&&(s.BatteryFlag&128)==0;
    p.percent=p.present&&s.BatteryLifePercent<=100?s.BatteryLifePercent:-1;
    p.saver=s.SystemStatusFlag==1;
    if(!p.present){meter.trend.Reset();return p;}
    p.source=s.ACLineStatus==0?Battery:s.ACLineStatus==1?((s.BatteryFlag&8)?Charging:Plugged):SourceUnknown;
    if(p.source==Battery&&s.BatteryLifeTime!=UINT32_MAX&&s.BatteryLifeTime>0){p.seconds=s.BatteryLifeTime;p.windowsEta=true;}
    SYSTEM_BATTERY_STATE b{};
    if(CallNtPowerInformation(SystemBatteryState,nullptr,0,&b,sizeof(b))==0&&b.BatteryPresent) {
        if(p.source==Battery&&!p.windowsEta&&b.EstimatedTime!=UINT32_MAX&&b.EstimatedTime>0){p.seconds=b.EstimatedTime;p.windowsEta=true;}
        if(b.RemainingCapacity!=UINT32_MAX&&b.RemainingCapacity>0)p.remainingMWh=b.RemainingCapacity;
        if(b.MaxCapacity!=UINT32_MAX&&b.MaxCapacity>0)p.fullMWh=b.MaxCapacity;
        const int64_t rate=static_cast<int32_t>(b.Rate);
        if(b.Rate!=0x80000000u&&b.Rate!=UINT32_MAX&&rate!=0&&std::abs(rate)<500000) {
            p.watts=p.reportedWatts=std::abs(rate)/1000.0;
            // Linear time-to-full. Real charging tapers near the top, so this is a floor.
            if(p.source==Charging&&rate>0&&p.remainingMWh!=UINT32_MAX&&p.fullMWh!=UINT32_MAX&&p.fullMWh>p.remainingMWh){
                double estimate=3600.0*(p.fullMWh-p.remainingMWh)/rate;
                if(estimate>0&&estimate<2*86400)p.fullSeconds=uint32_t(estimate);
            }
            if(p.source==Battery&&!p.windowsEta) {
                if(b.EstimatedTime!=UINT32_MAX&&b.EstimatedTime>0){p.seconds=b.EstimatedTime;p.windowsEta=true;}
                else if(b.Discharging&&rate<0&&b.RemainingCapacity!=UINT32_MAX&&b.RemainingCapacity>0) {
                    double estimate=3600.0*b.RemainingCapacity/-rate;
                    if(estimate>0&&estimate<7*86400)p.seconds=static_cast<uint32_t>(estimate);
                }
            }
        }
    }
    if(p.source==Battery&&!g.hidden&&g.displayOn&&!g.locked&&!g.suspended){
        uint64_t tick=GetTickCount64();uint32_t capacity=UINT32_MAX,span=0;double watts=0;
        if(meter.Capacity(tick,capacity)){
            p.capacityMWh=capacity;
            if(meter.trend.Add(tick,capacity,watts,span))ApplyRecentDrain(p,capacity,watts,span);
        }else meter.trend.Reset();
        p.drainCapable=meter.device!=INVALID_HANDLE_VALUE;
    }else meter.trend.Reset();
    return p;
}
void LoadHistory() {
    HANDLE f=CreateFileW((data+L"\\history.bin").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    if(f==INVALID_HANDLE_VALUE)return;
    HistoryHeader h{};LARGE_INTEGER size{};
    if(GetFileSizeEx(f,&size)&&ExactRead(f,&h,sizeof(h))&&h.magic==0x31484342&&h.count<=MAX_HISTORY&&size.QuadPart==sizeof(h)+h.count*sizeof(Sample)) {
        std::vector<Sample> v(h.count);if(ExactRead(f,v.data(),DWORD(v.size()*sizeof(Sample)))) {
            uint64_t now=Now(),prev=0;bool ok=true;
            for(auto& s:v)if(s.percent<0||s.percent>100||s.source>SourceUnknown||s.utc<prev||s.utc>now+300){ok=false;break;}else prev=s.utc;
            if(ok){for(auto& s:v)if(s.utc+HISTORY_DAYS*DAY>=now)g.history.push_back(s);}
        }
    }
    CloseHandle(f);
}
void LoadDrain(){
    HANDLE f=CreateFileW((data+L"\\drain.bin").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    if(f==INVALID_HANDLE_VALUE)return;
    DrainHeader h;LARGE_INTEGER size{};
    if(GetFileSizeEx(f,&size)&&ExactRead(f,&h,sizeof(h))&&h.magic==0x31444342&&h.count<=24*(HISTORY_DAYS+1)&&size.QuadPart==LONGLONG(sizeof(h)+h.count*sizeof(DrainHour))){
        std::vector<DrainHour> v(h.count);
        if(ExactRead(f,v.data(),DWORD(v.size()*sizeof(DrainHour)))){
            bool ok=true;uint64_t prev=0;
            for(auto& d:v)if(d.hour%3600||d.hour<prev||!std::isfinite(d.mwh)||d.mwh<0||d.mwh>1e6f||d.seconds>3600){ok=false;break;}else prev=d.hour+1;
            if(ok)g.drain.swap(v);
        }
    }
    CloseHandle(f);
}
bool SaveHistory() {
    if(g.readOnly||!g.dirty)return true;
    HistoryHeader h;h.count=static_cast<uint32_t>(g.history.size());
    if(!AtomicWrite(data+L"\\history.bin",&h,sizeof(h),g.history.data(),DWORD(g.history.size()*sizeof(Sample))))return false;
    DrainHeader d;d.count=uint32_t(g.drain.size());
    AtomicWrite(data+L"\\drain.bin",&d,sizeof(d),g.drain.data(),DWORD(g.drain.size()*sizeof(DrainHour)));
    g.dirty=false;g.lastSave=Now();return true;
}
void AddDrain(uint64_t hour,double mwh,uint64_t seconds){
    auto it=std::lower_bound(g.drain.begin(),g.drain.end(),hour,[](const DrainHour& d,uint64_t h){return d.hour<h;});
    if(it==g.drain.end()||it->hour!=hour)it=g.drain.insert(it,DrainHour{hour,0,0});
    it->mwh+=float(mwh);it->seconds=uint32_t(std::min<uint64_t>(3600,it->seconds+seconds));
}
// Integrates the drop in the battery's remaining-energy counter while on battery. A drop
// across several hours (sleep, screen off) is spread evenly over that span. Charging,
// capacity recalibration or a missing reading restart the baseline instead of guessing.
void RecordDrain(uint64_t now){
    const Power& p=g.power;
    if(p.source!=Battery||p.remainingMWh==UINT32_MAX){g.drainBase=UINT32_MAX;return;}
    if(g.drainBase!=UINT32_MAX&&now>g.drainBaseUtc&&now-g.drainBaseUtc<=DAY&&p.remainingMWh<g.drainBase){
        double lost=double(g.drainBase-p.remainingMWh),span=double(now-g.drainBaseUtc);
        for(uint64_t t=g.drainBaseUtc;t<now;){uint64_t h=t/3600*3600,end=std::min(now,h+3600);AddDrain(h,lost*double(end-t)/span,end-t);t=end;}
        uint64_t keep=now>(HISTORY_DAYS+1)*DAY?now-(HISTORY_DAYS+1)*DAY:0;
        g.drain.erase(g.drain.begin(),std::lower_bound(g.drain.begin(),g.drain.end(),keep,[](const DrainHour& d,uint64_t h){return d.hour<h;}));
        g.dirty=true;
    }
    if(g.drainBase==UINT32_MAX||p.remainingMWh!=g.drainBase||now-g.drainBaseUtc>DAY){g.drainBase=p.remainingMWh;g.drainBaseUtc=now;}
}
double DrainAt(uint64_t hour,uint32_t* seconds=nullptr){
    auto it=std::lower_bound(g.drain.begin(),g.drain.end(),hour,[](const DrainHour& d,uint64_t h){return d.hour<h;});
    bool found=it!=g.drain.end()&&it->hour==hour;if(seconds)*seconds=found?it->seconds:0;return found?it->mwh:0;
}
void RecordPower(uint64_t now,bool force=false) {
    if(g.power.percent<0)return;
    Sample s{now,static_cast<int16_t>(g.power.percent),static_cast<uint8_t>(g.power.source),0};
    bool transition=!g.history.empty()&&g.history.back().source!=s.source;
    if(!g.history.empty()){
        const Sample& last=g.history.back();
        if(now<last.utc)return;
        if(!force&&!transition&&now-last.utc<290)return; // Slack for coalesced timers firing slightly early.
        s.gap=g.historyGap||now-last.utc>660;
    }else s.gap=1;
    g.historyGap=false;
    auto first=std::lower_bound(g.history.begin(),g.history.end(),now>HISTORY_DAYS*DAY?now-HISTORY_DAYS*DAY:0,[](const Sample&a,uint64_t t){return a.utc<t;});
    g.history.erase(g.history.begin(),first);
    if(g.history.size()>=MAX_HISTORY)g.history.erase(g.history.begin(),g.history.begin()+std::min<size_t>(100,g.history.size()));
    g.history.push_back(s);g.dirty=true;
    if(transition||g.lastSave==0||now-g.lastSave>=900)SaveHistory();
}
void LoadPrefs() {
    HANDLE f=CreateFileW((data+L"\\preferences.bin").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    if(f==INVALID_HANDLE_VALUE)return;Prefs p;LARGE_INTEGER size{};
    if(GetFileSizeEx(f,&size)){
        // Older versions expanded app usage downward; that view is now the Apps tab.
        if(size.QuadPart==sizeof(LegacyPrefs)){
            LegacyPrefs old;
            if(ExactRead(f,&old,sizeof(old))&&old.magic==0x31504342&&old.top<=1&&old.expanded<=1){p.x=old.x;p.y=old.y;p.top=old.top;p.tab=old.expanded;g.prefs=p;}
        }else if(size.QuadPart==sizeof(PrefsV2)){
            PrefsV2 old;
            if(ExactRead(f,&old,sizeof(old))&&old.magic==0x32504342&&old.top<=1&&old.expanded<=1&&old.desktop<=1){p.x=old.x;p.y=old.y;p.top=old.top;p.tab=old.expanded;p.desktop=old.desktop;g.prefs=p;}
        }else if(size.QuadPart==sizeof(p)&&ExactRead(f,&p,sizeof(p))&&p.magic==0x33504342&&p.top<=1&&p.tab<=3&&p.desktop<=1&&p.taskbar<=1&&p.alerts<=7&&p.size<=2)g.prefs=p;
    }
    CloseHandle(f);g.top=g.prefs.top;g.tab=g.prefs.tab;g.desktopWanted=g.prefs.desktop!=0;
}
void SavePrefs() {
    if(g.readOnly||!g.window)return;
    RECT r{};GetWindowRect(g.window,&r);g.prefs.x=r.left;g.prefs.y=r.top;
    g.prefs.tab=uint8_t(g.tab);g.prefs.top=g.top;g.prefs.desktop=g.desktopWanted;
    AtomicWrite(data+L"\\preferences.bin",&g.prefs,sizeof(g.prefs),nullptr,0);
}
bool ReadEnergy(const wstring& path,std::vector<Energy>& target,EnergyHeader& header) {
    HANDLE f=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    if(f==INVALID_HANDLE_VALUE)return false;
    EnergyHeader h;LARGE_INTEGER size{};bool ok=false;
    // Version 1 caches (older widgets and hourly helpers) have no component breakdown.
    bool valid=GetFileSizeEx(f,&size)&&ExactRead(f,&h,sizeof(h))&&h.magic==0x31454342&&(h.version==1||h.version==2)&&h.count<=MAX_ENERGY;
    size_t record=h.version==1?sizeof(LegacyEnergy):sizeof(Energy);
    if(valid&&size.QuadPart==LONGLONG(sizeof(h)+h.count*record)){
        std::vector<Energy> v(h.count);bool read=false;
        if(h.version==1){
            std::vector<LegacyEnergy> old(h.count);
            if((read=ExactRead(f,old.data(),DWORD(old.size()*record)))!=false)
                for(size_t i=0;i<old.size();i++){v[i].hour=old[i].hour;v[i].id=old[i].id;v[i].mj=old[i].mj;v[i].source=old[i].source;memcpy(v[i].name,old[i].name,sizeof(v[i].name));}
        }else read=ExactRead(f,v.data(),DWORD(v.size()*record));
        if(read){
            ok=h.updated<=Now()+300;
            for(auto& e:v){
                if(e.source>SourceUnknown||!std::isfinite(e.mj)||e.mj<0||e.hour>h.updated+3600||!wmemchr(e.name,0,64)){ok=false;break;}
                for(float& part:e.parts)if(!std::isfinite(part)||part<0)part=0;
            }
            if(ok){target.swap(v);header=h;header.version=2;}
        }
    }
    CloseHandle(f);return ok;
}
wstring DisplayName(const wstring& raw);
void BuildView(uint64_t now) {
    UsageView v;uint64_t hour=now/3600*3600;g.viewHour=hour;
    for(int i=0;i<24;i++){
        v.hours[i]=hour-(23-i)*3600;
        // Measured battery loss only applies to on-battery views.
        // Only hours Cell observed for at least 45 minutes; a partial hour would overstate Windows' coverage.
        uint32_t seen=0;double mwh=g.filter!=1?DrainAt(v.hours[i],&seen):0;
        if(seen>=2700){v.measured[i]=mwh*3600.0;if(g.selected<0||g.selected==i)v.measuredTotal+=v.measured[i];}
    }
    // Apps are merged by display name: one product often has several executables or identities.
    std::map<wstring,AppTotal> totals;
    for(const auto& e:g.energy){
        if(e.hour<v.hours[0]||e.hour>hour)continue;
        // The chart and the list share the source filter so they always describe the same energy.
        if(g.filter==0&&e.source!=Battery)continue;
        if(g.filter==1&&e.source!=Plugged)continue;
        int i=static_cast<int>((e.hour-v.hours[0])/3600);
        if(e.source==Battery)v.battery[i]+=e.mj;else if(e.source==Plugged)v.plugged[i]+=e.mj;else v.unknown[i]+=e.mj;
        v.day+=e.mj;
        wstring name=e.id==UNATTRIBUTED_ID?wstring():DisplayName(e.name);
        if(g.appSelected&&!name.empty()&&name==g.appName)v.app[i]+=e.mj;
        if(g.selected>=0&&i!=g.selected)continue;
        if(e.source==Battery&&e.id!=UNATTRIBUTED_ID)v.attributedOnBattery+=e.mj;
        if(e.id==UNATTRIBUTED_ID){v.unassigned+=e.mj;continue;}
        auto& a=totals[name];a.name=name;a.mj+=e.mj;v.total+=e.mj;
        for(int k=0;k<PART_COUNT;k++)a.parts[k]+=e.parts[k];
        if(e.mj>a.idMj){a.idMj=e.mj;a.id=e.id;} // The biggest contributor's identity supplies the icon.
    }
    for(auto& kv:totals)if(kv.second.mj>0)v.apps.push_back(std::move(kv.second));
    std::sort(v.apps.begin(),v.apps.end(),[](const AppTotal&a,const AppTotal&b){return a.mj>b.mj;});
    if(g.filter!=3)g.page=std::clamp(g.page,0,v.Pages()-1); // The live tab pages its own list.
    g.view=std::move(v);
}

// SRUM is exported ONLY in a short-lived helper. The widget never scans processes.
class CsvReader {
    HANDLE file=INVALID_HANDLE_VALUE;std::array<uint8_t,16384> buffer{};
    size_t at=0,used=0;int pending=-1,encoding=0,low=-1;
    int Byte(){if(at==used){DWORD n=0;if(!ReadFile(file,buffer.data(),DWORD(buffer.size()),&n,nullptr)||!n)return -1;at=0;used=n;}return buffer[at++];}
    int Char(){
        if(low>=0){int p=low;low=-1;return p;}
        int c=Byte();if(c<0)return -1;
        if(encoding){int d=Byte();return d<0?-1:encoding==1?c|(d<<8):(c<<8)|d;}
        if(c<128)return c;
        uint32_t cp;int count;
        if((c&0xe0)==0xc0){cp=c&31;count=1;}else if((c&0xf0)==0xe0){cp=c&15;count=2;}else if((c&0xf8)==0xf0){cp=c&7;count=3;}else return 0xfffd;
        for(int i=0;i<count;i++){int b=Byte();if(b<0)return -1;if((b&0xc0)!=0x80)return 0xfffd;cp=(cp<<6)|(b&63);}
        if(cp>0x10ffff)return 0xfffd;
        if(cp>0xffff){cp-=0x10000;low=0xdc00+(cp&1023);return 0xd800+(cp>>10);}return int(cp);
    }
    int Get(){if(pending>=0){int c=pending;pending=-1;return c;}return Char();}
public:
    bool error=false;
    explicit CsvReader(const wstring& path){
        file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
        if(file==INVALID_HANDLE_VALUE){error=true;return;}
        int a=Byte(),b=Byte(),c=Byte();
        if(a==255&&b==254){encoding=1;at=2;}else if(a==254&&b==255){encoding=2;at=2;}else if(!(a==239&&b==187&&c==191))at=0;
    }
    ~CsvReader(){if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);}
    bool Row(std::vector<wstring>& fields){
        fields.clear();wstring field;bool quoted=false,started=false;size_t length=0;
        while(true){int c=Get();if(c<0){if(quoted)error=true;if(!started&&field.empty()&&fields.empty())return false;fields.push_back(std::move(field));return !error;}
            started=true;if(++length>262144||fields.size()>255){error=true;return false;}
            if(c==L'"'){
                if(quoted){int d=Get();if(d==L'"')field.push_back(L'"');else{quoted=false;pending=d;}}
                else if(field.empty())quoted=true;else field.push_back(L'"');
            }else if(c==L','&&!quoted){fields.push_back(std::move(field));field.clear();}
            else if((c==L'\r'||c==L'\n')&&!quoted){if(c==L'\r'){int d=Get();if(d!=L'\n')pending=d;}fields.push_back(std::move(field));return true;}
            else field.push_back(wchar_t(c));
        }
    }
};
wstring Normalize(const wstring&s){wstring n;for(wchar_t c:s)if((c>=L'a'&&c<=L'z')||(c>=L'0'&&c<=L'9'))n+=c;else if(c>=L'A'&&c<=L'Z')n+=c+32;return n;}
wstring Trim(const wstring&s){size_t a=0,b=s.size();while(a<b&&iswspace(s[a]))++a;while(b>a&&iswspace(s[b-1]))--b;return s.substr(a,b-a);}
bool Number(const wstring&s,double&v){if(s.empty())return false;wchar_t* end=nullptr;v=wcstod(s.c_str(),&end);while(end&&iswspace(*end))++end;return end!=s.c_str()&&end&&!*end&&std::isfinite(v)&&v>=0;}
uint64_t ParseTimestamp(const wstring& text) {
    int y=0,m=0,d=0,h=0,mi=0,sec=0;
    if(swscanf(text.c_str(),L"%d-%d-%d%*c%d:%d:%d",&y,&m,&d,&h,&mi,&sec)!=6||y<2000||y>2100||m<1||m>12||d<1||d>31||h<0||h>23||mi<0||mi>59||sec<0||sec>59)return 0;
    SYSTEMTIME s{};s.wYear=WORD(y);s.wMonth=WORD(m);s.wDay=WORD(d);s.wHour=WORD(h);s.wMinute=WORD(mi);s.wSecond=WORD(sec);FILETIME ft{};
    if(!SystemTimeToFileTime(&s,&ft))return 0;ULARGE_INTEGER u;u.LowPart=ft.dwLowDateTime;u.HighPart=ft.dwHighDateTime;uint64_t utc=u.QuadPart/10000000ULL-11644473600ULL;
    // Windows SRUM exports UTC. Explicit ISO offsets are also accepted.
    size_t sign=text.find_first_of(L"+-",19);
    if(sign!=wstring::npos){int oh=0,om=0;if(swscanf(text.c_str()+sign+1,L"%d:%d",&oh,&om)!=2||oh>23||om>59)return 0;int off=(oh*60+om)*60;utc=text[sign]==L'+'?utc-off:utc+off;}
    return utc;
}
uint64_t Hash(const wstring&s){uint64_t h=1469598103934665603ULL;for(wchar_t c:s){wchar_t l=towlower(c);h^=uint16_t(l);h*=1099511628211ULL;}return h;}
wstring AppName(wstring s){
    auto slash=s.find_last_of(L"\\/");if(slash!=wstring::npos)s=s.substr(slash+1);
    auto bang=s.find(L'!');if(bang!=wstring::npos)s=s.substr(0,bang);
    bool executable=s.size()>4&&Normalize(s.substr(s.size()-4))==L"exe";
    auto under=s.find(L'_');if(under!=wstring::npos&&!executable)s=s.substr(0,under);
    if(executable)s.resize(s.size()-4);
    const std::pair<const wchar_t*,const wchar_t*> friendly[]={
        {L"msedge",L"Microsoft Edge"},{L"chrome",L"Google Chrome"},{L"brave",L"Brave"},{L"firefox",L"Firefox"},
        {L"code",L"Visual Studio Code"},{L"codex",L"Codex"},{L"cellbattery",L"Cell"},{L"spotify",L"Spotify"},
        {L"explorer",L"Windows Explorer"},{L"svchost",L"Windows services"},{L"dwm",L"Desktop Window Manager"},
        {L"wmiprvse",L"WMI provider host"},{L"audiodg",L"Windows audio"}
    };
    for(auto& pair:friendly)if(_wcsicmp(s.c_str(),pair.first)==0){s=pair.second;break;}
    if(s.empty())s=L"System";if(s.size()>63)s.resize(63);return s;
}
bool ParseEnergy(const wstring& path,uint64_t now,std::vector<Energy>& output,EnergyHeader& head,wstring& error,std::vector<AppPath>* identities=nullptr) {
    CsvReader csv(path);std::vector<wstring> fields;std::vector<wstring> names;std::map<uint64_t,wstring> seen;
    int app=-1,time=-1,source=-1,total=-1;std::vector<std::pair<int,int>> components;bool schema=false; // (column, Part)
    std::map<std::tuple<uint64_t,uint64_t,uint8_t>,Energy> aggregate;
    uint64_t latest=0;size_t accepted=0,invalid=0;
    while(csv.Row(fields)){
        if(!schema){
            names.clear();for(auto& f:fields)names.push_back(Normalize(f));
            for(size_t i=0;i<names.size();i++){
                auto& n=names[i];
                if(n==L"appid"||n==L"appname"||n==L"application"||n==L"processname")app=int(i);
                if(n==L"timestamp"||n==L"endtime")time=int(i);
                if(n==L"onbattery")source=int(i);
                if(n==L"totalenergyconsumption"||n==L"totalenergy")total=int(i);
            }
            if(app<0||time<0){app=time=source=total=-1;continue;}
            const std::pair<const wchar_t*,int> comp[]={
                {L"cpuenergyconsumption",PartCpu},{L"socenergyconsumption",PartCpu},{L"gpuenergyconsumption",PartGpu},{L"npuenergyconsumption",PartOther},
                {L"displayenergyconsumption",PartDisplay},{L"diskenergyconsumption",PartDisk},{L"networkenergyconsumption",PartNetwork},{L"mbbenergyconsumption",PartNetwork},
                {L"lossenergyconsumption",PartOther},{L"energyloss",PartOther},{L"otherenergyconsumption",PartOther},
                {L"cpuenergy",PartCpu},{L"socenergy",PartCpu},{L"gpuenergy",PartGpu},{L"npuenergy",PartOther},{L"displayenergy",PartDisplay},{L"diskenergy",PartDisk},
                {L"networkenergy",PartNetwork},{L"mbbenergy",PartNetwork},{L"lossenergy",PartOther},{L"otherenergy",PartOther}};
            for(size_t i=0;i<names.size();i++)for(auto& n:comp)if(names[i]==n.first){components.emplace_back(int(i),n.second);break;}
            if(total<0&&components.empty()){error=L"Windows exported no recognized energy columns.";return false;}schema=true;continue;
        }
        if(fields.size()!=names.size()){if(fields.size()>1)++invalid;continue;}
        uint64_t stamp=ParseTimestamp(fields[time]);if(!stamp){++invalid;continue;}
        if(stamp>now+300||stamp/3600>now/3600||stamp/3600*3600+24*3600<=now/3600*3600)continue;
        if(fields[app].empty()||fields[app].size()>4096){++invalid;continue;}
        wstring identity=Trim(fields[app]);
        // EMI_RAPL_* rows are hardware domains, including nested/overlapping CPU
        // package, core, GPU and DRAM meters. They are not app attribution.
        if(_wcsnicmp(identity.c_str(),L"EMI_",4)==0&&identity.find_first_of(L"\\/")==wstring::npos&&Normalize(identity).find(L"exe")==wstring::npos)continue;
        double mj=0,parts[PART_COUNT]{};bool numeric=true;
        for(auto& c:components){
            double value=0;
            if(!Number(fields[c.first],value)){if(total<0){numeric=false;break;}continue;} // Components are optional detail when a total exists.
            parts[c.second]+=value;if(total<0)mj+=value;
        }
        if(numeric&&total>=0)numeric=Number(fields[total],mj);
        if(!numeric||!std::isfinite(mj)||mj>1e18){++invalid;continue;}
        uint8_t src=SourceUnknown;
        if(source>=0){auto n=Normalize(fields[source]);if(n==L"1"||n==L"true"||n==L"yes")src=Battery;else if(n==L"0"||n==L"false"||n==L"no")src=Plugged;}
        bool unassigned=Normalize(identity)==L"unknown"||Normalize(identity)==L"unattributed";
        uint64_t id=unassigned?UNATTRIBUTED_ID:Hash(identity);uint64_t hour=stamp/3600*3600;
        auto key=std::make_tuple(hour,id,src);wstring name=unassigned?L"Windows / unattributed":AppName(identity);
        if(aggregate.size()>=MAX_ENERGY-72&&aggregate.find(key)==aggregate.end()){id=0;name=L"Other apps";key=std::make_tuple(hour,id,src);}
        auto& e=aggregate[key];e.hour=hour;e.id=id;e.source=src;e.mj+=mj;wcsncpy(e.name,name.c_str(),63);e.name[63]=0;
        for(int k=0;k<PART_COUNT;k++)if(std::isfinite(parts[k])&&parts[k]<1e18)e.parts[k]+=float(parts[k]);
        if(identities&&id&&id!=UNATTRIBUTED_ID&&identity.size()<260&&seen.size()<MAX_APP_PATHS)seen.emplace(id,identity);
        latest=std::max(latest,stamp);++accepted;
    }
    if(csv.error){error=L"Windows history was unreadable or malformed.";return false;}
    if(!schema){error=L"This Windows history format is not supported.";return false;}
    if(invalid&&accepted==0){error=L"Windows exported records that could not be read.";return false;}
    std::vector<Energy> result;result.reserve(aggregate.size());for(auto& kv:aggregate)result.push_back(kv.second);
    output.swap(result);head=EnergyHeader{};head.count=uint32_t(output.size());head.updated=now;head.latest=latest;
    if(identities){identities->clear();for(auto& kv:seen){AppPath p;p.id=kv.first;wcsncpy(p.identity,kv.second.c_str(),259);identities->push_back(p);}}
    return true;
}
bool WriteAppPaths(const wstring& folder,const std::vector<AppPath>& paths){
    AppsHeader h;h.count=uint32_t(paths.size());
    return AtomicWrite(folder+L"\\apps.bin",&h,sizeof(h),paths.data(),DWORD(paths.size()*sizeof(AppPath)));
}
bool ReadAppPaths(const wstring& folder,std::map<uint64_t,wstring>& target){
    HANDLE f=CreateFileW((folder+L"\\apps.bin").c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    if(f==INVALID_HANDLE_VALUE)return false;
    AppsHeader h;LARGE_INTEGER size{};bool ok=false;
    if(GetFileSizeEx(f,&size)&&ExactRead(f,&h,sizeof(h))&&h.magic==0x31414342&&h.count<=MAX_APP_PATHS&&size.QuadPart==sizeof(h)+h.count*sizeof(AppPath)){
        std::vector<AppPath> v(h.count);
        if(ExactRead(f,v.data(),DWORD(v.size()*sizeof(AppPath)))){
            std::map<uint64_t,wstring> m;ok=true;
            for(auto& p:v){if(!wmemchr(p.identity,0,260)){ok=false;break;}m[p.id]=p.identity;}
            if(ok)target.swap(m);
        }
    }
    CloseHandle(f);return ok;
}
void WriteStatus(const wstring& s) {
    uint32_t length=uint32_t(s.size());AtomicWrite(data+L"\\refresh.status",&length,sizeof(length),s.c_str(),length*sizeof(wchar_t));
}
wstring ReadStatus(const wstring& directory=L""){
    HANDLE f=CreateFileW(((directory.empty()?data:directory)+L"\\refresh.status").c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);if(f==INVALID_HANDLE_VALUE)return L"Windows app history could not be refreshed.";
    uint32_t n=0;wstring s;
    if(ExactRead(f,&n,4)&&n<1024){s.resize(n);if(!ExactRead(f,s.data(),n*2))s.clear();}CloseHandle(f);return s;
}
bool CheckScheduledCache(uint64_t now,bool force=false){
    if(g.scheduledFolder.empty()||(!force&&!g.scheduledPending&&now>=g.cacheCheck&&now-g.cacheCheck<60))return false;
    g.cacheCheck=now;bool changed=false;WIN32_FILE_ATTRIBUTE_DATA file{};
    bool previousAvailable=g.hourlyAvailable;
    g.hourlyAvailable=GetFileAttributesW((g.scheduledFolder+L"\\hourly.enabled").c_str())!=INVALID_FILE_ATTRIBUTES;
    changed=previousAvailable!=g.hourlyAvailable;
    if(GetFileAttributesExW((g.scheduledFolder+L"\\energy.bin").c_str(),GetFileExInfoStandard,&file)&&CompareFileTime(&file.ftLastWriteTime,&g.scheduledStamp)!=0){
        std::vector<Energy> records;EnergyHeader header;
        if(ReadEnergy(g.scheduledFolder+L"\\energy.bin",records,header)){
            g.scheduledStamp=file.ftLastWriteTime;
            if(header.updated>=g.energyHeader.updated){
                g.energy.swap(records);g.energyHeader=header;g.notice.clear();changed=true;
                if(!g.readOnly)AtomicWrite(data+L"\\energy.bin",&header,sizeof(header),g.energy.data(),DWORD(g.energy.size()*sizeof(Energy)));
                // Older hourly helpers do not write apps.bin; keep any icons we already know.
                std::map<uint64_t,wstring> paths;
                if(ReadAppPaths(g.scheduledFolder,paths)){
                    g.appPaths.swap(paths);
                    if(!g.readOnly){std::vector<AppPath> list;for(auto& kv:g.appPaths){AppPath p;p.id=kv.first;wcsncpy(p.identity,kv.second.c_str(),259);list.push_back(p);}WriteAppPaths(data,list);}
                }
            }
            if(g.scheduledPending){g.scheduledPending=false;g.refreshing=false;changed=true;}
        }
    }
    if(GetFileAttributesExW((g.scheduledFolder+L"\\refresh.status").c_str(),GetFileExInfoStandard,&file)&&CompareFileTime(&file.ftLastWriteTime,&g.scheduledStatusStamp)!=0){
        g.scheduledStatusStamp=file.ftLastWriteTime;wstring status=ReadStatus(g.scheduledFolder);
        if(!status.empty()){g.notice=status;if(g.scheduledPending){g.scheduledPending=false;g.refreshing=false;}changed=true;}
    }
    if(g.scheduledPending&&GetTickCount64()-g.scheduledRequest>100000){
        g.scheduledPending=false;g.refreshing=false;g.notice=L"The hourly refresh job did not finish. Check its status in Windows Task Scheduler.";changed=true;
    }
    return changed;
}
int ExportEnergy() {
    HANDLE mutex=CreateMutexW(nullptr,TRUE,L"Local\\CellBatteryExport1");
    if(!mutex||GetLastError()==ERROR_ALREADY_EXISTS){if(mutex)CloseHandle(mutex);return 2;}
    wstring temp=data+L"\\srum-import.tmp.csv";
    wchar_t system[32768];UINT length=GetSystemDirectoryW(system,32768);
    if(!length||length>=32768){WriteStatus(L"Windows system directory is unavailable.");CloseHandle(mutex);return 1;}
    wstring exe=wstring(system)+L"\\powercfg.exe";
    wstring command=L"\""+exe+L"\" /srumutil /output \""+temp+L"\" /csv";
    STARTUPINFOW si{};si.cb=sizeof(si);si.dwFlags=STARTF_USESHOWWINDOW;si.wShowWindow=SW_HIDE;PROCESS_INFORMATION pi{};
    DWORD exit=1;
    bool launched=CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW|BELOW_NORMAL_PRIORITY_CLASS,nullptr,root.c_str(),&si,&pi)!=0;
    if(launched){CloseHandle(pi.hThread);DWORD wait=WaitForSingleObject(pi.hProcess,60000);if(wait!=WAIT_OBJECT_0){TerminateProcess(pi.hProcess,ERROR_TIMEOUT);WaitForSingleObject(pi.hProcess,5000);exit=ERROR_TIMEOUT;}else GetExitCodeProcess(pi.hProcess,&exit);CloseHandle(pi.hProcess);}
    int result=1;wstring error;
    if(launched&&exit==0){
        std::vector<Energy> records;EnergyHeader h;std::vector<AppPath> identities;
        if(ParseEnergy(temp,Now(),records,h,error,&identities)){
            // apps.bin first: a reader that sees the new energy.bin should find matching icons.
            WriteAppPaths(data,identities);
            if(AtomicWrite(data+L"\\energy.bin",&h,sizeof(h),records.data(),DWORD(records.size()*sizeof(Energy)))){WriteStatus(L"");result=0;}else error=L"Cannot save the app cache. Move Cell to a writable folder.";
        }
    }else error=exit==ERROR_TIMEOUT?L"Windows history took too long. Your previous cache is kept.":L"Windows denied or could not export app history. Try Refresh as administrator, or use Windows battery settings.";
    if(result)WriteStatus(error);DeleteFileW(temp.c_str());ReleaseMutex(mutex);CloseHandle(mutex);return result;
}

int Px(float p){return int(std::lround(p*g.scale));}
float WidgetScale(){return g.prefs.size==1?1.0f:g.prefs.size==2?1.3f:1.15f;}
Color Tint(Color c,int alpha){return Color(BYTE(alpha),c.GetR(),c.GetG(),c.GetB());}
void Rounded(Graphics& gr,RectF r,float radius,Color c) {
    GraphicsPath path;float d=radius*2;
    path.AddArc(r.X,r.Y,d,d,180,90);path.AddArc(r.GetRight()-d,r.Y,d,d,270,90);
    path.AddArc(r.GetRight()-d,r.GetBottom()-d,d,d,0,90);path.AddArc(r.X,r.GetBottom()-d,d,d,90,90);path.CloseFigure();
    SolidBrush b(c);gr.FillPath(&b,&path);
}
void Fonts(){
    // 0 small, 1 body, 2 label, 3 title (bold), 4 time left, 5 ring (bold), 6 draw (bold), 7 chip value (bold).
    const float sizes[]={11,12,13,14,15,34,18,13};
    for(size_t i=0;i<g.fonts.size();i++)g.fonts[i]=std::make_unique<Font>(L"Segoe UI",sizes[i],i==3||i==5||i==6||i==7?FontStyleBold:FontStyleRegular,UnitPixel);
}
void TextAt(Graphics& gr,const wstring& s,float x,float y,float width,int font,Color color,bool right=false){
    StringFormat fmt(StringFormat::GenericTypographic());fmt.SetFormatFlags(StringFormatFlagsNoWrap);fmt.SetTrimming(StringTrimmingEllipsisCharacter);
    if(right)fmt.SetAlignment(StringAlignmentFar);SolidBrush b(color);
    gr.DrawString(s.c_str(),int(s.size()),g.fonts[font].get(),RectF(x,y,width,76),&fmt,&b);
}
void Stroke(Graphics& gr,float x,float y,float x2,float y2,Color c,float width=1){Pen p(c,width);p.SetStartCap(LineCapRound);p.SetEndCap(LineCapRound);gr.DrawLine(&p,x,y,x2,y2);}
void Chevron(Graphics& gr,float x,float y,bool left,Color c){float d=left?-1.f:1.f;Stroke(gr,x-2*d,y-4,x+2*d,y,c,1.6f);Stroke(gr,x+2*d,y,x-2*d,y+4,c,1.6f);}
void VerticalChevron(Graphics& gr,float x,float y,bool up,Color c){float d=up?-1.f:1.f;Stroke(gr,x-4,y-2*d,x,y+2*d,c,1.6f);Stroke(gr,x,y+2*d,x+4,y-2*d,c,1.6f);}
void PinIcon(Graphics& gr,float x,float y,Color c){
    Stroke(gr,x-4,y-6,x+4,y-6,c);Stroke(gr,x-2,y-5,x-2,y,c);Stroke(gr,x+2,y-5,x+2,y,c);
    Stroke(gr,x-2,y,x-5,y+3,c);Stroke(gr,x+2,y,x+5,y+3,c);Stroke(gr,x-5,y+3,x+5,y+3,c);Stroke(gr,x,y+3,x,y+8,c);
}
void RefreshIcon(Graphics& gr,float x,float y,Color c){Pen p(c,1.5f);gr.DrawArc(&p,x-6.f,y-6.f,12.f,12.f,30.f,280.f);Stroke(gr,x+6,y-5,x+6,y,c,1.5f);Stroke(gr,x+6,y-5,x+1,y-5,c,1.5f);}
bool CanJoin(const Sample&a,const Sample&b){return !b.gap&&b.utc>a.utc&&b.utc-a.utc<=660&&a.percent>=0&&b.percent>=0;}
Color SourceColor(uint8_t source){return source==Battery?GREEN:source==Charging?GOLD:source==Plugged?BLUE:MUTED;}
const wchar_t* SourceName(uint8_t source){return source==Battery?L"on battery":source==Charging?L"charging":source==Plugged?L"plugged in":L"source unknown";}
bool IsLow(){return g.power.source==Battery&&g.power.percent>=0&&g.power.percent<=LOW_PERCENT;}
Color StateColor(){return IsLow()?RED:SourceColor(g.power.source);}
float ChartX(uint64_t utc,uint64_t start){return CHART_X+CHART_W*float(double(utc-start)/DAY);}
float ChartY(int percent){return CHART_Y+CHART_H-CHART_H*percent/100.f;}
// Does a logical-unit rectangle intersect the pixel dirty rectangle?
bool Touches(const RECT* dirty,float x,float y,float w,float h){return !dirty||(dirty->right>Px(x)&&dirty->left<Px(x+w)&&dirty->bottom>Px(y)&&dirty->top<Px(y+h));}
void TextCenter(Graphics& gr,const wstring& s,float x,float y,float width,int font,Color color){
    StringFormat fmt(StringFormat::GenericTypographic());fmt.SetFormatFlags(StringFormatFlagsNoWrap);fmt.SetTrimming(StringTrimmingEllipsisCharacter);
    fmt.SetAlignment(StringAlignmentCenter);SolidBrush b(color);gr.DrawString(s.c_str(),int(s.size()),g.fonts[font].get(),RectF(x,y,width,76),&fmt,&b);
}
// Nearest recorded sample to a chart x position, within about one sampling interval.
const Sample* HistoryAt(float x,uint64_t now){
    uint64_t start=now-DAY;double target=double(start)+double(x-CHART_X)/CHART_W*DAY,bestGap=420;const Sample* best=nullptr;
    for(auto& s:g.history){if(s.utc<start||s.utc>now)continue;double d=std::abs(double(s.utc)-target);if(d<bestGap){bestGap=d;best=&s;}}
    return best;
}
// Battery level curve over [start,end): a soft area and line per observed run (same source, no gap),
// a dot for a lone reading, and a faint dashed bridge across unobserved stretches up to 6 h
// (sleep, screen off, Cell closed) so the trend stays readable without inventing samples.
bool PaintLevelCurve(Graphics& gr,uint64_t start,uint64_t end,float x,float y,float w,float h,float lineWidth){
    bool drawn=false,bridge=false;PointF last;uint64_t lastUtc=0;double span=double(end-start);
    auto px=[&](uint64_t t){return x+w*float(double(t-start)/span);};auto py=[&](int p){return y+h-h*p/100.f;};
    size_t i=0,n=g.history.size();
    while(i<n){
        const Sample& first=g.history[i];
        if(first.utc<start||first.utc>end){++i;continue;}
        size_t j=i+1;
        while(j<n&&g.history[j].utc<=end&&g.history[j-1].source==first.source&&CanJoin(g.history[j-1],g.history[j]))++j;
        std::vector<PointF> points;for(size_t k=i;k<j;k++)points.emplace_back(px(g.history[k].utc),py(g.history[k].percent));
        Color c=SourceColor(first.source);
        if(bridge&&first.utc>lastUtc&&first.utc-lastUtc<=6*3600){
            Pen dash(Tint(MUTED,140),1.2f);dash.SetDashStyle(DashStyleDash);gr.DrawLine(&dash,last,points.front());
        }
        if(points.size()>=2){
            std::vector<PointF> area(points);area.emplace_back(points.back().X,y+h);area.emplace_back(points.front().X,y+h);
            SolidBrush fill(Tint(c,48));gr.FillPolygon(&fill,area.data(),int(area.size()));
            Pen line(c,lineWidth);line.SetLineJoin(LineJoinRound);line.SetStartCap(LineCapRound);line.SetEndCap(LineCapRound);
            gr.DrawLines(&line,points.data(),int(points.size()));
        }else{SolidBrush dot(c);gr.FillEllipse(&dot,points[0].X-lineWidth,points[0].Y-lineWidth,2*lineWidth,2*lineWidth);}
        drawn=true;last=points.back();lastUtc=g.history[j-1].utc;
        // A change of source with no gap continues from the shared sample; anything else is a gap.
        bool continues=j<n&&g.history[j].utc<=end&&CanJoin(g.history[j-1],g.history[j]);
        bridge=!continues;i=continues?j-1:j;
    }
    return drawn;
}
void PaintHistory(Graphics& gr,uint64_t now) {
    const float x=CHART_X,y=CHART_Y,w=CHART_W,h=CHART_H;
    Pen grid(LINE,1);grid.SetDashStyle(DashStyleDot);
    gr.DrawLine(&grid,x,y,x+w,y);gr.DrawLine(&grid,x,y+h/2,x+w,y+h/2);
    Stroke(gr,x,y+h,x+w,y+h,LINE);
    uint64_t start=now-DAY;
    bool drawn=PaintLevelCurve(gr,start,now,x,y,w,h,2.4f);
    if(!drawn)TextAt(gr,L"History builds while Cell runs",x,y+h/2-8,w,0,MUTED);
    else if(!g.history.empty()&&!g.hovering){
        auto&s=g.history.back();if(s.utc>=start&&s.utc<=now){SolidBrush b(SourceColor(s.source));gr.FillEllipse(&b,ChartX(s.utc,start)-3.f,ChartY(s.percent)-3.f,6.f,6.f);}
    }
    if(g.hovering){
        const Sample* s=HistoryAt(g.hoverX,now);float hx=s?ChartX(s->utc,start):g.hoverX;
        Stroke(gr,hx,y-2,hx,y+h,Tint(FG,90),1);
        if(s){SolidBrush ring(BG),dot(SourceColor(s->source));gr.FillEllipse(&ring,hx-4.5f,ChartY(s->percent)-4.5f,9.f,9.f);gr.FillEllipse(&dot,hx-3.f,ChartY(s->percent)-3.f,6.f,6.f);}
    }
}
void PaintLegend(Graphics& gr,uint64_t now){
    if(g.hovering){
        const Sample* s=HistoryAt(g.hoverX,now);
        if(s){
            wstring text=Clock(s->utc)+L"  "+std::to_wstring(s->percent)+L"% · "+SourceName(s->source);
            TextAt(gr,text,CHART_X,LEGEND_Y,CHART_W,1,SourceColor(s->source));
        }else{
            uint64_t t=now-DAY+uint64_t(double(std::clamp(g.hoverX,CHART_X,CHART_X+CHART_W)-CHART_X)/CHART_W*DAY);
            TextAt(gr,Clock(t)+L"  not recorded",CHART_X,LEGEND_Y,CHART_W,1,MUTED);
        }
        return;
    }
    TextAt(gr,L"24h",CHART_X,LEGEND_Y+1,26,0,MUTED);
    const std::pair<Color,const wchar_t*> keys[]={{GREEN,L"Battery"},{GOLD,L"Charging"},{BLUE,L"Plugged"}};
    float kx=CHART_X+32;
    for(auto& k:keys){SolidBrush b(k.first);gr.FillEllipse(&b,kx,LEGEND_Y+6,5.f,5.f);TextAt(gr,k.second,kx+9,LEGEND_Y+1,56,0,MUTED);kx+=k.second[0]==L'C'?64.f:56.f;}
}
wstring Wh(uint32_t mwh){if(mwh==UINT32_MAX||mwh==0)return L"—";wchar_t b[24];swprintf(b,24,mwh>=100000?L"%.0f Wh":L"%.1f Wh",mwh/1000.0);return b;}

// ---- Readings derived from what Cell has observed ----
uint32_t FullChargeMWh(){return g.power.fullMWh!=UINT32_MAX?g.power.fullMWh:g.facts.valid?g.facts.fullMWh:0;}
// Draw samples (W), one per visible sample (~2 s); reset whenever the power source changes.
size_t WattCount(){return std::min(g.wattsCount,g.watts.size());}
float WattAt(size_t i){size_t n=WattCount();return g.watts[(g.wattsCount-n+i)%g.watts.size()];}
double AverageWatts(size_t last){
    size_t n=WattCount(),k=std::min(n,last);double sum=0;size_t used=0;
    for(size_t i=n-k;i<n;i++)if(WattAt(i)>0){sum+=WattAt(i);++used;}
    return used?sum/used:0;
}
// Time-left range from how much the draw moved over the last few minutes (25th–75th percentile).
bool EtaRange(uint32_t& low,uint32_t& high){
    if(g.power.source!=Battery||g.power.remainingMWh==UINT32_MAX)return false;
    std::vector<float> v;for(size_t i=0;i<WattCount();i++)if(WattAt(i)>0.2f)v.push_back(WattAt(i));
    if(v.size()<30)return false;
    std::sort(v.begin(),v.end());
    double p25=v[v.size()/4],p75=v[v.size()*3/4],wh=g.power.remainingMWh/1000.0;
    if(p25<=0)return false;
    low=uint32_t(wh/p75*3600);high=uint32_t(wh/p25*3600);
    return high>low+5*60&&high<7*86400; // A narrow range adds nothing over the single estimate.
}
// Start of the current on-battery (or on-power) stretch, from recorded history.
bool StretchStart(uint64_t& utc,int& percent){
    if(g.history.empty()||g.power.percent<0)return false;
    bool battery=g.power.source==Battery;
    if((g.history.back().source==Battery)!=battery)return false;
    size_t i=g.history.size();
    while(i>1&&(g.history[i-2].source==Battery)==battery)--i;
    utc=g.history[i-1].utc;percent=g.history[i-1].percent;return true;
}
// Average on-battery draw from Cell's measured drain over the last 7 days; needs 2 h of data.
double TypicalWatts(uint64_t now){
    double mwh=0,seconds=0;
    for(auto& d:g.drain)if(d.hour+7*DAY>=now&&d.seconds>=600){mwh+=d.mwh;seconds+=d.seconds;}
    return seconds>=7200?mwh/1000.0/(seconds/3600.0):0;
}
wstring Span(uint64_t seconds){
    uint64_t m=(seconds+30)/60;wchar_t b[32];
    if(m>=60)swprintf(b,32,L"%lluh %02llum",m/60,m%60);else swprintf(b,32,L"%llum",m);return b;
}

// Approximate duration for small tiles: whole hours from 10 h up.
wstring About(uint32_t seconds){uint32_t m=(seconds+30)/60;return m>=600?L"≈"+std::to_wstring((m+30)/60)+L"h":L"≈"+EtaText(seconds);}

// ---- Battery tab primitives ----
constexpr float RING_CX=72,RING_CY=140,RING_R=52,RING_W=11,CHIP_Y=244,CHIP_H=50;
void Bolt(Graphics& gr,float cx,float cy,float s,Color c){
    PointF bolt[]={{cx+1*s,cy-6*s},{cx-4*s,cy+1*s},{cx-0.5f*s,cy+1*s},{cx-1.5f*s,cy+6*s},{cx+4*s,cy-1*s},{cx+0.5f*s,cy-1*s}};
    SolidBrush brush(c);gr.FillPolygon(&brush,bolt,6);
}
void PaintRing(Graphics& gr){
    const float cx=RING_CX,cy=RING_CY,r=RING_R;
    Pen track(TRACK,RING_W);gr.DrawEllipse(&track,cx-r,cy-r,2*r,2*r);
    float shown=std::clamp(g.ringShown,0.f,100.f);
    if(g.power.percent>=0&&shown>0.2f){
        Pen arc(StateColor(),RING_W);arc.SetStartCap(LineCapRound);arc.SetEndCap(LineCapRound);
        if(shown>=99.9f)gr.DrawEllipse(&arc,cx-r,cy-r,2*r,2*r);else gr.DrawArc(&arc,cx-r,cy-r,2*r,2*r,-90.f,360.f*shown/100.f);
    }
    wstring text=g.power.percent<0?L"—":std::to_wstring(g.power.percent)+L"%";
    TextCenter(gr,text,cx-r,cy-28,2*r,5,IsLow()?RED:FG);
    wstring sub;
    if(g.power.source==Battery)sub=g.power.seconds!=UINT32_MAX&&g.power.seconds>0?EtaText(g.power.seconds):L"on battery";
    else if(g.power.source==Charging)sub=g.power.fullSeconds!=UINT32_MAX?L"full in "+EtaText(g.power.fullSeconds):L"charging";
    else if(g.power.source==Plugged)sub=g.power.percent>=100?L"full":L"paused";
    TextCenter(gr,sub,cx-r,cy+15,2*r,1,MUTED);
    if(g.power.source==Charging)Bolt(gr,cx,cy-33,1.1f,GOLD);
}
void Chip(Graphics& gr,float x,float w,const wchar_t* label,const wstring& value,const wstring& sub,Color color){
    Rounded(gr,RectF(x,CHIP_Y,w,CHIP_H),8,TRACK);
    TextAt(gr,label,x+8,CHIP_Y+6,w-12,0,MUTED);
    TextAt(gr,value,x+8,CHIP_Y+19,w-12,7,color);
    TextAt(gr,sub,x+8,CHIP_Y+35,w-12,0,MUTED);
}
void PaintChips(Graphics& gr,uint64_t now){
    const float gap=6,w=(CHART_W-2*gap)/3;wchar_t b[48];
    int health=g.facts.HealthPercent();wstring value=L"—";Color color=FG;
    if(health>0){swprintf(b,48,L"%d%%",std::min(health,100));value=b;color=health>=80?FG:health>=60?GOLD:RED;}
    wstring sub=g.facts.valid&&g.facts.cycles>0?std::to_wstring(g.facts.cycles)+L" cycles":Wh(FullChargeMWh())+L" full";
    Chip(gr,CHART_X,w,L"Health",value,sub,color);
    uint64_t since=0;int from=-1;bool battery=g.power.source==Battery;
    if(StretchStart(since,from)&&now>=since&&g.power.percent>=0){
        int delta=g.power.percent-from;swprintf(b,48,delta>0?L"+%d%%":delta<0?L"−%d%%":L"%d%%",std::abs(delta));
        Chip(gr,CHART_X+w+gap,w,battery?L"Unplugged":L"On power",b,Span(now-since),FG);
    }else Chip(gr,CHART_X+w+gap,w,battery?L"Unplugged":L"On power",L"—",L"not recorded",FG);
    double typical=TypicalWatts(now);uint32_t full=FullChargeMWh();
    if(typical>0&&full){Chip(gr,CHART_X+2*(w+gap),w,L"Full charge",About(uint32_t(full/1000.0/typical*3600)),L"usual use",FG);}
    else Chip(gr,CHART_X+2*(w+gap),w,L"Full charge",L"Learning",L"needs 2 h",MUTED);
}
wstring StatusText(){
    if(!g.power.valid)return L"Power status unavailable";
    if(!g.power.present)return L"No battery detected";
    if(g.power.source==Charging)return L"Charging";
    if(g.power.source==Plugged)return g.power.percent>=100?L"Fully charged":L"Plugged in";
    if(g.power.source==Battery){
        if(IsLow())return g.power.saver?L"Low battery · saver on":L"Low battery";
        return g.power.saver?L"On battery · saver on":L"On battery";
    }
    return L"Power source unknown";
}
wstring MainEta(){
    if(!g.power.present)return L"Battery unavailable";
    if(g.power.source==Charging)return g.power.fullSeconds!=UINT32_MAX?L"Full in "+EtaText(g.power.fullSeconds):L"Charging";
    if(g.power.source==Plugged)return g.power.percent>=100?L"On AC power":L"Charging paused";
    if(g.power.source==SourceUnknown)return L"Waiting for Windows";
    return EtaText(g.power.seconds)+(g.power.seconds!=UINT32_MAX&&g.power.seconds>0?L" left":L"");
}
wstring EtaLabel(){
    if(!g.power.present)return L"A system battery is required";
    if(g.power.source==Charging)return g.power.fullSeconds!=UINT32_MAX?L"At current rate · charging slows near full":L"Windows has not reported a charge rate";
    if(g.power.source==Plugged)return g.power.percent>=100?L"Battery is full and resting":L"Held by a charge limit or smart charging";
    if(g.power.source==SourceUnknown)return L"Windows has not reported the source";
    if(g.power.windowsEta)return g.power.recentDrain?L"Windows estimate · recent draw averaged":L"Windows remaining-time estimate";
    if(g.power.recentDrain)return L"From the last 1–2 min of drain";
    if(g.power.seconds!=UINT32_MAX)return g.power.drainCapable?L"From reported rate · refining…":L"From Windows' reported discharge rate";
    return g.power.drainCapable?L"Measuring recent drain…":L"Waiting for a Windows estimate…";
}
// ---- Apps and Now tabs (card coordinates) ----
constexpr float CONTENT_Y=78,U_SEG_Y=76,U_FRESH_Y=108,U_LABEL_Y=125,U_CHART_Y=146,U_CHART_H=32,U_SUMMARY_Y=198,U_LIST_Y=220,U_ROW_H=27,U_FOOT_Y=362,
    N_LABEL_Y=104,N_CHART_Y=124,N_CHART_H=52;
// Readable names at display time, so caches written by older versions benefit too.
wstring DisplayName(const wstring& raw){
    static const std::pair<const wchar_t*,const wchar_t*> friendly[]={
        {L"claude",L"Claude"},{L"discord",L"Discord"},{L"slack",L"Slack"},{L"ms-teams",L"Microsoft Teams"},{L"msteams",L"Microsoft Teams"},
        {L"steam",L"Steam"},{L"steamwebhelper",L"Steam"},{L"obs64",L"OBS Studio"},{L"vlc",L"VLC"},{L"notepad",L"Notepad"},
        {L"windowsterminal",L"Windows Terminal"},{L"virtualboxvm",L"VirtualBox VM"},{L"zoom",L"Zoom"},{L"msedgewebview2",L"Edge WebView"},
        {L"searchhost",L"Windows Search"},{L"olk",L"Outlook"},{L"outlook",L"Outlook"},{L"winword",L"Word"},{L"excel",L"Excel"},
        {L"powerpnt",L"PowerPoint"},{L"taskmgr",L"Task Manager"},{L"onedrive",L"OneDrive"},{L"whatsapp",L"WhatsApp"},{L"telegram",L"Telegram"},
        {L"devenv",L"Visual Studio"},{L"python",L"Python"},{L"pythonw",L"Python"},{L"node",L"Node.js"},{L"javaw",L"Java"},{L"java",L"Java"},
        {L"powershell",L"PowerShell"},{L"pwsh",L"PowerShell"},{L"cmd",L"Command Prompt"},{L"msmpeng",L"Defender antivirus"},
        {L"avp",L"Kaspersky"},{L"avpui",L"Kaspersky"},{L"firefox",L"Firefox"},{L"chrome",L"Google Chrome"},{L"msedge",L"Microsoft Edge"},{L"code",L"Visual Studio Code"}
    };
    for(auto& p:friendly)if(_wcsicmp(raw.c_str(),p.first)==0)return p.second;
    // Packaged ids read Publisher.Product (Mozilla.Firefox): show the product, CamelCase split.
    auto dot=raw.find_last_of(L'.');
    if(dot!=wstring::npos&&dot>0&&dot+1<raw.size()&&raw.find(L' ')==wstring::npos&&iswupper(raw[0])&&iswupper(raw[dot+1])){
        wstring tail=raw.substr(dot+1);
        for(auto& p:friendly)if(_wcsicmp(tail.c_str(),p.first)==0)return p.second;
        wstring out;for(size_t i=0;i<tail.size();i++){if(i&&iswupper(tail[i])&&iswlower(tail[i-1]))out+=L' ';out+=tail[i];}
        return out;
    }
    // Executable names are often all lowercase (rustdesk): capitalise the first letter only.
    if(!raw.empty()&&iswlower(raw[0])&&std::none_of(raw.begin(),raw.end(),[](wchar_t c){return iswupper(c);})){wstring out=raw;out[0]=towupper(out[0]);return out;}
    return raw;
}
Color AvatarColor(uint64_t id){
    const Color palette[]={GREEN,BLUE,GOLD,Color(255,196,170,231),Color(255,129,212,207),Color(255,236,164,176)};
    return palette[(id^(id>>29))%6];
}
// Turns a SRUM identity into something the shell can parse: a DOS path or a packaged-app AUMID.
wstring ShellParsingName(const wstring& identity){
    if(identity.empty())return L"";
    if(identity.find(L'!')!=wstring::npos&&identity.find_first_of(L"\\/")==wstring::npos)return L"shell:AppsFolder\\"+identity;
    if(identity.size()>3&&identity[1]==L':'&&identity[2]==L'\\')return identity;
    if(_wcsnicmp(identity.c_str(),L"\\Device\\",8)==0){
        static std::vector<std::pair<wstring,wstring>> drives;static bool loaded=false;
        if(!loaded){loaded=true;for(wchar_t d=L'A';d<=L'Z';d++){wchar_t name[3]={d,L':',0},target[512];if(QueryDosDeviceW(name,target,512))drives.emplace_back(target,name);}}
        for(auto& drive:drives)
            if(identity.size()>drive.first.size()&&_wcsnicmp(identity.c_str(),drive.first.c_str(),drive.first.size())==0&&identity[drive.first.size()]==L'\\')
                return drive.second+identity.substr(drive.first.size());
    }
    return L"";
}
std::unique_ptr<Bitmap> BitmapFromHBITMAP(HBITMAP source){
    BITMAP bm{};if(!GetObjectW(source,sizeof(bm),&bm)||bm.bmWidth<=0||bm.bmHeight<=0||bm.bmWidth>256||bm.bmHeight>256)return nullptr;
    int w=bm.bmWidth,h=bm.bmHeight;BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(bi.bmiHeader);bi.bmiHeader.biWidth=w;bi.bmiHeader.biHeight=-h;
    bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
    std::vector<uint32_t> pixels(size_t(w)*h);HDC dc=GetDC(nullptr);int rows=GetDIBits(dc,source,0,h,pixels.data(),&bi,DIB_RGB_COLORS);ReleaseDC(nullptr,dc);
    if(rows!=h)return nullptr;
    bool alpha=false;for(auto p:pixels)if(p>>24){alpha=true;break;}
    if(!alpha)for(auto& p:pixels)p|=0xff000000;
    auto out=std::make_unique<Bitmap>(w,h,PixelFormat32bppPARGB);BitmapData d{};Rect r(0,0,w,h);
    if(out->LockBits(&r,ImageLockModeWrite,PixelFormat32bppPARGB,&d)!=Ok)return nullptr;
    for(int y=0;y<h;y++)memcpy(static_cast<BYTE*>(d.Scan0)+y*d.Stride,pixels.data()+size_t(y)*w,size_t(w)*4);
    out->UnlockBits(&d);return out;
}
// Icons load lazily for visible rows only and are cached (failures too) for the session.
Bitmap* IconFor(uint64_t key,const wstring& parsingName){
    auto found=g.icons.find(key);if(found!=g.icons.end())return found->second.get();
    std::unique_ptr<Bitmap> icon;IShellItemImageFactory* factory=nullptr;
    if(!parsingName.empty()&&SUCCEEDED(SHCreateItemFromParsingName(parsingName.c_str(),nullptr,IID_PPV_ARGS(&factory)))){
        HBITMAP bitmap=nullptr;
        if(SUCCEEDED(factory->GetImage(SIZE{32,32},SIIGBF_ICONONLY|SIIGBF_BIGGERSIZEOK,&bitmap))&&bitmap){icon=BitmapFromHBITMAP(bitmap);DeleteObject(bitmap);}
        factory->Release();
    }
    if(g.icons.size()>=64)g.icons.clear();
    Bitmap* raw=icon.get();g.icons[key]=std::move(icon);return raw;
}
Bitmap* AppIcon(uint64_t id){
    auto path=g.appPaths.find(id);
    return IconFor(id,path!=g.appPaths.end()?ShellParsingName(path->second):wstring());
}
// ---- Live view: per-app CPU time from one NtQuerySystemInformation call per sample ----
#pragma pack(push,8)
struct ProcessEntry {
    ULONG next,threads;LARGE_INTEGER privateWorkingSet;ULONG hardFaults,threadsPeak;ULONGLONG cycles;
    LARGE_INTEGER created,user,kernel;struct{USHORT length,maximum;PWSTR buffer;} image;LONG priority;HANDLE pid,parent;
};
#pragma pack(pop)
constexpr uint64_t LIVE_WINDOW_MS=30000,LIVE_PERIOD_MS=3900;
bool LiveActive(){return g.tab==2&&!g.hidden&&g.displayOn&&!g.locked&&!g.suspended;}
void StopLive(){g.liveSnaps.clear();g.live.clear();g.liveBuffer.clear();g.liveBuffer.shrink_to_fit();g.liveBusy=0;}
bool SampleLive(){
    using Query=LONG(WINAPI*)(ULONG,PVOID,ULONG,PULONG);
    static Query query=reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQuerySystemInformation"));
    if(!query)return false;
    if(g.liveBuffer.size()<512*1024)g.liveBuffer.resize(512*1024);
    ULONG needed=0;LONG status=-1;
    for(int tries=0;tries<4;tries++){ // STATUS_INFO_LENGTH_MISMATCH: grow and retry.
        status=query(5/*SystemProcessInformation*/,g.liveBuffer.data(),ULONG(g.liveBuffer.size()),&needed);
        if(status!=LONG(0xC0000004))break;
        g.liveBuffer.resize(std::min<size_t>(std::max<size_t>(size_t(needed)+65536,g.liveBuffer.size()*2),16*1024*1024));
    }
    if(status<0)return false;
    State::LiveSnapshot snap;snap.tick=GetTickCount64();
    for(size_t offset=0;offset+sizeof(ProcessEntry)<=g.liveBuffer.size();){
        auto* p=reinterpret_cast<const ProcessEntry*>(g.liveBuffer.data()+offset);
        if(p->pid){ // PID 0 is the idle process.
            wstring image=p->image.buffer&&p->image.length?wstring(p->image.buffer,p->image.length/2):L"System";
            wstring name=DisplayName(AppName(image));
            snap.cpu[name]+=uint64_t(p->user.QuadPart+p->kernel.QuadPart);
            snap.pid.emplace(name,uint32_t(uintptr_t(p->pid)));
        }
        if(!p->next)break;offset+=p->next;
    }
    g.liveSnaps.push_back(std::move(snap));
    // Keep a ~30 s window: drop the oldest while the next one still spans the window.
    while(g.liveSnaps.size()>2&&g.liveSnaps.back().tick-g.liveSnaps[1].tick>=LIVE_WINDOW_MS)g.liveSnaps.erase(g.liveSnaps.begin());
    g.live.clear();g.liveBusy=0;
    if(g.liveSnaps.size()<2)return true;
    auto& a=g.liveSnaps.front();auto& b=g.liveSnaps.back();
    double capacity=double(b.tick-a.tick)*10000.0*std::max<DWORD>(1,GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)),busy=0;
    for(auto& kv:b.cpu){
        auto before=a.cpu.find(kv.first);if(before==a.cpu.end()||kv.second<=before->second)continue; // Exited or new processes: no reliable delta.
        double used=double(kv.second-before->second);busy+=used;
        State::LiveApp app;app.name=kv.first;app.cpu=capacity>0?100*used/capacity:0;app.pid=b.pid[kv.first];g.live.push_back(app);
    }
    for(auto& app:g.live)app.share=busy>0?100*app.cpu*capacity/100/busy:0;
    std::sort(g.live.begin(),g.live.end(),[](const State::LiveApp& x,const State::LiveApp& y){return x.cpu>y.cpu;});
    if(g.live.size()>40)g.live.resize(40);
    g.liveBusy=capacity>0?100*busy/capacity:0;
    return true;
}
Bitmap* LiveIcon(const State::LiveApp& app){
    auto known=g.livePaths.find(app.name);
    if(known==g.livePaths.end()){
        wstring path;HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,app.pid);
        if(process){wchar_t buffer[MAX_PATH*2];DWORD size=MAX_PATH*2;if(QueryFullProcessImageNameW(process,0,buffer,&size))path.assign(buffer,size);CloseHandle(process);}
        if(g.livePaths.size()>256)g.livePaths.clear();
        known=g.livePaths.emplace(app.name,path).first;
    }
    return IconFor(Hash(L"live:"+app.name),known->second);
}
Color FilterAccent(){return g.filter==1?BLUE:GREEN;}
bool LiveTab(){return g.tab==2;}
double HourEnergy(int i){return g.view.battery[i]+g.view.plugged[i]+g.view.unknown[i];}
int UsageHourAt(float x){return x<CHART_X||x>=CHART_X+CHART_W?-1:std::clamp(int((x-CHART_X)/(CHART_W/24)),0,23);}
size_t ListCount(){return LiveTab()?g.live.size():g.view.apps.size();}
int ListPages(){return std::max(1,int((ListCount()+APP_ROWS-1)/APP_ROWS));}
wstring WhText(double mj){
    double wh=mj/3.6e6;wchar_t b[32];
    if(wh<0.005)return L"<0.01 Wh";
    swprintf(b,32,wh<10?L"%.2f Wh":L"%.1f Wh",wh);return b;
}
wstring UsageFreshness(uint64_t now){
    if(g.refreshing)return L"Refreshing Windows history…";
    if(!g.notice.empty())return L"Refresh issue · details in the menu";
    if(!g.energyHeader.updated)return L"Windows' own estimates · refresh to load";
    auto age=now>=g.energyHeader.updated?(now-g.energyHeader.updated)/60:0;
    wstring s=age<1?L"Refreshed just now":age<60?L"Refreshed "+std::to_wstring(age)+L"m ago":L"Refreshed "+std::to_wstring(age/60)+L"h ago";
    return s+(g.hourlyAvailable?L" · hourly":L" · on demand");
}
// One line under the chart that puts the numbers in context.
wstring UsageSummary(){
    wchar_t b[200];
    if(LiveTab()){
        if(g.liveSnaps.size()<2)return L"Measuring CPU use…";
        swprintf(b,200,L"CPU %.0f%% busy · top apps, last 30 s",g.liveBusy);return b;
    }
    if(g.appSelected){
        const AppTotal* app=nullptr;for(auto& a:g.view.apps)if(a.name==g.appName){app=&a;break;}
        if(!app)return L"No energy for this app in the current selection";
        wstring s=WhText(app->mj);
        if(uint32_t full=FullChargeMWh()){swprintf(b,200,L" (%.1f%% of a charge)",app->mj/3600.0/full*100);s+=b;}
        double parts=0;for(double v:app->parts)parts+=v;
        if(parts>0){
            const wchar_t* labels[PART_COUNT]={L"CPU",L"GPU",L"Screen",L"Net",L"Disk",L"Other"};
            std::vector<std::pair<double,int>> order;for(int k=0;k<PART_COUNT;k++)if(app->parts[k]>0)order.emplace_back(app->parts[k],k);
            std::sort(order.rbegin(),order.rend());
            for(size_t k=0;k<order.size()&&k<2;k++){swprintf(b,200,L" · %ls %.0f%%",labels[order[k].second],100*order[k].first/parts);s+=b;}
        }
        return s;
    }
    if(g.filter==0){
        double measured=0,covered=0;
        for(int i=0;i<24;i++)if((g.selected<0||g.selected==i)&&g.view.measured[i]>0){measured+=g.view.measured[i];covered+=g.view.battery[i];}
        if(measured>0){swprintf(b,200,L"Battery lost %ls · Windows covers %.0f%%",WhText(measured).c_str(),std::min(100.0,100*covered/measured));return b;}
        swprintf(b,200,L"Apps used %ls on battery",WhText(g.view.total).c_str());return b;
    }
    double all=g.view.total+g.view.unassigned;
    if(all<=0)return L"";
    swprintf(b,200,L"Apps used %ls · %.0f%% unassigned",WhText(g.view.total).c_str(),100*g.view.unassigned/all);return b;
}
void PaintAppIcon(Graphics& gr,Bitmap* icon,const wstring& name,uint64_t key,float y){
    if(icon){
        auto mode=gr.GetInterpolationMode();gr.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        gr.DrawImage(icon,RectF(CHART_X,y,17,17));gr.SetInterpolationMode(mode);
    }else{
        Color c=AvatarColor(key);SolidBrush badge(Tint(c,48));gr.FillEllipse(&badge,CHART_X-1,y-1,19.f,19.f);
        TextCenter(gr,name.empty()?L"·":wstring(1,wchar_t(towupper(name[0]))),CHART_X-1,y+1,19,7,c);
    }
}
// Row layout shared by the Apps and Now lists.
void PaintRow(Graphics& gr,int row,float ry,Bitmap* icon,const wstring& name,uint64_t key,const wstring& detail,const wstring& value,double share,Color accent,bool chosen){
    if(chosen)Rounded(gr,RectF(6,ry-3,228,U_ROW_H-1),6,Tint(accent,30));
    else if(g.hoverRow==row)Rounded(gr,RectF(6,ry-3,228,U_ROW_H-1),6,Tint(FG,14));
    PaintAppIcon(gr,icon,name,key,ry);
    TextAt(gr,name,36,ry,detail.empty()?140:102,2,FG);
    if(!detail.empty())TextAt(gr,detail,136,ry+2,54,0,MUTED,true);
    TextAt(gr,value,176,ry,52,7,chosen?FG:accent,true);
    Rounded(gr,RectF(36,ry+19,192,4),2,TRACK);
    Rounded(gr,RectF(36,ry+19,std::max(4.f,float(192*share/100)),4),2,accent);
}
void PaintLiveChart(Graphics& gr,float y,float h){
    const float x=CHART_X,w=CHART_W;
    size_t n=WattCount();float peak=1;for(size_t i=0;i<n;i++)peak=std::max(peak,WattAt(i));
    Pen grid(LINE,1);grid.SetDashStyle(DashStyleDot);gr.DrawLine(&grid,x,y,x+w,y);gr.DrawLine(&grid,x,y+h/2,x+w,y+h/2);
    if(n>=2){
        std::vector<PointF> points;
        for(size_t i=0;i<n;i++)points.emplace_back(x+w-float(n-1-i)*w/float(g.watts.size()-1),y+h-h*WattAt(i)/peak);
        std::vector<PointF> area(points);area.emplace_back(points.back().X,y+h);area.emplace_back(points.front().X,y+h);
        Color c=g.power.source==Charging?GOLD:GREEN;
        SolidBrush fill(Tint(c,45));gr.FillPolygon(&fill,area.data(),int(area.size()));
        Pen line(c,1.8f);line.SetLineJoin(LineJoinRound);gr.DrawLines(&line,points.data(),int(points.size()));
        wchar_t b[24];swprintf(b,24,L"peak %.0f W",peak);TextAt(gr,b,x+w-80,N_LABEL_Y,80,0,MUTED,true);
    }
    Stroke(gr,x,y+h+0.5f,x+w,y+h+0.5f,LINE);
    TextAt(gr,L"5 min ago",x,y+h+4,80,0,MUTED);TextAt(gr,L"now",x+w-45,y+h+4,45,0,MUTED,true);
}
void PaintHourChart(Graphics& gr,Color accent){
    const float x=CHART_X,y=U_CHART_Y,w=CHART_W,h=U_CHART_H,step=w/24,barW=step-2.f;
    double peak=0;for(int i=0;i<24;i++)peak=std::max({peak,HourEnergy(i),g.view.measured[i]});
    for(int i=0;i<24;i++){
        float left=x+i*step;
        if(g.selected==i)Rounded(gr,RectF(left-1,y-4,step,h+6),3,Tint(accent,34));
        else if(g.hoverHour==i)Rounded(gr,RectF(left-1,y-4,step,h+6),3,Tint(FG,16));
        if(peak<=0){Rounded(gr,RectF(left,y+h-3,barW,3),1.5f,LINE);continue;}
        bool dim=(g.selected>=0&&g.selected!=i)||g.appSelected;
        const double values[]={g.view.battery[i],g.view.plugged[i],g.view.unknown[i]};
        const Color colors[]={GREEN,BLUE,MUTED}; // Same meaning as the battery history: plugged is blue.
        float yy=y+h;
        for(int k=0;k<3;k++){
            if(values[k]<=0)continue;float bh=std::max(1.5f,float(values[k]/peak*h));yy-=bh;
            Rounded(gr,RectF(left,yy,barW,bh),std::min(1.6f,bh/2),Tint(colors[k],dim?60:220));
        }
        // Outline: energy the battery actually lost in that hour, measured by Cell.
        if(g.view.measured[i]>0){
            float mh=std::max(2.f,float(g.view.measured[i]/peak*h));Pen outline(Tint(FG,dim?70:160),1);
            gr.DrawRectangle(&outline,left+0.5f,y+h-mh,barW-1,mh);
        }
        if(g.appSelected&&g.view.app[i]>0){float ah=std::max(2.f,float(g.view.app[i]/peak*h));Rounded(gr,RectF(left,y+h-ah,barW,ah),1.2f,FG);}
    }
    Stroke(gr,x,y+h+0.5f,x+w,y+h+0.5f,LINE);
    TextAt(gr,Clock(g.view.hours[0]),x,y+h+4,60,0,MUTED);
    TextCenter(gr,Clock(g.view.hours[12]),x+w/2-30,y+h+4,60,0,MUTED);
    TextAt(gr,L"now",x+w-45,y+h+4,45,0,MUTED,true);
}
void PaintFooterPages(Graphics& gr,const wstring& foot){
    TextAt(gr,foot,CHART_X,U_FOOT_Y+3,ListPages()>1?118:216,0,MUTED);
    if(ListPages()>1)TextAt(gr,std::to_wstring(g.page+1)+L" / "+std::to_wstring(ListPages()),122,U_FOOT_Y+3,48,0,MUTED,true);
}
void PaintAppsTab(Graphics& gr,uint64_t now){
    Color accent=FilterAccent();
    Rounded(gr,RectF(CHART_X,U_SEG_Y,164,26),8,TRACK); // Segmented-control track; the filter buttons sit on it.
    TextAt(gr,UsageFreshness(now),CHART_X,U_FRESH_Y,216,0,g.notice.empty()?MUTED:GOLD);
    wstring label;Color labelColor=MUTED;bool legend=false;
    if(g.hoverHour>=0){
        int i=g.hoverHour;wchar_t b[120];
        wstring span=Clock(g.view.hours[i])+L"–"+Clock(g.view.hours[i]+3600);
        if(g.view.measured[i]>0)swprintf(b,120,L"%ls · lost %ls · apps %ls",span.c_str(),WhText(g.view.measured[i]).c_str(),WhText(HourEnergy(i)).c_str());
        else swprintf(b,120,L"%ls · apps %ls",span.c_str(),WhText(HourEnergy(i)).c_str());
        label=b;labelColor=FG;
    }else if(g.selected>=0){label=Clock(g.view.hours[g.selected])+L"–"+Clock(g.view.hours[g.selected]+3600)+L" selected";labelColor=accent;}
    else if(g.appSelected){label=g.appName+L" · last 24 hours";labelColor=FG;}
    else{label=L"Last 24 hours";legend=g.filter!=1;}
    TextAt(gr,label,CHART_X,U_LABEL_Y,legend?140:190,1,labelColor);
    if(legend){Pen outline(Tint(FG,160),1);gr.DrawRectangle(&outline,160.5f,U_LABEL_Y+3.5f,6.f,9.f);TextAt(gr,L"battery lost",170,U_LABEL_Y+1,58,0,MUTED,true);}
    PaintHourChart(gr,accent);
    TextAt(gr,UsageSummary(),CHART_X,U_SUMMARY_Y,216,0,g.appSelected?FG:MUTED);
    if(g.view.apps.empty()){
        wstring message=g.energyHeader.updated?L"No app energy for this selection":L"Load Windows app history";
        TextAt(gr,message,CHART_X,U_LIST_Y+6,216,2,FG);
        TextAt(gr,g.refreshing?L"Windows is preparing the export…":!g.notice.empty()?g.notice:L"Refresh ↻ loads Windows' saved estimates.",CHART_X,U_LIST_Y+28,216,0,MUTED);
        TextAt(gr,L"Or open Now for live CPU use without waiting.",CHART_X,U_LIST_Y+46,216,0,MUTED);
    }else{
        for(int row=0;row<APP_ROWS;row++){
            size_t idx=size_t(g.page*APP_ROWS+row);if(idx>=g.view.apps.size())break;
            auto& a=g.view.apps[idx];wchar_t b[24];double share=g.view.total>0?100*a.mj/g.view.total:0;swprintf(b,24,share<1?L"<1%%":L"%.0f%%",share);
            PaintRow(gr,row,U_LIST_Y+row*U_ROW_H,AppIcon(a.id),a.name,a.id,WhText(a.mj),b,share,accent,g.appSelected&&a.name==g.appName);
        }
    }
    PaintFooterPages(gr,g.energyHeader.latest?L"Data through "+Clock(g.energyHeader.latest):L"");
}
void PaintNowTab(Graphics& gr){
    TextAt(gr,L"Live CPU use and battery draw · nothing stored",CHART_X,U_SEG_Y+5,216,0,MUTED);
    TextAt(gr,g.power.source==Battery?L"Battery draw, last 5 minutes":g.power.source==Charging?L"Charging rate, last 5 minutes":L"On power · battery draw not measured",CHART_X,N_LABEL_Y,150,1,FG);
    PaintLiveChart(gr,N_CHART_Y,N_CHART_H);
    TextAt(gr,UsageSummary(),CHART_X,U_SUMMARY_Y,216,0,MUTED);
    if(g.live.empty())TextAt(gr,L"First results appear in a few seconds.",CHART_X,U_LIST_Y+6,216,0,MUTED);
    for(int row=0;row<APP_ROWS;row++){
        size_t idx=size_t(g.page*APP_ROWS+row);if(idx>=g.live.size())break;
        auto& a=g.live[idx];wchar_t b[24];swprintf(b,24,a.cpu<0.1?L"<0.1%%":L"%.1f%%",a.cpu);
        PaintRow(gr,row,U_LIST_Y+row*U_ROW_H,LiveIcon(a),a.name,Hash(a.name),L"CPU",b,a.share,GREEN,false);
    }
    PaintFooterPages(gr,L"Updates every 4 s while open");
}
// Right of the ring: live draw, a 5-minute sparkline, and the power state.
void PaintDrawColumn(Graphics& gr){
    const float x=138,w=90;wchar_t b[40];
    bool charging=g.power.source==Charging;
    TextAt(gr,charging?L"Charging at":g.power.source==Battery?L"Drawing":L"Battery",x,84,w,0,MUTED);
    if(g.power.watts>0){swprintf(b,40,L"%.1f W",g.power.watts);TextAt(gr,b,x,98,w,6,FG);}
    else TextAt(gr,L"—",x,98,w,6,MUTED);
    double avg=AverageWatts(g.watts.size());
    if(avg>0){swprintf(b,40,L"5 min avg %.1f W",avg);TextAt(gr,b,x,122,w,0,MUTED);}
    const float sy=140,sh=26;size_t n=WattCount();
    Stroke(gr,x,sy+sh,x+w,sy+sh,LINE);
    if(n>=2){
        float peak=0.5f;for(size_t i=0;i<n;i++)peak=std::max(peak,WattAt(i));
        std::vector<PointF> points;for(size_t i=0;i<n;i++)points.emplace_back(x+w-float(n-1-i)*w/float(g.watts.size()-1),sy+sh-sh*WattAt(i)/peak);
        std::vector<PointF> area(points);area.emplace_back(points.back().X,sy+sh);area.emplace_back(points.front().X,sy+sh);
        Color c=charging?GOLD:StateColor();SolidBrush fill(Tint(c,45));gr.FillPolygon(&fill,area.data(),int(area.size()));
        Pen line(c,1.5f);line.SetLineJoin(LineJoinRound);gr.DrawLines(&line,points.data(),int(points.size()));
    }
    TextAt(gr,StatusText(),x,174,w,1,StateColor());
}
void PaintBatteryTab(Graphics& gr,uint64_t now,const RECT* dirty){
    if(Touches(dirty,12,80,124,124))PaintRing(gr);
    if(Touches(dirty,134,80,98,118))PaintDrawColumn(gr);
    if(Touches(dirty,10,198,220,42)){
        wstring eta=MainEta();uint32_t low=0,high=0;
        if(g.power.source==Battery&&EtaRange(low,high))eta=EtaText(low)+L" – "+EtaText(high)+L" left";
        TextAt(gr,eta,CHART_X,201,216,4,FG);
        TextAt(gr,EtaLabel(),CHART_X,222,216,0,MUTED);
    }
    if(Touches(dirty,10,CHIP_Y-2,220,CHIP_H+4))PaintChips(gr,now);
    if(Touches(dirty,CHART_X-4,CHART_Y-6,CHART_W+8,CHART_H+10))PaintHistory(gr,now);
    if(Touches(dirty,0,LEGEND_Y-4,CW,22))PaintLegend(gr,now);
}
// Week tab: measured battery use per local day, and the 7-day level curve.
struct DayUse { uint64_t start=0; double mwh=0,seconds=0; };
std::array<DayUse,7> WeekDays(uint64_t now){
    std::array<DayUse,7> days{};
    SYSTEMTIME local=LocalTime(now),utc{};local.wHour=local.wMinute=local.wSecond=local.wMilliseconds=0;
    uint64_t today=now/DAY*DAY;
    if(TzSpecificLocalTimeToSystemTime(nullptr,&local,&utc)){FILETIME f{};SystemTimeToFileTime(&utc,&f);ULARGE_INTEGER u;u.LowPart=f.dwLowDateTime;u.HighPart=f.dwHighDateTime;today=u.QuadPart/10000000ULL-11644473600ULL;}
    for(int i=0;i<7;i++)days[i].start=today-uint64_t(6-i)*DAY;
    for(auto& d:g.drain){
        if(d.hour+3600<=days[0].start)continue;
        int i=int((int64_t(d.hour)-int64_t(days[0].start))/int64_t(DAY));
        if(i>=0&&i<7){days[i].mwh+=d.mwh;days[i].seconds+=d.seconds;}
    }
    return days;
}
void PaintWeekTab(Graphics& gr,uint64_t now){
    auto days=WeekDays(now);double total=0,seconds=0;for(auto& d:days){total+=d.mwh;seconds+=d.seconds;}
    const float gap=6,w=(CHART_W-2*gap)/3,y0=CHIP_Y;wchar_t b[48];
    // Chips reuse the battery-tab style at the top of this tab.
    auto chip=[&](float x,const wchar_t* label,const wstring& value,const wstring& sub,Color c){
        Rounded(gr,RectF(x,80,w,CHIP_H),8,TRACK);TextAt(gr,label,x+8,86,w-12,0,MUTED);TextAt(gr,value,x+8,99,w-12,7,c);TextAt(gr,sub,x+8,115,w-12,0,MUTED);
    };
    (void)y0;
    double typical=TypicalWatts(now);uint32_t full=FullChargeMWh();
    if(typical>0&&full)chip(CHART_X,L"Full charge",About(uint32_t(full/1000.0/typical*3600)),L"usual use",FG);
    else chip(CHART_X,L"Full charge",L"Learning",L"needs 2 h",MUTED);
    swprintf(b,48,total>0?L"%.0f Wh":L"—",total/1000);chip(CHART_X+w+gap,L"Used",b,L"last 7 days",FG);
    if(typical>0)swprintf(b,48,L"%.1f W",typical);else wcscpy(b,L"—");chip(CHART_X+2*(w+gap),L"Usual draw",b,L"on battery",FG);
    // Daily bars.
    TextAt(gr,L"Battery used per day",CHART_X,138,150,1,FG);TextAt(gr,L"Wh, measured",148,139,80,0,MUTED,true);
    const float by=172,bh=54,col=CHART_W/7;double peak=1;for(auto& d:days)peak=std::max(peak,d.mwh);
    const wchar_t* names[]={L"Sun",L"Mon",L"Tue",L"Wed",L"Thu",L"Fri",L"Sat"};
    for(int i=0;i<7;i++){
        float cx=CHART_X+col*i+col/2;bool today=i==6;
        Rounded(gr,RectF(cx-9,by,18,bh),4,TRACK);
        if(days[i].mwh>0){
            float h=std::max(4.f,float(days[i].mwh/peak*bh));Rounded(gr,RectF(cx-9,by+bh-h,18,h),4,today?GREEN:Tint(GREEN,170));
            swprintf(b,48,L"%.0f",days[i].mwh/1000);TextCenter(gr,b,cx-15,by+bh-h-15,30,0,MUTED);
        }
        TextCenter(gr,names[LocalTime(days[i].start+DAY/2).wDayOfWeek],cx-16,by+bh+4,32,0,today?FG:MUTED);
    }
    // Level curve across the same 7 days.
    TextAt(gr,L"Battery level",CHART_X,252,150,1,FG);
    const float ly=272,lh=70;
    Pen grid(LINE,1);grid.SetDashStyle(DashStyleDot);
    for(int i=1;i<7;i++){float x=CHART_X+col*i;gr.DrawLine(&grid,x,ly,x,ly+lh);}
    Stroke(gr,CHART_X,ly+lh,CHART_X+CHART_W,ly+lh,LINE);
    if(!PaintLevelCurve(gr,days[0].start,days[0].start+7*DAY,CHART_X,ly,CHART_W,lh,1.8f))TextAt(gr,L"Fills in as Cell runs through the week",CHART_X,ly+lh/2-8,216,0,MUTED);
    // Footer: the heaviest day, or how the week fills in.
    int heavy=-1;for(int i=0;i<7;i++)if(days[i].mwh>0&&(heavy<0||days[i].mwh>days[heavy].mwh))heavy=i;
    wstring foot=heavy>=0?L"Heaviest day: "+wstring(names[LocalTime(days[heavy].start+DAY/2).wDayOfWeek])+L" · "+WhText(days[heavy].mwh*3600):L"Cell measures real drain while it runs";
    (void)seconds;TextAt(gr,foot,CHART_X,U_FOOT_Y+3,216,0,MUTED);
}
void PaintHeader(Graphics& gr,bool tabs){
    SolidBrush dot(StateColor());gr.FillEllipse(&dot,14.f,17.f,7.f,7.f);TextAt(gr,L"cell",27,11,40,3,FG);
    wstring quick=g.power.percent>=0?std::to_wstring(g.power.percent)+L"%":L"—";
    if(g.power.source==Battery&&g.power.seconds!=UINT32_MAX&&g.power.seconds>0)quick+=L" · "+EtaText(g.power.seconds);
    else if(g.power.source==Charging)quick+=L" · charging";
    // The ring already shows this on the battery tab; elsewhere it stays in view here.
    if(!tabs)TextAt(gr,L"click to open",64,14,164,0,MUTED,true);
    else if(g.tab!=0)TextAt(gr,quick,62,13,108,1,MUTED);
    if(tabs)Rounded(gr,RectF(12,42,216,26),8,TRACK);
}
void Paint(Graphics& gr,int width,int height,uint64_t now,const RECT* dirty=nullptr){
    gr.SetSmoothingMode(SmoothingModeAntiAlias);gr.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    SolidBrush background(BG);gr.FillRectangle(&background,0,0,width,height);gr.ScaleTransform(g.scale,g.scale);
    if(Touches(dirty,0,0,CW,70))PaintHeader(gr,true);
    bool body=Touches(dirty,0,70,CW,CH-70);
    switch(g.tab){
    case 0:PaintBatteryTab(gr,now,dirty);break;
    case 1:if(body)PaintAppsTab(gr,now);break;
    case 2:if(body)PaintNowTab(gr);break;
    default:if(body)PaintWeekTab(gr,now);break;
    }
}
// The taskbar hover card: the battery tab without tabs or buttons.
constexpr float PEEK_SHIFT=CONTENT_Y-44,PEEK_H=CH-PEEK_SHIFT;
void PaintPeek(Graphics& gr,int width,int height,uint64_t now){
    gr.SetSmoothingMode(SmoothingModeAntiAlias);gr.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    SolidBrush background(BG);gr.FillRectangle(&background,0,0,width,height);gr.ScaleTransform(g.scale,g.scale);
    PaintHeader(gr,false);gr.TranslateTransform(0,-PEEK_SHIFT);
    bool hovering=g.hovering;g.hovering=false;PaintBatteryTab(gr,now,nullptr);g.hovering=hovering;
}
wstring ButtonCaption(int id){
    switch(id){case Menu:return L"Menu";case Refresh:return g.hourlyAvailable?L"Refresh Windows app history":L"Refresh Windows app history (administrator consent)";
    case FilterBattery:return L"Battery";case FilterPlugged:return L"Plugged";case FilterAll:return L"All";case ClearHour:return L"Clear selection";
    case TabBattery:return L"Battery";case TabApps:return L"Apps";case TabNow:return L"Now";case TabWeek:return L"Week";
    case PreviousPage:return L"Previous apps";case NextPage:return L"Next apps";case DesktopPin:return g.desktopWanted?L"Unpin from desktop":L"Pin to desktop (behind app windows)";}return L"";
}
bool IsTab(int id){return id>=TabBattery&&id<=TabWeek;}
bool IsFilter(int id){return id==FilterBattery||id==FilterPlugged||id==FilterAll;}
bool IsSelected(int id){return (id==DesktopPin&&g.desktop)||(IsTab(id)&&g.tab==id-TabBattery)||(id==FilterBattery&&g.filter==0)||(id==FilterPlugged&&g.filter==1)||(id==FilterAll&&g.filter==2);}
void DrawButton(Graphics& gr,int id,float w,float h,bool pressed=false,bool focus=false){
    gr.SetSmoothingMode(SmoothingModeAntiAlias);gr.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    bool segment=IsTab(id)||IsFilter(id);
    // Overfill by a pixel: anti-aliased edges would otherwise let the button's own grey erase show through.
    SolidBrush parentBrush(segment?TRACK:BG);gr.FillRectangle(&parentBrush,-1.f,-1.f,w+2,h+2);Color fg=MUTED;
    bool selected=IsSelected(id);
    if(selected&&segment){Rounded(gr,RectF(0,0,w,h),6,SEL);fg=IsTab(id)?FG:id==FilterPlugged?BLUE:id==FilterAll?FG:GREEN;}
    else if(selected){Rounded(gr,RectF(0,0,w,h),6,Tint(GREEN,26));fg=GREEN;}
    if(pressed)Rounded(gr,RectF(0,0,w,h),6,Tint(FG,26));
    if(segment)TextCenter(gr,ButtonCaption(id),0,(h-15)/2,w,1,fg);
    else switch(id){
    case DesktopPin:PinIcon(gr,w/2,h/2,fg);break;
    case Menu:{SolidBrush b(fg);for(int i=0;i<3;i++)gr.FillEllipse(&b,w/2-6+i*5,h/2-1,2.4f,2.4f);break;}
    case Refresh:RefreshIcon(gr,w/2,h/2,g.refreshing?LINE:fg);break;
    case ClearHour:Stroke(gr,w/2-3,h/2-3,w/2+3,h/2+3,fg);Stroke(gr,w/2+3,h/2-3,w/2-3,h/2+3,fg);break;
    case PreviousPage:Chevron(gr,w/2,h/2,true,fg);break;
    case NextPage:Chevron(gr,w/2,h/2,false,fg);break;
    default:break;
    }
    if(focus){Pen p(Tint(GREEN,170),1);p.SetDashStyle(DashStyleDot);gr.DrawRectangle(&p,2.f,2.f,w-5,h-5);}
}
// tabs: bit mask of the tabs (1 << g.tab) on which the control is shown.
struct ControlPosition { int id,x,y,w,h,tabs; };
const ControlPosition controls[]={
    {DesktopPin,174,8,26,26,15},{Menu,202,8,26,26,15},
    {TabBattery,14,44,53,22,15},{TabApps,67,44,53,22,15},{TabNow,120,44,53,22,15},{TabWeek,173,44,53,22,15},
    {FilterBattery,14,78,60,22,2},{FilterPlugged,74,78,60,22,2},{FilterAll,134,78,40,22,2},{Refresh,202,76,26,26,2},
    {ClearHour,206,int(U_LABEL_Y)-2,22,20,2},{PreviousPage,176,int(U_FOOT_Y)-4,24,22,6},{NextPage,204,int(U_FOOT_Y)-4,24,22,6}
};
void LayoutButtons(){
    for(auto& c:controls){HWND b=GetDlgItem(g.window,c.id);if(!b)continue;
        MoveWindow(b,Px(float(c.x)),Px(float(c.y)),Px(float(c.w)),Px(float(c.h)),TRUE);
        bool visible=((c.tabs>>g.tab)&1)!=0;
        if(c.id==ClearHour&&g.selected<0&&!g.appSelected)visible=false;
        if(c.id==PreviousPage||c.id==NextPage){if(ListPages()<=1)visible=false;EnableWindow(b,c.id==PreviousPage?g.page>0:g.page+1<ListPages());}
        if(c.id==Refresh){EnableWindow(b,!g.refreshing);SetWindowTextW(b,ButtonCaption(c.id).c_str());}
        if(c.id==DesktopPin)SetWindowTextW(b,ButtonCaption(c.id).c_str());
        ShowWindow(b,visible?SW_SHOWNOACTIVATE:SW_HIDE);
    }
}
void Redraw(){if(g.window&&!g.hidden&&g.displayOn&&!g.locked&&!g.suspended){InvalidateRect(g.window,nullptr,FALSE);for(HWND b:g.buttons)InvalidateRect(b,nullptr,FALSE);}}
bool LightTaskbar(){
    DWORD value=0,size=sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",L"SystemUsesLightTheme",RRF_RT_REG_DWORD,nullptr,&value,&size)==ERROR_SUCCESS&&value!=0;
}
// Tray glyph: the percentage as outlined digits over a thin level bar.
std::unique_ptr<Bitmap> MakeTrayBitmap(int size,int percent,uint8_t source,bool low,bool light){
    auto image=std::make_unique<Bitmap>(size,size,PixelFormat32bppARGB);Graphics gr(image.get());
    gr.SetSmoothingMode(SmoothingModeAntiAlias);gr.Clear(Color(0,0,0,0));
    float s=float(size),bar=std::max(2.f,std::round(s/7));
    Color ink=light?Color(255,24,28,29):Color(255,246,249,246);
    Color accent=low?RED:SourceColor(source);
    if(light)accent=Color(255,BYTE(accent.GetR()*0.62),BYTE(accent.GetG()*0.62),BYTE(accent.GetB()*0.62));
    Rounded(gr,RectF(0.5f,s-bar,s-1,bar),bar/2,Tint(ink,70));
    if(percent>0)Rounded(gr,RectF(0.5f,s-bar,std::max(bar,(s-1)*std::min(percent,100)/100.f),bar),bar/2,accent);
    wstring text=percent<0?L"?":std::to_wstring(std::min(percent,100));
    FontFamily family(L"Segoe UI");GraphicsPath path;StringFormat fmt(StringFormat::GenericTypographic());
    float em=text.size()>=3?s*0.6f:s*0.86f;
    path.AddString(text.c_str(),int(text.size()),&family,FontStyleBold,em,PointF(0,0),&fmt);
    RectF bounds;path.GetBounds(&bounds);
    float room=s-bar-1;
    if(bounds.Width>0&&bounds.Height>0){
        float fit=std::min(1.f,std::min((s-0.5f)/bounds.Width,room/bounds.Height));
        Matrix m;m.Translate((s-bounds.Width*fit)/2,(room-bounds.Height*fit)/2);m.Scale(fit,fit);m.Translate(-bounds.X,-bounds.Y);
        path.Transform(&m);
    }
    SolidBrush brush(low?accent:ink);gr.FillPath(&brush,&path);
    return image;
}
void UpdateTrayIcon(){
    if(!g.tray.hWnd)return;
    // With the taskbar pill showing the percentage, the tray keeps the plain Cell icon.
    if(g.pill){if(g.trayKey!=1){g.tray.hIcon=g.icon;g.tray.uFlags=NIF_ICON;if(Shell_NotifyIconW(NIM_MODIFY,&g.tray))g.trayKey=1;}return;}
    int size=std::clamp(GetSystemMetricsForDpi(SM_CXSMICON,GetDpiForWindow(g.window)),16,64);
    uint32_t key=uint32_t(g.power.percent+1)|(uint32_t(g.power.source)<<8)|(uint32_t(IsLow())<<12)|(uint32_t(g.lightTaskbar)<<13)|(uint32_t(size)<<16);
    if(key==g.trayKey&&g.trayIcon)return;
    HICON icon=nullptr;MakeTrayBitmap(size,g.power.percent,g.power.source,IsLow(),g.lightTaskbar)->GetHICON(&icon);
    if(!icon)return;
    g.tray.hIcon=icon;g.tray.uFlags=NIF_ICON;
    if(Shell_NotifyIconW(NIM_MODIFY,&g.tray)){if(g.trayIcon)DestroyIcon(g.trayIcon);g.trayIcon=icon;g.trayKey=key;}
    else{DestroyIcon(icon);g.tray.hIcon=g.trayIcon?g.trayIcon:g.icon;}
}
void UpdateTray(){
    if(!g.tray.hWnd)return;
    UpdateTrayIcon();
    wstring s=L"Cell · "+(g.power.percent>=0?std::to_wstring(g.power.percent)+L"% · ":L"")+StatusText();
    if(g.power.source==Battery||(g.power.source==Charging&&g.power.fullSeconds!=UINT32_MAX))s+=L" · "+MainEta();
    if(s==g.tray.szTip)return;wcsncpy(g.tray.szTip,s.c_str(),127);g.tray.szTip[127]=0;g.tray.uFlags=NIF_TIP|NIF_SHOWTIP;Shell_NotifyIconW(NIM_MODIFY,&g.tray);
    wstring title=L"Cell · "+StatusText()+L" · "+MainEta();SetWindowTextW(g.window,title.c_str());
}
void UpdatePill();
void CheckAlerts(const Power& previous,uint64_t now);
// The ring sweeps to a new level over a few frames, only while the card is visible.
void StartRingAnimation(){
    float target=float(std::max(0,g.power.percent));
    if(!g.window||g.hidden||!IsWindowVisible(g.window)||g.tab!=0||std::abs(target-g.ringShown)<0.5f){g.ringShown=target;return;}
    if(!g.animating){g.animating=SetTimer(g.window,ANIM_TIMER,16,nullptr)!=0;if(!g.animating)g.ringShown=target;}
}
void SamplePower(bool force=false){
    ++g.powerReads;
    Power previous=g.power;uint64_t lastHistory=g.history.empty()?0:g.history.back().utc,previousSample=g.lastSample;
    g.power=ReadPower();auto now=Now();RecordPower(now,force);RecordDrain(now);g.lastSample=now;
    if(previous.source!=g.power.source)g.wattsCount=0; // Draw samples describe one power state only.
    size_t previousWatts=g.wattsCount;
    if(!g.hidden&&g.power.watts>0&&(g.power.source==Battery||g.power.source==Charging)){g.watts[g.wattsCount%g.watts.size()]=float(g.power.watts);++g.wattsCount;}
    if(LiveActive()){
        if(g.liveSnaps.empty()||GetTickCount64()-g.liveSnaps.back().tick>=LIVE_PERIOD_MS){
            SampleLive();RECT panel{0,Px(70),Px(CW),Px(float(CH))};InvalidateRect(g.window,&panel,FALSE);
        }
    }else if(!g.liveSnaps.empty())StopLive();
    bool hourChanged=g.viewHour!=now/3600*3600,cacheChanged=CheckScheduledCache(now,force);
    if(hourChanged)g.selected=-1;
    if(hourChanged||cacheChanged){BuildView(now);LayoutButtons();}
    auto& p=g.power;
    bool factsChanged=false;
    if(force||previous.source!=p.source||!g.facts.readAt||now<g.facts.readAt||now-g.facts.readAt>=3600){
        BatteryFacts facts=ReadBatteryFacts();facts.readAt=now;
        factsChanged=facts.valid!=g.facts.valid||facts.designMWh!=g.facts.designMWh||facts.fullMWh!=g.facts.fullMWh||facts.cycles!=g.facts.cycles;g.facts=facts;
    }
    bool structural=force||hourChanged||cacheChanged||factsChanged||previous.percent!=p.percent||previous.source!=p.source||previous.saver!=p.saver||previous.valid!=p.valid||previous.present!=p.present||previous.windowsEta!=p.windowsEta||previous.recentDrain!=p.recentDrain||previous.drainCapable!=p.drainCapable||(previous.fullSeconds==UINT32_MAX)!=(p.fullSeconds==UINT32_MAX);
    UpdateTray();UpdatePill();CheckAlerts(previous,now);
    if(structural){Redraw();StartRingAnimation();return;}
    if(g.hidden||!g.displayOn||g.locked||g.suspended)return;
    auto invalidate=[](float x,float y,float width,float height){RECT rect{Px(x),Px(y),Px(x+width),Px(y+height)};InvalidateRect(g.window,&rect,FALSE);};
    bool minute=now/60!=previousSample/60;
    if(g.tab==0){
        if(previousWatts!=g.wattsCount||std::lround(previous.watts*10)!=std::lround(p.watts*10))invalidate(134,80,98,118);
        if(minute||EtaDisplayMinute(previous.seconds)!=EtaDisplayMinute(p.seconds)||EtaDisplayMinute(previous.fullSeconds)!=EtaDisplayMinute(p.fullSeconds)){invalidate(10,198,220,42);invalidate(12,80,124,124);}
        if(minute||previous.remainingMWh/100!=p.remainingMWh/100||previous.fullMWh/100!=p.fullMWh/100)invalidate(10,CHIP_Y-2,220,CHIP_H+4);
        if(!g.history.empty()&&lastHistory!=g.history.back().utc){invalidate(CHART_X-4,CHART_Y-6,CHART_W+8,CHART_H+10);if(g.hovering)invalidate(0,LEGEND_Y-4,CW,22);}
    }else if(g.tab==1&&minute)invalidate(10,U_FRESH_Y-2,220,18);
    else if(g.tab==2&&previousWatts!=g.wattsCount)invalidate(10,N_LABEL_Y-2,220,N_CHART_H+40);
    if(minute)invalidate(60,8,112,24); // Header quick status on the other tabs.
}
UINT DesiredRefreshInterval(){return !g.displayOn||g.locked||g.suspended?0:!g.hidden?LIVE_INTERVAL_MS:g.pill?PILL_INTERVAL_MS:HIDDEN_INTERVAL_MS;}
void SetTimerState(){
    UINT interval=DesiredRefreshInterval();
    if(interval!=LIVE_INTERVAL_MS)meter.trend.Reset();
    if(interval&&(!g.timer||g.timerInterval!=interval)){
        g.timer=SetCoalescableTimer(g.window,SAMPLE_TIMER,interval,nullptr,g.hidden?5000:250)!=0;
        g.timerInterval=g.timer?interval:0;
    }else if(!interval&&g.timer){KillTimer(g.window,SAMPLE_TIMER);g.timer=false;g.timerInterval=0;}
}
int WidgetHeight(){return CH;}
void ResizeWidget(){
    RECT r{};GetWindowRect(g.window,&r);MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromWindow(g.window,MONITOR_DEFAULTTONEAREST),&mi);
    int w=Px(CW),h=Px(float(WidgetHeight()));
    int x=std::clamp(r.left,mi.rcWork.left,std::max(mi.rcWork.left,mi.rcWork.right-w));
    int y=std::clamp(r.top,mi.rcWork.top,std::max(mi.rcWork.top,mi.rcWork.bottom-h));
    POINT position{x,y};
    if(g.desktop)MapWindowPoints(nullptr,g.desktopHost,&position,1);
    SetWindowPos(g.window,g.desktop?HWND_TOP:g.top?HWND_TOPMOST:HWND_NOTOPMOST,position.x,position.y,w,h,SWP_NOACTIVATE|SWP_FRAMECHANGED);
    SetWindowRgn(g.window,g.desktop?CreateRoundRectRgn(0,0,w+1,h+1,Px(18),Px(18)):nullptr,TRUE);
    g.buffer.reset();LayoutButtons();Redraw();
}
HWND FindDesktopHost(){
    HWND shell=FindWindowW(L"Progman",nullptr);
    HWND view=FindWindowExW(shell,nullptr,L"SHELLDLL_DefView",nullptr);
    if(view&&IsWindowVisible(view))return view;
    view=nullptr;
    EnumWindows([](HWND window,LPARAM raw)->BOOL{
        wchar_t name[40]{};GetClassNameW(window,name,40);
        if(wcscmp(name,L"WorkerW")&&wcscmp(name,L"Progman"))return TRUE;
        HWND child=FindWindowExW(window,nullptr,L"SHELLDLL_DefView",nullptr);
        if(child&&IsWindowVisible(child)){*reinterpret_cast<HWND*>(raw)=child;return FALSE;}return TRUE;
    },reinterpret_cast<LPARAM>(&view));
    return view;
}
bool SetDesktopParent(HWND host){
    if(!g.window||!IsWindow(g.window)||(host&&!IsWindow(host)))return false;
    RECT screen{};GetWindowRect(g.window,&screen);LONG_PTR oldStyle=GetWindowLongPtrW(g.window,GWL_STYLE);
    if(host){
        SetWindowPos(g.window,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        SetWindowLongPtrW(g.window,GWL_STYLE,(oldStyle&~(WS_POPUP|WS_THICKFRAME|WS_SYSMENU|WS_MINIMIZEBOX))|WS_CHILD|WS_CLIPSIBLINGS);
    }
    SetLastError(0);HWND previous=SetParent(g.window,host);DWORD error=GetLastError();
    if(!previous&&error){SetWindowLongPtrW(g.window,GWL_STYLE,oldStyle);ResizeWidget();return false;}
    if(!host)SetWindowLongPtrW(g.window,GWL_STYLE,(oldStyle&~WS_CHILD)|WS_POPUP|WS_THICKFRAME|WS_SYSMENU|WS_MINIMIZEBOX);
    g.desktop=host!=nullptr;g.desktopHost=host;
    g.scale=GetDpiForWindow(g.window)/96.f*WidgetScale();
    POINT position{screen.left,screen.top};if(host)MapWindowPoints(nullptr,host,&position,1);
    SetWindowPos(g.window,nullptr,position.x,position.y,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
    ResizeWidget();return true;
}
bool ApplyDesktopPin(){
    if(!g.desktopWanted){if(g.desktop)return SetDesktopParent(nullptr);return true;}
    if(g.desktop&&IsWindow(g.desktopHost)&&GetParent(g.window)==g.desktopHost)return true;
    HWND host=FindDesktopHost();return host&&SetDesktopParent(host);
}
void RetryDesktopPin(){
    if(!g.controller)return;
    KillTimer(g.controller,DESKTOP_RETRY_TIMER);g.desktopRetries=0;
    if(g.desktopWanted&&!ApplyDesktopPin())SetCoalescableTimer(g.controller,DESKTOP_RETRY_TIMER,10000,nullptr,1000);
}
HWND FindRunningWidget(){
    HWND controller=FindWindowW(CONTROL_CLASS,nullptr);
    return controller?controller:FindWindowW(WINDOW_CLASS,nullptr);
}
bool StartupEnabled(){
    HKEY key;wchar_t value[32768];DWORD bytes=sizeof(value),type=0;
    if(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,KEY_QUERY_VALUE,&key)!=ERROR_SUCCESS)return false;
    LONG rc=RegQueryValueExW(key,L"CellBattery",nullptr,&type,reinterpret_cast<BYTE*>(value),&bytes);RegCloseKey(key);
    value[32767]=0;return rc==ERROR_SUCCESS&&type==REG_SZ&&wstring(value)==L"\""+ExePath()+L"\"";
}
void ToggleStartup(){
    bool enabled=StartupEnabled();HKEY key;
    if(RegCreateKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr)!=ERROR_SUCCESS){MessageBoxW(g.window,L"Windows could not update the startup preference.",L"Cell",MB_OK);return;}
    LONG rc;if(enabled)rc=RegDeleteValueW(key,L"CellBattery");else{wstring value=L"\""+ExePath()+L"\"";rc=RegSetValueExW(key,L"CellBattery",0,REG_SZ,reinterpret_cast<const BYTE*>(value.c_str()),DWORD((value.size()+1)*2));}
    RegCloseKey(key);if(rc!=ERROR_SUCCESS)MessageBoxW(g.window,L"Windows could not save the startup preference.",L"Cell",MB_OK);
}
void ShowWidget(){
    g.hidden=false;ShowWindow(g.window,SW_SHOWNOACTIVATE);ResizeWidget();if(!g.desktop)SetForegroundWindow(g.window);
    if(g.timer){KillTimer(g.window,SAMPLE_TIMER);g.timer=false;}SamplePower();SetTimerState();
}
struct RefreshWait { HANDLE process; HWND window;bool scheduled=false; };
DWORD WINAPI AwaitRefresh(void* raw){
    std::unique_ptr<RefreshWait> wait(static_cast<RefreshWait*>(raw));WaitForSingleObject(wait->process,INFINITE);DWORD code=1;GetExitCodeProcess(wait->process,&code);CloseHandle(wait->process);
    PostMessageW(wait->window,WM_IMPORTED,code,wait->scheduled?1:0);return 0;
}
void StartRefresh(){
    if(g.refreshing)return;
    if(CheckScheduledCache(Now(),true)){BuildView(Now());LayoutButtons();Redraw();}
    if(g.hourlyAvailable&&!g.scheduledTask.empty()){
        wchar_t system[32768];UINT length=GetSystemDirectoryW(system,32768);
        if(!length||length>=32768){g.notice=L"Windows Task Scheduler could not be located.";Redraw();return;}
        wstring exe=wstring(system)+L"\\schtasks.exe",command=L"\""+exe+L"\" /Run /TN \"\\"+g.scheduledTask+L"\"";
        STARTUPINFOW si{};si.cb=sizeof(si);si.dwFlags=STARTF_USESHOWWINDOW;si.wShowWindow=SW_HIDE;PROCESS_INFORMATION process{};
        if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,system,&si,&process)){g.notice=L"Windows could not start the hourly refresh job.";Redraw();return;}
        CloseHandle(process.hThread);auto* wait=new RefreshWait{process.hProcess,g.controller?g.controller:g.window,true};HANDLE thread=CreateThread(nullptr,0,AwaitRefresh,wait,0,nullptr);
        if(!thread){CloseHandle(process.hProcess);delete wait;}else CloseHandle(thread);
        g.scheduledPending=true;g.scheduledRequest=GetTickCount64();g.refreshing=true;g.notice.clear();LayoutButtons();Redraw();return;
    }
    if(g.readOnly){MessageBoxW(g.window,L"Move Cell to a writable folder before loading app history.",L"Cell",MB_OK);return;}
    wstring exe=ExePath();SHELLEXECUTEINFOW se{};se.cbSize=sizeof(se);se.fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_FLAG_NO_UI;
    se.hwnd=g.window;se.lpVerb=L"runas";se.lpFile=exe.c_str();se.lpParameters=L"--refresh";se.lpDirectory=root.c_str();se.nShow=SW_HIDE;
    if(!ShellExecuteExW(&se)){g.notice=GetLastError()==ERROR_CANCELLED?L"Refresh cancelled; existing cache kept.":L"Windows could not start the history export.";Redraw();return;}
    auto* wait=new RefreshWait{se.hProcess,g.controller?g.controller:g.window};HANDLE thread=CreateThread(nullptr,0,AwaitRefresh,wait,0,nullptr);
    if(!thread){CloseHandle(se.hProcess);delete wait;g.notice=L"Export started; reopen Cell to load its cache.";}else{CloseHandle(thread);g.refreshing=true;g.notice.clear();}
    LayoutButtons();Redraw();
}
void Act(int id);
void EnsurePill();
void HidePeek();
void Popup(POINT p){
    HMENU menu=CreatePopupMenu();
    AppendMenuW(menu,MF_STRING,Show,L"Show widget");
    AppendMenuW(menu,MF_STRING|(g.desktopWanted?MF_CHECKED:0),DesktopPin,L"Pin to desktop");
    AppendMenuW(menu,MF_STRING|(g.desktopWanted?MF_GRAYED:0)|(!g.desktopWanted&&g.top?MF_CHECKED:0),Topmost,L"Keep above other windows");
    AppendMenuW(menu,MF_STRING|(g.prefs.taskbar?MF_CHECKED:0),TaskbarPill,L"Show in taskbar");
    AppendMenuW(menu,MF_STRING|(StartupEnabled()?MF_CHECKED:0),Startup,L"Start with Windows");
    HMENU alerts=CreatePopupMenu();
    AppendMenuW(alerts,MF_STRING|(g.prefs.alerts&1?MF_CHECKED:0),AlertUnplug,L"Remind me to unplug at 80%");
    AppendMenuW(alerts,MF_STRING|(g.prefs.alerts&2?MF_CHECKED:0),AlertLow,L"Low battery at 20%");
    AppendMenuW(alerts,MF_STRING|(g.prefs.alerts&4?MF_CHECKED:0),AlertDrain,L"Unusually high drain");
    AppendMenuW(menu,MF_POPUP,UINT_PTR(alerts),L"Alerts");
    HMENU sizes=CreatePopupMenu();
    AppendMenuW(sizes,MF_STRING|(g.prefs.size==1?MF_CHECKED:0),SizeSmall,L"Small");
    AppendMenuW(sizes,MF_STRING|(g.prefs.size==0?MF_CHECKED:0),SizeMedium,L"Medium");
    AppendMenuW(sizes,MF_STRING|(g.prefs.size==2?MF_CHECKED:0),SizeLarge,L"Large");
    AppendMenuW(menu,MF_POPUP,UINT_PTR(sizes),L"Size");AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
    if(g.hourlyAvailable)AppendMenuW(menu,MF_STRING|MF_GRAYED,0,L"App history refreshes hourly");
    AppendMenuW(menu,MF_STRING|(g.refreshing?MF_GRAYED:0),Refresh,L"Refresh Windows app history…");
    AppendMenuW(menu,MF_STRING,BatteryReadings,L"Battery reading details…");
    AppendMenuW(menu,MF_STRING,Settings,L"Open Windows battery settings");AppendMenuW(menu,MF_STRING,DataFolder,L"Open Cell's data folder");
    if(!g.notice.empty())AppendMenuW(menu,MF_STRING,999,L"Show refresh details…");
    AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING,Hide,L"Hide to tray");AppendMenuW(menu,MF_STRING,Quit,L"Exit Cell");
    HWND owner=g.desktop?g.controller:g.window;SetForegroundWindow(owner);int choice=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,p.x,p.y,0,owner,nullptr);DestroyMenu(menu);PostMessageW(owner,WM_NULL,0,0);
    if(choice==999)MessageBoxW(g.window,g.notice.c_str(),L"Cell · app history",MB_OK|MB_ICONINFORMATION);else if(choice)Act(choice);
}
void Act(int id){
    switch(id){
    case Expand:Act(TabBattery+(g.tab+1)%4);break;
    case TabBattery:case TabApps:case TabNow:case TabWeek:
        if(g.tab==id-TabBattery)break;
        g.tab=id-TabBattery;g.page=0;g.hoverRow=g.hoverHour=-1;g.hovering=false;
        if(g.tab==2)SampleLive();else StopLive();
        if(g.tab==1)BuildView(Now());
        if(g.tab==0){g.ringShown=0;StartRingAnimation();}
        LayoutButtons();Redraw();SavePrefs();break;
    case Menu:{RECT r;GetWindowRect(GetDlgItem(g.window,Menu),&r);Popup(POINT{r.right,r.bottom});break;}
    case Refresh:StartRefresh();break;
    case FilterLive:Act(TabNow);break;
    case FilterBattery:case FilterPlugged:case FilterAll:
        g.filter=id==FilterBattery?0:id==FilterPlugged?1:2;g.page=0;g.hoverRow=g.hoverHour=-1;
        BuildView(Now());LayoutButtons();Redraw();break;
    case TaskbarPill:g.prefs.taskbar=!g.prefs.taskbar;EnsurePill();SetTimerState();SavePrefs();break;
    case SizeSmall:case SizeMedium:case SizeLarge:
        g.prefs.size=uint8_t(id==SizeSmall?1:id==SizeLarge?2:0);g.scale=GetDpiForWindow(g.window)/96.f*WidgetScale();
        if(g.peek)HidePeek();ResizeWidget();SavePrefs();break;
    case AlertUnplug:case AlertLow:case AlertDrain:g.prefs.alerts^=uint8_t(1<<(id-AlertUnplug));SavePrefs();break;
    case ClearHour:g.selected=-1;g.appSelected=false;g.page=0;BuildView(Now());LayoutButtons();Redraw();break;
    case PreviousPage:if(g.page>0)--g.page;LayoutButtons();Redraw();break;
    case NextPage:if(g.page+1<ListPages())++g.page;LayoutButtons();Redraw();break;
    case Show:ShowWidget();break;
    case Topmost:if(!g.desktopWanted){g.top=!g.top;ResizeWidget();SavePrefs();}break;
    case DesktopPin:{
        g.desktopWanted=!g.desktopWanted;
        if(!ApplyDesktopPin()){
            g.desktopWanted=g.desktop;
            MessageBoxW(g.controller,L"Windows could not attach Cell to the desktop right now. The floating widget is still available; try the pin again after the desktop has loaded.",L"Cell · desktop pin",MB_OK|MB_ICONINFORMATION);
        }
        if(g.controller)KillTimer(g.controller,DESKTOP_RETRY_TIMER);LayoutButtons();Redraw();SavePrefs();break;
    }
    case Startup:ToggleStartup();break;
    case Settings:ShellExecuteW(g.window,L"open",L"ms-settings:batterysaver",nullptr,nullptr,SW_SHOWNORMAL);break;
    case DataFolder:ShellExecuteW(g.window,L"open",data.c_str(),nullptr,nullptr,SW_SHOWNORMAL);break;
    case BatteryReadings:{
        wchar_t b[160];wstring text;
        if(g.power.reportedWatts>0){swprintf(b,160,L"Windows-reported battery rate: %.2f W\n",g.power.reportedWatts);text=b;}
        else text=L"Windows-reported battery rate: unavailable\n";
        if(g.power.recentDrain){swprintf(b,160,L"Recent capacity-loss average: %.2f W over %u seconds\n",g.power.watts,g.power.drainSpan);text+=b;}
        else text+=L"Recent drain: collecting observations; allow 1–2 minutes with the widget visible.\n";
        if(g.power.capacityMWh!=UINT32_MAX){swprintf(b,160,L"Battery-reported remaining energy: %.2f Wh\n",g.power.capacityMWh/1000.0);text+=b;}
        else if(g.power.remainingMWh!=UINT32_MAX){swprintf(b,160,L"Remaining energy: %.2f Wh\n",g.power.remainingMWh/1000.0);text+=b;}
        if(g.power.source==Charging&&g.power.fullSeconds!=UINT32_MAX)text+=L"Time to full at the current rate: "+EtaText(g.power.fullSeconds)+L"\n";
        g.facts=ReadBatteryFacts();g.facts.readAt=Now();
        if(g.facts.valid){
            swprintf(b,160,L"\nDesign capacity: %.2f Wh\nFull-charge capacity: %.2f Wh\n",g.facts.designMWh/1000.0,g.facts.fullMWh/1000.0);text+=b;
            swprintf(b,160,L"Health (full-charge ÷ design): %d%%\n",g.facts.HealthPercent());text+=b;
            text+=g.facts.cycles?L"Charge cycles: "+std::to_wstring(g.facts.cycles)+L"\n":L"Charge cycles: not reported by this battery\n";
        }else text+=L"\nDesign capacity and cycle count are not reported by this battery.\n";
        text+=L"\nReadings refresh about every 2 seconds. The battery gauge updates in steps, so recent drain is an average and can take 1–2 minutes to reflect a workload change. Windows' reported rate can lag or disagree with that trend.\n\nTime left prefers Windows' ETA when available, otherwise recent drain, then the reported rate while collecting observations. All are estimates. App history refreshes separately, on demand.";
        MessageBoxW(g.window,text.c_str(),L"Cell · battery readings",MB_OK|MB_ICONINFORMATION);break;
    }
    case Hide:g.hidden=true;ShowWindow(g.window,SW_HIDE);if(g.timer){KillTimer(g.window,SAMPLE_TIMER);g.timer=false;}SetTimerState();SaveHistory();break;
    case Quit:g.quitting=true;DestroyWindow(g.window);break;
    }
}

// ---- Alerts (opt-in; delivered as quiet Windows notifications from the tray icon) ----
void Notify(const wstring& title,const wstring& text){
    ++g.alertsSent;if(!g.tray.hWnd)return;
    NOTIFYICONDATAW n=g.tray;n.uFlags=NIF_INFO;n.dwInfoFlags=NIIF_INFO|NIIF_RESPECT_QUIET_TIME;
    wcsncpy(n.szInfoTitle,title.c_str(),63);n.szInfoTitle[63]=0;wcsncpy(n.szInfo,text.c_str(),255);n.szInfo[255]=0;
    Shell_NotifyIconW(NIM_MODIFY,&n);
}
// Alerts fire on crossings, never just because Cell started at that level.
void CheckAlerts(const Power& previous,uint64_t now){
    const Power& p=g.power;if(p.percent<0||previous.percent<0)return;
    bool onPower=p.source==Charging||p.source==Plugged,wasOnPower=previous.source==Charging||previous.source==Plugged;
    if((g.prefs.alerts&1)&&onPower&&wasOnPower&&previous.percent<80&&p.percent>=80)
        Notify(L"Battery at "+std::to_wstring(p.percent)+L"%",L"Unplug now to reduce battery wear.");
    if((g.prefs.alerts&2)&&p.source==Battery&&previous.source==Battery&&previous.percent>LOW_PERCENT&&p.percent<=LOW_PERCENT)
        Notify(L"Battery at "+std::to_wstring(p.percent)+L"%",(p.seconds!=UINT32_MAX?L"About "+EtaText(p.seconds)+L" left. ":wstring())+L"Plug in soon.");
    double typical=TypicalWatts(now),recent=AverageWatts(30);
    if((g.prefs.alerts&4)&&p.source==Battery&&typical>0&&WattCount()>=30&&recent>std::max(2*typical,typical+5)&&now-g.lastDrainAlert>=1800){
        g.lastDrainAlert=now;wchar_t b[160];swprintf(b,160,L"Drawing %.0f W, about %.0f× your usual %.1f W. Open Cell → Now to see which apps.",recent,recent/typical,typical);
        Notify(L"Unusually high drain",b);
    }
}

// ---- Taskbar pill: a small child of the taskbar left of the tray, with a hover card ----
const wchar_t* PILL_CLASS=L"CellBatteryPill1";
const wchar_t* PEEK_CLASS=L"CellBatteryPeek1";
HWND Taskbar(){return FindWindowW(L"Shell_TrayWnd",nullptr);}
// Pill rectangle in taskbar client coordinates. Horizontal taskbars only.
bool PillLayout(RECT& rect,bool& twoLines){
    HWND bar=Taskbar();if(!bar)return false;
    RECT client{};GetClientRect(bar,&client);int bw=client.right,bh=client.bottom;if(bh<=0||bh>bw)return false;
    float s=GetDpiForWindow(bar)/96.f;
    twoLines=bh>=int(40*s);int h=std::min(bh-int(4*s),int(40*s)),w=int((twoLines?132:124)*s);
    int right=bw-int(260*s);
    HWND notify=FindWindowExW(bar,nullptr,L"TrayNotifyWnd",nullptr);
    if(notify&&IsWindowVisible(notify)){RECT n{};GetWindowRect(notify,&n);MapWindowPoints(nullptr,bar,reinterpret_cast<POINT*>(&n),2);if(n.left>0)right=n.left-int(6*s);}
    rect={right-w,(bh-h)/2,right,(bh-h)/2+h};return rect.left>int(200*s);
}
void PaintPill(Graphics& gr,float w,float h,bool twoLines,bool light,bool hover){
    Color ink=light?Color(255,24,28,29):Color(255,244,247,244),sub=light?Color(255,78,86,83):Color(255,190,200,195);
    Color accent=IsLow()?RED:SourceColor(g.power.source);
    if(light&&!g.lightApps)accent=Color(255,BYTE(accent.GetR()*0.6),BYTE(accent.GetG()*0.6),BYTE(accent.GetB()*0.6));
    // Near-transparent fill keeps the whole pill clickable; hover brightens it like taskbar buttons.
    Rounded(gr,RectF(0.5f,0.5f,w-1,h-1),6,Tint(ink,hover?30:3));
    const float bx=9,by=h/2-5.5f,bw=21,bh=11;
    Rounded(gr,RectF(bx-0.5f,by-0.5f,bw+1,bh+1),3,Tint(ink,190));
    Rounded(gr,RectF(bx+0.8f,by+0.8f,bw-1.6f,bh-1.6f),2,light?Color(255,236,236,236):Color(255,32,32,32));
    Rounded(gr,RectF(bx+bw+0.5f,by+3.5f,2,4),1,Tint(ink,190));
    if(g.power.percent>0)Rounded(gr,RectF(bx+2,by+2,std::max(2.f,(bw-4)*std::min(g.power.percent,100)/100.f),bh-4),1.5f,accent);
    if(g.power.source==Charging)Bolt(gr,bx+bw/2,by+bh/2,0.75f,ink);
    wstring pct=g.power.percent>=0?std::to_wstring(g.power.percent)+L"%":L"—",detail;
    if(g.power.source==Battery)detail=g.power.seconds!=UINT32_MAX&&g.power.seconds>0?EtaText(g.power.seconds):L"on battery";
    else if(g.power.source==Charging)detail=g.power.fullSeconds!=UINT32_MAX?L"full "+EtaText(g.power.fullSeconds):L"charging";
    else detail=g.power.source==Plugged?(g.power.percent>=100?L"full":L"plugged in"):L"";
    const float tx=bx+bw+9;
    if(twoLines){
        wchar_t b[24];if(g.power.watts>0){swprintf(b,24,L" · %.1f W",g.power.watts);detail+=b;}
        TextAt(gr,pct,tx,h/2-16,w-tx-4,7,ink);TextAt(gr,detail,tx,h/2+1,w-tx-4,0,sub);
    }else{
        RectF bounds;gr.MeasureString(pct.c_str(),int(pct.size()),g.fonts[7].get(),PointF(0,0),StringFormat::GenericTypographic(),&bounds);
        TextAt(gr,pct,tx,h/2-9,w-tx-4,7,ink);
        if(!detail.empty())TextAt(gr,L"· "+detail,tx+bounds.Width+5,h/2-8,w-tx-bounds.Width-8,1,sub);
    }
}
void RenderPill(){
    if(!g.pill||!IsWindow(g.pill))return;
    RECT r{};GetClientRect(g.pill,&r);int w=r.right,h=r.bottom;if(w<=0||h<=0)return;
    BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(bi.bmiHeader);bi.bmiHeader.biWidth=w;bi.bmiHeader.biHeight=-h;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
    HDC screen=GetDC(nullptr),memory=CreateCompatibleDC(screen);void* bits=nullptr;
    HBITMAP dib=CreateDIBSection(memory,&bi,DIB_RGB_COLORS,&bits,nullptr,0);
    if(dib&&bits){
        HGDIOBJ old=SelectObject(memory,dib);
        {
            Bitmap canvas(w,h,w*4,PixelFormat32bppPARGB,static_cast<BYTE*>(bits));Graphics gr(&canvas);
            gr.Clear(Color(0,0,0,0));gr.SetSmoothingMode(SmoothingModeAntiAlias);gr.SetTextRenderingHint(TextRenderingHintAntiAlias);
            float s=GetDpiForWindow(g.pill)/96.f;gr.ScaleTransform(s,s);PaintPill(gr,w/s,h/s,g.pillTwoLines,g.lightTaskbar,g.pillHover);
        }
        POINT source{0,0};SIZE size{w,h};BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
        UpdateLayeredWindow(g.pill,screen,nullptr,&size,memory,&source,0,&blend,ULW_ALPHA);
        SelectObject(memory,old);DeleteObject(dib);
    }
    DeleteDC(memory);ReleaseDC(nullptr,screen);
}
// Repaint only when what the pill shows has changed.
void UpdatePill(){
    if(!g.pill)return;
    auto minute=[](uint32_t s){uint64_t m=EtaDisplayMinute(s);return m==UINT64_MAX?0u:unsigned(m);};
    wchar_t key[96];swprintf(key,96,L"%d|%d|%u|%u|%.1f|%d|%d|%d",g.power.percent,int(g.power.source),minute(g.power.seconds),minute(g.power.fullSeconds),
        g.pillTwoLines?g.power.watts:0.0,int(g.pillHover),int(g.lightTaskbar),int(IsLow()));
    if(g.pillKey==key)return;g.pillKey=key;RenderPill();
}
void HidePeek(){if(g.peek&&IsWindowVisible(g.peek))ShowWindow(g.peek,SW_HIDE);}
void ShowPeek(){
    if(!g.pill||(!g.hidden&&!g.desktop&&g.window&&IsWindowVisible(g.window)))return; // The full card is already in view.
    if(!g.peek){
        g.peek=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST|WS_EX_NOACTIVATE|WS_EX_TRANSPARENT,PEEK_CLASS,L"Cell",WS_POPUP,0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(!g.peek)return;
        DWORD corner=2;DwmSetWindowAttribute(g.peek,33,&corner,sizeof(corner));
    }
    BOOL dark=!g.lightApps;DwmSetWindowAttribute(g.peek,20,&dark,sizeof(dark));
    SamplePower();g.ringShown=float(std::max(0,g.power.percent));
    RECT pill{},bar{};GetWindowRect(g.pill,&pill);GetWindowRect(Taskbar(),&bar);
    MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromWindow(g.pill,MONITOR_DEFAULTTONEAREST),&mi);
    int w=Px(float(CW)),h=Px(PEEK_H),gap=Px(8);
    bool top=bar.top<=mi.rcMonitor.top&&bar.bottom<(mi.rcMonitor.top+mi.rcMonitor.bottom)/2;
    int x=std::clamp(int(pill.right-w),int(mi.rcMonitor.left)+gap,int(mi.rcMonitor.right)-w-gap),y=top?bar.bottom+gap:bar.top-h-gap;
    SetWindowPos(g.peek,HWND_TOPMOST,x,y,w,h,SWP_NOACTIVATE|SWP_SHOWWINDOW);InvalidateRect(g.peek,nullptr,FALSE);
}
LRESULT CALLBACK PeekProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
    if(msg==WM_NCHITTEST)return HTTRANSPARENT;
    if(msg==WM_ERASEBKGND)return 1;
    if(msg==WM_PAINT){
        PAINTSTRUCT ps;HDC hdc=BeginPaint(hwnd,&ps);RECT r;GetClientRect(hwnd,&r);
        if(r.right>0&&r.bottom>0){Bitmap buffer(r.right,r.bottom,PixelFormat32bppPARGB);{Graphics gr(&buffer);PaintPeek(gr,r.right,r.bottom,Now());}Graphics screen(hdc);screen.DrawImage(&buffer,0,0);}
        EndPaint(hwnd,&ps);return 0;
    }
    if(msg==WM_DESTROY){if(g.peek==hwnd)g.peek=nullptr;return 0;}
    return DefWindowProcW(hwnd,msg,wp,lp);
}
LRESULT CALLBACK PillProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
    switch(msg){
    case WM_MOUSEMOVE:
        if(!g.pillTracking){TRACKMOUSEEVENT t{sizeof(t),TME_LEAVE|TME_HOVER,hwnd,350};g.pillTracking=TrackMouseEvent(&t)!=0;}
        if(!g.pillHover){g.pillHover=true;UpdatePill();}return 0;
    case WM_MOUSEHOVER:ShowPeek();return 0;
    case WM_MOUSELEAVE:g.pillTracking=false;g.pillHover=false;UpdatePill();HidePeek();return 0;
    case WM_LBUTTONUP:HidePeek();if(g.hidden||!g.window||!IsWindowVisible(g.window))Act(Show);else Act(Hide);return 0;
    case WM_RBUTTONUP:{HidePeek();POINT p;GetCursorPos(&p);Popup(p);return 0;}
    case WM_DPICHANGED_AFTERPARENT:g.pillKey.clear();EnsurePill();return 0;
    case WM_DESTROY:if(g.pill==hwnd)g.pill=nullptr;g.pillHover=g.pillTracking=false;g.pillKey.clear();return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
void RegisterPillClasses(){
    static bool registered=false;if(registered)return;registered=true;
    WNDCLASSEXW wc{};wc.cbSize=sizeof(wc);wc.lpfnWndProc=PillProc;wc.hInstance=GetModuleHandleW(nullptr);wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.lpszClassName=PILL_CLASS;RegisterClassExW(&wc);
    WNDCLASSEXW pc{};pc.cbSize=sizeof(pc);pc.lpfnWndProc=PeekProc;pc.hInstance=wc.hInstance;pc.lpszClassName=PEEK_CLASS;RegisterClassExW(&pc);
}
// Creates, moves or removes the pill to match the preference and the current taskbar.
void EnsurePill(){
    RECT rect{};bool twoLines=false;
    bool want=g.prefs.taskbar&&!g.quitting&&!g.readOnly&&PillLayout(rect,twoLines);
    HWND bar=Taskbar();
    if(!want||(g.pill&&(!IsWindow(g.pill)||GetParent(g.pill)!=bar))){
        HidePeek();if(g.pill&&IsWindow(g.pill))DestroyWindow(g.pill);g.pill=nullptr;
        g.trayKey=UINT32_MAX;UpdateTrayIcon(); // The tray takes the percentage back.
        if(!want){SetTimerState();return;}
    }
    if(!g.pill){
        RegisterPillClasses();
        g.pill=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,PILL_CLASS,L"Cell",WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS,
            rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,bar,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(!g.pill)return;
        g.pillTwoLines=twoLines;g.pillKey.clear();SetWindowPos(g.pill,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);UpdatePill();UpdateTrayIcon();SetTimerState();return;
    }
    RECT now{};GetWindowRect(g.pill,&now);MapWindowPoints(nullptr,bar,reinterpret_cast<POINT*>(&now),2);
    if(!EqualRect(&now,&rect)||g.pillTwoLines!=twoLines){
        g.pillTwoLines=twoLines;SetWindowPos(g.pill,HWND_TOP,rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,SWP_NOACTIVATE);g.pillKey.clear();UpdatePill();
    }
}

bool PngEncoder(CLSID& encoder){
    UINT count=0,size=0;if(GetImageEncodersSize(&count,&size)!=Ok||!size)return false;
    std::vector<BYTE> bytes(size);auto* info=reinterpret_cast<ImageCodecInfo*>(bytes.data());
    if(GetImageEncoders(count,size,info)!=Ok)return false;
    for(UINT i=0;i<count;i++)if(wcscmp(info[i].MimeType,L"image/png")==0){encoder=info[i].Clsid;return true;}return false;
}
std::unique_ptr<Bitmap> MakeIconBitmap(){
    auto image=std::make_unique<Bitmap>(64,64,PixelFormat32bppARGB);Graphics gr(image.get());gr.SetSmoothingMode(SmoothingModeAntiAlias);gr.Clear(Color(0,0,0,0));
    Rounded(gr,RectF(2,2,60,60),18,BG);Rounded(gr,RectF(16,22,31,20),5,GREEN);Rounded(gr,RectF(48,28,4,8),1.5f,GREEN);
    Rounded(gr,RectF(20,26,22,12),2,TRACK);Rounded(gr,RectF(20,26,14,12),2,GREEN);return image;
}
bool SaveIcon(const wstring&path){
    auto image=MakeIconBitmap();CLSID cls;if(!PngEncoder(cls))return false;
    IStream* stream=nullptr;if(FAILED(CreateStreamOnHGlobal(nullptr,TRUE,&stream)))return false;
    bool ok=image->Save(stream,&cls,nullptr)==Ok;STATSTG st{};stream->Stat(&st,STATFLAG_NONAME);
    if(st.cbSize.QuadPart>1048576)ok=false;
    LARGE_INTEGER zero{};stream->Seek(zero,STREAM_SEEK_SET,nullptr);std::vector<BYTE> png(size_t(st.cbSize.QuadPart));ULONG read=0;
    if(ok)ok=SUCCEEDED(stream->Read(png.data(),ULONG(png.size()),&read))&&read==png.size();stream->Release();
    if(!ok)return false;
    #pragma pack(push,1)
    struct IconHeader { uint16_t reserved=0,type=1,count=1;uint8_t w=64,h=64,colors=0,pad=0;uint16_t planes=1,bits=32;uint32_t size=0,offset=22; };
    #pragma pack(pop)
    IconHeader header;header.size=uint32_t(png.size());return AtomicWrite(path,&header,sizeof(header),png.data(),DWORD(png.size()));
}
void AddTray(){
    g.tray={};g.tray.cbSize=sizeof(g.tray);g.tray.hWnd=g.window;g.tray.uID=1;g.trayKey=UINT32_MAX;g.lightTaskbar=LightTaskbar();
    g.tray.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP|NIF_SHOWTIP;g.tray.uCallbackMessage=WM_TRAY;g.tray.hIcon=g.icon;
    wcscpy(g.tray.szTip,L"Cell battery");Shell_NotifyIconW(NIM_ADD,&g.tray);g.tray.uVersion=NOTIFYICON_VERSION_4;Shell_NotifyIconW(NIM_SETVERSION,&g.tray);UpdateTray();
}
LRESULT CALLBACK ButtonSubclass(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR){
    if(msg==BM_SETSTYLE)return 0; // Keep BS_OWNERDRAW: see CreateControls.
    if(msg==WM_ERASEBKGND)return 1; // WM_DRAWITEM paints every pixel; skip the system grey.
    return DefSubclassProc(hwnd,msg,wp,lp);
}
void CreateControls(){
    for(auto& c:controls){
        HWND button=CreateWindowExW(0,L"BUTTON",ButtonCaption(c.id).c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,Px(c.x),Px(c.y),Px(c.w),Px(c.h),g.window,reinterpret_cast<HMENU>(INT_PTR(c.id)),GetModuleHandleW(nullptr),nullptr);
        SetWindowSubclass(button,ButtonSubclass,1,0);g.buttons.push_back(button);
    }
    g.tooltip=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,g.window,nullptr,GetModuleHandleW(nullptr),nullptr);
    SendMessageW(g.tooltip,TTM_SETMAXTIPWIDTH,0,Px(350));
    for(auto& c:controls){
        TOOLINFOW ti{};ti.cbSize=sizeof(ti);ti.uFlags=TTF_IDISHWND|TTF_SUBCLASS;ti.hwnd=g.window;ti.uId=UINT_PTR(GetDlgItem(g.window,c.id));
        ti.lpszText=LPSTR_TEXTCALLBACKW;SendMessageW(g.tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&ti));
    }
    LayoutButtons();
}
void RegisterNotifications(){
    const GUID guids[]={
        {0xa7ad8041,0xb45a,0x4cae,{0x87,0xa3,0xee,0xcb,0xb4,0x68,0xa9,0xe1}}, // battery percent
        {0x5d3e9a59,0xe9d5,0x4b00,{0xa6,0xbd,0xff,0x34,0xff,0x51,0x65,0x48}}, // AC/DC source
        {0x6fe69556,0x704a,0x47a0,{0x8f,0x24,0xc2,0x8d,0x93,0x6f,0xda,0x47}}, // console display state
        {0xe00958c0,0xc213,0x4ace,{0xac,0x77,0xfe,0xcc,0xed,0x2e,0xee,0xa5}}  // battery saver
    };
    for(auto& guid:guids){auto handle=RegisterPowerSettingNotification(g.window,&guid,DEVICE_NOTIFY_WINDOW_HANDLE);if(handle)g.notifications.push_back(handle);}
    WTSRegisterSessionNotification(g.window,NOTIFY_FOR_THIS_SESSION);
}
bool CreateWidget(bool visible=true);
LRESULT CALLBACK ControllerProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
    static UINT taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated");
    if(msg==taskbarCreated){if(IsWindow(g.window)){AddTray();RetryDesktopPin();g.pill=nullptr;EnsurePill();}return 0;}
    if(msg==WM_TIMER&&wp==PILL_TIMER){EnsurePill();return 0;}
    if(msg==WM_COMMAND){if(IsWindow(g.window))Act(LOWORD(wp));return 0;}
    if(msg==WM_TIMER&&wp==DESKTOP_RETRY_TIMER){
        if(!g.desktopWanted||ApplyDesktopPin()||++g.desktopRetries>=6)KillTimer(hwnd,DESKTOP_RETRY_TIMER);
        LayoutButtons();return 0;
    }
    // Refresh results arrive here because the widget window can be recreated mid-refresh.
    if(msg==WM_IMPORTED){if(IsWindow(g.window))SendMessageW(g.window,msg,wp,lp);return 0;}
    if(msg==WM_SETTINGCHANGE&&IsWindow(g.window))SendMessageW(g.window,msg,wp,lp);
    if(msg==WM_RECREATE&&!g.quitting){
        g.historyGap=true;
        if(!CreateWidget()){g.quitting=true;PostQuitMessage(1);}return 0;
    }
    // Desktop child windows do not receive ordinary top-level broadcasts.
    if(g.desktop&&(msg==WM_POWERBROADCAST||msg==WM_DISPLAYCHANGE||msg==WM_QUERYENDSESSION||msg==WM_ENDSESSION))
        return SendMessageW(g.window,msg,wp,lp);
    if(msg==WM_DESTROY){KillTimer(hwnd,DESKTOP_RETRY_TIMER);KillTimer(hwnd,PILL_TIMER);return 0;}
    return DefWindowProcW(hwnd,msg,wp,lp);
}
LRESULT CALLBACK WindowProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
    static UINT taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated");
    if(msg==taskbarCreated){if(!g.controller)AddTray();return 0;}
    switch(msg){
    case WM_NCCALCSIZE:if(wp)return 0;break;
    case WM_NCHITTEST:{
        POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ScreenToClient(hwnd,&p);
        if(!g.desktop&&p.y>=0&&p.y<Px(40)&&p.x<Px(170))return HTCAPTION;
        return HTCLIENT;
    }
    case WM_ERASEBKGND:return 1;
    case WM_PAINT:{
        PAINTSTRUCT ps;HDC hdc=BeginPaint(hwnd,&ps);RECT r;GetClientRect(hwnd,&r);
        int w=r.right,h=r.bottom;
        if(w>0&&h>0){
            if(!g.buffer||int(g.buffer->GetWidth())!=w||int(g.buffer->GetHeight())!=h)g.buffer=std::make_unique<Bitmap>(w,h,PixelFormat32bppPARGB);
            Rect changed(ps.rcPaint.left,ps.rcPaint.top,ps.rcPaint.right-ps.rcPaint.left,ps.rcPaint.bottom-ps.rcPaint.top);
            Graphics canvas(g.buffer.get());canvas.SetClip(changed);Paint(canvas,w,h,Now(),&ps.rcPaint);
            Graphics screen(hdc);screen.DrawImage(g.buffer.get(),changed,changed.X,changed.Y,changed.Width,changed.Height,UnitPixel);
        }
        EndPaint(hwnd,&ps);return 0;
    }
    case WM_DRAWITEM:{
        auto* d=reinterpret_cast<DRAWITEMSTRUCT*>(lp);if(d->CtlType!=ODT_BUTTON)break;
        Graphics gr(d->hDC);gr.ScaleTransform(g.scale,g.scale);
        DrawButton(gr,int(d->CtlID),float(d->rcItem.right-d->rcItem.left)/g.scale,float(d->rcItem.bottom-d->rcItem.top)/g.scale,(d->itemState&ODS_SELECTED)!=0,(d->itemState&ODS_FOCUS)&&!(d->itemState&ODS_NOFOCUSRECT));return TRUE;
    }
    case WM_NOTIFY:{
        auto* n=reinterpret_cast<NMHDR*>(lp);
        if(n->code==TTN_GETDISPINFOW){auto* t=reinterpret_cast<NMTTDISPINFOW*>(lp);static wstring tip;int id=GetDlgCtrlID(reinterpret_cast<HWND>(n->idFrom));
            if(id==Refresh)tip=(g.hourlyAvailable?L"App history refreshes hourly without prompts. Click to request the same scheduled job now. ":L"Refresh saved Windows app estimates. Only the temporary exporter asks for administrator consent. ")+wstring(L"No resident service is installed. Hardware EMI meters are excluded. Unassigned energy stays separate from app shares.")+(g.energyHeader.latest?L" Latest Windows record: "+Clock(g.energyHeader.latest)+L".":L"");
            else if(id==FilterBattery)tip=L"Share of Windows' reported app energy while on battery. It is not the percentage of battery capacity lost.";
            else if(id==FilterPlugged)tip=L"App energy used while plugged in, separate from energy stored in the battery by charging.";
            else if(id==ClearHour)tip=L"Clear the selected hour and app. Tip: click any bar to filter by that hour, or any app to see its hourly energy.";
            else tip=ButtonCaption(id);t->lpszText=const_cast<wchar_t*>(tip.c_str());return 0;
        }break;
    }
    case WM_COMMAND:if(HIWORD(wp)==BN_CLICKED)Act(LOWORD(wp));return 0;
    case WM_LBUTTONDOWN:
        if(g.desktop&&GET_Y_LPARAM(lp)<Px(40)&&GET_X_LPARAM(lp)<Px(170)){
            g.dragging=true;GetCursorPos(&g.dragStart);GetWindowRect(hwnd,&g.dragRect);SetCapture(hwnd);
        }return 0;
    case WM_MOUSEMOVE:{
        if(g.dragging){
            POINT cursor{};GetCursorPos(&cursor);POINT position{g.dragRect.left+cursor.x-g.dragStart.x,g.dragRect.top+cursor.y-g.dragStart.y};
            MapWindowPoints(nullptr,g.desktopHost,&position,1);
            SetWindowPos(hwnd,nullptr,position.x,position.y,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
            return 0;
        }
        // Battery tab: hovering the 24h chart reads out the nearest recorded sample in the legend row.
        float x=GET_X_LPARAM(lp)/g.scale,y=GET_Y_LPARAM(lp)/g.scale;
        bool over=g.tab==0&&x>=CHART_X-2&&x<=CHART_X+CHART_W+2&&y>=CHART_Y-6&&y<=CHART_Y+CHART_H+4;
        float hx=std::clamp(x,CHART_X,CHART_X+CHART_W);
        if(over!=g.hovering||(over&&std::abs(hx-g.hoverX)>=0.5f)){
            g.hovering=over;g.hoverX=hx;
            RECT rect{0,Px(CHART_Y-6),Px(float(CW)),Px(LEGEND_Y+18)};InvalidateRect(hwnd,&rect,FALSE);
        }
        // Apps tab: hovering an hour previews it; Apps and Now: hovering a row highlights it.
        int hour=-1,row=-1;
        if(g.tab==1&&y>=U_CHART_Y-4&&y<=U_CHART_Y+U_CHART_H+2)hour=UsageHourAt(x);
        if((g.tab==1||g.tab==2)&&x>=6&&x<234&&y>=U_LIST_Y-3&&y<U_LIST_Y+APP_ROWS*U_ROW_H-3){
            row=int((y-U_LIST_Y+3)/U_ROW_H);if(size_t(g.page*APP_ROWS+row)>=ListCount())row=-1;
        }
        if(hour!=g.hoverHour){g.hoverHour=hour;RECT rect{0,Px(U_LABEL_Y-2),Px(float(CW)),Px(U_CHART_Y+U_CHART_H+4)};InvalidateRect(hwnd,&rect,FALSE);}
        if(row!=g.hoverRow){g.hoverRow=row;RECT rect{0,Px(U_LIST_Y-4),Px(float(CW)),Px(U_LIST_Y+APP_ROWS*U_ROW_H)};InvalidateRect(hwnd,&rect,FALSE);}
        if((over||hour>=0||row>=0)&&!g.tracking){TRACKMOUSEEVENT t{sizeof(t),TME_LEAVE,hwnd,0};g.tracking=TrackMouseEvent(&t)!=0;}
        return 0;
    }
    case WM_SETCURSOR:
        if(LOWORD(lp)==HTCLIENT&&(g.hoverHour>=0||(g.hoverRow>=0&&g.tab==1))){SetCursor(LoadCursorW(nullptr,IDC_HAND));return TRUE;}
        break;
    case WM_MOUSELEAVE:
        g.tracking=false;
        if(g.hovering){g.hovering=false;RECT rect{0,Px(CHART_Y-6),Px(float(CW)),Px(LEGEND_Y+18)};InvalidateRect(hwnd,&rect,FALSE);}
        if(g.hoverHour>=0||g.hoverRow>=0){g.hoverHour=g.hoverRow=-1;RECT rect{0,Px(70),Px(float(CW)),Px(float(CH))};InvalidateRect(hwnd,&rect,FALSE);}
        return 0;
    case WM_SETTINGCHANGE:
        if(lp&&wcscmp(reinterpret_cast<const wchar_t*>(lp),L"ImmersiveColorSet")==0){
            bool light=LightTaskbar();if(light!=g.lightTaskbar){g.lightTaskbar=light;UpdateTray();g.pillKey.clear();UpdatePill();}
            bool apps=LightApps();if(apps!=g.lightApps){ApplyTheme(apps);BOOL dark=!apps;DwmSetWindowAttribute(hwnd,20,&dark,sizeof(dark));
                COLORREF border=RGB(LINE.GetR(),LINE.GetG(),LINE.GetB());DwmSetWindowAttribute(hwnd,34,&border,sizeof(border));Redraw();}
        }
        break;
    case WM_CAPTURECHANGED:g.dragging=false;return 0;
    case WM_LBUTTONUP:{
        if(g.dragging){g.dragging=false;ReleaseCapture();ResizeWidget();SavePrefs();return 0;}
        float x=GET_X_LPARAM(lp)/g.scale,y=GET_Y_LPARAM(lp)/g.scale;
        if(g.tab==1&&y>=U_CHART_Y-4&&y<=U_CHART_Y+U_CHART_H+2&&UsageHourAt(x)>=0){
            int hour=UsageHourAt(x);g.selected=g.selected==hour?-1:hour; // Clicking the selected hour clears it.
            g.page=0;BuildView(Now());LayoutButtons();Redraw();
        }else if(g.tab==1&&x>=6&&x<234&&y>=U_LIST_Y-3&&y<U_LIST_Y+APP_ROWS*U_ROW_H-3){
            // Clicking an app overlays its hourly energy on the chart; clicking it again clears it.
            size_t idx=size_t(g.page*APP_ROWS+int((y-U_LIST_Y+3)/U_ROW_H));
            if(idx<g.view.apps.size()){
                auto& a=g.view.apps[idx];
                if(g.appSelected&&g.appName==a.name)g.appSelected=false;else{g.appSelected=true;g.appName=a.name;}
                BuildView(Now());LayoutButtons();Redraw();
            }
        }return 0;
    }
    case WM_MOUSEWHEEL:if(g.tab==1||g.tab==2)Act(GET_WHEEL_DELTA_WPARAM(wp)<0?NextPage:PreviousPage);return 0;
    case WM_CONTEXTMENU:{POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};if(p.x==-1){RECT r;GetWindowRect(hwnd,&r);p={r.left+Px(230),r.top+Px(40)};}Popup(p);return 0;}
    case WM_KEYDOWN:
        if(wp==VK_ESCAPE)Act(Hide);else if(wp==VK_F5)Act(Refresh);
        else if(wp==VK_RIGHT)Act(TabBattery+(g.tab+1)%4);else if(wp==VK_LEFT)Act(TabBattery+(g.tab+3)%4);
        return 0;
    case WM_TIMER:
        if(wp==SAMPLE_TIMER)SamplePower();
        else if(wp==ANIM_TIMER){
            float target=float(std::max(0,g.power.percent));g.ringShown+=(target-g.ringShown)*0.2f;
            if(std::abs(target-g.ringShown)<0.4f||g.tab!=0||g.hidden){g.ringShown=target;KillTimer(hwnd,ANIM_TIMER);g.animating=false;}
            RECT ring{Px(12),Px(80),Px(136),Px(204)};InvalidateRect(hwnd,&ring,FALSE);
        }
        return 0;
    case WM_POWERBROADCAST:
        if(wp==PBT_APMSUSPEND){g.suspended=true;g.historyGap=true;SaveHistory();SetTimerState();}
        else if(wp==PBT_APMRESUMEAUTOMATIC||wp==PBT_APMRESUMESUSPEND){g.suspended=false;g.historyGap=true;SamplePower(true);SetTimerState();}
        else if(wp==PBT_APMPOWERSTATUSCHANGE)SamplePower();
        else if(wp==PBT_POWERSETTINGCHANGE){
            auto* p=reinterpret_cast<POWERBROADCAST_SETTING*>(lp);
            const GUID display={0x6fe69556,0x704a,0x47a0,{0x8f,0x24,0xc2,0x8d,0x93,0x6f,0xda,0x47}};
            if(IsEqualGUID(p->PowerSetting,display)&&p->DataLength==sizeof(DWORD)){
                DWORD state=0;memcpy(&state,p->Data,sizeof(state));g.displayOn=state!=0;
                if(g.displayOn){g.historyGap=true;SamplePower(true);}else{g.historyGap=true;SaveHistory();}SetTimerState();
            }else SamplePower();
        }return TRUE;
    case WM_WTSSESSION_CHANGE:
        if(wp==WTS_SESSION_LOCK){g.locked=true;g.historyGap=true;SaveHistory();SetTimerState();}
        else if(wp==WTS_SESSION_UNLOCK){g.locked=false;g.historyGap=true;SamplePower(true);SetTimerState();}return 0;
    case WM_IMPORTED:
        if(lp==1){if(wp!=0){g.scheduledPending=false;g.refreshing=false;g.notice=L"Windows could not run the hourly refresh job. Re-run hourly setup to repair it.";}LayoutButtons();Redraw();return 0;}
        g.refreshing=false;
        if(wp==0){if(!ReadEnergy(data+L"\\energy.bin",g.energy,g.energyHeader))g.notice=L"Windows export finished, but its cache could not be read.";else{ReadAppPaths(data,g.appPaths);g.icons.clear();g.notice.clear();g.selected=-1;g.page=0;BuildView(Now());}}
        else g.notice=ReadStatus();LayoutButtons();Redraw();return 0;
    case WM_TRAY:{
        UINT event=LOWORD(lp);
        if(event==NIN_SELECT||event==NIN_KEYSELECT||event==WM_LBUTTONUP)ShowWidget();
        else if(event==WM_CONTEXTMENU||event==WM_RBUTTONUP){POINT p;GetCursorPos(&p);Popup(p);}return 0;
    }
    case WM_DPICHANGED:{g.scale=HIWORD(wp)/96.f*WidgetScale();auto* r=reinterpret_cast<RECT*>(lp);POINT position{r->left,r->top};if(g.desktop)MapWindowPoints(nullptr,g.desktopHost,&position,1);SetWindowPos(hwnd,nullptr,position.x,position.y,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);ResizeWidget();return 0;}
    case WM_DISPLAYCHANGE:ResizeWidget();return 0;
    case WM_SIZE:g.buffer.reset();return 0;
    case WM_EXITSIZEMOVE:ResizeWidget();SavePrefs();return 0;
    case WM_SYSCOMMAND:if((wp&0xfff0)==SC_MINIMIZE){Act(Hide);return 0;}if((wp&0xfff0)==SC_MAXIMIZE)return 0;break;
    case WM_CLOSE:Act(Hide);return 0;
    case WM_QUERYENDSESSION:SaveHistory();SavePrefs();return TRUE;
    case WM_ENDSESSION:if(wp){SaveHistory();SavePrefs();}return 0;
    case WM_DESTROY:
        SaveHistory();SavePrefs();for(auto h:g.notifications)UnregisterPowerSettingNotification(h);g.notifications.clear();
        WTSUnRegisterSessionNotification(hwnd);if(g.timer)KillTimer(hwnd,SAMPLE_TIMER);if(g.animating)KillTimer(hwnd,ANIM_TIMER);g.animating=false;
        Shell_NotifyIconW(NIM_DELETE,&g.tray);g.tray={};g.timer=false;g.timerInterval=0;
        if(g.trayIcon){DestroyIcon(g.trayIcon);g.trayIcon=nullptr;}g.trayKey=UINT32_MAX;g.hovering=g.tracking=false;
        g.window=nullptr;g.desktopHost=nullptr;g.desktop=false;g.tooltip=nullptr;g.buttons.clear();g.buffer.reset();
        if(!g.quitting&&g.desktopWanted&&!g.readOnly&&g.controller)PostMessageW(g.controller,WM_RECREATE,0,0);else PostQuitMessage(0);return 0;
    }return DefWindowProcW(hwnd,msg,wp,lp);
}
void EfficiencyMode(){
    PROCESS_POWER_THROTTLING_STATE throttle{};throttle.Version=PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    throttle.ControlMask=PROCESS_POWER_THROTTLING_EXECUTION_SPEED;throttle.StateMask=PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    SetProcessInformation(GetCurrentProcess(),ProcessPowerThrottling,&throttle,sizeof(throttle));
    MEMORY_PRIORITY_INFORMATION priority{MEMORY_PRIORITY_LOW};SetProcessInformation(GetCurrentProcess(),ProcessMemoryPriority,&priority,sizeof(priority));
}
bool CreateWidget(bool visible){
    INITCOMMONCONTROLSEX common{sizeof(common),ICC_STANDARD_CLASSES};InitCommonControlsEx(&common);
    WNDCLASSEXW wc{};wc.cbSize=sizeof(wc);wc.lpfnWndProc=WindowProc;wc.hInstance=GetModuleHandleW(nullptr);wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.lpszClassName=WINDOW_CLASS;wc.style=CS_DBLCLKS;
    if(!g.icon){auto icon=MakeIconBitmap();icon->GetHICON(&g.icon);}wc.hIcon=g.icon;wc.hIconSm=g.icon;RegisterClassExW(&wc);
    if(!g.controller){
        WNDCLASSEXW control{};control.cbSize=sizeof(control);control.lpfnWndProc=ControllerProc;control.hInstance=wc.hInstance;control.lpszClassName=CONTROL_CLASS;RegisterClassExW(&control);
        g.controller=CreateWindowExW(WS_EX_TOOLWINDOW,CONTROL_CLASS,L"Cell control",WS_POPUP,0,0,0,0,nullptr,nullptr,wc.hInstance,nullptr);
        if(!g.controller)return false;
    }
    HDC dc=GetDC(nullptr);g.scale=GetDeviceCaps(dc,LOGPIXELSX)/96.f*WidgetScale();ReleaseDC(nullptr,dc);
    POINT point{g.prefs.x==INT32_MIN?0:g.prefs.x,g.prefs.y==INT32_MIN?0:g.prefs.y};
    HMONITOR monitor=MonitorFromPoint(point,MONITOR_DEFAULTTONEAREST);MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(monitor,&mi);
    int w=Px(CW),h=Px(float(WidgetHeight()));
    int x=g.prefs.x==INT32_MIN?mi.rcWork.right-w-Px(24):g.prefs.x,y=g.prefs.y==INT32_MIN?mi.rcWork.bottom-h-Px(24):g.prefs.y;
    x=std::clamp(x,int(mi.rcWork.left),int(std::max(mi.rcWork.left,mi.rcWork.right-w)));y=std::clamp(y,int(mi.rcWork.top),int(std::max(mi.rcWork.top,mi.rcWork.bottom-h)));
    g.window=CreateWindowExW(WS_EX_TOOLWINDOW,WINDOW_CLASS,L"Cell battery",WS_POPUP|WS_THICKFRAME|WS_SYSMENU|WS_MINIMIZEBOX|WS_CLIPCHILDREN,x,y,w,h,nullptr,nullptr,wc.hInstance,nullptr);
    if(!g.window)return false;
    g.scale=GetDpiForWindow(g.window)/96.f*WidgetScale();
    DWORD corner=2;DwmSetWindowAttribute(g.window,33,&corner,sizeof(corner));BOOL dark=!g.lightApps;DwmSetWindowAttribute(g.window,20,&dark,sizeof(dark));
    COLORREF border=RGB(LINE.GetR(),LINE.GetG(),LINE.GetB());DwmSetWindowAttribute(g.window,34,&border,sizeof(border));
    CreateControls();ResizeWidget();
    if(visible){
        AddTray();RegisterNotifications();RetryDesktopPin();
        if(!g.hidden)ShowWindow(g.window,SW_SHOWNOACTIVATE);
        SamplePower(true);EnsurePill();if(g.controller)SetCoalescableTimer(g.controller,PILL_TIMER,3000,nullptr,1000);SetTimerState();
    }
    return true;
}

bool WriteBytes(const wstring&path,const void* p,DWORD n){HANDLE f=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);if(f==INVALID_HANDLE_VALUE)return false;bool ok=ExactWrite(f,p,n);CloseHandle(f);return ok;}
wstring Timestamp(uint64_t t){FILETIME f=FileTime(t);SYSTEMTIME s{};FileTimeToSystemTime(&f,&s);wchar_t b[64];swprintf(b,64,L"%04u-%02u-%02u:%02u:%02u:%02u.0000",s.wYear,s.wMonth,s.wDay,s.wHour,s.wMinute,s.wSecond);return b;}
std::string Utf8(const wstring&s){int n=WideCharToMultiByte(CP_UTF8,0,s.c_str(),int(s.size()),nullptr,0,nullptr,nullptr);std::string result(n,0);WideCharToMultiByte(CP_UTF8,0,s.c_str(),int(s.size()),result.data(),n,nullptr,nullptr);return result;}
int SelfTest(const wstring&folder){
    g.readOnly=true; // Tests must never persist their temporary state to the user's data.
    CreateDirectoryW(folder.c_str(),nullptr);std::string report;int passed=0,failed=0;
    auto check=[&](bool ok,const char*name){report+=(ok?"PASS ":"FAIL ");report+=name;report+="\r\n";if(ok)++passed;else ++failed;};
    check(EtaText(UINT32_MAX)==L"Estimating"&&EtaText(3600+42*60)==L"1h 42m","ETA formatting and unknown sentinel");
    check(EtaDisplayMinute(29)==EtaDisplayMinute(30)&&EtaDisplayMinute(89)!=EtaDisplayMinute(90),"ETA invalidation follows the displayed rounded-minute boundary");
    {
        DrainTrend trend;double watts=0;uint32_t span=0;bool ready=false;
        for(uint64_t t=0;t<=58000;t+=2000)ready=trend.Add(t,40000-uint32_t((t/1000)*1.25/10)*10,watts,span);
        check(!ready,"Recent drain waits for a full minute of capacity observations");
        for(uint64_t t=60000;t<=120000;t+=2000)ready=trend.Add(t,40000-uint32_t((t/1000)*1.25/10)*10,watts,span);
        check(ready&&std::abs(watts-4.5)<0.15&&span==120,"Recent drain resolves a quantized 4.5 W capacity trend");
        Power p;p.source=Battery;p.watts=p.reportedWatts=10.8;p.seconds=13000;
        ApplyRecentDrain(p,39000,4.5,120);
        check(p.recentDrain&&p.watts==4.5&&p.reportedWatts==10.8&&p.seconds==31200,"Capacity-derived draw and fallback ETA replace a mismatched reported rate");
        p.windowsEta=true;p.seconds=18000;ApplyRecentDrain(p,39000,4.5,120);
        check(p.seconds==18000&&p.windowsEta,"Windows ETA retains priority over a recent-drain fallback");
        ready=trend.Add(140000,39800,watts,span);check(!ready&&trend.count==1,"Sampling gaps discard stale drain estimates");
        trend.Reset();for(uint64_t t=0;t<=120000;t+=2000)ready=trend.Add(t,40000,watts,span);
        check(!ready,"An unchanged capacity counter never fabricates zero draw or an ETA");
        trend.Reset();for(uint64_t t=0;t<=120000;t+=2000)ready=trend.Add(t,t<64000?40000:39920,watts,span);
        check(!ready,"Insufficient capacity resolution keeps the reported-rate fallback");
        ready=trend.Add(122000,40000,watts,span);check(!ready&&trend.count==1,"Rising capacity resets a discharge trend");
        trend.Reset();for(uint64_t t=0;t<=600000;t+=2000)ready=trend.Add(t,40000-uint32_t((t/1000)*1.25/10)*10,watts,span);
        check(ready&&std::abs(watts-4.5)<0.15&&span<=120&&trend.count<=80,"A long observation stream remains bounded to the recent two-minute window");
        ready=trend.Add(602000,UINT32_MAX,watts,span);check(!ready&&trend.count==0,"Unavailable capacity clears the recent estimate");
    }
    {
        wstring previousData=data;Prefs previousPrefs=g.prefs;bool previousTop=g.top,previousDesktop=g.desktopWanted;int previousTab=g.tab;
        data=folder;LegacyPrefs legacy;legacy.x=123;legacy.y=234;legacy.top=1;legacy.expanded=1;
        bool saved=AtomicWrite(data+L"\\preferences.bin",&legacy,sizeof(legacy),nullptr,0);LoadPrefs();
        check(saved&&g.prefs.x==123&&g.prefs.y==234&&g.top&&g.tab==1&&!g.desktopWanted&&g.prefs.taskbar==1,"Oldest preferences migrate; expanded app usage opens on the Apps tab");
        PrefsV2 v2;v2.x=55;v2.y=66;v2.desktop=1;
        saved=AtomicWrite(data+L"\\preferences.bin",&v2,sizeof(v2),nullptr,0);LoadPrefs();
        check(saved&&g.prefs.x==55&&g.desktopWanted&&g.tab==0&&g.prefs.alerts==0,"Version 2 preferences migrate with the desktop pin and alerts off");
        Prefs current;current.x=321;current.y=432;current.desktop=1;current.tab=3;current.taskbar=0;current.alerts=5;
        saved=AtomicWrite(data+L"\\preferences.bin",&current,sizeof(current),nullptr,0);LoadPrefs();
        check(saved&&g.prefs.x==321&&g.prefs.y==432&&g.desktopWanted&&!g.top&&g.tab==3&&g.prefs.taskbar==0&&g.prefs.alerts==5,"Tab, taskbar and alert choices survive a save and reload");
        data=previousData;g.prefs=previousPrefs;g.top=previousTop;g.tab=previousTab;g.desktopWanted=previousDesktop;
    }
    uint64_t now=Now(),hour=now/3600*3600,stamp=hour-3600;wstring ts=Timestamp(stamp);
    check(ParseTimestamp(ts)==stamp,"SRUM UTC timestamp round trip");
    check(ParseTimestamp(L"2026-13-99:99:00:00")==0,"Reject invalid timestamps");
    check(ParseTimestamp(L"2026-10-15T13:00:00+01:00")==ParseTimestamp(L"2026-10-15T12:00:00Z"),"Explicit timezone offsets");
    wstring csv=L"AppId,TimeStamp,OnBattery,TotalEnergyConsumption,CPUEnergyConsumption\r\n";
    csv+=L"\"C:\\Apps\\alpha,one.exe\","+ts+L",1,3600,1000\r\n";
    csv+=L"C:\\Apps\\beta.exe,"+ts+L",0,7200,2000\r\n";
    csv+=L"\"C:\\Apps\\alpha,one.exe\","+ts+L",True,1800,500\r\n";
    csv+=L"C:\\Apps\\unknown.exe,"+ts+L",?,900,100\r\n";
    csv+=L"C:\\Apps\\old.exe,"+Timestamp(hour-25*3600)+L",1,999999,0\r\n";
    wstring path=folder+L"\\fixture.csv";std::string utf8=Utf8(csv);WriteBytes(path,utf8.data(),DWORD(utf8.size()));
    std::vector<Energy> records;EnergyHeader header;wstring error;
    bool parsed=ParseEnergy(path,now,records,header,error);
    check(parsed&&records.size()==3,"Stream CSV; quoted commas; aggregate same app and hour; omit old records");
    double sum=0;for(auto&e:records)sum+=e.mj;check(std::abs(sum-13500)<.01,"Prefer total energy without double counting CPU");
    g.energy=records;g.filter=0;g.selected=-1;BuildView(now);
    check(g.view.apps.size()==1&&std::abs(g.view.total-5400)<.01,"Battery filter excludes plugged-in and unknown-source energy");
    g.filter=1;BuildView(now);check(g.view.apps.size()==1&&std::abs(g.view.total-7200)<.01,"Plugged-in filter remains separate from charging");
    g.filter=2;BuildView(now);check(g.view.apps.size()==3&&std::abs(g.view.total-13500)<.01,"All-source totals preserve unknown attribution");
    g.selected=23;BuildView(now);check(g.view.apps.empty(),"Recorded-hour filtering");g.selected=-1;
    std::vector<uint8_t> utf16(2+csv.size()*2);utf16[0]=255;utf16[1]=254;memcpy(utf16.data()+2,csv.data(),csv.size()*2);WriteBytes(path,utf16.data(),DWORD(utf16.size()));
    check(ParseEnergy(path,now,records,header,error)&&records.size()==3,"UTF-16LE Windows exports");
    wstring componentCsv=L"AppId,TimeStamp,CPUEnergyConsumption,DiskEnergyConsumption\napp.exe,"+ts+L",12,8\n";
    utf8=Utf8(componentCsv);WriteBytes(path,utf8.data(),DWORD(utf8.size()));
    check(ParseEnergy(path,now,records,header,error)&&records.size()==1&&records[0].mj==20&&records[0].source==SourceUnknown,"Sum recognized components; never invent missing power source");
    wstring meterCsv=L"AppId, TimeStamp, OnBattery, TotalEnergyConsumption\nEMI_RAPL_Package0_PKG, "+ts+L", TRUE, 1000\nUnknown, "+ts+L", TRUE, 100\nC:\\Apps\\EMI_tool.exe, "+ts+L", TRUE, 50\n";
    utf8=Utf8(meterCsv);WriteBytes(path,utf8.data(),DWORD(utf8.size()));
    check(ParseEnergy(path,now,records,header,error)&&records.size()==2,"Exclude overlapping hardware-meter domains while retaining real EMI-named applications");
    auto originalEnergy=g.energy;g.energy=records;g.filter=0;g.selected=-1;BuildView(now);
    check(g.view.apps.size()==1&&g.view.total==50&&g.view.unassigned==100,"Keep Windows unattributed energy outside app shares");g.energy=originalEnergy;
    wstring unicodeCsv=L"AppId,TimeStamp,TotalEnergyConsumption\n\"C:\\Apps\\power \U0001F50B.exe\","+ts+L",12\n";
    utf8=Utf8(unicodeCsv);utf8="\xef\xbb\xbf"+utf8;WriteBytes(path,utf8.data(),DWORD(utf8.size()));
    check(ParseEnergy(path,now,records,header,error)&&records.size()==1&&wstring(records[0].name).find(L"\U0001F50B")!=wstring::npos,"UTF-8 BOM and supplementary Unicode app names");
    std::string largeCsv="AppId,TimeStamp,OnBattery,TotalEnergyConsumption\n";std::string asciiTime=Utf8(ts);
    for(size_t i=0;i<MAX_ENERGY+500;i++)largeCsv+="app"+std::to_string(i)+".exe,"+asciiTime+",1,1\n";
    WriteBytes(path,largeCsv.data(),DWORD(largeCsv.size()));bool bounded=ParseEnergy(path,now,records,header,error);double boundedTotal=0;for(auto&e:records)boundedTotal+=e.mj;
    check(bounded&&records.size()<=MAX_ENERGY&&boundedTotal==MAX_ENERGY+500,"Bound app-hour cache size and preserve overflow energy in Other apps");
    utf8="bad,columns\n1,2\n";WriteBytes(path,utf8.data(),DWORD(utf8.size()));
    check(!ParseEnergy(path,now,records,header,error),"Unsupported CSV schema produces a visible failure");
    utf8=Utf8(L"AppId,TimeStamp,TotalEnergyConsumption\napp.exe,"+ts+L",NaN\n");WriteBytes(path,utf8.data(),DWORD(utf8.size()));
    check(!ParseEnergy(path,now,records,header,error),"Reject non-finite energy instead of creating misleading totals");
    path=folder+L"\\cache.bin";header=EnergyHeader{};header.count=uint32_t(g.energy.size());header.updated=now;
    check(AtomicWrite(path,&header,sizeof(header),g.energy.data(),DWORD(g.energy.size()*sizeof(Energy))),"Atomic cache write");
    std::vector<Energy> loaded;EnergyHeader loadedHeader;
    check(ReadEnergy(path,loaded,loadedHeader)&&loaded.size()==g.energy.size(),"Validate and read binary cache");
    header.count=UINT32_MAX;AtomicWrite(path,&header,sizeof(header),nullptr,0);
    check(!ReadEnergy(path,loaded,loadedHeader)&&loaded.size()==g.energy.size(),"Reject corrupt cache length and retain previous data");
    {
        auto savedEnergy=g.energy;auto savedHeader=g.energyHeader;
        wstring scheduled=folder+L"\\scheduled";CreateDirectoryW(scheduled.c_str(),nullptr);g.scheduledFolder=scheduled;
        Energy record;record.hour=hour;record.id=99;record.source=Battery;record.mj=3600;wcscpy(record.name,L"Scheduled test app");
        EnergyHeader fresh;fresh.count=1;fresh.updated=now;fresh.latest=hour;
        WriteBytes(scheduled+L"\\hourly.enabled","enabled",7);AtomicWrite(scheduled+L"\\energy.bin",&fresh,sizeof(fresh),&record,sizeof(record));
        g.energyHeader.updated=now-60;g.scheduledPending=true;g.refreshing=true;
        bool changed=CheckScheduledCache(now,true);
        check(changed&&g.hourlyAvailable&&g.energy.size()==1&&g.energy[0].id==99,"A completed hourly cache is detected and loaded without elevation");
        check(!g.scheduledPending&&!g.refreshing,"An on-demand scheduled refresh finishes when its cache arrives");
        fresh.updated=now-120;record.id=100;AtomicWrite(scheduled+L"\\energy.bin",&fresh,sizeof(fresh),&record,sizeof(record));g.scheduledStamp={};CheckScheduledCache(now,true);
        check(g.energy[0].id==99&&g.energyHeader.updated==now,"An older scheduled cache cannot overwrite newer observations");
        fresh.count=UINT32_MAX;AtomicWrite(scheduled+L"\\energy.bin",&fresh,sizeof(fresh),nullptr,0);g.scheduledStamp={};CheckScheduledCache(now,true);
        check(g.energy[0].id==99,"A malformed scheduled cache preserves the current app history");
        g.energy=savedEnergy;g.energyHeader=savedHeader;g.scheduledFolder.clear();g.scheduledStamp={};g.scheduledStatusStamp={};g.hourlyAvailable=false;g.cacheCheck=0;
    }
    Sample a{now-300,80,Battery,0},b{now,79,Battery,0};check(CanJoin(a,b),"Connect observed battery samples");b.gap=1;check(!CanJoin(a,b),"Never interpolate across suspension or unobserved gaps");b.gap=0;b.utc=now+1200;check(!CanJoin(a,b),"Long sampling gaps remain blank");
    g.energy.clear();g.filter=0;BuildView(now);g.power=ReadPower();
    if(CreateWidget(false)){
        RECT r;GetClientRect(g.window,&r);check(r.right==Px(float(CW))&&r.bottom==Px(float(CH)),"The card has fixed dimensions at the display DPI");
        for(int tab=3;tab>=0;tab--)Act(TabBattery+tab);
        GetClientRect(g.window,&r);check(g.tab==0&&r.right==Px(float(CW))&&r.bottom==Px(float(CH)),"Switching tabs never resizes the card");
        check(GetDlgItem(g.window,TabWeek)&&GetDlgItem(g.window,Refresh)&&GetDlgItem(g.window,FilterBattery)&&GetDlgItem(g.window,DesktopPin),"Keyboard-accessible native button controls include tabs and desktop pin");
        auto shown=[](int id){return (GetWindowLongPtrW(GetDlgItem(g.window,id),GWL_STYLE)&WS_VISIBLE)!=0;}; // The test card itself is never shown.
        check(shown(TabApps)&&!shown(FilterBattery),"Controls follow the active tab");
        bool controlsFit=true;
        for(auto& c:controls)if(c.x<0||c.y<0||c.x+c.w>CW||c.y+c.h>CH||!c.tabs)controlsFit=false;
        check(controlsFit,"Every control fits inside the card");
        {
            RECT before{},after{};GetWindowRect(g.window,&before);
            HWND host=CreateWindowExW(0,L"STATIC",L"Cell test desktop",WS_POPUP,0,0,4096,4096,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
            bool pinned=host&&SetDesktopParent(host);GetWindowRect(g.window,&after);
            check(pinned&&GetParent(g.window)==host&&(GetWindowLongPtrW(g.window,GWL_STYLE)&WS_CHILD)&&before.left==after.left&&before.top==after.top,"Desktop pin preserves screen position and attaches the widget as a child");
            check(IsWindow(g.controller)&&GetParent(g.controller)==nullptr&&!(GetWindowLongPtrW(g.controller,GWL_STYLE)&WS_VISIBLE),"Hidden top-level controller stays reachable while the widget is pinned");
            bool unpinned=SetDesktopParent(nullptr);GetWindowRect(g.window,&after);
            check(unpinned&&!g.desktop&&GetParent(g.window)==nullptr&&(GetWindowLongPtrW(g.window,GWL_STYLE)&WS_POPUP)&&after.left==before.left&&after.top==before.top,"Unpin restores a normal floating window and its position");
            if(host)DestroyWindow(host);
        }
        {
            Power saved=g.power;g.power.watts=8.25;g.ringShown=float(std::max(0,g.power.percent));int width=Px(float(CW)),height=Px(float(CH));uint64_t renderTime=Now();
            Bitmap partial(width,height,PixelFormat32bppPARGB),reference(width,height,PixelFormat32bppPARGB);
            {Graphics gr(&partial);Paint(gr,width,height,renderTime);}
            g.power.watts=12.34;RECT dirty{Px(134),Px(80),Px(232),Px(198)};
            {Graphics gr(&partial);gr.SetClip(Rect(dirty.left,dirty.top,dirty.right-dirty.left,dirty.bottom-dirty.top));Paint(gr,width,height,renderTime,&dirty);}
            {Graphics gr(&reference);Paint(gr,width,height,renderTime);}
            BitmapData a{},b{};Rect all(0,0,width,height);bool equal=false;
            if(partial.LockBits(&all,ImageLockModeRead,PixelFormat32bppPARGB,&a)==Ok){
                if(reference.LockBits(&all,ImageLockModeRead,PixelFormat32bppPARGB,&b)==Ok){equal=true;for(int y=0;y<height;y++)if(memcmp(static_cast<BYTE*>(a.Scan0)+y*a.Stride,static_cast<BYTE*>(b.Scan0)+y*b.Stride,width*4)!=0){equal=false;break;}reference.UnlockBits(&b);}partial.UnlockBits(&a);
            }
            check(equal,"Partial draw-column redraw exactly matches a full render");g.power=saved;
        }
        g.hidden=false;g.displayOn=true;g.locked=false;g.suspended=false;SetTimerState();
        check(g.timer&&g.timerInterval==LIVE_INTERVAL_MS,"Visible widget uses the live two-second coalesced timer");
        uint64_t beforeReads=g.powerReads,deadline=GetTickCount64()+5200;
        while(GetTickCount64()<deadline){MSG message;while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}MsgWaitForMultipleObjects(0,nullptr,FALSE,50,QS_ALLINPUT);}
        check(g.powerReads>=beforeReads+2,"Actual Windows timer delivers repeated live power samples");
        g.hidden=true;SetTimerState();check(g.timer&&g.timerInterval==HIDDEN_INTERVAL_MS,"Hidden widget reduces sampling to five minutes");g.hidden=false;
        g.displayOn=false;SetTimerState();check(!g.timer,"No sampling timer while display is off");
        g.displayOn=true;g.locked=true;SetTimerState();check(!g.timer,"No sampling timer while session is locked");
        g.locked=false;g.suspended=true;SetTimerState();check(!g.timer,"No sampling timer during suspension");
        // Test windows are destroyed without persisting test preferences.
        g.readOnly=true;DestroyWindow(g.window);g.window=nullptr;
        if(g.controller){DestroyWindow(g.controller);g.controller=nullptr;}
    }else check(false,"Create hidden native test window");
    {
        Power saved=g.power;g.power=Power{};g.power.valid=g.power.present=true;g.power.source=Charging;g.power.percent=60;g.power.fullSeconds=3900;
        check(MainEta()==L"Full in 1h 05m","Charging shows a time-to-full estimate");
        g.power.source=Plugged;g.power.percent=80;check(MainEta()==L"Charging paused","Plugged in below full is reported as paused charging");
        g.power.source=Battery;g.power.percent=12;check(IsLow()&&StatusText()==L"Low battery","Low battery state is flagged at or below the threshold");
        g.power=saved;
        BatteryFacts facts;facts.valid=true;facts.designMWh=50000;facts.fullMWh=46000;check(facts.HealthPercent()==92,"Battery health is full-charge over design capacity");
        auto glyph=MakeTrayBitmap(16,76,Battery,false,false);check(glyph&&glyph->GetWidth()==16,"Tray glyph renders at small-icon size");
        {
            Power savedPower=g.power;auto savedHistory=g.history;auto savedDrain=g.drain;size_t savedCount=g.wattsCount;auto savedWatts=g.watts;Prefs savedPrefs=g.prefs;unsigned sent=g.alertsSent;
            g.power=Power{};g.power.valid=g.power.present=true;g.power.source=Battery;g.power.percent=50;g.power.remainingMWh=40000;
            g.wattsCount=0;for(int i=0;i<40;i++){g.watts[i]=float(8+(i%5));++g.wattsCount;}
            uint32_t low=0,high=0;
            check(EtaRange(low,high)&&low<high&&low>=uint32_t(40.0/12*3600)-60&&high<=uint32_t(40.0/8*3600)+60,"Time-left range spans the recent spread of draw");
            g.wattsCount=10;check(!EtaRange(low,high),"No range until about a minute of draw has been seen");
            g.history={Sample{now-4000,90,Charging,0},Sample{now-3000,95,Battery,0},Sample{now-2000,80,Battery,0},Sample{now-10,70,Battery,0}};
            uint64_t since=0;int from=0;check(StretchStart(since,from)&&since==now-3000&&from==95,"Since-unplug starts at the last switch to battery");
            g.drain.clear();for(int h=1;h<=3;h++)AddDrain(now/3600*3600-uint64_t(h)*3600,5000,3600);
            check(std::abs(TypicalWatts(now)-5)<0.01,"Usual draw comes from Cell's measured drain");
            auto days=WeekDays(now);check(std::abs(days[0].mwh+days[1].mwh+days[2].mwh+days[3].mwh+days[4].mwh+days[5].mwh+days[6].mwh-15000)<1,"The week view counts every measured hour once");
            g.prefs.alerts=3;Power before=g.power;
            before.source=Charging;before.percent=79;g.power.source=Charging;g.power.percent=80;CheckAlerts(before,now);
            before.percent=85;g.power.percent=86;CheckAlerts(before,now);
            before.source=Battery;before.percent=21;g.power.source=Battery;g.power.percent=20;CheckAlerts(before,now);
            check(g.alertsSent==sent+2,"Alerts fire once on crossing 80% while charging and 20% on battery");
            g.prefs.alerts=0;before.percent=21;CheckAlerts(before,now);check(g.alertsSent==sent+2,"Alerts stay quiet unless switched on");
            Bitmap pill(248,80,PixelFormat32bppARGB);{Graphics gr(&pill);gr.Clear(Color(0,0,0,0));gr.ScaleTransform(2,2);PaintPill(gr,124,40,true,false,false);}
            Color pixel;int ink=0;for(int y=0;y<80;y+=2)for(int x=0;x<248;x+=2){pill.GetPixel(x,y,&pixel);if(pixel.GetA()>200)++ink;}
            check(ink>20,"Taskbar pill draws its battery and text on a transparent background");
            g.power=savedPower;g.history=savedHistory;g.drain=savedDrain;g.wattsCount=savedCount;g.watts=savedWatts;g.prefs=savedPrefs;
        }
        auto savedHistory=g.history;g.history.clear();g.history.push_back(Sample{now-DAY/2,55,Battery,1});
        check(DisplayName(L"Mozilla.Firefox")==L"Firefox"&&DisplayName(L"Microsoft.WindowsTerminal")==L"Windows Terminal"&&DisplayName(L"python3.12")==L"Python3.12"&&DisplayName(L"rustdesk")==L"Rustdesk"&&DisplayName(L"claude")==L"Claude","App names are tidied for display without mangling versions");
        check(ShellParsingName(L"Mozilla.Firefox_n80bbvh6b1yt2!App")==L"shell:AppsFolder\\Mozilla.Firefox_n80bbvh6b1yt2!App"&&ShellParsingName(L"C:\\Apps\\a.exe")==L"C:\\Apps\\a.exe"&&ShellParsingName(L"Unknown").empty(),"Packaged apps and paths map to shell icon sources");
        {
            wchar_t windows[MAX_PATH]{};GetWindowsDirectoryW(windows,MAX_PATH);wchar_t device[512]{};wchar_t drive[3]={windows[0],L':',0};
            bool mapped=QueryDosDeviceW(drive,device,512)&&_wcsicmp(ShellParsingName(wstring(device)+(windows+2)).c_str(),windows)==0;
            check(mapped,"SRUM device paths convert to drive-letter paths");
        }
        {
            wstring appsCsv=L"AppId,TimeStamp,OnBattery,TotalEnergyConsumption\nC:\\Apps\\alpha.exe,"+ts+L",1,10\nC:\\Apps\\alpha.exe,"+ts+L",0,5\nUnknown,"+ts+L",1,3\n";
            std::string bytes=Utf8(appsCsv);wstring csvPath=folder+L"\\apps.csv";WriteBytes(csvPath,bytes.data(),DWORD(bytes.size()));
            std::vector<Energy> rows;EnergyHeader head;wstring err;std::vector<AppPath> ids;std::map<uint64_t,wstring> loadedPaths;
            bool ok=ParseEnergy(csvPath,now,rows,head,err,&ids)&&ids.size()==1&&WriteAppPaths(folder,ids)&&ReadAppPaths(folder,loadedPaths);
            check(ok&&loadedPaths.size()==1&&loadedPaths.begin()->first==Hash(L"C:\\Apps\\alpha.exe")&&loadedPaths.begin()->second==L"C:\\Apps\\alpha.exe","App identities round-trip through apps.bin and exclude unattributed energy");
        }
        {
            auto savedEnergy=g.energy;int savedFilter=g.filter;g.energy.clear();
            for(int i=0;i<12;i++){Energy e;e.hour=now/3600*3600;e.id=uint64_t(i+1);e.source=Battery;e.mj=100.0+i;swprintf(e.name,64,L"App%d",i+1);g.energy.push_back(e);}
            Energy plugged;plugged.hour=now/3600*3600;plugged.id=1;plugged.source=Plugged;plugged.mj=1000;g.energy.push_back(plugged);
            g.filter=0;g.selected=-1;g.appSelected=true;g.appName=L"App1";g.page=9;BuildView(now);
            check(g.view.Pages()==3&&g.page==2,"Five apps per page and the page clamps to the last one");
            check(std::abs(g.view.battery[23]-1266)<.01&&g.view.plugged[23]==0&&std::abs(g.view.app[23]-100)<.01,"Chart follows the source filter and overlays the selected app");
            g.appSelected=false;g.page=0;g.energy=savedEnergy;g.filter=savedFilter;BuildView(now);
        }
        {
            // Duplicate identities with one display name merge; the largest supplies the icon.
            auto savedEnergy=g.energy;g.energy.clear();Energy a,b;
            a.hour=b.hour=now/3600*3600;a.source=b.source=Battery;a.id=11;b.id=12;a.mj=300;b.mj=100;
            wcscpy(a.name,L"steam");wcscpy(b.name,L"steamwebhelper");a.parts[PartCpu]=200;a.parts[PartDisplay]=100;b.parts[PartNetwork]=100;
            g.energy={a,b};g.filter=0;g.selected=-1;BuildView(now);
            check(g.view.apps.size()==1&&g.view.apps[0].name==L"Steam"&&g.view.apps[0].mj==400&&g.view.apps[0].id==11&&g.view.apps[0].parts[PartNetwork]==100,"Executables of one product merge, keeping their component breakdown");
            g.energy=savedEnergy;BuildView(now);
        }
        {
            wstring partsCsv=L"AppId,TimeStamp,OnBattery,TotalEnergyConsumption,CPUEnergyConsumption,DisplayEnergyConsumption,NetworkEnergyConsumption\nC:\\Apps\\a.exe,"+ts+L",1,100,60,30,\n";
            std::string bytes=Utf8(partsCsv);wstring csvPath=folder+L"\\parts.csv";WriteBytes(csvPath,bytes.data(),DWORD(bytes.size()));
            std::vector<Energy> rows;EnergyHeader head;wstring err;
            check(ParseEnergy(csvPath,now,rows,head,err)&&rows.size()==1&&rows[0].mj==100&&rows[0].parts[PartCpu]==60&&rows[0].parts[PartDisplay]==30&&rows[0].parts[PartNetwork]==0,"Component energy is kept as detail; a blank component does not reject the row");
            // A version 1 cache (older widget or hourly helper) still loads.
            EnergyHeader old;old.version=1;old.count=1;old.updated=now;LegacyEnergy legacy;legacy.hour=now/3600*3600;legacy.id=5;legacy.mj=42;legacy.source=Battery;wcscpy(legacy.name,L"Old");
            AtomicWrite(folder+L"\\legacy.bin",&old,sizeof(old),&legacy,sizeof(legacy));
            std::vector<Energy> loaded;EnergyHeader loadedHeader;
            check(ReadEnergy(folder+L"\\legacy.bin",loaded,loadedHeader)&&loaded.size()==1&&loaded[0].mj==42&&loaded[0].parts[PartCpu]==0&&loadedHeader.version==2,"Version 1 caches load and are upgraded in memory");
        }
        {
            // Measured drain: a 3 Wh drop across 10:30–11:30 splits evenly into both hours; charging resets the baseline.
            auto savedDrain=g.drain;Power savedPower=g.power;g.drain.clear();g.drainBase=UINT32_MAX;
            uint64_t base=now/3600*3600-2*3600;g.power=Power{};g.power.source=Battery;
            g.power.remainingMWh=40000;RecordDrain(base+1800);g.power.remainingMWh=37000;RecordDrain(base+5400);
            check(g.drain.size()==2&&std::abs(DrainAt(base)-1500)<1&&std::abs(DrainAt(base+3600)-1500)<1,"Measured battery loss is spread across the hours it spanned");
            g.power.source=Charging;g.power.remainingMWh=39000;RecordDrain(base+5500);g.power.source=Battery;g.power.remainingMWh=39000;RecordDrain(base+5600);
            g.power.remainingMWh=38900;RecordDrain(base+5700);
            check(std::abs(DrainAt(base+3600)-1600)<1,"Charging restarts measurement instead of counting the gain");
            g.drain=savedDrain;g.power=savedPower;g.drainBase=UINT32_MAX;
        }
        {
            int savedTab=g.tab;g.tab=2;
            bool first=SampleLive();Sleep(300);for(volatile int spin=0;spin<20000000;spin=spin+1){}bool second=SampleLive();
            bool self=false;for(auto& app:g.live)if(app.cpu>0)self=true;
            check(first&&second&&g.liveSnaps.size()==2&&self&&g.liveBusy>0,"Live sampler measures per-app CPU time from one system snapshot");
            StopLive();check(g.liveSnaps.empty()&&g.liveBuffer.capacity()==0,"Leaving the live tab frees its buffers");
            g.tab=savedTab;
        }
        const Sample* hit=HistoryAt(CHART_X+CHART_W/2,now);
        check(hit&&hit->percent==55&&!HistoryAt(CHART_X+10,now),"Chart hover finds the nearest sample and ignores unrecorded time");
        g.history=savedHistory;
    }
    report+="\r\n"+std::to_string(passed)+" passed, "+std::to_string(failed)+" failed.\r\n";
    WriteBytes(folder+L"\\test-results.txt",report.data(),DWORD(report.size()));return failed?1:0;
}
void SeedDemo(){
    uint64_t now=Now(),hour=now/3600*3600;
    g.power={true,true,false,76,Battery,4*3600+38*60,false,9.8};g.history.clear();
    g.power.reportedWatts=9.8;g.power.recentDrain=true;g.power.drainSpan=120;g.power.remainingMWh=38100;g.power.fullMWh=50100;
    g.facts.valid=true;g.facts.designMWh=56000;g.facts.fullMWh=50100;g.facts.cycles=214;
    // A week of days: charge for 2.5 h, then discharge; the last sample matches the 76% reading.
    for(int i=0;i<=7*288;i++){
        uint64_t utc=now-7*DAY+uint64_t(i)*300;int phase=(i+63)%144,percent;uint8_t source;
        if(phase<36){percent=20+phase*75/36;source=Charging;}else{percent=95-(phase-36)*75/108;source=Battery;}
        g.history.push_back(Sample{utc,int16_t(percent),source,0});
    }
    g.energy.clear();
    const wchar_t* names[]={L"Microsoft Edge",L"Visual Studio Code",L"Mozilla.Firefox",L"Windows Explorer",L"Spotify",L"Notepad",L"System",L"Other apps"};
    const double weights[]={5,3,2.4,1.6,1.2,0.6,1,0.5};
    for(int i=0;i<24;i++)for(int app=0;app<8;app++){
        if(app==2&&(i<14||i>19))continue; // A burst of activity for the overlay preview.
        Energy e;e.hour=hour-(23-i)*3600;e.id=app+1;e.source=(i>=8&&i<=11)?Plugged:Battery;e.mj=weights[app]*(30+(i*17+app*9)%70)*3600;wcscpy(e.name,names[app]);g.energy.push_back(e);
    }
    g.appPaths.clear();g.icons.clear();g.drain.clear();
    // Sample measured drain: Windows typically explains only part of what the battery loses.
    for(uint64_t h=hour-7*DAY;h<=hour;h+=3600){
        auto at=std::lower_bound(g.history.begin(),g.history.end(),h,[](const Sample& a,uint64_t t){return a.utc<t;});
        if(at!=g.history.end()&&at->source==Battery)AddDrain(h,4200*(0.8+0.45*((h/3600*7)%5)/4.0)*(0.85+0.15*((h/DAY)%3)),3600);
    }
    g.wattsCount=0;for(int i=0;i<150;i++){g.watts[i]=float(9.8*(0.82+0.36*((i*37)%11)/10.0));++g.wattsCount;}
    g.ringShown=76;
    for(auto& e:g.energy){e.parts[PartCpu]=float(e.mj*0.55);e.parts[PartDisplay]=float(e.mj*0.25);e.parts[PartNetwork]=float(e.mj*0.12);e.parts[PartOther]=float(e.mj*0.08);}
    g.appPaths[1]=L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe";
    g.appPaths[4]=L"C:\\Windows\\explorer.exe";g.appPaths[6]=L"C:\\Windows\\System32\\notepad.exe";
    g.energyHeader.updated=now-20*60;g.energyHeader.latest=hour;g.filter=0;g.selected=-1;
    g.appSelected=true;g.appName=DisplayName(names[2]);BuildView(now);
}
int Render(const wstring&folder,bool demo){
    CreateDirectoryW(folder.c_str(),nullptr);g.power=ReadPower();g.facts=ReadBatteryFacts();LoadHistory();LoadDrain();ReadEnergy(data+L"\\energy.bin",g.energy,g.energyHeader);BuildView(Now());if(demo)SeedDemo();
    float oldScale=g.scale;g.scale=2;CLSID cls;if(!PngEncoder(cls))return 1;
    const wchar_t* tabNames[]={L"battery",L"apps",L"now",L"week"};
    for(int tab=0;tab<4;tab++){
        if(demo&&tab==2)continue; // The live tab shows this machine's real processes, never sample data.
        g.tab=tab;g.page=0;g.ringShown=float(std::max(0,g.power.percent));
        if(tab==2){SampleLive();Sleep(2000);SampleLive();}
        int width=Px(float(CW)),height=Px(float(CH));Bitmap image(width,height,PixelFormat32bppARGB);Graphics gr(&image);Paint(gr,width,height,Now());gr.ResetTransform();
        for(auto& c:controls){
            if(!((c.tabs>>g.tab)&1))continue;if(c.id==ClearHour&&g.selected<0&&!g.appSelected)continue;if((c.id==PreviousPage||c.id==NextPage)&&ListPages()<=1)continue;
            auto state=gr.Save();gr.TranslateTransform(float(Px(float(c.x))),float(Px(float(c.y))));gr.ScaleTransform(g.scale,g.scale);DrawButton(gr,c.id,float(c.w),float(c.h));gr.Restore(state);
        }
        if(demo){Graphics mark(&image);mark.ScaleTransform(g.scale,g.scale);TextCenter(mark,L"SAMPLE DATA",70,0,100,0,MUTED);}
        if(image.Save((folder+L"\\"+(demo?L"sample-":L"live-")+tabNames[tab]+L".png").c_str(),&cls,nullptr)!=Ok)return 1;
    }
    StopLive();g.tab=0;
    {
        int width=Px(float(CW)),height=Px(PEEK_H);Bitmap image(width,height,PixelFormat32bppARGB);{Graphics gr(&image);PaintPeek(gr,width,height,Now());}
        image.Save((folder+L"\\"+(demo?L"sample-":L"live-")+L"peek.png").c_str(),&cls,nullptr);
    }
    {
        // Taskbar pill on dark and light taskbars, at 32 px (one line) and 48 px (two lines), 2x.
        const float heights[]={32,48};Bitmap strip(2*2*150,2*2*48,PixelFormat32bppARGB);Graphics sg(&strip);
        sg.SetSmoothingMode(SmoothingModeAntiAlias);sg.SetTextRenderingHint(TextRenderingHintAntiAlias);
        for(int light=0;light<2;light++)for(int k=0;k<2;k++){
            float h=heights[k],w=k?132.f:124.f;auto state=sg.Save();
            SolidBrush bar(light?Color(255,238,238,238):Color(255,30,30,30));sg.FillRectangle(&bar,light*300,k*96,300,96);
            sg.TranslateTransform(float(light*300+10),float(k*96)+(96-2*h)/2);sg.ScaleTransform(2,2);
            PaintPill(sg,w,h-4,h>=40,light!=0,k==1);sg.Restore(state);
        }
        strip.Save((folder+L"\\"+(demo?L"sample-":L"live-")+L"pill.png").c_str(),&cls,nullptr);
    }
    {
        // Tray glyph preview, enlarged 4x: rows are dark/light taskbars; columns are states x sizes.
        const int sizes[]={16,20,24,32};const std::tuple<int,uint8_t,bool> states[]={{76,Battery,false},{100,Plugged,false},{43,Charging,false},{9,Battery,true}};
        const int cell=136;Bitmap strip(16*cell,2*cell,PixelFormat32bppARGB);Graphics sg(&strip);
        sg.SetInterpolationMode(InterpolationModeNearestNeighbor);sg.SetPixelOffsetMode(PixelOffsetModeHalf);
        for(int light=0;light<2;light++){
            SolidBrush taskbar(light?Color(255,238,238,238):Color(255,32,32,32));sg.FillRectangle(&taskbar,0,light*cell,16*cell,cell);
            for(int st=0;st<4;st++)for(int si=0;si<4;si++){
                int size=sizes[si],x=(st*4+si)*cell+(cell-size*4)/2,y=light*cell+(cell-size*4)/2;
                auto icon=MakeTrayBitmap(size,std::get<0>(states[st]),std::get<1>(states[st]),std::get<2>(states[st]),light!=0);
                sg.DrawImage(icon.get(),Rect(x,y,size*4,size*4),0,0,size,size,UnitPixel);
            }
        }
        strip.Save((folder+L"\\tray-preview.png").c_str(),&cls,nullptr);
    }
    SaveIcon(folder+L"\\CellBattery.ico");g.scale=oldScale;return 0;
}
int DesktopCheck(const wstring& folder){
    g.readOnly=true;CreateDirectoryW(folder.c_str(),nullptr);std::string report;
    if(!CreateWidget(false))return 1;
    HWND host=FindDesktopHost();g.desktopWanted=true;bool pinned=ApplyDesktopPin();
    wchar_t name[80]{};if(host)GetClassNameW(host,name,80);
    report="Desktop host: "+Utf8(name)+"\r\n";
    report+=pinned&&GetParent(g.window)==host?"PASS Attach hidden test widget to the actual desktop\r\n":"FAIL Attach hidden test widget to the actual desktop\r\n";
    RECT size{};GetClientRect(g.window,&size);report+="Pinned size: "+std::to_string(size.right)+" x "+std::to_string(size.bottom)+" px; DPI "+std::to_string(GetDpiForWindow(g.window))+"\r\n";
    bool controller=IsWindow(g.controller)&&FindWindowW(CONTROL_CLASS,nullptr)!=nullptr;
    report+=controller?"PASS Command controller remains a top-level window\r\n":"FAIL Command controller remains a top-level window\r\n";
    g.desktopWanted=false;bool unpinned=ApplyDesktopPin();
    report+=unpinned&&!GetParent(g.window)?"PASS Detach the test widget from the desktop\r\n":"FAIL Detach the test widget from the desktop\r\n";
    g.quitting=true;DestroyWindow(g.window);DestroyWindow(g.controller);g.controller=nullptr;
    WriteBytes(folder+L"\\desktop-check.txt",report.data(),DWORD(report.size()));return pinned&&controller&&unpinned?0:1;
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int){
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32|LOAD_LIBRARY_SEARCH_USER_DIRS);
    int argc=0;LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    wstring mode=argc>1?argv[1]:L"",folder=argc>2?argv[2]:root;LocalFree(argv);
    if(mode==L"--refresh-to"&&argc<3)return ERROR_INVALID_PARAMETER;
    InitPaths(mode==L"--refresh-to"?folder:L"");if(folder.empty())folder=root;
    if(mode==L"--refresh-to"||mode==L"--refresh"){EfficiencyMode();return ExportEnergy();}
    if(mode==L"--quit"){HWND widget=FindRunningWidget();if(widget)PostMessageW(widget,WM_COMMAND,Quit,0);return 0;}
    if(mode==L"--import"){
        std::vector<Energy> records;EnergyHeader h;wstring error;std::vector<AppPath> identities;
        if(!ParseEnergy(folder,Now(),records,h,error,&identities)){WriteStatus(error);return 1;}
        WriteAppPaths(data,identities);
        bool saved=AtomicWrite(data+L"\\energy.bin",&h,sizeof(h),records.data(),DWORD(records.size()*sizeof(Energy)));
        WriteStatus(saved?L"":L"Cannot write the app history cache.");return saved?0:1;
    }
    GdiplusStartupInput input;input.SuppressBackgroundThread=TRUE;GdiplusStartupOutput output;ULONG_PTR token=0,hook=0;
    if(GdiplusStartup(&token,&input,&output)!=Ok)return 1;
    if(output.NotificationHook)output.NotificationHook(&hook);Fonts();int result=0;
    HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE); // Shell icon lookups.
    ReadAppPaths(data,g.appPaths);
    if(mode==L"--self-test")result=SelfTest(folder);
    else if(mode==L"--desktop-check")result=DesktopCheck(folder);
    else if(mode==L"--render"||mode==L"--render-demo")result=Render(folder,mode==L"--render-demo");
    else{
        HANDLE mutex=CreateMutexW(nullptr,TRUE,L"Local\\CellBatteryWidget1");
        if(mutex&&GetLastError()==ERROR_ALREADY_EXISTS){HWND existing=FindRunningWidget();if(existing)PostMessageW(existing,WM_COMMAND,Show,0);CloseHandle(mutex);}
        else{
            ApplyTheme(LightApps());EfficiencyMode();InitScheduledPaths();LoadPrefs();LoadHistory();LoadDrain();ReadEnergy(data+L"\\energy.bin",g.energy,g.energyHeader);CheckScheduledCache(Now(),true);BuildView(Now());g.power=ReadPower();
            if(g.readOnly)g.notice=L"Data folder is read-only. History cannot be saved here.";
            if(CreateWidget()){MSG message;while(GetMessageW(&message,nullptr,0,0)>0){
                if(message.message==WM_KEYDOWN&&message.wParam==VK_F5){Act(Refresh);continue;}
                if(message.message==WM_KEYDOWN&&message.wParam==VK_ESCAPE){Act(Hide);continue;}
                if(!IsDialogMessageW(g.window,&message)){TranslateMessage(&message);DispatchMessageW(&message);}
            }}
            else result=1;
            if(mutex){ReleaseMutex(mutex);CloseHandle(mutex);}
        }
    }
    if(g.peek)DestroyWindow(g.peek);if(g.pill&&IsWindow(g.pill))DestroyWindow(g.pill);g.pill=nullptr;
    if(g.controller){DestroyWindow(g.controller);g.controller=nullptr;}g.buffer.reset();for(auto& f:g.fonts)f.reset();if(g.icon)DestroyIcon(g.icon);if(g.trayIcon)DestroyIcon(g.trayIcon);
    g.icons.clear();if(SUCCEEDED(com))CoUninitialize();
    if(output.NotificationUnhook&&hook)output.NotificationUnhook(hook);GdiplusShutdown(token);return result;
}
