"""Compile and execute the shipped Kick/crabWalk bodies with offline boundary fakes."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default="g++")
    args = parser.parse_args()
    brain = Path(__file__).resolve().parents[1]
    tree = (brain / "src/brain_tree.cpp").read_text(encoding="utf-8")
    kick = tree[tree.index("NodeStatus Kick::onStart()"):
                tree.index("rclcpp::Time RLVisionKick::_lastExitTime")]
    client = (brain / "src/robot_client.cpp").read_text(encoding="utf-8")
    crab = client[client.index("int RobotClient::crabWalk("):
                  client.index("bool RobotClient::isStandingStill(")]
    fixture = Path(__file__).with_name("kick_runtime_fixture.cpp").read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="k1-kick-runtime-") as directory:
        source = Path(directory) / "kick_runtime.cpp"
        source.write_text(fixture.replace("// PRODUCTION_FUNCTIONS", kick + crab), encoding="utf-8")
        executable = Path(directory) / "kick_runtime.exe"
        subprocess.run([args.compiler, "-std=c++17", "-Wall", "-Wextra", "-Wno-unused-variable",
                        "-I", str(brain / "include"), str(source), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
