#!/usr/bin/env python3
"""
Booster K1 配置脚本（5v5 代码，用于 3v3 比赛）
用于快速配置机器人参数：team_id, player_id, field_type, player_role, player_start_pos,
treat_person_as_robot, game_control_ip
"""

import os
import re
import sys
import shlex
import shutil
import ipaddress
import subprocess

# 设置工具位于项目根目录下的 setup 文件夹。
script_dir = os.path.dirname(os.path.abspath(__file__))
project_root = os.path.dirname(script_dir)

# 模板与脚本放在一起，实际配置写入项目的 src 目录。
CONFIG_FILE = os.path.join(project_root, "src", "brain", "config", "config.yaml")
CONFIG_FILE_EXAMPLE = os.path.join(script_dir, "config.yaml.example")
GAME_CONTROL_LAUNCH_PY = os.path.join(project_root, "src", "game_controller", "launch", "launch.py")
QUICKSTART_FILE = "/home/booster/1.sh"

# 球员编号沿用原项目范围，不限制为 3v3 的 1、2、3。
VALID_PLAYER_IDS = [1, 2, 3, 4, 5, 6]
VALID_FIELD_TYPES = ["adult_size", "kid_size", "robo_league"]
VALID_ROLES = ["striker", "goal_keeper"]
VALID_START_POSITIONS = ["left", "right"]
CONFIG_KEYS = (
    "team_id", "player_id", "field_type", "player_role", "player_start_pos",
    "treat_person_as_robot", "game_control_ip",
)
CONFIG_VALUE_PATTERN = r"""(?:"([^"\r\n]*)"|'([^'\r\n]*)'|([^ \t\r\n#]+))"""

def gen_quickstart_sh():
    """为当前项目生成快捷启动入口，使用已运行的 Booster 头部相机。"""
    target = QUICKSTART_FILE
    if not os.path.isdir(os.path.dirname(target)):
        print("未找到 /home/booster，跳过生成快捷启动脚本")
        return
    if os.path.exists(target):
        print(f"快捷启动脚本已存在，保留现有文件：{target}")
        return
    
    commands = [
        "#!/bin/bash",
        "set -e",
        "# Booster 头部相机由相机服务提供，不额外启动 RealSense 驱动。",
        f"cd -- {shlex.quote(project_root)}",
        'exec bash ./scripts/start.sh "$@"',
    ]
    content = "\n".join(commands) + "\n"
    with open(target, "w", encoding="utf-8", newline="\n") as f:
        f.write(content)
        
    os.chmod(target, 0o755)
    print("已创建快速启动 1.sh")

# def copy_latest_vision_config():
#     """复制手眼标定配置文件"""
#     target_config = os.path.join(project_root, "src", "vision", "config", "vision.yaml")
#     source_config = "/opt/booster/vision.yaml"

#     if not os.path.exists(source_config):
#         print(f"⚠️  最新标定文件夹中找不到 vision_local.yaml: {source_config}")
#         return

#     # 确保目标目录存在
#     target_dir = os.path.dirname(target_config)
#     if not os.path.exists(target_dir):
#         os.makedirs(target_dir)

#     shutil.copy2(source_config, target_config)
#     print(f"✅ 已复制最新手眼标定配置：/opt/booster/vision.yaml → src/vision/config/vision.yaml")


def make_scripts_executable():
    """赋予 scripts 目录下所有文件可执行权限"""
    scripts_dir = os.path.join(project_root, "scripts")
    if os.path.exists(scripts_dir):
        for filename in os.listdir(scripts_dir):
            filepath = os.path.join(scripts_dir, filename)
            if os.path.isfile(filepath):
                try:
                    os.chmod(filepath, 0o755)
                except Exception:
                    pass
                
    calib_script = os.path.join(project_root, "calibration.sh")
    try:
        os.chmod(calib_script, 0o755)
        print("chmoded calib script")
    except Exception:
        print("couldn't chmod calib script")


def read_config_values(config_content):
    """读取可配置项的当前值，支持带引号和不带引号的 YAML 标量。"""
    values = {}
    for key in CONFIG_KEYS:
        pattern = rf"^[ \t]*{key}:[ \t]*{CONFIG_VALUE_PATTERN}"
        match = re.search(pattern, config_content, re.MULTILINE)
        if match:
            values[key] = next(value for value in match.groups() if value is not None)
    return values


