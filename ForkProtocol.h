#ifndef __FORK_PROTOCOL_H__
#define __FORK_PROTOCOL_H__

// ---------------------------------------------------------------------------
// ForkProtocol : compatibility shim between the stock 10068 gameserver and the
// 10096 "fork" client.
//
// The 10096 client renumbered and reshaped a handful of C2S packets without
// touching the RakNet framing that wraps them:
//
//   wire     =>   [0x87 messageStart][varint packetId][packet body]
//
// The reshuffling is small and fully mapped (see kForkC2SWireTable):
//
//   * ids 7/8 are swapped: the fork's player movement rides id 8 (stock 7),
//     so a fork id 7 carries a stock VehicleMovement body;
//   * stock 80 (WatchRespawn) was moved to the end of the 80..84 block, which
//     shifts the input packets down by one:
//         fork 80 = stock 81 MovementInput      (8 float bytes + nested
//                                                 id-8 movement records)
//         fork 81 = stock 82 SendJumpChanged    (1 flag byte)
//         fork 82 = stock 83 SendPlayerRotation (8 float bytes)
//         fork 83 = stock 84 SendDownChanged    (1 flag byte)
//         fork 84 = the relocated WatchRespawn (empty body)
//   * every other id is 10068's own, with 10068's body: the fork's own
//     packets are appended at 179+ (DHInfo is 179).
//
// Undoing that here matters twice over. First, a wrong body fed to a stock
// decoder desyncs the RakNet receive loop: it re-reads leftover body bytes as
// a bogus "packet id" and starts a cascade of
//
//     unknown packet id: 0
//     unknown packet id: 4339 / 4340 / 5004 / 5188
//     Failed to read Real          /  reached end of stream
//
// that the old builds logged every frame and that aborts whole datagrams
// (`return` in RaknetInstance::handleNormalData). Second - and this is the
// bug that made 10096 look broken - a DRAINED packet aborts the whole
// message too (RaknetInstance.cpp:475-479 breaks the loop), so draining ids
// 13/17/129 threw the held-item packets away AND everything packed behind
// them, movement records included. Everything the stock decoders can read is
// therefore passed through now, and only bodies that cannot hold the stock
// layout are drained.
//
// This file is the single place where the renumbering / body reshaping is
// undone. It exposes @c onRecvC2S which is installed unconditionally in
// ServerNetworkCore::host() as the C2S gateway, and is fully pref-independent:
//
//   * a stock 10068 client never sends id 179, so it is never classified, and
//     every id it sends (5, 8, 9, 13, 17, ...) is passed through untouched;
//   * the 10096 fork client is recognised purely from the wire (first C2S id
//     179 only); once a session is classified its ids are remapped / validated
//     here and the stock handlers run as if it were a 10068 client.
//
// The same GameServer_d.exe therefore still serves stock clients without any
// config change (the "UseForkProtocol" player pref is never read; a comment
// below explains why it is intentionally dead).
// ---------------------------------------------------------------------------

#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <set>
#include <mutex>
#include <memory>
#include "Log/Log.h"
#include "Util/PlayerPrefs.h"
#include "Network/RaknetInstance.h"
#include "Network/protocol/DataPacket.h"
#include "Network/protocol/Serializer.h"
#include "Network/protocol/C2SPackets.h"
#include "Network/ServerNetworkRecver.h"
#include "ServerNetwork.h"
#include "ServerPacketSender.h"
#include "ClientPeer.h"

// Emits a pre-built payload under a chosen packet id, used to inject fork-only
// packets (e.g. the expanded LoginResult above) through the normal
// RaknetSession::sendPacket path without touching the shared protocol headers.
class ForkRawPacket : public DataPacket
{
public:
	virtual ui16 pid() const override { return m_pid; }
	virtual int channel() const override { return m_channel; }
	virtual void encode(RakNet::BitStream* stream) const override
	{
		if (!m_body.empty())
		{
			stream->Write(m_body.c_str(), static_cast<unsigned int>(m_body.size()));
		}
	}
	virtual void decode(RakNet::BitStream*) override {}

	ui16 m_pid = 0;
	std::string m_body;
	// RaknetSession::sendPacket CONCATENATES every packet that shares a channel
	// into a single RakNet message (RaknetSession.h:39-52), and the client's
	// decode loop aborts the rest of a message on the first packet it cannot
	// decode (RaknetInstance.cpp:496). Anything that must not be swallowed has to
	// live on its own channel so it is the only packet in its message.
	// Channel 1 = emNormal (everything else), 2 = emMapData, 3 = fork-critical.
	int m_channel = DataPacket::emNormal;
};

