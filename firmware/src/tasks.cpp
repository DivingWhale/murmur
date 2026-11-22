#include "tasks.h"
#include "pi_sync.h"
#include "ui_kit.h"
#include <Preferences.h>

namespace model {
namespace {
Preferences store;
uint16_t state;    // Bits 0..5: completed. Bits 8..13: moved into Today.
uint16_t waiting;
struct Text { const char *zh, *english; };
const Text titles[maxTasks] = {
    {"给魏老师发邮件约实验室时间","Email Dr. Weber about lab slot"},
    {"还图书馆的书","Return library books"},{"买咖啡滤纸","Buy coffee filters"},
    {"完成报告初稿","Finish report draft"},{"备份照片","Back up photos"},
    {"读研讨课第四章","Read chapter 4 for seminar"}
};
// The first three tasks are scheduled for today; the rest wait in When I Have Time.
constexpr int scheduledToday=3;
const Text notes[] = {
    {"组会上终于把实验设计讲清楚了。准备充分时，其实没那么紧张。",
     "The best ideas this week came when I wasn't at the desk. Walk first, then write."},
    {"散步的时候想明白了：先把问题写下来，答案常常就在问题里。",
     "Asking one honest question in the seminar was easier than staying quiet."}
};
const Text noteTimes[] = {{"11月16日 20:10","12 Nov · 18:04"},{"11月17日 07:30","15 Nov · 09:22"}};
const char *pick(const Text &t) { return ui::tr(t.zh,t.english); }
bool inToday(int id) { return id<scheduledToday || (state & (1u<<(8+id))); }
bool save(uint16_t next) {
    if (store.putUShort("tasks",next)!=sizeof(next)) return false;
    state=next;
    notePending();
    return true;
}
}

void load() {
    store.begin("murmur-ui");
    state=store.getUShort("tasks",0);
    waiting=store.getUShort("pending",0);
}
const std::vector<int>& ids(bool today,bool completed) {
#ifdef ARDUINO_ARCH_ESP32
    if (piSync::paired()) return piSync::ids(today,completed);
#endif
    static std::vector<int> lists[3];
    auto &list=lists[today?(completed?1:0):2];
    list.clear();
    for (int i=0;i<maxTasks;++i)
        if (inToday(i)==today && (!today || done(i)==completed)) list.push_back(i);
    return list;
}
const char *title(int id) {
#ifdef ARDUINO_ARCH_ESP32
    if (piSync::paired()) return piSync::title(id);
#endif
    return pick(titles[id]); }
const char *detail(int id) {
#ifdef ARDUINO_ARCH_ESP32
    if (piSync::paired()) return piSync::detail(id);
#endif
    return id==1?ui::tr("延续","Carried over"):""; }
const char *deadline(int id) {
#ifdef ARDUINO_ARCH_ESP32
    if (piSync::paired()) return piSync::deadline(id);
#endif
    return id==3?ui::tr("周五截止","Due Fri"):nullptr; }
bool done(int id) {
#ifdef ARDUINO_ARCH_ESP32
    if (piSync::paired()) return piSync::done(id);
#endif
    return state & (1u<<id); }
bool toggleDone(int id) {
#ifdef ARDUINO_ARCH_ESP32
    if (piSync::paired()) return piSync::change(id,false);
#endif
    return save(state ^ (1u<<id)); }
bool moveToToday(int id) {
#ifdef ARDUINO_ARCH_ESP32
    if (piSync::paired()) return piSync::change(id,true);
#endif
    return save(state | (1u<<(8+id))); }
unsigned pending() { return piSync::paired()?piSync::pending():waiting; }
void notePending() { if (piSync::paired()) { piSync::recordingSaved(); return; } ++waiting; store.putUShort("pending",waiting); }
int noteCount() { return int(sizeof(notes)/sizeof(*notes)); }
const char *note(int index) { return pick(notes[index]); }
const char *noteTime(int index) { return pick(noteTimes[index]); }
}
