#pragma once
#include <Windows.h>

// Diagnostic-only: IAT-hooks Engine.dll's imported recv/WSARecv so we can see,
// from a live process, which Engine.dll function actually owns the socket
// read loop in builds where UNetworkHandler no longer exists. Never throws;
// logs to <dll dir>\netprobe.log next to app.log. Safe to leave compiled in,
// since every failure path degrades to "do nothing" rather than crashing the
// host process.
namespace NetworkProbe
{
	void Install(HMODULE hEngine);
}
