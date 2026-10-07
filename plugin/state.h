#pragma once
// The plugin's state text, shared by projects (effGetChunk / effSetChunk) and preset files.
//
// "subforce 1": key=value lines of REAL values (Hz, seconds, semitones, option index) for every
// sound parameter, plus, in a project, the preset it came from and the unit the instance is
// (Analog's tolerances; a project from before units is unit 1). Survives parameters being added
// or reordered AND ranges changing (a 0..1 value would silently move when a range does).
#include "surface.h"

#include <string>

namespace sf {

constexpr int kStateVersion = 1;

std::string saveState(const Surface& s, bool asPreset);
bool isStateText(const std::string& text);   // "subforce <version >= 1>" (a UTF-8 BOM allowed)
// A preset starts from the defaults (what it doesn't say is the default); a project's state
// only overrides what it lists. False if it isn't SubForce state.
bool loadState(Surface& s, const std::string& text, bool asPreset);

} // namespace sf
