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
constexpr int CW=272, HEIGHT=320, DETAIL_HEIGHT=366, EXPANDED_HEIGHT=HEIGHT+DETAIL_HEIGHT;
// Compact layout anchors (logical units before scaling).
constexpr float TILE_Y=198, TILE_H=34, CHART_X=20, CHART_Y=244, CHART_W=232, CHART_H=38, LEGEND_Y=293;
constexpr float WIDGET_SCALE=0.8f;
constexpr uint64_t DAY=86400, HISTORY_DAYS=7;
constexpr size_t MAX_HISTORY=12000, MAX_ENERGY=16384;
constexpr uint64_t UNATTRIBUTED_ID=UINT64_MAX;
constexpr UINT WM_TRAY=WM_APP+1, WM_IMPORTED=WM_APP+2, WM_RECREATE=WM_APP+3;
constexpr UINT_PTR SAMPLE_TIMER=1, DESKTOP_RETRY_TIMER=2;
constexpr UINT LIVE_INTERVAL_MS=2000, HIDDEN_INTERVAL_MS=300000;
const wchar_t* WINDOW_CLASS=L"CellBatteryWidget1";
const wchar_t* CONTROL_CLASS=L"CellBatteryControl1";
enum Source : uint8_t { Battery=0, Charging=1, Plugged=2, SourceUnknown=3 };
enum Action { Expand=100, Menu, Refresh, FilterBattery, FilterPlugged, FilterAll,
              ClearHour, PreviousPage, NextPage, Show=200, Topmost, Startup,
              Settings, DataFolder, Hide, Quit, BatteryReadings, DesktopPin, FilterLive=300 };
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
struct Prefs { uint32_t magic=0x32504342; int32_t x=INT32_MIN,y=INT32_MIN; uint8_t top=0,expanded=0,desktop=0; };
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
    std::array<std::unique_ptr<Font>,7> fonts;
    std::vector<HWND> buttons;
    std::vector<HPOWERNOTIFY> notifications;
    NOTIFYICONDATAW tray{};
    HICON icon=nullptr;
} g;
wstring root,data;
const Color BG(255,19,25,26), PANEL(255,25,32,33), LINE(255,43,53,53),
    FG(255,236,243,236), MUTED(255,146,164,157), GREEN(255,183,235,171),
    GOLD(255,237,192,121), BLUE(255,144,173,189), TRACK(255,39,51,47), RED(255,240,134,118);
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
        if(size.QuadPart==sizeof(LegacyPrefs)){
            LegacyPrefs old;
            if(ExactRead(f,&old,sizeof(old))&&old.magic==0x31504342&&old.top<=1&&old.expanded<=1){p.x=old.x;p.y=old.y;p.top=old.top;p.expanded=old.expanded;g.prefs=p;}
        }else if(size.QuadPart==sizeof(p)&&ExactRead(f,&p,sizeof(p))&&p.magic==0x32504342&&p.top<=1&&p.expanded<=1&&p.desktop<=1)g.prefs=p;
    }
    CloseHandle(f);g.top=g.prefs.top;g.expanded=g.prefs.expanded;g.desktopWanted=g.prefs.desktop!=0;
}
void SavePrefs() {
    if(g.readOnly||!g.window)return;
    RECT r{};GetWindowRect(g.window,&r);g.prefs.x=r.left;g.prefs.y=r.top;
    g.prefs.expanded=g.expanded;g.prefs.top=g.top;g.prefs.desktop=g.desktopWanted;
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
        if(g.filter!=1){v.measured[i]=DrainAt(v.hours[i])*3600.0;if(g.selected<0||g.selected==i)v.measuredTotal+=v.measured[i];}
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
Color Tint(Color c,int alpha){return Color(BYTE(alpha),c.GetR(),c.GetG(),c.GetB());}
void Rounded(Graphics& gr,RectF r,float radius,Color c) {
    GraphicsPath path;float d=radius*2;
    path.AddArc(r.X,r.Y,d,d,180,90);path.AddArc(r.GetRight()-d,r.Y,d,d,270,90);
    path.AddArc(r.GetRight()-d,r.GetBottom()-d,d,d,0,90);path.AddArc(r.X,r.GetBottom()-d,d,d,90,90);path.CloseFigure();
    SolidBrush b(c);gr.FillPath(&b,&path);
}
void Fonts(){
    const float sizes[]={10,11,12,14,22,56,16};
    for(size_t i=0;i<g.fonts.size();i++)g.fonts[i]=std::make_unique<Font>(L"Segoe UI",sizes[i],i==3||i==5?FontStyleBold:FontStyleRegular,UnitPixel);
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
// Nearest recorded sample to a chart x position, within about one sampling interval.
const Sample* HistoryAt(float x,uint64_t now){
    uint64_t start=now-DAY;double target=double(start)+double(x-CHART_X)/CHART_W*DAY,bestGap=420;const Sample* best=nullptr;
    for(auto& s:g.history){if(s.utc<start||s.utc>now)continue;double d=std::abs(double(s.utc)-target);if(d<bestGap){bestGap=d;best=&s;}}
    return best;
}
void PaintHistory(Graphics& gr,uint64_t now) {
    const float x=CHART_X,y=CHART_Y,w=CHART_W,h=CHART_H;
    Pen grid(LINE,1);grid.SetDashStyle(DashStyleDot);
    gr.DrawLine(&grid,x,y,x+w,y);gr.DrawLine(&grid,x,y+h/2,x+w,y+h/2);
    Stroke(gr,x,y+h,x+w,y+h,LINE);
    bool drawn=false;uint64_t start=now-DAY;
    // Each run of joinable, same-source samples gets a soft area fill and one polyline.
    for(size_t i=1;i<g.history.size();){
        auto& a=g.history[i-1];auto& b=g.history[i];
        if(a.utc<start||b.utc>now||!CanJoin(a,b)){++i;continue;}
        uint8_t source=a.source;std::vector<PointF> points{PointF(ChartX(a.utc,start),ChartY(a.percent))};size_t j=i;
        while(j<g.history.size()&&g.history[j-1].source==source&&g.history[j].utc<=now&&CanJoin(g.history[j-1],g.history[j])){
            points.emplace_back(ChartX(g.history[j].utc,start),ChartY(g.history[j].percent));++j;
        }
        std::vector<PointF> area(points);area.emplace_back(points.back().X,y+h);area.emplace_back(points.front().X,y+h);
        SolidBrush fill(Tint(SourceColor(source),38));gr.FillPolygon(&fill,area.data(),int(area.size()));
        Pen line(SourceColor(source),1.7f);line.SetLineJoin(LineJoinRound);line.SetStartCap(LineCapRound);line.SetEndCap(LineCapRound);
        gr.DrawLines(&line,points.data(),int(points.size()));drawn=true;i=j;
    }
    if(!drawn)TextAt(gr,L"History builds while Cell runs",x,y+h/2-7,w,0,MUTED);
    else if(!g.history.empty()&&!g.hovering){
        auto&s=g.history.back();if(s.utc>=start&&s.utc<=now){SolidBrush b(SourceColor(s.source));gr.FillEllipse(&b,ChartX(s.utc,start)-2.5f,ChartY(s.percent)-2.5f,5.f,5.f);}
    }
    if(g.hovering){
        const Sample* s=HistoryAt(g.hoverX,now);float hx=s?ChartX(s->utc,start):g.hoverX;
        Stroke(gr,hx,y-2,hx,y+h,Tint(FG,90),1);
        if(s){SolidBrush ring(BG),dot(SourceColor(s->source));gr.FillEllipse(&ring,hx-4.f,ChartY(s->percent)-4.f,8.f,8.f);gr.FillEllipse(&dot,hx-2.5f,ChartY(s->percent)-2.5f,5.f,5.f);}
    }
}
void PaintLegend(Graphics& gr,uint64_t now){
    if(g.hovering){
        const Sample* s=HistoryAt(g.hoverX,now);
        if(s){
            wstring text=Clock(s->utc)+L"  "+std::to_wstring(s->percent)+L"% · "+SourceName(s->source);
            TextAt(gr,text,20,LEGEND_Y,205,1,SourceColor(s->source));
        }else{
            uint64_t t=now-DAY+uint64_t(double(std::clamp(g.hoverX,CHART_X,CHART_X+CHART_W)-CHART_X)/CHART_W*DAY);
            TextAt(gr,Clock(t)+L"  not recorded",20,LEGEND_Y,205,1,MUTED);
        }
        return;
    }
    TextAt(gr,L"24h",20,LEGEND_Y+1,26,0,MUTED);
    const std::pair<Color,const wchar_t*> keys[]={{GREEN,L"Battery"},{GOLD,L"Charging"},{BLUE,L"Plugged"}};
    float kx=48;
    for(auto& k:keys){SolidBrush b(k.first);gr.FillEllipse(&b,kx,LEGEND_Y+6,4.f,4.f);TextAt(gr,k.second,kx+8,LEGEND_Y+1,52,0,MUTED);kx+=k.second[0]==L'C'?58.f:50.f;}
}
wstring Wh(uint32_t mwh){if(mwh==UINT32_MAX||mwh==0)return L"—";wchar_t b[24];swprintf(b,24,mwh>=100000?L"%.0f Wh":L"%.1f Wh",mwh/1000.0);return b;}
void Tile(Graphics& gr,float x,float w,const wchar_t* label,const wstring& value,Color color){
    Rounded(gr,RectF(x,TILE_Y,w,TILE_H),6,TRACK);
    TextAt(gr,label,x+8,TILE_Y+5,w-12,0,MUTED);
    TextAt(gr,value,x+8,TILE_Y+17,w-12,2,color);
}
void PaintTiles(Graphics& gr){
    const float gap=6,w=(CHART_W-2*gap)/3;wchar_t b[32];
    int health=g.facts.HealthPercent();wstring healthText=L"—";Color healthColor=FG;
    if(health>0){swprintf(b,32,L"%d%%",std::min(health,100));healthText=b;healthColor=health>=80?FG:health>=60?GOLD:RED;}
    Tile(gr,CHART_X,w,L"HEALTH",healthText,healthColor);
    uint32_t remaining=g.power.remainingMWh!=UINT32_MAX?g.power.remainingMWh:g.power.capacityMWh;
    Tile(gr,CHART_X+w+gap,w,L"STORED",Wh(remaining),FG);
    if(g.facts.valid&&g.facts.cycles>0)Tile(gr,CHART_X+2*(w+gap),w,L"CYCLES",std::to_wstring(g.facts.cycles),FG);
    else Tile(gr,CHART_X+2*(w+gap),w,L"FULL",Wh(g.power.fullMWh!=UINT32_MAX?g.power.fullMWh:g.facts.fullMWh),FG);
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
// ---- Energy-by-app panel (coordinates are relative to the panel top, HEIGHT) ----
constexpr float U_SEG_Y=54,U_LABEL_Y=90,U_CHART_Y=110,U_CHART_H=40,U_SUMMARY_Y=174,U_LIST_Y=198,U_ROW_H=27,U_FOOT_Y=336;
void TextCenter(Graphics& gr,const wstring& s,float x,float y,float width,int font,Color color){
    StringFormat fmt(StringFormat::GenericTypographic());fmt.SetFormatFlags(StringFormatFlagsNoWrap);fmt.SetTrimming(StringTrimmingEllipsisCharacter);
    fmt.SetAlignment(StringAlignmentCenter);SolidBrush b(color);gr.DrawString(s.c_str(),int(s.size()),g.fonts[font].get(),RectF(x,y,width,76),&fmt,&b);
}
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
bool LiveActive(){return g.expanded&&g.filter==3&&!g.hidden&&g.displayOn&&!g.locked&&!g.suspended;}
void StopLive(){g.liveSnaps.clear();g.live.clear();g.liveBuffer.clear();g.liveBuffer.shrink_to_fit();g.wattsCount=0;g.liveBusy=0;}
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
bool LiveTab(){return g.filter==3;}
double HourEnergy(int i){return g.view.battery[i]+g.view.plugged[i]+g.view.unknown[i];}
int UsageHourAt(float x){return x<20||x>=252?-1:std::clamp(int((x-20)/(232.f/24)),0,23);}
size_t ListCount(){return LiveTab()?g.live.size():g.view.apps.size();}
int ListPages(){return std::max(1,int((ListCount()+APP_ROWS-1)/APP_ROWS));}
wstring WhText(double mj){
    double wh=mj/3.6e6;wchar_t b[32];
    if(wh<0.005)return L"<0.01 Wh";
    swprintf(b,32,wh<10?L"%.2f Wh":L"%.1f Wh",wh);return b;
}
uint32_t FullChargeMWh(){return g.power.fullMWh!=UINT32_MAX?g.power.fullMWh:g.facts.valid?g.facts.fullMWh:0;}
wstring UsageFreshness(uint64_t now){
    if(LiveTab())return L"Live CPU use and battery draw · not stored";
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
        swprintf(b,200,L"CPU %.0f%% busy · top apps by CPU time, last 30 s",g.liveBusy);return b;
    }
    if(g.appSelected){
        const AppTotal* app=nullptr;for(auto& a:g.view.apps)if(a.name==g.appName){app=&a;break;}
        if(!app)return L"No energy for this app in the current selection";
        wstring s=WhText(app->mj);
        if(uint32_t full=FullChargeMWh()){swprintf(b,200,L" · ≈%.1f%% of charge",app->mj/3600.0/full*100);s+=b;}
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
        if(measured>0){
            swprintf(b,200,L"Battery lost %ls · Windows estimates cover %.0f%% of it",WhText(measured).c_str(),std::min(100.0,100*covered/measured));return b;
        }
        swprintf(b,200,L"Apps used %ls on battery · real drain is being measured",WhText(g.view.total).c_str());return b;
    }
    double all=g.view.total+g.view.unassigned;
    if(all<=0)return L"";
    swprintf(b,200,L"Apps used %ls · %.0f%% not assigned to an app",WhText(g.view.total).c_str(),100*g.view.unassigned/all);return b;
}
void PaintAppIcon(Graphics& gr,Bitmap* icon,const wstring& name,uint64_t key,float y){
    if(icon){
        auto mode=gr.GetInterpolationMode();gr.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        gr.DrawImage(icon,RectF(20,y,18,18));gr.SetInterpolationMode(mode);
    }else{
        Color c=AvatarColor(key);Rounded(gr,RectF(20,y,18,18),5,Tint(c,40));
        TextCenter(gr,name.empty()?L"·":wstring(1,wchar_t(towupper(name[0]))),20,y+2,18,0,c);
    }
}
void PaintLiveChart(Graphics& gr){
    const float x=20,y=U_CHART_Y,w=232,h=U_CHART_H;
    size_t n=std::min(g.wattsCount,g.watts.size());float peak=1;
    for(size_t i=0;i<n;i++)peak=std::max(peak,g.watts[i]);
    Pen grid(LINE,1);grid.SetDashStyle(DashStyleDot);gr.DrawLine(&grid,x,y,x+w,y);gr.DrawLine(&grid,x,y+h/2,x+w,y+h/2);
    if(n>=2){
        std::vector<PointF> points;
        for(size_t i=0;i<n;i++){
            float value=g.watts[(g.wattsCount-n+i)%g.watts.size()];
            points.emplace_back(x+w-float(n-1-i)*w/float(g.watts.size()-1),y+h-h*value/peak);
        }
        std::vector<PointF> area(points);area.emplace_back(points.back().X,y+h);area.emplace_back(points.front().X,y+h);
        SolidBrush fill(Tint(GREEN,40));gr.FillPolygon(&fill,area.data(),int(area.size()));
        Pen line(GREEN,1.6f);line.SetLineJoin(LineJoinRound);gr.DrawLines(&line,points.data(),int(points.size()));
        wchar_t b[24];swprintf(b,24,L"peak %.0f W",peak);TextAt(gr,b,x+w-70,U_LABEL_Y,70,0,MUTED,true);
    }
    Stroke(gr,x,y+h+0.5f,x+w,y+h+0.5f,LINE);
    TextAt(gr,L"5 min ago",x,y+h+5,80,0,MUTED);TextAt(gr,L"now",x+w-45,y+h+5,45,0,MUTED,true);
}
void PaintHourChart(Graphics& gr,Color accent){
    const float x=20,y=U_CHART_Y,w=232,h=U_CHART_H,step=w/24,barW=step-2.5f;
    double peak=0;for(int i=0;i<24;i++)peak=std::max({peak,HourEnergy(i),g.view.measured[i]});
    for(int i=0;i<24;i++){
        float left=x+i*step;
        if(g.selected==i)Rounded(gr,RectF(left-1.25f,y-4,step,h+6),3,Tint(accent,30));
        else if(g.hoverHour==i)Rounded(gr,RectF(left-1.25f,y-4,step,h+6),3,Tint(FG,14));
        if(peak<=0){Rounded(gr,RectF(left,y+h-3,barW,3),1.5f,LINE);continue;}
        bool dim=(g.selected>=0&&g.selected!=i)||g.appSelected;
        const double values[]={g.view.battery[i],g.view.plugged[i],g.view.unknown[i]};
        const Color colors[]={GREEN,BLUE,MUTED}; // Same meaning as the battery history: plugged is blue.
        float yy=y+h;
        for(int k=0;k<3;k++){
            if(values[k]<=0)continue;float bh=std::max(1.5f,float(values[k]/peak*h));yy-=bh;
            SolidBrush b(Tint(colors[k],dim?60:215));gr.FillRectangle(&b,left,yy,barW,bh);
        }
        // Outline: energy the battery actually lost in that hour, measured by Cell.
        if(g.view.measured[i]>0){
            float mh=std::max(2.f,float(g.view.measured[i]/peak*h));Pen outline(Tint(FG,dim?70:150),1);
            gr.DrawRectangle(&outline,left+0.5f,y+h-mh,barW-1,mh);
        }
        if(g.appSelected&&g.view.app[i]>0){float ah=std::max(2.f,float(g.view.app[i]/peak*h));Rounded(gr,RectF(left,y+h-ah,barW,ah),1.2f,FG);}
    }
    Stroke(gr,x,y+h+0.5f,x+w,y+h+0.5f,LINE);
    TextAt(gr,Clock(g.view.hours[0]),x,y+h+5,60,0,MUTED);
    TextCenter(gr,Clock(g.view.hours[12]),x+w/2-30,y+h+5,60,0,MUTED);
    TextAt(gr,L"now",x+w-45,y+h+5,45,0,MUTED,true);
}
void PaintUsage(Graphics& gr,uint64_t now){
    auto state=gr.Save();gr.TranslateTransform(0,float(HEIGHT));
    SolidBrush pb(PANEL);gr.FillRectangle(&pb,0.f,0.f,float(CW),float(DETAIL_HEIGHT));Stroke(gr,20,0,252,0,LINE);
    Color accent=FilterAccent();
    TextAt(gr,LiveTab()?L"Right now":L"Energy by app",20,14,190,3,FG);
    TextAt(gr,UsageFreshness(now),20,34,196,0,g.notice.empty()||LiveTab()?MUTED:GOLD);
    Rounded(gr,RectF(20,U_SEG_Y,232,26),8,TRACK); // Segmented-control track; the filter buttons sit on it.
    // Chart label row.
    wstring label;Color labelColor=MUTED;bool legend=false;
    if(LiveTab()){
        label=g.power.source==Battery?L"Battery draw, last 5 minutes":L"On external power · battery draw not measured";
    }else if(g.hoverHour>=0){
        int i=g.hoverHour;wchar_t b[120];
        wstring span=Clock(g.view.hours[i])+L"–"+Clock(g.view.hours[i]+3600);
        if(g.view.measured[i]>0)swprintf(b,120,L"%ls · lost %ls · apps %ls",span.c_str(),WhText(g.view.measured[i]).c_str(),WhText(HourEnergy(i)).c_str());
        else swprintf(b,120,L"%ls · apps %ls",span.c_str(),WhText(HourEnergy(i)).c_str());
        label=b;labelColor=FG;
    }else if(g.selected>=0){label=Clock(g.view.hours[g.selected])+L"–"+Clock(g.view.hours[g.selected]+3600)+L" selected";labelColor=accent;}
    else if(g.appSelected){label=g.appName+L" · last 24 hours";labelColor=FG;}
    else{label=L"Last 24 hours";legend=g.filter!=1;}
    TextAt(gr,label,20,U_LABEL_Y,legend||LiveTab()?150:205,0,labelColor);
    if(legend){Pen outline(Tint(FG,150),1);gr.DrawRectangle(&outline,178.5f,U_LABEL_Y+2.5f,6.f,9.f);TextAt(gr,L"battery lost",188,U_LABEL_Y,64,0,MUTED,true);}
    if(LiveTab())PaintLiveChart(gr);else PaintHourChart(gr,accent);
    TextAt(gr,UsageSummary(),20,U_SUMMARY_Y,232,0,g.appSelected&&!LiveTab()?FG:MUTED);
    // List.
    if(LiveTab()){
        if(g.live.empty())TextAt(gr,L"First results appear in a few seconds.",20,U_LIST_Y+8,232,0,MUTED);
        for(int row=0;row<APP_ROWS;row++){
            size_t idx=size_t(g.page*APP_ROWS+row);if(idx>=g.live.size())break;
            auto& a=g.live[idx];float ry=U_LIST_Y+row*U_ROW_H;
            if(g.hoverRow==row)Rounded(gr,RectF(14,ry-3,244,U_ROW_H-1),6,Tint(FG,12));
            PaintAppIcon(gr,LiveIcon(a),a.name,Hash(a.name),ry);
            TextAt(gr,a.name,46,ry+1,140,1,FG);
            wchar_t b[24];swprintf(b,24,a.cpu<0.1?L"<0.1%% CPU":L"%.1f%% CPU",a.cpu);TextAt(gr,b,180,ry+1,72,1,accent,true);
            Rounded(gr,RectF(46,ry+17,206,3),1.5f,LINE);Rounded(gr,RectF(46,ry+17,std::max(3.f,float(206*a.share/100)),3),1.5f,accent);
        }
    }else if(g.view.apps.empty()){
        wstring message=g.energyHeader.updated?L"No app energy for this selection":L"Load Windows app history";
        TextAt(gr,message,20,U_LIST_Y+8,232,2,FG);
        TextAt(gr,g.refreshing?L"Windows is preparing the export…":!g.notice.empty()?g.notice:L"Refresh ↻ loads Windows' saved estimates.",20,U_LIST_Y+30,232,0,MUTED);
        TextAt(gr,L"Or open Now for live CPU use without waiting.",20,U_LIST_Y+50,232,0,MUTED);
    }else{
        for(int row=0;row<APP_ROWS;row++){
            size_t idx=size_t(g.page*APP_ROWS+row);if(idx>=g.view.apps.size())break;
            auto& a=g.view.apps[idx];float ry=U_LIST_Y+row*U_ROW_H;
            bool chosen=g.appSelected&&a.name==g.appName;
            if(chosen)Rounded(gr,RectF(14,ry-3,244,U_ROW_H-1),6,Tint(accent,28));
            else if(g.hoverRow==row)Rounded(gr,RectF(14,ry-3,244,U_ROW_H-1),6,Tint(FG,12));
            PaintAppIcon(gr,AppIcon(a.id),a.name,a.id,ry);
            TextAt(gr,a.name,46,ry+1,112,1,FG);
            wchar_t b[24];double share=g.view.total>0?100*a.mj/g.view.total:0;swprintf(b,24,share<1?L"<1%%":L"%.0f%%",share);
            TextAt(gr,WhText(a.mj),158,ry+1,58,0,MUTED,true);
            TextAt(gr,b,216,ry+1,36,1,chosen?FG:accent,true);
            Rounded(gr,RectF(46,ry+17,206,3),1.5f,LINE);
            Rounded(gr,RectF(46,ry+17,std::max(3.f,float(206*share/100)),3),1.5f,accent);
        }
    }
    // Footer: how current the data is, and paging.
    wstring foot;
    if(LiveTab())foot=L"Updates every 4 s while open";
    else if(g.energyHeader.latest)foot=L"Windows data through "+Clock(g.energyHeader.latest);
    TextAt(gr,foot,20,U_FOOT_Y+5,140,0,MUTED);
    if(ListPages()>1)TextAt(gr,std::to_wstring(g.page+1)+L" / "+std::to_wstring(ListPages()),148,U_FOOT_Y+5,48,0,MUTED,true);
    gr.Restore(state);
}
void Paint(Graphics& gr,int width,int height,uint64_t now,const RECT* dirty=nullptr){
    gr.SetSmoothingMode(SmoothingModeAntiAlias);gr.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    auto touches=[&](float x,float y,float w,float h){return !dirty||(dirty->right>Px(x)&&dirty->left<Px(x+w)&&dirty->bottom>Px(y)&&dirty->top<Px(y+h));};
    SolidBrush background(BG);gr.FillRectangle(&background,0,0,width,height);gr.ScaleTransform(g.scale,g.scale);
    if(touches(0,0,272,42)){
        SolidBrush dot(StateColor());gr.FillEllipse(&dot,20.f,23.f,6.f,6.f);TextAt(gr,L"cell",33,16,160,3,FG);
    }
    if(touches(0,42,272,70)){
        wstring percentage=g.power.percent<0?L"—":std::to_wstring(g.power.percent);
        TextAt(gr,percentage,17,43,180,5,IsLow()?RED:FG);
        RectF percentBounds;
        gr.MeasureString(percentage.c_str(),int(percentage.size()),g.fonts[5].get(),PointF(0,0),StringFormat::GenericTypographic(),&percentBounds);
        TextAt(gr,L"%",21+percentBounds.Width,80,36,4,MUTED);
        const float bx=214,by=78;Rounded(gr,RectF(bx,by,33,17),4,LINE);Rounded(gr,RectF(bx+33,by+5,3,7),1.5f,LINE);
        if(g.power.percent>=0)Rounded(gr,RectF(bx+3,by+3,std::max(1.f,27.f*g.power.percent/100),11),2,StateColor());
        if(g.power.source==Charging){PointF bolt[]={{229,77},{224,86},{229,86},{226,95},{235,83},{230,83}};SolidBrush brush(BG);gr.FillPolygon(&brush,bolt,6);}
    }
    if(touches(20,118,172,23))TextAt(gr,StatusText(),20,121,172,1,StateColor());
    if(touches(196,118,59,23)&&g.power.watts>0){
        // Sign shows direction: energy into (+) or out of (−) the battery.
        wchar_t b[30];const wchar_t* sign=g.power.source==Charging?L"+":g.power.source==Battery?L"−":L"";
        swprintf(b,30,L"%ls%.2f W",sign,g.power.watts);TextAt(gr,b,197,121,55,1,MUTED,true);
    }
    if(touches(18,146,239,30))TextAt(gr,MainEta(),20,149,235,4,FG);
    if(touches(18,177,241,18))TextAt(gr,EtaLabel(),20,179,238,0,MUTED);
    if(touches(18,TILE_Y-2,236,TILE_H+4))PaintTiles(gr);
    if(touches(16,CHART_Y-4,240,CHART_H+8))PaintHistory(gr,now);
    if(touches(0,LEGEND_Y-4,228,22))PaintLegend(gr,now);
    if(g.expanded&&touches(0,HEIGHT,CW,DETAIL_HEIGHT))PaintUsage(gr,now);
}
wstring ButtonCaption(int id){
    switch(id){case Expand:return g.expanded?L"Collapse app usage":L"Expand app usage";case Menu:return L"Menu";case Refresh:return g.hourlyAvailable?L"Refresh Windows app history":L"Refresh Windows app history (administrator consent)";
    case FilterBattery:return L"On battery";case FilterPlugged:return L"Plugged in";case FilterAll:return L"All";case FilterLive:return L"Now";case ClearHour:return L"Clear selection";
    case PreviousPage:return L"Previous apps";case NextPage:return L"Next apps";case DesktopPin:return g.desktopWanted?L"Unpin from desktop":L"Pin to desktop (behind app windows)";}return L"";
}
bool IsSelected(int id){return (id==DesktopPin&&g.desktop)||(id==FilterBattery&&g.filter==0)||(id==FilterPlugged&&g.filter==1)||(id==FilterAll&&g.filter==2)||(id==FilterLive&&g.filter==3);}
void DrawButton(Graphics& gr,int id,float w,float h,bool pressed=false,bool focus=false){
    gr.SetSmoothingMode(SmoothingModeAntiAlias);gr.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    bool filter=id==FilterBattery||id==FilterPlugged||id==FilterAll||id==FilterLive;
    Color parent=id==Expand||id==Menu||id==DesktopPin?BG:filter?TRACK:PANEL;
    SolidBrush parentBrush(parent);gr.FillRectangle(&parentBrush,0.f,0.f,w,h);Color fg=MUTED;
    bool selected=IsSelected(id);
    if(id==Expand){Rounded(gr,RectF(0,0,w,h),7,TRACK);fg=GREEN;}
    if(selected&&filter){Rounded(gr,RectF(0,0,w,h),6,Color(255,57,72,67));fg=id==FilterPlugged?BLUE:id==FilterAll?FG:GREEN;}
    else if(selected){Rounded(gr,RectF(0,0,w,h),6,Tint(GREEN,20));fg=GREEN;}
    if(pressed)Rounded(gr,RectF(0,0,w,h),6,Tint(FG,24));
    if(filter){TextCenter(gr,ButtonCaption(id),0,(h-14)/2,w,1,fg);if(focus){Pen p(Tint(GREEN,170),1);p.SetDashStyle(DashStyleDot);gr.DrawRectangle(&p,2.f,2.f,w-5,h-5);}return;}
    switch(id){
    case Expand:VerticalChevron(gr,w/2,h/2,g.expanded,fg);break;
    case DesktopPin:PinIcon(gr,w/2,h/2,fg);break;
    case Menu:{SolidBrush b(fg);for(int i=0;i<3;i++)gr.FillEllipse(&b,w/2-6+i*5,h/2-1,2.f,2.f);break;}
    case Refresh:RefreshIcon(gr,w/2,h/2,g.refreshing?LINE:fg);break;
    case ClearHour:Stroke(gr,w/2-3,h/2-3,w/2+3,h/2+3,fg);Stroke(gr,w/2+3,h/2-3,w/2-3,h/2+3,fg);break;
    case PreviousPage:Chevron(gr,w/2,h/2,true,fg);break;
    case NextPage:Chevron(gr,w/2,h/2,false,fg);break;
    default:TextAt(gr,ButtonCaption(id),9,(h-14)/2,w-18,1,fg);break;
    }
    if(focus){Pen p(Tint(GREEN,170),1);p.SetDashStyle(DashStyleDot);gr.DrawRectangle(&p,2.f,2.f,w-5,h-5);}
}
struct ControlPosition { int id,x,y,w,h;bool details; };
const ControlPosition controls[]={
    {DesktopPin,200,12,28,28,false},{Menu,232,12,28,28,false},{Expand,230,int(LEGEND_Y)-6,30,26,false},{Refresh,224,HEIGHT+10,28,28,true},
    {FilterBattery,22,HEIGHT+56,64,22,true},{FilterPlugged,86,HEIGHT+56,62,22,true},{FilterAll,148,HEIGHT+56,48,22,true},{FilterLive,196,HEIGHT+56,54,22,true},
    {ClearHour,230,HEIGHT+86,22,20,true},{PreviousPage,200,HEIGHT+334,24,22,true},{NextPage,228,HEIGHT+334,24,22,true}
};
void LayoutButtons(){
    for(auto& c:controls){HWND b=GetDlgItem(g.window,c.id);if(!b)continue;
        MoveWindow(b,Px(c.x),Px(c.y),Px(c.w),Px(c.h),TRUE);
        bool visible=!c.details||g.expanded;
        if(c.id==ClearHour&&((g.selected<0&&!g.appSelected)||LiveTab()))visible=false;
        if(c.id==PreviousPage||c.id==NextPage){if(ListPages()<=1)visible=false;EnableWindow(b,c.id==PreviousPage?g.page>0:g.page+1<ListPages());}
        if(c.id==Refresh){EnableWindow(b,!g.refreshing);if(LiveTab())visible=false;}
        if(c.id==Refresh)SetWindowTextW(b,ButtonCaption(c.id).c_str());
        if(c.id==Expand)SetWindowTextW(b,ButtonCaption(c.id).c_str());
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
void SamplePower(bool force=false){
    ++g.powerReads;
    Power previous=g.power;uint64_t lastHistory=g.history.empty()?0:g.history.back().utc,previousSample=g.lastSample;
    g.power=ReadPower();auto now=Now();RecordPower(now,force);RecordDrain(now);g.lastSample=now;
    if(LiveActive()){
        g.watts[g.wattsCount%g.watts.size()]=g.power.source==Battery?float(g.power.watts):0.f;++g.wattsCount;
        if(g.liveSnaps.empty()||GetTickCount64()-g.liveSnaps.back().tick>=LIVE_PERIOD_MS){
            SampleLive();RECT panel{0,Px(float(HEIGHT)),Px(CW),Px(float(EXPANDED_HEIGHT))};InvalidateRect(g.window,&panel,FALSE);
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
    UpdateTray();
    if(structural){Redraw();return;}
    if(g.hidden||!g.displayOn||g.locked||g.suspended)return;
    auto invalidate=[](float x,float y,float width,float height){RECT rect{Px(x),Px(y),Px(x+width),Px(y+height)};InvalidateRect(g.window,&rect,FALSE);};
    if(std::lround(previous.watts*100)!=std::lround(p.watts*100))invalidate(196,118,59,23);
    if(EtaDisplayMinute(previous.seconds)!=EtaDisplayMinute(p.seconds)||EtaDisplayMinute(previous.fullSeconds)!=EtaDisplayMinute(p.fullSeconds))invalidate(18,146,239,30);
    if(previous.remainingMWh/100!=p.remainingMWh/100||previous.capacityMWh/100!=p.capacityMWh/100||previous.fullMWh/100!=p.fullMWh/100)invalidate(18,TILE_Y-2,236,TILE_H+4);
    if(!g.history.empty()&&lastHistory!=g.history.back().utc){invalidate(16,CHART_Y-4,240,CHART_H+8);if(g.hovering)invalidate(0,LEGEND_Y-4,228,22);}
    if(g.expanded&&now/60!=previousSample/60)invalidate(18,HEIGHT+249,180,22);
}
UINT DesiredRefreshInterval(){return !g.displayOn||g.locked||g.suspended?0:g.hidden?HIDDEN_INTERVAL_MS:LIVE_INTERVAL_MS;}
void SetTimerState(){
    UINT interval=DesiredRefreshInterval();
    if(interval!=LIVE_INTERVAL_MS)meter.trend.Reset();
    if(interval&&(!g.timer||g.timerInterval!=interval)){
        g.timer=SetCoalescableTimer(g.window,SAMPLE_TIMER,interval,nullptr,g.hidden?5000:250)!=0;
        g.timerInterval=g.timer?interval:0;
    }else if(!interval&&g.timer){KillTimer(g.window,SAMPLE_TIMER);g.timer=false;g.timerInterval=0;}
}
int WidgetHeight(){return g.expanded?EXPANDED_HEIGHT:HEIGHT;}
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
    g.scale=GetDpiForWindow(g.window)/96.f*WIDGET_SCALE;
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
void Popup(POINT p){
    HMENU menu=CreatePopupMenu();
    AppendMenuW(menu,MF_STRING,Show,L"Show widget");
    AppendMenuW(menu,MF_STRING|(g.desktopWanted?MF_CHECKED:0),DesktopPin,L"Pin to desktop");
    AppendMenuW(menu,MF_STRING|(g.desktopWanted?MF_GRAYED:0)|(!g.desktopWanted&&g.top?MF_CHECKED:0),Topmost,L"Keep above other windows");
    AppendMenuW(menu,MF_STRING|(StartupEnabled()?MF_CHECKED:0),Startup,L"Start with Windows");AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
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
    case Expand:g.expanded=!g.expanded;ResizeWidget();SavePrefs();break;
    case Menu:{RECT r;GetWindowRect(GetDlgItem(g.window,Menu),&r);Popup(POINT{r.right,r.bottom});break;}
    case Refresh:StartRefresh();break;
    case FilterBattery:case FilterPlugged:case FilterAll:case FilterLive:
        g.filter=id==FilterBattery?0:id==FilterPlugged?1:id==FilterAll?2:3;g.page=0;g.hoverRow=g.hoverHour=-1;
        if(g.filter==3&&g.liveSnaps.empty()){SampleLive();g.watts[g.wattsCount%g.watts.size()]=g.power.source==Battery?float(g.power.watts):0.f;++g.wattsCount;}else if(g.filter!=3)StopLive();
        BuildView(Now());LayoutButtons();Redraw();break;
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
void CreateControls(){
    for(auto& c:controls){
        HWND button=CreateWindowExW(0,L"BUTTON",ButtonCaption(c.id).c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,Px(c.x),Px(c.y),Px(c.w),Px(c.h),g.window,reinterpret_cast<HMENU>(INT_PTR(c.id)),GetModuleHandleW(nullptr),nullptr);
        g.buttons.push_back(button);
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
    if(msg==taskbarCreated){if(IsWindow(g.window)){AddTray();RetryDesktopPin();}return 0;}
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
    if(msg==WM_DESTROY){KillTimer(hwnd,DESKTOP_RETRY_TIMER);return 0;}
    return DefWindowProcW(hwnd,msg,wp,lp);
}
LRESULT CALLBACK WindowProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
    static UINT taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated");
    if(msg==taskbarCreated){if(!g.controller)AddTray();return 0;}
    switch(msg){
    case WM_NCCALCSIZE:if(wp)return 0;break;
    case WM_NCHITTEST:{
        POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ScreenToClient(hwnd,&p);
        if(!g.desktop&&p.y>=0&&p.y<Px(40)&&p.x<Px(196))return HTCAPTION;
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
        DrawButton(gr,int(d->CtlID),float(d->rcItem.right-d->rcItem.left)/g.scale,float(d->rcItem.bottom-d->rcItem.top)/g.scale,(d->itemState&ODS_SELECTED)!=0,(d->itemState&ODS_FOCUS)!=0);return TRUE;
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
        if(g.desktop&&GET_Y_LPARAM(lp)<Px(40)&&GET_X_LPARAM(lp)<Px(196)){
            g.dragging=true;GetCursorPos(&g.dragStart);GetWindowRect(hwnd,&g.dragRect);SetCapture(hwnd);
        }return 0;
    case WM_MOUSEMOVE:{
        if(g.dragging){
            POINT cursor{};GetCursorPos(&cursor);POINT position{g.dragRect.left+cursor.x-g.dragStart.x,g.dragRect.top+cursor.y-g.dragStart.y};
            MapWindowPoints(nullptr,g.desktopHost,&position,1);
            SetWindowPos(hwnd,nullptr,position.x,position.y,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
            return 0;
        }
        // Hovering the 24h chart reads out the nearest recorded sample in the legend row.
        float x=GET_X_LPARAM(lp)/g.scale,y=GET_Y_LPARAM(lp)/g.scale;
        bool over=x>=CHART_X-2&&x<=CHART_X+CHART_W+2&&y>=CHART_Y-6&&y<=CHART_Y+CHART_H+4;
        float hx=std::clamp(x,CHART_X,CHART_X+CHART_W);
        if(over!=g.hovering||(over&&std::abs(hx-g.hoverX)>=0.5f)){
            g.hovering=over;g.hoverX=hx;
            RECT rect{0,Px(CHART_Y-6),Px(228),Px(LEGEND_Y+18)};InvalidateRect(hwnd,&rect,FALSE);
        }
        // Energy panel: hovering an hour previews its share; hovering a row highlights it.
        int hour=-1,row=-1;
        if(g.expanded){
            float py=y-HEIGHT;
            if(!LiveTab()&&py>=U_CHART_Y-4&&py<=U_CHART_Y+U_CHART_H+2)hour=UsageHourAt(x);
            if(x>=14&&x<258&&py>=U_LIST_Y-3&&py<U_LIST_Y+APP_ROWS*U_ROW_H-3){
                row=int((py-U_LIST_Y+3)/U_ROW_H);if(size_t(g.page*APP_ROWS+row)>=ListCount())row=-1;
            }
        }
        if(hour!=g.hoverHour){g.hoverHour=hour;RECT rect{0,Px(HEIGHT+U_LABEL_Y-2),Px(CW),Px(HEIGHT+U_CHART_Y+U_CHART_H+4)};InvalidateRect(hwnd,&rect,FALSE);}
        if(row!=g.hoverRow){g.hoverRow=row;RECT rect{0,Px(HEIGHT+U_LIST_Y-4),Px(CW),Px(HEIGHT+U_LIST_Y+APP_ROWS*U_ROW_H)};InvalidateRect(hwnd,&rect,FALSE);}
        if((over||hour>=0||row>=0)&&!g.tracking){TRACKMOUSEEVENT t{sizeof(t),TME_LEAVE,hwnd,0};g.tracking=TrackMouseEvent(&t)!=0;}
        return 0;
    }
    case WM_SETCURSOR:
        if(LOWORD(lp)==HTCLIENT&&(g.hoverHour>=0||g.hoverRow>=0)){SetCursor(LoadCursorW(nullptr,IDC_HAND));return TRUE;}
        break;
    case WM_MOUSELEAVE:
        g.tracking=false;
        if(g.hovering){g.hovering=false;RECT rect{0,Px(CHART_Y-6),Px(228),Px(LEGEND_Y+18)};InvalidateRect(hwnd,&rect,FALSE);}
        if(g.hoverHour>=0||g.hoverRow>=0){g.hoverHour=g.hoverRow=-1;RECT rect{0,Px(float(HEIGHT)),Px(CW),Px(float(EXPANDED_HEIGHT))};InvalidateRect(hwnd,&rect,FALSE);}
        return 0;
    case WM_SETTINGCHANGE:
        if(lp&&wcscmp(reinterpret_cast<const wchar_t*>(lp),L"ImmersiveColorSet")==0){bool light=LightTaskbar();if(light!=g.lightTaskbar){g.lightTaskbar=light;UpdateTray();}}
        break;
    case WM_CAPTURECHANGED:g.dragging=false;return 0;
    case WM_LBUTTONUP:{
        if(g.dragging){g.dragging=false;ReleaseCapture();ResizeWidget();SavePrefs();return 0;}
        float x=GET_X_LPARAM(lp)/g.scale,y=GET_Y_LPARAM(lp)/g.scale;
        float py=y-HEIGHT;
        if(LiveTab()){}else if(g.expanded&&py>=U_CHART_Y-4&&py<=U_CHART_Y+U_CHART_H+2&&UsageHourAt(x)>=0){
            int hour=UsageHourAt(x);g.selected=g.selected==hour?-1:hour; // Clicking the selected hour clears it.
            g.page=0;BuildView(Now());LayoutButtons();Redraw();
        }else if(g.expanded&&x>=14&&x<258&&py>=U_LIST_Y-3&&py<U_LIST_Y+APP_ROWS*U_ROW_H-3){
            // Clicking an app overlays its hourly energy on the chart; clicking it again clears it.
            size_t idx=size_t(g.page*APP_ROWS+int((py-U_LIST_Y+3)/U_ROW_H));
            if(idx<g.view.apps.size()){
                auto& a=g.view.apps[idx];
                if(g.appSelected&&g.appName==a.name)g.appSelected=false;else{g.appSelected=true;g.appName=a.name;}
                BuildView(Now());LayoutButtons();Redraw();
            }
        }return 0;
    }
    case WM_MOUSEWHEEL:if(g.expanded)Act(GET_WHEEL_DELTA_WPARAM(wp)<0?NextPage:PreviousPage);return 0;
    case WM_CONTEXTMENU:{POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};if(p.x==-1){RECT r;GetWindowRect(hwnd,&r);p={r.left+Px(230),r.top+Px(40)};}Popup(p);return 0;}
    case WM_KEYDOWN:if(wp==VK_ESCAPE){if(g.expanded)Act(Expand);else Act(Hide);}else if(wp==VK_F5)Act(Refresh);else if(wp==VK_DOWN&&!g.expanded)Act(Expand);else if(wp==VK_UP&&g.expanded)Act(Expand);return 0;
    case WM_TIMER:if(wp==SAMPLE_TIMER)SamplePower();return 0;
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
    case WM_DPICHANGED:{g.scale=HIWORD(wp)/96.f*WIDGET_SCALE;auto* r=reinterpret_cast<RECT*>(lp);POINT position{r->left,r->top};if(g.desktop)MapWindowPoints(nullptr,g.desktopHost,&position,1);SetWindowPos(hwnd,nullptr,position.x,position.y,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);ResizeWidget();return 0;}
    case WM_DISPLAYCHANGE:ResizeWidget();return 0;
    case WM_SIZE:g.buffer.reset();return 0;
    case WM_EXITSIZEMOVE:ResizeWidget();SavePrefs();return 0;
    case WM_SYSCOMMAND:if((wp&0xfff0)==SC_MINIMIZE){Act(Hide);return 0;}if((wp&0xfff0)==SC_MAXIMIZE)return 0;break;
    case WM_CLOSE:Act(Hide);return 0;
    case WM_QUERYENDSESSION:SaveHistory();SavePrefs();return TRUE;
    case WM_ENDSESSION:if(wp){SaveHistory();SavePrefs();}return 0;
    case WM_DESTROY:
        SaveHistory();SavePrefs();for(auto h:g.notifications)UnregisterPowerSettingNotification(h);g.notifications.clear();
        WTSUnRegisterSessionNotification(hwnd);if(g.timer)KillTimer(hwnd,SAMPLE_TIMER);
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
    HDC dc=GetDC(nullptr);g.scale=GetDeviceCaps(dc,LOGPIXELSX)/96.f*WIDGET_SCALE;ReleaseDC(nullptr,dc);
    POINT point{g.prefs.x==INT32_MIN?0:g.prefs.x,g.prefs.y==INT32_MIN?0:g.prefs.y};
    HMONITOR monitor=MonitorFromPoint(point,MONITOR_DEFAULTTONEAREST);MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(monitor,&mi);
    int w=Px(CW),h=Px(float(WidgetHeight()));
    int x=g.prefs.x==INT32_MIN?mi.rcWork.right-w-Px(24):g.prefs.x,y=g.prefs.y==INT32_MIN?mi.rcWork.bottom-h-Px(24):g.prefs.y;
    x=std::clamp(x,int(mi.rcWork.left),int(std::max(mi.rcWork.left,mi.rcWork.right-w)));y=std::clamp(y,int(mi.rcWork.top),int(std::max(mi.rcWork.top,mi.rcWork.bottom-h)));
    g.window=CreateWindowExW(WS_EX_TOOLWINDOW,WINDOW_CLASS,L"Cell battery",WS_POPUP|WS_THICKFRAME|WS_SYSMENU|WS_MINIMIZEBOX|WS_CLIPCHILDREN,x,y,w,h,nullptr,nullptr,wc.hInstance,nullptr);
    if(!g.window)return false;
    g.scale=GetDpiForWindow(g.window)/96.f*WIDGET_SCALE;
    DWORD corner=2;DwmSetWindowAttribute(g.window,33,&corner,sizeof(corner));BOOL dark=TRUE;DwmSetWindowAttribute(g.window,20,&dark,sizeof(dark));
    COLORREF border=RGB(43,53,53);DwmSetWindowAttribute(g.window,34,&border,sizeof(border));
    CreateControls();ResizeWidget();
    if(visible){AddTray();RegisterNotifications();RetryDesktopPin();SamplePower(true);if(!g.hidden)ShowWindow(g.window,SW_SHOWNOACTIVATE);SetTimerState();}
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
        wstring previousData=data;Prefs previousPrefs=g.prefs;bool previousTop=g.top,previousExpanded=g.expanded,previousDesktop=g.desktopWanted;
        data=folder;LegacyPrefs legacy;legacy.x=123;legacy.y=234;legacy.top=1;legacy.expanded=1;
        bool saved=AtomicWrite(data+L"\\preferences.bin",&legacy,sizeof(legacy),nullptr,0);LoadPrefs();
        check(saved&&g.prefs.x==123&&g.prefs.y==234&&g.top&&g.expanded&&!g.desktopWanted,"Existing preferences migrate without losing position or display choices");
        Prefs current;current.x=321;current.y=432;current.desktop=1;
        saved=AtomicWrite(data+L"\\preferences.bin",&current,sizeof(current),nullptr,0);LoadPrefs();
        check(saved&&g.prefs.x==321&&g.prefs.y==432&&g.desktopWanted&&!g.top&&!g.expanded,"Desktop pin preference survives a save and reload");
        data=previousData;g.prefs=previousPrefs;g.top=previousTop;g.expanded=previousExpanded;g.desktopWanted=previousDesktop;
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
        RECT r;GetClientRect(g.window,&r);check(r.right==Px(CW)&&r.bottom==Px(HEIGHT),"Native compact client has fixed dimensions");
        int originalWidth=r.right;
        g.expanded=true;ResizeWidget();GetClientRect(g.window,&r);check(r.right==originalWidth&&r.bottom==Px(EXPANDED_HEIGHT),"Expansion keeps the compact width and extends downward");
        check(GetDlgItem(g.window,Expand)&&GetDlgItem(g.window,Refresh)&&GetDlgItem(g.window,FilterBattery)&&GetDlgItem(g.window,DesktopPin),"Keyboard-accessible native button controls include desktop pin");
        check(Px(CW)==int(std::lround(CW*GetDpiForWindow(g.window)/96.f*0.8f)),"Compact widget is twenty percent smaller at the current display DPI");
        bool controlsFit=true;
        for(auto& c:controls)if(c.x<0||c.y<0||c.x+c.w>CW||c.y+c.h>EXPANDED_HEIGHT||(c.details&&c.y<HEIGHT))controlsFit=false;
        check(controlsFit,"Every detail control fits below the compact panel");
        g.expanded=false;ResizeWidget();
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
            Power saved=g.power;g.power.watts=8.25;int width=Px(CW),height=Px(HEIGHT);uint64_t renderTime=Now();
            Bitmap partial(width,height,PixelFormat32bppPARGB),reference(width,height,PixelFormat32bppPARGB);
            {Graphics gr(&partial);Paint(gr,width,height,renderTime);}
            g.power.watts=12.34;RECT dirty{Px(196),Px(118),Px(255),Px(141)};
            {Graphics gr(&partial);gr.SetClip(Rect(dirty.left,dirty.top,dirty.right-dirty.left,dirty.bottom-dirty.top));Paint(gr,width,height,renderTime,&dirty);}
            {Graphics gr(&reference);Paint(gr,width,height,renderTime);}
            BitmapData a{},b{};Rect all(0,0,width,height);bool equal=false;
            if(partial.LockBits(&all,ImageLockModeRead,PixelFormat32bppPARGB,&a)==Ok){
                if(reference.LockBits(&all,ImageLockModeRead,PixelFormat32bppPARGB,&b)==Ok){equal=true;for(int y=0;y<height;y++)if(memcmp(static_cast<BYTE*>(a.Scan0)+y*a.Stride,static_cast<BYTE*>(b.Scan0)+y*b.Stride,width*4)!=0){equal=false;break;}reference.UnlockBits(&b);}partial.UnlockBits(&a);
            }
            check(equal,"Partial wattage redraw exactly matches a full render");g.power=saved;
        }
        g.hidden=false;g.displayOn=true;g.locked=false;g.suspended=false;SetTimerState();
        check(g.timer&&g.timerInterval==LIVE_INTERVAL_MS,"Visible widget uses the live two-second coalesced timer");
        uint64_t beforeReads=g.powerReads,deadline=GetTickCount64()+5200;
        while(GetTickCount64()<deadline){MSG message;while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}MsgWaitForMultipleObjects(0,nullptr,FALSE,50,QS_ALLINPUT);}
        check(g.powerReads>=beforeReads+2,"Actual Windows timer delivers repeated live power samples");
        g.hidden=true;SetTimerState();check(g.timer&&g.timerInterval==HIDDEN_INTERVAL_MS,"Hidden widget reduces sampling to five minutes");g.hidden=false;
        g.expanded=false;g.displayOn=false;SetTimerState();check(!g.timer,"No sampling timer while display is off");
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
            int savedFilter=g.filter;bool savedExpanded=g.expanded;g.filter=3;g.expanded=true;
            bool first=SampleLive();Sleep(300);for(volatile int spin=0;spin<20000000;spin=spin+1){}bool second=SampleLive();
            bool self=false;for(auto& app:g.live)if(app.cpu>0)self=true;
            check(first&&second&&g.liveSnaps.size()==2&&self&&g.liveBusy>0,"Live sampler measures per-app CPU time from one system snapshot");
            StopLive();check(g.liveSnaps.empty()&&g.liveBuffer.capacity()==0,"Leaving the live tab frees its buffers");
            g.filter=savedFilter;g.expanded=savedExpanded;
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
    for(int i=0;i<=288;i++){uint64_t utc=now-DAY+i*300;int percent;uint8_t source;
        if(i<96){percent=90-i/3;source=Battery;}else if(i<138){percent=58+(i-96)*36/42;source=Charging;}else{percent=94-(i-138)*18/150;source=Battery;}
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
    for(int i=0;i<24;i++){double mj=0;for(auto& e:g.energy)if(e.hour==hour-(23-i)*3600&&e.source==Battery)mj+=e.mj;if(mj>0)AddDrain(hour-(23-i)*3600,mj/3600*(1.3+0.5*((i*7)%5)/4.0),3600);}
    for(auto& e:g.energy){e.parts[PartCpu]=float(e.mj*0.55);e.parts[PartDisplay]=float(e.mj*0.25);e.parts[PartNetwork]=float(e.mj*0.12);e.parts[PartOther]=float(e.mj*0.08);}
    g.appPaths[1]=L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe";
    g.appPaths[4]=L"C:\\Windows\\explorer.exe";g.appPaths[6]=L"C:\\Windows\\System32\\notepad.exe";
    g.energyHeader.updated=now-20*60;g.energyHeader.latest=hour;g.filter=0;g.selected=-1;
    g.appSelected=true;g.appName=DisplayName(names[2]);BuildView(now);
}
int Render(const wstring&folder,bool demo){
    CreateDirectoryW(folder.c_str(),nullptr);g.power=ReadPower();g.facts=ReadBatteryFacts();LoadHistory();LoadDrain();ReadEnergy(data+L"\\energy.bin",g.energy,g.energyHeader);BuildView(Now());if(demo)SeedDemo();
    float oldScale=g.scale;g.scale=2;CLSID cls;if(!PngEncoder(cls))return 1;
    for(int pass=0;pass<(demo?2:3);pass++){
        int expanded=pass>0;
        if(pass==2){ // Live tab with this machine's real processes (never in the published sample renders).
            g.filter=3;g.page=0;SampleLive();Sleep(2000);SampleLive();
            for(int i=0;i<40;i++){g.watts[g.wattsCount%g.watts.size()]=float(std::max(0.0,g.power.watts*(0.8+0.4*((i*37)%11)/10.0)));++g.wattsCount;}
        }
        g.expanded=expanded!=0;int width=Px(CW),height=Px(float(WidgetHeight()));Bitmap image(width,height,PixelFormat32bppARGB);Graphics gr(&image);Paint(gr,width,height,Now());gr.ResetTransform();
        for(auto& c:controls){if(c.details&&!g.expanded)continue;if(c.id==Refresh&&LiveTab())continue;if(c.id==ClearHour&&g.selected<0&&!g.appSelected)continue;if((c.id==PreviousPage||c.id==NextPage)&&ListPages()<=1)continue;
            auto state=gr.Save();gr.TranslateTransform(float(Px(c.x)),float(Px(c.y)));gr.ScaleTransform(g.scale,g.scale);DrawButton(gr,c.id,float(c.w),float(c.h));gr.Restore(state);}
        if(demo){Graphics mark(&image);mark.ScaleTransform(g.scale,g.scale);TextAt(mark,L"SAMPLE",125,21,55,0,MUTED);}
        wstring name=demo?L"sample-":L"live-";name+=pass==2?L"now.png":expanded?L"expanded.png":L"compact.png";
        if(image.Save((folder+L"\\"+name).c_str(),&cls,nullptr)!=Ok)return 1;
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
            EfficiencyMode();InitScheduledPaths();LoadPrefs();LoadHistory();LoadDrain();ReadEnergy(data+L"\\energy.bin",g.energy,g.energyHeader);CheckScheduledCache(Now(),true);BuildView(Now());g.power=ReadPower();
            if(g.readOnly)g.notice=L"Data folder is read-only. History cannot be saved here.";
            if(CreateWidget()){MSG message;while(GetMessageW(&message,nullptr,0,0)>0){
                if(message.message==WM_KEYDOWN&&message.wParam==VK_F5){Act(Refresh);continue;}
                if(message.message==WM_KEYDOWN&&message.wParam==VK_ESCAPE){Act(g.expanded?Expand:Hide);continue;}
                if(!IsDialogMessageW(g.window,&message)){TranslateMessage(&message);DispatchMessageW(&message);}
            }}
            else result=1;
            if(mutex){ReleaseMutex(mutex);CloseHandle(mutex);}
        }
    }
    if(g.controller){DestroyWindow(g.controller);g.controller=nullptr;}g.buffer.reset();for(auto& f:g.fonts)f.reset();if(g.icon)DestroyIcon(g.icon);if(g.trayIcon)DestroyIcon(g.trayIcon);
    g.icons.clear();if(SUCCEEDED(com))CoUninitialize();
    if(output.NotificationUnhook&&hook)output.NotificationUnhook(hook);GdiplusShutdown(token);return result;
}
