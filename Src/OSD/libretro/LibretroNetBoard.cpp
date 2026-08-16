#include "LibretroNetBoard.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iterator>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

#include "Supermodel.h"

namespace
{
constexpr uint8_t kProtocolVersion = 2;
constexpr uint8_t kPacketHello = 1;
constexpr uint8_t kPacketFrame = 2;
constexpr size_t kPacketHeaderSize = 18;
constexpr auto kFrameWaitTimeout = std::chrono::milliseconds(250);

struct QueuedPacket
{
  std::vector<uint8_t> bytes;
  uint16_t sender = 0;
};

struct NetpacketSession
{
  std::mutex mutex;
  bool interfaceSupported = false;
  bool active = false;
  uint16_t localId = 0;
  retro_netpacket_send_t send = nullptr;
  retro_netpacket_poll_receive_t pollReceive = nullptr;
  retro_log_printf_t log = nullptr;
  unsigned expectedCabinets = 2;
  std::set<uint16_t> clients;
  std::vector<QueuedPacket> packets;
};

NetpacketSession g_session;

void SessionLog(enum retro_log_level level, const char *message)
{
  retro_log_printf_t logger = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_session.mutex);
    logger = g_session.log;
  }
  if (logger)
    logger(level, "[Supermodel] [NetBoard] %s\n", message);
}

void RETRO_CALLCONV NetpacketStart(uint16_t clientId,
                                   retro_netpacket_send_t send,
                                   retro_netpacket_poll_receive_t pollReceive)
{
  {
    std::lock_guard<std::mutex> lock(g_session.mutex);
    g_session.active = true;
    g_session.localId = clientId;
    g_session.send = send;
    g_session.pollReceive = pollReceive;
    g_session.clients.clear();
    g_session.packets.clear();
    if (clientId != 0)
      g_session.clients.insert(0);
  }
  SessionLog(RETRO_LOG_INFO,
             clientId == 0 ? "Netpacket host session started"
                           : "Netpacket client session started");
}

void RETRO_CALLCONV NetpacketReceive(const void *data, size_t size,
                                     uint16_t clientId)
{
  if (!data || size == 0 || size > 65536)
    return;

  QueuedPacket packet;
  packet.sender = clientId;
  packet.bytes.resize(size);
  std::memcpy(packet.bytes.data(), data, size);

  std::lock_guard<std::mutex> lock(g_session.mutex);
  if (g_session.active)
    g_session.packets.emplace_back(std::move(packet));
}

void RETRO_CALLCONV NetpacketStop()
{
  {
    std::lock_guard<std::mutex> lock(g_session.mutex);
    g_session.active = false;
    g_session.send = nullptr;
    g_session.pollReceive = nullptr;
    g_session.clients.clear();
    g_session.packets.clear();
  }
  SessionLog(RETRO_LOG_INFO, "Netpacket session stopped");
}

bool RETRO_CALLCONV NetpacketConnected(uint16_t clientId)
{
  bool accepted = false;
  unsigned connectedCabinets = 1;
  unsigned expectedCabinets = 2;
  {
    std::lock_guard<std::mutex> lock(g_session.mutex);
    expectedCabinets = g_session.expectedCabinets;
    accepted = g_session.active && g_session.localId == 0 &&
               (g_session.clients.count(clientId) != 0 ||
                g_session.clients.size() + 1 < expectedCabinets);
    if (accepted)
      g_session.clients.insert(clientId);
    connectedCabinets = static_cast<unsigned>(g_session.clients.size() + 1);
  }
  const std::string message = accepted
    ? "Slave client " + std::to_string(clientId) + " connected (" +
      std::to_string(connectedCabinets) + "/" +
      std::to_string(expectedCabinets) + " cabinets)"
    : "Additional client rejected (configured for " +
      std::to_string(expectedCabinets) + " cabinets)";
  SessionLog(accepted ? RETRO_LOG_INFO : RETRO_LOG_WARN,
             message.c_str());
  return accepted;
}

