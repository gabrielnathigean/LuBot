#pragma once
#include "Domain/Repositories/EntityRepositoryInterface.h"

// Always-empty EntityRepositoryInterface, for every repository Moonland
// doesn't have a packet-derived source for yet (hero, drop, player, skill,
// item, abnormal effect, chat). VersionAbstractFactory::GetRepositories()
// requires all eight; returning nothing from these is honest about what
// isn't implemented, rather than reading broken live-memory pointers like
// the Interlude wrappers still try to.
namespace Moonland
{
	class EmptyRepository : public L2Bot::Domain::Repositories::EntityRepositoryInterface
	{
	public:
		const std::unordered_map<std::uint32_t, std::shared_ptr<L2Bot::Domain::Entities::EntityInterface>> GetEntities() override
		{
			return {};
		}
		void Reset() override {}
	};
}
