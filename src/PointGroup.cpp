#include "PointGroup.h"
#include <algorithm>

std::vector<PointGroup> groupPoints(const std::vector<PointTemplate>& points,
                                    PointType type) {
    // 1. 筛出指定类型的点，记下原下标
    struct Item {
        size_t idx;
        uint16_t addr;
        uint16_t count;
        unsigned pollMs;
    };
    std::vector<Item> items;
    for (size_t i = 0; i < points.size(); ++i) {
        if (points[i].type == type) {
            items.push_back({i, points[i].southAddr, points[i].southCount, points[i].pollMs});
        }
    }

    // 2. 按地址排序
    std::sort(items.begin(), items.end(),
              [](const Item& a, const Item& b) { return a.addr < b.addr; });

    // 3. 贪心合并连续地址
    std::vector<PointGroup> groups;
    for (auto& it : items) {
        if (groups.empty()) {
            PointGroup g;
            g.startAddr = it.addr;
            g.count     = it.count;
            g.pollMs    = it.pollMs;
            g.pointIdx.push_back(it.idx);
            groups.push_back(g);
            continue;
        }

        PointGroup& last = groups.back();
        uint16_t lastEnd = last.startAddr + last.count;   // 上一组末尾的下一个地址

        if (it.addr <= lastEnd && it.pollMs == last.pollMs) {
            // 连续或重叠，合并
            uint16_t newEnd = std::max<uint16_t>(lastEnd, it.addr + it.count);
            last.count = newEnd - last.startAddr;
            last.pointIdx.push_back(it.idx);
        } else {
            // 不连续，开新组
            PointGroup g;
            g.startAddr = it.addr;
            g.count     = it.count;
            g.pollMs    = it.pollMs;
            g.pointIdx.push_back(it.idx);
            groups.push_back(g);
        }
    }

    return groups;
}
