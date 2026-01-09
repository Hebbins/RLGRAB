#include "pch.h"
#include "Seekr.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <filesystem>
#include <memory>
#include <chrono>

// Windows Crypto
#define NOMINMAX
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib") 

std::shared_ptr<CVarManagerWrapper> _globalCvarManager;

BAKKESMOD_PLUGIN(Seekr, "Seekr", plugin_version, PLUGINTYPE_FREEPLAY)

void Seekr::onLoad()
{
	_globalCvarManager = cvarManager;

	// 1. CVars
	cvarManager->registerCvar("seekr_api_key", "", "Your Seekr API Key", false)
		.addOnValueChanged([this](std::string oldValue, CVarWrapper cvar) {
		api_key = cvar.getStringValue();
			});

	cvarManager->registerCvar("seekr_chat_enabled", "1", "Enable Seekr Chat Overlay", false);

	cvarManager->registerCvar("seekr_debug_enabled", "0", "Enable Seekr Debug Functions", false);

	// 2. Config
	cvarManager->executeCommand("exec seekr.cfg");

	// 3. Match Events
	gameWrapper->HookEvent("Function TAGame.GameEvent_Soccar_TA.EventMatchEnded", std::bind(&Seekr::OnMatchEnded, this, std::placeholders::_1));
	gameWrapper->HookEvent("Function TAGame.GameEvent_Soccar_TA.PostBeginPlay", std::bind(&Seekr::StartLiveUpdateLoop, this, std::placeholders::_1));
	gameWrapper->HookEventWithCaller<ActorWrapper>(
		"Function TAGame.GFxData_GameEvent_TA.ShouldShowPodiumUI",
		[this](ActorWrapper caller, ...)
		{
			struct cGFxData_GameEvent_TA
			{
				unsigned char _obj[0x108];
				void* ServerName_Data;
				unsigned char _ServerNameCount[0x8];
				void* GamemodeName_Data;
				unsigned char _GamemodeNameCount[0x8];
				unsigned char _pad[0x20];
				int32_t SeriesGame;
			};
			cGFxData_GameEvent_TA* c = (cGFxData_GameEvent_TA*)caller.memory_address;
			UnrealStringWrapper ServerName((uintptr_t)&c->ServerName_Data);
			UnrealStringWrapper GamemodeName((uintptr_t)&c->GamemodeName_Data);

			//LOG("ServerName: [{}] | GamemodeName: [{}] | SeriesGame: {}", ServerName.ToString(), GamemodeName.ToString(), c->SeriesGame);

			GameModeTitle = GamemodeName.ToString();
			ServerTitle = ServerName.ToString();
			sSeriesGame = c->SeriesGame;

		});

	// 4. Reputation Events
	gameWrapper->HookEvent("Function TAGame.GameEvent_TA.EventPlayerAdded", std::bind(&Seekr::OnLobbyEvent, this, std::placeholders::_1));
	gameWrapper->HookEvent("Function TAGame.GameEvent_TA.EventPlayerRemoved", std::bind(&Seekr::OnLobbyEvent, this, std::placeholders::_1));

	// 5. Scoreboard State Hooks
	gameWrapper->HookEvent("Function TAGame.GFxData_GameEvent_TA.OnOpenScoreboard", std::bind(&Seekr::OnScoreboardStateChanged, this, "Open"));
	gameWrapper->HookEvent("Function TAGame.GFxData_GameEvent_TA.OnCloseScoreboard", std::bind(&Seekr::OnScoreboardStateChanged, this, "Close"));

	//gameWrapper->RegisterDrawable(std::bind(&Seekr::RenderSeekr, this, std::placeholders::_1));

	// 6. GUI
	//gameWrapper->RegisterDrawable(std::bind(&Seekr::Render, this, std::placeholders::_1));

	// 7. Delayed Init
	gameWrapper->SetTimeout([this](GameWrapper* gw) {
		DelayedInit();
		}, 1.5f);
}

void Seekr::DelayedInit() {
	cvarManager->executeCommand("plugin unload SeekrUpdater");
	api_key = cvarManager->getCvar("seekr_api_key").getStringValue();
	if (api_key.length() > 10) CheckUserProfile();

	//ChatLoop();

	UpdateCheckLoop();
	HeartbeatLoop();
}

