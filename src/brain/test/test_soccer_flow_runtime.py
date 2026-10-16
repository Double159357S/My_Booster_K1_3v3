"""Execute production search/decision/SDK request functions with offline boundaries."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


def block(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    tokens = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/|[{}]', re.S)
    depth = 0
    for token in tokens.finditer(source, opening):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return source[start:token.end()]
    raise ValueError(f"Unterminated C++ block: {signature}")


def match_kick_parameters(config):
    """Read numeric kick settings from the shipped, mapping-only ROS YAML."""
    parents = []
    parameters = {}
    for line in config.splitlines():
        content = line.split("#", 1)[0].strip()
        if not content or ":" not in content:
            continue
        indent = len(line) - len(line.lstrip())
        key, value = (part.strip() for part in content.split(":", 1))
        while parents and parents[-1][0] >= indent:
            parents.pop()
        if not value:
            parents.append((indent, key))
            continue
        path = ".".join([name for _, name in parents][2:] + [key])
        if path != "strategy.enable_auto_visual_kick" and not path.startswith(
                ("strategy.auto_visual_kick", "strategy.goalie_visual_kick")):
            continue
        parameters[path] = float({"true": "1", "false": "0"}.get(value, value))
    if not parameters:
        raise ValueError("No match kick settings found")
    return ",\n".join("{" + json.dumps(key) + ", " + str(value) + "}"
                       for key, value in parameters.items())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default="g++")
    args = parser.parse_args()
    brain = Path(__file__).resolve().parents[1]
    tree = (brain / "src/brain_tree.cpp").read_text(encoding="utf-8")
    brain_source = (brain / "src/brain.cpp").read_text(encoding="utf-8")
    headers = (brain / "include/brain_tree.h").read_text(encoding="utf-8")
    client = (brain / "src/robot_client.cpp").read_text(encoding="utf-8")
    client_header = (brain / "include/robot_client.h").read_text(encoding="utf-8")
    declarations = "\n".join(block(headers, f"class {name} :") + ";" for name in
                             ("RLVisionKick", "MoveToFarSetPlaySearchArea", "StrikerDecide", "CamTrackBall", "Chase", "Adjust"))
    methods = "\n".join(block(tree, signature) for signature in (
        "NodeStatus MoveToFarSetPlaySearchArea::onStart()",
        "NodeStatus MoveToFarSetPlaySearchArea::onRunning()",
        "void MoveToFarSetPlaySearchArea::onHalted()", "NodeStatus StrikerDecide::tick()", "NodeStatus CamTrackBall::tick()",
        "NodeStatus Chase::tick()", "NodeStatus Adjust::tick()",
        "NodeStatus RLVisionKick::onStart()", "NodeStatus RLVisionKick::onRunning()",
        "void RLVisionKick::onHalted()", "void RLVisionKick::recordExitTime()",
        "void RLVisionKick::startDecelerate(", "bool RLVisionKick::stepDecelerate()"))
    rpc = "\n".join(block(client, signature) for signature in (
        "int RobotClient::call(", "double RobotClient::visualKickAckTimeoutMs()",
        "kick_strategy::StartState RobotClient::visualKickStartState()",
        "void RobotClient::visualKickResponse(", "int RobotClient::RLVisionKick(",
        "int RobotClient::robocupWalk()"))
    fixture = Path(__file__).with_name("soccer_flow_runtime_fixture.cpp").read_text(encoding="utf-8")
    fixture = fixture.replace("// CLIENT_DECLARATION", block(client_header, "class RobotClient\n").replace("private:", "public:") + ";")
    fixture = fixture.replace("// TREE_DECLARATIONS", declarations)
    fixture = fixture.replace("// PRODUCTION_FUNCTIONS", block(tree, "namespace\n{") + "\n" + methods + "\n" + rpc + "\n" + block(brain_source, "void Brain::updateKickoffMemory()"))
    fixture = fixture.replace("// MATCH_KICK_PARAMETERS", match_kick_parameters(
        (brain / "config/config.yaml").read_text(encoding="utf-8")))
    with tempfile.TemporaryDirectory(prefix="k1-soccer-flow-") as directory:
        source = Path(directory) / "soccer_flow.cpp"
        source.write_text(fixture, encoding="utf-8")
        executable = Path(directory) / "soccer_flow.exe"
        vendor = brain.parents[1] / "src/booster_ros2_interface/include/booster_interface"
        subprocess.run([args.compiler, "-std=c++17", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-unused-variable",
                        "-I", str(brain / "include"), "-I", str(vendor), str(source),
                        "-o", str(executable)], check=True)
        environment = os.environ.copy()
        compiler_directory = Path(args.compiler).parent
        if compiler_directory != Path("."):
            environment["PATH"] = str(compiler_directory.resolve()) + os.pathsep + environment.get("PATH", "")
        subprocess.run([str(executable)], check=True, env=environment)


if __name__ == "__main__":
    main()
