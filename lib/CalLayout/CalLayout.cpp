// CalLayout.cpp
#include "CalLayout.h"
#include <algorithm>

namespace {
struct IntervalTmp {
    size_t idx;
    int startMin; // minutes from midnight
    int endMin;   // minutes from midnight
    String startIso;
    String endIso;
};

int parseMinutes(const String& iso) {
    int t = iso.indexOf('T');
    if (t < 0 || iso.length() < t + 6) return 0;
    int h = iso.substring(t+1, t+3).toInt();
    int m = iso.substring(t+4, t+6).toInt();
    return h * 60 + m;
}

String plusOneHour(const String& iso) {
    int t = iso.indexOf('T');
    if (t < 0 || iso.length() < t + 6) return iso;
    int year = iso.substring(0,4).toInt();
    int mon  = iso.substring(5,7).toInt();
    int day  = iso.substring(8,10).toInt();
    int h    = iso.substring(t+1, t+3).toInt();
    int mi   = iso.substring(t+4, t+6).toInt();
    h += 1;
    if (h >= 24) h = 23; // clamp (simple)
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:00+0000", year, mon, day, h, mi);
    return String(buf);
}
}

std::vector<CalLayoutBox> computeCalendarLayout(const std::vector<CalLayoutInput>& inputs) {
    std::vector<IntervalTmp> intervals;
    intervals.reserve(inputs.size());
    for (size_t i=0;i<inputs.size();++i) {
        String endIso = inputs[i].endIso.length() ? inputs[i].endIso : plusOneHour(inputs[i].startIso);
        int s = parseMinutes(inputs[i].startIso);
        int e = parseMinutes(endIso);
        if (e <= s) e = s + 60; // fallback 1h
        intervals.push_back({i, s, e, inputs[i].startIso, endIso});
    }
    // Sort by start asc, duration desc
    std::sort(intervals.begin(), intervals.end(), [](const IntervalTmp& a, const IntervalTmp& b){
        if (a.startMin != b.startMin) return a.startMin < b.startMin;
        return (a.endMin - a.startMin) > (b.endMin - b.startMin);
    });

    struct GroupInfo { std::vector<size_t> indices; int maxEnd; }; 
    std::vector<GroupInfo> groups;
    // Build overlap groups
    for (size_t i=0;i<intervals.size();++i) {
        if (groups.empty() || intervals[i].startMin >= groups.back().maxEnd) {
            groups.push_back(GroupInfo{{i}, intervals[i].endMin});
        } else {
            groups.back().indices.push_back(i);
            if (intervals[i].endMin > groups.back().maxEnd) groups.back().maxEnd = intervals[i].endMin;
        }
    }

    std::vector<CalLayoutBox> result;
    result.reserve(inputs.size());

    for (auto &g : groups) {
        // Column assignment within group
        std::vector<int> colEnd; // per column endMin
        struct LocalPlaced { size_t vecIdx; int col; };
        std::vector<LocalPlaced> local;
        for (size_t gi : g.indices) {
            const auto &iv = intervals[gi];
            int col = 0;
            for (;;++col) {
                bool clash = false;
                for (auto &lp : local) {
                    if (lp.col == col) {
                        const auto &other = intervals[lp.vecIdx];
                        bool overlap = !(iv.endMin <= other.startMin || iv.startMin >= other.endMin);
                        if (overlap) { clash = true; break; }
                    }
                }
                if (!clash) break;
            }
            local.push_back({gi, col});
            if ((int)colEnd.size() <= col) colEnd.resize(col+1, 0);
            if (iv.endMin > colEnd[col]) colEnd[col] = iv.endMin;
        }
        int totalCols = (int)colEnd.size();
        // Emit results
        for (auto &lp : local) {
            const auto &iv = intervals[lp.vecIdx];
            result.push_back({iv.idx, lp.col, totalCols, iv.endIso});
        }
    }

    // Restore original event order for stable drawing order top->down using start
    std::sort(result.begin(), result.end(), [&](const CalLayoutBox& a, const CalLayoutBox& b){
        const auto &A = intervals[a.eventIndex];
        const auto &B = intervals[b.eventIndex];
        if (A.startMin != B.startMin) return A.startMin < B.startMin;
        return a.column < b.column;
    });
    return result;
}
