#pragma once

// 队友 UDP 消息格式：交换机器人状态、球信息和共享控球权。
// 收发端直接复制结构体的二进制字节，因此字段顺序、类型和对齐必须保持一致。
// 本文件负责定义格式和解码；发送、接收在 brain_communication.cpp，控球仲裁在 team_cooperation.h。

#include <cstddef>
#include <cstring>
#include <cmath>
#include "utils/geometry_types.h"
#include "utils/team_cooperation.h"

// 协议标识与包长度共同决定解析方式：
// 31202：120 字节原始包，或 128 字节早期过渡包。
// 31203：128 字节状态包，视觉踢球字段追加在旧字段之后。
// 31204：160 字节完整包，在状态包后增加控球权、会话号和控球资格。
#define VALIDATION_COMMUNICATION_LEGACY 31202
#define VALIDATION_COMMUNICATION 31203
#define VALIDATION_COMMUNICATION_OWNERSHIP 31204

// 机器人自身状态。当前发送端将它作为 TeamOwnershipPacket 的前 128 字节。
struct TeamCommunicationMsg
{
    int validation = VALIDATION_COMMUNICATION; // 状态包格式标识；完整包发送时设为 31204。
    int communicationId; // 每发送一次递增的消息序号，结合 session 检查重复和乱序。
    int teamId; // 队号，接收端只采用本队消息。
    int playerId; // 球员号，用于确定消息来自哪台机器人。
    int playerRole; // 角色：1 前锋，2 守门员，3 未知。
    bool isAlive; // 是否具备有效上场状态，具体资格由策略层判断。
    bool isLead; // 发送方的控球状态，新协作逻辑依据有效的 ownership 授权设置。
    bool ballDetected; // 本机是否有新鲜的直接视觉检测；共享球不能重新标成检测到。
    bool ballLocationKnown; // 有新鲜本机球或共享球，供仲裁判断能否接近球。
    double ballConfidence; // 球信息的置信度。
    double ballRange; // 机器人到球的距离，单位为米。
    double cost; // 到达并处理球的估计成本，用于比较主攻候选，通常越小越有利。
    Point ballPosToField; // 球在场地坐标系中的位置。
    Pose2D robotPoseToField; // 机器人在场地坐标系中的位置和朝向。
    double kickDir; // 期望踢球方向，场地坐标系角度，单位为弧度。
    double thetaRb; // 机器人指向球的场地坐标系角度，单位为弧度。
    int cmdId; // 指令序号，用于识别新的 cmd 请求。
    // 旧式角色请求：100 表示要球；十位为 1 表示交接守门员，个位指定接替球员。
    // 例如指令值 11 表示请求 1 号接替守门员；新的共享控球权由 ownership 决定。
    int cmd;
    // 新字段只能追加在旧包末尾，不能插到中间破坏 120 字节旧协议的字段偏移。
    bool isInVisualKick; // 是否正在执行视觉踢球
};

// offsetof 获取字段的字节偏移；前 120 字节必须保持旧协议布局。
// static_assert 在编译时检查布局，发现不兼容的改动就报错。
constexpr std::size_t TEAM_COMMUNICATION_LEGACY_SIZE =
    offsetof(TeamCommunicationMsg, isInVisualKick);
static_assert(TEAM_COMMUNICATION_LEGACY_SIZE == 120,
              "legacy team packet prefix must remain byte-compatible");

// 原始 120 字节包，仅用于解码旧消息。共有字段的含义与上面的状态包相同。
// 使用独立结构体读取旧布局，再逐字段转换，保留当前输出中新增字段的初始化值。
struct TeamCommunicationMsgLegacy
{
    int validation = VALIDATION_COMMUNICATION_LEGACY;
    int communicationId;
    int teamId;
    int playerId;
    int playerRole;
    bool isAlive;
    bool isLead;
    bool ballDetected;
    bool ballLocationKnown;
    double ballConfidence;
    double ballRange;
    double cost;
    Point ballPosToField;
    Pose2D robotPoseToField;
    double kickDir;
    double thetaRb;
    int cmdId;
    int cmd;
};
static_assert(sizeof(TeamCommunicationMsgLegacy) == TEAM_COMMUNICATION_LEGACY_SIZE,
              "legacy team packet layout changed");

// 兼容本次迁移早期生成过的 128 字节过渡包：isInVisualKick 曾被插在 bool 字段中间。
// 仅用于解码旧状态信息；当前发送端统一发送 TeamOwnershipPacket。
struct TeamCommunicationMsgInsertedV1
{
    int validation = VALIDATION_COMMUNICATION_LEGACY;
    int communicationId;
    int teamId;
    int playerId;
    int playerRole;
    bool isAlive;
    bool isLead;
    bool isInVisualKick;
    bool ballDetected;
    bool ballLocationKnown;
    double ballConfidence;
    double ballRange;
    double cost;
    Point ballPosToField;
    Pose2D robotPoseToField;
    double kickDir;
    double thetaRb;
    int cmdId;
    int cmd;
};
static_assert(sizeof(TeamCommunicationMsgInsertedV1) == 128,
              "transitional team packet layout changed");

