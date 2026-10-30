#ifndef POINT_TEMPLATE_H
#define POINT_TEMPLATE_H

#include <string>
#include <vector>
#include <cstdint>

enum class PointType { YC, YX, YK, YT };
enum class PointDataType { U16, S16, BOOL };

struct PointTemplate {
    std::string pointId;
    std::string deviceId; // 可选；为空表示应用到所有设备
    std::string desc;
    PointType   type;
    uint16_t    southAddr  = 0;
    uint16_t    southCount = 1;
    float       scale      = 1.0f;
    float       offset     = 0.0f;
    std::string unit;
    uint16_t    northAddr  = 0;
    PointDataType dataType = PointDataType::U16;
    unsigned    pollMs = 2000;
    float       deadband = 0.0f;
    float       minValue = -65535.0f;
    float       maxValue = 65535.0f;
    std::string interlockPoint;
    float       interlockValue = 1.0f;
};

// 从配置文件加载点表。
std::vector<PointTemplate> loadPointTemplates(const std::string& path);

// 将点类型转换为字符串。
const char* pointTypeStr(PointType t);

#endif
