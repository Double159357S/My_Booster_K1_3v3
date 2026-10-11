# Questions

## （一）关于机器人可能会遇到的问题

### 1. 视觉节点收不到图像和 CameraInfo

记录日期：2026-10-05；更新日期：2026-10-07，机器人 `192.168.133.21`。

#### 问题现象

模型初始化成功，但没有图像和检测输出，日志提示：

```text
no valid CameraInfo received within 3.000 s; using YAML fallback
```

#### 问题原因

头部相机服务在错误的 **2053 年**启动，时间回拨到 **2026 年**后持续丢帧，日志出现 `drop one message to avoid timestamp disorder`。本次只重启处理服务未恢复，还需要重启原始采集服务。

#### 解决方法

先登录机器人主机，再登录头部板：

```bash
ssh booster@192.168.133.21
ssh root@192.168.127.10
```

头部密码取自主机 `/opt/booster/BoosterRos2/install/booster-x5-camera/booster_x5_manager.sh` 中的 `REMOTE_PASS`。

在头部检查时间，**确认系统时间正确后**重启两项服务：

```bash
date -Is
hwclock --show --utc
# 确认系统时间正确后执行
systemctl restart start_camera.service
systemctl restart start_stereo.service
```

仅当 RTC 错误时，再执行 `hwclock --systohc --utc`。本次 RTC 已正确，无需写入。

#### 验证结果

回到机器人主机检查：

```bash
cd /home/booster/Workspace/K1_5v5_demo_1.7
grep '\[vision\]\[intrinsics\]' vision.log | tail -5
```

出现 `using CameraInfo topic intrinsics`，图像和检测输出恢复。本次实测 RGB、CameraInfo 约 **20 Hz**，检测约 **16.7 Hz**；尚未验证重新开机后的恢复情况。

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

### 2. WSL 裁判机网络与 VS Code 1006 错误

记录日期：2026-10-07。

#### 问题现象

WSL 的 IP 为 `172.22.237.178`，机器人为 `192.168.133.43`。虽然能 ping 通，但裁判机的 UDP 广播不一定能到达机器人。

切换镜像网络并重启 WSL 后，VS Code 提示“无法获取远程环境”和 `WebSocket close with status code 1006`。

#### 问题原因

WSL 默认 NAT 网络不能直接向机器人所在局域网广播。重启 WSL 后，VS Code 的 WSL 辅助进程仍缓存旧连接，导致重连失败。

#### 解决方法

1. 在 Windows 11 22H2 或更新版本中，编辑 `C:\Users\wjc\.wslconfig`，在文件中设置：

   ```ini
   [wsl2]
   networkingMode=mirrored
   ```

   这是文件内容，不能直接粘贴到 Ubuntu 终端执行。

2. 保存工作，在 **Windows PowerShell** 执行，然后重新进入 Ubuntu：

   ```powershell
   wsl --shutdown
   ```

3. 如果 VS Code 仍报 1006，在 **Windows PowerShell** 重启 WSL 辅助进程：

   ```powershell
   Get-CimInstance Win32_Process -Filter "Name='Code.exe'" |
       Where-Object { $_.CommandLine -match 'ms-vscode-remote\.remote-wsl.*[\\/]wslDaemon\.js' } |
       ForEach-Object { Stop-Process -Id $_.ProcessId }
   ```

   回到 VS Code，按 **Ctrl + Shift + P**，执行 **Developer: Reload Window（重新加载窗口）**。

4. 在 Ubuntu 用 `ip -4 addr` 检查网卡。GameController 的 **Interface** 选择机器人所在网段的接口，本次为 **`eth2`（`192.168.133.210`）**。确认防火墙允许 UDP **3838**（比赛状态）和 **3939**（机器人回报）。

#### 验证结果

镜像网络已生效，WSL 能 ping 通机器人，VS Code 连接已恢复。**后续已验证 UDP 双向通信，回包防火墙问题见第 4 项。**

### 3. WSL 中 GameController 窗口不可见，并出现 EGL/DRI3 警告

记录日期：2026-10-07。

#### 问题现象与原因

执行 `./game_controller` 后窗口不可见，终端出现 EGL/Mesa/DRI3 警告，窗口标题带有 `[WARN:COPY MODE]`。

原因是硬件渲染异常，同时 WSLg 共享内存显示通道初始化失败，日志显示 `use_gfxredir = 0`。

#### 解决方法

1. 在 Ubuntu 的 `game_controller` 启动脚本中，原有 `exec` 命令前加入：

   ```bash
   if [[ "$(uname -r)" == *[Mm]icrosoft* ]]; then
     export GDK_BACKEND="${GDK_BACKEND:-x11}"
     export WEBKIT_DISABLE_COMPOSITING_MODE="${WEBKIT_DISABLE_COMPOSITING_MODE:-1}"
     export LIBGL_ALWAYS_SOFTWARE="${LIBGL_ALWAYS_SOFTWARE:-1}"
   fi
   ```

   本次文件路径：`/home/wjc/game_controller-7.0.0-rc.3-4-x86_64-unknown-linux-gnu/game_controller`。

2. 保存 Linux 图形程序中的工作，在 **Windows PowerShell** 重启 WSLg 图形服务：

   ```powershell
   wsl --system --exec pkill -TERM -x weston
   ```

3. 回到 Ubuntu 的裁判机目录，重新启动：

   ```bash
   ./game_controller
   ```