// ---------------------------------------------------------------------------
// S2C other-player movement -> 10096 fork sessions.
//
// THE STOCK MECHANISM (what this now reuses). In 10068 a player's OWN movement
// is never relayed by the entity tracker: EntityTrackerEntry::sendMovement
// returns early for players/vehicles unless a forced teleport is pending. Player
// replication instead rides three small C2S handlers on the MOVER's session
// (C2SPlayerActionPacketHandles.cpp):
//     MovementInput (1646)  -> broadCastPlayerMovementInput + broadCastPlayerMovement
//     SendJumpChanged (1671) -> broadCastPlayerJumpChanged + broadCastPlayerMovement
//     SendPlayerRotation (1684) -> broadCastPlayerRotation    + broadCastPlayerMovement
// broadCastPlayerRotation (ServerPacketSender2.cpp:341) emits
// S2CPacketSyncPlayerRotation, and broadCastPlayerMovement (:358) emits
// S2CPacketEntityPlayerTeleport (sendPlayerTeleport @493) with an ANCHORED,
// per (entity, viewer) RELATIVE position from
// TeleportPositionConverter::ConvertToRelativeAndUpdateAnchor.
// The receiving client converts the anchor back to absolute and applies it
// smoothly:
//     S2CEntityPacketHandles.cpp:872 S2CPacketEntityPlayerTeleport handler
//         toPos = TeleportPositionConverter::ConvertToAbsoluteAndUpdateAnchor(id, rel)
//         serverPos = (int)(toPos * 32)
//         EntityOtherPlayerMP: toPos.y += 1/32; setPositionAndRotation2(toPos,
//                              getNextYaw(), getNextPitch(), 3)   // interpolated
//     S2CStatusPacketHandles.cpp:229 S2CPacketSyncPlayerRotation handler
//         setPositionAndRotation2(position, yaw, pitch, 3)  // refreshes nextYaw/Pitch
// i.e. the model is placed at the player's CURRENT map position and animated,
// which is exactly the requested "replicate where they are".
//
// WHY IT WAS MISSING ON 10096. The fork client renumbers PlayerMovement to C2S
// id 8 and is drained by ForkProtocol::onRecvC2S; its MovementInput (81),
// SendPlayerRotation (83) and friends are drained too. So NONE of the three
// stock handlers above ever ran for a fork player, meaning no
// S2CPacketEntityPlayerTeleport was ever broadcast for them and every other
// client - stock or fork - saw the avatar frozen at its spawn position.
//
// THE FIX (server-only, no fork-specific S2C packet): after a fork session's
// injected movement has been applied, ClientPeer::movePlayer re-emits exactly
// what the stock handlers would have emitted:
//     broadCastPlayerRotation(entityId, false);  // when rotating
//     broadCastPlayerMovement(entityId, false);
// That is the same anchored S2CPacketEntityPlayerTeleport every 10068 client
// already understands, so stock viewers are unaffected and 10096 viewers now
// get the player at the right place. The tracker early-return and the pid-21
// movement overloads are back to their untouched stock form.
//
// DEAD ENDS (do not reintroduce):
//   * pid 359 = SpawnEntityItem on the client (DataPacket.h:379). Custom
//     records there made the client spawn BLOCKS at the moved-to position
//     instead of animating the player.
//   * stock pid-21 deltas to fork sessions -> other player disappears.
//   * fork-only S2CPacketEntityTeleport (floor(pos*32)) from EntityTrackerEntry:
//     the raw teleport handler (S2CEntityPacketHandles.cpp:168) snaps with a
//     +yOffset/+1/32 correction, which put the model "in the air" and then lost
//     it; the anchored player-teleport path above is the correct one.
//   * The old stock early-return (player movement never relayed) is why fork
//     viewers saw frozen players; EntityTrackerEntry falls through when the
//     mover is a fork session OR any tracking viewer is a fork session.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Byte-level helpers shared by the decoders below. Everything is little-endian
// on both stock and fork wire formats. The movement decode is exercised against
// the "justfixthewalkingbugs" capture (client port 63017 / server port 19130).
// ---------------------------------------------------------------------------
namespace forkdetail
{
	// Minimal Cartesian position reported by the fork movement packet.
	struct ForkMovement
	{
		float x, y, z, yaw, pitch;
		bool onGround;   // tail byte bit 7 (0x80), see decodeForkMovement
		bool valid;
	};

	// Float32 without relying on pointer alignment (body runs may start on any
	// byte) and NaN / Inf traps: an unchecked NaN at the server transform stage
	// would poison the player's position for everyone nearby.
	inline float readFloatLE(const ui8* p)
	{
		float v = 0.0f;
		std::memcpy(&v, p, sizeof(v));
		return v;
	}

	inline bool saneFloat(float v)
	{
		// NaN fails v == v; +-Inf fails (v - v)==0.
		return v == v && (v - v) == 0.0f;
	}

	// Canonical minimal stock Rebirth body:
	//   [String len=0][ui16 netVersion=0]   ->  00 00 00
	// Namespace-scope const has internal linkage, so including this header from
	// several TUs does not produce duplicate symbols; the address handed to the
	// gateway hook is the copy in the TU that runs onRecvC2S, and it stays alive
	// (static storage duration) until RaknetInstance reads it.
	const ui8 kCanonicalRebirth[3] = { 0x00, 0x00, 0x00 };

