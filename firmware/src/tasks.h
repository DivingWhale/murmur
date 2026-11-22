#pragma once
#include <cstdint>
#include <vector>

// Task list and lock-screen notes shown on the handheld.
//
// Unpaired previews keep sample state in NVS. Paired handhelds use the Pi
// task cache on the card. Row indices are valid until the next sync.
namespace model {
constexpr int maxTasks=6;

void load();
// Task ids in Today (unfinished or, with `completed`, finished) and in When I Have Time.
const std::vector<int>& ids(bool today,bool completed);
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