void Seekr::sDebugLog(const std::string& logMsg) const
{
	if (!(user_role == "user"))
	{
		bool debug_enabled = cvarManager->getCvar("seekr_debug_enabled").getBoolValue();
		if (debug_enabled)
		{
			cvarManager->log("[Seekr Debugger]: " + logMsg);
		}
	}
}
static int GetRegionId(const std::string& region)
{
	if (region.size() < 2)
		return -1; // unknown / invalid

	// Handle 2-letter "ME" explicitly
	if (region.rfind("ME", 0) == 0) // starts with "ME"
		return 6;

	// Use first 3 chars for the rest
	if (region.size() < 3)
		return -1;

	std::string key = region.substr(0, 3);

	if (key == "USE") return 0;
	if (key == "EU-") return 1;   // if your EU prefix is "EU-" adjust as needed
	if (key == "USW") return 2;
	if (key == "ASC") return 3;
	if (key == "ASM") return 4;
	if (key == "JPN") return 5;
	if (key == "OCE") return 7;
	if (key == "SAF") return 8;
	if (key == "SAM") return 9;
	if (key == "IND") return 10;

	return -1; // unknown
}


static std::string RegionToString(int region)
{
	switch (region)
	{
	case 0:  return "US-East";
	case 1:  return "Europe";
	case 2:  return "US-West";
	case 3:  return "Asia-SE Mainland";
	case 4:  return "Asia-SE Martitime";
	case 5:  return "Asia-East";
	case 6:  return "Middle East";
	case 7:  return "Oceania";
	case 8:  return "South Africa";
	case 9:  return "South America";
	case 10:  return "India";
	default: return "Unknown";
	}
}

int Seekr::GetCurrentMaxTeamSize()
{
	// You can optionally skip the IsInGame checks if they’re giving you trouble in privates

	// Best entry point for most matches (ranked, privates, customs, etc.)
	ServerWrapper server = gameWrapper->GetGameEventAsServer();
	if (server.IsNull())
	{
		// Fallbacks for other contexts
		server = gameWrapper->GetOnlineGame();
		if (server.IsNull())
			server = gameWrapper->GetCurrentGameState();
	}

	if (server.IsNull())
		return -1; // could not determine

	TeamGameEventWrapper game(server.memory_address);
	if (game.IsNull())
		return -1;

	return game.GetMaxTeamSize();
}

int Seekr::GetTeamScore(int teamIndex)
{
	// Basic sanity check
	if (teamIndex < 0)
		return -1;

	// Treat any actual match context as valid
	if (!gameWrapper->IsInOnlineGame() &&
		!gameWrapper->IsInCustomTraining() &&
		!gameWrapper->IsInFreeplay() &&
		!gameWrapper->IsInReplay() &&
		!gameWrapper->IsInGame())
	{
		return -1;
	}

	// Prefer game event as server (works well for privates/customs)
	ServerWrapper server = gameWrapper->GetGameEventAsServer();
	if (server.IsNull())
	{
		// Fallbacks
		server = gameWrapper->GetOnlineGame();
		if (server.IsNull())
			server = gameWrapper->GetCurrentGameState();
	}

	if (server.IsNull())
		return -1;

	TeamGameEventWrapper game(server.memory_address);
	if (game.IsNull())
		return -1;

	auto teams = game.GetTeams();
	if (teams.Count() <= teamIndex)
		return -1;

	TeamWrapper team = teams.Get(teamIndex);
	if (team.IsNull())
		return -1;

	return team.GetScore();
}

void Seekr::ChatLoop() {
	// ADD THIS CHECK:
	bool enabled = cvarManager->getCvar("seekr_chat_enabled").getBoolValue();
	if (enabled) {
		GetChat();
	}

	// Re-run this function in 5 seconds
	gameWrapper->SetTimeout([this](GameWrapper* gw) {
		ChatLoop();
		}, 5.0f);
}

