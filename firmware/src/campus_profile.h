#pragma once
#include <stdint.h>

struct CampusLogin {
    char user[129]{};
    char password[129]{};
    void clear();
    ~CampusLogin() { clear(); }
};
bool hasCampusProfile();
bool saveCampusProfile(const CampusLogin &login);
bool loadCampusProfile(CampusLogin &login);
bool forgetCampusProfile();
uint32_t campusClockHint();
