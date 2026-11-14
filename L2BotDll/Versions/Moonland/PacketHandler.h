#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

// Real, per-opcode parser for Moonland's custom protocol (see
// docs/en/MOONLAND_PROTOCOL_RE.md in the repo root for how these were
// confirmed). Subscribes to NetworkProbe's packet observer - does not
// install its own hook, so it can never conflict with or duplicate
// NetworkProbe's vtable+0x7c patch.
//
// Position comes from the Attack (0x33) packet, not from NpcInfo (0x0c -
// its candidate coordinate-shaped dwords never correlated cleanly across
// samples, see the doc): 0x33 carries the attacker's and target's position
// directly as two (X,Y,Z) triples, confirmed by matching its byte offsets
// to the packet's own fixed 53-byte length. That means a tracked NPC's
// position is only as fresh as its last recorded hit - one that hasn't
// fought anyone yet still reports (0,0,0).
namespace Moonland
{
	struct Position
	{
		float_t x = 0;
		float_t y = 0;
		float_t z = 0;
	};

	struct TrackedNpc
	{
		std::wstring name;
		bool alive = true;
		Position position;
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

		// Read directly from game memory: GEngine -> UGameEngine::GetMyPawn()
		// -> [+0x58] (both the global and the accessor are still exported
		// by name even though the packet-handling classes are gone - see
		// the doc). No confirmed packet carries our own object id, and a
		// frequency-based guess over Attack packets turned out unreliable
		// in any populated area, so this reads the real value instead.
		// Returns 0 before the hero has spawned into the world.
		uint32_t GetHeroId() const;
		uint32_t GetHeroTargetId() const;

		// Last position seen for the hero in a 0x33 packet where the hero
		// was the attacker. (0,0,0) until the hero has landed a hit.
		Position GetHeroPosition() const;

		void Reset();

	private:
		PacketHandler() = default;
		void OnPacket(const unsigned char* data, int len);

		mutable std::mutex m_Mutex;
		std::unordered_map<uint32_t, TrackedNpc> m_Npcs;
		uint32_t m_HeroId = 0;
		uint32_t m_HeroTargetId = 0;
		Position m_HeroPosition;
		bool m_Initialized = false;
	};
}
