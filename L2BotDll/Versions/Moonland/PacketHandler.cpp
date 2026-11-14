#include "pch.h"
#include "PacketHandler.h"
#include "../../Common/NetworkProbe.h"
#include <atomic>
#include "Domain/Events/AttackedEvent.h"
#include "Domain/Events/CreatureDiedEvent.h"
#include "Domain/Services/ServiceLocator.h"

using namespace L2Bot::Domain;

namespace Moonland
{
	namespace
	{
		// buffer layout for all three opcodes, confirmed by single-target
		// lifecycle trace in docs/en/MOONLAND_PROTOCOL_RE.md:
		//   [0..2)  2-byte LE length prefix (already consumed by the caller,
		//           kept here since every offset below is relative to the
		//           start of the buffer NetworkProbe hands us)
		//   [2]     opcode
		//   [3..7)  4-byte LE object id (attacker id for 0x33, creature id
		//           for 0x0 and 0xc)
		constexpr int kOpcodeOffset = 2;
		constexpr int kObjectIdOffset = 3;
		constexpr int kNpcInfoNameOffset = 21; // see the two 0xc samples in the doc
		constexpr int kAttackTargetIdOffset = 7;

		// Attack (0x33) layout, confirmed by the fixed 53-byte packet length
		// matching exactly: attackerId, targetId, 00000000, damage(dword),
		// hitFlag(dword), 00000005, attackerPos{X,Y,Z}, 2-byte gap,
		// targetPos{X,Y,Z}. Coordinates are signed (world origin is roughly
		// centered, per every sample seen so far having a mix of positive
		// and negative values).
		constexpr int kAttackPosOffset = 27;
		constexpr int kAttackPosGap = 2;
		constexpr int kAttackPosStride = 12; // 3 x int32

		uint32_t ReadU32LE(const unsigned char* data, int offset)
		{
			return static_cast<uint32_t>(data[offset])
				| (static_cast<uint32_t>(data[offset + 1]) << 8)
				| (static_cast<uint32_t>(data[offset + 2]) << 16)
				| (static_cast<uint32_t>(data[offset + 3]) << 24);
		}

		int32_t ReadI32LE(const unsigned char* data, int offset)
		{
			return static_cast<int32_t>(ReadU32LE(data, offset));
		}

		Position ReadPosition(const unsigned char* data, int offset)
		{
			Position p;
			p.x = static_cast<float_t>(ReadI32LE(data, offset));
			p.y = static_cast<float_t>(ReadI32LE(data, offset + 4));
			p.z = static_cast<float_t>(ReadI32LE(data, offset + 8));
			return p;
		}

		// NpcInfo's name is a NUL-terminated UTF-16LE string starting at
		// kNpcInfoNameOffset. Bounded by `len` so a short/malformed packet
		// never reads past the buffer NetworkProbe already validated.
		std::wstring ReadNpcName(const unsigned char* data, int len)
		{
			std::wstring result;
			for (int i = kNpcInfoNameOffset; i + 1 < len; i += 2)
			{
				const wchar_t ch = static_cast<wchar_t>(data[i] | (data[i + 1] << 8));
				if (ch == 0) break;
				result.push_back(ch);
			}
			return result;
		}
	}

	PacketHandler& PacketHandler::GetInstance()
	{
		static PacketHandler instance;
		return instance;
	}

	namespace
	{
		// GEngine and UGameEngine::GetMyPawn are both exported by name even
		// though the packet-handling classes this client used to export
		// are gone (see the doc).
		typedef void* (__thiscall* GetMyPawnFnType)(void*);

		// Confirmed by a bounded scan of GetMyPawn()'s own struct (logged in
		// a prior build of this file, then removed): exactly one dword in
		// the whole first 0x600 bytes has the 0x10xxxxxx shape every
		// network object id observed so far has, and it sat at this fixed
		// offset, stable across three separate reads - and that same value
		// matched a live combat packet's target id in the same session.
		// This is our own network object id, read directly instead of
		// guessed from packet frequency (which breaks down whenever some
		// other nearby entity attacks several different targets - see the
		// doc for a concrete case of that).
		constexpr int kPawnNetworkIdOffset = 0x58;

