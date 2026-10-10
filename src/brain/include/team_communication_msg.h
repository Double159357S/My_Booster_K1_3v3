#pragma once

#include <cstddef>
#include <cstring>
#include <cmath>
#include "types.h"
#include "utils/team_cooperation.h"

#define VALIDATION_COMMUNICATION_LEGACY 31202
#define VALIDATION_COMMUNICATION 31203
#define VALIDATION_COMMUNICATION_OWNERSHIP 31204
struct TeamCommunicationMsg
{
    int validation = VALIDATION_COMMUNICATION; // validate msg, to determine if it's sent by us.
    int communicationId;
    int teamId;
    int playerId;
    int playerRole; // 1: striker, 2: goal_keeper, 3: unknown
    bool isAlive; // 是否在场上, 且没有在罚时中
    bool isLead; // 是否在控球状态
    bool ballDetected;
    bool ballLocationKnown;
    double ballConfidence;
    double ballRange;
    double cost; // 计算从当前状态到能踢到球的成本
    Point ballPosToField;
    Pose2D robotPoseToField;
    double kickDir;
    double thetaRb;
    int cmdId; // 每个 player 发布时, 需要将 cmdId + 1. 用来代表发布的顺序. 
    int cmd; // 百位为 1 时, 代表自己要球控球. 十位为 1 时, 代表守门员要求另一个球员接替守门员角色, 此时个位数字代表接替球员的 playerId. 例如: 100, 代表自己要球控球, 另一个 striker 进入辅助角色; 011, 代表守门员要出击, 要求 1 号球员接替守门.  
    // 新字段只能追加在旧包末尾，不能插到中间破坏 120 字节旧协议的字段偏移。
    bool isInVisualKick; // 是否正在执行视觉踢球
};

constexpr std::size_t TEAM_COMMUNICATION_LEGACY_SIZE =
    offsetof(TeamCommunicationMsg, isInVisualKick);
static_assert(TEAM_COMMUNICATION_LEGACY_SIZE == 120,
              "legacy team packet prefix must remain byte-compatible");

// Original 120-byte packet. Keep an explicit decoder type instead of copying
// only a prefix into TeamCommunicationMsg.
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
// 仅用于网络解码/滚动升级，新的发送端使用上面的可追加布局。
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

// Preserve the complete 128-byte v2 prefix, including its tail padding.
// All on-field robots must upgrade together: old senders cannot confirm grants.
struct TeamOwnershipPacket {
    TeamCommunicationMsg state{};
    team_cooperation::Announcement ownership;
    std::uint64_t session = 0;
    std::uint32_t canControl = 0; // Calibrated and in autonomous football mode.
    std::uint32_t reserved = 0;
};
static_assert(sizeof(TeamCommunicationMsg) == 128, "v2 team packet layout changed");
static_assert(offsetof(TeamOwnershipPacket, ownership) == 128, "v2 prefix changed");
static_assert(sizeof(TeamOwnershipPacket) == 160, "ownership packet layout changed");

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

inline bool decodeTeamPacket(const void *data, std::size_t size, TeamOwnershipPacket &out) {
    out = TeamOwnershipPacket{};
    if (!data || size < sizeof(int)) return false;
    int validation = 0;
    std::memcpy(&validation, data, sizeof(validation));
    const auto *bytes = static_cast<const unsigned char *>(data);
    // Check native bool representations before copying any untrusted bool.
    const std::size_t boolEnd = validation == VALIDATION_COMMUNICATION_LEGACY &&
        size == sizeof(TeamCommunicationMsgInsertedV1) ? 25 : 24;
    if (size < boolEnd) return false;
    for (std::size_t i = 20; i < boolEnd; ++i) if (bytes[i] > 1) return false;
    if (validation == VALIDATION_COMMUNICATION_OWNERSHIP && size == sizeof(out)) {
        if (bytes[120] > 1) return false;
        std::memcpy(&out, data, sizeof(out));
        if (out.session == 0 || out.canControl > 1 || out.reserved != 0 ||
            out.ownership.coordinator < 0 || out.ownership.owner < 0 ||
            out.ownership.leaseMs > 10000) return false;
    } else if (validation == VALIDATION_COMMUNICATION && size == sizeof(out.state)) {
        if (bytes[120] > 1) return false;
        std::memcpy(&out.state, data, sizeof(out.state));
    } else if (validation == VALIDATION_COMMUNICATION_LEGACY &&
               size == sizeof(TeamCommunicationMsgLegacy)) {
        TeamCommunicationMsgLegacy old{};
        std::memcpy(&old, data, sizeof(old));
        copyLegacyTeamState(old, out.state);
    } else if (validation == VALIDATION_COMMUNICATION_LEGACY &&
               size == sizeof(TeamCommunicationMsgInsertedV1)) {
        TeamCommunicationMsgInsertedV1 old{};
        std::memcpy(&old, data, sizeof(old));
        copyLegacyTeamState(old, out.state);
        out.state.isInVisualKick = old.isInVisualKick;
    } else return false;
    const auto &m = out.state;
    return m.playerId > 0 && m.playerRole >= 1 && m.playerRole <= 3 &&
        std::isfinite(m.cost) && m.cost >= 0.0 && std::isfinite(m.ballRange) &&
        std::isfinite(m.ballPosToField.x) && std::isfinite(m.ballPosToField.y) &&
        std::isfinite(m.robotPoseToField.x) && std::isfinite(m.robotPoseToField.y) &&
        std::isfinite(m.robotPoseToField.theta);
}