void RETRO_CALLCONV NetpacketDisconnected(uint16_t clientId)
{
  {
    std::lock_guard<std::mutex> lock(g_session.mutex);
    g_session.clients.erase(clientId);
  }
  SessionLog(RETRO_LOG_WARN, "Slave client disconnected");
}

bool SessionInterfaceSupported()
{
  std::lock_guard<std::mutex> lock(g_session.mutex);
  return g_session.interfaceSupported;
}

bool SessionActive()
{
  std::lock_guard<std::mutex> lock(g_session.mutex);
  return g_session.active;
}

uint16_t SessionLocalId()
{
  std::lock_guard<std::mutex> lock(g_session.mutex);
  return g_session.localId;
}

void SessionSetExpectedCabinets(unsigned cabinets)
{
  std::lock_guard<std::mutex> lock(g_session.mutex);
  g_session.expectedCabinets = std::max(2u, std::min(16u, cabinets));
}

std::vector<QueuedPacket> TakePackets()
{
  std::vector<QueuedPacket> packets;
  std::lock_guard<std::mutex> lock(g_session.mutex);
  packets.swap(g_session.packets);
  return packets;
}

void PollPackets()
{
  retro_netpacket_poll_receive_t pollReceive = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_session.mutex);
    if (g_session.active)
      pollReceive = g_session.pollReceive;
  }
  if (pollReceive)
    pollReceive();
}

bool SendPacket(const std::vector<uint8_t> &packet, uint16_t peer)
{
  retro_netpacket_send_t send = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_session.mutex);
    if (g_session.active)
      send = g_session.send;
  }
  if (!send)
    return false;

  send(RETRO_NETPACKET_RELIABLE | RETRO_NETPACKET_FLUSH_HINT,
       packet.data(), packet.size(), peer);
  return true;
}

uint16_t ReadU16(const uint8_t *data)
{
  return static_cast<uint16_t>(data[0]) |
         (static_cast<uint16_t>(data[1]) << 8);
}

uint32_t ReadU32(const uint8_t *data)
{
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

void AppendU16(std::vector<uint8_t> &data, uint16_t value)
{
  data.push_back(static_cast<uint8_t>(value));
  data.push_back(static_cast<uint8_t>(value >> 8));
}

void AppendU32(std::vector<uint8_t> &data, uint32_t value)
{
  data.push_back(static_cast<uint8_t>(value));
  data.push_back(static_cast<uint8_t>(value >> 8));
  data.push_back(static_cast<uint8_t>(value >> 16));
  data.push_back(static_cast<uint8_t>(value >> 24));
}

uint32_t HashName(const std::string &name)
{
  uint32_t hash = 2166136261u;
  for (unsigned char c : name)
  {
    hash ^= c;
    hash *= 16777619u;
  }
  return hash;
}
}

CLibretroNetBoard::CLibretroNetBoard(const Util::Config::Node &config)
  : m_config(config)
{
}

CLibretroNetBoard::~CLibretroNetBoard() = default;

bool CLibretroNetBoard::RegisterNetpacketInterface(
  retro_environment_t environment, retro_log_printf_t logger)
{
  static const retro_netpacket_callback callbacks = {
    NetpacketStart,
    NetpacketReceive,
    NetpacketStop,
    nullptr,
    NetpacketConnected,
    NetpacketDisconnected,
    "Supermodel Model 3 NetBoard v2"
  };

  const bool supported = environment &&
    environment(RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE,
                const_cast<retro_netpacket_callback *>(&callbacks));
  {
    std::lock_guard<std::mutex> lock(g_session.mutex);
    g_session.interfaceSupported = supported;
    g_session.log = logger;
  }
  return supported;
}

