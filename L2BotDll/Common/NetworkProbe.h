#pragma once
#include <Windows.h>
#include <functional>

// Diagnostic-only: IAT-hooks Engine.dll's imported recv/WSARecv so we can see,
// from a live process, which Engine.dll function actually owns the socket
// read loop in builds where UNetworkHandler no longer exists. Never throws;
// logs to <dll dir>\netprobe.log next to app.log. Safe to leave compiled in,
// since every failure path degrades to "do nothing" rather than crashing the
// host process.
namespace NetworkProbe
{
	void Install(HMODULE hEngine);

	// Called, after the original dispatcher and the diagnostic log line, with
	// the still-encrypted-header-but-decrypted-body buffer exactly as handed
	// to Engine.dll's real packet dispatcher: data[0..1] is the 2-byte LE
	// length prefix, data[2] is the opcode, data[2..len) is the full payload.
	// Exists so a real per-opcode parser (Versions/Moonland/PacketHandler)
	// doesn't need its own vtable+0x7c hook - NetworkProbe already owns that
	// hook and forwards to the original unconditionally either way.
	void SetPacketObserver(std::function<void(const unsigned char* data, int len)> observer);

	// Lets other diagnostic code (e.g. Versions/Moonland/PacketHandler's
	// own-pawn probe) write to the same netprobe.log without duplicating
	// the path-resolution/locking logic here.
	void LogDiag(const std::wstring& line);
}
