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
	int  pollIntervalMs;    // How often to rescan Launch.log
	bool logDuplicates;     // If false, only keep unique endpoints

	// State
	std::atomic<bool> running;
	std::atomic<bool> inMatch;     // kept for compatibility, not required by log scanning
	std::atomic<bool> ipScanDone;  // unused by log scanning, kept for compatibility if needed

	std::thread workerThread;

	std::mutex ipsMutex;
	std::vector<std::string> knownEndpoints; // labels, e.g. "ServerName (ip:port)" or "ip:port"
	int selectedIndex = -1;

	// BakkesMod helpers
	void RegisterCVars();
	void RegisterNotifiers();
	void RegisterHooks();

	// Match state (kept for compatibility)
	void OnMatchStarted(std::string eventName);
	void OnMatchEnded(std::string eventName);

	// Worker
	void WorkerLoop();

	// Log-based collection
	void ScanLaunchLog();
	static std::string GetDocumentsPath();
	static std::string GetLaunchLogPath();
	static void ParseLaunchLogLine(
		const std::string& line,
		std::string& outServerName,
		std::string& outGameUrl);

	// Utilities
	static std::string Trim(const std::string& s);
	static bool Contains(const std::vector<std::string>& v, const std::string& value);
	static std::string ExtractIpOnly(const std::string& endpoint);

	// UI helpers
	void RenderIpListUI();
	void CopySelectedIpToClipboard();
};
