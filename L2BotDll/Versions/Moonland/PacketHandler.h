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

		// Running total of damage this id has taken, from 0x62's delta
		// field. Kept as a fallback alongside the real currentHp/maxHp
		// below (from 0x18, confirmed - see the doc): 0x18 only fires for
		// an id the hero has targeted, so anything never selected still
		// has maxHp == 0 and only this counter to go on.
		uint32_t damageTaken = 0;

		// Real HP, from 0x18 (confirmed: matches the exact value shown in
		// the game's own target-HP display, including fluctuating with the
		// target's own regen between hits - not a simple damage counter).
		// maxHp is 0 until the hero has targeted this id at least once (the
		// first 0x18 for a given id carries both current and max; every
		// later one for the same id only refreshes current).
		uint32_t currentHp = 0;
		uint32_t maxHp = 0;
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

		// Same running total as TrackedNpc::damageTaken, for the hero.
		uint32_t GetHeroDamageTaken() const;

		// Real current HP/MP, from 0x18's hero-tied shape (see the doc).
		// No confirmed source yet for max HP/MP - both return 0 until the
		// still-missing own-CharInfo packet (0x0b) is decoded further.
		uint32_t GetHeroCurrentHp() const;
		uint32_t GetHeroCurrentMp() const;

		void Reset();

	private:
		PacketHandler() = default;
		void OnPacket(const unsigned char* data, int len);

		mutable std::mutex m_Mutex;
		std::unordered_map<uint32_t, TrackedNpc> m_Npcs;
		uint32_t m_HeroId = 0;
		uint32_t m_HeroTargetId = 0;
		Position m_HeroPosition;
		uint32_t m_HeroDamageTaken = 0;
		uint32_t m_HeroCurrentHp = 0;
		uint32_t m_HeroCurrentMp = 0;
		bool m_Initialized = false;
	};
}
