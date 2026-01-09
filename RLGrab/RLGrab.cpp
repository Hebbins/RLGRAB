#include "pch.h"
#include "RLGrab.h"

#include "imgui/imgui.h"

#include <Windows.h>
#include <shlobj_core.h>

#include <fstream>
#include <sstream>
#include <set>
#include <chrono>
#include <regex>

#pragma comment(lib, "shell32.lib")

std::shared_ptr<CVarManagerWrapper> _globalCvarManager;

BAKKESMOD_PLUGIN(RLGrab, "RL GRAB", plugin_version, PLUGINTYPE_FREEPLAY)

using namespace std::chrono_literals;

std::string RLGrab::GetPluginName() { return "RLGRAB"; }

// ----------------- Small helpers -----------------

std::string RLGrab::Trim(const std::string& s)
{
	size_t start = s.find_first_not_of(" \t\r\n");
	if (start == std::string::npos)
		return "";
	size_t end = s.find_last_not_of(" \t\r\n");
	return s.substr(start, end - start + 1);
}

bool RLGrab::Contains(const std::vector<std::string>& v, const std::string& value)
{
	for (const auto& it : v)
		if (it == value)
			return true;
	return false;
}

std::string RLGrab::ExtractIpOnly(const std::string& endpoint)
{
	// endpoint is "ip:port" OR "ServerName (ip:port)"
	// Try to find the last '(' and ')' and extract "ip:port" if present.
	auto openParen = endpoint.find_last_of('(');
	auto closeParen = endpoint.find_last_of(')');
	if (openParen != std::string::npos && closeParen != std::string::npos && closeParen > openParen + 1)
	{
		std::string inner = endpoint.substr(openParen + 1, closeParen - openParen - 1);
		auto colon = inner.find(':');
		if (colon != std::string::npos)
			return inner.substr(0, colon);
		return inner;
	}

	// Fallback: treat whole string as "ip:port" and strip port
	auto colon = endpoint.find(':');
	if (colon == std::string::npos)
		return endpoint;
	return endpoint.substr(0, colon);
}

// ----------------- Log file helpers -----------------

std::string RLGrab::GetDocumentsPath()
{
	wchar_t path[MAX_PATH] = { 0 };
	if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, path)))
	{
		std::wstring wpath(path);
		return std::string(wpath.begin(), wpath.end());
	}
	return "";
}

std::string RLGrab::GetLaunchLogPath()
{
	// Documents\My Games\Rocket League\TAGame\Logs\Launch.log
	std::string docs = GetDocumentsPath();
	if (docs.empty())
		return "";

	std::ostringstream oss;
	oss << docs << "\\My Games\\Rocket League\\TAGame\\Logs\\Launch.log";
	return oss.str();
}

