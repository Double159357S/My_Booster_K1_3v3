# 能看到球，但追球、对准或踢球失败：本地代码修复

日期：2026-10-13。仅修改本地工程，未连接、同步或启动任何机器人。

这次修复针对视觉检测之后的运动链。静态代码和离线测试能够确认以下缺陷，但没有故障现场日志，不能据此断言每台机器人的现场失败都由同一个原因造成。

| 代码位置 | 原行为及影响 | 修改后的行为 |
|---|---|---|
| `brain.cpp`：`getGameObjects`、`calibrateOdom` | 把拍摄时的相对坐标套到当前身体姿态上；再次定位时又使用旧相对坐标重算球位。走动、转身或定位校正会使球位偏移，连队友共享球也会被挪动。 | 查找拍摄时刻的里程计姿态，将本机观测固定在 odom 中，再投影到当前坐标；共享球保留场地坐标。 |
| `brain.cpp`：`updateBallPrediction` | 直接在会被定位校正改变的场地坐标中估计速度，定位跳变可能被误认为球在滚动。 | 在 odom 中估计，再转换位置和速度；定位校正时同步转换已有轨迹和拦截目标。倒地可能重置 odom，因此清除旧观测与预测。 |
| `brain_tree.cpp`：`Chase` | 前向速度按到球距离计算，而不是到绕球/接近目标点的距离；转向时缩小的前向速度又被最低步速放大。 | 按实际目标点减速；侧后方目标先转身；到达目标后停步，并保留转向时缩小的前向分量。 |
| `robot_client.cpp`：`crabWalk` | 将很小的横向速度独立放大到 0.3，会改变趟球方向。 | 保留计算出的速度向量，趟球节点负责加速。 |
| `subtree_striker_play.xml` | 普通 `Sequence` 在踢球节点返回 `RUNNING` 时，后续 tick 跳过前面的决策节点，球位和球权变化不能及时触发重新决策。 | 使用 `ReactiveSequence` 每帧决策；正在执行的视觉踢球仍由已有会话退出逻辑管理。 |
| `brain_tree.cpp`：`Kick` | 一些中止分支和 `onHalted` 不停步；加速按每帧加 0.08，100 Hz 下很快达到全速；软开球只限制首帧，定位球标志还可能在启动前被清除。 | 中止和抢占明确停步；按经过时间加速；软开球限速覆盖完整动作；固定启动时的截止时间，并检查本机球源和球权。 |
| `brain_tree.cpp`：视觉踢球切换 | 踢球退出后可以立即重复启动；丢球时普通找球节点可能同时发头部命令。 | 前锋退出后等待配置的 600 ms 冷却；会话标志由踢球节点管理；普通找球在前锋视觉踢球会话中让出头部控制。 |
| `vision`：`DataSyncer`、`VisionNode` | 缓冲区预填默认姿态；过期姿态只输出警告仍继续投影；逆序搜索遇到较差样本就提前退出，可能漏掉更接近的乱序样本。头部订阅的 reliable QoS 也可能与传感器 best-effort 发布端不匹配。 | 缓冲区从空开始；选择有效最近样本；图像与头部姿态差超过 120 ms 时丢弃该帧；头部订阅使用传感器 QoS，并移除重复的检测图像订阅。 |
| `brain.cpp`：`imageCallback`、检测与球记忆 | 图像尺寸更新依赖日志开关；缺失投影坐标会越界；检测断流时旧的 `ballDetected` 标志可能一直保留。 | 关掉日志也更新真实图像尺寸；拒绝无效投影、过期和乱序帧；在 tick 中使断流球失效。 |

涉及实现：[brain.cpp](../src/brain/src/brain.cpp)、[brain_tree.cpp](../src/brain/src/brain_tree.cpp)、[robot_client.cpp](../src/brain/src/robot_client.cpp)、[前锋行为树](../src/brain/behavior_trees/subtrees/subtree_striker_play.xml)、[视觉同步](../src/vision/src/base/data_syncer.cpp)、[视觉节点](../src/vision/src/vision_node.cpp)。

前锋决策层只给 `ReactiveSequence` 一个可能返回 `RUNNING` 的动作子序列，使每帧决策和动作保持能同时成立，并避免启用单异步子节点检查时发生异常。控制节点语义参考 [BehaviorTree.CPP 官方实现](https://github.com/BehaviorTree/BehaviorTree.CPP/blob/4.6.2/src/controls/reactive_sequence.cpp)。

## 本地验证

在工程根目录执行：

```powershell
python scripts/test_local.py
```

已通过：

- 5 个 C++ 测试程序：踢球几何、对准策略、三机协作、运动与观测坐标、裁判机 v20 协议。
- 5 项行为树检查：READY、让行、前锋和守门员动作路由，以及运行中的踢球不能跳过决策。
- 踢球运行测试：直接提取当前源码中的 `Kick` 和 `crabWalk` 函数，以本地模拟的 ROS/SDK 边界执行，验证趟球方向、加速、软开球、球权撤销、失球、球越过身体、避障中止、抢占停步和固定截止时间。
- 视觉同步测试：在本地 WSL 中编译实际 `data_syncer.cpp`，检查启动无姿态、单样本、乱序、过期/未来姿态和缓存长度。测试源码副本与工程文件的 SHA256 一致。

视觉同步测试也可在具备 ROS 2、OpenCV 和 yaml-cpp 的本地 Linux/WSL 中重跑。先加载本地 ROS 2 环境，再执行：

```bash
cmake -S src/vision/test/offline -B build/local_vision_sync
cmake --build build/local_vision_sync -j2
ctest --test-dir build/local_vision_sync --output-on-failure
```

这些测试验证了策略、动作函数和同步组件，不覆盖完整 `brain_node` 的 ROS/Booster SDK 编译、视觉模型质量、相机实际标定及机器人步态。整机编译和实机效果尚未验证。本次未修改三台机器人的身份、网络设置或相机外参，也未执行远程部署。
