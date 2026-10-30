#include "PointTemplate.h"
#include "Logger.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

// ============================================================================
// 工具：trim
// ============================================================================
/**
 * @brief 去除字符串首尾空白字符。
 * @param s 待处理字符串。
 * @return 去除首尾空白后的字符串。
 */
static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// ============================================================================
// 类型转字符串
// ============================================================================
/**
 * @brief 将点类型转换为文本表示。
 * @param t 点类型。
 * @return 点类型字符串。
 */
const char* pointTypeStr(PointType t) {
    switch (t) {
        case PointType::YC: return "YC";
        case PointType::YX: return "YX";
        case PointType::YK: return "YK";
        case PointType::YT: return "YT";
    }
    return "??";
}

// ============================================================================
// 加载点表
// ============================================================================
/**
 * @brief 解析点表配置文件并返回点模板列表。
 * @param path 点表配置文件路径。
 * @return 已成功解析的点模板列表。
 */
std::vector<PointTemplate> loadPointTemplates(const std::string& path) {
    std::vector<PointTemplate> pts;
    std::ifstream in(path);
    if (!in) {
        LOG_WARN("Config", "无法打开点表: " + path);
        return pts;
    }

    std::string line;
    int lineNo = 0;
    int okCount = 0, failCount = 0;
    std::set<std::string> ids;
    std::set<std::string> southKeys, northKeys;

    while (std::getline(in, line)) {
        lineNo++;

        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;

        std::stringstream ss(t);
        std::string tok;
        std::vector<std::string> parts;
        while (std::getline(ss, tok, ',')) {
            parts.push_back(trim(tok));
        }

        if (parts.size() != 9 && parts.size() != 10 && parts.size() != 17) {
            LOG_ERROR("Config", "点表第 " + std::to_string(lineNo) +
                               " 行字段数无效（只接受 9/10/17 列）");
            failCount++;
            continue;
        }

        if (parts.size() < 9) {
            LOG_WARN("Config", "点表第 " + std::to_string(lineNo) +
                              " 行字段不足（需 9 个），跳过");
            failCount++;
            continue;
        }

        PointTemplate p;

        // 类型
        if (parts[0] == "YC") p.type = PointType::YC;
        else if (parts[0] == "YX") p.type = PointType::YX;
        else if (parts[0] == "YK") p.type = PointType::YK;
        else if (parts[0] == "YT") p.type = PointType::YT;
        else {
            LOG_WARN("Config", "点表第 " + std::to_string(lineNo) +
                              " 行类型无效: " + parts[0]);
            failCount++;
            continue;
        }

        // 点名、描述
        p.pointId = parts[1];
        p.desc    = parts[2];

        if (p.pointId.empty()) {
            LOG_WARN("Config", "点表第 " + std::to_string(lineNo) +
                              " 行点名为空，跳过");
            failCount++;
            continue;
        }

        // 数值字段，带异常捕获
        try {
            auto parseU16 = [](const std::string& s) {
                size_t used = 0; long v = std::stol(s, &used);
                if (used != s.size() || v < 0 || v > 65535) throw std::out_of_range("uint16范围错误");
                return static_cast<uint16_t>(v);
            };
            p.southAddr  = parseU16(parts[3]);
            p.southCount = parseU16(parts[4]);
            size_t usedScale = 0, usedOffset = 0;
            p.scale      = std::stof(parts[5], &usedScale);
            p.offset     = std::stof(parts[6], &usedOffset);
            if (usedScale != parts[5].size() || usedOffset != parts[6].size() ||
                !std::isfinite(p.scale) || !std::isfinite(p.offset))
                throw std::invalid_argument("比例/偏移不是有限数值");
            p.unit       = parts[7];
            p.northAddr  = parseU16(parts[8]);
            if (parts.size() > 9) p.deviceId = parts[9];
            if (parts.size() > 10 && !parts[10].empty()) {
                if (parts[10] == "U16") p.dataType = PointDataType::U16;
                else if (parts[10] == "S16") p.dataType = PointDataType::S16;
                else if (parts[10] == "BOOL") p.dataType = PointDataType::BOOL;
                else throw std::invalid_argument("未知数据类型: " + parts[10]);
            }
            if (parts.size() > 11 && !parts[11].empty()) p.pollMs = static_cast<unsigned>(std::stoul(parts[11]));
            if (parts.size() > 12 && !parts[12].empty()) p.deadband = std::stof(parts[12]);
            if (parts.size() > 13 && !parts[13].empty()) p.minValue = std::stof(parts[13]);
            if (parts.size() > 14 && !parts[14].empty()) p.maxValue = std::stof(parts[14]);
            if (parts.size() > 15) p.interlockPoint = parts[15];
            if (parts.size() > 16 && !parts[16].empty()) p.interlockValue = std::stof(parts[16]);
        } catch (const std::exception& e) {
            LOG_ERROR("Config", "点表第 " + std::to_string(lineNo) +
                               " 行解析错误: " + e.what());
            failCount++;
            continue;
        }

        // 校验
        if (p.southCount != 1 || p.scale == 0.0f ||
            p.pollMs == 0 || p.pollMs > 3600000 ||
            !std::isfinite(p.deadband) || p.deadband < 0 ||
            !std::isfinite(p.minValue) || !std::isfinite(p.maxValue) || p.minValue > p.maxValue ||
            !std::isfinite(p.interlockValue) ||
            ((p.type == PointType::YX || p.type == PointType::YK) && p.dataType != PointDataType::U16 && p.dataType != PointDataType::BOOL) ||
            ((p.type == PointType::YC || p.type == PointType::YT) && p.dataType == PointDataType::BOOL) ||
            ((p.type == PointType::YX || p.type == PointType::YK) && p.dataType == PointDataType::S16)) {
            LOG_ERROR("Config", "点表第 " + std::to_string(lineNo) + " 行字段组合或范围无效");
            failCount++; continue;
        }
        const std::string idKey = p.deviceId + "." + p.pointId;
        const std::string southKey = p.deviceId + "." + std::to_string(static_cast<int>(p.type)) + "." + std::to_string(p.southAddr);
        const std::string northKey = std::to_string(static_cast<int>(p.type)) + "." + std::to_string(p.northAddr);
        if (!ids.insert(idKey).second || !southKeys.insert(southKey).second || !northKeys.insert(northKey).second) {
            LOG_ERROR("Config", "点表第 " + std::to_string(lineNo) + " 行点名或地址重复");
            failCount++; continue;
        }

        pts.push_back(p);
        okCount++;
    }

    if (failCount > 0) { LOG_ERROR("Config", "点表存在错误，拒绝部分加载"); return {}; }
    LOG_INFO("Config", "点表加载: " + std::to_string(okCount) +
                      " 个点 (失败 " + std::to_string(failCount) + " 行)");
    return pts;
}
