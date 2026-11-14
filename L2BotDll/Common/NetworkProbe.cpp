#include "pch.h"
#include "NetworkProbe.h"
#include <winsock2.h>
#include <intrin.h>
#include <atomic>

#pragma comment(lib, "ws2_32.lib")

namespace NetworkProbe
{
	namespace
	{
		typedef int(WSAAPI* RecvFn)(SOCKET, char*, int, int);
		typedef int(WSAAPI* WSARecvFn)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);

		RecvFn g_OriginalRecv = nullptr;
		WSARecvFn g_OriginalWSARecv = nullptr;
		std::wstring g_LogPath;
		std::mutex g_LogMutex;

		std::wstring ResolveLogPath()
		{
			wchar_t buf[MAX_PATH] = {};
			HMODULE hSelf = nullptr;
			// GetModuleHandleExW with FROM_ADDRESS finds this very module without
			// needing a global HMODULE passed in from DllMain.
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
				reinterpret_cast<LPCWSTR>(&ResolveLogPath), &hSelf);
			if (hSelf && GetModuleFileNameW(hSelf, buf, MAX_PATH))
			{
				std::wstring path(buf);
				const size_t slash = path.rfind(L'\\');
				if (slash != std::wstring::npos) path = path.substr(0, slash);
				return path + L"\\netprobe.log";
			}
			return L"netprobe.log";
		}

		std::wstring ResolveCaller(void* returnAddress)
		{
			HMODULE hMod = nullptr;
			if (!GetModuleHandleExW(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(returnAddress), &hMod) || !hMod)
			{
				wchar_t fallback[32];
				swprintf_s(fallback, L"0x%p", returnAddress);
				return fallback;
			}

			wchar_t nameBuf[MAX_PATH] = {};
			GetModuleFileNameW(hMod, nameBuf, MAX_PATH);
			std::wstring name(nameBuf);
			const size_t slash = name.rfind(L'\\');
			if (slash != std::wstring::npos) name = name.substr(slash + 1);

			const uintptr_t base = reinterpret_cast<uintptr_t>(hMod);
			const uintptr_t addr = reinterpret_cast<uintptr_t>(returnAddress);
			wchar_t out[MAX_PATH + 32];
			swprintf_s(out, L"%s+0x%zx", name.c_str(), addr - base);
			return out;
		}

		void LogLine(const std::wstring& line)
		{
			std::lock_guard<std::mutex> lock(g_LogMutex);
			std::wofstream f(g_LogPath, std::ios::app);
			if (!f.is_open()) return;

			SYSTEMTIME st; GetLocalTime(&st);
			wchar_t ts[32];
			swprintf_s(ts, L"[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
			f << ts << line << L"\n";
		}

		std::wstring HexDump(const char* data, int len)
		{
			const int capped = (len < 0) ? 0 : (len > 1024 ? 1024 : len);
			std::wstringstream ss;
			for (int i = 0; i < capped; ++i)
			{
				wchar_t b[4];
				swprintf_s(b, L"%02x ", static_cast<unsigned char>(data[i]));
				ss << b;
			}
			return ss.str();
		}

		int WSAAPI HookedRecv(SOCKET s, char* buf, int len, int flags);

		// Static disassembly of the function that owns the socket read (always
		// Engine.dll+0x303a18 per netprobe.log) shows that once a full packet is
		// framed, it does `mov ecx, esi; call dword ptr [eax+0x7c]` where esi is
		// a this-pointer the function received in a stack argument and never
		// overwrites across the call to recv(). Per the x86 calling convention
		// esi is callee-saved, so the moment control reaches our IAT hook it
		// still holds that connection object - we just have to read it before
		// our own C++ prologue could touch it. Logged read-only; nothing here
		// is patched (no byte in Engine.dll or any vtable is modified).
		bool ProbeConnectionGuarded(void* connection, void** outVtable, void** outSlot7C)
		{
			__try
			{
				if (!connection) return false;
				void** vtable = *reinterpret_cast<void***>(connection);
				if (!vtable) return false;
				*outVtable = vtable;
				*outSlot7C = vtable[0x7c / sizeof(void*)];
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// The real per-packet dispatcher: __thiscall void(void* data, int len),
		// found at [vtable+0x7c] of the connection object. Original signature
		// decrypts `data` in place (first thing it does, per static disasm at
		// Engine.dll+0x302820) then builds a {opcode, subOpcode, dataPtr, len}
		// descriptor from the decrypted bytes - functionally the old
		// UNetworkHandler::AddNetworkQueue, just never exported by name here.
		typedef void(__thiscall* PacketDispatchFn)(void*, void*, int);
		PacketDispatchFn g_OriginalDispatch = nullptr;
		std::atomic<bool> g_PacketHookInstalled{ false };

		void __fastcall HookedPacketDispatch(void* self, void* /*edx*/, void* data, int len)
		{
			// Forward first: the original decrypts `data` in place, and nothing
			// downstream of this call should ever be skipped or delayed.
			if (g_OriginalDispatch) g_OriginalDispatch(self, data, len);

			if (len > 2)
			{
				// data[0..1] is still the 2-byte LE length prefix per static
				// disasm (Engine.dll+0x302820 reads the real opcode from
				// [buffer+2]) - log both so a mismatch is visible, not assumed.
				auto* bytes = static_cast<unsigned char*>(data);
				std::wstringstream ss;
				ss << L"packet decrypted len=" << len
				   << L" opcode=0x" << std::hex << static_cast<int>(bytes[2])
				   << L" data=" << HexDump(static_cast<char*>(data), len);
				LogLine(ss.str());
			}
		}

		bool InstallPacketDispatchGuarded(void* vtable, void** outOriginal)
		{
			__try
			{
				void** slot = reinterpret_cast<void**>(static_cast<char*>(vtable) + 0x7c);
				DWORD oldProtect;
				if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) return false;
				*outOriginal = *slot;
				*slot = reinterpret_cast<void*>(&HookedPacketDispatch);
				VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		void CaptureConnectionPointer(void* esiValue)
		{
			static std::atomic<int> loggedCount{ 0 };
			const bool stillLogging = loggedCount.fetch_add(1) < 20; // a few samples are enough

			void* vtable = nullptr;
			void* slot7C = nullptr;
			const bool ok = ProbeConnectionGuarded(esiValue, &vtable, &slot7C);

			if (stillLogging)
			{
				std::wstringstream ss;
				ss << L"conn-probe esi=0x" << std::hex << reinterpret_cast<uintptr_t>(esiValue);
				if (ok)
				{
					ss << L" vtable=0x" << std::hex << reinterpret_cast<uintptr_t>(vtable)
					   << L" slot[0x7c]=" << ResolveCaller(slot7C);
				}
				else
				{
					ss << L" (unreadable)";
				}
				LogLine(ss.str());
			}

			if (ok && !g_PacketHookInstalled.exchange(true))
			{
				void* original = nullptr;
				const bool installed = InstallPacketDispatchGuarded(vtable, &original);
				if (installed) g_OriginalDispatch = reinterpret_cast<PacketDispatchFn>(original);
				LogLine(installed
					? L"packet dispatch hook installed at vtable+0x7c"
					: L"packet dispatch hook FAILED to install");
			}
		}

		// Tail-jumps into HookedRecv after sampling esi, so the stdcall args
		// recv's IAT caller pushed are untouched when HookedRecv runs - it does
		// its own `ret 16` exactly as the real recv() would have.
		__declspec(naked) void NakedRecvEntry()
		{
			__asm
			{
				push esi
				call CaptureConnectionPointer
				add esp, 4
				jmp HookedRecv
			}
		}

		int WSAAPI HookedRecv(SOCKET s, char* buf, int len, int flags)
		{
			// Raw-bytes logging moved to HookedPacketDispatch (post-decrypt,
			// one line per complete message instead of one per TCP read).
			return g_OriginalRecv ? g_OriginalRecv(s, buf, len, flags) : SOCKET_ERROR;
		}

		int WSAAPI HookedWSARecv(SOCKET s, LPWSABUF buffers, DWORD bufferCount, LPDWORD bytesRecvd,
			LPDWORD flags, LPWSAOVERLAPPED overlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE completion)
		{
			void* caller = _ReturnAddress();
			int result = g_OriginalWSARecv
				? g_OriginalWSARecv(s, buffers, bufferCount, bytesRecvd, flags, overlapped, completion)
				: SOCKET_ERROR;

			if (result == 0 && bytesRecvd && *bytesRecvd > 0 && bufferCount > 0 && buffers)
			{
				std::wstringstream ss;
				ss << L"WSARecv caller=" << ResolveCaller(caller)
				   << L" sock=" << s
				   << L" bytes=" << *bytesRecvd
				   << L" data=" << HexDump(buffers[0].buf, static_cast<int>(*bytesRecvd));
				LogLine(ss.str());
			}
			return result;
		}

		// Finds the IAT slot Engine.dll uses for ws2_32!<importName> and repoints
		// it at hookFn, handing back the original function pointer. Returns
		// nullptr (does nothing) if Engine.dll does not import that symbol by
		// name, or the import table cannot be parsed - never throws.
		void* PatchImport(HMODULE hEngine, const char* importName, void* hookFn)
		{
			auto base = reinterpret_cast<uintptr_t>(hEngine);
			auto dos = reinterpret_cast<PIMAGE_DOS_HEADER>(base);
			if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;

			auto nt = reinterpret_cast<PIMAGE_NT_HEADERS>(base + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

			const auto& importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
			if (importDir.VirtualAddress == 0) return nullptr;

			auto descriptor = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(base + importDir.VirtualAddress);
			for (; descriptor->Name != 0; ++descriptor)
			{
				const char* dllName = reinterpret_cast<const char*>(base + descriptor->Name);
				if (_stricmp(dllName, "ws2_32.dll") != 0) continue;

				auto thunk = reinterpret_cast<PIMAGE_THUNK_DATA>(base + descriptor->FirstThunk);
				auto originalThunk = descriptor->OriginalFirstThunk
					? reinterpret_cast<PIMAGE_THUNK_DATA>(base + descriptor->OriginalFirstThunk)
					: thunk;

				for (; originalThunk->u1.AddressOfData != 0; ++thunk, ++originalThunk)
				{
					if (IMAGE_SNAP_BY_ORDINAL(originalThunk->u1.Ordinal)) continue;

					auto importByName = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(base + originalThunk->u1.AddressOfData);
					if (strcmp(importByName->Name, importName) != 0) continue;

					void* original = reinterpret_cast<void*>(thunk->u1.Function);

					DWORD oldProtect;
					if (!VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProtect))
						return nullptr;
					thunk->u1.Function = reinterpret_cast<ULONG_PTR>(hookFn);
					VirtualProtect(&thunk->u1.Function, sizeof(void*), oldProtect, &oldProtect);

					return original;
				}
			}
			return nullptr;
		}
	}

		// Isolated from Install() because a function containing a __try/__except
		// block must not also contain objects needing unwinding (C2712);
		// everything with C++ destructors (std::wstring, etc.) stays in Install.
		bool InstallGuarded(HMODULE hEngine, void** outRecv, void** outWSARecv)
		{
			__try
			{
				*outRecv = PatchImport(hEngine, "recv", reinterpret_cast<void*>(&NakedRecvEntry));
				*outWSARecv = PatchImport(hEngine, "WSARecv", reinterpret_cast<void*>(&HookedWSARecv));
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

	void Install(HMODULE hEngine)
	{
		if (!hEngine) return;
		g_LogPath = ResolveLogPath();

		void* originalRecv = nullptr;
		void* originalWSARecv = nullptr;
		const bool ran = InstallGuarded(hEngine, &originalRecv, &originalWSARecv);

		if (originalRecv) g_OriginalRecv = reinterpret_cast<RecvFn>(originalRecv);
		if (originalWSARecv) g_OriginalWSARecv = reinterpret_cast<WSARecvFn>(originalWSARecv);

		std::wstringstream ss;
		ss << L"NetworkProbe installed: ran=" << ran
		   << L" recv=" << (g_OriginalRecv ? L"hooked" : L"not-found-in-IAT")
		   << L" WSARecv=" << (g_OriginalWSARecv ? L"hooked" : L"not-found-in-IAT");
		LogLine(ss.str());
	}
}
