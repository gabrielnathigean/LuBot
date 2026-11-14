#include "pch.h"
#include "PacketHandler.h"
#include "../../Common/NetworkProbe.h"
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

		uint32_t ReadU32LE(const unsigned char* data, int offset)
		{
			return static_cast<uint32_t>(data[offset])
				| (static_cast<uint32_t>(data[offset + 1]) << 8)
				| (static_cast<uint32_t>(data[offset + 2]) << 16)
				| (static_cast<uint32_t>(data[offset + 3]) << 24);
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

			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				const uint32_t newCount = ++m_AttackerHitCounts[attackerId];
				if (attackerId == m_HeroId || newCount > m_AttackerHitCounts[m_HeroId])
				{
					m_HeroId = attackerId;
					m_HeroTargetId = targetId;
				}
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
				if (it != m_Npcs.end()) it->second.alive = false;
			}

			if (const auto& dispatcher = Services::ServiceLocator::GetInstance().GetEventDispatcher())
			{
				dispatcher->Dispatch(Events::CreatureDiedEvent(creatureId, {}));
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

	void PacketHandler::Reset()
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Npcs.clear();
		m_AttackerHitCounts.clear();
		m_HeroId = 0;
		m_HeroTargetId = 0;
	}
}
