#include "pi_sync.h"
#include "task_notice.h"
#include "campus_wifi.h"
#include "card_usb.h"
#include "ui_kit.h"
#include <Preferences.h>
#include <SD_MMC.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <cJSON.h>
#include <atomic>
#include <memory>
#include <string>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace piSync {
namespace {
using Json=std::unique_ptr<cJSON,decltype(&cJSON_Delete)>;
using std::string;
struct Row {
    int id;
    string title, detail, plan, due;
    bool done, promoted, dirty;
    bool operator==(const Row &r) const {
        return id==r.id && title==r.title && detail==r.detail && plan==r.plan && due==r.due &&
               done==r.done && promoted==r.promoted && dirty==r.dirty;
    }
};
struct Edit { int id; bool done,promoted; };
struct Pair { char host[64]; char key[65]; };
Pair pair{};
Preferences cfg;
std::vector<Row> rows;
std::vector<int> lists[3];
string day;
bool loaded, scanned, requested=true, session, finished, cloudPending;
unsigned recordings;
uint32_t sessionAt, nextAttempt, nextDayCheck;
int lastHttp;
enum class Status { Waiting, Working, Synced, Processing, Failed, Card };
Status status=Status::Waiting;
constexpr char cache[]="/sdcard/murmur/tasks.json";
constexpr char backup[]="/sdcard/murmur/tasks.prev";
constexpr char temp[]="/sdcard/murmur/tasks.tmp";

// Card operations run only on the UI thread. The network task owns an independent RAM block.
struct Card {
    bool owned=tryTakeCardForLocalFiles();
    bool mounted=owned && SD_MMC.setPins(cardPins::clk,cardPins::cmd,cardPins::d0,cardPins::d1,cardPins::d2,cardPins::d3)
                 && SD_MMC.begin("/sdcard",false,false);
    ~Card(){ if(owned){SD_MMC.end(); releaseCardAfterRecording();} }
};
struct Job {
    bool upload=false;
    string file, path;
    uint32_t start=0, total=0;
    size_t bytes=0;
    std::unique_ptr<uint8_t,decltype(&free)> block{nullptr,free};
    std::vector<Edit> edits;
    string editsJson;
    String response;
    int code=0;
};
std::unique_ptr<Job> active;
std::atomic<bool> netDone{false};
string current;
uint32_t offset;

string todayDate() {
    time_t now=time(nullptr);
    if (now<1704067200) return {};
    tm local{}; localtime_r(&now,&local);
    char s[11]; strftime(s,sizeof(s),"%Y-%m-%d",&local);
    return s;
}
void rebuild() {
    for (auto &list:lists) list.clear();
    for (size_t i=0;i<rows.size();++i) {
        auto &r=rows[i];
        bool today=r.promoted || (!r.plan.empty() && r.plan.substr(0,10)<=day) ||
                   (!r.due.empty() && r.due.substr(0,10)<=day);
        if (r.done || today || r.plan.empty()) lists[r.done?1:today?0:2].push_back(i);
    }
}
string field(cJSON *array,int i) {
    const char *s=cJSON_GetStringValue(cJSON_GetArrayItem(array,i));
    return s?s:"";
}
bool decode(cJSON *array,std::vector<Row> &dest) {
    if (!cJSON_IsArray(array)) return false;
    cJSON *a=nullptr;
    cJSON_ArrayForEach(a,array) {
        if (!cJSON_IsArray(a) || cJSON_GetArraySize(a)<7) return false;
        auto number=[&](int i){ return int(cJSON_GetNumberValue(cJSON_GetArrayItem(a,i))); };
        if (!cJSON_IsNumber(cJSON_GetArrayItem(a,0)) || !cJSON_IsString(cJSON_GetArrayItem(a,1)) ||
            !cJSON_IsNumber(cJSON_GetArrayItem(a,5)) || !cJSON_IsNumber(cJSON_GetArrayItem(a,6))) return false;
        dest.push_back({number(0),field(a,1),field(a,2),field(a,3),field(a,4),bool(number(5)),bool(number(6)),
                        cJSON_GetArraySize(a)>7 && number(7)!=0});
    }
    return true;
}
bool save(const std::vector<Row> &data) {
    Card card;
    if (!card.mounted) return false;
    if (!SD_MMC.exists("/murmur") && !SD_MMC.mkdir("/murmur")) return false;
    Json doc(cJSON_CreateArray(),cJSON_Delete);
    for (auto &r:data) {
        cJSON *a=cJSON_CreateArray(); cJSON_AddItemToArray(doc.get(),a);
        cJSON_AddItemToArray(a,cJSON_CreateNumber(r.id));
        for (auto s:{&r.title,&r.detail,&r.plan,&r.due}) cJSON_AddItemToArray(a,cJSON_CreateString(s->c_str()));
        for (bool b:{r.done,r.promoted,r.dirty}) cJSON_AddItemToArray(a,cJSON_CreateNumber(b));
    }
    std::unique_ptr<char,decltype(&free)> text(cJSON_PrintUnformatted(doc.get()),free);
    if (!text) return false;
    FILE *f=fopen(temp,"wb"); if (!f) return false;
    bool ok=fputs(text.get(),f)>=0 && fflush(f)==0 && fsync(fileno(f))==0;
    if (fclose(f)!=0 || !ok) return false;
    if (access(cache,F_OK)==0) {
        if (access(backup,F_OK)==0 && unlink(backup)!=0) return false;
        if (rename(cache,backup)!=0) return false;
    }
    return rename(temp,cache)==0;
}
bool readCache() {
    bool found=false;
    for(auto path:{cache,backup}) {
        FILE *f=fopen(path,"rb"); if (!f) continue;
        found=true; fseek(f,0,SEEK_END); long size=ftell(f); rewind(f);
        string text;
        if (size>0 && size<1024*1024) text.resize(size);
        bool ok=!text.empty() && fread(&text[0],1,text.size(),f)==text.size();
        fclose(f);
        Json json(ok?cJSON_Parse(text.c_str()):nullptr,cJSON_Delete);
        std::vector<Row> restored;
        if (decode(json.get(),restored)) {rows.swap(restored); rebuild(); return true;}
    }
    return !found;
}
bool scan() {
    Card card;
    if (!card.mounted) return false;
    if (!loaded) {
        if (!readCache()) return false;
        loaded=true;
    }
    DIR *dir=opendir("/sdcard/murmur");
    if (!dir) return errno==ENOENT;
    recordings=0;
    string first;
    while (auto *ent=readdir(dir)) {
        string name=ent->d_name;
        if (name.size()<10 || name.compare(0,4,"rec-") || name.compare(name.size()-9,9,".wav.send")) continue;
        name.resize(name.size()-5);
        if (access(("/sdcard/murmur/"+name).c_str(),F_OK)!=0) continue;
        ++recordings;
        if (first.empty() || name<first) first=name;
    }
    closedir(dir);
    if (current!=first) { current=first; offset=0; }
    return true;
}
bool stage(Job &j) {
    Card card;
    if (!card.mounted) return false;
    FILE *f=fopen(("/sdcard/murmur/"+current).c_str(),"rb");
    if (!f) return false;
    struct stat info{};
    bool ok=fstat(fileno(f),&info)==0 && info.st_size>offset && fseek(f,offset,SEEK_SET)==0;
    if (ok) {
        j.total=info.st_size; j.start=offset;
        j.bytes=std::min<size_t>(256*1024,j.total-offset);
        j.block.reset(static_cast<uint8_t*>(ps_malloc(j.bytes)));
        ok=j.block && fread(j.block.get(),1,j.bytes,f)==j.bytes;
    }
    fclose(f);
    j.upload=true; j.file=current; j.path="/v1/recordings/"+current;
    return ok;
}
void network(void *arg) {
    auto &j=*static_cast<Job*>(arg);
    {
        WiFiClientSecure client;
        client.setPreSharedKey("murmur",pair.key);
        client.setHandshakeTimeout(5);
        HTTPClient http;
        http.setConnectTimeout(4000); http.setTimeout(8000);
        if (http.begin(client,String("https://")+pair.host+":8443"+j.path.c_str())) {
            if (j.upload) {
                http.addHeader("Content-Type","audio/wav");
                http.addHeader("X-Offset",String(j.start)); http.addHeader("X-Size",String(j.total));
                j.code=http.POST(j.block.get(),j.bytes);
            } else {
                http.addHeader("Content-Type","application/json");
                j.code=http.POST(reinterpret_cast<uint8_t*>(&j.editsJson[0]),j.editsJson.size());
            }
            if (j.code==200) j.response=http.getString();
            http.end();
        }
    }
    netDone.store(true); vTaskDelete(nullptr);
}
bool apply(Job &j) {
    lastHttp=j.code;
    if (j.code!=200) return false;
    Json doc(cJSON_Parse(j.response.c_str()),cJSON_Delete);
    if (j.upload) {
        cJSON *at=cJSON_GetObjectItem(doc.get(),"offset");
        if (!cJSON_IsNumber(at) || at->valuedouble<0 || at->valuedouble>j.total) return false;
        offset=uint32_t(at->valuedouble);
        if (offset==j.total) {
            if (!cJSON_IsTrue(cJSON_GetObjectItem(doc.get(),"received"))) return false;
            Card card;
            if (!card.mounted || unlink(("/sdcard/murmur/"+j.file+".send").c_str())!=0) {offset=0; return false;}
            current.clear(); offset=0; scanned=false;
        }
        return true;
    }
    std::vector<Row> next;
    if (!decode(cJSON_GetObjectItem(doc.get(),"tasks"),next)) return false;
    for (auto &old:rows) {
        if (!old.dirty) continue;
        bool acknowledged=false;
        for (auto &e:j.edits) if (e.id==old.id && e.done==old.done && e.promoted==old.promoted) acknowledged=true;
        if (!acknowledged) for (auto &r:next) if (r.id==old.id) {
            r.done=old.done; r.promoted=old.promoted; r.dirty=true;
        }
    }
    if (next!=rows) {
        if (!save(next)) return false;
        // Both the card cache and the Pi snapshot are in ascending task ID order.
        auto old=rows.begin();
        for (const auto &r:next) {
            while (old!=rows.end() && old->id<r.id) ++old;
            if (old==rows.end() || old->id!=r.id) taskNotice::added(r.title,r.plan,r.due);
        }
        rows.swap(next); rebuild();
    }
    cloudPending=cJSON_GetNumberValue(cJSON_GetObjectItem(doc.get(),"processing"))>0;
    status=cloudPending?Status::Processing:Status::Synced;
    finished=!cloudPending && !pending();
    nextAttempt=millis()+1500;
    return true;
}
}

