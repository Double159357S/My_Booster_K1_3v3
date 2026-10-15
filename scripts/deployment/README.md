# 同步本地代码到机器人

在本地工程根目录打开 Windows PowerShell，运行：

```powershell
powershell -ExecutionPolicy Bypass -File ".\scripts\sync_robots.ps1"
```

脚本依次询问：

1. 本地工程路径；直接回车使用脚本所在的工程目录，也可以输入其他目录，支持带空格或外围引号的路径。
2. IP 段，只填前三段，例如 `192.168.133`；填错会立即提示重新输入。
3. 机器人尾号，例如 `.43`；多台可输入 `.43,.21`，也支持 `43 21`。
4. SSH 密码，本次运行只输入一次，适用于所选机器使用相同密码的情况。
5. 同步成功后选择后续操作：`1` 仅同步（直接回车也是此项）、`2` 编译、`3` 编译并启动 Demo。预览和回滚不询问此项。

**没有默认机器人，也不会扫描或自动选择其他 IP。** 空尾号会退出。

输入示例：

```text
请输入 IP 段（前三段，例如 192.168.133）: 192.168.133
请输入机器人尾号（最后一段，例如 .43 或 .43,.21）: .43,.21
本次目标：192.168.133.43, 192.168.133.21
```

本地路径不固定；机器人目录默认为 `/home/booster/Workspace/K1_5v5_demo_1.7`，SSH 用户为 `booster`。

## 同步内容

- 比较 `src`、`scripts`、`setup`、`configs`、`utils`、`distribution` 中的文件，以本地代码为准，只复制新增和有差异的文件。
- 排除 `build`、`install`、日志、缓存、压缩包和本地编译产物；保留源码随附的 `.so`、`.a` 依赖库。
- 保留机器人独有文件，不删除本地没有的文件。
- 保留相机/场地标定 YAML、Fast DDS 配置和 `*_local.yaml`。
- 合并 `src/brain/config/config.yaml`：保留机器人 `game` 下的全部参数，以及 `rerunLog.server_ip`、`rerunLog.log_dir`；其余参数全部按本地更新，包括 `robot`、`vision` 和 `game_control_ip`。
- 保留 GameController 接收节点中的 IP 白名单设置。
- Linux 文本转为 LF 换行；Shell 脚本补齐执行权限。

`game` 整块保留机器人配置，包括 `team_id`、`player_id`、`field_type`、`player_role`、`player_start_pos`、`treat_person_as_robot`、`number_of_players` 及机器人自有的其他字段。本地新增的 `game` 字段也不会被加入。日志服务器和存储目录同样保留原值；其他日志选项仍更新。

以上规则只针对 brain 的 `config.yaml` 参数合并。单独的视觉标定 YAML、`*_local.yaml` 和 `configs/fastdds.xml` 仍按原规则保留。若机器人有 `config_local.yaml`，它的同名参数仍可能覆盖更新后的 `config.yaml`。

覆盖前备份到机器人目录下的 `.robot-sync-backups/<备份编号>`，覆盖后校验哈希。单台复制失败时恢复已经覆盖的文件；多台并非一起提交，失败详情查看报告。报告位于本地工程的 `.robot-sync/<运行编号>/result.json`。

## 可选参数

只预览差异，在运行命令末尾加 `-DryRun`；预览仍会询问本地工程路径、IP 段和尾号。

已明确本地路径时，可用 `-LocalRoot "D:\Projects\My_Booster_K1_3v3"` 跳过路径询问。

默认只同步代码。同步成功后，菜单如下：

```text
同步完成。请选择后续操作：1 仅同步（默认）；2 编译；3 编译并启动 Demo:
```

此时文件已经同步。输入数字后按回车；直接回车或 `Ctrl+C` 都只结束操作选择，不编译、不启动。SSH 子进程不读取键盘，菜单会恢复 Windows 的行输入和字符回显，并暂时关闭鼠标选择暂停。如果旧脚本停在这里无法输入，先按 `Esc` 取消控制台文字选择，再输入 `1` 回车；也可以按 `Ctrl+C` 退出，已同步的文件不会撤回。

