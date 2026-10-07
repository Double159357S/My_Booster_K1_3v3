# 机器人配置工具

将整个 `setup` 文件夹放在目标项目根目录下，与 `src`、`scripts` 文件夹同级。

```text
项目根目录/
├── setup/
│   ├── set_up.py
│   ├── set_up.sh
│   ├── config.yaml.example
│   └── README.md
├── src/
└── scripts/
```

## 运行

在机器人 Ubuntu 的项目根目录执行：

```bash
python3 setup/set_up.py
```

保存配置成功后，`set_up.py` 会自动调用同目录的 `set_up.sh`，按裁判机 IP 所在的 `/24` 网段设置本机 SSH 防火墙，可能需要输入 `sudo` 密码。取消保存时不会修改防火墙。

Windows PowerShell 中也可以编辑配置：

```powershell
python .\setup\set_up.py
```

如果已经进入 `setup` 文件夹，直接执行 `python3 set_up.py`，Windows 使用 `python .\set_up.py`。

Windows 上仅保存配置，需将配置部署到机器人后应用防火墙。WSL 属于 Linux，运行时修改的是 WSL 的防火墙；要配置机器人，应在机器人上运行。

只重试 SSH 防火墙时，在机器人 Ubuntu 上执行：

```bash
bash setup/set_up.sh
```

不带参数时读取 `src/brain/config/config.yaml` 中已保存的 `game_control_ip`；也可显式传入裁判机 IP，例如 `bash setup/set_up.sh 192.168.133.210`。`.sh` 只修改防火墙，不会再次启动 `.py`。

网段沿用原来的 `/24` 掩码（`255.255.255.0`），例如 `192.168.133.210` 对应 `192.168.133.0/24`，`192.168.10.110` 对应 `192.168.10.0/24`。这是从输入 IP 推导的 `/24` 网段，不是检测裁判机网卡的真实子网掩码。

需要 `iptables` 和 root 权限或 `sudo`。脚本在独立的 `BOOSTER_SETUP_SSH` 链中设置规则：保留已建立的连接，允许裁判机网段的新 SSH 连接，拒绝其他新 SSH 连接。重复执行会更新该链中的网段规则，不累积旧网段。首次在 `INPUT` 最前面插入 SSH 跳转规则，避免已有 SSH 拒绝规则抢先命中。

规则作用于运行脚本的机器。如果防火墙步骤失败，配置文件仍已保存，脚本会返回失败状态；排查后可单独运行 `.sh` 重试。

两个脚本都会按自身位置查找文件，可以从其他目录通过完整路径运行。Python 脚本仅使用标准库，无需安装额外 Python 包。

## 配置内容

按提示依次设置：

1. `team_id`：队伍编号，1–255，与裁判机一致。
2. `player_id`：机器人编号，支持 1–6；3v3 同队通常使用 1、2、3，不能重复。
3. `field_type`：`adult_size`、`kid_size` 或 `robo_league`，默认模板为 `kid_size`。
4. `player_role`：`striker` 或 `goal_keeper`。
5. `player_start_pos`：`left` 或 `right`，对应己方半场入场边线。
6. `treat_person_as_robot`：`true` 用于调试，正式比赛使用 `false`。
7. `game_control_ip`：运行裁判机的机器 IP，同时写入接收白名单，并用于推导 SSH 放行网段。

每项提示都会显示 `config.yaml` 中的当前值，直接按回车即可保留，也可以输入新值修改。例如 `field_type` 当前为 `kid_size` 时，按回车保留，输入 `adult_size` 则切换场地。`false` 同样可以通过回车保留。

首次运行时默认值来自模板。`team_id: 0`、`player_id: 0`、`TODO` 等无效占位值不能用回车跳过，需要填写有效值。

已有配置时输入 `y` 进入逐项设置；单个设置可按回车保留，最终保存仍需输入 `y` 确认。保存后在 Linux 上自动应用防火墙，即使裁判机 IP 按回车保留，也会使用该 IP 设置规则。

脚本会更新目标项目的 `src/brain/config/config.yaml` 和 `src/game_controller/launch/launch.py`。如果 `config.yaml` 不存在，会从同文件夹的 `config.yaml.example` 创建。

在 `/home/booster` 存在且没有 `1.sh` 时，脚本会生成 `/home/booster/1.sh`，指向目标项目的 `scripts/start.sh`。

配置完成后，在项目根目录构建并启动：

```bash
colcon build
bash scripts/start.sh
```
