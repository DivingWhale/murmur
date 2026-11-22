#pragma once
#include <string>

namespace taskNotice {
// Called after a sync has committed a previously unseen task to the card.
void added(const std::string &title,const std::string &plan,const std::string &due);
void notesAdded(unsigned n);
bool take(std::string &text);
}
