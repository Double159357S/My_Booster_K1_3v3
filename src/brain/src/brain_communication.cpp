#include "brain.h"
#include "brain_communication.h"

#include <array>
#include <cerrno>
#include <cstring>

BrainCommunication::BrainCommunication(Brain *argBrain) : brain(argBrain) {}

BrainCommunication::~BrainCommunication()
{
    clearupGameControllerUnicast();
    clearupTeamCommunication();
}

void BrainCommunication::initCommunication()
{
    initGameControllerUnicast();
    if (brain->config->enableCom)
    {
        cout << RED_CODE << "Communication enabled." << RESET_CODE << endl;
        _team_udp_port = 10000 + brain->config->teamId;
        initTeamCommunication();
    }
    else
        cout << RED_CODE << "Communication disabled." << RESET_CODE << endl;
}

void BrainCommunication::initGameControllerUnicast()
{
    try
    {
        _gc_send_socket = socket(AF_INET, SOCK_DGRAM, 0);
        if (_gc_send_socket < 0)
            throw std::runtime_error(format("gc socket failed: %s", strerror(errno)));

        string gamecontrol_ip = brain->get_parameter("game_control_ip").as_string();
        cout << GREEN_CODE << format("GameControl IP: %s", gamecontrol_ip.c_str())
             << RESET_CODE << endl;
        _gcsaddr.sin_family = AF_INET;
        _gcsaddr.sin_addr.s_addr = inet_addr(gamecontrol_ip.c_str());
        _gcsaddr.sin_port = htons(GAMECONTROLLER_RETURN_PORT);

        _unicast_gamecontrol_flag.store(true);
        _gamecontrol_unicast_thread = std::thread([this](){ unicastToGameController(); });
    }
    catch(const std::exception& e)
    {
        std::cerr << e.what() << '\n';
    }
}

void BrainCommunication::clearupGameControllerUnicast()
{
    _unicast_gamecontrol_flag.store(false);
    if (_gamecontrol_unicast_thread.joinable())
        _gamecontrol_unicast_thread.join();
    if (_gc_send_socket >= 0)
    {
        close(_gc_send_socket);
        _gc_send_socket = -1;
    }
}

void BrainCommunication::unicastToGameController()
{
    while (_unicast_gamecontrol_flag.load())
    {
        // Protocol is unknown until the GameController receiver accepts a packet.
        // This prevents a startup v12/v2 response from being emitted to a v20 referee.
        const uint16_t protocolVersion = _game_controller_protocol_version.load();
        if (protocolVersion == 0)
        {
            this_thread::sleep_for(chrono::milliseconds(GAME_CONTROLLER_PROTOCOL_WAIT_INTERVAL_MS));
            continue;
        }

        {
            std::lock_guard<std::recursive_mutex> stateLock(brain->brainStateMutex_);
            if (protocolVersion >= 20)
            {
                const bool ballLocationKnown = brain->tree->getEntry<bool>("ball_location_known");
                gc_return_data_v20.teamNum = brain->config->teamId;
                gc_return_data_v20.playerNum = brain->config->playerId;
                gc_return_data_v20.fallen =
                    brain->data->recoveryState == RobotRecoveryState::HAS_FALLEN ? 1 : 0;
                gc_return_data_v20.pose[0] = static_cast<float>(brain->data->robotPoseToField.x * 1000.0);
                gc_return_data_v20.pose[1] = static_cast<float>(brain->data->robotPoseToField.y * 1000.0);
                gc_return_data_v20.pose[2] = static_cast<float>(brain->data->robotPoseToField.theta);
                gc_return_data_v20.ballAge = ballLocationKnown
                    ? static_cast<float>(brain->msecsSince(brain->data->ball.timePoint) / 1000.0)
                    : -1.0F;
                gc_return_data_v20.ball[0] = ballLocationKnown
                    ? static_cast<float>(brain->data->ball.posToRobot.x * 1000.0)
                    : 0.0F;
                gc_return_data_v20.ball[1] = ballLocationKnown
                    ? static_cast<float>(brain->data->ball.posToRobot.y * 1000.0)
                    : 0.0F;
                const int ret = sendto(_gc_send_socket, &gc_return_data_v20,
                                       sizeof(gc_return_data_v20), 0,
                                       reinterpret_cast<sockaddr *>(&_gcsaddr), sizeof(_gcsaddr));
                if (ret < 0)
                    cout << RED_CODE << format("gc sendto failed: %s", strerror(errno)) << RESET_CODE << endl;
            }
            else
            {
                gc_return_data.team = brain->config->teamId;
                gc_return_data.player = brain->config->playerId;
                gc_return_data.message = GAMECONTROLLER_RETURN_MSG_ALIVE;
                const int ret = sendto(_gc_send_socket, &gc_return_data, sizeof(gc_return_data), 0,
                                       reinterpret_cast<sockaddr *>(&_gcsaddr), sizeof(_gcsaddr));
                if (ret < 0)
                    cout << RED_CODE << format("gc sendto failed: %s", strerror(errno)) << RESET_CODE << endl;
            }
        }
        this_thread::sleep_for(chrono::milliseconds(BROADCAST_GAME_CONTROL_INTERVAL_MS));
    }
}

