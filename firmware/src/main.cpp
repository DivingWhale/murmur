#include "audio.h"
#include "board.h"
#include "card_usb.h"
#include "campus_wifi.h"
#include "campus_profile.h"
#include <cstring>
#include <USB.h>
#include "fonts.h"
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <sys/time.h>
#include <time.h>

namespace {
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
enum class Motion { None, Left, Right, Up, Down, EdgeLeft, EdgeRight };
Motion pageMotion=Motion::None;
Screen renderedScreen=Screen::Lock;
uint32_t motionEnds=0;
lv_point_t settingsPoint;
lv_obj_t *settingsList, *settingRows[SettingCount], *brightnessFill, *brightPercent, *filledPercent, *toast;
lv_obj_t *panelRoot, *pageLayer;
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
constexpr uint32_t bg=0x0b0c0e, surface=0x1c1f24, line=0x262a30, ink=0xf4f3ef,
    muted=0x9ca0a8, dim=0x6b7079, yellow=0xffd23f, red=0xff5c4d, due=0xffb067;
Preferences prefs;
bool en, sleepAfter, dirty=true;
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
uint16_t taskState; // Bits 0..5 completed, bits 8..13 promoted into Today.
const char *faultZh="", *faultEn="";
lv_obj_t *timerLabel, *recordDot, *bars[20], *countdown, *clockLabel, *dateLabel;
lv_obj_t *overlay, *progressFill;
lv_obj_t *tickerTitle, *tickerChip, *tickerDots[6];
lv_anim_t scrollAnimation;
unsigned lastSeconds;
uint8_t lastLevel;
const char *tr(const char *zh,const char *english) { return en ? english : zh; }
struct Item { const char *zh, *english; };
const Item titles[] = {
    {"给魏老师发邮件约实验室时间","Email Dr. Weber about lab slot"},
    {"还图书馆的书","Return library books"},{"买咖啡滤纸","Buy coffee filters"},
    {"完成报告初稿","Finish report draft"},{"备份照片","Back up photos"},
    {"读研讨课第四章","Read chapter 4 for seminar"}
};
const Item excerpts[] = {
    {"组会上终于把实验设计讲清楚了。准备充分时，其实没那么紧张。",
     "The best ideas this week came when I wasn't at the desk. Walk first, then write."},
    {"散步的时候想明白了：先把问题写下来，答案常常就在问题里。",
     "Asking one honest question in the seminar was easier than staying quiet."}
};
const char *title(int id) { return tr(titles[id].zh,titles[id].english); }
int tasks(bool today,int *ids) {
    int n=0;
    for (int i=0;i<6;++i)
        if ((i<3 || bool(taskState & (1u<<(8+i)))) == today &&
            (!today || bool(taskState & (1u<<i))==showCompleted)) ids[n++]=i;
    return n;
}
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
    screen=target;
    lastInput=entered=interacted=rotated=millis();
    dirty=true;
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
        if (result==AudioResult::Ok) { go(Screen::Saved); startSavedCampusWifi(); } // Upload point once the Pi sync exists.
        else audioError(result);
        return;
    }
    sleepAfter=screen==Screen::Sleep;
    battery=batteryPercent();
    if (battery>=0 && battery<=3 && !charging()) {
        showError("电量过低，请先充电","Battery too low to record");
        return;
    }
    auto result=startRecording();
    if (result==AudioResult::Ok) { recordingFrom=screen; go(Screen::Recording); }
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
lv_obj_t *box(int x,int y,int w,int h,uint32_t color,int radius=0,uint32_t border=0,int thickness=0,
              lv_obj_t *parent=nullptr) {
    auto *o=lv_obj_create(parent ? parent : (panelRoot?panelRoot:(pageLayer?pageLayer:lv_scr_act())));
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o,x,y); lv_obj_set_size(o,w,h);
    lv_obj_set_style_bg_color(o,lv_color_hex(color),0);
    lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);
    lv_obj_set_style_radius(o,radius,0);
    if (thickness) {
        lv_obj_set_style_border_color(o,lv_color_hex(border),0);
        lv_obj_set_style_border_width(o,thickness,0);
    }
    return o;
}
lv_obj_t *label(int x,int y,int width,const char *s,int size,uint32_t color=ink,
                lv_text_align_t align=LV_TEXT_ALIGN_LEFT,lv_label_long_mode_t mode=LV_LABEL_LONG_CLIP,
                lv_obj_t *parent=nullptr) {
    auto *o=lv_label_create(parent ? parent : (panelRoot?panelRoot:(pageLayer?pageLayer:lv_scr_act())));
    lv_obj_set_style_text_font(o,fontFor(size),0);
    lv_obj_set_style_text_color(o,lv_color_hex(color),0);
    lv_obj_set_style_text_align(o,align,0);
    lv_label_set_long_mode(o,mode);
    lv_obj_set_width(o,width);
    if (mode!=LV_LABEL_LONG_WRAP) lv_obj_set_height(o,fontFor(size)->line_height);
    lv_label_set_text(o,s);
    lv_obj_set_pos(o,x,y);
    return o;
}
lv_obj_t *center(int y,const char *s,int size,uint32_t color=ink) {
    return label(8,y,224,s,size,color,LV_TEXT_ALIGN_CENTER,LV_LABEL_LONG_WRAP);
}
void dots(int x,int y,int count,int selected) {
    for (int i=0;i<count;++i) box(x+i*9,y,5,5,i==selected?ink:dim,3);
}
constexpr int batteryIconWidth=76, batteryIconHeight=26;
void paintBatteryIcon(lv_obj_t *icon) {
    lv_obj_clean(icon);
    uint32_t color=isCharging || battery<0 || battery>20?muted:(battery<=10?red:due);
    char text[8]="--";
    if (battery>=0) snprintf(text,sizeof(text),"%d%%",max(battery>0?1:0,(battery+2)/5*5));
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
void translate(lv_obj_t *obj,int start,int end,bool vertical,int ms) {
    lv_anim_t a;
    lv_anim_init(&a); lv_anim_set_var(&a,obj);
    if (vertical) lv_anim_set_exec_cb(&a,[](void *p,int32_t v){lv_obj_set_style_translate_y(static_cast<lv_obj_t*>(p),v,0);});
    else lv_anim_set_exec_cb(&a,[](void *p,int32_t v){lv_obj_set_style_translate_x(static_cast<lv_obj_t*>(p),v,0);});
    lv_anim_set_values(&a,start,end); lv_anim_set_time(&a,ms);
    lv_anim_set_path_cb(&a,lv_anim_path_ease_out); lv_anim_start(&a);
}
void animateNote(lv_obj_t *obj) {
    if (slideDirection) {
        translate(obj,28*slideDirection,0,false,280);
        lv_obj_fade_in(obj,280,0);
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
    auto &n=excerpts[note];
    int noteTop=top+headerHeight+10;
    auto *body=label(14,noteTop,212,tr(n.zh,n.english),en?17:18,ink,LV_TEXT_ALIGN_LEFT,LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(body,en?2:4,0);
    lv_obj_update_layout(body);
    const char *datesZh[]={"11月16日 20:10","11月17日 07:30"};
    const char *datesEn[]={"12 Nov · 18:04","15 Nov · 09:22"};
    auto *stamp=label(14,noteTop+6+lv_obj_get_height(body),212,en?datesEn[note]:datesZh[note],14,muted);
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
    int id=later[ticket], width=id==3?145:210;
    lv_obj_set_width(tickerTitle,width);
    lv_label_set_text(tickerTitle,title(id));
    if (id==3) lv_obj_clear_flag(tickerChip,LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(tickerChip,LV_OBJ_FLAG_HIDDEN);
    for (int i=0;i<count;++i)
        lv_obj_set_style_bg_color(tickerDots[i],lv_color_hex(i==ticket?ink:dim),0);
    int textWidth=lv_txt_get_width(title(id),strlen(title(id)),fontFor(en?16:17),0,LV_TEXT_FLAG_NONE);
    scrollPeriod=textWidth<=width?3000:1000+(textWidth+40)*1000/30;
    animateNote(tickerTitle); animateNote(tickerChip);
}
void todayView() {
    int ids[6],later[6],n=tasks(true,ids),k=tasks(false,later);
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
    auto *previousParent=panelRoot;
    taskList=box(0,32,240,132,bg);
    lv_obj_add_flag(taskList,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scroll_dir(taskList,LV_DIR_VER);
    lv_obj_set_scrollbar_mode(taskList,LV_SCROLLBAR_MODE_OFF);
    panelRoot=taskList;
    if (!n) center(47,showCompleted?tr("暂无已完成任务","No completed tasks"):tr("全部完成","All clear"),17,muted);
    for (int i=0;i<n;++i) {
        int id=ids[i],y=i*63;
        bool focus=focused==i, done=taskState & (1<<id);
        if (focus) box(5,y,230,60,surface,9,yellow,2);
        box(15,y+20,20,20,done?muted:(focus?surface:bg),10,done?muted:dim,2);
        if (done) {
            auto *tick=label(14,y+20,22,LV_SYMBOL_OK,14,bg,LV_TEXT_ALIGN_CENTER);
            lv_obj_set_style_text_font(tick,LV_FONT_DEFAULT,0);
        }
        const char *meta=id==1?tr("延续","Carried over"):(id==3?tr("周五截止","Due Fri"):"");
        scrollTitle(45,y+(*meta?8:19),178,title(id),focus,done);
        if (*meta) label(45,y+33,178,meta,14,id==3?due:muted);
    }
    panelRoot=previousParent;
    lv_obj_update_layout(taskList);
    lv_obj_scroll_to_y(taskList,taskScrollY,LV_ANIM_OFF);
    if (revealFocus && focused<n) {
        int top=focused*63;
        int offset=constrain(int(lv_obj_get_scroll_y(taskList)),max(0,top+60-132),top);
        lv_obj_scroll_to_y(taskList,offset,LV_ANIM_ON);
    }
    revealFocus=false;
    box(0,164,240,1,line);
    bool pause=focused==n;
    if (pause) box(5,170,230,65,surface,9,yellow,2);
    label(14,179,183,pause?tr("已暂停 · 按 OK 选择","PAUSED · OK TO CHOOSE"):
                        tr("有空再做","WHEN I HAVE TIME"),13,pause?yellow:muted);
    if (!k) { label(14,201,210,tr("暂无任务","Nothing waiting"),17,dim); return; }
    for (int i=0;i<k;++i) tickerDots[i]=box(226-9*k+9*i,185,5,5,dim,3);
    tickerTitle=scrollTitle(14,200,210,"",true);
    tickerChip=box(166,200,60,23,0x30251c,8);
    label(0,2,60,tr("周五截止","Due Fri"),13,due,LV_TEXT_ALIGN_CENTER,LV_LABEL_LONG_CLIP,tickerChip);
    updateTicker(later,k);
}
void pickerView() {
    int ids[6],n=tasks(false,ids);
    if (!n) { go(Screen::Today); return; }
    ticket%=n;
    center(18,tr("移到今天？","MOVE TO TODAY?"),14,yellow);
    animateNote(center(53,title(ids[ticket]),20));
    dots(118-(n-1)*4,111,n,ticket);
    if (ids[ticket]==3) center(85,tr("周五截止","Due Fri"),14,due);
    box(8,130,108,72,surface,9);
    box(124,130,108,72,yellow,9);
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
void symbol(int x,int y,int w,const char *glyph,lv_obj_t *parent=nullptr) {
    auto *o=label(x,y,w,glyph,14,ink,LV_TEXT_ALIGN_CENTER,LV_LABEL_LONG_CLIP,parent);
    lv_obj_set_style_text_font(o,LV_FONT_DEFAULT,0);
}
void settingsHeader(const char *title) {
    symbol(8,9,12,LV_SYMBOL_LEFT);
    label(25,5,126,title,17);
    batteryIcon(230,16);
}
void paintBrightness() {
    const bool big=screen==Screen::Brightness;
    int pct=brightness*5;
    lv_obj_set_width(brightnessFill,(big?212:220)*pct/100);
    lv_label_set_text_fmt(brightPercent,"%d%%",pct);
    if (!big) lv_label_set_text_fmt(filledPercent,"%d%%",pct);
}
void brightnessBar(bool big) {
    int w=big?212:220,h=big?72:52;
    auto *bar=box(big?14:10,big?108:116,w,h,0x2a2d33,12);
    lv_obj_set_style_clip_corner(bar,true,0);
    // Draw labels on both backgrounds. The fill clips the dark copies at its right edge.
    label(10,(h-24)/2,30,"☀",20,muted,LV_TEXT_ALIGN_LEFT,LV_LABEL_LONG_CLIP,bar);
    if (!big) brightPercent=label(w-72,(h-22)/2,60,"",17,ink,LV_TEXT_ALIGN_RIGHT,LV_LABEL_LONG_CLIP,bar);
    brightnessFill=box(0,0,1,h,ink,0,0,0,bar);
    label(10,(h-24)/2,30,"☀",20,bg,LV_TEXT_ALIGN_LEFT,LV_LABEL_LONG_CLIP,brightnessFill);
    if (big) brightPercent=center(47,"",40);
    else filledPercent=label(w-72,(h-22)/2,60,"",17,bg,LV_TEXT_ALIGN_RIGHT,LV_LABEL_LONG_CLIP,brightnessFill);
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
    symbol(10,14,15,LV_SYMBOL_WIFI);
    // Diagonal stroke distinguishes an offline radio.
    static const lv_point_t slash[]={{0,0},{14,14}};
    if (!campusOnline()) {
        auto *strike=lv_line_create(panel);
        lv_line_set_points(strike,slash,2);
        lv_obj_set_style_line_color(strike,lv_color_hex(dim),0);
        lv_obj_set_style_line_width(strike,2,0);
        lv_obj_set_pos(strike,10,12);
    }
    label(30,12,115,campusOnline()?"HKUSTGZ":tr("未连接","Disconnected"),14);
    label(145,13,85,campusOnline()?tr("在线","Online"):tr("离线","Offline"),13,muted,LV_TEXT_ALIGN_RIGHT);
    box(10,42,106,66,surface,12);
    box(124,42,106,66,surface,12);
    symbol(10,52,106,LV_SYMBOL_REFRESH);
    label(10,78,106,tr("立即同步","Sync now"),14,ink,LV_TEXT_ALIGN_CENTER);
    label(124,48,106,tr("中","EN"),20,ink,LV_TEXT_ALIGN_CENTER);
    label(124,78,106,tr("语言","Language"),14,ink,LV_TEXT_ALIGN_CENTER);
    brightnessBar(false);
    box(10,176,220,48,surface,12);
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
    settingsList=box(0,32,240,208,bg);
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
        auto *row=settingRows[i]=box(5,56*i,230,56,bg,9,0,0,settingsList);
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
    box(0,SettingCount*56+7,1,1,bg,0,0,0,settingsList);
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
    box(14,126,100,42,surface,7); box(126,126,100,42,surface,7);
    label(14,131,100,"-",20,ink,LV_TEXT_ALIGN_CENTER);label(126,131,100,"+",20,ink,LV_TEXT_ALIGN_CENTER);
    box(14,181,212,42,surface,7);
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
            label(12,5,150,md?tr("分子动力学","MD simulations"):"Claude",17);
            batteryIcon(230,16);
            center(100,md?tr("即将上线","Coming soon"):tr("暂无用量数据","No usage data yet"),18);
            if (md) center(133,tr("模拟进度将显示在这里。","Simulation progress will show here."),14,muted);
        }
        panelRoot=nullptr;
    }
}
void render() {
    dirty=false;
    if (screen==Screen::Settings && settingsList) settingsY=lv_obj_get_scroll_y(settingsList);
    if (taskList) taskScrollY=lv_obj_get_scroll_y(taskList);
    taskList=nullptr;
    bool moving=pageMotion!=Motion::None && pageMotion!=Motion::EdgeLeft && pageMotion!=Motion::EdgeRight;
    auto *outgoing=moving?pageLayer:nullptr;
    bool closingPanel=outgoing && renderedScreen==Screen::Panel && pageMotion==Motion::Up;
    bool openingPanel=outgoing && screen==Screen::Panel && pageMotion==Motion::Down;
    pageLayer=pageTrack=nullptr;
    batteryIconCount=0;
    batterySetting=nullptr;
    draggingPages=false;
    auto *root=lv_scr_act();
    for(int i=int(lv_obj_get_child_cnt(root))-1;i>=0;--i) {
        auto *child=lv_obj_get_child(root,i);
        if(child!=outgoing) lv_obj_del(child);
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
        if (confirm) {
            center(49,tr("丢弃本次录音？","Discard this recording?"),18);
            center(140,tr("NEXT 选择 · OK 确认","NEXT selects · OK confirms"),14,muted);
        } else {
            for (int i=0;i<20;++i) bars[i]=box(31+9*i,132,6,14,line,3);
        }
        for (int i=0;i<2;++i) {
            int x=i?124:8;
            bool selected=confirm && discardSelected==bool(i);
            box(x,170,108,64,surface,9,yellow,selected?2:0);
            const char *action=i?tr("结束并保存","Stop & save"):tr("取消","Cancel");
            if (confirm) action=i?tr("丢弃","Discard"):tr("继续录音","Keep recording");
            label(x,confirm?191:180,108,action,confirm&&en?14:17,confirm&&i?red:ink,LV_TEXT_ALIGN_CENTER);
            if (!confirm) label(x,207,108,i?"+":"NEXT",14,muted,LV_TEXT_ALIGN_CENTER);
        }
        break;
    }
    case Screen::Saved: {
        box(99,36,42,42,surface,21,muted,2);
        auto *tick=center(49,LV_SYMBOL_OK,14);
        lv_obj_set_style_text_font(tick,LV_FONT_DEFAULT,0);
        center(91,tr("已保存","Saved"),20);
        char msg[56]; auto s=recordedSeconds();
        snprintf(msg,sizeof(msg),"%lu:%02lu · %s",(unsigned long)(s/60),(unsigned long)(s%60),
                 tr("已存入 microSD","on microSD"));
        center(127,msg,14,muted); center(211,tr("保存在本机","Stored locally"),14,muted);
        break;
    }
    case Screen::Error:
        center(45,"!",20,red);
        center(91,tr(faultZh,faultEn),20);
        if (strcmp(faultEn,"Task save failed") && strcmp(faultEn,"Discard failed. Check SD."))
            center(140,tr("本次录音未保存","Recording was not saved"),14,muted);
        center(211,tr("按 OK 关闭","OK to dismiss"),14,muted);
        break;
    case Screen::Sleep: break;
    case Screen::ReleasePower: center(100,tr("松开关机","Release to power off"),18); break;
    case Screen::UnplugPower: center(100,tr("拔下 USB 后关机","Unplug USB to power off"),18); break;
    }
    refreshBatteryWidgets();
    if (pageMotion==Motion::EdgeLeft || pageMotion==Motion::EdgeRight) {
        lv_anim_t a;
        lv_anim_init(&a); lv_anim_set_var(&a,pageLayer);
        lv_anim_set_exec_cb(&a,[](void *p,int32_t x){lv_obj_set_style_translate_x(static_cast<lv_obj_t*>(p),x,0);});
        lv_anim_set_values(&a,0,pageMotion==Motion::EdgeLeft?-14:14);
        lv_anim_set_time(&a,100); lv_anim_set_playback_time(&a,240); lv_anim_start(&a);
    } else if (pageMotion!=Motion::None) {
        bool vertical=pageMotion==Motion::Up || pageMotion==Motion::Down;
        int ms=vertical?280:260;
        if (closingPanel) {
            lv_obj_move_to_index(pageLayer,0);
            translate(outgoing,0,-240,true,ms);
        } else {
            int offset=pageMotion==Motion::Down || pageMotion==Motion::Right?-40:40;
            translate(pageLayer,openingPanel?-240:offset,0,vertical,ms);
        }
        if (outgoing) lv_obj_del_delayed(outgoing,ms+32);
        motionEnds=millis()+ms;
    }
    pageMotion=Motion::None; slideDirection=0;
    if (offlineToast) {
        toast=box(53,194,134,36,surface,12,muted,1);
        label(0,7,134,campusOnline()?tr("尚未同步","Never synced"):tr("离线","Offline"),17,ink,LV_TEXT_ALIGN_CENTER,LV_LABEL_LONG_CLIP,toast);
    }
}
void saveChange(uint16_t value) {
    if (prefs.putUShort("tasks",value)==sizeof(value)) taskState=value;
    else { faultZh="任务保存失败"; faultEn="Task save failed"; go(Screen::Error); }
    dirty=true;
}
void choose() {
    int ids[6],n=tasks(false,ids);
    if (!n) return;
    int id=ids[ticket%n];
    showCompleted=false; revealFocus=true;
    go(Screen::Today);
    saveChange(taskState | (1u<<(8+id)));
    int today[6];
    int count=tasks(true,today);
    for (int i=0;i<count;++i) if (today[i]==id) focused=i;
    ticket=0;
}
void browse(int step) {
    slideDirection=step;
    if (screen==Screen::Lock) note=(note+step+2)%2;
    else {
        int ids[6],n=tasks(false,ids);
        if (!n) return;
        ticket=(ticket+step+n)%n;
    }
    interacted=rotated=millis();
    dirty=true;
}
void shortKey(int pin) {
    if (pin==plusPin) { recordKey(); return; }
    bool ok=pin==okPin;
    int ids[6],later[6],n=tasks(true,ids),k=tasks(false,later);
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
        else if (focused<n) saveChange(taskState ^ (1u<<ids[focused]));
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
        if (target==Screen::Sleep) s=tr("休眠","Sleep");
        if (target==Screen::Lock) s=tr("锁屏","Lock");
        label(0,94,240,s,20,ink,LV_TEXT_ALIGN_CENTER,LV_LABEL_LONG_CLIP,overlay);
        box(70,135,100,5,dim,3,0,0,overlay);
        progressFill=box(70,135,1,5,ink,3,0,0,overlay);
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
    int step=dx<0?1:-1, ids[6], later[6], n=tasks(true,ids), k=tasks(false,later);
    interacted=millis();
    if (recordingActive()) {
        if (!tap || y<170 || y>=234 || x<8 || x>=232 || (x>=116 && x<124)) return;
        if (screen==Screen::ConfirmDiscard) resolveDiscard(x>=124);
        else if (x>=124) recordKey();
        else askDiscard();
    } else if (y<32 && dy>=30 && dy>abs(dx) &&
        (screen==Screen::Today || screen==Screen::Claude || screen==Screen::MD)) {
        panelFrom=screen; go(Screen::Panel);
    } else if (screen==Screen::Panel) {
        if (-dy>=30 && -dy>abs(dx)) go(panelFrom);
        else if (tap && y>=42 && y<108) {
            if (x>=10 && x<116) syncNow();
            else if (x>=124 && x<230) changeLanguage();
        } else if (tap && x>=10 && x<230 && y>=176 && y<224) {
            settingFocus=0; settingsY=0; freeBytes=cardFreeBytes(); go(Screen::Settings);
        }
    } else if (screen==Screen::Settings) {
        if (dx>=40 && horizontal) go(Screen::Panel);
        else if (tap && y<32 && x<145) go(Screen::Panel);
        else if (tap && settingsList && y>=32) {
            int pos=y-32+lv_obj_get_scroll_y(settingsList);
            if (pos>=0 && pos<SettingCount*56) {
                int old=settingFocus;
                settingFocus=pos/56;
                styleRow(old); styleRow(settingFocus);
                activateSetting(settingFocus);
            }
        }
    } else if (screen==Screen::SetClock && tap) {
        if (y<32 && x<145) go(Screen::Settings);
        else if (y>=126 && y<168) adjustClock(x<120?-1:1);
        else if (y>=181 && y<224) nextClockField();
    } else if (screen==Screen::Brightness) {
        if (tap && y<32 && x<145) go(Screen::Settings);
    } else if (screen==Screen::Lock) {
        if (-dy>=60 && -dy>abs(dx)) go(Screen::Today);
        else if (horizontal && abs(dx)>=24) browse(step);
    } else if (screen==Screen::Today && y>=164) {
        if (k && (horizontal || tap)) { focused=n; if (horizontal) browse(step); else go(Screen::Picker); }
    } else if (screen==Screen::Today && tap && x<105 && y<32) {
        showCompleted=!showCompleted; focused=0;
        if(taskList) lv_obj_scroll_to_y(taskList,0,LV_ANIM_OFF);
        taskScrollY=0; dirty=true;
    } else if (screen==Screen::Picker) {
        if (horizontal) browse(step);
        else if (tap && y>=130 && y<202) { if (x<116) go(Screen::Today); else if (x>=124) choose(); }
    } else if (horizontal && (screen==Screen::Today || screen==Screen::Claude || screen==Screen::MD)) {
        int page=pageIndex(screen);
        if (page+step<0 || page+step>2) { pageMotion=step>0?Motion::EdgeLeft:Motion::EdgeRight; dirty=true; }
        else go(pages[page+step]);
    } else if (tap && screen==Screen::Today && taskList && y>=32 && y<164) {
        int pos=y-32+lv_obj_get_scroll_y(taskList);
        if (pos>=0 && pos%63<60 && pos/63<n) { focused=pos/63; shortKey(okPin); }
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
            listTouch=screen==Screen::Today && ty>=32 && ty<164;
            pageTouch=pageTrack && from==renderedScreen && (from!=Screen::Today || ty<164);
            verticalDrag=false;
            if (pageTouch) {
                trackStart=lv_obj_get_style_x(pageTrack,0);
                draggingPages=lv_anim_del(pageTrack,positionTrack);
            }
            slider=(screen==Screen::Panel && tx>=10 && tx<230 && ty>=116 && ty<168) ||
                   (screen==Screen::Brightness && tx>=14 && tx<226 && ty>=108 && ty<180);
            moved=screen==Screen::Settings && settingsList && lv_obj_is_scrolling(settingsList);
            if (listTouch && taskList && lv_obj_is_scrolling(taskList)) moved=true;
        }
        x=tx; y=ty; lastInput=lastSeen=now;
        settingsTouch=screen==from && ((from==Screen::Settings && startY>=32) || listTouch);
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
            const int inset=screen==Screen::Panel?10:14, width=240-2*inset;
            setBrightnessLevel(((x-inset)*20+width-1)/width);
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
    int ids[6],n=tasks(true,ids);
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
            int later[6], count=tasks(false,later);
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
        uint8_t level=audioLevel();
        if (screen==Screen::Recording && lastLevel!=level) {
            for (int i=0;i<20;++i) lv_obj_set_style_bg_color(bars[i],lv_color_hex(i<level?red:line),0);
            lastLevel=level;
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
    taskState=prefs.getUShort("tasks",0);
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
    delay(screen==Screen::Sleep?10:2);
}
