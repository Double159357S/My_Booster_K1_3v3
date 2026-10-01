#!/bin/bash
echo "[START VISION]"
cd `dirname $0`
cd ..

source ./install/setup.bash
export FASTRTPS_DEFAULT_PROFILES_FILE=./configs/fastdds.xml

#vision_config_path:= 可以指定配置文件路径，如果不额外指定使用路径为<workspace>/install/vision/share/vision/config/
ros2 launch vision launch.py vision_config_path:=/opt/booster show_det:=false save_data:=true
