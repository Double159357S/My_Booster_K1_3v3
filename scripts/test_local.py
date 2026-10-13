"""Offline strategy checks. Does not import ROS or connect to any robot."""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default="g++", help="C++17 compiler path")
    args = parser.parse_args()
    compiler = shutil.which(args.compiler)
    if compiler is None:
        parser.error(f"C++ compiler not found: {args.compiler}")
    root = Path(__file__).resolve().parents[1]
    tests = [("brain", name) for name in (
        "test_kick_geometry", "test_kick_strategy", "test_team_cooperation", "test_motion_commands")]
    tests.append(("game_controller", "protocol_v20_test"))
    with tempfile.TemporaryDirectory(prefix="k1-local-tests-") as directory:
        for package, name in tests:
            executable = Path(directory) / (name + (".exe" if sys.platform == "win32" else ""))
            subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-pedantic", "-Wno-unknown-pragmas",
                            "-I", str(root / "src" / package / "include"),
                            str(root / "src" / package / "test" / (name + ".cpp")),
                            "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)
            print(f"PASS {name}", flush=True)
    subprocess.run([sys.executable, str(root / "src/brain/test/test_kick_behavior_tree.py")], check=True)
    subprocess.run([sys.executable, str(root / "src/brain/test/test_kick_runtime.py"),
                    "--compiler", compiler], check=True)
    subprocess.run([sys.executable, str(root / "src/brain/test/test_soccer_flow_runtime.py"),
                    "--compiler", compiler], check=True)
    print("All offline strategy checks passed. No robot connections were made.")


if __name__ == "__main__":
    main()
