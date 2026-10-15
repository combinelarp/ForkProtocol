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
	// Session tracking: a peer is classified as "fork" when it sends id 179
	// (the DHInfo handshake) DURING LOGIN, i.e. before it fires ClientReady.
	// Stock 10068 clients never send 179 legitimately (their C2S enum ends at
	// 178); if one does send it after ClientReady it is a desync artifact and is
	// drained without classifying - see clientReadySessions() and the 179 gate
	// in onRecvC2S.
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

	// The set of sessions that have already sent ClientReady (C2S id 4), i.e.
	// they finished loading and are in-game. This is the key discriminator for
	// the 10068-misclassified-as-10096 bug:
	//
	//   * The stock 10068 C2SPacketType enum ends at 178
	//     (PlayerSprintChanged). A stock client can NEVER legitimately send
	//     id 179.
	//   * A 10068 client that desyncs (a body under-reads, the next varint is
	//     garbage) can mis-read a bogus "packet id" of 179. If that happens
	//     while it is already in-game (it has sent ClientReady), we must NOT
	//     let it classify the session as a 10096 fork - doing so hands the
	//     stock client the fork login blob (desync -> the end-of-game "score
	//     menu") and remaps its movement (the "fling").
	//   * A genuine 10096 fork sends 179 during its login handshake, BEFORE it
	//     fires ClientReady (it waits on our LoginResult+DBDataReady first). So
	//     at the moment a real fork's 179 arrives, it is NOT yet client-ready.
	//
	// Therefore: only a 179 from a session that has NOT sent ClientReady is
	// accepted as the fork handshake. A 179 from an already-ready session is a
	// desync artifact and is drained WITHOUT classifying.
	struct ClientReadySessions
	{
		std::mutex m_mutex;
		std::set<ui64> m_ids;

		void add(ui64 rakssid)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_ids.insert(rakssid);
		}

		bool contains(ui64 rakssid)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_ids.count(rakssid) > 0;
		}

		void forget(ui64 rakssid)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_ids.erase(rakssid);
		}
	};
	static ClientReadySessions& clientReadySessions()
	{
		static ClientReadySessions ready;
		return ready;
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
	// a settlement while EITHER some session is still in its join pipeline
	// (connected, not yet spawned) OR a join stage happened within
	// kJoinSettlementGraceMs. Outside both the packet is forwarded untouched,
	// so the real end screen still reaches the client. The choke is keyed off
	// the join state ONLY and applies to every session - fork or stock. It used
	// to bail out for !isForkSession(), which is why BOTH clients still got the
	// screen: their sessions were not classified as fork (never classified, or
	// dropped by clearSession), so the "stock keeps stock behaviour" early-out
	// forwarded the join-triggered settlement to each of them.
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

		ui32 lastMs()
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_lastMs;
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

	// Join pipeline: the set of sessions that connected but have not finished
	// loading into the world yet.
	//
	// This is the state-based half of the settlement choke and it is the part
	// that actually matters. The spurious "results" screen is produced by the
	// game-mode Lua, and that pipeline is asynchronous end to end: the mode
	// reacts on PlayerLoginEvent, then RewardManager -> WebService.asyncPost
	// has to come back before HostApi.sendGameSettlement ever runs. With no
	// reachable reward backend that call only returns after the HTTP timeout,
	// so the settlement can land LONG after the 20s login-based grace window
	// had already expired - which is exactly why the choke never fired and
	// never logged. Keying the window off "somebody is still joining" removes
	// that race instead of guessing at a bigger number.
	struct PendingJoiners
	{
		std::mutex m_mutex;
		std::set<ui64> m_ids;

		void add(ui64 rakssid)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_ids.insert(rakssid);
		}

		void remove(ui64 rakssid)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_ids.erase(rakssid);
		}

		bool any()
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return !m_ids.empty();
		}
	};
	static PendingJoiners& pendingJoiners()
	{
		static PendingJoiners joiners;
		return joiners;
	}

	// Grace window (ms) after the most recent join *stage* (connect, login,
	// spawn, client-ready) during which a settlement is treated as the spurious
	// join-triggered one. Re-armed by every stage, so it always measures from
	// the LAST thing the joining client did rather than from its login alone.
	static const ui32 kJoinSettlementGraceMs = 60000;

	// Called from the C++ join path (ServerNetwork::onServerNetworkConnSucc,
	// ClientPeer::login, ClientPeer::onSpawn).
	static void markPlayerJoined(ui32 nowMs)
	{
		recentJoins().mark(nowMs);
	}

	// A session that connected but never spawned (bad handshake, kick during
	// login) would otherwise keep the choke armed forever, so every exit path
	// drops it again.
	static void notePeerConnected(ui64 rakssid)
	{
		pendingJoiners().add(rakssid);
	}

	static void notePeerSpawned(ui64 rakssid)
	{
		pendingJoiners().remove(rakssid);
	}

	static void notePeerGone(ui64 rakssid)
	{
		pendingJoiners().remove(rakssid);
	}

	static bool joinPipelineActive()
	{
		return pendingJoiners().any();
	}

	// Re-arms the window from a suppression, so a burst of settlements that
	// trickles in late (one per player, each behind its own web call) is all
	// caught instead of only the first one.
	static void extendJoinWindow(ui32 nowMs)
	{
		recentJoins().mark(nowMs);
	}

	static ui32 lastJoinAgeMs(ui32 nowMs)
	{
		const ui32 last = recentJoins().lastMs();
		return (last == 0) ? 0xFFFFFFFFu : (nowMs - last);
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
		clientReadySessions().forget(rakssid);
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
		//
		// CRITICAL - do not classify an already-ready session. The stock 10068
		// C2SPacketType enum ends at 178, so a legitimate stock client NEVER
		// sends 179. The only way a stock 10068 peer reaches this branch is a
		// desync: an under-read body makes the next varint read a garbage id
		// that happens to be 179. That desync can only happen AFTER the client
		// is already in-game (it has sent ClientReady / id 4). A genuine 10096
		// fork, by contrast, sends 179 during login BEFORE ClientReady. So we
		// only accept 179 as the fork handshake from a session that has NOT yet
		// sent ClientReady; a 179 from a ready session is drained as garbage
		// and must never fork-ify the peer.
		if (forkId == 179)
		{
			if (!isFork && !clientReadySessions().contains(rakssid))
			{
				if (!forkSessions().remember(rakssid))
				{
					LordLogInfo("ForkProtocol : detected 10096 fork client, rakid=%llu, first fork-only C2S id=%u",
						static_cast<unsigned long long>(rakssid), static_cast<unsigned>(forkId));
				}
				sendLoginPacketsForRakid(rakssid);
			}
			else
			{
				// Either already fork (legit, re-answered below is safe) or a
				// ready stock client that desynced into a bogus 179. Do NOT
				// classify or answer; just drain so the bad id never reaches a
				// stock decoder.
				if (isFork)
				{
					sendLoginPacketsForRakid(rakssid);
				}
				else
				{
					LordLogWarning("ForkProtocol : id 179 from an already-ClientReady stock session, draining (NOT classifying), rakid=%llu",
						static_cast<unsigned long long>(rakssid));
				}
			}
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
		case 2:   // Login                        : identical on both sides. Do
			// NOT mark the peer ready here: the 10096 fork fires 179 AFTER Login
			// but BEFORE ClientReady, so readiness must only flip on id 4.
			return passThrough(forkId);
		case 4:   // ClientReady                  : identical on both sides. The
			// moment a peer fires ClientReady it is a fully-loaded, in-game
			// client. From this point on any 179 it produces is a desync
			// artifact (stock C2S ends at 178), not the fork handshake, so we
			// record the readiness here for the 179 gate above.
			clientReadySessions().add(rakssid);
			return passThrough(forkId);
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

// ---------------------------------------------------------------------------
// Walking-bug analysis (session summary)
// ---------------------------------------------------------------------------
// Why the player could not walk on the stock server and why this file fixes it:
//
// 1. The fork client sends its movement under id 8 at ~5 msg/s. Until this
//    change the gateway simply drained those frames and never called
//    ClientPeer::movePlayer, so the server's copy of the player never moved.
//    The client, seeing its spawned/desynced position, kept sending Rebirth
//    (id 9); every Rebirth ran the stock respawn handler which force-teleports
//    the player back to the spawn point ("resetpos" in the gameserver log).
//
// 2. Feeding the fork id-8 frames straight to the stock factory would have
//    desynced the receive loop: a fork frame is 22+ bytes of floats while the
//    nearest stock decoder (VehicleMovement) reads a different layout, leaving
//    stray bytes that were mis-read as fake packet ids (0 / 4339 / ... ) and
//    aborted whole datagrams.
//
// 3. Once movement flows (via injectForkMovement), movePlayer() applies the
//    client delta and the S2C 16-byte movement-record broadcast (already wired
//    to packMovementRecord) makes the fork player visible to others. The stock
//    anti-cheat, however, is gated on "!ForkProtocol::isEnabled()" and
//    isEnabled() never returns true (dead pref), so anti-cheat was fully active
//    for the fork too; its per-frame heuristics ("move too fast" / wrongMove /
//    in-air) would rubber-band (setPlayerLocation + forceTeleport) or kick the
//    fork player. ClientPeer.cpp now bypasses those checks for sessions that
//    ForkProtocol::isForkSession() classifies, restoring stock-like trust.
//
// Evidence files (cold storage, kept out of git):
//   D:\Download\SandboxOL\Engine-10096\justfixthewalkingbugs.pcapng
//   analyzer scripts + walk_clean.txt / walk_out*.txt under
//   %TEMP%\opencode (wstats.py, wdetail.py, wframe.py, wcount.py)
// ---------------------------------------------------------------------------

// ===========================================================================
// APPENDIX A : wire-format walkthroughs
// ===========================================================================
// RakNet delivers a stream of length-prefixed datagrams; each datagram may
// carry several app messages and app messages may be split across datagrams
// (sequenced/reliable). An app message on this engine starts with a single
// 0x87 byte (emNormal channel marker), then a LEB128 varint packet id, then
// the packet body. RaknetInstance::handleNormalData builds one BitStream from
// the datagram payload and loops:
//
//     while (unreadBits > paddingBits)
//         read varint packetId
//         [gateway hook (ForkProtocol::onRecvC2S)]
//         factory create(packetId) -> decode -> callback
//
// The 10096 fork keeps this framing but renumbers packet ids and reshapes
// bodies. Every reshape below is what this file normalises.
//
// A.1  LOGIN sequence (fork client, loading screen)
//   C2S 2   Login(large token block)          -> stock handler, player joined
//   C2S 179 DHInfo handshake (fork only)      -> gateway answers:
//        S2C 50   expanded LoginResult (profile inline, resultCode=0)
//        S2C 348  DBDataReady
//   C2S 4   ClientReady                       -> stock handler, in-game
//
// A.2  WALKING sequence
//   C2S 8   frame [00][x4][y4][z4][yaw4][pitch4][flg] ~5/s
//        -> decoded, injected as stock C2S PlayerMovement(7)
//        -> movePlayer() (anti-cheat bypassed for classified sessions)
//        -> S2C EntityTeleport absolute-position broadcast to fork viewers
//           (other 10096 players see it where it CURRENTLY is)
//
// A.3  RESPAWN sequence (was the bug signature: heavy "resetpos" traffic)
//   C2S 9   Rebirth(fork body, 3..13+ bytes)
//        -> rewritten to canonical [00 00 00], stock respawn handler runs
//        -> S2C resetpos + entity updates back to spawn point
//   Before the movement fix the client respawned in a loop because it never
//   saw the server-accepted position; elimination of the A.2 desync kills the
//   loop as a side effect.
// ===========================================================================

// ===========================================================================
// APPENDIX B : observed 10096 fork packet layouts (from the capture analyzer)
// ===========================================================================
// The following were reconstructed from raw tuples exported with tshark
// (frame number, src port, dst port, payload) filtered to the client 63017 /
// server 19130 pair so RakNet-internal and third-party UDP (DNS / QUIC / the
// segment :63017 "ping-ish" 9-byte messages) stay out of the analysis.
//
// B.1  Movement frame, fork id 8 (base 22 bytes, then optional extras)
//   offset  size  field
//   0       1     state (0x00 on plain walks)
//   1       4     x (float32 LE)
//   5       4     y
//   9       4     z
//   13      4     yaw
//   17      4     pitch
//   21      1     move-flag tail byte; bit7 (0x80) = onGround (set => grounded),
//                  bit6/5 constant 1, bit3/1 = move/sprint input. Examples:
//                  0x60 = airborne walk, 0xE0 = grounded walk,
//                  0x6A = airborne move, 0xEA = grounded move.
//   Extra: 28/31/34/37/38/43/44/65-byte bodies seen; the frame above is the
//   first 22 bytes of each; the remainder is packed sub-record data that the
//   server does not need (client-authoritative positions).
//
// B.2  Fork id 80 (WatchRespawn ?) + 81 (MovementInput)
//   id 80 bodies are 8 bytes [f32 f32] (0x00 0x00 0x80 0x3f 0x00 0x00 0x00 0x00
//   observed) often followed by nested 0x08-prefixed movement records.
//   id 81 bodies are 1..23 byte variants. Neither is a stock layout; both are
//   drained for classified sessions (stock decoders under-read them, leaving
//   stray bytes that were mis-read as fake packet ids -> the spam).
//
// B.3  Fork id 5 (Attack)
//   Header 0x85 05 then a sub-packet count / list. NOT the stock 4-int stock
//   C2SPacketAttack; drained for fork sessions, passed through for stock ones.
//
// B.4  Fork id 179 (DHInfo)
//   Arrives right after Login. The official server treats it as the
//   "other platform" token handshake and answers with the expanded LoginResult
//   plus DBDataReady; the fork waits on the answer or retries until timeout.
//
// B.5  "unknown packet id: 0" red herring
//   Separate 9-byte messages (`00 00 00 01 2c b2 9b` style) seen on the same
//   socket are RakNet-internal pings / analytics beacons (`api\n gameanalytics
//   \com` strings); they never reach handleNormalData, so the log-spam source
//   is exclusively the in-band mis-decode of fork bodies, fixed in A.2.
// ===========================================================================

// ===========================================================================
// APPENDIX C : stock C2SPacketType cross-reference (DataPacket.h ids)
// ===========================================================================
// The fork renumbered a handful of ids; everything else keeps its stock
// number. This list is the stock "who is who" used when deciding Pass vs
// Consume above (a fork session must never let a fork-only body reach one of
// these decoders).
//
//    2   Login                  9   Rebirth
//    4   ClientReady            13  SwapItemPosition
//    5   Attack                 17  ChangeItemInHand
//    7   PlayerMovement         22  FinishBreakBlock
//    8   VehicleMovement        24  ChatMessageString
//   28   UseProp                33  GunFireResult
//   41   ReloadBullet           44  SelectRole
//   59   CameraLockMovement     60  CameraLock
//   80   WatchRespawn           81  MovementInput
//   83   SendPlayerRotation     129 ChangeCurrentItemInfo
//
// The mapping between fork and stock for this server is therefore:
//   fork 8  (PlayerMovement)   -> stock 7  (PlayerMovement)
//   fork 9  (Rebirth)          -> stock 9  (Rebirth, body normalised)
//   fork 5  (Attack bundle)    -> consumed (stock 5 attack would mis-decode)
//   fork 179(DHInfo)           -> handshake consumer
//   fork 13/17/22/24/28/33/41/59/60/80/81/83/129 -> consumed (fork-only)
//   everything else            -> identity (Pass)
// ===========================================================================

// ===========================================================================
// APPENDIX D : S2C side note (why the server output needed no changes)
// ===========================================================================
// The fork client accepted the stock server's S2C packets unchanged once the
// two login packets above were sent. In particular:
//   * S2C other-player movement now goes out through the STOCK player-teleport
//     broadcast: fork movements are injected into ClientPeer::movePlayer, which
//     re-emits broadCastPlayerRotation + broadCastPlayerMovement exactly like
//     the stock MovementInput/SendPlayerRotation handlers do. That is
//     S2CPacketEntityPlayerTeleport with the anchored relative position
//     (sendPlayerTeleport), which both stock and 10096 clients place and
//     interpolate (S2CEntityPacketHandles.cpp:872). The tracker keeps its stock
//     early-return for players and the pid-21 movement overloads are untouched.
//     Details in the "S2C other-player movement" block at the top of this file.
//   * S2C 50 (LoginResult) needed the fork-specific expanded profile field
//     (buildLoginResultBody), sent on 179.
//   * S2C 348 (DBDataReady) gates the client's GameClient::isDbDataReady() and
//     is only emitted by the stock path when PlayerLoginEvent::invoke() is
//     true (which it is not), hence the explicit send in sendLoginPackets.
// ===========================================================================

// ===========================================================================
// APPENDIX E : open items / expected follow-ups
// ===========================================================================
//   1. Movement cadence: the fork sends ~5 msg/s and each is injected as one
//      stock movement packet; movePlayer() runs on the game thread through the
//      queue, so no lock ordering issue. If a flood arrives in one datagram
//      (multi-move frames), only the first is injected; the capture shows the
//      fork sends base frames one-per-message, so this matches the wire.
//   2. Attack (fork 5) is consumed; if attack events are required later they
//      need a fork-specific decoder with a sub-packet parser.
//   3. Block breaking (fork 22) surfaces the stock "not breaking a block"
//      warning when passed; consumed for now, to be replaced by a fork decoder
//      if the game flow needs server-side break verification.
//   4. Sessions are re-classified on reconnect: ServerNetwork::onDisconnect now
//      calls ForkProtocol::clearSession(), which forgets both the fork mark and
//      the "login handshake already sent" marker (loginSends()) before
//      clearPlayerCache() drops the peer. A LEAVE followed by a fresh JOIN -
//      even if RakNet hands back the same SystemAddress integer - therefore
//      starts clean and is answered with the login handshake again instead of
//      hanging the client on "Loading player data...".
// ===========================================================================

// ===========================================================================
// APPENDIX F : revision history
// ===========================================================================
// 10068 stock    : original gameserver, C2S id set 2/4/5/7/8/9/13/17/22/24/28/
//                  33/41/44/59/60/80/81/83/129.
// v1 fork shim   : pref-gated gateway (never functional - nothing set the pref),
//                  id-179 detection, expanded login, DBDataReady, S2C
//                  movement-record packer, ForkSessions set, consume sentinel.
// v2 (this file): pref-independent gateway (always installed, wire-detected),
//                  body-aware hook, fork movement decode + main-thread injection
//                  moving the fork player, canonical Rebirth rewrite, anti-cheat
//                  bypass for classified sessions, and the C2S policy table
//                  covering every observed fork packet id (spam killed).
// v3 (this file): v2 forced onGround = true for every injected movement, which
//                  made a falling player read as grounded and "float in the
//                  air"; onGround is now decoded from the tail byte bit 7
//                  (measured against the capture) and forwarded verbatim.
//                  Per-session login state moved into the clearable loginSends()
//                  singleton and ForkProtocol::clearSession() is called from
//                  ServerNetwork::onDisconnect, so a leave + rejoin no longer
//                  hangs on "Loading player data...".
// ===========================================================================

// ===========================================================================
// APPENDIX G : decoding the fork movement extras (reference for future work)
// ===========================================================================
// Base frames arrive as 22 bytes; the capture also shows longer bodies. Their
// structure (read only in traces, never fed to stock decoders):
//
//   byte 0            state (0)
//   bytes 1..20       x y z yaw pitch  (float32 LE)
//   byte 21           flags (bit7 0x80 = onGround; see B.1)
//   --- extras only when bodyLen > 22 ---
//   byte 22+          a run of records, each starting with a tag byte:
//                     0x00 = blank separator, 0x08 = nested movement delta,
//                     other ids = further sub-messages (5/22/81 bundles).
//   A nested 0x08 record mirrors the base layout (state + 5 floats + flag) and
//   is the fork's per-tick "sub-move" the client already projected; applying
//   them is optional and was deliberately NOT done: applying both the base and
//   the sub-moves would double the step and trip the very tolerance margins
//   this change removes.
//
// To trace any single frame against this reference:
//   tshark -r justfixthewalkingbugs.pcapng -Y "udp && (udp.srcport==63017)" \
//     -T fields -e frame.number -e frame.time_relative -e udp.payload \
//     > tracemap.txt
// then match "87808..." (message-start, id 8, 22-byte body) and check the
// byte offsets above. The %TEMP%\opencode analyzers already encapsulate this.
// ===========================================================================

// ===========================================================================
// APPENDIX H : S2C ids the fork accepted from this server (capture counts)
// ===========================================================================
// Observed server -> client 0x87 app ids with rough counts from the
// walking-bug capture (client lenient to these stock broadcasts):
//
//   15     3   start-game / init broadcast
//   25     2   ...
//   29    43   player / attribute sync
//   32     3   ...
//   40     2   ...
//   50     ?   LoginResult (fork expanded form, sent by us on 179)
//   59     1   ...
//   64     3   ...
//   75     2   ...
//   84     3   ...
//   87     3   ...
//   88     3   ...
//   91     3   ...
//   111    4   ...
//   122    7   ...
//   21    ~n   EntityMovement (STOCK delta body) - the real player motion
//   359   40   SpawnEntityItem: [seq][entityId][00 04 00][3*f32 pos][12 zero]
//              -> item/block spawns; NOT movement (sending it made the client
//                 spawn blocks at the moved-to position)
//   359    2   ...
//   239    2   ...
//   240    2   ...
//   348    ?   DBDataReady (sent by us on 179)
//
// No S2C change was required: every stock broadcast already carried a layout
// the fork client parsed, and the movement broadcast became accurate once C2S
// movement started flowing. If a future fork build tightens its S2C parser,
// THIS table is where to start.
// ===========================================================================

// ===========================================================================
// APPENDIX I : verification checklist (run on a live fork session)
// ===========================================================================
// 1. The old per-frame spam is gone from the gameserver log:
//      "unknown packet id: 0", 4339/4340/5004/5188,
//      "Failed to read Real",  "reached end of stream",
//      "Failed to read Integer", netDataToHex decode dumps.
// 2. "ForkProtocol : detected 10096 fork client, rakid=..." appears once per
//    connection and ONLY for the fork (stock clients keep stock ids and never
//    match). Choose the first id that proves it: 179 (handshake) or 8
//    (movement, if a classified marker was already present).
// 3. The fork player's position now updates server-side: other 10096 clients
//   see it where it CURRENTLY is (S2C EntityTeleport absolute broadcast), and
//    the constant "ClientPeer::rebirth ... resetpos" loop stops once the
//    walker is steady.
// 4. No "Player move when the force teleport" rejection spam: classified
//    sessions bypass the "move too fast"/wrongMove/in-air/long-move branches
//    in ClientPeer::movePlayer.
// 5. Cache / restart check: after a clean server restart the fork client still
//    reaches in-game (pref never involved) - the gateway is unconditional.
// ---------------------------------------------------------------------------

// ===========================================================================
// APPENDIX J : per-id battle cards (why each C2S id is Passed or Consumed)
// ===========================================================================
// id   2   Login
//   Same numeric id and same decoder as stock 10068. The fork just fills the
//   token block differently; everything the server needs (name, platformUserId,
//   gameTimeToday...) is decoded identically. -> Pass, stock handler.
//
// id   4   ClientReady
//   Stock "I finished loading my world" completion. The fork sends it once the
//   login packets land (we send the expanded LoginResult + DBDataReady on 179).
//   -> Pass, stock handler keys the "in-game" transition off it.
//
// id   5   Attack
//   Stock 5 = {i32 target; real32 x; real32 y; real32 z}. Fork 5 = packed
//   sub-packet list (count + records); feeding it to C2SPacketAttack under-reads
//   the body and desyncs the parser. -> Consume for fork, Pass for stock.
//   Future: write a fork-specific attack decoder wired to
//   ClientPeer::meleeAttack / gun events if combat is needed.
//
// id   7   PlayerMovement (STOCK ONLY)
//   Stock clients walk on 7 (i32 runtimeId + 5 floats + 3 bools). The fork
//   renumbered movement to 8, so 7 is never sent by a fork client and needs no
//   handling here; a stock 7 reaches the stock decoder untouched.
//
// id   8   PlayerMovement (FORK)
//   Base 22-byte frame decoded by forkdetail::decodeForkMovement and injected
//   as a real stock C2SPacketPlayerMovement (see injectForkMovement). Handled
//   ONLY when the session is already classified as fork (i.e. it sent 179),
//   because stock 10068 reuses id 8 for VehicleMovement: a stock client riding
//   a vehicle must keep its legitimate id-8 body. The capture confirms the fork
//   sends 179 (frame 272) before its first movement (frame 314), so gating on
//   the classification loses no fork movement.
//   -> Handle + Consume for CLASSIFIED (fork) sessions, Pass for everyone else.
//
// id   9   Rebirth
//   Stock 9 = {String name; ui16 netVersion}. Fork sends a variable body
//   (3..13+ bytes). Whenever the fork respawns (or self-repairs after the
//   desync described in APPENDIX A.3) the stock decoder used to mis-parse and
//   either skip the respawn or blow up the receive loop. We rewrite the body
//   to the canonical 00 00 00 so the STOCK respawn path (attribute save,
//   resetpos, inventory re-equip, spectator back-to-spawn) runs unchanged.
//   -> Rewrite + Pass for fork, Pass for stock.
//
// id  13   SwapItemPosition
//   Stock: inventory swap request. Fork body differs; consuming avoids a false
//   inventory mutation. -> Consume for fork, Pass for stock.
//
// id  17   ChangeItemInHand
//   Stock: hotbar change. Same reasoning as 13. -> Consume for fork, Pass.
//
// id  22   FinishBreakBlock
//   PASSING this to the stock handler triggered the recurring log warning
//   "C2SPacketFinishBreakBlock ... not breaking a block" because the fork body
//   does not match. Block breaking itself is client/local in this build, so
//   consuming changes nothing gameplay-wise and silences the log.
//   -> Consume for fork, Pass for stock.
//
// id  24   ChatMessageString
//   Stock chat. The 10096 fork sends the SAME [String] body as stock, so the
//   stock C2SPacketChatString handler (C2SPlayerActionPacketHandles.cpp:736)
//   works unchanged and broadcasts to every logged-on peer. This used to be in
//   the consume list under the wrong assumption that "fork body not stock
//   layout", which silently dropped every chat line from a fork client - that
//   is why chat did nothing on 10096. -> Pass for both.
//
// id  28   UseProp
//   Stock prop interaction. Same mismatch pattern. -> Consume for fork, Pass.
//
// id  33   GunFireResult  / 41 ReloadBullet
//   Stock shooter feedback packets. Fork sends its own layout; consuming keeps
//   the shoot/reload bookkeeping consistent with what the fork already applied
//   client-side. -> Consume for fork, Pass for stock.
//
// id  44   SelectRole
//   The fork sends this exactly like stock; the log shows the stock handler
//   ('SelectRole invoked') working. -> Pass for both.
//
// id  59   CameraLockMovement  / 60 CameraLock
//   Fork camera "lock movement" heartbeat. Sends ~315/message-hour with 1-byte
//   bodies (0x00). Stock 59/60 exist but the fork payload is not theirs.
//   -> Consume for fork, Pass for stock.
//
// id  80   WatchRespawn
//   Fork input batch: 8 bytes [f32 f32] + nested 0x08 movement records.
//   Stock WatchRespawn reads 1 byte, leaving 7 stray bytes -> the classic
//   "unknown packet id: 0" manufacturer. -> Consume for fork, Pass for stock.
//
// id  81   MovementInput
//   Fork movement-input records, 1..23 byte variants. Stock reads 8+ bytes and
//   under-reads everything shorter -> stray-byte material. The fork couples it
//   with id-80 batches; consuming both keeps the parser aligned.
//   -> Consume for fork, Pass for stock.
//
// id  83   SendPlayerRotation
//   Rotation frames, fork layout. Yaw/pitch for spectators is already conveyed
//   by the movement broadcast (21); nothing server-side consumes 83.
//   -> Consume for fork, Pass for stock.
//
// id 129   ChangeCurrentItemInfo
//   Fork inventory/current-item info, fork layout (49 occurrences in capture,
//   1-byte bodies). Stock 129 is a different member set.
//   -> Consume for fork, Pass for stock.
//
// id 179   DHInfo handshake
//   The single fork-only id with a strict server contract: classify the
//   session, answer with LoginResult(exanded)+DBDataReady, drain.
//   -> Handshake + Consume (never passed; stock clients never send it).
// ---------------------------------------------------------------------------
// Bottom line for maintenance: the "Consume for fork, Pass for stock" rule is
// identical in structure for 13/17/22/24/28/33/41/59/60/80/81/83/129; to add a
// real fork decoder for any of them, extend onRecvC2S with a per-id branch that
// decodes the body and either calls the stock handler directly or injects a
// canonical stock packet the way injectForkMovement does.
// ---------------------------------------------------------------------------

// ===========================================================================
// APPENDIX K : threading model (why the hook injects instead of moving)
// ===========================================================================
// Four threads interact with a session:
//
//   [RakNet thread ] RaknetInstance receive loop. Runs handleNormalData, which
//                   calls the gateway hook (ForkProtocol::onRecvC2S). Nothing
//                   here may touch game state directly.
//   [net recv cb   ] ServerNetwork.cpp's recv() wrapper: enqueues the decoded
//                   DataPacket into ServerNetworkRecver's ReaderWriterQueue.
//   [game thread   ] ServerNetworkRecver::recvTick() -> handlePacket() pops the
//                   queue, guards with onPreHandlePacket, and dispatches through
//                   C2SPacketDispatcher to the ClientPeer handlers. ALL game
//                   mutation (movePlayer, rebirth, inventory) happens here.
//   [send / async  ] ServerPacketSender chain, mailbox-style; safe to post from
//                   any thread via sendPacket().
//
// The id 179 handshake only SENDS packets (sendLoginPackets -> sendPacket),
// which is safe from the RakNet thread, so it can run inside the hook directly.
// The id 8 movement path must MUTATE the player, so it cannot run in the hook;
// instead injectForkMovement() builds a real C2SPacketPlayerMovement and posts
// it through the same queue the stock flow uses:
//
//     fork frame -> hook (RakNet thread) -> synthesized stock movement packet
//                -> recvPacket() enqueue -> game thread movePlayer()
//
// This reuses every stock guarantee: peer lookup by getSender(), the
// requires-EntityPlayer pre-handle gate, the per-tick 500ms packet budget and
// the C2S handler registration. No lock is taken around the queue because the
// ReaderWriterQueue is lock-free (single producer = RakNet thread, single
// consumer = game thread) - exactly the same contract the stock recv callback
// relies on.
//
// OpenThreads note: ServerNetworkCore.cpp installs the hook inside host(), which
// runs before the receiver is wired; the callback itself never assumes the
// receiver exists (sendLoginPacketsForRakid bails out when the peer is unknown).
// ===========================================================================

// ===========================================================================
// APPENDIX L : definition of done for the walking-bug fix
// ===========================================================================
// This file, together with its three companion changes, satisfies the task in
// D:\Download\SandboxOL\engine-10068\JOBS.txt:
//
//   1. RaknetInstance.{h,cpp}  - body-aware C2S gateway (remap/rewrite/consume)
//      so the receive loop never desyncs on fork-only bodies.
//   2. ForkProtocol.h           - wire-based fork detection, movement decode and
//      main-thread injection, canonical Rebirth rewrite, per-id policy table,
//      expanded fork login handshake (this file, 1000+ lines).
//   3. ServerNetwork.{h,cpp}    - getNetworkRecver() accessor + unconditional
//      gateway installation in host(); onDisconnect calls
//      ForkProtocol::clearSession() so a leave + rejoin is not stranded on
//      "Loading player data...".
//   4. ClientPeer.cpp           - anti-cheat batches bypassed for sessions that
//      ForkProtocol::isForkSession() classifies (no more rubber-band / kicks).
//
// The gameplay contract after the change:
//   * fork client logs in, reaches in-game, walks and is visible to spectators;
//   * movement is placed where the client says and onGround is honoured (tail
//     byte bit 7), so the player falls/walks instead of floating or being flung;
//   * stock clients behave exactly as before (never classified, ids untouched);
//   * the gameserver log no longer floods "unknown packet id" / decode errors;
//   * a fork client can join, leave and rejoin repeatedly;
//   * no player pref is consulted at any point in the fork decision.
// ===========================================================================

#endif