def print_current_config(config_content):
    """显示当前配置"""
    print("-" * 60)
    print("当前配置:")

    values = read_config_values(config_content)
    for key in CONFIG_KEYS:
        value = values.get(key, "")
        status = "✗ 待配置" if value in ["", "0", "TODO"] else "✓"
        print(f"  {key:21s}: {value:25s} [{status}]")

    # 读取 launch.py 中的 ip_white_list
    if os.path.exists(GAME_CONTROL_LAUNCH_PY):
        with open(GAME_CONTROL_LAUNCH_PY, "r", encoding="utf-8") as f:
            launch_content = f.read()
        ip_white_list_match = re.search(r'"ip_white_list":\s*\[([\s\S]*?)\]', launch_content)
        if ip_white_list_match:
            ip_list_raw = ip_white_list_match.group(1).strip()
            # 提取所有 IP 地址
            ips = re.findall(r'"([^"]+)"', ip_list_raw)
            if ips:
                ip_value = ', '.join(ips)
                status = "✗ 待配置" if "TODO" in ip_value else "✓"
                print(f"  ip_white_list:    {ip_value:25s} [{status}]")
            else:
                print(f"  ip_white_list:    (空)                      [✗ 待配置]")

    print("-" * 60)
    print()


def create_config_from_example():
    """如果 config.yaml 不存在，从 config.yaml.example 创建"""
    if os.path.isfile(CONFIG_FILE):
        print(f"✅ 配置文件已存在：{CONFIG_FILE}\n")
        return

    if not os.path.exists(CONFIG_FILE_EXAMPLE):
        print(f"❌ 错误：找不到配置文件模板：{CONFIG_FILE_EXAMPLE}")
        sys.exit(1)

    print("未找到 config.yaml，正在从 config.yaml.example 创建...")
    shutil.copy2(CONFIG_FILE_EXAMPLE, CONFIG_FILE)
    print(f"✅ 已创建配置文件：{CONFIG_FILE}\n")


def input_with_validation(prompt, validator, error_msg, default=None):
    """带验证的输入函数，回车保留当前有效值，非法占位值仍须修改。"""
    if default is not None:
        default = str(default).strip()
        hint = "回车保持" if validator(default) else "需填写有效值"
        prompt = f"{prompt.rstrip(': ')} [当前值：{default}，{hint}]: "

    while True:
        user_input = input(prompt).strip()
        if not user_input and default is not None:
            user_input = default
        if validator(user_input):
            return user_input
        print(f"  ❌ {error_msg}")


def get_team_id(default=None):
    """获取 team_id"""
    print("\n【步骤 1/7】设置队伍 ID (team_id)")
    print("  说明：必须与 GameControl 裁判系统的队伍 ID 保持一致")
    print("  有效范围：1-255")

    def validator(val):
        try:
            num = int(val)
            return 1 <= num <= 255
        except ValueError:
            return False

    return input_with_validation(
        "  请输入 team_id: ",
        validator,
        "无效的队伍 ID，请输入 1-255 之间的整数",
        default=default,
    )


def get_player_id(default=None):
    """获取 player_id"""
    print("\n【步骤 2/7】设置球员 ID (player_id)")
    print(f"  说明：机器人在队伍中的编号")
    print(f"  有效选项：{VALID_PLAYER_IDS}")

    def validator(val):
        try:
            num = int(val)
            return num in VALID_PLAYER_IDS
        except ValueError:
            return False

    return input_with_validation(
        "  请输入 player_id: ",
        validator,
        f"无效的球员 ID，请输入 {VALID_PLAYER_IDS} 中的一个数字",
        default=default,
    )


def get_field_type(default=None):
    """获取 field_type，回车保持当前场地类型，输入新值即可修改。"""
    print("\n【步骤 3/7】设置场地类型 (field_type)")
    print("  说明：如需修改请输入新场地类型，按回车保持当前设置")
    print(f"  有效选项：{VALID_FIELD_TYPES}")

    def validator(val):
        return val.lower() in VALID_FIELD_TYPES

    return input_with_validation(
        "  请输入 field_type: ",
        validator,
        f"无效的场地类型，请输入 {'、'.join(VALID_FIELD_TYPES)}",
        default=default,
    ).lower()


def get_player_role(default=None):
    """获取 player_role"""
    print("\n【步骤 4/7】设置球员角色 (player_role)")
    print(f"  说明：机器人在场上的角色")
    print(f"  有效选项：{VALID_ROLES}")
    print("    - striker: 前锋，负责进攻")
    print("    - goal_keeper: 守门员，负责防守球门")

    def validator(val):
        return val.lower() in VALID_ROLES

    return input_with_validation(
        "  请输入 player_role: ",
        validator,
        f"无效的角色，请输入 {' 或 '.join(VALID_ROLES)}",
        default=default,
    ).lower()