void Seekr::GetChat() {
	CurlRequest req;
	req.url = API_URL + "?action=get_chat";
	req.verb = "GET";

	HttpWrapper::SendCurlRequest(req, [this](int code, std::string result) {
		gameWrapper->Execute([this, code, result](GameWrapper* gw) {
			if (code != 200) return;

			// Start parsing after "data"
			size_t currentPos = result.find("\"data\"");
			if (currentPos == std::string::npos) return;

			while (true) {
				// 1. Find ID
				size_t idKeyPos = result.find("\"id\"", currentPos);
				if (idKeyPos == std::string::npos) break;

				// Extract ID
				size_t idColon = result.find(":", idKeyPos);
				size_t idQuoteStart = result.find("\"", idColon);
				if (idQuoteStart == std::string::npos) break;

				size_t idQuoteEnd = idQuoteStart + 1;
				while (idQuoteEnd < result.length()) {
					if (result[idQuoteEnd] == '"' && result[idQuoteEnd - 1] != '\\') break;
					idQuoteEnd++;
				}
				std::string id = result.substr(idQuoteStart + 1, idQuoteEnd - (idQuoteStart + 1));

				// 2. Find User
				size_t userKeyPos = result.find("\"user\"", idQuoteEnd);
				if (userKeyPos == std::string::npos) break;

				size_t userColon = result.find(":", userKeyPos);
				size_t userQuoteStart = result.find("\"", userColon);

				size_t userQuoteEnd = userQuoteStart + 1;
				while (userQuoteEnd < result.length()) {
					if (result[userQuoteEnd] == '"' && result[userQuoteEnd - 1] != '\\') break;
					userQuoteEnd++;
				}
				std::string user = result.substr(userQuoteStart + 1, userQuoteEnd - (userQuoteStart + 1));

				// 3. Find Message
				size_t msgKeyPos = result.find("\"message\"", userQuoteEnd);
				if (msgKeyPos == std::string::npos) break;

				size_t msgColon = result.find(":", msgKeyPos);
				size_t msgQuoteStart = result.find("\"", msgColon);

				size_t msgQuoteEnd = msgQuoteStart + 1;
				while (msgQuoteEnd < result.length()) {
					if (result[msgQuoteEnd] == '"' && result[msgQuoteEnd - 1] != '\\') break;
					msgQuoteEnd++;
				}
				std::string message = result.substr(msgQuoteStart + 1, msgQuoteEnd - (msgQuoteStart + 1));

				// 4. Check Cache and Log
				if (seenMessageIds.find(id) == seenMessageIds.end()) {
					seenMessageIds.insert(id);

					// Add to render list
					ChatMessage newMsg;
					newMsg.user = user;
					newMsg.message = message;
					newMsg.receivedTime = std::chrono::steady_clock::now();

					chatMessages.push_back(newMsg);

					while (chatMessages.size() > 30) {
						chatMessages.erase(chatMessages.begin());
					}

					// Still log to console if you want
					//cvarManager->log("Seekr Chat - " + user + ": " + message);
				}

				// Advance
				currentPos = msgQuoteEnd;
			}
			});
		});
}

void Seekr::SendChatMessage(std::string message) {
	if (message.empty()) return;

	chatStatusMsg = "Sending...";

	if (api_key.length() < 10) {
		chatStatusMsg = "Error: Invalid API Key";
		return;
	}

	std::stringstream json;
	json << "{";
	json << "\"api_key\": \"" << api_key << "\",";
	json << "\"message\": \"" << SanitizeJsonString(message) << "\"";
	json << "}";

	CurlRequest req;
	req.url = API_URL + "?action=send_chat";
	req.verb = "POST";
	req.headers = { {"Content-Type", "application/json"} };
	req.body = json.str();

	HttpWrapper::SendCurlRequest(req, [this](int code, std::string result) {
		gameWrapper->Execute([this, code, result](GameWrapper* gw) {
			if (code == 200) {
				if (result.find("success") != std::string::npos) {
					chatStatusMsg = "Sent!";
					GetChat(); // Refresh immediately
				}
				else if (result.find("Cooldown") != std::string::npos) {
					chatStatusMsg = "Cooldown (5s)";
				}
				else if (result.find("Duplicate") != std::string::npos) {
					chatStatusMsg = "Duplicate Message";
				}
				else {
					chatStatusMsg = "Error sending message";
				}
			}
			else {
				chatStatusMsg = "HTTP Error: " + std::to_string(code);
			}
			});
		});
}

void Seekr::onUnload() {}

void Seekr::SaveConfig()
{
	try {
		auto path = gameWrapper->GetBakkesModPath() / "cfg" / "seekr.cfg";
		std::ofstream file(path);
		if (file.is_open()) {
			file << "seekr_api_key \"" << api_key << "\"" << std::endl;
			file.close();
			//cvarManager->log("Seekr: Config saved.");
		}
	}
	catch (...) {}
}

// --- SECURITY: HMAC GENERATION ---

std::string Seekr::GetAPISecret() {
	unsigned char encrypted[] = {
		0x1E, 0x6C, 0x15, 0x2D, 0x76, 0x38, 0x67, 0x71,
		0x19, 0x60, 0x73, 0x2F, 0x04, 0x6D, 0x74, 0x25
	};

	std::string secret;
	for (unsigned char c : encrypted) { secret += (char)(c ^ 0x55); }
	return secret;
}

std::string Seekr::GenerateSignature(std::string apiKey, std::string matchGuid, std::string timestamp) {
	std::string secret = GetAPISecret();
	std::string payload = apiKey + matchGuid + timestamp + secret;
	HCRYPTPROV hProv = 0; HCRYPTHASH hHash = 0; BYTE rgbHash[32]; DWORD cbHash = 32; std::ostringstream oss;
	if (CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
		if (CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
			if (CryptHashData(hHash, (BYTE*)payload.c_str(), (DWORD)payload.length(), 0)) {
				if (CryptGetHashParam(hHash, HP_HASHVAL, rgbHash, &cbHash, 0)) {
					for (DWORD i = 0; i < cbHash; i++) { oss << std::hex << std::setw(2) << std::setfill('0') << (int)rgbHash[i]; }
				}
			}
			CryptDestroyHash(hHash);
		}
		CryptReleaseContext(hProv, 0);
	}
	return oss.str();
}