	// 10096 fork movement (fork id 8): a fixed 22-byte base frame
	//
	//   +----+-----+-----+-----+-----+-----+-----+
	//   | 00 |  x  |  y  |  z  | yaw |pitch| flg |
	//   +----+-----+-----+-----+-----+-----+-----+
	//     0    1     5     9    13    17    21
	//
	// Longer bodies (28/31/34/37/38/43/44/65 bytes seen in the capture) embed
	// the identical 22-byte frame first and then append packed extra records
	// (sub-moves), which are irrelevant server-side. The state byte at offset 0
	// is 0 for plain walks; the tail byte at offset 21 is a movement flag word.
	//
	// Tail-byte bits (measured against the walking capture by correlating each
	// byte value with whether the vertical delta of the same frame was stable):
	//
	//   bit7 0x80 : onGround. Frames whose y is stable (|dy| < 0.05) are
	//               0xE0 / 0xEA (bit set) while airborne frames (|dy| > 0.3,
	//               free fall) are 0x60 / 0x6A (bit clear) in over 95% of the
	//               samples. This is THE fix for the player "floating in the
	//               air": the gateway used to hard-code onGround = true, which
	//               made the server believe a falling player was grounded.
	//   bit6 0x40 : constant 1 on every observed frame (base walk state).
	//   bit5 0x20 : constant 1 on every observed frame (base walk state).
	//   bit3 0x08 : movement/sprint input (0x6A / 0xEA variants).
	//   bit1 0x02 : movement input (0x6A / 0xEA variants).
	//   bit0 0x01 : seen once (sneak / rare input); ignored.
	inline ForkMovement decodeForkMovement(const ui8* body, ui32 bodyLen)
	{
		ForkMovement mv = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, true, false };
		if (!body || bodyLen < 22)
		{
			return mv;
		}
		mv.x = readFloatLE(body + 1);
		mv.y = readFloatLE(body + 5);
		mv.z = readFloatLE(body + 9);
		mv.yaw = readFloatLE(body + 13);
		mv.pitch = readFloatLE(body + 17);
		mv.onGround = (body[21] & 0x80) != 0;
		if (!saneFloat(mv.x) || !saneFloat(mv.y) || !saneFloat(mv.z) ||
			!saneFloat(mv.yaw) || !saneFloat(mv.pitch))
		{
			return mv;
		}
		mv.valid = true;
		return mv;
	}

	// ---------------------------------------------------------------------
	// Stock C2S body-layout validators, ported 1:1 from the 10068 client:
	//   dev\client\Src\Network\ClientPacketSender.cpp (what the client writes)
	//   dev\logic\Src\Network\protocol\C2SPackets.h    (the field order)
	//   dev\logic\Src\Network\protocol\Serializer.h    (the byte shape)
	//
	// The 10096 fork is an updated 10068, so its bodies for every id below 80
	// are byte-compatible with stock - that is why chat (24) worked the moment
	// it was passed through, and why the item packets only ever needed to stop
	// being drained. Ids >= 80 are renumbered (see passForkInput).
	//
	// RaknetInstance::handleNormalData hands this hook the REMAINDER of the
	// RakNet message as `body` (RaknetInstance.cpp:467-473) - a fork message is
	// [id][body][id][body]... - so a validator only has to prove the stock
	// fields FIT at the head of that remainder. When they do, pass-through lets
	// the stock decoder read exactly those bytes and the receive loop carries
	// on with the packets that follow. When they do not, the body really is
	// foreign and draining it keeps the stock decoder from throwing inside
	// decode() (the old "unknown packet id: 0" / "reached end of stream" spam).
	// ---------------------------------------------------------------------
	struct StockBody
	{
		const ui8* p;
		ui32 len;
		ui32 pos;
		bool ok;

		StockBody(const ui8* body, ui32 bodyLen)
			: p(body), len(body ? bodyLen : 0), pos(0), ok(body != nullptr)
		{
		}

		bool bytes(ui32 n)
		{
			if (!ok || n > len - pos)
			{
				ok = false;
				return false;
			}
			pos += n;
			return true;
		}

		// One 7-bit varint (Serializer::innerRead). Signed members are zigzag
		// encoded, which does not change the byte shape, so a plain unsigned
		// decode is enough to skip them. Truncated / over-long encodings fail.
		bool varint(ui32& out)
		{
			if (!ok)
			{
				return false;
			}
			const ui32 start = pos;
			ui32 ret = 0;
			ui32 shift = 0;
			ui8 b = 0;
			do
			{
				if (!bytes(1))
				{
					return false;
				}
				b = p[pos - 1];
				ret |= static_cast<ui32>(b & 0x7F) << shift;
				shift += 7;
			} while ((b & 0x80) != 0 && (pos - start) < 5);
			if (b & 0x80)
			{
				ok = false;
				return false;
			}
			out = ret;
			return true;
		}

		bool varint()
		{
			ui32 v = 0;
			return varint(v);
		}

		bool fixed32() { return bytes(4); }  // real32 / Vector3 component
		bool flag() { return bytes(1); }     // bool / bool8

		// String / std::string: varint length + raw bytes.
		bool string()
		{
			ui32 n = 0;
			if (!varint(n))
			{
				return false;
			}
			return bytes(n);
		}
	};

	// True when `body` can be decoded as the stock 10068 packet `id`.
	inline bool fitsStock(ui16 id, const ui8* body, ui32 bodyLen)
	{
		StockBody r(body, bodyLen);
		switch (id)
		{
		case 5:   // Attack : i32 targetEntityRuntimeId + 3x real32 target
			return r.varint() && r.fixed32() && r.fixed32() && r.fixed32() && r.ok;
		case 13:  // SwapItemPosition      : i32 index1, i32 index2
		case 17:  // ChangeItemInHand      : i32 slotIndex, i32 currentItemIndex
			return r.varint() && r.varint() && r.ok;
		case 22:  // FinishBreakBlock      : empty
		case 41:  // ReloadBullet          : empty
		case 80:  // WatchRespawn          : empty (fork renumbers it away)
			return true;
		case 28:  // UseProp               : String propId
			return r.string() && r.ok;
		case 33:  // GunFireResult         : Vector3 pos, Vector3 dir, ui32
			// tracyType, Vector3i blockPos, Vector3 hitpos, ui32 hitEntityID,
			// bool isNeedConsumeBullet, bool isHeadshot
			return r.fixed32() && r.fixed32() && r.fixed32()
				&& r.fixed32() && r.fixed32() && r.fixed32()
				&& r.varint()
				&& r.varint() && r.varint() && r.varint()
				&& r.fixed32() && r.fixed32() && r.fixed32()
				&& r.varint()
				&& r.flag() && r.flag() && r.ok;
		case 59:  // CameraLockMovement    : Vector3 nextPosition
			return r.fixed32() && r.fixed32() && r.fixed32() && r.ok;
		case 60:  // CameraLock            : bool8 isLocked
		case 82:  // SendJumpChanged       : bool isJump
		case 84:  // SendDownChanged       : bool isDown
			return r.flag() && r.ok;
		case 81:  // MovementInput         : real32 forward, real32 strafe
		case 83:  // SendPlayerRotation    : real32 yaw, real32 pitch
			return r.fixed32() && r.fixed32() && r.ok;
		case 129: // ChangeCurrentItemInfo : i32 itemIndex
			return r.varint() && r.ok;
		default:
			return false;
		}
	}

	// Reference card of every C2S packet id the 10096 fork was observed to send
	// (from the walking-bug capture and the pre-join loading capture). It is a
	// documentation table only; the dispatch switch below is authoritative.
	struct ForkC2SIdDoc
	{
		uint16_t id;
		const char* name;    // what the fork client intends / stock id it maps to
		const char* policy;  // Pass / Consume / Handle+Consume / Rewrite+Pass
		const char* note;
	};

	const ForkC2SIdDoc kForkC2SWireTable[] =
	{
		{ 2,   "Login",                  "Pass",         "identical to stock 2; drives the normal login flow" },
		{ 4,   "ClientReady",            "Pass",         "identical to stock 4; completes the loading screen" },
		{ 5,   "Attack",                 "Validate+Pass","stock layout (varint target + 3x real32); drained only when truncated" },
		{ 7,   "VehicleMovement",        "Remap->8",     "ids 7/8 are swapped: a fork 7 carries a stock VehicleMovement body" },
		{ 8,   "Player movement",        "Handle+Consume", "fork renumber (stock 7); decoded -> movePlayer then drained" },
		{ 9,   "Rebirth / respawn",      "Rewrite+Pass", "canonical body [00 00 00] so the stock respawn handler runs" },
		{ 13,  "SwapItemPosition",       "Validate+Pass","stock layout (i32,i32 varints) - inventory swap" },
		{ 17,  "ChangeItemInHand",       "Validate+Pass","stock layout (i32,i32 varints) - held item replication" },
		{ 22,  "FinishBreakBlock",       "Pass",         "stock body is empty; drained only when the fork grows a body" },
		{ 24,  "ChatMessageString",      "Pass",         "identical on both sides; consuming it broke chat on 10096" },
		{ 28,  "UseProp",                "Validate+Pass","stock layout (String) - prop usage" },
		{ 33,  "GunFireResult",          "Validate+Pass","stock layout; drained when the body cannot hold it" },
		{ 41,  "ReloadBullet",           "Pass",         "stock body is empty" },
		{ 44,  "SelectRole",             "Pass",         "stock handler invoked fine in the log" },
		{ 59,  "CameraLockMovement",     "Validate+Pass","stock layout (Vector3 = 12 bytes)" },
		{ 60,  "CameraLock",             "Validate+Pass","stock layout (bool8 = 1 byte)" },
		{ 80,  "MovementInput bundle",   "Remap->81",    "fork dropped stock 80, so >=80 are shifted; 8B floats = stock 81" },
		{ 81,  "SendJumpChanged",        "Remap->82",    "1-byte body = stock 82; read 1 byte then the rest of the message" },
		{ 82,  "SendPlayerRotation",     "Remap->83",    "8B yaw/pitch = stock 83" },
		{ 83,  "SendDownChanged",        "Remap->84",    "1-byte body = stock 84" },
		{ 84,  "WatchRespawn relocated", "Validate+Pass","empty body -> stock 80, payload -> stock 84" },
		{ 85,  "(any other 2..178)",     "Pass",         "stock 10068 id with the stock 10068 body; 179+ is fork-only" },
		{ 129, "ChangeCurrentItemInfo",  "Validate+Pass","stock layout (i32 varint) - held item replication" },
		{ 179, "DHInfo handshake",       "Handshake",    "fork-only; answer with expanded LoginResult + DBDataReady" },
		{ 0,   "(wire separator)",       "n/a",          "0x87 app-message separators are not ids; loop guards them" },
	};
} // namespace forkdetail