void CLibretroNetBoard::ShutdownNetpacketInterface()
{
  std::lock_guard<std::mutex> lock(g_session.mutex);
  g_session.active = false;
  g_session.send = nullptr;
  g_session.pollReceive = nullptr;
  g_session.clients.clear();
  g_session.packets.clear();
}

bool CLibretroNetBoard::IsGame(const char *gameName) const
{
  return m_gameInfo.name == gameName || m_gameInfo.parent == gameName;
}

CLibretroNetBoard::GameType CLibretroNetBoard::DetectGameType() const
{
  if (IsGame("daytona2") || IsGame("harley") || IsGame("scud") ||
      IsGame("srally2") || IsGame("skichamp") || IsGame("spikeout") ||
      IsGame("spikeofe"))
    return GameType::Type1;
  if (IsGame("lemans24") || IsGame("von2") || IsGame("dirtdvls"))
    return GameType::Type2;
  return GameType::Unsupported;
}

const char *CLibretroNetBoard::NetworkFamily() const
{
  if (IsGame("daytona2"))
    return "daytona2";
  if (IsGame("dirtdvls"))
    return "dirtdvls";
  if (IsGame("harley"))
    return "harley";
  if (IsGame("lemans24"))
    return "lemans24";
  if (IsGame("scud"))
    return "scud";
  if (IsGame("skichamp"))
    return "skichamp";
  if (IsGame("spikeofe"))
    return "spikeofe";
  if (IsGame("spikeout"))
    return "spikeout";
  if (IsGame("srally2"))
    return "srally2";
  if (IsGame("von2"))
    return "von2";
  return nullptr;
}

bool CLibretroNetBoard::HasNetpacketTransport() const
{
  return m_gameType == GameType::Type1 || m_gameType == GameType::Type2;
}

bool CLibretroNetBoard::IsRelayRole(uint16_t role) const
{
  return m_gameType == GameType::Type2 && role >= 0x8000;
}

const char *CLibretroNetBoard::RoleName(uint16_t role) const
{
  if (role == 0)
    return "Master";
  return IsRelayRole(role) ? "Relay/Satellite" : "Slave";
}

Result CLibretroNetBoard::Init(UINT8 *netRAMPtr, UINT8 *netBufferPtr)
{
  m_ram = netRAMPtr;
  m_buffer = netBufferPtr;
  m_commRAM = m_buffer;
  m_externalCommRAM = m_buffer + 0x10000;
  m_expectedCabinets = std::max(
    2u, std::min(16u, m_config["NetworkCabinets"].ValueAs<unsigned>()));
  SessionSetExpectedCabinets(m_expectedCabinets);
  const bool requested = m_gameInfo.netboard_present &&
                         m_config["Network"].ValueAs<bool>();

  if (!requested)
    return Result::OKAY;
  m_gameType = DetectGameType();
  const char *networkFamily = NetworkFamily();
  if (m_gameType == GameType::Unsupported || !networkFamily)
  {
    InfoLog("Network Board is not recognized for %s; ignoring the request",
            m_gameInfo.name.c_str());
    return Result::OKAY;
  }

  m_attached = true;
  m_transportSupported = HasNetpacketTransport() &&
                         SessionInterfaceSupported();
  m_gameHash = HashName(networkFamily);
  Reset();
  if (m_transportSupported)
    InfoLog("Libretro Network Board connected (%s family; %u-cabinet "
            "netpacket transport configured)", networkFamily,
            m_expectedCabinets);
  else if (HasNetpacketTransport())
    InfoLog("Libretro Network Board connected (%s family; frontend "
            "netpacket transport unavailable)", networkFamily);
  else
    InfoLog("Libretro Network Board connected (%s family; linked play "
            "transport not yet implemented)", networkFamily);
  return Result::OKAY;
}

