#include "pch.h"
#include "Common/apihook.h"
#include "Common/NetworkProbe.h"
#include "Application.h"
#include "ProcessManipulation.h"
#include "Injector.h"
#include <algorithm>
#include <cctype>
#include <string>

InjectLibrary::Injector injector("L2BotHookMutex", WH_CALLWNDPROC);
HMODULE g_Module = NULL;
bool g_IsLineageProcess = false;
int g_ProcessId = 0;

// The Lineage 2 client ships as l2.exe on retail and as l2.bin on several
// private-server packs. Match both, case-insensitively, so the DLL no longer
// has to be byte-patched for a renamed executable.
static bool IsLineageProcessName(std::string name)
{
	std::transform(name.begin(), name.end(), name.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });

	static const char* const kNames[] = { "l2.exe", "l2.bin" };
	for (const char* candidate : kNames)
	{
		if (name == candidate) return true;
	}
	return false;
}

void ConfigLogger(HMODULE hModule)
{
	wchar_t buf[MAX_PATH];
	GetModuleFileNameW(hModule, buf, MAX_PATH);
	const std::wstring libName(buf);

	std::wstring directory;
	const size_t lastSlashIndex = libName.rfind(L"\\");
	if (std::string::npos != lastSlashIndex)
	{
		directory = libName.substr(0, lastSlashIndex);
	}

	std::vector <std::unique_ptr<Logger::LogChannel>> channels;
#ifdef _DEBUG
	channels.push_back(std::make_unique<OutputDebugLogChannel>(std::vector<Logger::LogLevel>{}));
#endif
	channels.push_back(std::make_unique<FileLogChannel>(directory + L"\\app.log", std::vector<Logger::LogLevel>{
#ifndef _DEBUG
		Logger::LogLevel::error,
		Logger::LogLevel::warning
#endif
	}));
	channels.push_back(std::make_unique<ChatLogChannel>(Enums::ChatChannelEnum::log, std::vector<Logger::LogLevel>{
		Logger::LogLevel::app
	}));
	Services::ServiceLocator::GetInstance().SetLogger(std::make_unique<Logger::Logger>(std::move(channels)));
}

DWORD WINAPI RuntimeThread(LPVOID)
{
	if (!g_IsLineageProcess)
	{
		return 0;
	}

	ConfigLogger(g_Module);

	// Diagnostic only, never throws: this build's UNetworkHandler hooks fail
	// to resolve (the class no longer exists in this Engine.dll), so probe the
	// socket recv path directly to find whatever replaced it before Start()
	// throws. Remove once NetworkHandlerWrapper targets the real dispatcher.
	NetworkProbe::Install(GetModuleHandleA("Engine.dll"));

	try
	{
		// The injected runtime belongs to the game session. It is deliberately
		// not destructed by the CRT while other threads are being terminated.
		// Hardcoded to Moonland's protocol (see docs/en/MOONLAND_PROTOCOL_RE.md).
		// Version::interlude is the stock build this fork started from; swap
		// back if building against an actual unmodified Interlude server.
		auto application = new Application(VersionAbstractFactory::Version::classic);
		InjectLibrary::StopCurrentProcess();
		try { application->Start(); }
		catch (...) { InjectLibrary::StartCurrentProcess(); throw; }
		InjectLibrary::StartCurrentProcess();
		Services::ServiceLocator::GetInstance().GetLogger()->Info(L"attached to Lineage 2 client {:#010x}", g_ProcessId);
	}
	catch (...)
	{
		Services::ServiceLocator::GetInstance().GetLogger()->Error(L"Unable to initialize the injected runtime");
	}

	return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule,
	DWORD  ul_reason_for_call,
	LPVOID lpReserved
)
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
		DisableThreadLibraryCalls(hModule);
		g_Module = hModule;
		g_ProcessId = GetCurrentProcessId();
		g_IsLineageProcess = IsLineageProcessName(InjectLibrary::GetCurrentProcessName());

		injector.SetHook(hModule);
		if (g_IsLineageProcess)
		{
			// Live hooks point into this module. Keep them valid until game exit;
			// never wait for worker threads from DllMain's loader lock.
			HMODULE pinned = NULL;
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
				reinterpret_cast<LPCWSTR>(&DllMain), &pinned)) return FALSE;
			HANDLE worker = CreateThread(NULL, 0, RuntimeThread, NULL, 0, NULL);
			if (worker) CloseHandle(worker);
		}
		break;
	case DLL_PROCESS_DETACH:

		injector.SetHook();
		break;
	}
	return TRUE;
}