- `1`：结束同步，显示手动编译、加载环境和启动命令。
- `2`：所选机器人同时编译，每台通过 `./scripts/build.sh` 并行构建工程中的全部包；结束后由你启动。
- `3`：先同时编译所有目标；全部成功后，在各台同一个 SSH shell 内执行 `source install/setup.bash` 和 `./scripts/start.sh`。任何一台编译失败都不会进入启动步骤。

编译前请在机器人上退出 AI、停止 Demo。选择 `3` 会执行现有 `start.sh` 中的停止旧节点和启动流程。启动命令完成不等于已经确认视觉、通信和比赛状态正常。

也可以用 `-Build` 直接选择编译，或 `-Start` 直接选择编译并启动，跳过操作菜单。每台按 `nproc` 设置并发；有依赖关系的包会等依赖完成，独立包并行构建。

选择仅同步后，你在机器人终端手动执行：

```bash
cd ~/Workspace/K1_5v5_demo_1.7
./scripts/build.sh && source install/setup.bash && ./scripts/start.sh
```

编译失败会保留失败退出码，后面的加载环境和启动不会执行。使用 `-RemoteRoot` 时，脚本显示的是对应目录的命令。

编译时显示完成包数、进度条、正在编译的包和总耗时，例如：

```text
192.168.133.43 | 4/7 pkgs | brain,vision [#########-------] packages | 02:15
```

`4/7` 表示已有 4 个包完成，进度条按包数计算，不代表耗时完成了 4/7。并行输出中的 CMake 百分比无法准确归属某个包，因此不显示混杂的单包百分比。全部成功后显示 `OK`，失败时显示 `FAILED`。完整日志保存在 `.robot-sync/<运行编号>/<机器人IP>.build.log`，每台分别记录。多台同时编译时使用带 IP 的进度行，避免进度条互相覆盖。

如果旧构建的普通目录阻碍创建符号链接（报错 `existing path cannot be removed: Is a directory`），脚本会先把对应的 ROS 消息生成目录备份到 `.robot-sync-backups/<运行编号>/build-conflicts`，再编译。手动运行 `build.sh`、`build_brain.sh`、`build_debug.sh` 也会执行这一检查；不带 `--symlink-install` 时，仍会处理 CMake 缓存中已开启符号链接安装的包。普通复制安装的包保持原样。无需手动删除整个 `build` 或 `install`。

已明确目标时可以省略地址询问：

```powershell
powershell -ExecutionPolicy Bypass -File ".\scripts\sync_robots.ps1" -IpPrefix 192.168.133 -RobotIds .43
```

回滚时加 `-RollbackBackup <备份编号>`，再输入需要回滚的目标；如果文件在同步后又被修改，脚本会拒绝覆盖这些修改。

程序使用 Windows Python 3.9+、PuTTY 的 `plink.exe`/`pscp.exe`（默认 `D:\putty`），机器人需要 Python 3.9+ 和 ROS 环境提供的 PyYAML。PuTTY 目录可通过 `-PuttyDirectory` 修改。SSH 主机指纹默认使用当前机器人的已验证指纹；换用不同主机指纹时通过 `-HostKey` 指定。

前锋协作通信协议升级仍需所有场上机器人更新、编译后一起重启比赛程序。可以分批同步，但不要混用新旧程序比赛。

## 验证

测试包含地址输入、同步、配置保留、回滚、并行进度、多台同时构建、菜单默认仅同步、键盘输入隔离、Windows 输入模式恢复、取消菜单保留同步结果、编译失败阻止启动及环境加载顺序。使用临时目录和模拟命令，不通过 SSH 连接设备，默认不执行编译。在带有 PyYAML 的 Linux/WSL 中执行：

```bash
python3 scripts/deployment/test_robot_sync.py
```