void CLibretroNetBoard::Reset()
{
  m_running = false;
  m_commBank = false;
  m_commRAM = m_buffer;
  m_externalCommRAM = m_buffer ? m_buffer + 0x10000 : nullptr;
  m_state = State::Start;
  m_irq2Ack = 0;
  m_status0 = 0;
  m_status1 = 0;
  m_counter = 0;
  m_segmentSize = 0;
  m_localRole = 0;
  m_machineIndex = 0;
  m_playableIndex = 0;
  m_helloInterval = 0;
  m_readyDelay = 0;
  m_peerRoles.clear();
  m_roster.clear();
  m_framePackets.clear();
}

uint16_t CLibretroNetBoard::ReadNetRAM16(unsigned index) const
{
  uint16_t value = 0;
  // CSimNetBoard's historical RAM16[addr] macro appends [addr] to the
  // byte-pointer expression before casting, so these constants are byte
  // offsets despite looking like uint16_t array indices.
  std::memcpy(&value, m_ram + index, sizeof(value));
  return value;
}

void CLibretroNetBoard::WriteCommWord(unsigned index, uint16_t value)
{
  std::memcpy(m_commRAM + index, &value, sizeof(value));
}

std::vector<uint8_t> CLibretroNetBoard::BuildPacket(
  uint8_t type, uint32_t frame, const uint8_t *payload,
  size_t payloadSize) const
{
  std::vector<uint8_t> bytes;
  bytes.reserve(kPacketHeaderSize + payloadSize);
  bytes.push_back('S');
  bytes.push_back('M');
  bytes.push_back('3');
  bytes.push_back('N');
  bytes.push_back(kProtocolVersion);
  bytes.push_back(type);
  AppendU16(bytes, m_localRole);
  AppendU32(bytes, m_gameHash);
  AppendU32(bytes, frame);
  AppendU16(bytes, static_cast<uint16_t>(payloadSize));
  if (payload && payloadSize)
    bytes.insert(bytes.end(), payload, payload + payloadSize);
  return bytes;
}

bool CLibretroNetBoard::DecodePacket(const std::vector<uint8_t> &bytes,
                                     ParsedPacket &packet) const
{
  if (bytes.size() < kPacketHeaderSize || bytes[0] != 'S' ||
      bytes[1] != 'M' || bytes[2] != '3' || bytes[3] != 'N' ||
      bytes[4] != kProtocolVersion || ReadU32(bytes.data() + 8) != m_gameHash)
    return false;

  const uint16_t payloadSize = ReadU16(bytes.data() + 16);
  if (bytes.size() != kPacketHeaderSize + payloadSize)
    return false;

  packet.type = bytes[5];
  packet.role = ReadU16(bytes.data() + 6);
  packet.frame = ReadU32(bytes.data() + 12);
  packet.payload.assign(bytes.begin() + kPacketHeaderSize, bytes.end());
  return true;
}

void CLibretroNetBoard::DrainPackets()
{
  for (auto &queued : TakePackets())
  {
    ParsedPacket packet;
    if (!DecodePacket(queued.bytes, packet))
      continue;

    if (packet.type == kPacketHello)
    {
      // A fast peer can complete its own Model 3 initialization first. Ignore
      // (rather than interpret) early hellos until this cabinet has received
      // its role and segment layout from the emulated main board; the peer
      // retransmits the hello while testing.
      if (m_state != State::Testing ||
          (m_gameType == GameType::Type2 && !m_segmentSize))
        continue;
      if (packet.payload.size() != sizeof(uint16_t))
      {
        EnterError("A linked cabinet sent an invalid handshake");
        return;
      }
      const unsigned peerCabinets = ReadU16(packet.payload.data());
      if (peerCabinets != m_expectedCabinets)
      {
        EnterError("Linked cabinets use different Linked Cabinets settings");
        return;
      }
      const bool firstHello = m_peerRoles.count(queued.sender) == 0;
      m_peerRoles[queued.sender] = packet.role;
      if (firstHello)
        InfoLog("Libretro NetBoard received peer hello: client=%u, role=%s "
                "(%u cabinets expected)", queued.sender,
                RoleName(packet.role),
                peerCabinets);
      // Reply before changing state. Otherwise the last cabinet to complete
      // its roster could become Ready without letting an earlier peer see it.
      SendHello();
      TryCompleteHandshake();
    }
    else if (packet.type == kPacketFrame && m_state == State::Ready &&
             queued.sender != SessionLocalId() &&
             std::find(m_roster.begin(), m_roster.end(), queued.sender) !=
               m_roster.end())
    {
      const auto expectedRole = m_peerRoles.find(queued.sender);
      if (expectedRole == m_peerRoles.end() ||
          packet.role != expectedRole->second)
      {
        EnterError("A linked cabinet changed Link Mode during play");
        return;
      }
      m_framePackets[packet.frame][queued.sender] = std::move(packet.payload);
      while (m_framePackets.size() > 4)
        m_framePackets.erase(m_framePackets.begin());
    }
  }
}

