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
- 合并 brain 配置：更新本地策略参数，同时保留机器人的球员号、角色、初始侧、机器人校准、相机参数、裁判机和日志服务器地址。
- 保留 GameController 接收节点中的 IP 白名单设置。
- Linux 文本转为 LF 换行；Shell 脚本补齐执行权限。

覆盖前备份到机器人目录下的 `.robot-sync-backups/<备份编号>`，覆盖后校验哈希。单台复制失败时恢复已经覆盖的文件；多台并非一起提交，失败详情查看报告。报告位于本地工程的 `.robot-sync/<运行编号>/result.json`。

## 可选参数

只预览差异，在运行命令末尾加 `-DryRun`；预览仍会询问本地工程路径、IP 段和尾号。

已明确本地路径时，可用 `-LocalRoot "D:\Projects\My_Booster_K1_3v3"` 跳过路径询问。

同步后自动编译加 `-Build`，使用单并发构建 `brain`、`vision`、`game_controller` 及其依赖。使用此选项前先停止 Demo。脚本不启动或停止机器人程序；更新完成后自行重启 Demo。

已明确目标时可以省略地址询问：

```powershell
powershell -ExecutionPolicy Bypass -File ".\scripts\sync_robots.ps1" -IpPrefix 192.168.133 -RobotIds .43
```

回滚时加 `-RollbackBackup <备份编号>`，再输入需要回滚的目标；如果文件在同步后又被修改，脚本会拒绝覆盖这些修改。

程序使用 Windows Python 3.9+、PuTTY 的 `plink.exe`/`pscp.exe`（默认 `D:\putty`），机器人需要 Python 3.9+ 和 ROS 环境提供的 PyYAML。PuTTY 目录可通过 `-PuttyDirectory` 修改。SSH 主机指纹默认使用当前机器人的已验证指纹；换用不同主机指纹时通过 `-HostKey` 指定。

前锋协作通信协议升级仍需所有场上机器人更新、编译后一起重启比赛程序。可以分批同步，但不要混用新旧程序比赛。

## 验证

本地有 21 项测试，包含地址输入检查以及实际驱动与远端助手的完整预览、复制、配置保留、重复同步和回滚流程；测试使用临时目录模拟机器人，不通过 SSH 连接设备。在带有 PyYAML 的 Linux/WSL 中执行：

```bash
python3 scripts/deployment/test_robot_sync.py
```