// --- REPUTATION LOGIC ---

void Seekr::OnScoreboardStateChanged(std::string eventName) {
	if (eventName == "Open") {
		isScoreboardOpen = true;
	}
	else {
		isScoreboardOpen = false;
	}
}

void Seekr::OnLobbyEvent(std::string eventName) {
	//gameWrapper->SetTimeout([this](GameWrapper* gw) { UpdateLobbyReputation(); }, 2.5f);

}

void Seekr::UpdateLobbyReputation() {
	if (!gameWrapper->IsInOnlineGame()) return;
	ServerWrapper server = gameWrapper->GetCurrentGameState();
	if (!server) return;
	ArrayWrapper<PriWrapper> players = server.GetPRIs();
	if (players.Count() == 0) return;

	std::stringstream idsJson;
	idsJson << "{ \"ids\": [";
	bool first = true;
	int validPlayers = 0;
	repCache.clear(); nameCache.clear(); teamCache.clear();

	for (int i = 0; i < players.Count(); i++) {
		PriWrapper p = players.Get(i);
		if (!p) continue;
		std::string pid = p.GetUniqueIdWrapper().GetIdString();
		std::string name = p.GetPlayerName().ToString();
		int team = p.GetTeamNum();
		if (pid.empty() || pid == "0" || pid == "0|0") continue;

		nameCache[pid] = name; repCache[pid] = 0; teamCache[pid] = team;
		if (!first) idsJson << ",";
		idsJson << "\"" << SanitizeJsonString(pid) << "\"";
		first = false;
		validPlayers++;
	}
	idsJson << "] }";

	if (validPlayers == 0) return;

	CurlRequest req; req.url = API_URL + "?action=get_batch_reputation"; req.verb = "POST"; req.body = idsJson.str();
	req.headers = { {"Content-Type", "application/json"} };

	HttpWrapper::SendCurlRequest(req, [this](int code, std::string result) {
		gameWrapper->Execute([this, result, code](GameWrapper* gw) {
			if (code != 200) return;
			for (auto const& [pid, name] : nameCache) {
				std::string searchKey = "\"" + pid + "\"";
				size_t pos = result.find(searchKey);
				if (pos != std::string::npos) {
					size_t colonPos = result.find(":", pos + searchKey.length());
					if (colonPos != std::string::npos) {
						size_t valStart = colonPos + 1;
						while (valStart < result.length() && isspace(result[valStart])) valStart++;
						if (valStart < result.length() && result[valStart] == '"') valStart++;
						size_t valEnd = valStart;
						if (valEnd < result.length() && result[valEnd] == '-') valEnd++;
						while (valEnd < result.length() && isdigit(result[valEnd])) valEnd++;
						try {
							std::string scoreStr = result.substr(valStart, valEnd - valStart);
							if (!scoreStr.empty() && scoreStr != "null") {
								int score = std::stoi(scoreStr);
								repCache[pid] = score;
							}
						}
						catch (...) {}
					}
				}
			}
			});
		});
}

// --- MISC ---

std::string Seekr::SanitizeJsonString(const std::string& str) {
	std::string out;
	for (unsigned char c : str) {
		if (c == '"') out += "\\\"";
		else if (c == '\\') out += "\\\\";
		else if (c < 32) {}
		else out += c;
	}
	return out;
}