void CLibretroNetBoard::SendHello()
{
  if (!SessionActive())
    return;
  std::vector<uint8_t> payload;
  AppendU16(payload, static_cast<uint16_t>(m_expectedCabinets));
  SendPacket(BuildPacket(kPacketHello, 0, payload.data(), payload.size()),
             RETRO_NETPACKET_BROADCAST);
}

bool CLibretroNetBoard::TryCompleteHandshake()
{
  if (m_state != State::Testing ||
      m_peerRoles.size() < m_expectedCabinets - 1)
    return false;
  if (m_peerRoles.size() != m_expectedCabinets - 1)
  {
    EnterError("More linked cabinets responded than configured");
    return false;
  }

  const uint16_t localId = SessionLocalId();
  if ((localId == 0) != (m_localRole == 0))
  {
    EnterError("RetroArch host must use Link Mode Master; clients must use Slave");
    return false;
  }

  m_roster.clear();
  m_roster.reserve(m_expectedCabinets);
  m_roster.push_back(localId);
  for (const auto &peer : m_peerRoles)
    m_roster.push_back(peer.first);
  std::sort(m_roster.begin(), m_roster.end());
  if (m_roster.size() != m_expectedCabinets || m_roster.front() != 0 ||
      std::adjacent_find(m_roster.begin(), m_roster.end()) != m_roster.end())
  {
    EnterError("Unable to build a unique linked-cabinet roster");
    return false;
  }

  for (uint16_t participant : m_roster)
  {
    const uint16_t role = participant == localId
      ? m_localRole : m_peerRoles[participant];
    if (participant == 0 ? role != 0 : role == 0)
    {
      EnterError(participant == 0
        ? "RetroArch host must use Link Mode Master"
        : "RetroArch clients cannot use Link Mode Master");
      return false;
    }
    if (m_gameType == GameType::Type1 && participant != 0 && role != 1)
    {
      EnterError("Every Type 1 RetroArch client must use Link Mode Slave");
      return false;
    }
  }

  const auto local = std::find(m_roster.begin(), m_roster.end(), localId);
  m_machineIndex = static_cast<uint16_t>(local - m_roster.begin());
  EnterReadyState();
  return m_state == State::Ready;
}