// Compatibility shim between the stock 10068 gameserver and the 10096 fork
// client. It does not rely on any player pref: the C2S packet-id gateway is
// installed unconditionally and treats a peer as the fork client when its wire
// traffic proves it (id 179). Stock 10068 clients
// never send 179 and keep their stock 5/8 behaviour, so the same binary serves
// both.
//
// WHY THE PREF IS DEAD (do not try to "fix"):
//   isEnabled() reads the "UseForkProtocol" player pref, which nothing in this
//   repository ever sets. The gate exists so {stock5 == 8 attack} and vehicle
//   movement (stock 8) are not mis-translated for stock clients; the fork is
//   now detected from the wire instead. Keep the two mechanisms separate.
class ForkProtocol
{
public:
	// RakNet channel reserved for fork-critical packets that must never share a
	// message with anything else. RaknetSession::sendPacket concatenates every
	// packet sharing a channel into ONE RakNet message, and the client aborts
	// the rest of a message on the first packet it cannot decode - so a packet
	// that shares a message with a badly-formed neighbour is silently dropped.
	// Channels 1 (emNormal) and 2 (emMapData) are already in use; 3 is free.
	static const int kForkCriticalChannel = 3;

	static bool isEnabled()
	{
		return BLOCKMAN::PlayerPrefs::Instance()->HasProperty("UseForkProtocol")
			&& BLOCKMAN::PlayerPrefs::Instance()->GetProperty<int>("UseForkProtocol") == 1;
	}

	// Map a C2S packet id received from the fork client onto the stock 10068
	// id the server understands. Falls back to identity for packets the fork
	// sends with the same id as the stock client. Kept for callers that only
	// need the remap table and not the full body-aware hook.
	static ui16 translateC2SPacketId(ui16 forkId)
	{
		return forkId;
	}

	// ---------------------------------------------------------------------
	// Session tracking: a peer stays classified as "fork" once it proved it by
	// sending id 179, 8 or (inside an already-classified session) any of the
	// fork-only ids below. Stock 10068 clients never send 179, so they are
	// never classified and keep stock 5/8.
	// ---------------------------------------------------------------------
	struct ForkSessions
	{
		std::mutex m_mutex;
		std::set<ui64> m_forkIds;

		// Marks the session; returns false when it was already known.
		bool remember(ui64 rakssid)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return !m_forkIds.insert(rakssid).second;
		}