def get_player_start_pos(default=None):
    """获取 player_start_pos"""
    print("\n【步骤 5/7】设置入场边线 (player_start_pos)")
    print("  说明：从己方半场的左侧或右侧边线上场，与实际摆放位置一致")
    print(f"  有效选项：{VALID_START_POSITIONS}")
    print("    - left: 左侧边线")
    print("    - right: 右侧边线")

    def validator(val):
        return val.lower() in VALID_START_POSITIONS

    return input_with_validation(
        "  请输入 player_start_pos: ",
        validator,
        "无效的入场边线，请输入 left 或 right",
        default=default,
    ).lower()


def get_treat_person_as_robot(default=None):
    """获取 treat_person_as_robot"""
    print("\n【步骤 6/7】设置是否将人类视为机器人 (treat_person_as_robot)")
    print("  说明：用于调试时将检测到的人类当作机器人处理")
    print("    - true: 开启，用于调试")
    print("    - false: 关闭，正式比赛时使用")

    def validator(val):
        return val.lower() in ["true", "false"]

    return input_with_validation(
        "  请输入 treat_person_as_robot (true/false): ",
        validator,
        "无效的布尔值，请输入 true 或 false",
        default=default,
    ).lower()


def get_game_control_ip(default=None):
    """获取 game_control_ip"""
    print("\n【步骤 7/7】设置裁判机 IP 地址 (game_control_ip)")
    print("  说明：运行 GameControl 裁判系统的机器 IP 地址")
    print("  格式：标准的 IPv4 地址，如 192.168.1.100")
    print("  提示：该 IP 将同时用于 ip_white_list，所在 /24 网段用于 SSH 防火墙")

    def validator(val):
        try:
            ipaddress.IPv4Address(val)
            return True
        except ipaddress.AddressValueError:
            return False

    return input_with_validation(
        "  请输入 game_control_ip: ",
        validator,
        "无效的 IP 地址，请输入格式如 192.168.1.100",
        default=default,
    )


def update_config_file(
    team_id, player_id, field_type, player_role, player_start_pos, treat_person_as_robot, game_control_ip
):
    """更新配置文件"""
    if not os.path.exists(CONFIG_FILE):
        print(f"\n❌ 错误：配置文件不存在：{CONFIG_FILE}")
        return False

    with open(CONFIG_FILE, "r", encoding="utf-8") as f:
        content = f.read()

    # 替换配置值
    replacements = {
        "team_id": team_id,
        "player_id": player_id,
        "field_type": f'"{field_type}"',
        "player_role": f'"{player_role}"',
        "player_start_pos": f'"{player_start_pos}"',
        "treat_person_as_robot": treat_person_as_robot,
        "game_control_ip": f'"{game_control_ip}"',
    }

    for key, value in replacements.items():
        pattern = rf"(^[ \t]*{key}:[ \t]*){CONFIG_VALUE_PATTERN}"
        content, count = re.subn(
            pattern, lambda match: match.group(1) + value, content, flags=re.MULTILINE
        )
        if count != 1:
            print(f"\n❌ 配置字段 {key} 缺失或重复，未保存配置，请检查 config.yaml。")
            return False

    # 写回文件
    with open(CONFIG_FILE, "w", encoding="utf-8") as f:
        f.write(content)

    return True


def update_launch_file(game_control_ip):
    """更新 launch.py 中的 ip_white_list，使用 game_control_ip"""
    if not os.path.exists(GAME_CONTROL_LAUNCH_PY):
        print(f"\n❌ 错误：launch 文件不存在：{GAME_CONTROL_LAUNCH_PY}")
        return False

    with open(GAME_CONTROL_LAUNCH_PY, "r", encoding="utf-8") as f:
        content = f.read()

    # 将 IP 转换为 Python 列表格式
    ip_list_str = f'"{game_control_ip}"'

    # 替换 ip_white_list 内容
    pattern = r'("ip_white_list":\s*\[)([\s\S]*?)(\])'
    replacement = r'\g<1>\n                        ' + ip_list_str + r'\n                        \g<3>'

    content = re.sub(pattern, replacement, content)

    # 写回文件
    with open(GAME_CONTROL_LAUNCH_PY, "w", encoding="utf-8") as f:
        f.write(content)

    return True