void CLibretroNetBoard::EnterReadyState()
{
  const uint16_t localId = SessionLocalId();
  const bool isMaster = m_localRole == 0;
  if ((localId == 0) != isMaster ||
      m_roster.size() != m_expectedCabinets ||
      m_machineIndex >= m_roster.size())
  {
    EnterError("The linked-cabinet roster is incomplete");
    return;
  }

  const uint16_t otherMachines = static_cast<uint16_t>(
    m_expectedCabinets - 1);
  m_segmentSize = ReadNetRAM16(
    m_gameType == GameType::Type1 ? 0x404 : 0x204);
  if (!m_segmentSize ||
      0x100u + (m_expectedCabinets + 1u) * m_segmentSize > 0x10000u)
  {
    EnterError("The game reported an invalid network segment size");
    return;
  }

  if (m_gameType == GameType::Type1)
  {
    m_status0 = 0;
    m_status1 = 0x2021 + otherMachines * 0x20 + m_machineIndex;
    WriteCommWord(0x0, ReadNetRAM16(0x400));
    WriteCommWord(0x2, otherMachines);
    WriteCommWord(0x4, m_machineIndex);
    WriteCommWord(0x8, FLIPENDIAN16(0x100 + m_segmentSize));
    WriteCommWord(0xa,
                  FLIPENDIAN16(ReadNetRAM16(0x402) - m_segmentSize - 1));
    WriteCommWord(0xc, FLIPENDIAN16(0x100));
    WriteCommWord(0xe,
                  FLIPENDIAN16(ReadNetRAM16(0x402) - m_segmentSize + 0x200));
  }
  else
  {
    unsigned playableCabinets = 0;
    m_playableIndex = 0;
    for (uint16_t participant : m_roster)
    {
      const uint16_t role = participant == localId
        ? m_localRole : m_peerRoles[participant];
      if (participant == localId)
        m_playableIndex = static_cast<uint16_t>(playableCabinets);
      if (!IsRelayRole(role))
        ++playableCabinets;
    }
    if (IsRelayRole(m_localRole) && !IsGame("dirtdvls"))
      m_playableIndex |= 0x80;

    const uint16_t otherPlayable = static_cast<uint16_t>(
      playableCabinets - 1);
    m_status0 = 5;
    m_status1 = IsGame("dirtdvls")
      ? static_cast<uint16_t>(0x7400 | (otherPlayable << 4) |
                              m_playableIndex)
      : static_cast<uint16_t>((otherPlayable << 8) | m_playableIndex);
    WriteCommWord(0x0, ReadNetRAM16(0x200));
    WriteCommWord(0x2,
                  static_cast<uint16_t>((otherPlayable << 8) |
                                        otherMachines));
    WriteCommWord(0x4,
                  static_cast<uint16_t>((m_playableIndex << 8) |
                                        m_machineIndex));
    WriteCommWord(0x8, FLIPENDIAN16(0x100 + m_segmentSize));
    WriteCommWord(0xa, FLIPENDIAN16(ReadNetRAM16(0x206)));
    WriteCommWord(0xc, FLIPENDIAN16(0x100));
    WriteCommWord(0xe,
                  FLIPENDIAN16(ReadNetRAM16(0x206) + 0x80));
  }
  WriteCommWord(0x6, 0);
  m_counter = 0;
  // Let both frontends complete the handshake frames before either core starts
  // a blocking board exchange. Without this small barrier the faster peer can
  // wait inside retro_run while the slower frontend is still finalizing its
  // netplay frame, preventing the latter from reaching its own exchange.
  m_readyDelay = 4;
  m_state = State::Ready;
  InfoLog("Libretro network link ready: %s, cabinet %u of %u, segment "
          "0x%X bytes", RoleName(m_localRole), m_machineIndex + 1,
          m_expectedCabinets, m_segmentSize);
}

