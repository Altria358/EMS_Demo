#ifndef POINT_GROUP_H
#define POINT_GROUP_H

#include "PointTemplate.h"
#include <cstdint>
#include <vector>
#include <chrono>

// 一组连续的寄存器
struct PointGroup {
    uint16_t startAddr;              // 起始地址
    uint16_t count;                  // 总寄存器数
    unsigned pollMs = 2000;
    unsigned consecutiveFailures = 0;
    std::chrono::steady_clock::time_point lastPoll{};
    std::vector<size_t> pointIdx;    // 包含的点在 points 里的下标
};

// 把同一类型（YC 或 YX）的点按连续地址分组
std::vector<PointGroup> groupPoints(const std::vector<PointTemplate>& points,
                                    PointType type);

#endif
