#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "plugin_host.h"
#include "plugin_manifest.h"
#include <memory>
#include <cstdio>
#include <cstring>

/* Implemented beside the other guest Lua adapters in recomp_manual.c. */
extern "C" int recomp_plugin_run_lua(const char *source,size_t source_length,
    char *out,size_t capacity,size_t *out_length);
/* d3d8_internal.h; composites premultiplied BGRA into the next presented frame. */
extern "C" BOOL d3d8_CompositeHostOverlay(const void *pixels,UINT width,UINT height,
    UINT x,UINT y,UINT logical_width,UINT logical_height);

namespace {
using namespace mercplugins;
struct Loaded {
    Manifest manifest;
    HMODULE module=nullptr;
    MercPluginHost2 host{};
    MercPlugin callbacks{};
    std::string directory,settings;
    bool entered=false,active=false;
    MercPluginCallback paint=nullptr;
    void *paint_userdata=nullptr;
};
struct Hotkey {
    Loaded *plugin;
    MercPluginCallback callback;
    void *userdata;
};
std::map<uint32_t,Hotkey> hotkeys;
constexpr uint32_t OVERLAY_WIDTH=960,OVERLAY_HEIGHT=540;
std::vector<std::unique_ptr<Loaded>> loaded;
DWORD owner=0;
void *guest=nullptr;
size_t guest_size=0;
Loaded *calling=nullptr;
bool dispatching=false,initialized=false,painting=false,ui_callback=false;
void *host_window=nullptr;
ULONGLONG previous=0;
uint64_t sequence=0;
std::string failure;
std::ofstream log_file;

std::wstring environment(const wchar_t *key) {
    DWORD n=GetEnvironmentVariableW(key,nullptr,0);
    if(!n)return {};
    std::wstring s(n,0);
    if(GetEnvironmentVariableW(key,s.data(),n)>=n)throw std::runtime_error("Plugin environment changed during startup.");
    s.resize(n-1);return s;
}
void record(const std::string &s) {
    if(log_file){log_file<<s<<"\n";log_file.flush();}
    fprintf(stderr,"[plugins] %s\n",s.c_str());
}
void MERC_PLUGIN_CALL plugin_log(void *ctx,const char *message) {
    if(GetCurrentThreadId()!=owner||!calling||ctx!=calling||!message)return;
    record(calling->manifest.id+": "+std::string(message));
}
bool accessible(void *address,size_t bytes,bool write) {
    auto p=reinterpret_cast<uintptr_t>(address),end=p+bytes;
    if(end<p)return false;
    while(p<end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if(!VirtualQuery(reinterpret_cast<void*>(p),&mbi,sizeof(mbi))||mbi.State!=MEM_COMMIT||
           (mbi.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
        DWORD protection=mbi.Protect&0xff;
        bool readable=protection==PAGE_READONLY||protection==PAGE_READWRITE||protection==PAGE_WRITECOPY||
                      protection==PAGE_EXECUTE_READ||protection==PAGE_EXECUTE_READWRITE||protection==PAGE_EXECUTE_WRITECOPY;
        bool writable=protection==PAGE_READWRITE||protection==PAGE_WRITECOPY||
                      protection==PAGE_EXECUTE_READWRITE||protection==PAGE_EXECUTE_WRITECOPY;
        if(!readable||(write&&!writable))return false;
        auto next=reinterpret_cast<uintptr_t>(mbi.BaseAddress)+mbi.RegionSize;
        if(next<=p)return false;p=next;
    }return true;
}
bool span(void *ctx,uint32_t address,size_t bytes,bool write) {
    return GetCurrentThreadId()==owner&&calling&&ctx==calling&&!painting&&!ui_callback&&guest&&bytes&&
        address>=0x10000u&&address<guest_size&&bytes<=guest_size-address&&
        accessible(static_cast<unsigned char*>(guest)+address,bytes,write);
}
int MERC_PLUGIN_CALL read_guest(void *ctx,uint32_t address,void *out,size_t bytes) {
    if(!out||!span(ctx,address,bytes,false)||!accessible(out,bytes,true))return 0;
    memmove(out,static_cast<unsigned char*>(guest)+address,bytes);return 1;
}
int MERC_PLUGIN_CALL write_guest(void *ctx,uint32_t address,const void *data,size_t bytes) {
    if(!data||!span(ctx,address,bytes,true)||!accessible(const_cast<void*>(data),bytes,false))return 0;
    memmove(static_cast<unsigned char*>(guest)+address,data,bytes);return 1;
}
int MERC_PLUGIN_CALL run_lua(void *ctx,const char *source,size_t length,char *out,size_t capacity,size_t *out_length) {
    if(out_length)*out_length=0;
    if(out&&capacity)out[0]=0;
    // Only the plugin currently inside on_update may run script; init/shutdown are too early/late.
    if(GetCurrentThreadId()!=owner||!calling||ctx!=calling||!dispatching||
       calling->manifest.api_version<2||!source||(capacity&&!out))return MERC_LUA_REJECTED;
    if(length>0x100000u)return MERC_LUA_NO_MEMORY;
    if(capacity&&!accessible(out,capacity,true))return MERC_LUA_REJECTED;
    return recomp_plugin_run_lua(source,length,out,capacity,out_length);
}
bool ui_call_allowed(void *ctx) {
    return GetCurrentThreadId()==owner&&calling&&ctx==calling&&!painting&&!ui_callback&&
        calling->manifest.api_version>=2;
}
int MERC_PLUGIN_CALL register_hotkey(void *ctx,uint32_t key,MercPluginCallback callback,void *userdata) {
    if(!ui_call_allowed(ctx)||!callback||!key||key>0xFE)return 0;
    if(key>=VK_F8&&key<=VK_F11)return 0; // recomp diagnostics and controls
    auto found=hotkeys.find(key);
    if(found!=hotkeys.end()&&found->second.plugin!=calling)return 0;
    hotkeys[key]=Hotkey{calling,callback,userdata};
    record(calling->manifest.id+": registered hotkey 0x"+[&]{char b[8];snprintf(b,sizeof(b),"%02X",key);return std::string(b);}());
    return 1;
}
int MERC_PLUGIN_CALL set_overlay(void *ctx,MercPluginCallback paint,void *userdata) {
    if(!ui_call_allowed(ctx))return 0;
    calling->paint=paint;calling->paint_userdata=paint?userdata:nullptr;
    return 1;
}
int MERC_PLUGIN_CALL draw_overlay(void *ctx,const void *pixels,uint32_t width,uint32_t height,uint32_t x,uint32_t y) {
    if(GetCurrentThreadId()!=owner||!painting||!calling||ctx!=calling||!pixels||!width||!height||
       width>OVERLAY_WIDTH||height>OVERLAY_HEIGHT||x>OVERLAY_WIDTH-width||y>OVERLAY_HEIGHT-height||
       !accessible(const_cast<void*>(pixels),size_t(width)*height*4u,false))return 0;
    return d3d8_CompositeHostOverlay(pixels,width,height,x,y,OVERLAY_WIDTH,OVERLAY_HEIGHT)?1:0;
}
void disable_after_exception(Loaded &p,const char *where) {
    p.active=false;p.paint=nullptr;
    for(auto it=hotkeys.begin();it!=hotkeys.end();)it=it->second.plugin==&p?hotkeys.erase(it):std::next(it);
    record(p.manifest.name+": "+where+" threw an exception; plugin disabled.");
}
void shutdown_all() {
    for(auto it=loaded.rbegin();it!=loaded.rend();++it) {
        auto &p=**it;
        if(p.entered&&p.callbacks.on_shutdown) {
            calling=&p;
            try {p.callbacks.on_shutdown(p.callbacks.userdata);}
            catch(...) {record(p.manifest.id+": shutdown threw an exception.");}
            calling=nullptr;
        }
        if(p.module)FreeLibrary(p.module);
    }
    hotkeys.clear();
    loaded.clear();guest=nullptr;guest_size=0;initialized=false;dispatching=false;painting=false;host_window=nullptr;
    if(log_file)log_file.close();
}
}
extern "C" const char *merc_plugins_error(void) {return failure.c_str();}
extern "C" int merc_plugins_initialize(void *base,size_t bytes,void *window) {
    if(initialized){failure="Plugin host was initialized twice.";return 0;}
    owner=GetCurrentThreadId();guest=base;guest_size=std::min<size_t>(bytes,0x4000000u);host_window=window;
    sequence=0;failure.clear();
    try {
        auto path=environment(L"MERCENARIES_PLUGIN_LIST");
        if(path.empty()){initialized=true;return 1;}
        auto settings=environment(L"MERCENARIES_PLUGIN_SETTINGS");
        if(settings.empty()||!fs::path(settings).is_absolute())throw std::runtime_error("Missing absolute plugin settings path.");
        fs::path list_path(path);
        if(!list_path.is_absolute()||fs::file_size(list_path)>65536)throw std::runtime_error("Invalid plugin selection file.");
        fs::create_directories(settings);
        log_file.open(fs::path(settings)/L"plugins.log",std::ios::trunc);
        if(!log_file)throw std::runtime_error("Cannot open plugin-settings/plugins.log.");
        std::ifstream input(list_path,std::ios::binary);std::string line;
        std::getline(input,line);
        if(trim(line)!="MERCENARIES_PLUGINS_1")throw std::runtime_error("Unsupported plugin selection file.");
        std::vector<Manifest> manifests;std::set<std::string> seen;
        while(std::getline(input,line)) {
            line=trim(line);
            if(!identifier(line)||!seen.insert(line).second||seen.size()>128)throw std::runtime_error("Invalid or duplicate plugin selection.");
            auto m=read_manifest(list_path.parent_path()/wide(line));
            if(!m||m->id!=line)throw std::runtime_error("Staged plugin manifest does not match "+line);
            manifests.push_back(*m);
        }
        validate(manifests);
        if(!base||guest_size<=0x10000)throw std::runtime_error("Guest RAM is not ready for plugins.");
        for(auto &m:manifests) {
            auto p=std::make_unique<Loaded>();p->manifest=m;
            p->directory=utf8(m.root.wstring());p->settings=utf8((fs::path(settings)/wide(m.id)).wstring());
            fs::create_directories(fs::path(wide(p->settings)));
            const uint32_t api=m.api_version;
            // v1 plugins see exactly the v1 host structure; v2 adds run_lua after it.
            p->host.v1={api>=2?uint32_t(sizeof(MercPluginHost2)):uint32_t(sizeof(MercPluginHost)),api,p.get(),
                p->manifest.id.c_str(),p->directory.c_str(),p->settings.c_str(),plugin_log,read_guest,write_guest};
            p->host.run_lua=api>=2?run_lua:nullptr;
            p->host.window=api>=2?host_window:nullptr;
            p->host.register_hotkey=api>=2?register_hotkey:nullptr;
            p->host.set_overlay=api>=2?set_overlay:nullptr;
            p->host.draw_overlay=api>=2?draw_overlay:nullptr;
            p->callbacks.struct_size=sizeof(MercPlugin);p->callbacks.api_version=api;
            auto library=m.root/wide(m.library);
            // Own directory for dependencies; no working-directory or PATH search.
            p->module=LoadLibraryExW(library.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
            if(!p->module)throw std::runtime_error(m.name+": could not load "+m.library+" (Windows error "+std::to_string(GetLastError())+"). Check x64 architecture and dependency DLLs.");
            auto entry=reinterpret_cast<MercPluginEntry>(GetProcAddress(p->module,"MercenariesPlugin_Init"));
            loaded.push_back(std::move(p));
            auto &plugin=*loaded.back();
            if(!entry)throw std::runtime_error(m.name+": DLL does not export MercenariesPlugin_Init.");
            char error[1024]={};plugin.entered=true;calling=&plugin;
            int ok=0;
            try {ok=entry(&plugin.host.v1,&plugin.callbacks,error,sizeof(error));}
            catch(...) {calling=nullptr;throw std::runtime_error(m.name+": initialization threw an exception.");}
            calling=nullptr;error[sizeof(error)-1]=0;
            if(plugin.callbacks.struct_size!=sizeof(MercPlugin)||plugin.callbacks.api_version!=plugin.manifest.api_version) {
                plugin.entered=false;
                throw std::runtime_error(m.name+": invalid plugin callback ABI.");
            }
            if(!ok)throw std::runtime_error(m.name+": initialization failed. "+error);
            plugin.active=true;record(m.name+" ["+m.id+"]: initialized");
        }
        previous=GetTickCount64();initialized=true;return 1;
    }catch(const std::exception &e) {
        calling=nullptr;failure=e.what();record(failure);shutdown_all();return 0;
    }
}
extern "C" int merc_plugins_update(void) {
    if(!initialized||GetCurrentThreadId()!=owner||dispatching||loaded.empty())return 1;
    dispatching=true;
    ULONGLONG now=GetTickCount64();
    MercPluginUpdate update{sizeof(MercPluginUpdate),MERCENARIES_PLUGIN_API_VERSION,++sequence,double(now-previous)/1000.0};
    previous=now;std::set<std::string> stopped;failure.clear();
    for(auto &entry:loaded) {
        auto &p=*entry;
        if(!p.active)continue;
        bool dependency_failed=false;
        for(const auto &id:p.manifest.requires)if(stopped.count(id))dependency_failed=true;
        if(dependency_failed) {
            p.active=false;stopped.insert(p.manifest.id);record(p.manifest.name+": updates stopped because a required plugin failed.");continue;
        }
        if(!p.callbacks.on_update)continue;
        calling=&p;int ok=0;
        update.api_version=p.manifest.api_version;
        try {ok=p.callbacks.on_update(p.callbacks.userdata,&update);}catch(...) {ok=0;}
        calling=nullptr;
        if(!ok) {
            p.active=false;stopped.insert(p.manifest.id);
            if(!failure.empty())failure+="\n";
            failure+=p.manifest.name+": update failed; this plugin and its dependents will receive no further updates. Restart the game after correcting the mod.";
            record(failure);
        }
    }
    dispatching=false;return failure.empty()?1:0;
}
extern "C" int merc_plugins_hotkey(unsigned key) {
    if(!initialized||GetCurrentThreadId()!=owner||calling||painting)return 0;
    auto found=hotkeys.find(key);
    if(found==hotkeys.end()||!found->second.plugin->active)return 0;
    auto hotkey=found->second;
    calling=hotkey.plugin;ui_callback=true;
    try {hotkey.callback(hotkey.userdata);}
    catch(...) {calling=nullptr;ui_callback=false;disable_after_exception(*hotkey.plugin,"hotkey");return 1;}
    calling=nullptr;ui_callback=false;return 1;
}
extern "C" void merc_plugins_paint_overlays(void) {
    if(!initialized||GetCurrentThreadId()!=owner||calling||painting)return;
    painting=true;
    for(auto &entry:loaded) {
        auto &p=*entry;
        if(!p.active||!p.paint)continue;
        calling=&p;
        try {p.paint(p.paint_userdata);}catch(...) {calling=nullptr;disable_after_exception(p,"overlay paint");}
        calling=nullptr;
    }
    painting=false;
}
extern "C" void merc_plugins_shutdown(void) {
    if(GetCurrentThreadId()!=owner||dispatching)return;
    dispatching=true;shutdown_all();
}


extern "C" void merc_plugins_show_error(void) {
    int n=MultiByteToWideChar(CP_UTF8,0,failure.c_str(),-1,nullptr,0);
    std::wstring text(n ? n : 1,0);
    if(n)MultiByteToWideChar(CP_UTF8,0,failure.c_str(),-1,text.data(),n);
    MessageBoxW(nullptr,text.c_str(),L"Plugin error",MB_OK|MB_ICONERROR);
}
