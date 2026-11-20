#include "audio.h"
#include "board.h"
#include "card_usb.h"
#include "campus_wifi.h"
#include "campus_profile.h"
#include <cstring>
#include <USB.h>
#include "fonts.h"
#include "icons.h"
#include "ui_kit.h"
#include "tasks.h"
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <sys/time.h>
#include <time.h>

using namespace ui;

namespace {
// Touch targets are drawn and hit-tested from the same rectangles (screen pixels).
struct Rect {
    int x,y,w,h;
    bool contains(int px,int py) const { return px>=x && px<x+w && py>=y && py<y+h; }
};
namespace layout {
constexpr int header=32;                       // Header band; a swipe down from here opens the quick panel.
constexpr Rect taskList{0,header,240,132};     // Today's scrolling rows, above the When I Have Time strip.
constexpr int rowPitch=63, rowHeight=60;
constexpr int strip=taskList.y+taskList.h;     // When I Have Time, to the bottom edge.
constexpr Rect countTap{0,0,105,header};       // Today header count: remaining <-> completed.
constexpr Rect backTap{0,0,145,header};        // "< Title" in settings headers.
constexpr Rect recCancel{8,170,108,64}, recSave{124,170,108,64};
constexpr Rect pickCancel{8,130,108,72}, pickMove{124,130,108,72};
constexpr Rect syncTile{10,42,106,66}, languageTile{124,42,106,66}, allSettings{10,176,220,48};
constexpr Rect panelBrightness{10,116,220,52}, screenBrightness{14,108,212,72};
constexpr int settingRow=56;
constexpr Rect clockMinus{14,126,100,42}, clockPlus{126,126,100,42}, clockNext{14,181,212,42};
}
USBCDC usbSerial;
enum class Screen { Lock, Today, Picker, Recording, ConfirmDiscard, Saved, Error, Claude, MD, Panel, Settings, Brightness, SetClock, Sleep, ReleasePower, UnplugPower };
Screen screen = Screen::Lock;
Screen panelFrom = Screen::Today;
Screen recordingFrom = Screen::Today;
bool discardSelected=false;
bool recordingActive() { return screen==Screen::Recording || screen==Screen::ConfirmDiscard; }
enum Setting { Network, Sync, Language, Bright, AutoSleep, Storage, Battery, Clock, Firmware, SettingCount };
int brightness=16, savedBrightness=16, sleepOption=1, settingFocus=0, settingsY=0;
const uint32_t idleLimits[]={30000,60000,300000,0};
int64_t freeBytes=-1;
uint32_t lastInput, toastStarted;
bool offlineToast, settingsTouch;
int slideDirection;
Motion pageMotion=Motion::None;
Screen renderedScreen=Screen::Lock;
uint32_t motionEnds=0;
lv_point_t settingsPoint;
lv_obj_t *settingsList, *settingRows[SettingCount], *brightnessFill, *brightPercent, *filledPercent, *toast;
constexpr int pageWidth=240;
constexpr Screen pages[]={Screen::Today,Screen::Claude,Screen::MD};
lv_obj_t *pageTrack;
bool draggingPages=false;
int pageIndex(Screen value) {
    for (int i=0;i<3;++i) if (pages[i]==value) return i;
    return -1;
}
void positionTrack(void *track,int32_t x) {
    lv_obj_set_x(static_cast<lv_obj_t*>(track),x);
}
Preferences prefs;
bool sleepAfter, dirty=true;
bool powerHoldStarted=false;
int focused=0, taskScrollY=0, ticket=0, note=0, battery=-1;
bool isCharging, chargeSample;
uint32_t chargeChangedAt;
// The pager keeps one header per page alive while the user swipes.
lv_obj_t *batteryIcons[sizeof(pages)/sizeof(*pages)], *batterySetting;
unsigned batteryIconCount;
bool showCompleted=false, revealFocus=false;
lv_obj_t *taskList;
uint32_t entered, interacted, rotated, lastBattery, lastTick, scrollPeriod=3000;
const char *faultZh="", *faultEn="";
lv_obj_t *timerLabel, *recordDot, *bars[20], *countdown, *clockLabel, *dateLabel;
// Recent input levels (0-20), oldest first, drawn as a scrolling waveform of 5-35 px bars.
uint8_t wave[20];
uint32_t waveSampled;
void paintWave() {
    for (int i=0;i<20;++i) {
        const int h=5+wave[i]*30/20;
        lv_obj_set_height(bars[i],h);
        lv_obj_set_y(bars[i],138-h/2);
    }
}
lv_obj_t *overlay, *progressFill;
lv_obj_t *tickerTitle, *tickerChip, *tickerChipText, *tickerDots[model::maxTasks];
lv_anim_t scrollAnimation;
unsigned lastSeconds;
uint8_t lastLevel;
struct Item { const char *zh, *english; }; // A label in both languages.
// Screens list Today's unfinished tasks, or the finished ones when the header count is tapped.
int tasks(bool today,int *ids) { return today?model::today(ids,showCompleted):model::later(ids); }
void storeBrightness();
void settlePage(int index) {
    draggingPages=false;
    int target=-index*pageWidth;
    int start=lv_obj_get_style_x(pageTrack,0);
    lv_anim_del(pageTrack,positionTrack);
    screen=renderedScreen=pages[index];
    lastInput=entered=interacted=rotated=millis();
    motionEnds=lastInput;
    if (toast) { lv_obj_del(toast); toast=nullptr; }
    offlineToast=false;
    if (target==start) return;
    lv_anim_t a; lv_anim_init(&a);
    lv_anim_set_var(&a,pageTrack); lv_anim_set_exec_cb(&a,positionTrack);
    int duration=constrain(abs(target-start)*260/pageWidth,80,260);
    lv_anim_set_time(&a,duration); lv_anim_set_path_cb(&a,lv_anim_path_ease_out);
    lv_anim_set_values(&a,start,target); lv_anim_start(&a);
    motionEnds+=duration;
}
void go(Screen target) {
    if (screen==Screen::Settings && settingsList) settingsY=lv_obj_get_scroll_y(settingsList);
    storeBrightness();
    settingsTouch=false;
    offlineToast=false;
    pageMotion=Motion::None;
    draggingPages=false;
    if (pageTrack && screen==renderedScreen && pageIndex(target)>=0 && target!=screen) {
        settlePage(pageIndex(target));
        return;
    }
    const bool waking=screen==Screen::Sleep && target!=Screen::Sleep && target!=Screen::Recording;
    if (target==Screen::Panel) pageMotion=screen==Screen::Settings?Motion::Right:Motion::Down;
    else if (screen==Screen::Panel) pageMotion=target==Screen::Settings?Motion::Left:Motion::Up;
    else if (screen==Screen::Lock && target==Screen::Today) pageMotion=Motion::Up;
    else if (screen==Screen::Settings && (target==Screen::Brightness || target==Screen::SetClock)) pageMotion=Motion::Left;
    else if ((screen==Screen::Brightness || screen==Screen::SetClock) && target==Screen::Settings) pageMotion=Motion::Right;
    screen=target;
    lastInput=entered=interacted=rotated=millis();
    dirty=true;
    setLowPower(target==Screen::Sleep);
    setBacklight(target==Screen::Sleep?0:brightness*5);
    if (waking) startCampusWifiOnWake();
}
void showError(const char *zh,const char *english) {
    faultZh=zh; faultEn=english; go(Screen::Error);
}
void audioError(AudioResult r) {
    powerHoldStarted=false;
    if (r==AudioResult::Mic) showError("麦克风不可用","Microphone unavailable");
    else if (r==AudioResult::Card) showError("请插入可用的 TF 卡","Insert a writable microSD");
    else showError("录音写入失败","Recording write failed");
}
void recordKey() {
    if (recordingActive()) {
        auto result=finishRecording();
        if (result==AudioResult::Ok) { model::notePending(); go(Screen::Saved); startCampusWifiAfterRecording(); } // Upload point once the Pi sync exists.
        else audioError(result);
        return;
    }
    sleepAfter=screen==Screen::Sleep;
    setLowPower(false); // Full speed before the microphone and card start.
    battery=batteryPercent();
    if (battery>=0 && battery<=3 && !charging()) {
        showError("电量过低","Battery too low");
        return;
    }
    auto result=startRecording();
    if (result==AudioResult::Ok) { memset(wave,0,sizeof(wave)); recordingFrom=screen; go(Screen::Recording); }
    else audioError(result);
}
void askDiscard() {
    discardSelected=false;
    go(Screen::ConfirmDiscard);
}
void resolveDiscard(bool discard) {
    if (!discard) { go(Screen::Recording); return; }
    if (discardRecording()) go(recordingFrom);
    else showError("取消失败，请检查 TF 卡","Discard failed. Check SD.");
}
constexpr int batteryIconWidth=76, batteryIconHeight=26;
void batteryText(char (&text)[8]) {
    strcpy(text,"--");
    if (battery>=0) snprintf(text,sizeof(text),"%d%%",max(battery>0?1:0,(battery+2)/5*5));
}
// Width of the drawn battery indicator (glyph, gap and percentage), for laying out what sits to its left.
int batteryContentWidth() {
    char text[8]; batteryText(text);
    return lv_txt_get_width(text,strlen(text),fontFor(en?13:14),0,LV_TEXT_FLAG_NONE)+3+22;
}
void paintBatteryIcon(lv_obj_t *icon) {
    lv_obj_clean(icon);
    uint32_t color=isCharging || battery<0 || battery>20?muted:(battery<=10?red:due);
    char text[8]; batteryText(text);
    const auto *font=fontFor(en?13:14);
    int width=lv_txt_get_width(text,strlen(text),font,0,LV_TEXT_FLAG_NONE);
    int x=batteryIconWidth-width-3-22, y=batteryIconHeight/2-6;
    box(x,y,19,12,bg,3,color,1,icon);
    box(x+20,y+4,2,4,color,1,0,0,icon);
    if (battery>0) box(x+3,y+3,max(1,(battery*14+50)/100),7,color,1,0,0,icon);
    if (isCharging) {
        static const lv_point_t bolt[]={{6,0},{2,5},{6,5},{3,10}};
        for (int stroke : {3,1}) {
            auto *line=lv_line_create(icon);
            lv_line_set_points(line,bolt,4);
            lv_obj_set_style_line_width(line,stroke,0);
            lv_obj_set_style_line_color(line,lv_color_hex(stroke==3?bg:color),0);
            lv_obj_set_pos(line,x+5,y+1);
        }
    }
    label(batteryIconWidth-width,batteryIconHeight/2-font->line_height/2,width,text,en?13:14,color,
          LV_TEXT_ALIGN_RIGHT,LV_LABEL_LONG_CLIP,icon);
}
void batteryIcon(int right,int cy) {
    auto *icon=box(right-batteryIconWidth,cy-batteryIconHeight/2,batteryIconWidth,batteryIconHeight,bg);
    lv_obj_set_style_bg_opa(icon,LV_OPA_TRANSP,0);
    batteryIcons[batteryIconCount++]=icon;
}
void refreshBatteryWidgets() {
    for (unsigned i=0;i<batteryIconCount;++i) paintBatteryIcon(batteryIcons[i]);
    if (batterySetting) {
        String value=battery<0?"--":String((battery+2)/5*5)+"%";
        if (isCharging) value+=tr(" · 充电中"," · charging");
        lv_label_set_text(batterySetting,value.c_str());
    }
    if (recordingActive()) {
        bool low=battery>=0 && battery<20 && !isCharging;
        if (low) {
            lv_obj_clear_flag(batteryIcons[0],LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(dateLabel,LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(batteryIcons[0],LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(dateLabel,LV_OBJ_FLAG_HIDDEN);
        }
    }
}
void refreshClock() {
    time_t now=time(nullptr);
    struct tm t{};
    localtime_r(&now,&t);
    bool set=now>=1704067200;
    char hm[12]="--:--", date[48]="";
    if (set) {
        strftime(hm,sizeof(hm),"%H:%M",&t);
        const char *days[]={"周日","周一","周二","周三","周四","周五","周六"};
        if (en) strftime(date,sizeof(date),pageIndex(screen)>=0?"%a %d":"%a %d %b",&t);
        else if (pageIndex(screen)>=0) snprintf(date,sizeof(date),"%s",days[t.tm_wday]);
        else snprintf(date,sizeof(date),"%d月%d日 %s",t.tm_mon+1,t.tm_mday,days[t.tm_wday]);
    } else if (screen==Screen::Lock || screen==Screen::Settings)
        snprintf(date,sizeof(date),"%s",tr("待校时","Clock unset"));
    if (clockLabel) lv_label_set_text(clockLabel,hm);
    if (dateLabel) lv_label_set_text(dateLabel,date);
}
void animateNote(lv_obj_t *obj) {
    if (slideDirection) {
        translate(obj,28*slideDirection,0,false,280);
        fade(obj,LV_OPA_TRANSP,280);
    }
}
void lockView() {
    const int top=12, right=226, small=en?13:14, row=fontFor(small)->line_height;
    const int headerHeight=2*row+3;
    clockLabel=label(14,top+(headerHeight-int(fontFor(34)->line_height))/2,112,"",34);
    lv_obj_set_style_text_letter_space(clockLabel,-1,0);
    batteryIcon(right,top+row/2);
    dateLabel=label(128,top+row+3,right-128,"",small,muted,LV_TEXT_ALIGN_RIGHT);
    refreshClock();

    int noteTop=top+headerHeight+10;
    auto *body=label(14,noteTop,212,model::note(note),en?17:18,ink,LV_TEXT_ALIGN_LEFT,LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(body,en?2:4,0);
    lv_obj_update_layout(body);
    auto *stamp=label(14,noteTop+6+lv_obj_get_height(body),212,model::noteTime(note),14,muted);
    animateNote(body); animateNote(stamp);
    dots(14,221,2,note);
    label(40,211,186,tr("OK/上滑 解锁 · + 录音","OK/↑ unlock · + record"),14,muted,LV_TEXT_ALIGN_RIGHT);
}
lv_obj_t *scrollTitle(int x,int y,int w,const char *s,bool scroll,bool done=false) {
    const int size=en?16:17;
    auto *o=label(x,y,w,s,size,done?dim:ink,LV_TEXT_ALIGN_LEFT,
                  scroll?LV_LABEL_LONG_SCROLL_CIRCULAR:LV_LABEL_LONG_DOT);
    lv_obj_set_style_anim_speed(o,30,0);
    if (scroll) lv_obj_set_style_anim(o,&scrollAnimation,0);
    if (done) lv_obj_set_style_text_decor(o,LV_TEXT_DECOR_STRIKETHROUGH,0);
    return o;
}
void updateTicker(int *later,int count) {
    ticket%=count;
    int id=later[ticket], width=model::deadline(id)?145:210;
    lv_obj_set_width(tickerTitle,width);
    lv_label_set_text(tickerTitle,model::title(id));
    if (model::deadline(id)) {
        lv_label_set_text(tickerChipText,model::deadline(id));
        lv_obj_clear_flag(tickerChip,LV_OBJ_FLAG_HIDDEN);
    }
    else lv_obj_add_flag(tickerChip,LV_OBJ_FLAG_HIDDEN);
    for (int i=0;i<count;++i)
        lv_obj_set_style_bg_color(tickerDots[i],lv_color_hex(i==ticket?ink:dim),0);
    int textWidth=lv_txt_get_width(model::title(id),strlen(model::title(id)),fontFor(en?16:17),0,LV_TEXT_FLAG_NONE);
    scrollPeriod=textWidth<=width?3000:1000+(textWidth+40)*1000/30;
    animateNote(tickerTitle); animateNote(tickerChip);
}
// Compact sync status left of the battery: a check when nothing is waiting, otherwise an icon and the count.
void syncStatus(int right,int cy) {
    auto *parent=panelRoot?panelRoot:pageLayer;
    if (!model::pending()) { icon(parent,Icon::Check,11,right-11,cy-6,muted); return; }
    char count[8]; snprintf(count,sizeof(count),"%u",model::pending());
    const bool online=campusOnline();
    const uint32_t color=online?syncBlue:muted;
    const int w=lv_txt_get_width(count,strlen(count),fontFor(13),0,LV_TEXT_FLAG_NONE);
    label(right-w,cy-fontFor(13)->line_height/2,w,count,13,color);
    if (online) icon(parent,Icon::Sync,11,right-w-4-11,cy-6,color);
    else icon(parent,Icon::WifiOff,12,right-w-4-12,cy-6,color);
}
void todayView() {
    int ids[model::maxTasks],later[model::maxTasks],n=tasks(true,ids),k=tasks(false,later);
    focused=constrain(focused,0,max(0,n-(k==0)));
    char remaining[32];
    snprintf(remaining,sizeof(remaining),showCompleted?tr("已完成：%d","Done: %d"):tr("剩余数：%d","Left: %d"),n);
    int size=en?16:17;
    int width=lv_txt_get_width(remaining,strlen(remaining),fontFor(size),0,LV_TEXT_FLAG_NONE);
    label(12,16-fontFor(size)->line_height/2,width,remaining,size);
    int dateX=12+width+6;
    dateLabel=label(dateX,16-fontFor(en?13:14)->line_height/2,166-dateX,"",en?13:14,muted);
    refreshClock();
    batteryIcon(230,16);
    syncStatus(230-batteryContentWidth()-8,16);
    auto *previousParent=panelRoot;
    taskList=box(layout::taskList.x,layout::taskList.y,layout::taskList.w,layout::taskList.h,bg);
    lv_obj_add_flag(taskList,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scroll_dir(taskList,LV_DIR_VER);
    lv_obj_set_scrollbar_mode(taskList,LV_SCROLLBAR_MODE_OFF);
    panelRoot=taskList;
    if (!n) center(47,showCompleted?tr("暂无已完成任务","No completed tasks"):tr("全部完成","All clear"),17,muted);
    for (int i=0;i<n;++i) {
        int id=ids[i],y=i*layout::rowPitch;
        bool focus=focused==i, done=model::done(id);
        if (focus) box(5,y,230,layout::rowHeight,surface,9,yellow,2);
        box(15,y+20,20,20,done?muted:(focus?surface:bg),10,done?muted:dim,2);
        if (done) icon(taskList,Icon::Tick,12,19,y+24,bg);
        const char *meta=model::deadline(id)?model::deadline(id):model::detail(id);
        scrollTitle(45,y+(*meta?8:19),178,model::title(id),focus,done);
        if (*meta) label(45,y+33,178,meta,14,model::deadline(id)?due:muted);
    }
    panelRoot=previousParent;
    lv_obj_update_layout(taskList);
    lv_obj_scroll_to_y(taskList,taskScrollY,LV_ANIM_OFF);
    if (revealFocus && focused<n) {
        int top=focused*layout::rowPitch;
        int offset=constrain(int(lv_obj_get_scroll_y(taskList)),max(0,top+layout::rowHeight-layout::taskList.h),top);
        lv_obj_scroll_to_y(taskList,offset,LV_ANIM_ON);
    }
    revealFocus=false;
    box(0,layout::strip,240,1,line);
    bool pause=focused==n;
    if (pause) box(5,170,230,65,surface,9,yellow,2);
    label(14,179,183,pause?tr("已暂停 · 按 OK 选择","PAUSED · OK TO CHOOSE"):
                        tr("有空再做","WHEN I HAVE TIME"),13,pause?yellow:muted);
    if (!k) { label(14,201,210,tr("暂无任务","Nothing waiting"),17,dim); return; }
    for (int i=0;i<k;++i) tickerDots[i]=box(226-9*k+9*i,185,5,5,dim,3);
    tickerTitle=scrollTitle(14,200,210,"",true);
    tickerChip=box(166,200,60,23,0x30251c,8);
    tickerChipText=label(0,2,60,"",13,due,LV_TEXT_ALIGN_CENTER,LV_LABEL_LONG_CLIP,tickerChip);
    updateTicker(later,k);
}
void pickerView() {
    int ids[model::maxTasks],n=tasks(false,ids);
    if (!n) { go(Screen::Today); return; }
    ticket%=n;
    center(18,tr("移到今天？","MOVE TO TODAY?"),14,yellow);
    // Prototype .cfw: the title (up to two lines), then a row with the deadline chip and position dots.
    auto *heading=center(45,model::title(ids[ticket]),20);
    lv_obj_update_layout(heading);
    const int lines=max(1,min(2,int(lv_obj_get_height(heading)/fontFor(20)->line_height)));
    lv_obj_set_height(heading,lines*fontFor(20)->line_height);
    lv_obj_set_y(heading,lines==1?53:41);
    animateNote(heading);
    const int rowY=(lines==1?53:41)+lines*fontFor(20)->line_height+8;
    const char *deadline=model::deadline(ids[ticket]);
    const bool chip=deadline!=nullptr;
    const int dotsWidth=n>1?9*n-4:0, chipWidth=chip?60:0, gap=chip && n>1?10:0;
    const int rowX=(240-chipWidth-gap-dotsWidth)/2;
    if (chip) {
        auto *c=box(rowX,rowY-4,60,23,0x30251c,8);
        label(0,2,60,deadline,13,due,LV_TEXT_ALIGN_CENTER,LV_LABEL_LONG_CLIP,c);
    }
    if (n>1) dots(rowX+chipWidth+gap,rowY+5,n,ticket);
    const Rect &c=layout::pickCancel, &m=layout::pickMove;
    box(c.x,c.y,c.w,c.h,surface,9);
    box(m.x,m.y,m.w,m.h,yellow,9);
    label(8,142,108,tr("取消","Cancel"),20,ink,LV_TEXT_ALIGN_CENTER);
    label(8,173,108,tr("长按 OK","Hold OK"),13,muted,LV_TEXT_ALIGN_CENTER);
    label(124,142,108,tr("移入","Move"),20,bg,LV_TEXT_ALIGN_CENTER);
    label(124,173,108,"OK",13,bg,LV_TEXT_ALIGN_CENTER);
    countdown=center(211,"",14,muted);
}
void showDuration(lv_obj_t *o,uint32_t seconds) {
    char s[20];
    snprintf(s,sizeof(s),"%lu:%02lu",(unsigned long)(seconds/60),(unsigned long)(seconds%60));
    lv_label_set_text(o,s);
}
void settingsHeader(const char *title) {
    symbol(8,9,12,LV_SYMBOL_LEFT);
    label(25,5,126,title,17);
    batteryIcon(230,16);
}
// Sun and percentage are drawn twice: light on the track and dark on the fill. The fill clips its
// copies at its right edge, so text crossing the edge stays readable on both sides.
void paintBrightness() {
    const bool big=screen==Screen::Brightness;
    int pct=brightness*5;
    lv_obj_set_width(brightnessFill,(big?layout::screenBrightness:layout::panelBrightness).w*pct/100);
    lv_label_set_text_fmt(brightPercent,"%d%%",pct);
    if (!big) lv_label_set_text_fmt(filledPercent,"%d%%",pct);
}
void brightnessBar(bool big) {
    const Rect &r=big?layout::screenBrightness:layout::panelBrightness;
    const int w=r.w,h=r.h;
    auto *bar=box(r.x,r.y,w,h,0x2a2d33,12);
    lv_obj_set_style_clip_corner(bar,true,0);
    const int textY=(h-fontFor(16)->line_height)/2;
    icon(bar,Icon::Sun,20,12,(h-20)/2,muted);
    if (!big) brightPercent=label(w-72,textY,60,"",16,ink,LV_TEXT_ALIGN_RIGHT,LV_LABEL_LONG_CLIP,bar);
    brightnessFill=box(0,0,1,h,ink,0,0,0,bar);
    icon(brightnessFill,Icon::Sun,20,12,(h-20)/2,bg);
    if (big) brightPercent=center(47,"",40);
    else filledPercent=label(w-72,textY,60,"",16,bg,LV_TEXT_ALIGN_RIGHT,LV_LABEL_LONG_CLIP,brightnessFill);
    paintBrightness();
}
void setBrightnessLevel(int level) {
    level=constrain(level,1,20);
    if (level==brightness) return;
    brightness=level;
    setBacklight(level*5);
    if (brightnessFill) paintBrightness();
}
void storeBrightness() {
    if (brightness!=savedBrightness && prefs.putUChar("brightness",brightness))
        savedBrightness=brightness;
}
void panelView() {
    auto *panel=box(0,0,240,240,0x121418);
    panelRoot=panel;
    icon(panel,campusOnline()?Icon::Wifi:Icon::WifiOff,15,10,15,muted);
    label(31,22-fontFor(14)->line_height/2,110,campusOnline()?"HKUSTGZ":tr("未连接","Disconnected"),14);
    label(145,22-fontFor(13)->line_height/2,85,campusOnline()?tr("在线","Online"):tr("离线","Offline"),13,muted,LV_TEXT_ALIGN_RIGHT);
    for (const Rect &t : {layout::syncTile,layout::languageTile}) box(t.x,t.y,t.w,t.h,surface,12);
    icon(panel,Icon::Sync,19,10+(106-19)/2,54,ink);
    label(10,78,106,tr("立即同步","Sync now"),14,ink,LV_TEXT_ALIGN_CENTER);
    label(124,48,106,tr("中","EN"),20,ink,LV_TEXT_ALIGN_CENTER);
    label(124,78,106,tr("语言","Language"),14,ink,LV_TEXT_ALIGN_CENTER);
    brightnessBar(false);
    box(layout::allSettings.x,layout::allSettings.y,layout::allSettings.w,layout::allSettings.h,surface,12);
    label(22,188,172,tr("全部设置","All settings"),17);
    symbol(202,193,20,LV_SYMBOL_RIGHT);
    box(102,232,36,4,0x3a3e45,2);
    panelRoot=nullptr;
}
void styleRow(int i) {
    bool active=i==settingFocus;
    auto *row=settingRows[i];
    lv_obj_set_style_bg_color(row,lv_color_hex(active?surface:bg),0);
    lv_obj_set_style_border_color(row,lv_color_hex(active?yellow:line),0);
    lv_obj_set_style_border_width(row,active?2:1,0);
    lv_obj_set_style_border_side(row,active?LV_BORDER_SIDE_FULL:LV_BORDER_SIDE_BOTTOM,0);
}
void settingsView() {
    settingsHeader(tr("设置","Settings"));
    settingsList=box(0,layout::header,240,240-layout::header,bg);
    lv_obj_add_flag(settingsList,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_SCROLL_ELASTIC|LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scroll_dir(settingsList,LV_DIR_VER);
    lv_obj_set_scrollbar_mode(settingsList,LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(settingsList,[](lv_event_t *event){
        auto *animation=static_cast<lv_anim_t *>(lv_event_get_param(event));
        if (animation) lv_anim_set_time(animation,200);
    },LV_EVENT_SCROLL_BEGIN,nullptr);
    const Item names[]={{"Wi-Fi","Wi-Fi"},{"立即同步","Sync now"},{"语言","Language"},{"亮度","Brightness"},
                        {"自动休眠","Auto-sleep"},{"存储","Storage"},{"电量","Battery"},{"时钟","Clock"},{"固件","Firmware"}};
    const Item times[]={{"30 秒","30 s"},{"1 分钟","1 min"},{"5 分钟","5 min"},{"从不","Never"}};
    for (int i=0;i<SettingCount;++i) {
        auto *row=settingRows[i]=box(5,layout::settingRow*i,230,layout::settingRow,bg,9,0,0,settingsList);
        String value;
        const char *sub="";
        switch(i) {
        case Network: value=campusOnline()?tr("在线","Online"):tr("离线","Offline"); sub=campusOnline()?"HKUSTGZ":tr("未连接","Disconnected"); break;
        case Sync: sub=tr("尚未同步","Never synced"); break;
        case Language: value=tr("中文","English"); break;
        case Bright: value=String(brightness*5)+"%"; break;
        case AutoSleep: value=tr(times[sleepOption].zh,times[sleepOption].english); break;
        case Storage:
            if (freeBytes<0) value=tr("不可用","Unavailable");
            else if (!freeBytes) value=tr("卡已满","Card full");
            else value=String(freeBytes/1e9,1)+tr(" GB 可用"," GB free");
            break;
        case Battery: break; // Populated together with the header indicator.
        case Clock: sub=" "; break;
        case Firmware: value="Murmur 0.1.0"; break;
        }
        label(10,*sub?4:15,110,tr(names[i].zh,names[i].english),17,ink,LV_TEXT_ALIGN_LEFT,LV_LABEL_LONG_CLIP,row);
        if (*sub) {
            auto *subLabel=label(10,30,145,sub,13,muted,LV_TEXT_ALIGN_LEFT,LV_LABEL_LONG_CLIP,row);
            if (i==Clock) dateLabel=subLabel;
        }
        if (i==Sync) symbol(199,20,19,LV_SYMBOL_RIGHT,row);
        else {
            auto *val=label(99,18,120,value.c_str(),14,i==Storage && freeBytes==0?red:muted,
                            LV_TEXT_ALIGN_RIGHT,LV_LABEL_LONG_DOT,row);
            if (i==Clock) clockLabel=val;
            if (i==Battery) batterySetting=val;
        }
        styleRow(i);
    }
    box(0,SettingCount*layout::settingRow+7,1,1,bg,0,0,0,settingsList);
    lv_obj_update_layout(settingsList);
    lv_obj_scroll_to_y(settingsList,settingsY,LV_ANIM_OFF);
    refreshClock();
}
void changeLanguage() {
    en=!en; prefs.putBool("english",en); dirty=true;
}
void syncNow() {
    if (screen==Screen::Panel) go(panelFrom);
    offlineToast=true; toastStarted=millis(); dirty=true;
}
// A fully depowered unit may need a trusted date before PEAP authentication.
int clockParts[5],clockField;
int clockMaximum(int field) {
    static const int days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    if (field==2) return days[clockParts[1]-1]+(clockParts[1]==2 && clockParts[0]%4==0);
    const int limits[]={2037,12,31,23,59};
    return limits[field];
}
void adjustClock(int step) {
    const int mins[]={2024,1,1,0,0};
    int lo=mins[clockField],hi=clockMaximum(clockField);
    clockParts[clockField]=lo+(clockParts[clockField]-lo+step+hi-lo+1)%(hi-lo+1);
    clockParts[2]=min(clockParts[2],clockMaximum(2));
    dirty=true;
}
void nextClockField() {
    if (clockField<4) {++clockField; dirty=true; return;}
    struct tm t{};
    t.tm_year=clockParts[0]-1900; t.tm_mon=clockParts[1]-1; t.tm_mday=clockParts[2];
    t.tm_hour=clockParts[3]; t.tm_min=clockParts[4];
    timeval tv{mktime(&t),0};
    if (tv.tv_sec>=1704067200 && settimeofday(&tv,nullptr)==0) go(Screen::Settings);
}
void editClock() {
    time_t now=time(nullptr);
    if(now<1704067200) now=max(uint32_t(1704067200),campusClockHint());
    struct tm t{};localtime_r(&now,&t);
    clockParts[0]=constrain(t.tm_year+1900,2024,2037);clockParts[1]=t.tm_mon+1;clockParts[2]=t.tm_mday;
    clockParts[3]=t.tm_hour;clockParts[4]=t.tm_min;clockField=0;go(Screen::SetClock);
}
void clockEditorView() {
    settingsHeader(tr("校时","Set clock"));
    const char *status=campusNetworkTime()?tr("已通过网络校时","Network time received"):
        (campusOnline()?tr("正在网络校时","Waiting for network time"):tr("联网后自动校时","Auto-sync when online"));
    center(31,status,13,muted);
    const char *names[]={tr("年","Year"),tr("月","Month"),tr("日","Day"),tr("时","Hour"),tr("分","Minute")};
    char date[32]; snprintf(date,sizeof(date),"%04d-%02d-%02d  %02d:%02d",clockParts[0],clockParts[1],clockParts[2],clockParts[3],clockParts[4]);
    center(51,date,18);center(91,names[clockField],17,yellow);
    for (const Rect &b : {layout::clockMinus,layout::clockPlus}) box(b.x,b.y,b.w,b.h,surface,7);
    label(14,131,100,"-",20,ink,LV_TEXT_ALIGN_CENTER);label(126,131,100,"+",20,ink,LV_TEXT_ALIGN_CENTER);
    box(layout::clockNext.x,layout::clockNext.y,layout::clockNext.w,layout::clockNext.h,surface,7);
    center(191,clockField==4?tr("保存","Save"):tr("下一项","Next field"),17);
}
void activateSetting(int i) {
    switch(i) {
    case Network: startSavedCampusWifi(); dirty=true; break;
    case Clock: editClock(); break;
    case Sync: syncNow(); break;
    case Language: changeLanguage(); break;
    case Bright: go(Screen::Brightness); break;
    case AutoSleep:
        sleepOption=(sleepOption+1)%4;
        prefs.putUChar("sleep",sleepOption);
        dirty=true;
        break;
    }
}
void pagesView() {
    pageTrack=box(-pageIndex(screen)*pageWidth,0,3*pageWidth,240,bg);
    for (int i=0;i<3;++i) {
        panelRoot=box(i*pageWidth,0,pageWidth,240,bg,0,0,0,pageTrack);
        if (pages[i]==Screen::Today) todayView();
        else {
            bool md=pages[i]==Screen::MD;
            const int size=en?16:17;
            label(12,16-fontFor(size)->line_height/2,150,md?tr("分子动力学","MD simulations"):"Claude",size);
            batteryIcon(230,16);
            if (md) {
                icon(panelRoot,Icon::Atom,40,100,62,muted);
                center(108,tr("即将上线","Coming soon"),18);
                center(137,tr("模拟进度将显示在这里。","Simulation progress will show here."),14,muted);
            } else center(107,tr("暂无用量数据","No usage data yet"),18);
        }
        panelRoot=nullptr;
    }
}
void render() {
    dirty=false;
    if (screen==Screen::Settings && settingsList) settingsY=lv_obj_get_scroll_y(settingsList);
    if (taskList) taskScrollY=lv_obj_get_scroll_y(taskList);
    taskList=nullptr;
    pageLayer=pageTrack=nullptr;
    batteryIconCount=0;
    batterySetting=nullptr;
    draggingPages=false;
    auto *root=lv_scr_act();
    for(int i=int(lv_obj_get_child_cnt(root))-1;i>=0;--i) {
        auto *child=lv_obj_get_child(root,i);
        lv_obj_del(child);
    }
    pageLayer=box(0,0,240,240,bg);
    renderedScreen=screen;
    toast=settingsList=brightnessFill=nullptr;
    overlay=progressFill=timerLabel=countdown=clockLabel=dateLabel=nullptr;
    lv_obj_set_style_bg_color(lv_scr_act(),lv_color_hex(bg),0);
    lv_obj_clear_flag(lv_scr_act(),LV_OBJ_FLAG_SCROLLABLE);
    lastSeconds=UINT32_MAX; lastLevel=255;
    switch(screen) {
    case Screen::Lock: lockView(); break;
    case Screen::Today: case Screen::Claude: case Screen::MD: pagesView(); break;
    case Screen::Picker: pickerView(); break;
    case Screen::Panel: panelView(); break;
    case Screen::Settings: settingsView(); break;
    case Screen::SetClock: clockEditorView(); break;
    case Screen::Brightness:
        settingsHeader(tr("亮度","Brightness"));
        brightnessBar(true);
        center(203,tr("拖动或轻点 · NEXT +5%","Drag or tap · NEXT +5%"),14,muted);
        break;
    case Screen::Recording:
    case Screen::ConfirmDiscard: {
        bool confirm=screen==Screen::ConfirmDiscard;
        recordDot=box(14,17,9,9,red,5);
        label(30,12,100,tr("录音中","REC"),14,red);
        dateLabel=label(125,12,101,"",14,muted,LV_TEXT_ALIGN_RIGHT);
        batteryIcon(226,19);
        refreshClock();
        timerLabel=center(confirm?82:49,"0:00",confirm?34:56);
        lv_obj_set_style_text_letter_space(timerLabel,-1,0);
        if (confirm) {
            center(49,tr("丢弃本次录音？","Discard this recording?"),18);
            center(140,tr("NEXT 选择 · OK 确认","NEXT selects · OK confirms"),14,muted);
        } else {
            for (int i=0;i<20;++i) bars[i]=box(42+8*i,0,5,5,red,2);
            paintWave();
        }
        for (int i=0;i<2;++i) {
            const Rect &b=i?layout::recSave:layout::recCancel;
            const int x=b.x;
            bool selected=confirm && discardSelected==bool(i);
            box(b.x,b.y,b.w,b.h,surface,9,yellow,selected?2:0);
            const char *action=i?tr("结束并保存","Stop & save"):tr("取消","Cancel");
            if (confirm) action=i?tr("丢弃","Discard"):tr("继续录音","Keep recording");
            label(x,confirm?191:180,108,action,confirm&&en?14:17,confirm&&i?red:ink,LV_TEXT_ALIGN_CENTER);
            if (!confirm) label(x,207,108,i?"+":"NEXT",14,muted,LV_TEXT_ALIGN_CENTER);
        }
        break;
    }
    case Screen::Saved: {
        icon(pageLayer,Icon::Saved,56,92,50,ink);
        center(116,tr("已保存","Saved"),20);
        char msg[56]; auto s=recordedSeconds();
        snprintf(msg,sizeof(msg),"%lu:%02lu · %s",(unsigned long)(s/60),(unsigned long)(s%60),
                 tr("已存入 microSD","on microSD"));
        center(146,msg,14,muted);
        center(212,campusOnline()?tr("即将同步","Syncing next"):tr("联网后同步","Will sync when online"),13,muted);
        break;
    }
    case Screen::Error: {
        if (!strcmp(faultEn,"Battery too low")) {
            // Large battery (the header glyph at 3x): outline, terminal and a sliver of charge.
            box(90,47,54,32,bg,8,red,4);
            box(146,59,5,12,red,2);
            box(98,55,max(3,battery*38/100),16,red,3);
        } else icon(pageLayer,Icon::Warn,44,98,40,red);
        center(94,tr(faultZh,faultEn),18);
        const char *detailZh="本次录音未保存。", *detailEn="This recording wasn't saved.";
        if (!strcmp(faultEn,"Battery too low")) { detailZh="请先充电再录音。\n现在录音可能损坏文件。"; detailEn="Charge the device to record.\nRecording now could damage the file."; }
        else if (!strcmp(faultEn,"Task save failed") || !strcmp(faultEn,"Discard failed. Check SD.")) detailZh=detailEn="";
        if (*detailEn) center(124,tr(detailZh,detailEn),14,muted);
        center(212,tr("按 OK 关闭","OK to dismiss"),13,muted);
        break;
    }
    case Screen::Sleep: break;
    case Screen::ReleasePower: center(100,tr("松开关机","Release to power off"),18); break;
    case Screen::UnplugPower: center(100,tr("拔下 USB 后关机","Unplug USB to power off"),18); break;
    }
    refreshBatteryWidgets();
    if (pageMotion==Motion::EdgeLeft || pageMotion==Motion::EdgeRight) bounce(pageLayer,pageMotion==Motion::EdgeLeft?-1:1);
    else if (pageMotion!=Motion::None) motionEnds=millis()+enter(pageLayer,pageMotion);
    pageMotion=Motion::None; slideDirection=0;
    if (offlineToast) {
        // Prototype toast: an ink pill 82 px above the bottom edge.
        const char *text=campusOnline()?tr("尚未同步","Never synced"):tr("离线","Offline");
        const int w=lv_txt_get_width(text,strlen(text),fontFor(14),0,LV_TEXT_FLAG_NONE)+24, h=fontFor(14)->line_height+10;
        toast=box((240-w)/2,240-82-h,w,h,ink,h/2);
        label(0,5,w,text,14,bg,LV_TEXT_ALIGN_CENTER,LV_LABEL_LONG_CLIP,toast);
        fade(toast,LV_OPA_TRANSP,160);
    }
}
void saveChange(bool saved) {
    if (!saved) { faultZh="任务保存失败"; faultEn="Task save failed"; go(Screen::Error); }
    dirty=true;
}
void choose() {
    int ids[model::maxTasks],n=tasks(false,ids);
    if (!n) return;
    int id=ids[ticket%n];
    showCompleted=false; revealFocus=true;
    go(Screen::Today);
    saveChange(model::moveToToday(id));
    int today[model::maxTasks];
    int count=tasks(true,today);
    for (int i=0;i<count;++i) if (today[i]==id) focused=i;
    ticket=0;
}
void browse(int step) {
    slideDirection=step;
    if (screen==Screen::Lock) note=(note+step+model::noteCount())%model::noteCount();
    else {
        int ids[model::maxTasks],n=tasks(false,ids);
        if (!n) return;
        ticket=(ticket+step+n)%n;
    }
    interacted=rotated=millis();
    dirty=true;
}
void shortKey(int pin) {
    if (pin==plusPin) { recordKey(); return; }
    bool ok=pin==okPin;
    int ids[model::maxTasks],later[model::maxTasks],n=tasks(true,ids),k=tasks(false,later);
    interacted=millis();
    switch(screen) {
    case Screen::Recording: if (!ok) askDiscard(); break;
    case Screen::ConfirmDiscard:
        if (ok) resolveDiscard(discardSelected);
        else { discardSelected=!discardSelected; dirty=true; }
        break;
    case Screen::Sleep: if (!ok) go(Screen::Lock); break;
    case Screen::Lock: if (ok) go(Screen::Today); else browse(1); break;
    case Screen::Today:
        if (!ok && n+(k>0)>0) { focused=(focused+1)%(n+(k>0)); revealFocus=true; rotated=interacted; dirty=true; }
        else if (focused<n) saveChange(model::toggleDone(ids[focused]));
        else if (k) go(Screen::Picker);
        break;
    case Screen::Picker: if (ok) choose(); else browse(1); break;
    case Screen::Panel: go(panelFrom); break;
    case Screen::Settings:
        if (!settingsList) break;
        if (ok) activateSetting(settingFocus);
        else {
            int old=settingFocus;
            settingFocus=(settingFocus+1)%SettingCount;
            styleRow(old); styleRow(settingFocus);
            lv_obj_scroll_to_view(settingRows[settingFocus],LV_ANIM_ON);
        }
        break;
    case Screen::Brightness:
        if (ok) go(Screen::Settings);
        else { setBrightnessLevel(brightness%20+1); storeBrightness(); }
        break;
    case Screen::SetClock: if (ok) nextClockField(); else adjustClock(1); break;
    case Screen::Claude: case Screen::MD: go(Screen::Today); break;
    case Screen::Error: if (ok) go(sleepAfter?Screen::Sleep:Screen::Today); break;
    default: break;
    }
}
bool holdTarget(int pin,Screen &target) {
    if (pin==plusPin) return false;
    if (screen==Screen::Picker && pin==okPin) target=Screen::Today;
    else if (screen==Screen::Lock) target=Screen::Sleep;
    else if (screen==Screen::Today || screen==Screen::Claude || screen==Screen::MD)
        target=pin==okPin?Screen::Lock:Screen::Sleep;
    else if (screen==Screen::Panel) target=pin==okPin?panelFrom:Screen::Sleep;
    else if (screen==Screen::Settings) target=pin==okPin?Screen::Panel:Screen::Sleep;
    else if (screen==Screen::Brightness || screen==Screen::SetClock) target=pin==okPin?Screen::Settings:Screen::Sleep;
    else return false;
    return true;
}
void holdOverlay(Screen target,int percent) {
    if (!overlay) {
        overlay=box(0,0,240,240,0);
        lv_obj_set_style_bg_opa(overlay,185,0);
        const char *s=tr("返回","Back");
        Icon glyph=Icon::Back;
        if (target==Screen::Sleep) { s=tr("休眠","Sleep"); glyph=Icon::Moon; }
        if (target==Screen::Lock) { s=tr("锁屏","Lock"); glyph=Icon::Lock; }
        // Prototype .hob: icon, label and bar stacked with 6 px gaps, centred.
        icon(overlay,glyph,32,104,83,ink);
        label(0,121,240,s,20,ink,LV_TEXT_ALIGN_CENTER,LV_LABEL_LONG_CLIP,overlay);
        box(70,152,100,5,0x3a3e45,3,0,0,overlay);
        progressFill=box(70,152,1,5,ink,3,0,0,overlay);
    }
    lv_obj_set_width(progressFill,max(1,percent));
}
struct Key { int pin; bool raw=false, down=false, used=false; uint32_t changed=0, start=0;
    explicit Key(int p):pin(p){} };
Key keys[]={Key(plusPin),Key(nextPin),Key(okPin)};
void preparePowerOff() {
    if (recordingActive()) {
        auto result=finishRecording();
        if (result!=AudioResult::Ok) { audioError(result); return; }
    }
    stopCampusWifi();
    go(Screen::ReleasePower);
}
void pollKeys(uint32_t now) {
    for (auto &k:keys) {
        if (screen==Screen::UnplugPower || (screen==Screen::ReleasePower && k.pin!=okPin)) continue;
        bool raw=digitalRead(k.pin)==LOW;
        if (raw!=k.raw) { k.raw=raw; k.changed=now; }
        if (now-k.changed>=24 && k.down!=raw) {
            k.down=raw;
            lastInput=now;
            if (k.pin==okPin) powerHoldStarted=raw;
            if (raw) { k.start=now; k.used=false; }
            else if (screen==Screen::ReleasePower && k.pin==okPin) {
                go(Screen::UnplugPower);
                releaseBatteryPower();
            } else if (!k.used) {
                if (overlay) { lv_obj_del(overlay); overlay=nullptr; }
                else shortKey(k.pin);
            }
        }
        Screen target;
        if (k.pin==okPin && powerHoldStarted && raw && k.down && now-k.start>=3000) {
            powerHoldStarted=false;
            k.used=true;
            preparePowerOff();
            continue;
        }
        if (k.down && !k.used && holdTarget(k.pin,target) && now-k.start>=250) {
            if (now-k.start>=800) { k.used=true; go(target); }
            else holdOverlay(target,(now-k.start-250)*100/550);
        }
    }
}
void gesture(int x,int y,int dx,int dy) {
    const bool horizontal=abs(dx)>12 && abs(dx)>abs(dy);
    const bool tap=abs(dx)<=12 && abs(dy)<=12;
    int step=dx<0?1:-1, ids[model::maxTasks], later[model::maxTasks], n=tasks(true,ids), k=tasks(false,later);
    interacted=millis();
    if (recordingActive()) {
        const bool save=layout::recSave.contains(x,y);
        if (!tap || (!save && !layout::recCancel.contains(x,y))) return;
        if (screen==Screen::ConfirmDiscard) resolveDiscard(save);
        else if (save) recordKey();
        else askDiscard();
    } else if (y<layout::header && dy>=30 && dy>abs(dx) &&
        (screen==Screen::Today || screen==Screen::Claude || screen==Screen::MD)) {
        panelFrom=screen; go(Screen::Panel);
    } else if (screen==Screen::Panel) {
        if (-dy>=30 && -dy>abs(dx)) go(panelFrom);
        else if (tap && layout::syncTile.contains(x,y)) syncNow();
        else if (tap && layout::languageTile.contains(x,y)) changeLanguage();
        else if (tap && layout::allSettings.contains(x,y)) {
            settingFocus=0; settingsY=0; freeBytes=cardFreeBytes(); go(Screen::Settings);
        }
    } else if (screen==Screen::Settings) {
        if (dx>=40 && horizontal) go(Screen::Panel);
        else if (tap && layout::backTap.contains(x,y)) go(Screen::Panel);
        else if (tap && settingsList && y>=layout::header) {
            int pos=y-layout::header+lv_obj_get_scroll_y(settingsList);
            if (pos>=0 && pos<SettingCount*layout::settingRow) {
                int old=settingFocus;
                settingFocus=pos/layout::settingRow;
                styleRow(old); styleRow(settingFocus);
                activateSetting(settingFocus);
            }
        }
    } else if (screen==Screen::SetClock && tap) {
        if (layout::backTap.contains(x,y)) go(Screen::Settings);
        else if (layout::clockMinus.contains(x,y)) adjustClock(-1);
        else if (layout::clockPlus.contains(x,y)) adjustClock(1);
        else if (layout::clockNext.contains(x,y)) nextClockField();
    } else if (screen==Screen::Brightness) {
        if (tap && layout::backTap.contains(x,y)) go(Screen::Settings);
    } else if (screen==Screen::Lock) {
        if (-dy>=60 && -dy>abs(dx)) go(Screen::Today);
        else if (horizontal && abs(dx)>=24) browse(step);
    } else if (screen==Screen::Today && y>=layout::strip) {
        if (k && (horizontal || tap)) { focused=n; if (horizontal) browse(step); else go(Screen::Picker); }
    } else if (screen==Screen::Today && tap && layout::countTap.contains(x,y)) {
        showCompleted=!showCompleted; focused=0;
        if(taskList) lv_obj_scroll_to_y(taskList,0,LV_ANIM_OFF);
        taskScrollY=0; dirty=true;
    } else if (screen==Screen::Picker) {
        if (horizontal) browse(step);
        else if (tap && layout::pickCancel.contains(x,y)) go(Screen::Today);
        else if (tap && layout::pickMove.contains(x,y)) choose();
    } else if (horizontal && (screen==Screen::Today || screen==Screen::Claude || screen==Screen::MD)) {
        int page=pageIndex(screen);
        if (page+step<0 || page+step>2) { pageMotion=step>0?Motion::EdgeLeft:Motion::EdgeRight; dirty=true; }
        else go(pages[page+step]);
    } else if (tap && screen==Screen::Today && taskList && layout::taskList.contains(x,y)) {
        int pos=y-layout::taskList.y+lv_obj_get_scroll_y(taskList);
        if (pos>=0 && pos%layout::rowPitch<layout::rowHeight && pos/layout::rowPitch<n) { focused=pos/layout::rowPitch; shortKey(okPin); }
    }
}
void pollTouch(uint32_t now) {
    static uint32_t polled,lastSeen;
    static int startX,startY,x,y;
    static bool tracking,listTouch,slider,moved;
    static bool pageTouch,verticalDrag;
    static int trackStart;
    static Screen from;
    if (screen==Screen::Sleep || overlay) {
        if (draggingPages && pageTrack) settlePage(pageIndex(screen));
        tracking=settingsTouch=false; return;
    }
    if (now-polled<10) return;
    polled=now;
    int tx,ty;
    Contact state=readContact(tx,ty);
    if (state==Contact::Pressed) {
        if (!tracking) {
            startX=tx; startY=ty; tracking=true; from=screen;
            listTouch=screen==Screen::Today && layout::taskList.contains(tx,ty);
            pageTouch=pageTrack && from==renderedScreen && (from!=Screen::Today || ty<layout::strip);
            verticalDrag=false;
            if (pageTouch) {
                trackStart=lv_obj_get_style_x(pageTrack,0);
                draggingPages=lv_anim_del(pageTrack,positionTrack);
            }
            slider=(screen==Screen::Panel && layout::panelBrightness.contains(tx,ty)) ||
                   (screen==Screen::Brightness && layout::screenBrightness.contains(tx,ty));
            moved=screen==Screen::Settings && settingsList && lv_obj_is_scrolling(settingsList);
            if (listTouch && taskList && lv_obj_is_scrolling(taskList)) moved=true;
        }
        x=tx; y=ty; lastInput=lastSeen=now;
        settingsTouch=screen==from && ((from==Screen::Settings && startY>=layout::header) || listTouch);
        settingsPoint={lv_coord_t(x),lv_coord_t(y)};
        int dx=x-startX, dy=y-startY;
        if (pageTouch && pageTrack && from==screen && !verticalDrag) {
            if (!draggingPages && abs(dy)>=8 && abs(dy)>=abs(dx)) verticalDrag=true;
            else if (draggingPages || (abs(dx)>=8 && abs(dx)>abs(dy))) {
                draggingPages=true;
                settingsTouch=false;
                int offset=trackStart+dx;
                const int end=-2*pageWidth;
                if (offset>0) offset=min(14,offset/4);
                else if (offset<end) offset=end-min(14,(end-offset)/4);
                positionTrack(pageTrack,offset);
                return;
            }
        }
        if (abs(x-startX)>=6 || abs(y-startY)>=6) moved=true;
        if (slider && screen==from) {
            const Rect &bar=screen==Screen::Panel?layout::panelBrightness:layout::screenBrightness;
            setBrightnessLevel(((x-bar.x)*20+bar.w-1)/bar.w);
        }
    } else if (tracking && (state==Contact::Released || now-lastSeen>100)) {
        tracking=settingsTouch=false;
        if (from!=screen) return;
        if (draggingPages && pageTrack) {
            int dx=x-startX, index=pageIndex(from);
            if (state==Contact::Released && abs(dx)>=24) {
                int offset=lv_obj_get_style_x(pageTrack,0);
                // Advance from the page visible under the finger, even when a
                // second swipe catches the preceding settle in mid-flight.
                index=dx<0?(-offset+pageWidth-1)/pageWidth:-offset/pageWidth;
            }
            settlePage(constrain(index,0,2));
            return;
        }
        if (recordingActive() && (state!=Contact::Released || moved)) return;
        if (slider) { storeBrightness(); return; }
        if (screen==Screen::Settings && moved && !(x-startX>=40 && x-startX>abs(y-startY))) return;
        if (listTouch && moved && abs(x-startX)<=max(12,abs(y-startY))) return;
        if (verticalDrag && abs(x-startX)>abs(y-startY)) return;
        gesture(startX,startY,x-startX,y-startY);
    }
}
void updateView(uint32_t now) {
    int ids[model::maxTasks],n=tasks(true,ids);
    bool idleScreen=screen==Screen::Lock || screen==Screen::Today || screen==Screen::MD ||
                    screen==Screen::Claude || screen==Screen::Panel || screen==Screen::Settings ||
                    screen==Screen::Brightness;
    if (idleScreen && idleLimits[sleepOption] && now-lastInput>=idleLimits[sleepOption]) go(Screen::Sleep);
    if (offlineToast && now-toastStarted>=3000) {
        offlineToast=false;
        if (toast) { lv_obj_del(toast); toast=nullptr; }
    }
    if (screen==Screen::Saved && now-entered>=2000) go(sleepAfter?Screen::Sleep:Screen::Today);
    if (screen==Screen::Lock && now-rotated>=6000 && !overlay) browse(1);
    if (screen==Screen::Picker && now-interacted>=8000 && !overlay) go(Screen::Today);
    if (screen==Screen::Today && !overlay) {
        if (focused==n && now-interacted>=10000) { focused=0; rotated=now; dirty=true; }
        else if (!dirty && focused!=n && now-rotated>=scrollPeriod) {
            int later[model::maxTasks], count=tasks(false,later);
            if (count>1) { ++ticket; slideDirection=1; updateTicker(later,count); slideDirection=0; }
            rotated=now;
        }
    }
    if (dirty && (renderedScreen!=screen || (!draggingPages && int32_t(now-motionEnds)>=0))) render();
    if (recordingActive()) {
        unsigned s=recordedSeconds();
        if (lastSeconds!=s) {
            showDuration(timerLabel,s); lastSeconds=s;
            lv_obj_set_style_opa(recordDot,s%2?LV_OPA_40:LV_OPA_COVER,0);
        }
        if (screen==Screen::Recording && now-waveSampled>=70) {
            waveSampled=now;
            memmove(wave,wave+1,sizeof(wave)-1);
            wave[19]=audioLevel();
            paintWave();
        }
    }
    if (countdown) {
        unsigned s=8-(now-interacted)/1000;
        if (lastSeconds!=s) {
            char text[60]; snprintf(text,sizeof(text),tr("%u 秒后自动取消","Cancels by itself in %us"),s);
            lv_label_set_text(countdown,text); lastSeconds=s;
        }
    }
    bool sample=charging();
    if (sample!=chargeSample) {
        chargeSample=sample; chargeChangedAt=now;
    }
    bool chargeChanged=sample!=isCharging && now-chargeChangedAt>=120;
    if (chargeChanged) isCharging=sample;
    bool levelChanged=false;
    if (now-lastBattery>=30000 && !recordingActive() && !overlay) {
        lastBattery=now;
        int b=batteryPercent();
        levelChanged=b!=battery;
        battery=b;
        refreshClock();
    }
    if (chargeChanged || levelChanged) refreshBatteryWidgets();
}
// Display refresh rate over the last second and the slowest loop since the last `perf`.
uint32_t measuredFps, slowestLoop;
void measurePerformance(uint32_t now,uint32_t loopStart) {
    static uint32_t windowStart, framesAtStart;
    slowestLoop=max(slowestLoop,now-loopStart);
    if (now-windowStart<1000) return;
    const uint32_t frames=displayFrames();
    measuredFps=(frames-framesAtStart)*1000/(now-windowStart);
    windowStart=now; framesAtStart=frames;
}
void serialSettings() {
    static char buf[288];
    static bool overflow=false;
    static unsigned len=0;
    while (usbSerial.available()) {
        char ch=usbSerial.read();
        if (ch=='\r') continue;
        if (ch!='\n') { if (len<sizeof(buf)-1) buf[len++]=ch; else overflow=true; continue; }
        buf[len]=0; len=0;
        if (overflow) { usbSerial.println("error input too long"); overflow=false; }
        else if (!strncmp(buf,"wifi ",5)) {
            campusCommand(buf+5,usbSerial);
            if (screen==Screen::Settings || screen==Screen::Panel) dirty=true;
        } else if (!strcmp(buf,"en") || !strcmp(buf,"zh")) {
            en=!strcmp(buf,"en"); prefs.putBool("english",en); dirty=true;
        } else if (!strcmp(buf,"perf")) {
            usbSerial.printf("perf fps=%lu loop_max_ms=%lu\n",(unsigned long)measuredFps,(unsigned long)slowestLoop);
            slowestLoop=0;
        } else if (!strcmp(buf,"levels")) {
            printMicLevels(usbSerial);
        } else if (!strncmp(buf,"time ",5)) {
            long seconds=atol(buf+5);
            if (seconds>=1704067200) { timeval t{seconds,0}; settimeofday(&t,nullptr); dirty=true; }
        }
        memset(buf,0,sizeof(buf));
    }
}
}

void setup() {
    beginCampusDiagnostics();
    boardBegin();
    static lv_indev_drv_t touchDriver;
    lv_indev_drv_init(&touchDriver);
    touchDriver.type=LV_INDEV_TYPE_POINTER;
    touchDriver.scroll_limit=6;
    touchDriver.scroll_throw=6;
    touchDriver.read_cb=[](lv_indev_drv_t *,lv_indev_data_t *d){
        d->state=settingsTouch?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;
        d->point=settingsPoint;
    };
    lv_indev_drv_register(&touchDriver);
    lv_timer_set_period(touchDriver.read_timer,16);
    lv_anim_init(&scrollAnimation);
    lv_anim_set_delay(&scrollAnimation,1000);
    beginCardUsb(usbSerial);
    prefs.begin("murmur-ui");
    en=prefs.getBool("english",false);
    savedBrightness=brightness=constrain(int(prefs.getUChar("brightness",16)),1,20);
    sleepOption=constrain(int(prefs.getUChar("sleep",1)),0,3);
    model::load();
    battery=batteryPercent();
    isCharging=chargeSample=charging();
    chargeChangedAt=millis();
    setenv("TZ","CST-8",1); tzset();
    for (auto &k:keys) { k.used=k.down=k.raw=(digitalRead(k.pin)==LOW); }
    go(Screen::Lock);
    startSavedCampusWifi();
    // A hung loop otherwise needs a power cut: the PWR gesture runs in loop().
    // Reboot instead. `wifi diag` then reports where Wi-Fi code was.
    esp_task_wdt_init(15,true);
    enableLoopWDT();
}
void loop() {
    uint32_t now=millis();
    const uint32_t loopStart=now;
    lv_tick_inc(now-lastTick); lastTick=now;
    // Breadcrumb codes for `wifi diag`: see docs/campus-wifi.md.
    diagLoopPhase(1);
    if (recordingActive()) {
        auto result=serviceRecording();
        if (result!=AudioResult::Ok) audioError(result);
    }
    diagLoopPhase(2);
    pollKeys(now);
    if (screen==Screen::ReleasePower || screen==Screen::UnplugPower) {
        if (dirty) render();
        lv_timer_handler();
        delay(5);
        return;
    }
    now=millis();
    diagLoopPhase(3);
    pollTouch(now);
    diagLoopPhase(4);
    serialSettings();
    diagLoopPhase(5);
    if (serviceCampusWifi()) {
        refreshClock();
        if (screen==Screen::Settings || screen==Screen::Panel || screen==Screen::SetClock) dirty=true;
    }
    diagLoopPhase(6);
    updateView(millis());
    diagLoopPhase(7);
    lv_timer_handler();
    diagLoopPhase(8);
    measurePerformance(millis(),loopStart);
    delay(screen==Screen::Sleep?10:2);
}