void CLibretroNetBoard::ExchangeFrame()
{
  if (!SessionActive())
  {
    EnterError("Netpacket session ended while the cabinet link was active");
    return;
  }

  const uint32_t nextFrame = static_cast<uint16_t>(m_counter + 1);
  std::vector<uint8_t> localSegment(m_segmentSize);
  std::memcpy(localSegment.data(), m_commRAM + 0x100, m_segmentSize);
  WriteCommWord(0x6, FLIPENDIAN16(static_cast<uint16_t>(nextFrame)));

  if (!SendPacket(BuildPacket(kPacketFrame, nextFrame,
                              localSegment.data(), localSegment.size()),
                  RETRO_NETPACKET_BROADCAST))
  {
    EnterError("Unable to send linked-cabinet network frame");
    return;
  }
  if (nextFrame == 1)
    InfoLog("Libretro NetBoard sent first data frame; waiting for %u peers",
            m_expectedCabinets - 1);

  const auto deadline = std::chrono::steady_clock::now() + kFrameWaitTimeout;
  do
  {
    PollPackets();
    DrainPackets();
    if (m_state == State::Error)
      return;

    const auto received = m_framePackets.find(nextFrame);
    if (received != m_framePackets.end() &&
        received->second.size() >= m_expectedCabinets - 1)
    {
      // The standalone TCP ring receives segments from the immediately
      // preceding cabinet backwards around the ring, and finally receives its
      // own segment. Reconstruct that same per-cabinet order from the common
      // roster rather than imposing the host's absolute order on every node.
      for (unsigned step = 1; step < m_expectedCabinets; ++step)
      {
        const unsigned sourceIndex =
          (m_machineIndex + m_expectedCabinets - step) % m_expectedCabinets;
        const uint16_t sourceId = m_roster[sourceIndex];
        const auto segment = received->second.find(sourceId);
        if (segment == received->second.end())
        {
          EnterError("A linked-cabinet frame is missing a roster segment");
          return;
        }
        if (segment->second.size() != m_segmentSize)
        {
          EnterError("A peer sent a network segment of the wrong size");
          return;
        }
        std::memcpy(m_commRAM + 0x100 + step * m_segmentSize,
                    segment->second.data(), m_segmentSize);
      }
      // The final ring segment is the local cabinet's own data. Copy it
      // locally instead of broadcasting a redundant loopback packet.
      std::memcpy(m_commRAM + 0x100 + m_expectedCabinets * m_segmentSize,
                  localSegment.data(), m_segmentSize);
      m_counter = static_cast<uint16_t>(nextFrame);
      m_framePackets.erase(m_framePackets.begin(),
                           std::next(received));
      SwapCommBanks();
      if (nextFrame == 1)
        InfoLog("Libretro NetBoard first data-frame exchange completed");
      return;
    }
    std::this_thread::yield();
  }
  while (SessionActive() && std::chrono::steady_clock::now() < deadline);

  EnterError("Timed out waiting for the linked-cabinet peer frame");
}

void CLibretroNetBoard::SwapCommBanks()
{
  m_commBank = !m_commBank;
  if (m_commBank)
  {
    m_commRAM = m_buffer + 0x10000;
    m_externalCommRAM = m_buffer;
  }
  else
  {
    m_commRAM = m_buffer;
    m_externalCommRAM = m_buffer + 0x10000;
  }
}

void CLibretroNetBoard::EnterError(const char *message)
{
  if (m_state != State::Error)
    ErrorLog("Libretro NetBoard: %s", message);
  if (m_gameType == GameType::Type1)
    m_status1 = 0x40;
  else if (IsGame("dirtdvls"))
    m_status1 = 0x8085;
  m_state = State::Error;
}

