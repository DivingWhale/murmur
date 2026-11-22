#include "task_notice.h"
#include "ui_kit.h"
#include <Arduino.h>
#include <time.h>

namespace taskNotice {
namespace {
unsigned count;
std::string firstTitle, firstDate;
bool deadline;

std::string dateLabel(const std::string &iso) {
    std::string label=iso.substr(0,10);
    const time_t now=time(nullptr);
    if (now>=1704067200) {
        tm local{};
        localtime_r(&now,&local);
        char today[11], tomorrow[11];
        strftime(today,sizeof(today),"%Y-%m-%d",&local);
        ++local.tm_mday;
        mktime(&local);
        strftime(tomorrow,sizeof(tomorrow),"%Y-%m-%d",&local);
        if (label==today) label=ui::tr("今天","Today");
        else if (label==tomorrow) label=ui::tr("明天","Tomorrow");
    }
    if (iso.size()>=16) label+=" "+iso.substr(11,5);
    return label;
}
}

void added(const std::string &title,const std::string &plan,const std::string &due) {
    if (count++==0) {
        firstTitle=title;
        deadline=plan.empty() && !due.empty();
        firstDate=plan.empty()?due:plan;
    }
}
bool take(std::string &text) {
    if (!count) return false;
    if (count>1) {
        char summary[48];
        snprintf(summary,sizeof(summary),ui::tr("新增 %u 项任务","+%u tasks"),count);
        text=summary;
    } else {
        text=deadline?ui::tr("截止 ","Due "):"";
        text+=firstDate.empty()?ui::tr("有空再做","When I have time"):dateLabel(firstDate);
        text+=" · "+firstTitle;
    }
    count=0;
    return true;
}
}
