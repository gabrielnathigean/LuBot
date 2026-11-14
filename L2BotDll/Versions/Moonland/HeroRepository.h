#pragma once
#include "Domain/Repositories/EntityRepositoryInterface.h"
#include "Domain/Entities/Hero.h"
#include "PacketHandler.h"

// The client UI (both the creature list and the map) refuses to build any
// view model at all - for the hero or for anything else - until a non-null
// Hero exists (see Client/Application/ViewModels/MainViewModel.cs and
// CreatureMapViewModel.cs). There is no confirmed opcode yet for our own
// CharInfo/UserInfo, so this can't read a real one; PacketHandler infers
// the hero's object id from Attack packets instead (see its header comment)
// and this repository turns that into the minimal Hero the UI needs to
// start rendering anything, with every stat defaulted until a real
// CharInfo/UserInfo opcode is found.
namespace Moonland
{
	class HeroRepository : public L2Bot::Domain::Repositories::EntityRepositoryInterface
	{
	public:
		const std::unordered_map<std::uint32_t, std::shared_ptr<L2Bot::Domain::Entities::EntityInterface>> GetEntities() override
		{
			using namespace L2Bot::Domain;

			std::unordered_map<std::uint32_t, std::shared_ptr<Entities::EntityInterface>> result;
			const auto heroId = PacketHandler::GetInstance().GetHeroId();
			if (!heroId) return result; // not yet inferred - UI stays blank until it is

			result[heroId] = std::make_shared<Entities::Hero>(
				heroId,
				ValueObjects::Transform(),  // position unknown - see class comment
				ValueObjects::FullName(),
				ValueObjects::VitalStats(0, 0, 0, 0, 0, 0),
				ValueObjects::Phenotype(),
				ValueObjects::ExperienceInfo(),
				ValueObjects::PermanentStats(),
				ValueObjects::VariableStats(),
				ValueObjects::Reputation(),
				ValueObjects::InventoryInfo(),
				PacketHandler::GetInstance().GetHeroTargetId(),
				true // isStanding - unknown, defaults to not sitting
			);
			return result;
		}

		void Reset() override {}
	};
}
