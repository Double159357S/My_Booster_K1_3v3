# 编译启动： （在机器人 Linux 上运行）

cd ~/Workspace/K1_5v5_demo_1.7
./scripts/build.sh
source install/setup.bash #编译成功、没有报错后，加载新环境
./scripts/start.sh

## 编译启动后，查看日志

tail -f brain.log
tail -f vision.log
tail -f game_controller.log
