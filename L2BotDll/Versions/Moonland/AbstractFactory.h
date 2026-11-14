#pragma once
#include "../VersionAbstractFactory.h"
#include "EmptyRepository.h"
#include "HeroRepository.h"
#include "NPCRepository.h"
#include "NoopInterfaces.h"
#include "PacketHandler.h"

namespace Moonland
{
	// VersionAbstractFactory::Version::classic. Named after the client this
	// was reverse engineered against (Moonland's "Custom Interlude
	// Remastered"), not after the Interlude protocol version - the wire
	// protocol itself was renumbered from stock Interlude, so this factory
	// shares none of Versions/Interlude's GameStructs code.
	class AbstractFactory : public VersionAbstractFactory
	{
	public:
		AbstractFactory() = default;
		virtual ~AbstractFactory() = default;

		Repositories::EntityRepositoryInterface& GetHeroRepository() const override
		{
			static HeroRepository result;
			return result;
		}
		Repositories::EntityRepositoryInterface& GetDropRepository() const override
		{
			static EmptyRepository result;
			return result;
		}
		Repositories::EntityRepositoryInterface& GetNPCRepository() const override
		{
			static NPCRepository result;
			return result;
		}
		Repositories::EntityRepositoryInterface& GetPlayerRepository() const override
		{
			static EmptyRepository result;
			return result;
		}
		Repositories::EntityRepositoryInterface& GetSkillRepository() const override
		{
			static EmptyRepository result;
			return result;
		}
		Repositories::EntityRepositoryInterface& GetItemRepository() const override
		{
			static EmptyRepository result;
			return result;
		}
		Repositories::EntityRepositoryInterface& GetAbnormalEffectRepository() const override
		{
			static EmptyRepository result;
			return result;
		}
		Repositories::EntityRepositoryInterface& GetChatMessageRepository() const override
		{
			static EmptyRepository result;
			return result;
		}
		Services::HeroServiceInterface& GetHeroService() const override
		{
			static NoopHeroService result;
			return result;
		}
		NetworkHandlerInterface& GetNetworkHandler() const override
		{
			static NoopNetworkHandler result;
			return result;
		}
		GameEngineInterface& GetGameEngine() const override
		{
			static NoopGameEngine result;
			return result;
		}
		L2GameDataInterface& GetL2GameData() const override
		{
			static NoopL2GameData result;
			return result;
		}
		FNameInterface& GetFName() const override
		{
			static NoopFName result;
			return result;
		}
	};
}