		// Resolves GetMyPawn() -> [+0x58] in one guarded read, isolated
		// from any C++ object with a destructor per the __try/__except +
		// unwinding rule used throughout this file.
		bool ReadOwnNetworkIdGuarded(void* gEngine, GetMyPawnFnType getMyPawn, uint32_t* outId)
		{
			__try
			{
				void* pawn = getMyPawn(gEngine);
				if (!pawn) return false;
				const uint32_t id = *reinterpret_cast<uint32_t*>(static_cast<unsigned char*>(pawn) + kPawnNetworkIdOffset);
				if ((id & 0xFF000000) != 0x10000000) return false; // sanity-check the shape every time, not just once
				*outId = id;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		uint32_t ResolveOwnNetworkId(HMODULE hEngine)
		{
			if (!hEngine) return 0;
			auto* gEnginePtrSlot = reinterpret_cast<void**>(GetProcAddress(hEngine, "?GEngine@@3PAVUEngine@@A"));
			auto* getMyPawn = GetProcAddress(hEngine, "?GetMyPawn@UGameEngine@@QAEPAVAPawn@@XZ");
			if (!gEnginePtrSlot || !getMyPawn) return 0;

			void* gEngine = *gEnginePtrSlot;
			if (!gEngine) return 0; // too early - hero not spawned into the world yet

			uint32_t id = 0;
			const auto fn = reinterpret_cast<GetMyPawnFnType>(getMyPawn);
			return ReadOwnNetworkIdGuarded(gEngine, fn, &id) ? id : 0;
		}

	}

	void PacketHandler::Init()
	{
		if (m_Initialized) return;
		m_Initialized = true;

		NetworkProbe::SetPacketObserver([this](const unsigned char* data, int len) {
			OnPacket(data, len);
		});
	}

	void PacketHandler::OnPacket(const unsigned char* data, int len)
	{
		// Re-resolved every ~20 packets rather than cached once: cheap (one
		// exported function call, one guarded memory read) and keeps
		// m_HeroId correct across respawn or zone change without needing
		// to hook anything for those events specifically.
		static std::atomic<int> heroIdRefreshCountdown{ 0 };
		if (heroIdRefreshCountdown.fetch_add(1) % 20 == 0)
		{
			if (const uint32_t heroId = ResolveOwnNetworkId(GetModuleHandleA("Engine.dll")))
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				m_HeroId = heroId;
			}
		}

		if (len <= kObjectIdOffset + 4) return;
		const unsigned char opcode = data[kOpcodeOffset];

		switch (opcode)
		{
		case 0x0c: // NpcInfo
		{
			const uint32_t objectId = ReadU32LE(data, kObjectIdOffset);
			if (!objectId) return;
			std::wstring name = ReadNpcName(data, len);

			std::lock_guard<std::mutex> lock(m_Mutex);
			auto& npc = m_Npcs[objectId];
			if (!name.empty()) npc.name = std::move(name);
			npc.alive = true;
			break;
		}
		case 0x33: // Attack: attackerId, targetId
		{
			if (len <= kAttackTargetIdOffset + 4) return;
			const uint32_t attackerId = ReadU32LE(data, kObjectIdOffset);
			const uint32_t targetId = ReadU32LE(data, kAttackTargetIdOffset);
			if (!attackerId || !targetId) return;

			const int targetPosOffset = kAttackPosOffset + kAttackPosStride + kAttackPosGap;
			const bool hasPositions = len >= targetPosOffset + kAttackPosStride;
			const uint32_t heroId = GetHeroId();

			if (hasPositions)
			{
				const Position attackerPos = ReadPosition(data, kAttackPosOffset);
				const Position targetPos = ReadPosition(data, targetPosOffset);

				std::lock_guard<std::mutex> lock(m_Mutex);
				if (attackerId == heroId) m_HeroPosition = attackerPos;
				else
				{
					const auto it = m_Npcs.find(attackerId);
					if (it != m_Npcs.end()) it->second.position = attackerPos;
				}

				if (targetId == heroId) m_HeroPosition = targetPos;
				else
				{
					const auto it = m_Npcs.find(targetId);
					if (it != m_Npcs.end()) it->second.position = targetPos;
				}
			}

			// m_HeroId now comes from ResolveOwnNetworkId() (read directly
			// off GetMyPawn(), not guessed from attack frequency), so this
			// only needs to notice when a target is one WE just attacked.
			if (attackerId == heroId)
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				m_HeroTargetId = targetId;
			}

			if (const auto& dispatcher = Services::ServiceLocator::GetInstance().GetEventDispatcher())
			{
				dispatcher->Dispatch(Events::AttackedEvent(attackerId, targetId));
			}
			break;
		}
		case 0x00: // Die (opcode byte 0x00, distinct from the "no packet" guard above)
		{
			const uint32_t creatureId = ReadU32LE(data, kObjectIdOffset);
			if (!creatureId) return;

			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				const auto it = m_Npcs.find(creatureId);
				if (it != m_Npcs.end())
				{
					it->second.alive = false;
					it->second.damageTaken = 0; // a respawn under the same id starts fresh
				}
			}

			if (const auto& dispatcher = Services::ServiceLocator::GetInstance().GetEventDispatcher())
			{
				dispatcher->Dispatch(Events::CreatureDiedEvent(creatureId, {}));
			}
			break;
		}
		case 0x62: // HP/CP delta: ..., targetId, attackerId, delta - always in
				   // the last 12 bytes regardless of the two header shapes
				   // seen so far (see the doc)
		{
			constexpr int kDamageDeltaTailSize = 12;
			if (len < kDamageDeltaTailSize) return;
			const int tailOffset = len - kDamageDeltaTailSize;
			const uint32_t targetId = ReadU32LE(data, tailOffset);
			const int32_t delta = ReadI32LE(data, tailOffset + 8);
			if (!targetId || delta >= 0) return; // only confirmed for damage (negative delta) so far

			std::lock_guard<std::mutex> lock(m_Mutex);
			if (targetId == m_HeroId) m_HeroDamageTaken += static_cast<uint32_t>(-delta);
			else
			{
				const auto it = m_Npcs.find(targetId);
				if (it != m_Npcs.end()) it->second.damageTaken += static_cast<uint32_t>(-delta);
			}
			break;
		}
		case 0x18:
		{
			// Hero's own current HP/MP, a third 0x18 shape distinct from
			// the target-HP ones below: targetId==heroId, len==28, a flag
			// byte of 0x03 at offset 12 (vs. 0x01/0x02 for target HP).
			// Confirmed by a two-phase capture: idle (pure regen, both
			// values climbing a fixed small amount per tick) then toggling
			// a continuous-HP-drain skill (sawtooth matching the skill's
			// tick damage exactly, and a one-packet MP dip of exactly the
			// skill's listed MP cost). No confirmed source yet for max
			// HP/MP here - a bounded and then a wide memory scan for the
			// exact max values shown on screen found neither in
			// GetMyPawn()'s struct, so the real own-CharInfo packet (0x0b,
			// which does carry our name - see the doc) likely carries them
			// instead, sent once at login; not decoded further yet.
			constexpr int kHeroVitalsLen = 28;
			constexpr int kHeroVitalsShapeOffset = 12;
			constexpr int kHeroVitalsShapeValue = 0x03;
			// Offset 13 is still unexplained (always 0x21 in every capture -
			// could be a flag or a 4th packed byte of something else).
			// Offset 14 looked like a 4-byte dword that never changed
			// (0x0005ec21 in every capture, including across separate
			// sessions), but read as a 3-byte LE value starting one byte
			// later it is exactly 1516 - current CP, and it stayed that
			// value because the hero's CP was full (1516/1516) throughout
			// every capture, never confirmed decreasing yet.
			constexpr int kHeroVitalsCpOffset = 14;
			constexpr int kHeroVitalsHpOffset = 19;
			constexpr int kHeroVitalsMpOffset = 24;
			if (len == kHeroVitalsLen && data[kHeroVitalsShapeOffset] == kHeroVitalsShapeValue)
			{
				const uint32_t ownerId = ReadU32LE(data, kObjectIdOffset);
				if (ownerId && ownerId == GetHeroId())
				{
					const uint32_t cp = static_cast<uint32_t>(data[kHeroVitalsCpOffset])
						| (static_cast<uint32_t>(data[kHeroVitalsCpOffset + 1]) << 8)
						| (static_cast<uint32_t>(data[kHeroVitalsCpOffset + 2]) << 16);

					std::lock_guard<std::mutex> lock(m_Mutex);
					m_HeroCurrentCp = cp;
					m_HeroCurrentHp = ReadU32LE(data, kHeroVitalsHpOffset);
					m_HeroCurrentMp = ReadU32LE(data, kHeroVitalsMpOffset);
				}
				break;
			}

			// Target HP status: targetId, then either (current,max) the
			// first time this id is targeted (byte[12]==0x02) or just
			// current on every later refresh (byte[12]==0x01). Confirmed
			// by matching the exact value in the game's own target-HP
			// display, oscillating with the target's own regen between
			// hits exactly as shown on screen.
			constexpr int kTargetHpShapeOffset = 12;
			constexpr int kTargetHpCurrentOffset = 14;
			constexpr int kTargetHpMaxOffsetFromCurrent = 5; // one flag byte, then max
			if (len <= kTargetHpCurrentOffset + 4) return;

			const uint32_t targetId = ReadU32LE(data, kObjectIdOffset);
			if (!targetId) return;
			const uint32_t currentHp = ReadU32LE(data, kTargetHpCurrentOffset);
			const bool hasMax = data[kTargetHpShapeOffset] == 0x02
				&& len >= kTargetHpCurrentOffset + kTargetHpMaxOffsetFromCurrent + 4;

			std::lock_guard<std::mutex> lock(m_Mutex);
			const auto it = m_Npcs.find(targetId);
			if (it != m_Npcs.end())
			{
				it->second.currentHp = currentHp;
				if (hasMax) it->second.maxHp = ReadU32LE(data, kTargetHpCurrentOffset + kTargetHpMaxOffsetFromCurrent);
			}
			break;
		}
		default:
			break;
		}
	}

	std::unordered_map<uint32_t, TrackedNpc> PacketHandler::GetNpcSnapshot() const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_Npcs;
	}

	uint32_t PacketHandler::GetHeroId() const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_HeroId;
	}

	uint32_t PacketHandler::GetHeroTargetId() const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_HeroTargetId;
	}

	uint32_t PacketHandler::GetHeroDamageTaken() const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_HeroDamageTaken;
	}

	Position PacketHandler::GetHeroPosition() const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_HeroPosition;
	}

	uint32_t PacketHandler::GetHeroCurrentHp() const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_HeroCurrentHp;
	}

	uint32_t PacketHandler::GetHeroCurrentMp() const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_HeroCurrentMp;
	}

	uint32_t PacketHandler::GetHeroCurrentCp() const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_HeroCurrentCp;
	}

	void PacketHandler::Reset()
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Npcs.clear();
		m_HeroId = 0;
		m_HeroTargetId = 0;
		m_HeroPosition = Position();
		m_HeroDamageTaken = 0;
		m_HeroCurrentHp = 0;
		m_HeroCurrentMp = 0;
		m_HeroCurrentCp = 0;
	}
}