void Seekr::CheckUserProfile()
{
	if (api_key.length() != 16) {
		if (!api_key.empty())
			upload_status_msg = "Invalid Key Length";
		return;
	}

	upload_status_msg = "Checking...";

	CurlRequest req;
	req.url = API_URL + "?action=get_current_user&api_key_query=" + api_key;
	req.verb = "GET";

	HttpWrapper::SendCurlRequest(req, [this](int code, std::string result)
		{
			gameWrapper->Execute([this, code, result](GameWrapper* gw)
				{
					if (code == 200)
					{
						// --- Parse upload_count (existing logic) ---
						{
							const std::string key = "\"upload_count\":";
							size_t pos = result.find(key);
							if (pos != std::string::npos)
							{
								size_t start = pos + key.length();
								size_t end = result.find(",", start);
								if (end == std::string::npos)
									end = result.find("}", start);

								try
								{
									std::string countStr = result.substr(start, end - start);
									countStr.erase(std::remove_if(countStr.begin(), countStr.end(),
										[](unsigned char c) { return !std::isdigit(c); }), countStr.end());
									if (!countStr.empty())
										total_user_uploads = std::stoi(countStr);

									upload_status_msg = "Connected";
								}
								catch (...)
								{
									upload_status_msg = "Parse Error";
									user_role = "Unknown";
								}
							}
							else
							{
								if (result.find("not_logged_in") != std::string::npos)
								{	
									upload_status_msg = "Invalid API Key";
									user_role = "Unknown";
								}
								else
									upload_status_msg = "Connected (No stats)";
							}
						}

						// --- Parse status ---
						std::string statusValue;
						{
							const std::string statusKey = "\"Status\":";
							size_t pos = result.find(statusKey);
							if (pos != std::string::npos)
							{
								size_t start = result.find_first_of("\"", pos + statusKey.length());
								if (start != std::string::npos)
								{
									size_t end = result.find_first_of("\"", start + 1);
									if (end != std::string::npos)
										statusValue = result.substr(start + 1, end - start - 1);
								}
							}
						}

						// --- Parse role ---
						{
							const std::string roleKey = "\"role\":";
							size_t pos = result.find(roleKey);
							if (pos != std::string::npos)
							{
								size_t start = result.find_first_of("\"", pos + roleKey.length());
								if (start != std::string::npos)
								{
									size_t end = result.find_first_of("\"", start + 1);
									if (end != std::string::npos)
										user_role = result.substr(start + 1, end - start - 1);
								}
							}
						}

						// --- Enforce active status ---
						if (!statusValue.empty() && statusValue != "active")
						{
							api_key.clear();                        // api_key = null;
							upload_status_msg = "Account not active";
							user_role = "Unknown";

							// Unload Seekr (replace "Seekr" with your actual plugin filename if different)
							//cvarManager->executeCommand("plugin unload Seekr");

							return;
						}
					}
					else
					{
						upload_status_msg = "Connection Failed";
						user_role = "Unknown";
					}
				});
		});
}