void RLGrab::ParseLaunchLogLine(
	const std::string& line,
	std::string& outServerName,
	std::string& outGameUrl)
{
	// Look for ServerName="..."
	// and GameURL="..."
	static const std::regex serverNameRegex(R"(ServerName="([^"]*)\")");
		static const std::regex gameUrlRegex(R"(GameURL="([^"]*)\")");

		std::smatch m;
	if (std::regex_search(line, m, serverNameRegex) && m.size() > 1)
	{
		outServerName = m[1].str();
	}
	if (std::regex_search(line, m, gameUrlRegex) && m.size() > 1)
	{
		outGameUrl = m[1].str();
	}
}

void RLGrab::ScanLaunchLog()
{
	std::string path = GetLaunchLogPath();
	if (path.empty())
	{
		return;
	}

	std::ifstream file(path, std::ios::in);
	if (!file.is_open())
	{
		return;
	}

	// We will parse from the beginning each time.
	// To avoid duplicates, we keep a set of endpoints we have seen in this plugin instance.
	std::vector<std::string> newEndpoints;
	std::set<std::string> seenEndpoints;

	{
		std::lock_guard<std::mutex> lock(ipsMutex);
		seenEndpoints.insert(knownEndpoints.begin(), knownEndpoints.end());
	}

	std::string line;
	std::string currentServerName;
	std::string currentGameUrl;

	while (std::getline(file, line))
	{
		std::string serverName;
		std::string gameUrl;
		ParseLaunchLogLine(line, serverName, gameUrl);

		if (!serverName.empty())
			currentServerName = serverName;
		if (!gameUrl.empty())
			currentGameUrl = gameUrl;

		// When we have a GameURL, we can log an endpoint.
		if (!currentGameUrl.empty())
		{
			// GameURL expected like "ip:port" or maybe with additional query params; keep it raw.
			std::string endpointLabel;
			if (!currentServerName.empty())
			{
				// Format: ServerName (ip:port)
				std::ostringstream ss;
				ss << currentServerName << " (" << currentGameUrl << ")";
				endpointLabel = ss.str();
			}
			else
			{
				endpointLabel = currentGameUrl;
			}

			// Avoid pure duplicates if logDuplicates == false.
			bool alreadySeen = (seenEndpoints.find(endpointLabel) != seenEndpoints.end());

			if (logDuplicates || !alreadySeen)
			{
				newEndpoints.push_back(endpointLabel);
				seenEndpoints.insert(endpointLabel);
			}

			// Reset currentGameUrl so a single line doesn't repeatedly add.
			currentGameUrl.clear();
		}
	}

	if (newEndpoints.empty())
		return;

	// Store with newest on top.
	{
		std::lock_guard<std::mutex> lock(ipsMutex);
		for (const auto& ep : newEndpoints)
		{
			// Insert at the beginning so newest appear first.
			knownEndpoints.insert(knownEndpoints.begin(), ep);
		}

		// Adjust selection to the first/newest if nothing selected yet.
		if (selectedIndex < 0 && !knownEndpoints.empty())
		{
			selectedIndex = 0;
		}
	}
}

// ----------------- BakkesMod lifecycle -----------------

void RLGrab::onLoad()
{
	// Defaults
	pollIntervalMs = 3000; // check every few seconds
	logDuplicates = false;

	running = true;
	inMatch = false;   // no longer used for network state, but kept for compatibility
	ipScanDone = false; // no longer used to stop scanning, always scanning

	RegisterCVars();
	RegisterNotifiers();
	RegisterHooks();

	_globalCvarManager = cvarManager;

	// Initial scan on load
	ScanLaunchLog();

	// Worker thread: periodically re-read Launch.log
	workerThread = std::thread([this]() { WorkerLoop(); });

	cvarManager->log("RLGrab loaded (log watcher).");
}

void RLGrab::onUnload()
{
	running = false;
	inMatch = false;

	if (workerThread.joinable())
		workerThread.join();

	cvarManager->log("RLGrab unloaded.");
}

// ----------------- BakkesMod plugin plumbing -----------------
void RLGrab::SetImGuiContext(uintptr_t ctx)
{
	ImGui::SetCurrentContext(reinterpret_cast<ImGuiContext*>(ctx));
}

void RLGrab::RenderSettings()
{
	ImGui::TextUnformatted("RL server IPs seen from Launch.log:");
	ImGui::Separator();
	RenderIpListUI();

	ImGui::Separator();

	// Basic options
	int poll = pollIntervalMs;
	if (ImGui::SliderInt("Poll interval (ms)", &poll, 1000, 10000))
	{
		pollIntervalMs = poll;
		auto c = cvarManager->getCvar("rlgrab_poll_interval_ms");
		if (!c.IsNull())
			c.setValue(pollIntervalMs);
	}

	bool logDup = logDuplicates;
	if (ImGui::Checkbox("Keep duplicate endpoints", &logDup))
	{
		logDuplicates = logDup;
		auto c = cvarManager->getCvar("rlgrab_log_duplicates");
		if (!c.IsNull())
			c.setValue(logDuplicates);
	}
}

// ----------------- UI -----------------

void RLGrab::RenderIpListUI()
{
	std::lock_guard<std::mutex> lock(ipsMutex);

	if (knownEndpoints.empty())
	{
		ImGui::TextUnformatted("No endpoints found yet. Play a match so Launch.log contains server info.");
		return;
	}

	// Build list of labels (newest first)
	std::vector<const char*> items;
	items.reserve(knownEndpoints.size());
	for (const auto& ep : knownEndpoints)
	{
		items.push_back(ep.c_str());
	}

	if (selectedIndex < 0 || selectedIndex >= (int)items.size())
		selectedIndex = 0;

	ImGui::Text("Endpoints (%d):", (int)items.size());
	ImGui::PushItemWidth(-1.0f);

	ImGui::ListBox("##rlgrab_eps", &selectedIndex, items.data(), (int)items.size(), 6);
	ImGui::PopItemWidth();

	if (ImGui::Button("Copy IP"))
	{
		CopySelectedIpToClipboard();
	}
}

void RLGrab::CopySelectedIpToClipboard()
{
	std::string ipOnly;
	{
		std::lock_guard<std::mutex> lock(ipsMutex);

		if (knownEndpoints.empty() || selectedIndex < 0 || selectedIndex >= (int)knownEndpoints.size())
			return;

		std::string endpoint = knownEndpoints[selectedIndex];
		ipOnly = ExtractIpOnly(endpoint);
	}

	if (ipOnly.empty())
		return;

	// Convert to wide string
	std::wstring wstr(ipOnly.begin(), ipOnly.end());
	const SIZE_T byteSize = (wstr.size() + 1) * sizeof(wchar_t);

	// Allocate global memory that the clipboard will own
	HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, byteSize);
	if (!hMem)
		return;

	void* pData = GlobalLock(hMem);
	if (!pData)
	{
		GlobalFree(hMem);
		return;
	}

	memcpy(pData, wstr.c_str(), byteSize);
	GlobalUnlock(hMem);

	// Optional: use the game window handle if available; otherwise nullptr
	HWND hwndOwner = nullptr;

	if (!OpenClipboard(hwndOwner))
	{
		GlobalFree(hMem);
		return;
	}

	if (!EmptyClipboard())
	{
		CloseClipboard();
		GlobalFree(hMem);
		return;
	}

	if (!SetClipboardData(CF_UNICODETEXT, hMem))
	{
		// On failure, we must free the memory because the system does not take ownership.
		GlobalFree(hMem);
		CloseClipboard();
		return;
	}

	// Success: the system now owns hMem; do NOT free it.
	CloseClipboard();
}


// ----------------- CVars / Hooks / Match state -----------------

void RLGrab::RegisterCVars()
{
	cvarManager->registerCvar("rlgrab_poll_interval_ms", std::to_string(pollIntervalMs), "Poll interval in milliseconds for Launch.log scan")
		.addOnValueChanged([this](std::string, CVarWrapper cvar)
			{
				int v = cvar.getIntValue();
				if (v < 1000) v = 1000;
				pollIntervalMs = v;
			});

	cvarManager->registerCvar("rlgrab_log_duplicates", logDuplicates ? "1" : "0", "Keep duplicate endpoints")
		.addOnValueChanged([this](std::string, CVarWrapper cvar)
			{
				logDuplicates = cvar.getBoolValue();
			});
}

void RLGrab::RegisterNotifiers()
{
	// Manual reset
	cvarManager->registerNotifier("rlgrab_reset",
		[this](std::vector<std::string>) {
			std::lock_guard<std::mutex> lock(ipsMutex);
			knownEndpoints.clear();
			selectedIndex = -1;
		},
		"Reset RLGrab IP list for current session", PERMISSION_ALL);

	// Optional: force rescan of Launch.log
	cvarManager->registerNotifier("rlgrab_rescan_log",
		[this](std::vector<std::string>) {
			ScanLaunchLog();
		},
		"Force immediate rescan of Launch.log", PERMISSION_ALL);
}

void RLGrab::RegisterHooks()
{
	// Hooks kept minimal; log-based approach does not require match events,
	// but we can still clear on new games if desired.

	gameWrapper->HookEvent("Function TAGame.GameEvent_Soccar_TA.PostBeginPlay",
		[this](std::string) {
			std::lock_guard<std::mutex> lock(ipsMutex);
			// Optionally: clear on new match
			// knownEndpoints.clear();
			// selectedIndex = -1;
		});

	gameWrapper->HookEvent("Function TAGame.GameEvent_Soccar_TA.EventMatchEnded",
		[this](std::string) {
			// No special handling needed; Launch.log scanning continues.
		});
}

void RLGrab::OnMatchStarted(std::string)
{
	// Not used in log-based approach, kept for compatibility.
	inMatch = true;
}

void RLGrab::OnMatchEnded(std::string)
{
	inMatch = false;
}

// ----------------- Worker loop -----------------

void RLGrab::WorkerLoop()
{
	while (running)
	{
		ScanLaunchLog();
		std::this_thread::sleep_for(std::chrono::milliseconds(pollIntervalMs));
	}
}
