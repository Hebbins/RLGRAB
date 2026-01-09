#pragma once

#include "bakkesmod/plugin/bakkesmodplugin.h"
#include "bakkesmod/plugin/PluginSettingsWindow.h"
#include "bakkesmod/plugin/PluginWindow.h"
#include "bakkesmod/wrappers/GuiManagerWrapper.h"
#include "bakkesmod/wrappers/includes.h"
#include "bakkesmod/wrappers/PlayerControllerWrapper.h"

#include "bakkesmod/wrappers/Engine/EngineTAWrapper.h"
#include "bakkesmod/wrappers/Engine/EnumWrapper.h"

#include "version.h"
#include <fstream>
#include <memory>
#include <map>
#include <set>
#include <string>
#include <vector>

constexpr auto plugin_version = stringify(VERSION_MAJOR) "." stringify(VERSION_MINOR) "." stringify(VERSION_PATCH) "." stringify(VERSION_BUILD);
#define CURRENT_PLUGIN_VERSION stringify(VERSION_MAJOR) "." stringify(VERSION_MINOR) "." stringify(VERSION_PATCH)

struct ChatMessage {
	std::string user;
	std::string message;
	std::chrono::steady_clock::time_point receivedTime;
};

struct ScoreboardRowRect
{
	float x;
	float y;
	float width;
	float height;
};


class Seekr : public BakkesMod::Plugin::BakkesModPlugin, public BakkesMod::Plugin::PluginSettingsWindow
{
	std::string api_key;
	std::string upload_status_msg = "Idle";
	std::string user_role = "Not Signed In";
	int total_user_uploads = 0;

	const std::string API_URL = "https://seekrstats.com/api/";

	void sDebugLog(const std::string& logMsg) const;
	bool debugEnabled;

	// State
	bool update_available = false;
	bool show_update_modal = false;
	std::string latest_version_str = "";
	std::string update_download_url = "";
	bool is_downloading = false;
	std::string update_status_msg = "Up to date";
	bool is_updating = false;
	bool is_live_tracking = false;

	float timeCounter;

	// Scoreboard State
	bool isScoreboardOpen = false;

	// Caches
	std::map<std::string, int> repCache;
	std::map<std::string, std::string> nameCache;
	std::map<std::string, int> teamCache;
	std::set<std::string> seenMessageIds;

	// Chat
	std::vector<ChatMessage> chatMessages;
	bool isChatWindowOpen = false;
	char chatInputBuf[256] = "";
	std::string chatStatusMsg = "";

public:
	virtual void onLoad();
	virtual void onUnload();
	void DelayedInit();
	int GetTeamScore(int teamIndex);
	int GetCurrentMaxTeamSize();
	std::string GameModeTitle;
	std::string ServerTitle;
	std::string sSeriesGame;
	bool isGamemodeHeatseeker() const;

	// Chat Logic
	void ChatLoop();
	void GetChat();
	void RenderChat(CanvasWrapper& canvas);

	// ADD: Send Logic
	void SendChatMessage(std::string message);

	// Match Logic
	void StartLiveUpdateLoop(std::string eventName);
	void LiveUpdateLoop();
	void StopLiveUpdateLoop();
	void OnMatchEnded(std::string eventName);
	void UploadMatchData(ServerWrapper& server, std::string key, std::string matchStatus = "FINISHED");
	void CheckUserProfile();

	// Reputation Logic
	void OnLobbyEvent(std::string eventName);
	void UpdateLobbyReputation();
	void OnScoreboardStateChanged(std::string eventName); // Hook Handler

	// Security
	std::string GetAPISecret();
	std::string GenerateSignature(std::string apiKey, std::string matchGuid, std::string timestamp);

	// Helpers
	std::string SanitizeJsonString(const std::string& str);
	void SaveConfig();

	// GUI
	void RenderSettings() override;
	std::string GetPluginName() override;
	void SetImGuiContext(uintptr_t ctx) override;

	void Render(CanvasWrapper canvas);
	void RenderReputationOverlay(CanvasWrapper& canvas);

	// Updater
	void UpdateCheckLoop();
	void CheckForUpdates();
	void InstallUpdate();
	bool IsVersionNewer(std::string current, std::string server);

	// Heartbeat
	void SendHeartbeat();
	void HeartbeatLoop();

	// Seekr Users on Scoreboard
	std::string baseUrl;

	// state
	std::unordered_map<std::string, bool> seekrUsers;

	// core flow
	void UpdateSeekrUsersIngame(ServerWrapper& server, const std::string& key);

	bool IsSeekrUser(const std::string& platformId) const;

	// rendering
	void RenderSeekr(CanvasWrapper canvas);
	bool IsScoreboardOpen() const;

	// scoreboard math – you implement using PlatformDisplay logic
	bool GetRowRectForPRI(PriWrapper pri, ScoreboardRowRect& outRect) const;
	Vector2 GetSeekrIconPos(const ScoreboardRowRect& row, float& outSize) const;
};