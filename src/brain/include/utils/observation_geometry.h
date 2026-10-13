#pragma once

#include <cmath>
#include "geometry_types.h"

namespace observation_geometry {

// 观测先固定在拍摄时的 odom 坐标中，再投影到当前场地/机器人坐标。
inline Point toParent(const Point &point, const Pose2D &frame) {
    const double c = std::cos(frame.theta), s = std::sin(frame.theta);
    return {frame.x + c * point.x - s * point.y,
            frame.y + s * point.x + c * point.y, point.z};
}

inline Point toLocal(const Point &point, const Pose2D &frame) {
    const double c = std::cos(frame.theta), s = std::sin(frame.theta);
    const double dx = point.x - frame.x, dy = point.y - frame.y;
    return {c * dx + s * dy, -s * dx + c * dy, point.z};
}

} // namespace observation_geometry
