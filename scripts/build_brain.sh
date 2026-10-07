#!/bin/bash
# 编译 brain 包。
# 背景: 本机(aarch64)只有 4 核且内存紧张(桌面 + 机器人进程已占大半),
#       并行度拉满会 OOM/疯狂 swap, 导致系统卡死、SSH 断开。
#       因此这里默认限制并发、降低优先级, 并放到 tmux 里跑。
#
# 可用环境变量覆盖:
#   BRAIN_BUILD_JOBS=1        # 并发度(默认 2), 还卡就降到 1
#   BRAIN_BUILD_NO_TMUX=1     # 不使用 tmux, 直接在当前终端编译
#   BRAIN_BUILD_SESSION=xxx   # tmux 会话名(默认 brain_build)
set -e

cd "$(dirname "$0")"
cd ..

JOBS="${BRAIN_BUILD_JOBS:-2}"
SESSION="${BRAIN_BUILD_SESSION:-brain_build}"

# 默认在 tmux 里编译: SSH 断开不会杀掉编译, 重连后 `tmux attach -t brain_build` 可继续查看。
if [ -z "$TMUX" ] && [ -z "$BRAIN_BUILD_NO_TMUX" ] && command -v tmux >/dev/null 2>&1; then
    if tmux has-session -t "$SESSION" 2>/dev/null; then
        echo "[build_brain] tmux 会话 '$SESSION' 已存在, 直接接入。"
        exec tmux attach -t "$SESSION"
    fi
    printf -v _cmd '%q ' "$0" "$@"
    echo "[build_brain] 在 tmux 会话 '$SESSION' 中编译(并发 ${JOBS}); SSH 断开不影响。"
    echo "[build_brain] 重连后查看: tmux attach -t $SESSION"
    exec tmux new-session -s "$SESSION" \
        "bash -c $(printf '%q' "${_cmd}; echo; echo ===brain_build_finished===; exec bash")"
fi

# 限制 make 并发, 避免同一次编译里再被 make 拉满
export MAKEFLAGS="-j${JOBS}"

# 降低 CPU/IO 优先级, 给 sshd 留出余量, 防止 SSH 卡断
NICE_IO=()
if command -v ionice >/dev/null 2>&1; then
    NICE_IO=(ionice -c3)
fi

nice -n 10 "${NICE_IO[@]}" \
    colcon build --symlink-install --parallel-workers "${JOBS}" --packages-select brain "$@"

espeak "build complete" >/dev/null 2>&1 || echo "Build complete"
