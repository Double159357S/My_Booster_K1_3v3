"""Execute production search/decision/SDK request functions with offline boundaries."""
import argparse
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default="g++")
    args = parser.parse_args()
    brain = Path(__file__).resolve().parents[1]
    tree = (brain / "src/brain_tree.cpp").read_text(encoding="utf-8")
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
    fixture = fixture.replace("// PRODUCTION_FUNCTIONS", block(tree, "namespace\n{") + "\n" + methods + "\n" + rpc)
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
