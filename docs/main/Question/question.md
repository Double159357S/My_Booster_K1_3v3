# Questions

## （一）关于机器人可能会遇到的问题

### 1. 视觉节点收不到图像和 CameraInfo

记录日期：2026-10-05。

#### 问题现象

检测和分割模型初始化成功，但 `vision.log` 停在以下警告，视觉节点收不到图像和内参：

```text
no valid CameraInfo received within 3.000 s; using YAML fallback
```

#### 问题原因

头部相机处理服务启动时，设备时间错误地处于 **2053 年**，随后系统时间回拨到 **2026 年**，硬件时钟 RTC 仍是 2053 年。处理节点持续打印：

```text
drop one message to avoid timestamp disorder
```

结合日志和重启后的恢复情况，判断时间回拨导致 `StereoNetNode` 持续丢帧，无法正常发布 RGB 和 CameraInfo，主机视觉节点因此超时。

#### 解决方法

从机器人主机登录头部相机板：

```bash
ssh root@192.168.127.10
```

在头部检查时间和处理日志：

```bash
date -Is
timedatectl status
journalctl -u start_stereo.service -n 20 --no-pager
```

**确认系统时间正确后**，重启处理服务并将当前时间写入 RTC：

```bash
systemctl restart start_stereo.service
hwclock --systohc --utc
```

以上两条修复命令在头部执行。本次没有修改视觉源码、YAML、启动脚本或 service 文件。

#### 验证结果

回到机器人主机，在工程目录检查日志：

```bash
cd /home/booster/Workspace/K1_5v5_demo_1.7
grep '\[vision\]\[intrinsics\]' vision.log | tail -5
```

出现 `using CameraInfo topic intrinsics`，说明视觉节点已自动使用相机实际内参。RGB 图像和检测输出恢复，本次实测检测发布频率约 **15～17 Hz**。

### 2. 入场定位失败，头部持续扫描找球

记录日期：2026-10-06。

#### 问题现象

机器人 `192.168.133.43` 按 **LT+A** 进入定位找球阶段后，头部持续转动，不能稳定跟球。相机已经能识别球，但入场定位连续失败，重新标定后仍未解决。

#### 问题原因

当时机器人使用 `game.field_type: "adult_size"`。此前填写的 **14×9 米**是标称或估算尺寸，当前视觉中的白线几何与该模型不匹配，定位残差无法通过校验。

入场定位未成功时，`odom_calibrated` 保持为 `false`，行为树持续执行场地扫描；因此，即使检测到球，也会表现为头部一直转动。

`/home/booster/Workspace/K1_5v5_demo_1.7/src/brain/include/types.h` 中的预设尺寸为：

| 场地类型 | 长×宽 | 中圈直径 |
|---|---|---|
| `adult_size` | 14.16×9.22 米 | 3.08 米 |
| `kid_size` | 9×6 米 | 1.5 米 |

以上是代码模型尺寸，实际场地仍应测量白线围成的范围；本次切换为 `kid_size` 后恢复正常。

#### 解决方法

1. 修改机器人源码配置 `/home/booster/Workspace/K1_5v5_demo_1.7/src/brain/config/config.yaml`，将 `game` 下的 `field_type` 改为 `"kid_size"`。
2. 同步安装配置 `/home/booster/Workspace/K1_5v5_demo_1.7/install/brain/share/brain/config/config.yaml`，再重启 **brain**。启动时读取的是安装目录的配置。
3. 按 **LT+A** 重新验证定位和跟球，成功后再按 **LT+B** 进入比赛策略。

裁判机的 `division` 与机器人场地模型分开设置；机器人定位使用的是 brain 的 `game.field_type`。

#### 验证结果

本次已恢复原有定位代码。用户将场地类型设为 `kid_size` 后，反馈定位找球恢复正常；最终采用切换场地预设的方案。

## （二）关于裁判机可能会遇到的问题

### 1. 配置为 striker，Ready 后却按守门员入场

记录日期：2026-10-05。

#### 问题现象

机器人 `192.168.133.43` 配置为 71 队、1 号、`striker`，但裁判机进入 Ready 后，机器人走向己方球门。日志显示：

```text
PlayerID: 1    Role: goal_keeper    StartRole: striker
State: READY
Teammates: Alive: 0
```

#### 问题原因

`player_role` 是启动角色。当前开启了 `strategy.cooperation.enable_role_switch: true`，同时 P2、P3 未建立有效通信，程序因此采用裁判机指定的守门员编号作为兜底。

本次收到的裁判机数据中，71 队的守门员是 **1 号**，正好对应这台机器人，运行角色便改成了 `goal_keeper`。裁判机新建比赛时默认每队的守门员为 1 号，该编号可以在比赛界面修改。

#### 解决方法

例如希望 **1 号当前锋、2 号当守门员、3 号当前锋**：

1. 各机器人分别设置唯一的 `game.player_id`，启动角色依次为 `striker`、`goal_keeper`、`striker`，`game.team_id` 均为 71。
2. 裁判机点击 **Start**，进入比赛主界面，先保持 **Initial**。
3. 在 **71 队**面板按住键盘 **Shift**，点击由 `Substitute` 变成的 `Select` 按钮，再选择 **2 号**作为守门员。选择编号时可以松开 Shift。
4. 设置完成后再进入 **Ready**。操作入口见[官方说明](https://github.com/RoboCup-HumanoidSoccerLeague/GameController/blob/v7.0.0-rc.3/README.md#substitution)。

如果测试时需要固定角色，将各机器人的 `strategy.cooperation.enable_role_switch` 改为 `false`，同步源码和安装配置后重启 brain。该机器人实际读取的配置为：

`/home/booster/Workspace/K1_5v5_demo_1.7/install/brain/share/brain/config/config.yaml`

保留自动角色切换时，队友通信完整并稳定后，程序还会根据距己方球门的距离等条件动态选守门员。

#### 配置与验证

- `params.yaml` 设置比赛人数、计时、罚时等，不设置守门员编号。
- 全局 `teams.yaml` 中的 `number` 对应机器人的 `team_id`；颜色列表只提供球衣颜色选项。**颜色不对应 `player_id`，守门员也不限定为 1 号。**
- 裁判机只指定守门员编号，其余球员的前锋职责由 brain 决定。
- 固定前锋测试时，Ready 日志应显示 `Role: striker`、`StartRole: striker`。只看到 `StartRole: striker` 不能证明当前角色仍是前锋。
