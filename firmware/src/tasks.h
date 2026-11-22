#pragma once
#include <cstdint>

// Task list and lock-screen notes shown on the handheld.
//
// For now this is the prototype's sample content, with completion and
// promotion persisted in NVS. When the Pi sync exists, it replaces the sample
// data behind this interface; the screens only use these functions.
namespace model {
constexpr int maxTasks=6;

void load();
// Task ids in Today (unfinished or, with `completed`, finished) and in When I Have Time.
int today(int *ids,bool completed);
int later(int *ids);
const char *title(int id);    // In the current screen language.
const char *detail(int id);   // Meta line such as "Carried over", or "".
const char *deadline(int id); // Such as "Due Fri", or nullptr.
bool done(int id);
// Persisting changes. Each returns false if NVS rejected the write.
bool toggleDone(int id);
bool moveToToday(int id);

// Local changes (task edits, recordings) not yet sent to the Pi.
unsigned pending();
void notePending();

int noteCount();
const char *note(int index);
const char *noteTime(int index);
}
