#pragma once
#include <string>
#include <vector>
struct cJSON;

namespace lockNotes {
struct Note { int id; std::string stamp, text; };
// Returned in ascending ID order. The lock screen starts with the newest.
const std::vector<Note>& all();
const std::string& version();
int cursor();
// Card access is owned by the caller for both operations.
void restore();
bool apply(cJSON *page);
}