void BrainCommunication::initTeamCommunication()
{
    try
    {
        _team_socket = socket(AF_INET, SOCK_DGRAM, 0);
        if (_team_socket < 0)
            throw std::runtime_error(format("socket failed: %s", strerror(errno)));

        int reuse = 1;
        if (setsockopt(_team_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0)
            throw std::runtime_error(format("Failed to set SO_REUSEADDR: %s", strerror(errno)));
        int broadcast = 1;
        if (setsockopt(_team_socket, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) < 0)
            throw std::runtime_error(format("Failed to set SO_BROADCAST: %s", strerror(errno)));
        timeval receiveTimeout{};
        receiveTimeout.tv_usec = 200000;
        if (setsockopt(_team_socket, SOL_SOCKET, SO_RCVTIMEO,
                       &receiveTimeout, sizeof(receiveTimeout)) < 0)
            throw std::runtime_error(format("Failed to set SO_RCVTIMEO: %s", strerror(errno)));

        sockaddr_in receiveAddr{};
        receiveAddr.sin_family = AF_INET;
        receiveAddr.sin_addr.s_addr = htonl(INADDR_ANY);
        receiveAddr.sin_port = htons(_team_udp_port);
        if (bind(_team_socket, reinterpret_cast<sockaddr *>(&receiveAddr), sizeof(receiveAddr)) < 0)
            throw std::runtime_error(format("bind failed: %s (port=%d)", strerror(errno), _team_udp_port));

        _team_broadcast_addr.sin_family = AF_INET;
        _team_broadcast_addr.sin_addr.s_addr = INADDR_BROADCAST;
        _team_broadcast_addr.sin_port = htons(_team_udp_port);
        cout << GREEN_CODE << format("Team communication broadcast/listen on UDP port %d", _team_udp_port)
             << RESET_CODE << endl;

        _team_communication_flag.store(true);
        _team_broadcast_thread = std::thread([this](){ broadcastTeamCommunication(); });
        _team_receive_thread = std::thread([this](){ spinTeamCommunicationReceiver(); });
    }
    catch(const std::exception& e)
    {
        _team_communication_flag.store(false);
        if (_team_broadcast_thread.joinable())
            _team_broadcast_thread.join();
        if (_team_receive_thread.joinable())
            _team_receive_thread.join();
        std::cerr << e.what() << '\n';
        brain->log->log("error/communication",
                        rerun::TextLog(format("Failed to initialize team communication: %s", e.what())));
        if (_team_socket >= 0)
        {
            close(_team_socket);
            _team_socket = -1;
        }
    }
}

void BrainCommunication::broadcastTeamCommunication()
{
    auto log = [=](string msg) {
        brain->log->setTimeNow();
        brain->log->log("debug/sendMsg", rerun::TextLog(msg));
    };
    while (_team_communication_flag.load())
    {
        TeamOwnershipPacket wire{};
        auto &msg = wire.state;
        {
            std::lock_guard<std::recursive_mutex> stateLock(brain->brainStateMutex_);
            wire.ownership = brain->data->ballArbitration.announcement(team_cooperation::steadyNowMs());
            wire.session = _team_session;
            wire.canControl = brain->data->tmImAlive &&
                brain->tree->getEntry<int>("control_state") == 3 ? 1 : 0;
            const bool ballLocationKnown = brain->tree->getEntry<bool>("ball_location_known");
            msg.validation = VALIDATION_COMMUNICATION_OWNERSHIP;
            msg.communicationId = _team_communication_msg_id++;
            msg.teamId = brain->config->teamId;
            msg.playerId = brain->config->playerId;
            msg.playerRole = brain->tree->getEntry<string>("player_role") == "striker" ? 1 : 2;
            msg.isAlive = brain->data->tmImAlive;
            msg.isLead = brain->data->tmImLead;
            msg.isInVisualKick = brain->data->tmImInVisualKick;
            msg.ballDetected = ballLocationKnown && brain->data->ballDetected;
            msg.ballLocationKnown = ballLocationKnown;
            msg.ballConfidence = ballLocationKnown ? brain->data->ball.confidence : 0.0;
            msg.ballRange = ballLocationKnown ? brain->data->ball.range : 0.0;
            msg.cost = brain->data->tmMyCost;
            msg.ballPosToField = ballLocationKnown ? brain->data->ball.posToField : Point{};
            msg.robotPoseToField = brain->data->robotPoseToField;
            msg.kickDir = brain->data->kickDir;
            msg.thetaRb = ballLocationKnown ? brain->data->robotBallAngleToField : 0.0;
            msg.cmdId = brain->data->tmMyCmdId;
            msg.cmd = brain->data->tmMyCmd;
        }
        log(format("ImAlive: %d, ImLead: %d, myCost: %.1f, myCmdId: %d, myCmd: %d",
                    msg.isAlive, msg.isLead, msg.cost, msg.cmdId, msg.cmd));

        const int ret = sendto(_team_socket, &wire, sizeof(wire), 0,
                               reinterpret_cast<sockaddr *>(&_team_broadcast_addr),
                               sizeof(_team_broadcast_addr));
        if (ret < 0)
            cout << RED_CODE << format("team broadcast failed: %s", strerror(errno)) << RESET_CODE << endl;
        else
        {
            std::lock_guard<std::recursive_mutex> stateLock(brain->brainStateMutex_);
            brain->data->sendId = msg.communicationId;
            brain->data->sendTime = brain->get_clock()->now();
        }
        this_thread::sleep_for(chrono::milliseconds(TEAM_COMMUNICATION_INTERVAL_MS));
    }
}

void BrainCommunication::spinTeamCommunicationReceiver()
{
    auto log = [=](string msg) {
        brain->log->setTimeNow();
        brain->log->log("debug/receiveMsg", rerun::TextLog(msg));
    };
    sockaddr_in addr{};
    socklen_t addrLen = sizeof(addr);
    TeamCommunicationMsg msg{};
    std::array<unsigned char, 256> packet{};

    while (_team_communication_flag.load())
    {
        msg = TeamCommunicationMsg{};
        packet.fill(0);
        addrLen = sizeof(addr);
        const ssize_t len = recvfrom(_team_socket, packet.data(), packet.size(), 0,
                                     reinterpret_cast<sockaddr *>(&addr), &addrLen);
        if (len < 0)
        {
            if (!_team_communication_flag.load())
                break;
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;
            cout << RED_CODE << format("receiving UDP message failed: %s", strerror(errno))
                 << RESET_CODE << endl;
            continue;
        }
        TeamOwnershipPacket wire{};
        if (!decodeTeamPacket(packet.data(), static_cast<std::size_t>(len), wire)) continue;
        msg = wire.state;
        std::lock_guard<std::recursive_mutex> stateLock(brain->brainStateMutex_);

        if (msg.teamId != brain->config->teamId)
            continue;
        brain->data->tmIP = inet_ntoa(addr.sin_addr);
        if (msg.playerId == brain->config->playerId)
        {
            brain->data->sendId = msg.communicationId;
            brain->data->sendTime = brain->get_clock()->now();
            continue;
        }

        const int tmIdx = msg.playerId - 1;
        if (tmIdx < 0 || tmIdx >= HL_MAX_NUM_PLAYERS ||
            msg.playerId > brain->config->numOfPlayers ||
            wire.ownership.coordinator > brain->config->numOfPlayers ||
            wire.ownership.owner > brain->config->numOfPlayers ||
            brain->data->penalty[tmIdx] == SUBSTITUTE)
            continue;
        log(format("TMID: %d, alive: %d, lead: %d, cost: %.1f, CmdId: %d, Cmd: %d",
                    msg.playerId, msg.isAlive, msg.isLead, msg.cost, msg.cmdId, msg.cmd));
        TMStatus &tmStatus = brain->data->tmStatus[tmIdx];
        // Do not let delayed legacy packets erase an adopted ownership grant.
        if (wire.session == 0 && tmStatus.teamSession != 0) continue;
        if (wire.session != 0) {
            if (wire.session == tmStatus.retiredTeamSession) continue;
            if (wire.session == tmStatus.teamSession && msg.communicationId <= tmStatus.teamSequence) continue;
            if (wire.session != tmStatus.teamSession) {
                tmStatus.retiredTeamSession = tmStatus.teamSession;
                tmStatus.teamSession = wire.session;
            }
            tmStatus.teamSequence = msg.communicationId;
        }
        tmStatus.ownership = wire.ownership;
        tmStatus.canControl = wire.canControl == 1;
        tmStatus.teamReceivedMs = team_cooperation::steadyNowMs();
        tmStatus.role = msg.playerRole == 1 ? "striker" :
            (msg.playerRole == 2 ? "goal_keeper" : "unknown");
        tmStatus.isAlive = msg.isAlive;
        tmStatus.ballDetected = msg.ballDetected;
        tmStatus.ballLocationKnown = msg.ballLocationKnown;
        tmStatus.ballConfidence = msg.ballConfidence;
        tmStatus.ballRange = msg.ballRange;
        tmStatus.cost = msg.cost;
        tmStatus.isLead = msg.isLead;
        tmStatus.isInVisualKick = msg.isInVisualKick;
        tmStatus.ballPosToField = msg.ballPosToField;
        tmStatus.robotPoseToField = msg.robotPoseToField;
        tmStatus.kickDir = msg.kickDir;
        tmStatus.thetaRb = msg.thetaRb;
        tmStatus.timeLastCom = brain->get_clock()->now();
        tmStatus.cmd = msg.cmd;
        tmStatus.cmdId = msg.cmdId;
        if (msg.cmdId > brain->data->tmCmdId)
        {
            brain->data->tmCmdId = msg.cmdId;
            brain->data->tmReceivedCmd = msg.cmd;
            brain->data->tmLastCmdChangeTime = brain->get_clock()->now();
            log(format("Received new command from teammate %d: %d", msg.playerId, msg.cmd));
        }
    }
}

void BrainCommunication::clearupTeamCommunication()
{
    _team_communication_flag.store(false);
    if (_team_broadcast_thread.joinable())
        _team_broadcast_thread.join();
    if (_team_receive_thread.joinable())
        _team_receive_thread.join();
    if (_team_socket >= 0)
    {
        close(_team_socket);
        _team_socket = -1;
    }
}
