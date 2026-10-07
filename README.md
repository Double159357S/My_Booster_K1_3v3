# My_Booster_K1_3v3

机器人配置脚本和模板集中在 [`setup/`](setup/README.md)。

在项目根目录运行：

```bash
python3 setup/set_up.py
```

Linux 上保存配置后会自动按裁判机 IP 所在的 `/24` 网段设置本机 SSH 防火墙。
单独重试防火墙使用 `bash setup/set_up.sh`，会读取已保存的裁判机 IP。
Windows 配置参数使用 `python .\setup\set_up.py`，防火墙需在机器人 Linux 上应用。

详细步骤、配置项和共享文件夹的方法见 [配置工具说明](setup/README.md)。
