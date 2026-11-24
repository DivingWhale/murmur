#include "lock_notes.h"
#include "task_notice.h"
#include <cJSON.h>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

namespace lockNotes {
namespace {
using Json=std::unique_ptr<cJSON,decltype(&cJSON_Delete)>;
using std::string;
constexpr char path[]="/sdcard/murmur/notes.jsonl";
constexpr char prev[]="/sdcard/murmur/notes.prev";
constexpr char temp[]="/sdcard/murmur/notes.tmp";
std::vector<Note> notes, incoming;
string savedVersion, incomingVersion;
bool fromBackup=false;

bool validToken(const char *s) {
    return s && strlen(s)==24 && strspn(s,"0123456789abcdef")==24;
}
bool appendRow(cJSON *row,std::vector<Note>& dest) {
    auto id=cJSON_GetArrayItem(row,0);
    auto stamp=cJSON_GetStringValue(cJSON_GetArrayItem(row,1));
    auto text=cJSON_GetStringValue(cJSON_GetArrayItem(row,2));
    if (!cJSON_IsArray(row) || cJSON_GetArraySize(row)!=3 || !cJSON_IsNumber(id) ||
        id->valuedouble!=id->valueint || id->valueint<1 || (!dest.empty() && id->valueint<=dest.back().id) ||
        !stamp || strlen(stamp)>40 || !text || strlen(text)>352) return false;
    dest.push_back({id->valueint,stamp,text});
    return true;
}
bool readFile(const char *name) {
    std::unique_ptr<FILE,decltype(&fclose)> f(fopen(name,"rb"),fclose);
    if (!f) return false;
    char line[1024], token[25];
    unsigned count;
    if (!fgets(line,sizeof(line),f.get()) || sscanf(line,"%24[0-9a-f] %u",token,&count)!=2 || !validToken(token)) return false;
    std::vector<Note> restored;
    while (fgets(line,sizeof(line),f.get())) {
        Json row(cJSON_Parse(line),cJSON_Delete);
        if (!strchr(line,'\n') || !appendRow(row.get(),restored)) return false;
    }
    if (ferror(f.get()) || restored.size()!=count) return false;
    notes.swap(restored); savedVersion=token;
    return true;
}
bool writeFile() {
    FILE *f=fopen(temp,"wb");
    if (!f) return false;
    bool ok=fprintf(f,"%s %u\n",incomingVersion.c_str(),unsigned(incoming.size()))>0;
    for (auto &n:incoming) {
        Json row(cJSON_CreateArray(),cJSON_Delete);
        if (!row || !cJSON_AddItemToArray(row.get(),cJSON_CreateNumber(n.id)) ||
            !cJSON_AddItemToArray(row.get(),cJSON_CreateString(n.stamp.c_str())) ||
            !cJSON_AddItemToArray(row.get(),cJSON_CreateString(n.text.c_str()))) {ok=false; break;}
        std::unique_ptr<char,decltype(&free)> text(cJSON_PrintUnformatted(row.get()),free);
        if (!text || fprintf(f,"%s\n",text.get())<0) {ok=false; break;}
    }
    ok=ok && fflush(f)==0 && fsync(fileno(f))==0;
    if (fclose(f) || !ok) return false;
    if (access(path,F_OK)==0) {
        if (fromBackup) { if (unlink(path)!=0) return false; }
        else {
            if (access(prev,F_OK)==0 && unlink(prev)!=0) return false;
            if (rename(path,prev)!=0) return false;
        }
    }
    return rename(temp,path)==0;
}
}
const std::vector<Note>& all(){ return notes; }
const string& version(){ return incoming.empty()?savedVersion:incomingVersion; }
int cursor(){ return incoming.empty()?0:incoming.back().id; }
void restore(){ fromBackup=!readFile(path) && readFile(prev); }

bool apply(cJSON *page) {
    const char *token=cJSON_GetStringValue(cJSON_GetObjectItem(page,"version"));
    auto after=cJSON_GetObjectItem(page,"after"), more=cJSON_GetObjectItem(page,"more"), items=cJSON_GetObjectItem(page,"items");
    if (!validToken(token) || !cJSON_IsNumber(after) || !cJSON_IsBool(more) || !cJSON_IsArray(items) ||
        cJSON_GetArraySize(items)>20) return false;
    if (after->valuedouble==0) { incoming.clear(); incomingVersion=token; }
    else if (incomingVersion!=token || after->valuedouble!=cursor()) return false;
    const size_t before=incoming.size();
    cJSON *row=nullptr;
    cJSON_ArrayForEach(row,items) {
        if (!appendRow(row,incoming)) {incoming.resize(before); return false;}
    }
    if (cJSON_IsTrue(more)) return incoming.size()>before;
    if (!writeFile()) {incoming.resize(before); return false;}
    auto old=notes.begin();
    unsigned added=0;
    for (const auto &n:incoming) {
        while (old!=notes.end() && old->id<n.id) ++old;
        if (old==notes.end() || old->id!=n.id) ++added;
    }
    taskNotice::notesAdded(added);
    savedVersion=incomingVersion; incomingVersion.clear();
    notes.swap(incoming); std::vector<Note>().swap(incoming);
    fromBackup=false;
    return true;
}
}
