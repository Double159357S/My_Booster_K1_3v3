#pragma once

// Shared by the strategy and the UDP wire format; no ROS dependency.
struct Pose2D {
    double x = 0.0;
    double y = 0.0;
    double theta = 0.0;
};

struct Point {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};