void CLibretroNetBoard::RunFrame()
{
  if (!IsRunning())
    return;
  if (m_state == State::Error)
    return;

  if (m_transportSupported)
  {
    PollPackets();
    DrainPackets();
    if (m_state == State::Error)
      return;
  }

  switch (m_state)
  {
  case State::Start:
    m_status0 = 0;
    m_status1 = IsGame("dirtdvls") ? 0x4004 : 0xe000;
    m_state = State::Init;
    break;

  case State::Init:
    std::memset(m_buffer, 0, 0x20000);
    if (m_gameType == GameType::Type1)
    {
      if (m_status0 & 0x8000)
      {
        m_irq2Ack |= 0x01;
        if (m_status0 == 0xf000)
        {
          m_status1 = 0;
          WriteCommWord(0x72, FLIPENDIAN16(0x1));
          const uint16_t roleValue = ReadNetRAM16(0x400);
          m_localRole = roleValue == 0 ? 0 : 1;
          InfoLog("Libretro NetBoard link request: client=%u, role=%s "
                  "(RAM[400]=0x%04X, RAM[402]=0x%04X, RAM[404]=0x%04X)",
                  SessionLocalId(), m_localRole == 0 ? "Master" : "Slave",
                  roleValue, ReadNetRAM16(0x402), ReadNetRAM16(0x404));
          m_state = State::Testing;
        }
        m_status0 = 0;
      }
    }
    else
    {
      // Type 2 boards perform their own initialization. This mirrors the
      // standalone simulator and is enough for the game and Service Menu to
      // observe the connected hardware even before a transport is available.
      m_status1 = 0;
      m_counter = 0;
      m_state = State::Testing;
    }
    break;

  case State::Testing:
  {
    if (m_gameType == GameType::Type1)
      ++m_status0;
    else if (!m_segmentSize)
    {
      const uint16_t segmentSize = ReadNetRAM16(0x204);
      if (!segmentSize)
        break;
      m_localRole = ReadNetRAM16(0x200);
      m_segmentSize = segmentSize;
      InfoLog("Libretro NetBoard link request: client=%u, role=%s "
              "(RAM[200]=0x%04X, RAM[204]=0x%04X, RAM[206]=0x%04X)",
              SessionLocalId(), RoleName(m_localRole), m_localRole,
              m_segmentSize, ReadNetRAM16(0x206));
    }
    if (!m_transportSupported)
      break;
    if (!SessionActive())
      break;
    if (m_helloInterval++ == 0 || m_helloInterval >= 15)
    {
      m_helloInterval = 0;
      SendHello();
    }
    // Poll immediately after the hello so localhost links do not have to wait
    // for another emulated frame before completing their handshake.
    PollPackets();
    DrainPackets();
    break;
  }

  case State::Ready:
    if (m_readyDelay)
      --m_readyDelay;
    else
      ExchangeFrame();
    break;

  case State::Error:
    break;
  }
}

UINT8 CLibretroNetBoard::ReadCommRAM8(unsigned addr)
{
  return m_externalCommRAM[addr];
}

UINT16 CLibretroNetBoard::ReadCommRAM16(unsigned addr)
{
  uint16_t value = 0;
  std::memcpy(&value, m_externalCommRAM + addr, sizeof(value));
  return value;
}

UINT32 CLibretroNetBoard::ReadCommRAM32(unsigned addr)
{
  uint32_t value = 0;
  std::memcpy(&value, m_externalCommRAM + addr, sizeof(value));
  return value;
}

void CLibretroNetBoard::WriteCommRAM8(unsigned addr, UINT8 data)
{
  m_externalCommRAM[addr] = data;
}

void CLibretroNetBoard::WriteCommRAM16(unsigned addr, UINT16 data)
{
  std::memcpy(m_externalCommRAM + addr, &data, sizeof(data));
}

void CLibretroNetBoard::WriteCommRAM32(unsigned addr, UINT32 data)
{
  std::memcpy(m_externalCommRAM + addr, &data, sizeof(data));
}

UINT16 CLibretroNetBoard::ReadIORegister(unsigned reg)
{
  if (!IsRunning())
    return 0;

  switch (reg)
  {
  case 0x00: return m_irq2Ack;
  case 0x88: return m_status0;
  case 0x8a: return m_status1;
  default:
    ErrorLog("Libretro NetBoard read from unknown IO register 0x%02X", reg);
    return 0;
  }
}

void CLibretroNetBoard::WriteIORegister(unsigned reg, UINT16 data)
{
  switch (reg)
  {
  case 0x00:
    m_irq2Ack = data;
    break;
  case 0x88:
    m_status0 = data;
    break;
  case 0x8a:
    m_status1 = data;
    break;
  case 0xc0:
    if (data == 0)
      Reset();
    else
      m_running = true;
    break;
  default:
    ErrorLog("Libretro NetBoard write to unknown IO register 0x%02X", reg);
    break;
  }
}
