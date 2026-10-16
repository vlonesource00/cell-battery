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
constexpr int CW=272, HEIGHT=272, EXPANDED_HEIGHT=544;
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
              Settings, DataFolder, Hide, Quit, BatteryReadings, DesktopPin };
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
#pragma pack(push,1)
struct Sample { uint64_t utc=0; int16_t percent=-1; uint8_t source=SourceUnknown, gap=0; };
struct Energy { uint64_t hour=0,id=0; double mj=0; uint8_t source=SourceUnknown; wchar_t name[64]{}; };
struct EnergyHeader { uint32_t magic=0x31454342,version=1,count=0; uint64_t updated=0,latest=0; };
struct HistoryHeader { uint32_t magic=0x31484342,count=0; };
struct LegacyPrefs { uint32_t magic=0x31504342; int32_t x=INT32_MIN,y=INT32_MIN; uint8_t top=0,expanded=0; };
struct Prefs { uint32_t magic=0x32504342; int32_t x=INT32_MIN,y=INT32_MIN; uint8_t top=0,expanded=0,desktop=0; };
#pragma pack(pop)
struct AppTotal { uint64_t id=0; wstring name; double mj=0; };
struct UsageView {
    std::array<double,24> battery{},plugged{},unknown{};
    std::array<uint64_t,24> hours{};
    std::vector<AppTotal> apps;
    double total=0,unassigned=0;
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
    Prefs prefs;
    std::vector<Sample> history;
    std::vector<Energy> energy;
    EnergyHeader energyHeader;
    UsageView view;
    int filter=0,selected=-1,page=0;
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
    GOLD(255,237,192,121), BLUE(255,144,173,189), TRACK(255,39,51,47);

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
        const int64_t rate=static_cast<int32_t>(b.Rate);
        if(b.Rate!=0x80000000u&&b.Rate!=UINT32_MAX&&rate!=0&&std::abs(rate)<500000) {
            p.watts=p.reportedWatts=std::abs(rate)/1000.0;
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
bool SaveHistory() {
    if(g.readOnly||!g.dirty)return true;
    HistoryHeader h;h.count=static_cast<uint32_t>(g.history.size());
    if(!AtomicWrite(data+L"\\history.bin",&h,sizeof(h),g.history.data(),DWORD(g.history.size()*sizeof(Sample))))return false;
    g.dirty=false;g.lastSave=Now();return true;
}
void RecordPower(uint64_t now,bool force=false) {
    if(g.power.percent<0)return;
    Sample s{now,static_cast<int16_t>(g.power.percent),static_cast<uint8_t>(g.power.source),0};
    bool transition=!g.history.empty()&&g.history.back().source!=s.source;
    if(!g.history.empty()){
        const Sample& last=g.history.back();
        if(now<last.utc)return;
        if(!force&&!transition&&now-last.utc<300)return;
        s.gap=g.historyGap||now-last.utc>360;
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
    if(GetFileSizeEx(f,&size)&&ExactRead(f,&h,sizeof(h))&&h.magic==0x31454342&&h.version==1&&h.count<=MAX_ENERGY&&size.QuadPart==sizeof(h)+h.count*sizeof(Energy)){
        std::vector<Energy> v(h.count);
        if(ExactRead(f,v.data(),DWORD(v.size()*sizeof(Energy)))){
            ok=h.updated<=Now()+300;
            for(auto& e:v)if(e.source>SourceUnknown||!std::isfinite(e.mj)||e.mj<0||e.hour>h.updated+3600||!wmemchr(e.name,0,64)){ok=false;break;}
            if(ok){target.swap(v);header=h;}
        }
    }
    CloseHandle(f);return ok;
}
void BuildView(uint64_t now) {
    UsageView v;uint64_t hour=now/3600*3600;g.viewHour=hour;
    for(int i=0;i<24;i++)v.hours[i]=hour-(23-i)*3600;
    std::map<uint64_t,AppTotal> totals;
    for(const auto& e:g.energy){
        if(e.hour<v.hours[0]||e.hour>hour)continue;
        int i=static_cast<int>((e.hour-v.hours[0])/3600);
        if(e.source==Battery)v.battery[i]+=e.mj;else if(e.source==Plugged)v.plugged[i]+=e.mj;else v.unknown[i]+=e.mj;
        if(g.selected>=0&&i!=g.selected)continue;
        if(g.filter==0&&e.source!=Battery)continue;
        if(g.filter==1&&e.source!=Plugged)continue;
        if(e.id==UNATTRIBUTED_ID){v.unassigned+=e.mj;continue;}
        auto& a=totals[e.id];a.id=e.id;a.name=e.name;a.mj+=e.mj;v.total+=e.mj;
    }
    for(auto& kv:totals)if(kv.second.mj>0)v.apps.push_back(std::move(kv.second));
    std::sort(v.apps.begin(),v.apps.end(),[](const AppTotal&a,const AppTotal&b){return a.mj>b.mj;});
    int maxPage=std::max(0,(static_cast<int>(v.apps.size())-1)/3);g.page=std::min(g.page,maxPage);
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
bool ParseEnergy(const wstring& path,uint64_t now,std::vector<Energy>& output,EnergyHeader& head,wstring& error) {
    CsvReader csv(path);std::vector<wstring> fields;std::vector<wstring> names;
    int app=-1,time=-1,source=-1,total=-1;std::vector<int> components;bool schema=false;
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
            const wchar_t* comp[]={L"cpuenergyconsumption",L"socenergyconsumption",L"gpuenergyconsumption",L"npuenergyconsumption",L"displayenergyconsumption",L"diskenergyconsumption",L"networkenergyconsumption",L"mbbenergyconsumption",L"lossenergyconsumption",L"energyloss",L"otherenergyconsumption",L"cpuenergy",L"socenergy",L"gpuenergy",L"npuenergy",L"displayenergy",L"diskenergy",L"networkenergy",L"mbbenergy",L"lossenergy",L"otherenergy"};
            for(size_t i=0;i<names.size();i++)for(auto n:comp)if(names[i]==n){components.push_back(int(i));break;}
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
        double mj=0;bool numeric=true;
        if(total>=0)numeric=Number(fields[total],mj);else for(int i:components){double value=0;if(!Number(fields[i],value)){numeric=false;break;}mj+=value;}
        if(!numeric||!std::isfinite(mj)||mj>1e18){++invalid;continue;}
        uint8_t src=SourceUnknown;
        if(source>=0){auto n=Normalize(fields[source]);if(n==L"1"||n==L"true"||n==L"yes")src=Battery;else if(n==L"0"||n==L"false"||n==L"no")src=Plugged;}
        bool unassigned=Normalize(identity)==L"unknown"||Normalize(identity)==L"unattributed";
        uint64_t id=unassigned?UNATTRIBUTED_ID:Hash(identity);uint64_t hour=stamp/3600*3600;
        auto key=std::make_tuple(hour,id,src);wstring name=unassigned?L"Windows / unattributed":AppName(identity);
        if(aggregate.size()>=MAX_ENERGY-72&&aggregate.find(key)==aggregate.end()){id=0;name=L"Other apps";key=std::make_tuple(hour,id,src);}
        auto& e=aggregate[key];e.hour=hour;e.id=id;e.source=src;e.mj+=mj;wcsncpy(e.name,name.c_str(),63);e.name[63]=0;
        latest=std::max(latest,stamp);++accepted;
    }
    if(csv.error){error=L"Windows history was unreadable or malformed.";return false;}
    if(!schema){error=L"This Windows history format is not supported.";return false;}
    if(invalid&&accepted==0){error=L"Windows exported records that could not be read.";return false;}
    std::vector<Energy> result;result.reserve(aggregate.size());for(auto& kv:aggregate)result.push_back(kv.second);
    output.swap(result);head=EnergyHeader{};head.count=uint32_t(output.size());head.updated=now;head.latest=latest;
    return true;
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
        std::vector<Energy> records;EnergyHeader h;
        if(ParseEnergy(temp,Now(),records,h,error)){
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
bool CanJoin(const Sample&a,const Sample&b){return !b.gap&&b.utc>a.utc&&b.utc-a.utc<=360&&a.percent>=0&&b.percent>=0;}
Color SourceColor(uint8_t source){return source==Battery?GREEN:source==Charging?GOLD:source==Plugged?BLUE:MUTED;}
void PaintHistory(Graphics& gr,uint64_t now) {
    const float x=20,y=203,w=232,h=29;
    Stroke(gr,x,y+h,x+w,y+h,LINE);
    bool drawn=false;uint64_t start=now-DAY;
    for(size_t i=1;i<g.history.size();i++){
        auto&a=g.history[i-1];auto&b=g.history[i];if(a.utc<start||b.utc>now||!CanJoin(a,b))continue;
        float ax=x+w*float(double(a.utc-start)/DAY),bx=x+w*float(double(b.utc-start)/DAY);
        float ay=y+h-h*a.percent/100.f,by=y+h-h*b.percent/100.f;
        Stroke(gr,ax,ay,bx,by,SourceColor(a.source),1.7f);drawn=true;
    }
    if(!drawn)TextAt(gr,L"Battery history starts here",x,y+4,w,0,MUTED);
    else if(!g.history.empty()){
        auto&s=g.history.back();if(s.utc>=start&&s.utc<=now){SolidBrush b(SourceColor(s.source));gr.FillEllipse(&b,x+w*float(double(s.utc-start)/DAY)-2,y+h-h*s.percent/100.f-2,4.f,4.f);}
    }
}
wstring StatusText(){
    if(!g.power.valid)return L"Power status unavailable";
    if(!g.power.present)return L"No battery detected";
    if(g.power.source==Charging)return L"Charging";
    if(g.power.source==Plugged)return g.power.percent==100?L"Fully charged":L"Plugged in";
    if(g.power.source==Battery)return g.power.saver?L"On battery · saver on":L"On battery";
    return L"Power source unknown";
}
wstring MainEta(){
    if(!g.power.present)return L"Battery unavailable";
    if(g.power.source==Charging)return L"Recharging";
    if(g.power.source==Plugged)return L"Ready when you are";
    if(g.power.source==SourceUnknown)return L"Waiting for Windows";
    return EtaText(g.power.seconds)+(g.power.seconds!=UINT32_MAX&&g.power.seconds>0?L" left":L"");
}
wstring EtaLabel(){
    if(!g.power.present)return L"A system battery is required";
    if(g.power.source==Charging)return L"External power connected";
    if(g.power.source==Plugged)return L"Battery is not discharging";
    if(g.power.source==SourceUnknown)return L"Windows has not reported the source";
    if(g.power.windowsEta)return g.power.recentDrain?L"Windows ETA · recent-average draw":L"Windows remaining-time estimate";
    if(g.power.recentDrain)return L"Recent drain (1–2 min) · estimated ETA";
    if(g.power.seconds!=UINT32_MAX)return L"Reported-rate estimate · warming up";
    return L"Learning recent battery drain…";
}
void PaintUsage(Graphics& gr,uint64_t now){
    auto state=gr.Save();gr.TranslateTransform(0,float(HEIGHT));
    SolidBrush pb(PANEL);gr.FillRectangle(&pb,0.f,0.f,float(CW),float(HEIGHT));Stroke(gr,20,0,252,0,LINE);
    TextAt(gr,L"Energy by app",20,18,190,3,FG);
    TextAt(gr,g.selected<0?L"LAST 24 CLOCK HOURS":L"RECORDED AT "+Clock(g.view.hours[g.selected]),20,79,200,0,MUTED);
    const float x=20,y=101,w=232,h=29,step=w/24;
    double peak=0;for(int i=0;i<24;i++)peak=std::max(peak,g.view.battery[i]+g.view.plugged[i]+g.view.unknown[i]);
    for(int i=0;i<24;i++){
        float left=x+i*step;
        if(g.selected==i)Rounded(gr,RectF(left-1,y-3,step,35),3,Tint(GREEN,22));
        if(peak<=0){Rounded(gr,RectF(left,y+23,step-3,3),1.5f,LINE);continue;}
        float yy=y+h;
        const double values[]={g.view.battery[i],g.view.plugged[i],g.view.unknown[i]};
        const Color colors[]={GREEN,GOLD,BLUE};
        for(int k=0;k<3;k++){float bh=float(values[k]/peak*h);if(bh>0){yy-=bh;SolidBrush b(Tint(colors[k],g.selected>=0&&g.selected!=i?65:200));gr.FillRectangle(&b,left,yy,step-3,std::max(1.f,bh));}}
    }
    TextAt(gr,Clock(g.view.hours[0]),x,y+h+6,60,0,MUTED);
    TextAt(gr,L"now",x+w-45,y+h+6,45,0,MUTED,true);
    TextAt(gr,L"Share of reported app energy",20,154,232,0,MUTED);
    if(g.view.apps.empty()){
        wstring message=g.energyHeader.updated?L"No app estimates for this selection":L"Load Windows app history";
        TextAt(gr,message,20,181,232,2,FG);
        TextAt(gr,g.refreshing?L"Windows is preparing the export…":!g.notice.empty()?g.notice:L"Refresh ↻ loads Windows' saved data.",20,204,232,0,MUTED);
        if(!g.energyHeader.updated)TextAt(gr,L"Refresh on demand · no process scanning",20,225,232,0,MUTED);
    }else{
        for(int row=0;row<3;row++){
            size_t idx=size_t(g.page*3+row);if(idx>=g.view.apps.size())break;
            auto&a=g.view.apps[idx];float yy=173+row*25.f;
            Color accent=g.filter==1?GOLD:GREEN;
            Rounded(gr,RectF(20,yy,19,19),5,Tint(accent,20));
            wstring letter=a.name.empty()?L"·":wstring(1,wchar_t(towupper(a.name[0])));
            TextAt(gr,letter,25,yy+2,13,0,accent);
            TextAt(gr,a.name,47,yy+1,159,1,FG);
            wchar_t b[24];double share=g.view.total>0?100*a.mj/g.view.total:0;swprintf(b,24,L"%.0f%%",share);
            TextAt(gr,b,207,yy+1,45,1,accent,true);
            Stroke(gr,47,yy+19,252,yy+19,LINE,1);
            Stroke(gr,47,yy+19,47+float(205*share/100),yy+19,accent,1.5f);
        }
    }
    wstring freshness;
    if(g.refreshing)freshness=L"Refreshing Windows history…";
    else if(!g.notice.empty())freshness=L"Refresh issue · see menu";
    else if(g.energyHeader.updated&&g.view.unassigned>0){
        wchar_t b[100];double share=100*g.view.unassigned/(g.view.total+g.view.unassigned);
        swprintf(b,100,L"Unassigned %.0f%% · cache %ls",share,Clock(g.energyHeader.updated).c_str());freshness=b;
    }else if(g.energyHeader.updated){
        auto age=now>=g.energyHeader.updated?(now-g.energyHeader.updated)/60:0;
        if(age<1)freshness=L"Cache updated just now";
        else if(age<60)freshness=L"Cache updated "+std::to_wstring(age)+L"m ago";
        else freshness=L"Cache updated "+std::to_wstring(age/60)+L"h ago";
    }else freshness=L"Windows estimates · refresh on demand";
    TextAt(gr,freshness,20,252,175,0,MUTED);gr.Restore(state);
}
void Paint(Graphics& gr,int width,int height,uint64_t now,const RECT* dirty=nullptr){
    gr.SetSmoothingMode(SmoothingModeAntiAlias);gr.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    auto touches=[&](float x,float y,float w,float h){return !dirty||(dirty->right>Px(x)&&dirty->left<Px(x+w)&&dirty->bottom>Px(y)&&dirty->top<Px(y+h));};
    SolidBrush background(BG);gr.FillRectangle(&background,0,0,width,height);gr.ScaleTransform(g.scale,g.scale);
    if(touches(0,0,272,42)){
        SolidBrush green(GREEN);gr.FillEllipse(&green,20.f,23.f,6.f,6.f);TextAt(gr,L"cell",33,16,160,3,FG);
    }
    if(touches(0,42,272,70)){
        TextAt(gr,g.power.percent<0?L"—":std::to_wstring(g.power.percent),17,43,180,5,FG);
        RectF percentBounds;wstring percentage=g.power.percent<0?L"—":std::to_wstring(g.power.percent);
        gr.MeasureString(percentage.c_str(),int(percentage.size()),g.fonts[5].get(),PointF(0,0),StringFormat::GenericTypographic(),&percentBounds);
        TextAt(gr,L"%",21+percentBounds.Width,80,36,4,MUTED);
        const float bx=214,by=78;Rounded(gr,RectF(bx,by,33,17),4,LINE);Rounded(gr,RectF(bx+33,by+5,3,7),1.5f,LINE);
        if(g.power.percent>=0)Rounded(gr,RectF(bx+3,by+3,std::max(1.f,27.f*g.power.percent/100),11),2,SourceColor(g.power.source));
        if(g.power.source==Charging){PointF bolt[]={{229,77},{224,86},{229,86},{226,95},{235,83},{230,83}};SolidBrush brush(BG);gr.FillPolygon(&brush,bolt,6);}
    }
    if(touches(20,118,172,23))TextAt(gr,StatusText(),20,121,172,1,SourceColor(g.power.source));
    if(touches(196,118,59,23)&&g.power.watts>0){wchar_t b[30];swprintf(b,30,L"%.2f W",g.power.watts);TextAt(gr,b,197,121,55,1,MUTED,true);}
    if(touches(18,146,239,30))TextAt(gr,MainEta(),20,149,235,4,FG);
    if(touches(18,177,241,18))TextAt(gr,EtaLabel(),20,179,238,0,MUTED);
    if(touches(18,200,237,38))PaintHistory(gr,now);
    if(touches(0,240,272,32)){
        TextAt(gr,L"24h",20,248,26,0,MUTED);
        SolidBrush b(GREEN);gr.FillEllipse(&b,52.f,253.f,4.f,4.f);TextAt(gr,L"Battery",61,248,47,0,MUTED);
        SolidBrush c(GOLD);gr.FillEllipse(&c,115.f,253.f,4.f,4.f);TextAt(gr,L"Charging",124,248,57,0,MUTED);
    }
    if(g.expanded&&touches(0,HEIGHT,CW,HEIGHT))PaintUsage(gr,now);
}
wstring ButtonCaption(int id){
    switch(id){case Expand:return g.expanded?L"Collapse app usage":L"Expand app usage";case Menu:return L"Menu";case Refresh:return g.hourlyAvailable?L"Refresh Windows app history":L"Refresh Windows app history (administrator consent)";
    case FilterBattery:return L"On battery";case FilterPlugged:return L"Plugged in";case FilterAll:return L"All";case ClearHour:return L"Clear selected hour";
    case PreviousPage:return L"Previous apps";case NextPage:return L"Next apps";case DesktopPin:return g.desktopWanted?L"Unpin from desktop":L"Pin to desktop (behind app windows)";}return L"";
}
bool IsSelected(int id){return (id==DesktopPin&&g.desktop)||(id==FilterBattery&&g.filter==0)||(id==FilterPlugged&&g.filter==1)||(id==FilterAll&&g.filter==2);}
void DrawButton(Graphics& gr,int id,float w,float h,bool pressed=false,bool focus=false){
    gr.SetSmoothingMode(SmoothingModeAntiAlias);gr.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    Color parent=id==Expand||id==Menu||id==DesktopPin?BG:PANEL;
    SolidBrush parentBrush(parent);gr.FillRectangle(&parentBrush,0.f,0.f,w,h);Color fg=MUTED;
    bool selected=IsSelected(id);
    if(id==Expand){Rounded(gr,RectF(0,0,w,h),7,TRACK);fg=GREEN;}
    if(selected){Rounded(gr,RectF(0,0,w,h),6,Tint(id==FilterPlugged?GOLD:GREEN,20));fg=id==FilterPlugged?GOLD:GREEN;}
    if(pressed)Rounded(gr,RectF(0,0,w,h),6,Tint(FG,24));
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
    {DesktopPin,200,12,28,28,false},{Menu,232,12,28,28,false},{Expand,230,240,30,26,false},{Refresh,224,HEIGHT+12,28,28,true},
    {FilterBattery,20,HEIGHT+44,88,25,true},{FilterPlugged,114,HEIGHT+44,88,25,true},{FilterAll,208,HEIGHT+44,44,25,true},
    {ClearHour,224,HEIGHT+73,28,22,true},{PreviousPage,202,HEIGHT+245,24,23,true},{NextPage,232,HEIGHT+245,24,23,true}
};
void LayoutButtons(){
    for(auto& c:controls){HWND b=GetDlgItem(g.window,c.id);if(!b)continue;
        MoveWindow(b,Px(c.x),Px(c.y),Px(c.w),Px(c.h),TRUE);
        bool visible=!c.details||g.expanded;
        if(c.id==ClearHour&&g.selected<0)visible=false;
        if(c.id==PreviousPage||c.id==NextPage){if(g.view.apps.size()<=3)visible=false;EnableWindow(b,c.id==PreviousPage?g.page>0:(g.page+1)*3<int(g.view.apps.size()));}
        if(c.id==Refresh)EnableWindow(b,!g.refreshing);
        if(c.id==Refresh)SetWindowTextW(b,ButtonCaption(c.id).c_str());
        if(c.id==Expand)SetWindowTextW(b,ButtonCaption(c.id).c_str());
        if(c.id==DesktopPin)SetWindowTextW(b,ButtonCaption(c.id).c_str());
        ShowWindow(b,visible?SW_SHOWNOACTIVATE:SW_HIDE);
    }
}
void Redraw(){if(g.window&&!g.hidden&&g.displayOn&&!g.locked&&!g.suspended){InvalidateRect(g.window,nullptr,FALSE);for(HWND b:g.buttons)InvalidateRect(b,nullptr,FALSE);}}
void UpdateTray(){
    if(!g.tray.hWnd)return;wstring s=L"Cell · "+(g.power.percent>=0?std::to_wstring(g.power.percent)+L"% · ":L"")+StatusText();
    if(g.power.source==Battery)s+=L" · "+MainEta();if(s==g.tray.szTip)return;wcsncpy(g.tray.szTip,s.c_str(),127);g.tray.szTip[127]=0;g.tray.uFlags=NIF_TIP;Shell_NotifyIconW(NIM_MODIFY,&g.tray);
    wstring title=L"Cell · "+StatusText()+L" · "+MainEta();SetWindowTextW(g.window,title.c_str());
}
void SamplePower(bool force=false){
    ++g.powerReads;
    Power previous=g.power;uint64_t lastHistory=g.history.empty()?0:g.history.back().utc,previousSample=g.lastSample;
    g.power=ReadPower();auto now=Now();RecordPower(now,force);g.lastSample=now;
    bool hourChanged=g.viewHour!=now/3600*3600,cacheChanged=CheckScheduledCache(now,force);
    if(hourChanged)g.selected=-1;
    if(hourChanged||cacheChanged){BuildView(now);LayoutButtons();}
    auto& p=g.power;
    bool structural=force||hourChanged||cacheChanged||previous.percent!=p.percent||previous.source!=p.source||previous.saver!=p.saver||previous.valid!=p.valid||previous.present!=p.present||previous.windowsEta!=p.windowsEta||previous.recentDrain!=p.recentDrain;
    UpdateTray();
    if(structural){Redraw();return;}
    if(g.hidden||!g.displayOn||g.locked||g.suspended)return;
    auto invalidate=[](int x,int y,int width,int height){RECT rect{Px(float(x)),Px(float(y)),Px(float(x+width)),Px(float(y+height))};InvalidateRect(g.window,&rect,FALSE);};
    if(std::lround(previous.watts*100)!=std::lround(p.watts*100))invalidate(196,118,59,23);
    if(EtaDisplayMinute(previous.seconds)!=EtaDisplayMinute(p.seconds))invalidate(18,146,239,30);
    if(!g.history.empty()&&lastHistory!=g.history.back().utc)invalidate(18,200,237,38);
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
        CloseHandle(process.hThread);auto* wait=new RefreshWait{process.hProcess,g.window,true};HANDLE thread=CreateThread(nullptr,0,AwaitRefresh,wait,0,nullptr);
        if(!thread){CloseHandle(process.hProcess);delete wait;}else CloseHandle(thread);
        g.scheduledPending=true;g.scheduledRequest=GetTickCount64();g.refreshing=true;g.notice.clear();LayoutButtons();Redraw();return;
    }
    if(g.readOnly){MessageBoxW(g.window,L"Move Cell to a writable folder before loading app history.",L"Cell",MB_OK);return;}
    wstring exe=ExePath();SHELLEXECUTEINFOW se{};se.cbSize=sizeof(se);se.fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_FLAG_NO_UI;
    se.hwnd=g.window;se.lpVerb=L"runas";se.lpFile=exe.c_str();se.lpParameters=L"--refresh";se.lpDirectory=root.c_str();se.nShow=SW_HIDE;
    if(!ShellExecuteExW(&se)){g.notice=GetLastError()==ERROR_CANCELLED?L"Refresh cancelled; existing cache kept.":L"Windows could not start the history export.";Redraw();return;}
    auto* wait=new RefreshWait{se.hProcess,g.window};HANDLE thread=CreateThread(nullptr,0,AwaitRefresh,wait,0,nullptr);
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
    case FilterBattery:case FilterPlugged:case FilterAll:g.filter=id==FilterBattery?0:id==FilterPlugged?1:2;g.page=0;BuildView(Now());LayoutButtons();Redraw();break;
    case ClearHour:g.selected=-1;g.page=0;BuildView(Now());LayoutButtons();Redraw();break;
    case PreviousPage:if(g.page>0)--g.page;LayoutButtons();Redraw();break;
    case NextPage:if((g.page+1)*3<int(g.view.apps.size()))++g.page;LayoutButtons();Redraw();break;
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
    g.tray={};g.tray.cbSize=sizeof(g.tray);g.tray.hWnd=g.window;g.tray.uID=1;
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
            else if(id==ClearHour)tip=L"Show all 24 clock hours again. Chart bars use Windows' record timestamps, not exact activity start times.";
            else tip=ButtonCaption(id);t->lpszText=const_cast<wchar_t*>(tip.c_str());return 0;
        }break;
    }
    case WM_COMMAND:if(HIWORD(wp)==BN_CLICKED)Act(LOWORD(wp));return 0;
    case WM_LBUTTONDOWN:
        if(g.desktop&&GET_Y_LPARAM(lp)<Px(40)&&GET_X_LPARAM(lp)<Px(196)){
            g.dragging=true;GetCursorPos(&g.dragStart);GetWindowRect(hwnd,&g.dragRect);SetCapture(hwnd);
        }return 0;
    case WM_MOUSEMOVE:
        if(g.dragging){
            POINT cursor{};GetCursorPos(&cursor);POINT position{g.dragRect.left+cursor.x-g.dragStart.x,g.dragRect.top+cursor.y-g.dragStart.y};
            MapWindowPoints(nullptr,g.desktopHost,&position,1);
            SetWindowPos(hwnd,nullptr,position.x,position.y,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
        }return 0;
    case WM_CAPTURECHANGED:g.dragging=false;return 0;
    case WM_LBUTTONUP:{
        if(g.dragging){g.dragging=false;ReleaseCapture();ResizeWidget();SavePrefs();return 0;}
        float x=GET_X_LPARAM(lp)/g.scale,y=GET_Y_LPARAM(lp)/g.scale;
        if(g.expanded&&x>=20&&x<252&&y>=HEIGHT+97&&y<=HEIGHT+142){g.selected=std::clamp(int((x-20)/(232.f/24)),0,23);g.page=0;BuildView(Now());LayoutButtons();Redraw();}return 0;
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
        if(wp==0){if(!ReadEnergy(data+L"\\energy.bin",g.energy,g.energyHeader))g.notice=L"Windows export finished, but its cache could not be read.";else{g.notice.clear();g.selected=-1;g.page=0;BuildView(Now());}}
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
        RECT r;GetClientRect(g.window,&r);check(r.right==Px(CW)&&r.bottom==Px(HEIGHT),"Native compact client has fixed square dimensions");
        int originalWidth=r.right;
        g.expanded=true;ResizeWidget();GetClientRect(g.window,&r);check(r.right==originalWidth&&r.bottom==Px(EXPANDED_HEIGHT),"Expansion keeps the compact width and extends downward");
        check(GetDlgItem(g.window,Expand)&&GetDlgItem(g.window,Refresh)&&GetDlgItem(g.window,FilterBattery)&&GetDlgItem(g.window,DesktopPin),"Keyboard-accessible native button controls include desktop pin");
        check(Px(CW)==int(std::lround(CW*GetDpiForWindow(g.window)/96.f*0.8f)),"Compact widget is twenty percent smaller at the current display DPI");
        bool controlsFit=true;
        for(auto& c:controls)if(c.x<0||c.y<0||c.x+c.w>CW||c.y+c.h>EXPANDED_HEIGHT||(c.details&&c.y<HEIGHT))controlsFit=false;
        check(controlsFit,"Every detail control fits below the compact square");
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
    report+="\r\n"+std::to_string(passed)+" passed, "+std::to_string(failed)+" failed.\r\n";
    WriteBytes(folder+L"\\test-results.txt",report.data(),DWORD(report.size()));return failed?1:0;
}
void SeedDemo(){
    uint64_t now=Now(),hour=now/3600*3600;
    g.power={true,true,false,76,Battery,4*3600+38*60,false,9.8};g.history.clear();
    g.power.reportedWatts=9.8;g.power.recentDrain=true;g.power.drainSpan=120;
    for(int i=0;i<=288;i++){uint64_t utc=now-DAY+i*300;int percent;uint8_t source;
        if(i<96){percent=90-i/3;source=Battery;}else if(i<138){percent=58+(i-96)*36/42;source=Charging;}else{percent=94-(i-138)*18/150;source=Battery;}
        g.history.push_back(Sample{utc,int16_t(percent),source,0});
    }
    g.energy.clear();const wchar_t* names[]={L"Microsoft Edge",L"Visual Studio Code",L"Spotify",L"System",L"Other apps"};
    for(int i=0;i<24;i++)for(int app=0;app<5;app++){
        Energy e;e.hour=hour-(23-i)*3600;e.id=app+1;e.source=(i>=8&&i<=11)?Plugged:Battery;e.mj=(app==0?5:app==1?3:app==2?1.6:app==3?1:0.5)*(30+(i*17+app*9)%70)*3600;wcscpy(e.name,names[app]);g.energy.push_back(e);
    }
    g.energyHeader.updated=now-20*60;g.energyHeader.latest=hour;g.filter=0;g.selected=-1;BuildView(now);
}
int Render(const wstring&folder,bool demo){
    CreateDirectoryW(folder.c_str(),nullptr);g.power=ReadPower();LoadHistory();ReadEnergy(data+L"\\energy.bin",g.energy,g.energyHeader);BuildView(Now());if(demo)SeedDemo();
    float oldScale=g.scale;g.scale=2;CLSID cls;if(!PngEncoder(cls))return 1;
    for(int expanded=0;expanded<2;expanded++){
        g.expanded=expanded!=0;int width=Px(CW),height=Px(float(WidgetHeight()));Bitmap image(width,height,PixelFormat32bppARGB);Graphics gr(&image);Paint(gr,width,height,Now());gr.ResetTransform();
        for(auto& c:controls){if(c.details&&!g.expanded)continue;if(c.id==ClearHour&&g.selected<0)continue;if((c.id==PreviousPage||c.id==NextPage)&&g.view.apps.size()<=3)continue;
            auto state=gr.Save();gr.TranslateTransform(float(Px(c.x)),float(Px(c.y)));gr.ScaleTransform(g.scale,g.scale);DrawButton(gr,c.id,float(c.w),float(c.h));gr.Restore(state);}
        if(demo){Graphics mark(&image);mark.ScaleTransform(g.scale,g.scale);TextAt(mark,L"SAMPLE",125,21,55,0,MUTED);}
        wstring name=demo?L"sample-":L"live-";name+=expanded?L"expanded.png":L"compact.png";
        if(image.Save((folder+L"\\"+name).c_str(),&cls,nullptr)!=Ok)return 1;
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
        std::vector<Energy> records;EnergyHeader h;wstring error;
        if(!ParseEnergy(folder,Now(),records,h,error)){WriteStatus(error);return 1;}
        bool saved=AtomicWrite(data+L"\\energy.bin",&h,sizeof(h),records.data(),DWORD(records.size()*sizeof(Energy)));
        WriteStatus(saved?L"":L"Cannot write the app history cache.");return saved?0:1;
    }
    GdiplusStartupInput input;input.SuppressBackgroundThread=TRUE;GdiplusStartupOutput output;ULONG_PTR token=0,hook=0;
    if(GdiplusStartup(&token,&input,&output)!=Ok)return 1;
    if(output.NotificationHook)output.NotificationHook(&hook);Fonts();int result=0;
    if(mode==L"--self-test")result=SelfTest(folder);
    else if(mode==L"--desktop-check")result=DesktopCheck(folder);
    else if(mode==L"--render"||mode==L"--render-demo")result=Render(folder,mode==L"--render-demo");
    else{
        HANDLE mutex=CreateMutexW(nullptr,TRUE,L"Local\\CellBatteryWidget1");
        if(mutex&&GetLastError()==ERROR_ALREADY_EXISTS){HWND existing=FindRunningWidget();if(existing)PostMessageW(existing,WM_COMMAND,Show,0);CloseHandle(mutex);}
        else{
            EfficiencyMode();InitScheduledPaths();LoadPrefs();LoadHistory();ReadEnergy(data+L"\\energy.bin",g.energy,g.energyHeader);CheckScheduledCache(Now(),true);BuildView(Now());g.power=ReadPower();
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
    if(g.controller){DestroyWindow(g.controller);g.controller=nullptr;}g.buffer.reset();for(auto& f:g.fonts)f.reset();if(g.icon)DestroyIcon(g.icon);
    if(output.NotificationUnhook&&hook)output.NotificationUnhook(hook);GdiplusShutdown(token);return result;
}