本次排查还尝试过临时挂载 `/mnt/shared_memory`，恢复后已清理；该步骤的必要性未单独验证。

#### 验证结果

Windows 上的 `Kid Size` 窗口恢复，菜单可操作，EGL/DRI3 警告消失，日志变为 `use_gfxredir = 1`。

参考：[微软 WSL 同类问题 #40618](https://github.com/microsoft/WSL/issues/40618)。

### 4. 机器人收到比赛数据，但裁判机仍显示红叉

记录日期：2026-10-07。

**原因：**Windows 的 WSL/Hyper-V 防火墙拦截了 UDP 3939 状态回包。

**设置位置：**用**管理员 Windows PowerShell** 管理 WSL/Hyper-V 防火墙。规则永久保存，重启后仍有效。

**解决：**一次放行机器人所在网段的 UDP 3939，之后同网段机器人换 IP 无需逐台设置。本次已创建以下规则，不必重复执行：

```powershell
New-NetFirewallHyperVRule -Name 'GameController-WSL-UDP3939-RobotLAN' `
  -DisplayName 'GameController robot status - local network UDP 3939' `
  -PolicyStore PersistentStore -Direction Inbound `
  -VMCreatorId '{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}' `
  -Protocol UDP -LocalAddresses Any -LocalPorts 3939 `
  -RemoteAddresses '192.168.133.0/24' -Action Allow -Enabled True
```

查看当前设置：

```powershell
Get-NetFirewallHyperVRule -Name 'GameController-WSL-UDP3939-RobotLAN' |
  Format-List DisplayName, Enabled, Protocol, LocalPorts, RemoteAddresses
```

**结果：**已停用原来的逐台 IP 规则，Team70 的 1、2、3 号机器人全部显示绿色勾。

更换网络导致网段变化时，需要修改规则的 `RemoteAddresses`。电脑 IP 变化时，机器人配置中的 `game_control_ip` 仍需同步更新。

参考：[微软 Hyper-V 防火墙说明](https://learn.microsoft.com/en-us/windows/security/operating-system-security/network-security/windows-firewall/hyper-v-firewall)。

### 5. 己方禁区无人处理球、队友不能共享球位置

记录日期：2026-10-11。

**发现：**`.43` 上一次日志显示两名队友消息过期、在线数为 0；另外，原策略会丢弃近距离共享球，守门员看不见球时也不能凭队友位置申请清球。

**修改：**允许共享新鲜的近球位置，守门员可申请控球并接近，踢球前由本机视觉确认；近球 2 秒仍看不见时释放资格给队友。新增 `[Team]` 日志用于检查通信和控球分配。

**复测：**`.43`、`.21`、`.97` 全部同步、编译、重启 Demo，保留各自球员号。运行 `grep '\[Team\]' brain.log | tail -n 10` 查看；正常上场且定位完成时应为 `online=2/2`。如果持续为 0，检查三台版本、队号、通信开关，以及机器人之间 UDP 10070（队号 70）的网络连通性。裁判机绿色勾不代表队友通信正常。

### 6. 守门员确认球后清球慢、反复调整

**发现：**`.97` 日志多次出现视觉踢球被行为树中断。自动选守门员时排除了正在踢球的球员，导致禁区清球中的守门员变成前锋，踢球被取消。

**修改：**禁区清球期间保留角色；确认球后持续跟踪，不重新扫头；启动站稳时间改为 80 毫秒。未真正启动 SDK 不计踢球冷却；已对准但在冷却或队友挡路时站定等待。

**复测：**同步后观察 `[Goalie]` 和 `[RLVisionKick]` 日志，比较同一球位的清球准备时间。网络仍需满足队友通信及控球授权，代码回归通过不能代替实机效果验证。

### 7. 对照原 Demo 检查追球和踢球退步

**发现：**新增的严格对准条件也在 SDK 踢球期间反复判断，容易中断脚位调整；路线已对准时仍继续绕球；远距离追球和接应速度低于原 Demo。守门员逐个比较候选距离，也可能因各台先处理自己而选出不同球员。

**修改：**SDK 接管后由退出逻辑处理失球、无进展和队友距离；路线达标后停止绕球；恢复原来的远距离速度上限，近球及靠近队友时仍减速；统一守门员选人次序规则。

**同步：**完成复制后选择 `1` 仅同步、`2` 并行编译、`3` 并行编译后加载环境并启动。默认仅同步；所选机器人同时编译，全部成功才启动。最新策略由使用者手动编译和实测。

### 8. 禁区聚集后谁都不踢球

**分析：**代码存在“主控等队友让开，非主控守门员仍往同一位置拦截”的互相等待；后方队友也会触发原来的 0.8 米整圈停球。共享球的旧主控还可能压过直接看见近球的候选。本次未取得故障时的比赛日志，现场原因仍需复测确认。

**修改：**非主控先让出清球通道，守门员让开后暂时站定；检查实际踢球通道；优先授权直接看见近球的合格机器人，保留单一球权和避碰。

**复测：**三台同步后手动编译，观察 `[Team]`、`[Goalie]`、`[Yield]`、`[RLVisionKick]`；新日志可区分未分出球权、队友挡路和球源失效。Python 路由检查通过，C++ 尚未编译、实机尚未验证。详情见 [球权分析](../../../src/brain/BALL_OWNERSHIP.md)。
