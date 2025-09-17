// CalLayout.h - calendar event overlap layout helper
#pragma once
#include <Arduino.h>
#include <vector>

// Lightweight description of an event needed for layout.
struct CalLayoutInput {
    String startIso; // ISO start
    String endIso;   // ISO end (may be empty -> treated as +1h)
};

struct CalLayoutBox {
    size_t eventIndex;   // index into original events vector
    int column;          // assigned column within its overlap group (leftmost if spanning)
    int groupColumns;    // total columns in that overlap group
    int colSpan;          // how many columns this event spans (>=1)
    String effectiveEnd; // resolved end ISO (with +1h fallback applied)
};

// Compute column layout for overlapping events.
// Rules:
//  * Earlier start -> left.
//  * If same start, longer duration -> left.
//  * Two parallel events -> each spans half width (groupColumns=2 used by renderer).
//  * More than two -> equal width columns.
// Original simple layout (one box per event).
std::vector<CalLayoutBox> computeCalendarLayout(const std::vector<CalLayoutInput>& inputs);
