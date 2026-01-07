#include "pch.h"
#include "RLGrab.h"

#include "imgui/imgui.h"

#include <Windows.h>
#include <iphlpapi.h>
#include <tlhelp32.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <sstream>
#include <set>
#include <chrono>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")


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

static std::string IPv4ToString(DWORD addr)
{
	BYTE b1 = (addr) & 0xFF;
	BYTE b2 = (addr >> 8) & 0xFF;
	BYTE b3 = (addr >> 16) & 0xFF;
	BYTE b4 = (addr >> 24) & 0xFF;

	std::ostringstream ss;
	ss << (int)b1 << "." << (int)b2 << "." << (int)b3 << "." << (int)b4;
	return ss.str();
}

static uint16_t PortFromNetworkOrder(DWORD dwPort)
{
	uint16_t portNet = static_cast<uint16_t>(dwPort);
	return ntohs(portNet);
}

std::string RLGrab::ExtractIpOnly(const std::string& endpoint)
{
	// endpoint is "ip:port"
	auto pos = endpoint.find(':');
	if (pos == std::string::npos)
		return endpoint;
	return endpoint.substr(0, pos);
}

// ----------------- Process enumeration -----------------

unsigned long RLGrab::FindRocketLeaguePid()
{
	unsigned long pid = 0;

	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE)
		return 0;

	PROCESSENTRY32 pe;
	pe.dwSize = sizeof(PROCESSENTRY32);

	if (Process32First(snapshot, &pe))
	{
		do
		{
			std::wstring exe(pe.szExeFile);
			std::wstring target = L"RocketLeague.exe";

			if (_wcsicmp(exe.c_str(), target.c_str()) == 0)
			{
				pid = pe.th32ProcessID;
				break;
			}
		} while (Process32Next(snapshot, &pe));
	}

	CloseHandle(snapshot);
	return pid;
}

// ----------------- TCP enumeration -----------------

bool RLGrab::GetAllTcpConnections(std::vector<ConnectionInfo>& outConnections)
{
	outConnections.clear();

	ULONG size = 0;
	DWORD ret = GetExtendedTcpTable(
		nullptr,
		&size,
		TRUE,
		AF_INET,
		TCP_TABLE_OWNER_PID_ALL,
		0);

	if (ret != ERROR_INSUFFICIENT_BUFFER)
	{
		return false;
	}

	auto buf = std::make_unique<char[]>(size);
	PMIB_TCPTABLE_OWNER_PID table = reinterpret_cast<PMIB_TCPTABLE_OWNER_PID>(buf.get());

	ret = GetExtendedTcpTable(
		table,
		&size,
		TRUE,
		AF_INET,
		TCP_TABLE_OWNER_PID_ALL,
		0);

	if (ret != NO_ERROR)
	{
		return false;
	}

	for (DWORD i = 0; i < table->dwNumEntries; ++i)
	{
		const MIB_TCPROW_OWNER_PID& row = table->table[i];

		ConnectionInfo info;
		info.localAddress = IPv4ToString(row.dwLocalAddr);
		info.localPort = PortFromNetworkOrder(row.dwLocalPort);
		info.remoteAddress = IPv4ToString(row.dwRemoteAddr);
		info.remotePort = PortFromNetworkOrder(row.dwRemotePort);
		info.pid = row.dwOwningPid;

		outConnections.push_back(info);
	}

	return true;
}

std::vector<RLGrab::ConnectionInfo> RLGrab::FilterByPid(const std::vector<ConnectionInfo>& all, unsigned long pid)
{
	std::vector<ConnectionInfo> filtered;
	for (const auto& c : all)
	{
		if (c.pid == pid)
			filtered.push_back(c);
	}
	return filtered;
}

// ----------------- BakkesMod lifecycle -----------------

void RLGrab::onLoad()
{
	// Winsock init
	WSADATA wsaData;
	int wsaErr = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (wsaErr != 0)
	{
		cvarManager->log("RLGrab: WSAStartup failed with error " + std::to_string(wsaErr));
	}

	// Defaults
	pollIntervalMs = 1500;
	logDuplicates = false;

	running = true;
	inMatch = false;
	ipScanDone = false;

	RegisterCVars();
	RegisterNotifiers();
	RegisterHooks();

	_globalCvarManager = cvarManager;

	// Worker thread
	workerThread = std::thread([this]() { WorkerLoop(); });

	cvarManager->log("RLGrab loaded.");
}

void RLGrab::onUnload()
{
	running = false;
	inMatch = false;

	if (workerThread.joinable())
		workerThread.join();

	WSACleanup();

	cvarManager->log("RLGrab unloaded.");
}

// ----------------- BakkesMod plugin plumbing -----------------
void RLGrab::SetImGuiContext(uintptr_t ctx)
{
	ImGui::SetCurrentContext(reinterpret_cast<ImGuiContext*>(ctx));
}

