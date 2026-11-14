#include "pch.h"
#include "VersionAbstractFactory.h"
#include "Interlude/AbstractFactory.h"
#include "Moonland/AbstractFactory.h"

const VersionAbstractFactory& VersionAbstractFactory::GetFactory(const Version version, const uint16_t radius)
{
	switch (version)
	{
	case Version::interlude:
	{
		// Function-local statics are initialised once, on first use, and are
		// thread-safe since C++11; the factory outlives the game session.
		static Interlude::AbstractFactory interlude = Interlude::AbstractFactory(radius);
		return interlude;
	}
	case Version::classic:
	{
		static Moonland::AbstractFactory moonland = Moonland::AbstractFactory();
		return moonland;
	}
	}

	// Falling off a switch returning a reference is undefined behaviour.
	// Never guess a factory: refuse explicitly.
	throw UnsupportedVersion(version);
}