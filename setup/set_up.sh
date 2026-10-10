#!/bin/bash

set -e

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

# 仅设置 SSH 防火墙，由 set_up.py 在保存配置后调用，不再启动配置流程。
# 可选参数为裁判机 IPv4 地址；未传参数时读取项目中已保存的 game_control_ip。
if [[ $# -gt 1 ]]; then
    echo "用法：bash setup/set_up.sh [裁判机 IPv4 地址]" >&2
    exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "未找到 python3，请先安装 Python 3。" >&2
    exit 1
fi

if ! command -v iptables >/dev/null 2>&1; then
    echo "未找到 iptables，请在目标机器人的 Linux 系统上安装 iptables 后重试。" >&2
    exit 1
fi

# 沿用原脚本的 /24 掩码，先验证 IP，再执行任何防火墙修改。
SSH_NETWORK="$(python3 - "${SCRIPT_DIR}/../src/brain/config/config.yaml" "${1:-}" <<'PY'
import ipaddress
import re
import sys

try:
    address = sys.argv[2]
    if not address:
        with open(sys.argv[1], "r", encoding="utf-8") as config_file:
            content = config_file.read()
        match = re.search(r"""^\s*game_control_ip:\s*(?:"([^"]*)"|'([^']*)'|([^\s#]+))""", content, re.MULTILINE)
        if not match:
            raise ValueError("配置文件中未找到 game_control_ip，请先运行 set_up.py。")
        address = next(value for value in match.groups() if value is not None)
    address = ipaddress.IPv4Address(address)
    print(ipaddress.IPv4Network(f"{address}/24", strict=False))
except (OSError, ValueError) as error:
    print(f"无法获取裁判机网段：{error}", file=sys.stderr)
    sys.exit(1)
PY
)"

IPTABLES=(iptables -w 5)
if [[ ${EUID} -ne 0 ]]; then
    if ! command -v sudo >/dev/null 2>&1; then
        echo "需要 root 权限或 sudo 才能设置 SSH 防火墙。" >&2
        exit 1
    fi
    IPTABLES=(sudo iptables -w 5)
fi

echo "SSH 防火墙允许裁判机网段：${SSH_NETWORK}（/24，掩码 255.255.255.0）"

# 用独立链管理本工具的 SSH 规则，切换网段时只替换放行规则。
# INPUT 中既有的其他规则由系统或其他工具管理，不做删除。
SSH_CHAIN="BOOSTER_SETUP_SSH"
if "${IPTABLES[@]}" -C INPUT -p tcp --dport 22 -j "${SSH_CHAIN}" >/dev/null 2>&1; then
    # 已生效的链不清空，保留现有连接规则和拒绝规则；更新失败时仍使用旧网段。
    "${IPTABLES[@]}" -C "${SSH_CHAIN}" -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT
    "${IPTABLES[@]}" -C "${SSH_CHAIN}" -j DROP
    "${IPTABLES[@]}" -R "${SSH_CHAIN}" 2 -s "${SSH_NETWORK}" -j ACCEPT
else
    if ! "${IPTABLES[@]}" -nL "${SSH_CHAIN}" >/dev/null 2>&1; then
        "${IPTABLES[@]}" -N "${SSH_CHAIN}"
    fi

    # 完整建立规则后才接入 INPUT，也可恢复上次未完成的初始化。
    "${IPTABLES[@]}" -F "${SSH_CHAIN}"
    "${IPTABLES[@]}" -A "${SSH_CHAIN}" -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT
    "${IPTABLES[@]}" -A "${SSH_CHAIN}" -s "${SSH_NETWORK}" -j ACCEPT
    "${IPTABLES[@]}" -A "${SSH_CHAIN}" -j DROP

    # 放在 INPUT 最前面，避免旧的 SSH DROP 规则先命中。
    "${IPTABLES[@]}" -I INPUT 1 -p tcp --dport 22 -j "${SSH_CHAIN}"
fi

echo "SSH 防火墙设置完成：允许该网段的新连接，保留已建立的 SSH 连接，拒绝其他新 SSH 连接。"