void RLGrab::RenderSettings()
{
	ImGui::TextUnformatted("RL server IPs seen this match:");
	ImGui::Separator();
	RenderIpListUI();

	ImGui::Separator();

	// Basic options
	int poll = pollIntervalMs;
	if (ImGui::SliderInt("Poll interval (ms)", &poll, 250, 5000))
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
		ImGui::TextUnformatted("No endpoints found yet. Start a match and wait a few seconds.");
		return;
	}

	// Build list of labels (ip:port)
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
	std::lock_guard<std::mutex> lock(ipsMutex);

	if (knownEndpoints.empty() || selectedIndex < 0 || selectedIndex >= (int)knownEndpoints.size())
		return;

	std::string endpoint = knownEndpoints[selectedIndex];
	std::string ipOnly = ExtractIpOnly(endpoint);

	if (OpenClipboard(nullptr))
	{
		EmptyClipboard();

		const size_t size = (ipOnly.size() + 1) * sizeof(wchar_t);
		HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, size);
		if (hMem)
		{
			wchar_t* dst = (wchar_t*)GlobalLock(hMem);
			if (dst)
			{
				std::wstring wstr(ipOnly.begin(), ipOnly.end());
				wcscpy_s(dst, wstr.size() + 1, wstr.c_str());
				GlobalUnlock(hMem);
				SetClipboardData(CF_UNICODETEXT, hMem);
			}
			else
			{
				GlobalFree(hMem);
			}
		}

		CloseClipboard();
	}
}

// ----------------- CVars / Hooks / Match state -----------------

void RLGrab::RegisterCVars()
{
	cvarManager->registerCvar("rlgrab_poll_interval_ms", std::to_string(pollIntervalMs), "Poll interval in milliseconds")
		.addOnValueChanged([this](std::string, CVarWrapper cvar)
			{
				int v = cvar.getIntValue();
				if (v < 250) v = 250;
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
	// Optional: manual reset
	cvarManager->registerNotifier("rlgrab_reset",
		[this](std::vector<std::string>) {
			std::lock_guard<std::mutex> lock(ipsMutex);
			knownEndpoints.clear();
			selectedIndex = -1;
			ipScanDone = false;
		},
		"Reset RLGrab IP list for current session", PERMISSION_ALL);
}

void RLGrab::RegisterHooks()
{
	gameWrapper->HookEvent("Function TAGame.GameEvent_Soccar_TA.PostBeginPlay",
		[this](std::string eventName) { OnMatchStarted(eventName); });

	gameWrapper->HookEvent("Function TAGame.GameEvent_Soccar_TA.EventMatchEnded",
		[this](std::string eventName) { OnMatchEnded(eventName); });

	// You can add additional hooks here for other modes if needed.
}

void RLGrab::OnMatchStarted(std::string)
{
	inMatch = true;
	ipScanDone = false;

	std::lock_guard<std::mutex> lock(ipsMutex);
	knownEndpoints.clear();
	selectedIndex = -1;
}

void RLGrab::OnMatchEnded(std::string)
{
	inMatch = false;
	ipScanDone = true; // no more scanning this match
}

// ----------------- Worker loop -----------------

void RLGrab::WorkerLoop()
{
	while (running)
	{
		if (inMatch && !ipScanDone)
		{
			QueryAndStoreIPs();
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(pollIntervalMs));
	}
}

// ----------------- IP collection -----------------

void RLGrab::QueryAndStoreIPs()
{
	unsigned long rlPid = FindRocketLeaguePid();
	if (rlPid == 0)
	{
		return;
	}

	std::vector<ConnectionInfo> all;
	if (!GetAllTcpConnections(all))
	{
		return;
	}

	auto rlConns = FilterByPid(all, rlPid);
	if (rlConns.empty())
		return;

	std::lock_guard<std::mutex> lock(ipsMutex);

	std::set<std::string> sessionEndpoints(knownEndpoints.begin(), knownEndpoints.end());
	bool newEndpoint = false;

	for (const auto& c : rlConns)
	{
		if (c.remoteAddress == "0.0.0.0")
			continue;

		std::ostringstream ss;
		ss << c.remoteAddress << ":" << c.remotePort;
		std::string endpoint = ss.str();

		if (!logDuplicates && sessionEndpoints.count(endpoint))
			continue;

		if (!sessionEndpoints.count(endpoint))
		{
			sessionEndpoints.insert(endpoint);
			knownEndpoints.push_back(endpoint);
			newEndpoint = true;
		}
	}

	// If we found at least one endpoint, we can stop scanning for this match.
	if (newEndpoint && !knownEndpoints.empty())
	{
		ipScanDone = true;
	}
}