def apply_ssh_firewall(game_control_ip):
    """配置保存成功后，在 Linux 上调用独立的 SSH 防火墙脚本。"""
    if not sys.platform.startswith("linux"):
        print("\n当前系统不是 Linux，已保存配置，未设置 SSH 防火墙。")
        print("请将配置部署到机器人，在机器人 Ubuntu 上运行 bash setup/set_up.sh。")
        return True

    firewall_script = os.path.join(script_dir, "set_up.sh")
    print("\n正在自动设置本机 SSH 防火墙，可能需要输入 sudo 密码...", flush=True)
    try:
        subprocess.run(["bash", firewall_script, game_control_ip], check=True)
    except (OSError, subprocess.CalledProcessError) as error:
        print(f"\n❌ 配置已保存，但 SSH 防火墙设置失败：{error}")
        print("请排查上述错误，然后在机器人上运行 bash setup/set_up.sh 重试。")
        return False
    return True


def main():
    # 确认工具放在目标项目中，避免在错误目录生成启动入口或修改配置。
    if not os.path.isdir(os.path.dirname(CONFIG_FILE)) or not os.path.isfile(GAME_CONTROL_LAUNCH_PY):
        print("❌ 错误：未找到目标项目的 brain 配置目录或 game_controller/launch/launch.py。")
        print("   请将整个 setup 文件夹放在项目根目录下，与 src、scripts 文件夹同级。")
        sys.exit(1)

    # 赋予 scripts 目录下所有文件可执行权限
    make_scripts_executable()
    print("已赋予 scripts 目录下所有文件可执行权限\n")

    # 复制最新的手眼标定配置
    # copy_latest_vision_config()
    # print()
    
    gen_quickstart_sh()
    print()

    # 从 example 创建 config.yaml（如果不存在）
    create_config_from_example()

    # 读取当前配置
    with open(CONFIG_FILE, "r", encoding="utf-8") as f:
        current_config = f.read()

    print_current_config(current_config)
    current_values = read_config_values(current_config)

    # 检查是否已经有有效配置
    has_todo = any(current_values.get(key) in [None, "", "0", "TODO"] for key in CONFIG_KEYS)

    if not has_todo:
        print("⚠️  当前配置似乎已经完成。")
        print("   继续可逐项修改，按回车保留当前有效值。")
        cont = input("   是否继续？(y/n): ").strip().lower()
        if cont != 'y':
            print("已取消。")
            return

    print("\n请按照提示输入配置参数...")
    print("每项按回车可保留当前有效值；0、TODO 等无效占位值需要重新填写。")
    print()

    # 获取所有配置
    team_id = get_team_id(current_values.get("team_id"))
    player_id = get_player_id(current_values.get("player_id"))
    field_type = get_field_type(current_values.get("field_type"))
    player_role = get_player_role(current_values.get("player_role"))
    player_start_pos = get_player_start_pos(current_values.get("player_start_pos"))
    treat_person_as_robot = get_treat_person_as_robot(current_values.get("treat_person_as_robot"))
    game_control_ip = get_game_control_ip(current_values.get("game_control_ip"))

    # 显示确认信息
    print("\n" + "=" * 60)
    print("请确认以下配置:")
    print("-" * 60)
    print(f"  team_id:          {team_id}")
    print(f"  player_id:        {player_id}")
    print(f"  field_type:       {field_type}")
    print(f"  player_role:      {player_role}")
    print(f"  player_start_pos: {player_start_pos}")
    print(f"  treat_person_as_robot: {treat_person_as_robot}")
    print(f"  game_control_ip:  {game_control_ip}")
    print(f"  ip_white_list:    {game_control_ip} (与 game_control_ip 相同)")
    ssh_network = ipaddress.IPv4Network(f"{game_control_ip}/24", strict=False)
    print(f"  ssh_allow_network: {ssh_network} (掩码 255.255.255.0)")
    print("-" * 60)

    if sys.platform.startswith("linux"):
        print("保存后将自动设置本机 SSH 防火墙：允许上述网段的新连接，保留已建立的连接。")

    confirm = input("确认保存配置？(y/n): ").strip().lower()
    if confirm != 'y':
        print("\n已取消，配置未保存。")
        return

    # 更新配置文件
    if update_config_file(
        team_id, player_id, field_type, player_role, player_start_pos, treat_person_as_robot, game_control_ip
    ):
        print("\n✅ 配置已成功保存!")
        print(f"   文件：{CONFIG_FILE}")
    else:
        print("\n❌ 配置文件保存失败!")
        sys.exit(1)

    # 更新 launch 文件（使用 game_control_ip 作为 ip_white_list）
    if update_launch_file(game_control_ip):
        print(f"   文件：{GAME_CONTROL_LAUNCH_PY}")
    else:
        print("\n❌ launch 文件保存失败!")
        sys.exit(1)

    if not apply_ssh_firewall(game_control_ip):
        sys.exit(1)

    print("\n提示：配置完成后，先colcon build，再运行 ./scripts/start.sh 启动机器人系统")


if __name__ == "__main__":
    main()