		bool contains(ui64 rakssid)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_forkIds.count(rakssid) > 0;
		}

		void forget(ui64 rakssid)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_forkIds.erase(rakssid);
		}
	};
	static ForkSessions& forkSessions()
	{
		static ForkSessions sessions;
		return sessions;
	}

	// Convenience used by ClientPeer::movePlayer to trust fork movement (the
	// stock anti-cheat rubber-bands / kicks are bypassed for classified
	// sessions only).
	static bool isForkSession(ui64 rakssid)
	{
		return forkSessions().contains(rakssid);
	}

	// ---------------------------------------------------------------------
	// Join tracking for the settlement choke (ServerNetwork::sendPacket).
	//
	// The game-mode Lua scripts push the post-game "results" screen - the pid
	// based settlements (S2C 54 Settlement, 66 PlayerSettlement, 67
	// GameSettlement, 164 GameSettlementExtra) AND the S2CPacketServerCommonData
	// keys that open the same gui (DeadSummaryData / SingleSettlement /
	// KeyAdSettlementSingle / DeathSettlement / ...) - at an ALREADY-PRESENT
	// player the moment ANOTHER player joins. That is a spurious "match over"
	// reaction to the join, not the real end of the match.
	//
	// A genuine match-over never coincides with a join, so the choke suppresses
	// a settlement for a fork session only while some player's join (login)
	// happened within kJoinSettlementGraceMs. Outside that window the packet is
	// forwarded untouched, so the real end screen still reaches the 10096
	// client. Stock sessions never take part in this.
	// ---------------------------------------------------------------------
	struct RecentJoins
	{
		std::mutex m_mutex;
		ui32 m_lastMs = 0;

		void mark(ui32 nowMs)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_lastMs = nowMs;
		}

		bool within(ui32 nowMs, ui32 windowMs)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_lastMs == 0)
			{
				return false;
			}
			return (nowMs - m_lastMs) < windowMs;
		}
	};
	static RecentJoins& recentJoins()
	{
		static RecentJoins joins;
		return joins;
	}

	// Grace window (ms) after the most recent player join during which a
	// settlement is treated as the spurious join-triggered one. Must cover the
	// whole load / spawn / first-tick of the joining client, which is what
	// actually triggers the screen on the already-present player.
	static const ui32 kJoinSettlementGraceMs = 20000;

	// Called from the C++ join path (ClientPeer::login).
	static void markPlayerJoined(ui32 nowMs)
	{
		recentJoins().mark(nowMs);
	}

	static bool settlementSuppressedByJoinWindow(ui32 nowMs)
	{
		return recentJoins().within(nowMs, kJoinSettlementGraceMs);
	}

	// Tracks for which sessions the fork login handshake (expanded LoginResult +
	// DBDataReady) was already emitted. It used to live as a function-local
	// static inside sendLoginPackets, which meant a reconnect that RakNet hands
	// the SAME SystemAddress integer could never be answered again: the second
	// id 179 found its id already in the set and returned early, so the client
	// was stuck on "Loading player data..." forever. Keeping it in a clearable
	// singleton lets clearSession() drop the entry on disconnect so the next
	// connection is served from scratch.
	struct ForkLoginSends
	{
		std::mutex m_mutex;
		std::set<ui64> m_sent;

		// Returns true when this call is the first for the session.
		bool markSent(ui64 rakssid)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_sent.insert(rakssid).second;
		}

		void forget(ui64 rakssid)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_sent.erase(rakssid);
		}
	};
	static ForkLoginSends& loginSends()
	{
		static ForkLoginSends sends;
		return sends;
	}

	// Drop every bit of per-session fork state. Called from
	// ServerNetwork::onDisconnect (and therefore from clearPlayerCache's callers)
	// so a LEAVE followed by a fresh JOIN starts clean:
	//   * forkSessions(): the peer is no longer classified as fork;
	//   * loginSends():   the login handshake may be sent again.
	// Without this, reconnect with a reused raknet id kept serving the stale
	// "already sent" state and the fork client hung on "Loading player data...".
	static void clearSession(ui64 rakssid)
	{
		forkSessions().forget(rakssid);
		loginSends().forget(rakssid);
	}

	// ---------------------------------------------------------------------
	// Gateway entry point. Installed into RaknetInstance as the C2S gateway
	// (ServerNetworkCore::host). Called from the server network thread with the
	// raw body that follows the packet id. Exactly one of:
	//   Pass        -> stock factory decodes the (stock or rewritten) body
	//   Rewrite+Pass-> body replaced with a canonical stock layout first
	//   Consume     -> drained, the dispatch loop stops for this datagram
	// The RaknetInstance side also keeps the legacy kForkPacketConsumed sentinel
	// for the wire-format changeover; our lambda folds consumed into it.
	// ---------------------------------------------------------------------
	static RaknetInstance::ForkPacketHookResult onRecvC2S(ui64 rakssid, ui16 forkId,
		const ui8* body, ui32 bodyLen)
	{
		// Classified so far? (Only id 179 classifies a session; the fork always
		// sends 179 before its first movement - verified in the walking capture,
		// 179 at frame 272, first id-8 frame at 314 - so this flag is already
		// true by the time movement arrives.)
		const bool isFork = forkSessions().contains(rakssid);

		// 179: DHInfo "Other platform" login handshake. Only the 10096 client
		// sends it, right after Login, then waits for the server's answer before
		// leaving the loading screen (retries ~30s, then Logout). The official
		// server replies with the expanded LoginResult (id 50) + DBDataReady;
		// replicate the answer here and drain the packet.
		if (forkId == 179)
		{
			if (!isFork && !forkSessions().remember(rakssid))
			{
				LordLogInfo("ForkProtocol : detected 10096 fork client, rakid=%llu, first fork-only C2S id=%u",
					static_cast<unsigned long long>(rakssid), static_cast<unsigned>(forkId));
			}
			sendLoginPacketsForRakid(rakssid);
			return consumedResult();
		}

		// 8: fork player movement. Stock id 8 is VehicleMovement, but the fork
		// renumbered PlayerMovement here (stock uses 7). Only sessions that are
		// ALREADY classified as fork get the movement handling: a stock client
		// riding a vehicle keeps sending legitimate id-8 VehicleMovement and must
		// pass through to the stock decoder. The fork's frame is not the stock
		// layout, so decode it to a real stock C2SPacketPlayerMovement and inject
		// it into the main-thread receiver queue; the stock PlayerMovement
		// handler then runs movePlayer() with full context and correct threading.
		// The fork message itself is drained afterwards.
		if (forkId == 8)
		{
			if (!isFork)
			{
				return passThrough(forkId);
			}
			forkdetail::ForkMovement mv = forkdetail::decodeForkMovement(body, bodyLen);
			if (mv.valid)
			{
				injectForkMovement(rakssid, mv);
			}
			else
			{
				LordLogError("ForkProtocol : dropped malformed movement body, rakid=%llu len=%u",
					static_cast<unsigned long long>(rakssid), static_cast<unsigned>(bodyLen));
			}
			return consumedResult();
		}

		switch (forkId)
		{
		case 2:   // Login                        : identical on both sides.
		case 4:   // ClientReady                  : identical on both sides.
		case 24:  // ChatMessageString            : identical on both sides. The
			// 10096 fork uses the same [String] chat body as stock, so the stock
			// C2SPacketChatString handler works unchanged. This used to sit in the
			// consume list, which silently dropped every chat line from a fork
			// client - that is why chat did nothing on 10096.
		case 44:  // SelectRole                   : stock handler worked fine in the log.
			return passThrough(forkId);

		case 9:   // Rebirth / respawn. The fork sends a variable-size body that
			// stock Rebirth ([String name][ui16 netVersion]) cannot match, which
			// used to desync the parser and skip the respawn handler. Rewrite to
			// the canonical empty body so the stock Respawn path runs and the
			// fork client gets its resetpos answer.
			if (isFork)
			{
				return rewrite(forkId, forkdetail::kCanonicalRebirth, sizeof(forkdetail::kCanonicalRebirth));
			}
			return passThrough(forkId);

		case 5:   // Attack
		case 13:  // SwapItemPosition      : inventory swap
		case 17:  // ChangeItemInHand      : held item -> other players
		case 22:  // FinishBreakBlock
		case 28:  // UseProp
		case 33:  // GunFireResult
		case 41:  // ReloadBullet
		case 59:  // CameraLockMovement
		case 60:  // CameraLock
		case 129: // ChangeCurrentItemInfo : held item info -> other players
			// 10096 is an updated 10068, so these bodies are the stock
			// Serializer layout (see forkdetail::fitsStock, ported from the
			// client's ClientPacketSender.cpp + C2SPackets.h). The stock
			// handlers can therefore run unchanged - which is the only way the
			// fork client can hold, swap and use items at all: the old blanket
			// drain silently threw every one of these packets away before the
			// inventory / held-item code ever saw it. A body that cannot hold
			// the stock fields is ALWAYS drained, classified fork session or
			// not: server.log showed real decode throws ("Failed to read
			// Integer/String/Real: reached end of stream") for truncated
			// Attack bodies arriving while the session was unclassified - a
			// throw drops the whole datagram anyway, so draining first is
			// strictly better and cannot spam the log.
			if (!forkdetail::fitsStock(forkId, body, bodyLen))
			{
				return consumedResult();
			}
			return passThrough(forkId);

		case 80:  // MovementInput bundle -> stock 81
		case 81:  // SendJumpChanged       -> stock 82
		case 82:  // SendPlayerRotation    -> stock 83
		case 83:  // SendDownChanged       -> stock 84
		case 84:  // relocated WatchRespawn (stock 80) or a stock 84 flag
			// The fork dropped stock 80 (WatchRespawn) from its enum, so every
			// id above it moved down by one; see passForkInput for the mapping
			// and for why these must be REMAPPED instead of drained.
			if (isFork)
			{
				return passForkInput(forkId, body, bodyLen);
			}
			return passThrough(forkId);

		case 7:   // Stock 7 is PlayerMovement, stock 8 is VehicleMovement; the
			// fork SWAPPED those two ids (its movement rides id 8, handled
			// above), so a fork id 7 carries a stock VehicleMovement body and
			// must be handed to stock id 8 - passing it through as 7 would let
			// the PlayerMovement decoder eat a different field layout.
			if (isFork)
			{
				return passThrough(static_cast<ui16>(C2SPacketType::VehicleMovement));
			}
			return passThrough(forkId);

		default:
			// Everything else. Stock clients are untouched. For a classified
			// fork session the ids below are 10068's own: the fork only
			// reshuffles 7/8 and the 80..84 block (both handled above) and puts
			// its own packets at 179+ (DHInfo is 179), so any other id in
			// 2..178 carries the stock body and the stock handler can run.
			// Draining them - the old policy - is what made place / use /
			// break / craft / equip packets silently vanish for 10096 players.
			// Anything outside that range (0, 1, 179+) stays drained so the
			// receive loop never walks into a body it cannot decode.
			if (isFork)
			{
				if (forkId >= 2 && forkId <= 178)
				{
					return passThrough(forkId);
				}
				return consumedResult();
			}
			return passThrough(forkId);
		}
	}

	// Legacy two-argument entry point kept for callers compiled against the old
	// gateway signature; delegates to the full body-aware hook.
	static ui16 onRecvC2SId(ui64 rakssid, ui16 forkId)
	{
		RaknetInstance::ForkPacketHookResult res = onRecvC2S(rakssid, forkId, nullptr, 0);
		return res.consumed ? static_cast<ui16>(RaknetInstance::kForkPacketConsumed) : res.packetId;
	}

	// ---------------------------------------------------------------------
	// Result builders for the hook.
	// ---------------------------------------------------------------------
	static RaknetInstance::ForkPacketHookResult passThrough(ui16 forkId)
	{
		return RaknetInstance::ForkPacketHookResult{ forkId, false, nullptr, 0 };
	}

	static RaknetInstance::ForkPacketHookResult consumedResult()
	{
		return RaknetInstance::ForkPacketHookResult{ 0, true, nullptr, 0 };
	}

	static RaknetInstance::ForkPacketHookResult rewrite(ui16 stockId, const ui8* data, ui32 len)
	{
		return RaknetInstance::ForkPacketHookResult{ stockId, false, data, len };
	}

	// ---------------------------------------------------------------------
	// The fork's input ids are renumbered, and they must be REMAPPED rather
	// than drained.
	//
	// The 10096 enum dropped stock 80 (WatchRespawn), so everything above it
	// moved down by one:
	//   fork 80 = stock 81 MovementInput      ([forward][strafe], 8 float bytes)
	//   fork 81 = stock 82 SendJumpChanged    (1 flag byte)
	//   fork 82 = stock 83 SendPlayerRotation ([yaw][pitch], 8 float bytes)
	//   fork 83 = stock 84 SendDownChanged    (1 flag byte)
	// fork 80 additionally packs NESTED id-8 movement records behind the input
	// pair inside the SAME RakNet message (that is why its messages run 8, 10,
	// 15, 20, 31, 32 bytes long instead of a constant 8).
	//
	// The old policy drained all four, and a drained result makes
	// RaknetInstance break out of the message (RaknetInstance.cpp:475-479) -
	// so every packet behind them was thrown away, including those nested
	// movement records. That is why fork players never moved on other screens
	// and why their held-item packets (17/129, often packed behind an input
	// frame) never reached the inventory code either.
	//
	// Remapping keeps both the body and the loop: the stock decoder consumes
	// exactly the float pair / flag byte, and the receive loop then parses the
	// remaining bytes as packet ids - the nested 08s fall straight into the
	// id-8 movement handler above.
	// ---------------------------------------------------------------------
	static RaknetInstance::ForkPacketHookResult passForkInput(ui16 forkId,
		const ui8* body, ui32 bodyLen)
	{
		// End of the reshuffled block: an empty body is the relocated stock 80
		// (WatchRespawn, whose stock body is empty); anything with payload is
		// left on the stock id, which is what the flag packet below expects.
		if (forkId == 84)
		{
			if (bodyLen == 0)
			{
				return passThrough(static_cast<ui16>(C2SPacketType::WatchRespawn));
			}
			return passThrough(forkId);
		}

		// 8-byte head -> the stock float pair.
		if (forkId == 80 || forkId == 82)
		{
			if (forkdetail::fitsStock(81, body, bodyLen))
			{
				// fork 80 -> stock 81 MovementInput, fork 82 -> stock 83 Rotation.
				return passThrough(forkId == 80 ? static_cast<ui16>(81) : static_cast<ui16>(83));
			}
			if (forkId == 80 && bodyLen == 0)
			{
				// A bare WatchRespawn: stock id 80 has an empty body.
				return passThrough(static_cast<ui16>(C2SPacketType::WatchRespawn));
			}
			return consumedResult();
		}

		// 1-byte head -> the stock boolean flag.
		if (forkId == 81 || forkId == 83)
		{
			if (bodyLen >= 1 && body != nullptr)
			{
				// fork 81 -> stock 82 SendJumpChanged, fork 83 -> stock 84
				// SendDownChanged. Only ONE byte is handed to the stock decoder
				// on purpose: anything that follows belongs to the next packet
				// of the message (typically a nested id-8 movement frame).
				return passThrough(forkId == 81 ? static_cast<ui16>(82) : static_cast<ui16>(84));
			}
			return consumedResult();
		}

		return passThrough(forkId);
	}

	// ---------------------------------------------------------------------
	// Decoded fork movement -> main-thread stock movement packet.
	//
	// MovePlayer() and everything behind it (entity tracking, colliders,
	// S2C-broadcast of the 16-byte movement records, user-attribute persistence)
	// is not safe to run from the network thread: ServerNetworkRecver::recvPacket
	// enqueues normal C2S onto the game thread, and the translator callback
	// fires on the RakNet thread. We therefore rebuild a REAL C2SPacketPlayerMovement
	// and hand it to the SAME public receiver queue the stock flow uses; the
	// dispatcher then resolves the peer by packet->getSender() and the pre-handle
	// gate (requires an EntityPlayer for anyone but Login/Rebirth) protects the
	// pre-spawn window.
	// ---------------------------------------------------------------------
	static void injectForkMovement(ui64 rakssid, const forkdetail::ForkMovement& mv)
	{
		auto movement = std::make_shared<C2SPacketPlayerMovement>(
			mv.x, mv.y, mv.z, mv.yaw, mv.pitch, mv.onGround);
		movement->m_playerRuntimeId = 0;
		movement->m_moving = true;
		movement->m_rotating = true;
		movement->setSender(rakssid);

		std::shared_ptr<DataPacket> asPacket = movement;
		ServerNetwork::Instance()->getNetworkRecver()->recvPacket(asPacket);

		// onGround is the fork tail-byte bit 7 (see forkdetail::decodeForkMovement).
		// It must be forwarded verbatim: forcing it to true made the server treat a
		// falling player as grounded, so the entity hung where the client left the
		// ground and the player appeared to "float in the air". Stock movePlayer()
		// then also skips the jump()/in-air branches consistently; the anti-cheat
		// kick / rubber-band paths stay bypassed for classified sessions in
		// ClientPeer::movePlayer, so a rare mis-labeled frame can neither kick nor
		// rubber-band the fork player.
	}

	// Ping the reference table once at startup, purely for engineering
	// diagnostics; not wired into any hot path.
	static void dumpForkC2SWireTable()
	{
		for (const auto& row : forkdetail::kForkC2SWireTable)
		{
			LordLogInfo("ForkProtocol : C2S wire table [%3u] %-24s %-12s %s",
				static_cast<unsigned>(row.id), row.name, row.policy, row.note);
		}
	}

	// Answer the fork client's id 179 request through the normal player map, so
	// everything stays in this shim; delegates to the (once per connection)
	// login-packet send. Not gated on the pref: a 179 on the wire is proof.
	static void sendLoginPacketsForRakid(ui64 rakssid)
	{
		auto peer = ServerNetwork::Instance()->findPlayerByRakssid(rakssid);
		if (!peer || peer->getName().empty())
		{
			LordLogError("ForkProtocol : cannot answer id 179, player not found, rakid=%llu",
				static_cast<unsigned long long>(rakssid));
			return;
		}
		const int32_t entityId = (peer->getEntityPlayer() != nullptr) ? peer->getEntityPlayer()->entityId : 0;
		if (entityId == 0)
		{
			// The embedded ChangePlayerInfo needs a real entity id. Do NOT
			// consume the once-per-session mark here: ClientPeer::sendLoginResult
			// will emit a correct blob once the player is spawned.
			LordLogError("ForkProtocol : id 179 arrived before the player spawned, rakid=%llu",
				static_cast<unsigned long long>(rakssid));
			return;
		}
		sendLoginPackets(rakssid, peer->getName().c_str(), peer->getPlatformUserId(), entityId);
	}

	// Full fork login handshake. The 10096 client needs the expanded LoginResult
	// (id 50) with the player profile inline; the stock scalar LoginResult alone
	// leaves it on "Loading player data...".
	//
	// Exactly ONE expanded id-50 goes out per session. The working reference
	// capture (newdump.pcapng frame 208) sends a single 165-byte login blob and
	// the client then fires C2SPacketClientReady. Emitting a second copy - once
	// as the answer to id 179 and once from ClientPeer::sendLoginResult - made
	// the fork re-apply its own profile twice and it never became ready, so it
	// sat on "Loading player data..." forever. markSent() is dropped by
	// clearSession() on disconnect, so a reconnect is still answered once.
	static void sendLoginPackets(ui64 rakssid, const char* name, uint64_t platformUserId, int32_t entityId)
	{
		if (!loginSends().markSent(rakssid))
		{
			return;
		}

		// The expanded LoginResult (id 50) carrying the fork profile inline.
		auto forkLogin = std::make_shared<ForkRawPacket>();
		forkLogin->m_pid = static_cast<ui16>(S2CPacketType::LoginFailure);
		forkLogin->m_body = buildLoginResultBody(name, platformUserId, entityId);
		ServerNetwork::Instance()->sendPacket(forkLogin, rakssid);

		// NOTE: DBDataReady (id 348) is deliberately NOT sent here.
		//
		// It must be the LAST login packet, emitted only after the player profile
		// (S2CPacketUserIn) has gone out - see ClientPeer::sendLoginResult, which
		// calls sendPlayerDBDataReady() after the sendUserIn() calls.
		//
		// isDbDataReady() gates Blockman::onGameReady(), which immediately fires
		// C2SPacketClientReady and drops the client into the world. Sending 348
		// first opened a window in which the client finished loading while its
		// UserManager was still empty (S2CPacketUserIn had not been applied yet),
		// which crashed WinShell on connect.
	}

	// sub_B87140: encode a 16-byte compressed movement record (S2C).
	//   out[16]   : destination buffer
	//   prev, cur : previous / current entity position
	//   flags     : dword written to record[12..14]
	//   rot       : packed rotation nibble field spread over record[8..11]
	//   yaw,pitch : radians scaled by 32768 -> uint16 (clamped 0x8000)
	//   type      : record[3] payload byte
	static void packMovementRecord(uint8_t out[16], const float prev[3], const float cur[3],
		uint32_t flags, uint32_t rot, float yaw, float pitch, uint8_t type)
	{
		const float kPosScale = 15.0f;
		const float kRotScale = 32768.0f;
		for (int i = 0; i < 3; ++i)
		{
			int v = static_cast<int>((cur[i] - prev[i]) * kPosScale + 0.5f);
			if (v < 0)
				v = 0;
			if (v > 0xF0)
				v = 0xF0;
			out[i] = static_cast<uint8_t>(v);
		}
		out[3] = type;

		int yawInt = static_cast<int>(yaw * kRotScale + 0.5f);
		if (yawInt < 0)
			yawInt = 0;
		if (yawInt > 0x8000)
			yawInt = 0x8000;
		int pitchInt = static_cast<int>(pitch * kRotScale + 0.5f);
		if (pitchInt < 0)
			pitchInt = 0;
		if (pitchInt > 0x8000)
			pitchInt = 0x8000;
		out[4] = static_cast<uint8_t>(yawInt & 0xFF);
		out[5] = static_cast<uint8_t>((yawInt >> 8) & 0xFF);
		out[6] = static_cast<uint8_t>(pitchInt & 0xFF);
		out[7] = static_cast<uint8_t>((pitchInt >> 8) & 0xFF);

		// rotation flags spread (sub_B87140 record[8..11])
		out[8] = static_cast<uint8_t>(16 * ((rot >> 12) & 0xF));
		out[9] = static_cast<uint8_t>(16 * (rot & 0xF));
		out[10] = static_cast<uint8_t>(rot & 0xF0);
		out[11] = static_cast<uint8_t>(16 * ((rot >> 8) & 0xF));

		out[12] = static_cast<uint8_t>(flags & 0xFF);
		out[13] = static_cast<uint8_t>((flags >> 8) & 0xFF);
		out[14] = static_cast<uint8_t>((flags >> 16) & 0xFF);
		out[15] = 0;
	}

	static void appendVarint(std::string& out, uint64_t value)
	{
		while ((value >> 7) != 0)
		{
			out.push_back(static_cast<char>(static_cast<uint8_t>(value) | 0x80));
			value >>= 7;
		}
		out.push_back(static_cast<char>(static_cast<uint8_t>(value & 0x7f)));
	}

	// The client's Serializer reads i32 as zigzag-then-varint
	// (Serializer.h: value = varint(); return (value >> 1) ^ -(value & 1)).
	static void appendZigzagVarint(std::string& out, int32_t value)
	{
		const uint64_t zig = static_cast<uint64_t>((static_cast<uint32_t>(value) << 1) ^
			static_cast<uint32_t>(value >> 31));
		appendVarint(out, zig);
	}

	// The 10096 fork replaced the one-int stock S2CPacketLoginResult (id 50)
	// with an expanded packet carrying the player profile inline. The stock
	// server only sends m_resultCode, so the fork client blocks on its loading
	// screen forever (LoginSuccEvent is emitted only for resultCode == 0).
	//
	// Layout recovered byte-for-byte from the WORKING reference login
	// (newdump.pcapng frame 208, 165 bytes, no trailing DBDataReady):
	//   [00 resultCode=succ][30 marker][varint platformUserId]
	//   [varint nameLen][name]
	//   [00 00 02 1e 00 02 5f][picUrl]
	//   [00 00][3d = ChangePlayerInfo(61)][zigzag varint entityId]
	//   [varint sigLen][colour-coded signature "..FF00FF00\n..FFFFFFFFPlayer<uid>"]
	//   [53 be 9a 0c cc ca a9 e4 fd 9e d4 0a 00]
	//
	// The [3d][zigzag entityId] pair IS a real S2CPacketChangePlayerInfo: the
	// signature string is its showName and the entity id its playerId. It used
	// to be hardcoded as [3d c0 48] (playerId 4640), which is one byte longer
	// than the reference's [3d 1c] (playerId 14) and shifted the whole tail, so
	// the fork's expanded-LoginResult parser lost alignment, dropped the batch
	// and never became ready - the client sat on "Loading player data...".
	static std::string buildLoginResultBody(const char* name, uint64_t platformUserId, int32_t entityId)
	{
		// avatar URL (the fork shows it on the profile card)
		static const char kPicUrl[] =
			"https://dss0.bdstatic.com/70cFuHSh_Q1YnxGkpoWK1HF6hhy/it/"
			"u=1432121695,1948459179&fm=26&gp=0.jpg";

		// fixed prefix of the signature; "<uid>" is appended in decimal
		static const char kSigPrefix[] =
			"\xe2\x96\xa2" "FF00FF00\n"
			"\xe2\x96\xa2" "FFFFFFFF";
		// exactly what precedes the signature string
		static const unsigned char kBeforeSig[] = {
			0x00, 0x00, 0x02, 0x1e, 0x00, 0x02, 0x5f
		};
		// exactly what sits between the picUrl and the embedded
		// ChangePlayerInfo packet (the old hardcoded [3d c0 48] was wrong -
		// see the layout note above)
		static const unsigned char kMidSig[] = {
			0x00, 0x00
		};
		// fixed 13-byte trailer (server signing blob)
		static const unsigned char kTail[] = {
			0x53, 0xbe, 0x9a, 0x0c, 0xcc, 0xca, 0xa9,
			0xe4, 0xfd, 0x9e, 0xd4, 0x0a, 0x00
		};

		const char* nm = (name != nullptr) ? name : "";
		const size_t nmLen = std::strlen(nm);
		const size_t urlLen = std::strlen(kPicUrl);

		char uidBuf[32];
		std::snprintf(uidBuf, sizeof(uidBuf), "%llu",
			static_cast<unsigned long long>(platformUserId));

		std::string sig(kSigPrefix);
		sig += "Player";
		sig += uidBuf;

		std::string body;
		body.reserve(2 + 10 + nmLen + sizeof(kBeforeSig) + urlLen +
			sizeof(kMidSig) + 10 + sig.size() + sizeof(kTail));

		body.push_back(static_cast<char>(0x00)); // resultCode = success
		body.push_back(static_cast<char>(0x30)); // fork profile marker
		appendVarint(body, platformUserId);
		appendVarint(body, static_cast<uint64_t>(nmLen));
		body.append(nm, nmLen);

		body.append(reinterpret_cast<const char*>(kBeforeSig), sizeof(kBeforeSig));
		body.append(kPicUrl, urlLen);
		body.append(reinterpret_cast<const char*>(kMidSig), sizeof(kMidSig));

		// Embedded S2CPacketChangePlayerInfo(61): playerId = entity id,
		// showName = the colour-coded signature. Byte-identical to the
		// reference ([3d][zigzag entityId] == 3d 1c for entityId 14).
		body.push_back(static_cast<char>(0x3d));
		appendZigzagVarint(body, entityId);

		appendVarint(body, static_cast<uint64_t>(sig.size()));
		body.append(sig);

		body.append(reinterpret_cast<const char*>(kTail), sizeof(kTail));

		// NOTE: DBDataReady (id 348) is deliberately NOT appended here. The
		// working reference login blob is exactly 165 bytes and ends on kTail;
		// the trailing [dc 02] made our blob one packet longer than the
		// reference and desynchronised the fork's decode loop. DBDataReady
		// already reaches the client twice over: inline at offset 317 of the
		// GameInfo batch (sendWorldInfo), and as its own message from
		// ClientPeer::login() and ClientPeer::sendLoginResult().

		return body;
	}
};
#endif
