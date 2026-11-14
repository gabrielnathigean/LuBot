#pragma once
#include "Domain/Repositories/EntityRepositoryInterface.h"
#include "Domain/Entities/NPC.h"
#include "Domain/ValueObjects/Transform.h"
#include "Domain/ValueObjects/FullName.h"
#include "Domain/ValueObjects/VitalStats.h"
#include "PacketHandler.h"

// Packet-derived NPC source: every entry comes from a real 0x0c (NpcInfo)
// packet observed live, with alive/dead state and position kept in sync by
// 0x33 (Attack, which carries both sides' position directly) and 0x00
// (Die). See docs/en/MOONLAND_PROTOCOL_RE.md for how each opcode was
// confirmed. Position is only as fresh as the NPC's last recorded hit - one
// that hasn't fought anyone yet still reports (0,0,0), since NpcInfo's own
// candidate coordinate fields never correlated cleanly across samples.
// This still gives real creature identity, combat flag and death state
// instead of the empty list the Interlude-era GetNextCreature() produces
// on this client (it reads live memory through an export that no longer
// exists).
namespace Moonland
{
	class NPCRepository : public L2Bot::Domain::Repositories::EntityRepositoryInterface
	{
	public:
		const std::unordered_map<std::uint32_t, std::shared_ptr<L2Bot::Domain::Entities::EntityInterface>> GetEntities() override
		{
			using namespace L2Bot::Domain;

			std::unordered_map<std::uint32_t, std::shared_ptr<Entities::EntityInterface>> result;
			for (const auto& kvp : PacketHandler::GetInstance().GetNpcSnapshot())
			{
				const auto objectId = kvp.first;
				const auto& tracked = kvp.second;

				const ValueObjects::VitalStats vitalStats = tracked.alive
					? ValueObjects::VitalStats(0, 0, 0, 0, 0, 0) // maxHp=0 -> IsAlive() treats as alive
					: ValueObjects::VitalStats(1, 0, 0, 0, 0, 0); // maxHp=1, hp=0 -> IsAlive() == false

				const ValueObjects::Transform transform(
					ValueObjects::Vector3(tracked.position.x, tracked.position.y, tracked.position.z),
					ValueObjects::Vector3(), ValueObjects::Vector3(), ValueObjects::Vector3()
				);

				result[objectId] = std::make_shared<Entities::NPC>(
					objectId,
					transform,
					false,                     // isHostile unknown yet
					0,                         // npcId (template id) unknown - no .dat lookup built
					ValueObjects::FullName(tracked.name, L""),
					vitalStats
				);
			}
			return result;
		}

		void Reset() override
		{
			PacketHandler::GetInstance().Reset();
		}
	};
}