void begin() {
    cfg.begin("murmur-pi",false);
    if (cfg.getBytesLength("pair")==sizeof(pair)) cfg.getBytes("pair",&pair,sizeof(pair));
    day=todayDate();
    if (day.empty()) day=cfg.getString("last-day","").c_str();
}
bool paired(){ return pair.host[0] && pair.key[0]; }
bool keepRadio(){ return paired() && session && !finished && millis()-sessionAt<60000; }
bool needsWake(){return paired() && (pending() || cloudPending || status==Status::Failed);}
void request(){requested=true; finished=false; nextAttempt=0; startSavedCampusWifi();}
void recordingSaved(){scanned=false; requested=true; finished=false; ++recordings;}
unsigned pending(){ unsigned n=recordings; for(auto &r:rows) n+=r.dirty; return n; }
const char *statusText(){
    using ui::tr;
    switch(status) {
    case Status::Working: return tr("正在同步","Syncing");
    case Status::Synced: return tr("已同步","Synced");
    case Status::Processing: return tr("正在整理","Processing");
    case Status::Failed: return tr("同步失败","Sync failed");
    case Status::Card: return tr("请检查 TF 卡","Check TF card");
    default: return tr("尚未同步","Never synced");
    }
}
bool service(bool recording,bool pickerOpen) {
    if (!paired()) return false;
    uint32_t now=millis();
    bool changed=false;
    if (int32_t(now-nextDayCheck)>=0 && !pickerOpen) {
        nextDayCheck=now+1000;
        string date=todayDate();
        if (!date.empty() && date!=day){ day=date; cfg.putString("last-day",day.c_str()); rebuild(); changed=true; }
    }
    if (!campusOnline()) { session=false; finished=false; }
    if (recording || pickerOpen || now<6500) return changed;
    if (active && netDone.load()) {
        if (!apply(*active)) {status=Status::Failed; finished=true; nextAttempt=now+5000;}
        active.reset(); netDone.store(false); changed=true;
    }
    if (active || int32_t(now-nextAttempt)<0) return changed;
    if (!scanned) {
        scanned=scan(); changed=true;
        if (!scanned) {status=Status::Card; nextAttempt=now+5000; return changed;}
    }
    if (!campusOnline()) return changed;
    if (!session || requested) {
        sessionAt=now; session=true; finished=false; requested=false;
    }
    if (finished || now-sessionAt>=60000) return changed;
    active.reset(new Job);
    if (!current.empty()) {
        if (!stage(*active)) {active.reset(); scanned=false; status=Status::Card; nextAttempt=now+5000; return true;}
    } else {
        active->path="/v1/sync"; active->editsJson="[";
        for (auto &r:rows) if(r.dirty) {
            if (!active->edits.empty()) active->editsJson+=",";
            char item[64]; snprintf(item,sizeof(item),"[%d,%d,%d]",r.id,r.done,r.promoted);
            active->editsJson+=item; active->edits.push_back({r.id,r.done,r.promoted});
        }
        active->editsJson+="]";
    }
    status=Status::Working;
    if (xTaskCreate(network,"pi-sync",10240,active.get(),1,nullptr)!=pdPASS) {
        active.reset(); finished=true; status=Status::Failed;
    }
    return true;
}
const std::vector<int>& ids(bool today,bool completed){return lists[today?(completed?1:0):2];}
const char *title(int i){return rows[i].title.c_str();}
const char *detail(int i){
    const auto &r=rows[i];
    static string meta;
    meta=r.plan.size()>=16?r.plan.substr(11,5):"";
    if (!r.detail.empty()) {
        if (!meta.empty()) meta+=" · ";
        meta+=r.detail;
    }
    return meta.c_str();
}
const char *deadline(int i){
    const auto &due=rows[i].due;
    if (due.empty()) return nullptr;
    static string label;
    label=ui::tr("截止 ","Due ");
    label+=due.substr(5,5);
    if (due.size()>=16) label+=" "+due.substr(11,5);
    return label.c_str();
}
bool done(int i){return rows[i].done;}
bool change(int i,bool promote) {
    auto &r=rows[i]; const Row old=r;
    if(promote) r.promoted=true; else r.done=!r.done;
    r.dirty=true;
    if (!save(rows)){r=old; return false;}
    rebuild(); requested=true;
    return true;
}
void command(char *args,Print &out) {
    if (!strcmp(args,"status")) {
        out.printf("sync paired=%u status=%u http=%d queued=%u tasks=%u processing=%u\n",
                   paired(),unsigned(status),lastHttp,pending(),unsigned(rows.size()),cloudPending);
    } else if (!strcmp(args,"now")) {request(); out.println("sync requested");}
    else if (!strncmp(args,"pair ",5) && !active) {
        Pair candidate{};
        char *key=strchr(args+5,' ');
        if (!key) {out.println("sync invalid pair");return;}
        *key++=0;
        IPAddress ip;
        if (!ip.fromString(args+5) || strlen(key)!=64 || strspn(key,"0123456789abcdefABCDEF")!=64) {
            out.println("sync invalid pair");return;
        }
        strlcpy(candidate.host,args+5,sizeof(candidate.host)); strlcpy(candidate.key,key,sizeof(candidate.key));
        if (cfg.putBytes("pair",&candidate,sizeof(candidate))!=sizeof(candidate)){out.println("sync save failed");return;}
        pair=candidate; scanned=false; request(); out.println("sync paired");
    } else out.println("sync command unavailable");
}
}
