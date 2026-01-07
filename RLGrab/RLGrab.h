#pragma once

#include "GuiBase.h"
#include "bakkesmod/plugin/bakkesmodplugin.h"
#include "bakkesmod/plugin/pluginwindow.h"
#include "bakkesmod/plugin/PluginSettingsWindow.h"

#include "version.h"

#include <mutex>
#include <atomic>
#include <thread>
#include <vector>
#include <string>

#include <Windows.h>

constexpr auto plugin_version = stringify(VERSION_MAJOR) "." stringify(VERSION_MINOR) "." stringify(VERSION_PATCH) "." stringify(VERSION_BUILD);


class RLGrab : public BakkesMod::Plugin::BakkesModPlugin, public BakkesMod::Plugin::PluginSettingsWindow
{
public:
	// BakkesMod lifecycle
	virtual void onLoad() override;
	virtual void onUnload() override;
	void RenderSettings() override;
	std::string GetPluginName() override;
	virtual void SetImGuiContext(uintptr_t ctx) override;

private:
	// Settings
	int  pollIntervalMs;    // How often to poll while in match
	bool logDuplicates;     // If false, only keep unique endpoints

	// State
	std::atomic<bool> running;
	std::atomic<bool> inMatch;
	std::atomic<bool> ipScanDone;  // true once we found > 0 IPs this match

	std::thread workerThread;

	std::mutex ipsMutex;
	std::vector<std::string> knownEndpoints; // "ip:port"
	int selectedIndex = -1;

	// BakkesMod helpers
	void RegisterCVars();
	void RegisterNotifiers();
	void RegisterHooks();

	// Match state
	void OnMatchStarted(std::string eventName);
	void OnMatchEnded(std::string eventName);

	// Worker
	void WorkerLoop();
	void QueryAndStoreIPs();

	// Integrated helper functionality
	struct ConnectionInfo
	{
		std::string localAddress;
		uint16_t    localPort;
		std::string remoteAddress;
		uint16_t    remotePort;
		unsigned long pid;
	};

	static unsigned long FindRocketLeaguePid();
	static bool GetAllTcpConnections(std::vector<ConnectionInfo>& outConnections);
	static std::vector<ConnectionInfo> FilterByPid(const std::vector<ConnectionInfo>& all, unsigned long pid);

	// Utilities
	static std::string Trim(const std::string& s);
	static bool Contains(const std::vector<std::string>& v, const std::string& value);
	static std::string ExtractIpOnly(const std::string& endpoint);

	// UI helpers
	void RenderIpListUI();
	void CopySelectedIpToClipboard();
};