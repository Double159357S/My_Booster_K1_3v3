cmake_minimum_required(VERSION 3.16)
project(k1_vision_sync_tests LANGUAGES CXX)

# Tests the real synchronizer without CUDA, TensorRT or a camera.
find_package(ament_cmake REQUIRED)
find_package(geometry_msgs REQUIRED)
find_package(OpenCV REQUIRED)
find_package(yaml-cpp REQUIRED)

add_executable(test_data_syncer
  ../base/test_data_syncer.cpp
  ../../src/base/data_syncer.cpp)
target_compile_features(test_data_syncer PRIVATE cxx_std_17)
target_include_directories(test_data_syncer PRIVATE ../../include ${OpenCV_INCLUDE_DIRS})
target_link_libraries(test_data_syncer ${OpenCV_LIBS} yaml-cpp)
ament_target_dependencies(test_data_syncer geometry_msgs)
enable_testing()
add_test(NAME vision_data_syncer COMMAND test_data_syncer)
