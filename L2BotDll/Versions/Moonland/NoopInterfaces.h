#pragma once
#include "../GameStructs/NetworkHandlerInterface.h"
#include "../GameStructs/GameEngineInterface.h"
#include "../GameStructs/L2GameDataInterface.h"
#include "../GameStructs/FNameInterface.h"
#include "Domain/Services/HeroServiceInterface.h"
#include "PacketHandler.h"

// Moonland doesn't hook these at all - PacketHandler gets everything it
// needs from NetworkProbe's packet observer instead of GetProcAddress'd
// live-memory accessors, which is exactly what doesn't exist on this
// client build. These exist only so VersionAbstractFactory's interface is
// satisfied; Application::Init()/Stop() call Init()/Restore() on each and
// get a no-op either way.
namespace Moonland
{
	class NoopNetworkHandler : public NetworkHandlerInterface
	{
	public:
		// The one real hookup point: Application::Init() calls this
		// uniformly for every version, after NetworkProbe::Install() (which
		// runs earlier, directly from RuntimeThread) has already patched
		// vtable+0x7c. PacketHandler only registers an observer - it is
		// never the one installing or owning that hook.
		void Init(HMODULE) override
		{
			PacketHandler::GetInstance().Init();
		}
		void Restore() override {}
	};

	class NoopGameEngine : public GameEngineInterface
	{
	public:
		void Init(HMODULE) override {}
		void Restore() override {}
	};

	class NoopL2GameData : public L2GameDataInterface
	{
	public:
		void Init(HMODULE) override {}
		void Restore() override {}
	};

	class NoopFName : public FNameInterface
	{
	public:
		void Init(HMODULE) override {}
	};

	// No outgoing commands implemented yet (no confirmed opcode for move/
	// attack/skill requests, nor an encryption key recovered for sending).
	// Every action is a safe no-op rather than a guess that would corrupt
	// the connection or get flagged as malformed traffic.
	class NoopHeroService : public L2Bot::Domain::Services::HeroServiceInterface
	{
	public:
		void Move(L2Bot::Domain::ValueObjects::Vector3) const override {}
		void AcquireTarget(int) const override {}
		void Attack(int) const override {}
		void Pickup(int) const override {}
		void UseSkill(int, bool, bool) const override {}
		void UseItem(int) const override {}
		void ToggleAutouseSoulshot(int) const override {}
		void Sit() const override {}
		void Stand() const override {}
	};
}
