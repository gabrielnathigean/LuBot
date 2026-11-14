#pragma once
#include "Domain/Repositories/EntityRepositoryInterface.h"
#include "Domain/Entities/Hero.h"
#include "PacketHandler.h"

// The client UI (both the creature list and the map) refuses to build any
// view model at all - for the hero or for anything else - until a non-null
// Hero exists (see Client/Application/ViewModels/MainViewModel.cs and
// CreatureMapViewModel.cs). PacketHandler resolves the hero's real object
// id directly from memory (GetMyPawn(), see its header comment), its
// position from the last Attack packet the hero was part of, and real
// current HP/MP from 0x18's hero-tied shape; this repository turns all of
// that into the Hero the UI needs, with maxHp/maxMp left at 0 (unknown -
// no confirmed source yet, likely the still-undecoded rest of the 0x0b
// own-CharInfo packet) and every other stat defaulted until a real
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
			if (!heroId) return result; // not yet resolved - UI stays blank until it is

			const auto pos = PacketHandler::GetInstance().GetHeroPosition();
			const ValueObjects::Transform transform(
				ValueObjects::Vector3(pos.x, pos.y, pos.z),
				ValueObjects::Vector3(), ValueObjects::Vector3(), ValueObjects::Vector3()
			);

			const ValueObjects::VitalStats vitalStats(
				0, PacketHandler::GetInstance().GetHeroCurrentHp(),
				0, PacketHandler::GetInstance().GetHeroCurrentMp(),
				0, PacketHandler::GetInstance().GetHeroCurrentCp()
			);

			result[heroId] = std::make_shared<Entities::Hero>(
				heroId,
				transform,
				ValueObjects::FullName(),
				vitalStats,
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
