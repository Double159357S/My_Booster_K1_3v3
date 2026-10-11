# 3v3 前锋协作

两个前锋同时靠近球时，只有获得共享控球权的机器人追球、调整和踢球；另一台在球的侧后方接应。靠近球不会绕过控球权检查。

## 更新要求

队内通信新增控球权、序号和进程标识，使用 `31204` / 160 字节数据包。**三台机器人必须一起更新 brain、配置和行为树，并重新编译、重启 Demo。** 新版本能读取旧包的状态信息，但旧包不能参与新的控球权确认，不支持混用版本比赛。

每台保留自己的 `game.player_id`、`game.player_role`、相机配置和网络地址，不要把同一份机器人配置直接覆盖到三台上。三台的 `game.team_id`、`game.number_of_players: 3`、协作参数应一致，并开启队内通信。

在已加载 ROS 和工程环境的机器人终端，从 Demo 根目录编译：

```bash
./scripts/build_brain.sh
colcon test --packages-select brain --ctest-args -R 'test_(team_cooperation|kick_geometry|kick_strategy|kick_behavior_tree)$' --output-on-failure
colcon test-result --verbose
```

## 默认行为和参数

| 参数（strategy.cooperation 下） | 默认值 | 含义 |
| --- | --- | --- |
| comm_fresh_ms | 800.0 | 队友消息过期时间，毫秒 |
| owner_lease_ms | 1200.0 | 控球授权期限，毫秒 |
| owner_hold_ms | 1500.0 | 主攻最短保持时间，毫秒 |
| owner_switch_margin | 0.75 | 换主攻所需的到球成本优势 |
| owner_confirm_ms | 500.0 | 成本优势持续确认时间，毫秒 |
| support_back_distance | 1.4 | 接应点在球后方的距离，米 |
| support_side_distance | 1.2 | 接应点的横向距离，米 |
| teammate_stop_distance | 0.8 | 禁止继续靠近队友的距离，米 |
| teammate_slow_distance | 1.3 | 开始限制接近速度的距离，米 |
| teammate_approach_speed | 0.4 | 减速区接近速度上限，米/秒 |

通信正常的、已定位的最小球员号负责发布控球权，其他机器人回传确认。3 人均未受罚时，需要至少 2 台确认；孤立的机器人不会自行抢球。主攻只有进入 AI 足球模式且知道球的位置时才参与候选。正常交接先撤销旧授权，留出退出踢球的时间，再授权下一台；协调机器人更换时默认等待约 1.95 秒。

接应方向在同一控球轮次内保持稳定，优先避开主攻手到球的路线；场地边缘会限制目标点。追球速度改为 1.0 米/秒，接应为 0.6 米/秒，并开启近球限速和追球避障。普通速度命令限制朝队友靠近的分量，保留后退和侧移；视觉踢球靠近队友时退出。

队友避碰依赖定位和新鲜通信，不能代替视觉避障。实机仍需验证定位误差、通信延迟和制动距离。此改动实现主攻/接应分工，未增加新的传球识别或传球执行策略。

## 回归测试

`test_team_cooperation` 覆盖成本抖动、持续优势交接、踢球期间保持主攻、退出踢球后的交接、协调机器人断联及重启、无多数确认、丢包与乱序、缓存授权过期、四种通信包格式，以及接应路线和多队友速度约束。`test_kick_geometry` 保留原有踢球几何测试。

守门员清球和前锋射门的更新、参数与复测步骤见 [KICKING.md](KICKING.md)，`test_kick_strategy` 验证清球、对准和球源有效期，`test_kick_behavior_tree` 验证决策的动作入口。