// 当前完整包：128 字节状态 + 16 字节控球声明 + 8 字节会话号 + 两个 4 字节字段。
// 保留完整状态包前缀及其尾部对齐填充。旧发送端不能确认新授权，场上机器人需统一升级。
struct TeamOwnershipPacket {
    TeamCommunicationMsg state{}; // 自身状态；发送端把 state.validation 设为 31204。
    // 控球声明：coordinator 为协调球员，owner 为控球球员；
    // revision 为决策版本，leaseMs 为授权剩余有效时间（毫秒）。
    team_cooperation::Announcement ownership;
    std::uint64_t session = 0; // 本次通信实例的固定编号，重启后改变，用于区分新旧实例消息。
    std::uint32_t canControl = 0; // 本帧控球候选资格，含角色、有效球源和共享球重识别状态。
    std::uint32_t reserved = 0; // 预留扩展字段，当前必须为 0。
};
static_assert(sizeof(TeamCommunicationMsg) == 128, "v2 team packet layout changed");
static_assert(offsetof(TeamOwnershipPacket, ownership) == 128, "v2 prefix changed");
static_assert(sizeof(TeamOwnershipPacket) == 160, "ownership packet layout changed");

// 将两种旧结构体的共有字段转换为统一状态，供策略层按相同方式读取。
// 旧包缺少的控球声明、session 和 canControl 在解码输出中保持零值。
template<class OldPacket>
inline void copyLegacyTeamState(const OldPacket &old, TeamCommunicationMsg &msg) {
    msg.validation = VALIDATION_COMMUNICATION;
    msg.communicationId = old.communicationId;
    msg.teamId = old.teamId;
    msg.playerId = old.playerId;
    msg.playerRole = old.playerRole;
    msg.isAlive = old.isAlive;
    msg.isLead = old.isLead;
    msg.ballDetected = old.ballDetected;
    msg.ballLocationKnown = old.ballLocationKnown;
    msg.ballConfidence = old.ballConfidence;
    msg.ballRange = old.ballRange;
    msg.cost = old.cost;
    msg.ballPosToField = old.ballPosToField;
    msg.robotPoseToField = old.robotPoseToField;
    msg.kickDir = old.kickDir;
    msg.thetaRb = old.thetaRb;
    msg.cmdId = old.cmdId;
    msg.cmd = old.cmd;
}

// 将收到的 UDP 字节解析到 out。格式或数值检查失败返回 false，接收端会丢弃该包。
// 这里只检查协议和数据；队号匹配、球员号上限以及会话/序号检查由接收端继续完成。
inline bool decodeTeamPacket(const void *data, std::size_t size, TeamOwnershipPacket &out) {
    // 每次清空输出，避免旧包未携带的字段残留上一次的授权。
    out = TeamOwnershipPacket{};
    if (!data || size < sizeof(int)) return false;
    // 先读取包头标识，再结合实际长度选择对应布局。
    int validation = 0;
    std::memcpy(&validation, data, sizeof(validation));
    const auto *bytes = static_cast<const unsigned char *>(data);
    // 前五个 int 占 20 字节，随后是 bool；过渡包这里多一个 isInVisualKick。
    // 先检查 bool 的原始字节只能是 0 或 1，再复制进 C++ 结构体。
    const std::size_t boolEnd = validation == VALIDATION_COMMUNICATION_LEGACY &&
        size == sizeof(TeamCommunicationMsgInsertedV1) ? 25 : 24;
    if (size < boolEnd) return false;
    for (std::size_t i = 20; i < boolEnd; ++i) if (bytes[i] > 1) return false;
    if (validation == VALIDATION_COMMUNICATION_OWNERSHIP && size == sizeof(out)) {
        // 31204 / 160 字节：解析完整包，并检查会话号、资格值和授权范围。
        // 正式布局的 isInVisualKick 位于第 120 字节偏移处。
        if (bytes[120] > 1) return false;
        std::memcpy(&out, data, sizeof(out));
        if (out.session == 0 || out.canControl > 1 || out.reserved != 0 ||
            out.ownership.coordinator < 0 || out.ownership.owner < 0 ||
            out.ownership.leaseMs > 10000) return false;
    } else if (validation == VALIDATION_COMMUNICATION && size == sizeof(out.state)) {
        // 31203 / 128 字节：只提供状态，没有新控球授权。
        if (bytes[120] > 1) return false;
        std::memcpy(&out.state, data, sizeof(out.state));
    } else if (validation == VALIDATION_COMMUNICATION_LEGACY &&
               size == sizeof(TeamCommunicationMsgLegacy)) {
        // 31202 / 120 字节：读取最早的状态格式，再转换共有字段。
        TeamCommunicationMsgLegacy old{};
        std::memcpy(&old, data, sizeof(old));
        copyLegacyTeamState(old, out.state);
    } else if (validation == VALIDATION_COMMUNICATION_LEGACY &&
               size == sizeof(TeamCommunicationMsgInsertedV1)) {
        // 31202 / 128 字节：读取过渡布局，额外恢复其中的视觉踢球状态。
        TeamCommunicationMsgInsertedV1 old{};
        std::memcpy(&old, data, sizeof(old));
        copyLegacyTeamState(old, out.state);
        out.state.isInVisualKick = old.isInVisualKick;
    } else return false;
    // 最后检查球员号、角色，以及策略运算使用的成本、距离和坐标是否为有效数值。
    const auto &m = out.state;
    return m.playerId > 0 && m.playerRole >= 1 && m.playerRole <= 3 &&
        std::isfinite(m.cost) && m.cost >= 0.0 && std::isfinite(m.ballRange) &&
        std::isfinite(m.ballPosToField.x) && std::isfinite(m.ballPosToField.y) &&
        std::isfinite(m.robotPoseToField.x) && std::isfinite(m.robotPoseToField.y) &&
        std::isfinite(m.robotPoseToField.theta);
}
