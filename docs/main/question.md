# Questions

## （一）关于裁判机可能会遇到的问题

1. 同一局域网内多裁判机情况，导致机器人出现混乱比赛状态

- 问题确定：
    在demo目录下执行命令：tail -f brain.log
    查看brain.log的信息，主要查看gamestate状态。
    如果发现一直在（ready、initial、set、palying）跳变，那么证明局域网内存在不止一个裁判机系统。

- 解决方案：
    第一步：进入robocup_demo/src/game_controller/launch/launch.py
    第19行，将白名单开关置为True，
    第22行：修改引号内的ip地址为裁判机系统的ip地址。要注意修改后的格式，要确认正确。
    此时就开启了白名单过滤功能，只会监听白名单内的裁判机了。

    第二步：修改src/brain/config/config.yaml参数：game_control_ip: "127.0.0.1"
    修改ip为对应game_controller的ip地址。

## （二）关于机器人可能会遇到的问题
