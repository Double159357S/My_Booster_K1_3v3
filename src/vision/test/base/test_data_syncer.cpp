#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include "booster_vision/base/data_syncer.hpp"

namespace {
void check(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
booster_vision::Pose poseAt(float x) {
    booster_vision::Pose pose;
    auto matrix = pose.toCVMat();
    matrix.at<float>(0, 3) = x;
    return pose;
}
}

int main() {
    using namespace booster_vision;
    try {
        DataSyncer sync(true);
        ColorDataBlock image(cv::Mat::zeros(2, 2, CV_8UC3), 10.0);
        auto block = sync.getSyncedDataBlock(image);
        check(!block.poseMatchesImage(120), "startup fabricated a valid head pose");
        check(block.depth_data.data.empty(), "startup fabricated a depth frame");
        sync.AddPose({poseAt(1.0f), 9.99});
        block = sync.getSyncedDataBlock(image);
        check(block.poseMatchesImage(120), "single pose was not matched");
        check(block.pose_data.data.getTranslationVec()[0] == 1.0f, "wrong head pose selected");
        // Late packets must not hide the closer sample behind them.
        sync.AddPose({poseAt(2.0f), 9.98});
        sync.AddPose({poseAt(3.0f), 9.70});
        sync.AddPose({poseAt(4.0f), 0.0});
        sync.AddPose({poseAt(5.0f), std::numeric_limits<double>::quiet_NaN()});
        block = sync.getSyncedDataBlock(image);
        check(block.pose_data.timestamp == 9.99, "out-of-order pose hides closest sample");
        sync.AddDepth({cv::Mat::ones(2, 2, CV_16UC1), 9.99});
        sync.AddDepth({cv::Mat::zeros(2, 2, CV_16UC1), 9.50});
        block = sync.getSyncedDataBlock(image);
        check(block.depth_data.timestamp == 9.99, "out-of-order depth hides closest sample");
        check(block.depth_data.data.at<unsigned short>(0, 0) == 1, "wrong depth contents selected");
        block.color_data.timestamp = 11.0;
        check(!block.poseMatchesImage(120), "stale head pose accepted");
        block.color_data.timestamp = 9.0;
        check(!block.poseMatchesImage(120), "future head pose accepted");
        block.color_data.timestamp = std::numeric_limits<double>::quiet_NaN();
        check(!block.poseMatchesImage(120), "invalid image time accepted");
        DataBuffer<3, PoseDataBlock> buffer;
        check(buffer.empty(), "buffer begins with placeholder poses");
        for (int i = 1; i <= 10; ++i) buffer.push_back({poseAt(i), static_cast<double>(i)});
        check(buffer.size() == 3 && buffer.front().timestamp == 8.0,
              "pose buffer did not keep the newest bounded history");
        std::cout << "Vision sync: startup, stale poses, reordering and bounded history passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