bool Seekr::isGamemodeHeatseeker() const
{
	std::string lower = GameModeTitle;
	std::transform(lower.begin(), lower.end(), lower.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	sDebugLog("Gamemode: " + GameModeTitle);
	sDebugLog("Series Game: " + sSeriesGame);
	return lower.find("heatseeker") != std::string::npos;
}

void Seekr::StartLiveUpdateLoop(std::string eventName) {
	is_live_tracking = true;

	timeCounter = 0;

	gameWrapper->SetTimeout([this](GameWrapper* gw) {
		LiveUpdateLoop();
		}, 5.0f);
}

void Seekr::StopLiveUpdateLoop() {
	is_live_tracking = false;
	sDebugLog("Stopped Updating ###");
}

void Seekr::LiveUpdateLoop() {
	if (!is_live_tracking) return;
	if (!gameWrapper->IsInOnlineGame()) {
		is_live_tracking = false;
		return;
	}

	std::string currentKey = cvarManager->getCvar("seekr_api_key").getStringValue();
	if (currentKey.length() != 16) return;

	ServerWrapper server = gameWrapper->GetCurrentGameState();
	if (!server) return;

	// --- EXACT SAME CHECKS AS OnMatchEnded ---
	auto playlist = server.GetPlaylist();
	if (!playlist) {
		gameWrapper->SetTimeout([this](GameWrapper* gw) { LiveUpdateLoop(); }, 5.0f);
		return;
	}

	int playlistId = playlist.GetPlaylistId();
	bool shouldUpload = false;

	// 1. Private Match
	if (playlistId == 6) {
		shouldUpload = true;
	}
	else {
		// 2. Public Match Detection
		std::string title = playlist.GetTitle().ToString();
		std::string titleLower = title;
		std::transform(titleLower.begin(), titleLower.end(), titleLower.begin(), ::tolower);

		bool isHeatseeker = (titleLower.find("heatseeker") != std::string::npos);
		bool isRicochet = (titleLower.find("ricochet") != std::string::npos);
		bool isSplitshot = (titleLower.find("splitshot") != std::string::npos) || (titleLower.find("split shot") != std::string::npos);
		bool isTournament = (titleLower.find("tournament") != std::string::npos);
		//cvarManager->log("Seekr Trace: Playlist ID: " + std::to_string(playlistId) + " | Title: " + titleLower);
		if (!isHeatseeker && !isRicochet && !isSplitshot && !isTournament) {
			return;
		}
		shouldUpload = true;
	}

	if (shouldUpload) {
		// Send LIVE status
		UploadMatchData(server, currentKey, "LIVE");
	}

	// Schedule next loop in 5 seconds
	gameWrapper->SetTimeout([this](GameWrapper* gw) {
		LiveUpdateLoop();
		}, 5.0f);
}

void Seekr::OnMatchEnded(std::string eventName)
{
	// STOP THE LIVE LOOP
	StopLiveUpdateLoop();

	if (update_available) return;
	std::string currentKey = cvarManager->getCvar("seekr_api_key").getStringValue();
	if (currentKey.length() != 16) return;
	if (!gameWrapper->IsInOnlineGame()) return;

	ServerWrapper server = gameWrapper->GetCurrentGameState();
	if (!server) return;
	auto playlist = server.GetPlaylist();
	if (!playlist) return;

	// 1. Check Private Match First
	int playlistId = playlist.GetPlaylistId();
	if (playlistId == 6 && isGamemodeHeatseeker()) {
		gameWrapper->SetTimeout([this, currentKey](GameWrapper* gw) {
			ServerWrapper sw = gw->GetCurrentGameState();
			// Pass "FINISHED"
			if (sw) UploadMatchData(sw, currentKey, "FINISHED");
			}, 1.0f);
		return;
	}

	// 2. Public Match Detection
	std::string title = playlist.GetTitle().ToString();
	std::string titleLower = title;
	std::transform(titleLower.begin(), titleLower.end(), titleLower.begin(), ::tolower);

	bool isHeatseeker = (titleLower.find("heatseeker") != std::string::npos);
	bool isRicochet = (titleLower.find("ricochet") != std::string::npos);
	bool isSplitshot = (titleLower.find("splitshot") != std::string::npos) || (titleLower.find("split shot") != std::string::npos);
	bool isTournament = (titleLower.find("tournament") != std::string::npos) && isGamemodeHeatseeker();

	if (!isHeatseeker && !isRicochet && !isSplitshot && !isTournament) {
		sDebugLog("Match ignored (Not a supported mode).");
		return;
	}

	gameWrapper->SetTimeout([this, currentKey](GameWrapper* gw) {
		ServerWrapper sw = gw->GetCurrentGameState();
		// Pass "FINISHED"
		if (sw) UploadMatchData(sw, currentKey, "FINISHED");
		}, 1.0f);
}

void Seekr::UploadMatchData(ServerWrapper& server, std::string key, std::string matchStatus) {
	sDebugLog("UploadMatchData Called. Match Status: " + matchStatus);
	try {
		PlayerControllerWrapper player = gameWrapper->GetPlayerController();
		if (!player) {
			//cvarManager->log("Seekr Trace: FAIL - No PlayerController");
			return;
		}

		PriWrapper localPri = player.GetPRI();
		if (!localPri) {
			//cvarManager->log("Seekr Trace: FAIL - No local PRI");
			return;
		}

		std::string uploaderPlatformId = localPri.GetUniqueIdWrapper().GetIdString();
		auto playlist = server.GetPlaylist();

		// Even if we allow LIVE updates without a playlist later, 
		// your current code relies on it for matchType logic.
		if (!playlist) {
			//cvarManager->log("Seekr Trace: FAIL - No PlaylistWrapper (Wait for sync)");
			return;
		}

		std::string matchType = "CASUAL";
		std::string mapName = gameWrapper->GetCurrentMap();
		int teamSize = 0;
		bool isSplitshot = false;
		bool isRicochet = false;
		bool isPrivate = false;
		std::string ServerName = ServerTitle;
		std::string region = RegionToString(GetRegionId(ServerName));
		bool isRanked = playlist.GetbRanked();
		int playlistId = playlist.GetPlaylistId();
		float totalTimePlayed = server.GetSecondsElapsed();
		
		if (timeCounter < totalTimePlayed && timeCounter < (totalTimePlayed + 10))
		{
			timeCounter = totalTimePlayed;
		}

		sDebugLog("Time Played" + std::to_string(totalTimePlayed));

		std::string pTitle = playlist.GetTitle().ToString();
		std::string pTitleLower = pTitle;
		std::transform(pTitleLower.begin(), pTitleLower.end(), pTitleLower.begin(), ::tolower);

		//cvarManager->log("Seekr Trace: Playlist ID: " + std::to_string(playlistId) + " | Title: " + pTitleLower);

		// --- MASTER FILTER LOGIC ---
		bool isHeatseekerBase = (pTitleLower.find("heatseeker") != std::string::npos);
		isPrivate = ((playlistId == 6) && isGamemodeHeatseeker());
		bool isTournament = (pTitleLower.find("tournament") != std::string::npos) && isGamemodeHeatseeker();

		if (!isHeatseekerBase && !isPrivate && !isTournament && !isTournament) {
			sDebugLog("Upload Aborting - Not Heatseeker");
			return;
		}

		// Categorize the specific Heatseeker flavor
		if (isPrivate) {
			matchType = "PRIVATE";
		}
		else if (pTitleLower.find("tournament") != std::string::npos) {
			matchType = "TOURNAMENT";
			//cvarManager->log("Tournament Title Found: " + lastTourTitle);
		}
		else if (pTitleLower.find("ricochet") != std::string::npos) {
			matchType = "RICOCHET";
			isRicochet = true;
		}
		else if (pTitleLower.find("splitshot") != std::string::npos || pTitleLower.find("split shot") != std::string::npos) {
			matchType = "SPLITSHOT";
			isSplitshot = true;
		}
		else if (isRanked) {
			matchType = "COMPETITIVE";
		}
		else {
			matchType = "CASUAL";
		}

		//cvarManager->log("Seekr Trace: Identified as " + matchType);

		// Player count logic
		ArrayWrapper<PriWrapper> allPris = server.GetPRIs();
		int activePlayerCount = 0;
		for (int i = 0; i < allPris.Count(); i++) {
			PriWrapper p = allPris.Get(i);
			if (p && (p.GetTeamNum() == 0 || p.GetTeamNum() == 1)) activePlayerCount++;
		}
		teamSize = (activePlayerCount > 0) ? (activePlayerCount + 1) / 2 : 1;

		// JSON Construction
		std::stringstream json;
		json << "{";
		json << "\"api_key\": \"" << key << "\",";
		json << "\"match_guid\": \"" << server.GetMatchGUID() << "\",";
		json << "\"match_status\": \"" << matchStatus << "\",";
		json << "\"uploader_platform_id\": \"" << SanitizeJsonString(uploaderPlatformId) << "\",";
		json << "\"match_type\": \"" << matchType << "\",";
		json << "\"team_size\": " << GetCurrentMaxTeamSize() << ",";
		json << "\"blue_goals\": " << GetTeamScore(0) << ",";
		json << "\"orange_goals\": " << GetTeamScore(1) << ",";
		json << "\"is_private\": " << (isPrivate ? "1" : "0") << ",";
		json << "\"is_splitshot\": " << (isSplitshot ? "1" : "0") << ",";
		json << "\"is_ricochet\": " << (isRicochet ? "1" : "0") << ",";
		json << "\"server_region\": \"" << SanitizeJsonString(region) << "\",";
		json << "\"map_name\": \"" << SanitizeJsonString(mapName) << "\",";
		json << "\"time_played\": \"" << timeCounter << "\",";
		json << "\"plugin_version\": \"" << CURRENT_PLUGIN_VERSION << "\",";
		json << "\"players\": [";

		ArrayWrapper<PriWrapper> players = server.GetPRIs();
		ArrayWrapper<TeamWrapper> teams = server.GetTeams();
		MMRWrapper mmrWrapper = gameWrapper->GetMMRWrapper();

		bool first = true;
		for (int i = 0; i < players.Count(); i++) {
			PriWrapper pri = players.Get(i);
			if (!pri || pri.GetTeamNum() > 1) continue;

			std::string pid = pri.GetUniqueIdWrapper().GetIdString();
			std::string name = pri.GetPlayerName().ToString();

			float playerMmr = mmrWrapper.GetPlayerMMR(pri.GetUniqueIdWrapper(), playlistId);
			int tournRank = 0, compRank = 0;
			if (matchType == "TOURNAMENT" || matchType == "COMPETITIVE") {
				SkillRank rank = mmrWrapper.GetPlayerRank(pri.GetUniqueIdWrapper(), playlistId);
				if (matchType == "TOURNAMENT") tournRank = rank.Tier;
				else compRank = rank.Tier;
			}

			bool isWinner = false;
			bool isUploader = (pid == uploaderPlatformId);
			if (teams.Count() >= 2) {
				int team = pri.GetTeamNum();
				int myScore = teams.Get(team).GetScore();
				int otherScore = teams.Get(team == 0 ? 1 : 0).GetScore();
				isWinner = (myScore > otherScore);
			}

			if (!first) json << ",";
			json << "{ \"platform_id\": \"" << SanitizeJsonString(pid) << "\", \"name\": \"" << SanitizeJsonString(name) << "\", \"team\": \"" << (pri.GetTeamNum() == 0 ? "blue" : "orange") << "\", \"is_winner\": " << (isWinner ? "true" : "false") << ", \"is_uploader\": " << (isUploader ? "true" : "false") << ", \"tourn_rank\": " << tournRank << ", \"comp_rank\": " << compRank << ", \"mmr\": " << playerMmr << ", \"score\": " << pri.GetMatchScore() << ", \"goals\": " << pri.GetMatchGoals() << ", \"assists\": " << pri.GetMatchAssists() << ", \"saves\": " << pri.GetMatchSaves() << "}";
			first = false;
		}
		json << "]}";

		std::string payload = json.str();
		//cvarManager->log("Seekr Trace: Payload Ready (" + std::to_string(payload.length()) + " bytes). Sending POST...");

		auto now = std::chrono::system_clock::now();
		std::string timestamp = std::to_string(std::chrono::system_clock::to_time_t(now));
		std::string signature = GenerateSignature(key, server.GetMatchGUID(), timestamp);

		CurlRequest req;
		req.url = API_URL + "?action=submit_match";
		req.verb = "POST";
		req.headers = {
			{"Content-Type", "application/json"},
			{"X-Seekr-Time", timestamp},
			{"X-Seekr-Sig", signature}
		};
		req.body = payload;

		HttpWrapper::SendCurlRequest(req, [this, matchStatus](int code, std::string result) {
			gameWrapper->Execute([this, code, result, matchStatus](GameWrapper* gw) {
				//cvarManager->log("Seekr HTTP Code: " + std::to_string(code) + " | Status: " + matchStatus);
				if (code == 200 && result.find("success") != std::string::npos) {
					upload_status_msg = "Synced (" + matchStatus + ")";
				}
				else {
					//cvarManager->log("Seekr API Resp: " + result);
				}
				});
			});

	}
	catch (const std::exception& e) {
		//cvarManager->log("Seekr Trace: CRASH in UploadMatchData: " + std::string(e.what()));
	}
	catch (...) {
		//cvarManager->log("Seekr Trace: Unknown CRASH in UploadMatchData");
	}
}

void Seekr::UpdateCheckLoop() {
	CheckForUpdates();
	gameWrapper->SetTimeout([this](GameWrapper* gw) { UpdateCheckLoop(); }, 300.0f);
}

void Seekr::CheckForUpdates() {
	if (is_updating) return;
	if (gameWrapper->IsInOnlineGame()) return;

	CurlRequest req; req.url = API_URL + "?action=check_version"; req.verb = "GET";
	HttpWrapper::SendCurlRequest(req, [this](int code, std::string result) {
		gameWrapper->Execute([this, code, result](GameWrapper* gw) {
			if (code == 200) {
				std::string key = "\"latest_version\":\"";
				size_t pos = result.find(key);
				if (pos != std::string::npos) {
					size_t start = pos + key.length();
					size_t end = result.find("\"", start);
					std::string serverVer = result.substr(start, end - start);
					if (IsVersionNewer(CURRENT_PLUGIN_VERSION, serverVer)) {
						//cvarManager->log("Seekr: Update found (" + serverVer + ").");
						InstallUpdate();
					}
				}
			}
			});
		});
}

void Seekr::InstallUpdate() {
	if (is_updating) return;
	is_updating = true;
	upload_status_msg = "Seekr: Update Found - Auto-Updating...";
	std::string pluginsDir = gameWrapper->GetBakkesModPath().string() + "\\plugins";
	std::string updaterPath = pluginsDir + "\\SeekrUpdater.dll";

	if (!std::filesystem::exists(updaterPath)) {
		//cvarManager->log("Seekr: Updater not found. Downloading...");
		CurlRequest req; req.url = "https://seekrstats.com/updates/SeekrUpdater.dll"; req.verb = "GET";
		HttpWrapper::SendCurlRequest(req, [this, updaterPath](int code, std::string result) {
			gameWrapper->Execute([this, code, result, updaterPath](GameWrapper* gw) {
				if (code == 200 && result.size() > 0) {
					std::ofstream file(updaterPath, std::ios::binary);
					if (file.is_open()) { file.write(result.data(), result.size()); file.close(); is_updating = false; InstallUpdate(); }
				}
				else { is_updating = false; }
				});
			});
		return;
	}
	//cvarManager->log("Seekr: Handing off to SeekrUpdater...");
	cvarManager->registerCvar("seekr_auto_update", "0", "State", true);
	cvarManager->getCvar("seekr_auto_update").setValue(1);
	cvarManager->executeCommand("plugin load SeekrUpdater");
}

bool Seekr::IsVersionNewer(std::string current, std::string server) {
	int v1a = 0, v1b = 0, v1c = 0; int v2a = 0, v2b = 0, v2c = 0;
	sscanf(current.c_str(), "%d.%d.%d", &v1a, &v1b, &v1c);
	sscanf(server.c_str(), "%d.%d.%d", &v2a, &v2b, &v2c);
	if (v2a > v1a) return true;
	if (v2a == v1a && v2b > v1b) return true;
	if (v2a == v1a && v2b == v1b && v2c > v1c) return true;
	return false;
}

void Seekr::HeartbeatLoop() {
	SendHeartbeat();
	gameWrapper->SetTimeout([this](GameWrapper* gw) { HeartbeatLoop(); }, 180.0f);
}

void Seekr::SendHeartbeat() {
	if (api_key.length() != 16) return;
	CurlRequest req; req.url = API_URL + "?action=heartbeat&api=" + api_key + "&version=" + CURRENT_PLUGIN_VERSION; req.verb = "GET";
	HttpWrapper::SendCurlRequest(req, [this](int code, std::string result) {});
}