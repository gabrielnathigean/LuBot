#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

// Real, per-opcode parser for Moonland's custom protocol (see
// docs/en/MOONLAND_PROTOCOL_RE.md in the repo root for how these three
// opcodes were confirmed). Subscribes to NetworkProbe's packet observer -
// does not install its own hook, so it can never conflict with or duplicate
// NetworkProbe's vtable+0x7c patch.
//
// Deliberately minimal: only the three opcodes confirmed by live-session
// correlation are parsed. Position is not read from NpcInfo yet (the
// candidate coordinate-shaped dwords after the name did not correlate
// cleanly across samples - see the doc); every tracked NPC reports position
// (0,0,0) until that is nailed down. This still gives the bot core real,
// live creature identity and combat/death state instead of nothing.
namespace Moonland
{
	struct TrackedNpc
	{
		std::wstring name;
		bool alive = true;
	};

	class PacketHandler
	{
	public:
		static PacketHandler& GetInstance();

		// Registers with NetworkProbe::SetPacketObserver(). Safe to call more
		// than once; only the first call takes effect.
		void Init();

		// Snapshot for NPCRepository::GetEntities(). Copies under lock so the
		// repository can iterate freely without touching the hook's mutex.
		std::unordered_map<uint32_t, TrackedNpc> GetNpcSnapshot() const;

		// There is no confirmed opcode for our own CharInfo/UserInfo yet (see
		// the doc), so the hero's object id can't be read directly off a
		// packet. Instead: every 0x33 (Attack) we see names an attacker id,
		// and the hero attacks far more often than any single NPC does, so
		// the most frequent attacker id converges on the hero's real id
		// within a few hits. Returns 0 until that happens.
		uint32_t GetHeroId() const;
		uint32_t GetHeroTargetId() const;

		void Reset();

	private:
		PacketHandler() = default;
		void OnPacket(const unsigned char* data, int len);

		mutable std::mutex m_Mutex;
		std::unordered_map<uint32_t, TrackedNpc> m_Npcs;
		std::unordered_map<uint32_t, uint32_t> m_AttackerHitCounts;
		uint32_t m_HeroId = 0;
		uint32_t m_HeroTargetId = 0;
		bool m_Initialized = false;
	};
}
