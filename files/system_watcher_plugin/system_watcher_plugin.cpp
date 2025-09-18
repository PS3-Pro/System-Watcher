#include "system_watcher_plugin.hpp"
#include <cell/cell_fs.h>
#include "Utils/Memory/Detours.hpp"
#include "Utils/Syscalls.hpp"
#include <algorithm>
#include <initializer_list>
#include <string>
#include <cstring>
#include <sys/sys_time.h>
#include <sys/prx.h>

using address_t = char[0x10];

__attribute__((noinline)) uint64_t PeekLv1(uint64_t addr)
{
	system_call_1(8, (uint64_t)addr);
	return_to_user_prog(uint64_t);
}

// ===== CLOCK =====
enum ClockState
{
	CLOCK_OVERCLOCK,
	CLOCK_STANDARD,
	CLOCK_UNDERCLOCK,
	CLOCK_BALANCED,
	CLOCK_ERROR
};

ClockState GetClockState()
{
	constexpr uint64_t GPU_CORE_CLOCK = 0x28000004028;
	constexpr uint64_t GPU_VRAM_CLOCK = 0x28000004010;

	auto readClock = [](uint64_t addr, int multiplier) -> int {
		union {
			struct { uint32_t junk0; uint8_t junk1, junk2, mul, junk3; };
			uint64_t value;
		} clock{};
		clock.value = PeekLv1(addr);
		return static_cast<int>(clock.mul) * multiplier;
	};

	int core_val = readClock(GPU_CORE_CLOCK, 50);
	int rsx_val = readClock(GPU_VRAM_CLOCK, 25);

	if (core_val <= 0 || rsx_val <= 0)
		return CLOCK_ERROR;

	if ((core_val > 500 && rsx_val < 650) || (core_val < 500 && rsx_val > 650))
		return CLOCK_BALANCED;

	if (core_val > 500 || rsx_val > 650)
		return CLOCK_OVERCLOCK;

	if (core_val == 500 && rsx_val == 650)
		return CLOCK_STANDARD;

	return CLOCK_UNDERCLOCK;
}

// ===== HELPERS =====
sys_prx_id_t GetModuleHandle(const char* moduleName) { return (moduleName) ? sys_prx_get_module_id_by_name(moduleName, 0, nullptr) : sys_prx_get_my_module_id(); }
sys_prx_module_info_t GetModuleInfo(sys_prx_id_t handle) { sys_prx_module_info_t info{}; static sys_prx_segment_info_t segments[10]{}; static char filename[SYS_PRX_MODULE_FILENAME_SIZE]{}; stdc::memset(segments, 0, sizeof(segments)); stdc::memset(filename, 0, sizeof(filename)); info.size = sizeof(info); info.segments = segments; info.segments_num = sizeof(segments) / sizeof(sys_prx_segment_info_t); info.filename = filename; info.filename_size = sizeof(filename); sys_prx_get_module_info(handle, 0, &info); return info; }
std::string GetModuleFilePath(const char* moduleName) { sys_prx_module_info_t info = GetModuleInfo(GetModuleHandle(moduleName)); return std::string(info.filename); }
std::string RemoveBaseNameFromPath(const std::string& filePath) { size_t lastPath = filePath.find_last_of("/"); if (lastPath == std::string::npos) return filePath; return filePath.substr(0, lastPath); }
std::string GetCurrentDir() { static std::string cachedModulePath; if (cachedModulePath.empty()) { std::string path = RemoveBaseNameFromPath(GetModuleFilePath(nullptr)); path += "/"; cachedModulePath = path; } return cachedModulePath; }
bool FileExists(const char* filePath) { CellFsStat stat; if (cellFsStat(filePath, &stat) == CELL_FS_SUCCEEDED) return (stat.st_mode & CELL_FS_S_IFREG); return false; }
bool ReadFile(const char* filePath, void* data, size_t size) { int fd; if (cellFsOpen(filePath, CELL_FS_O_RDONLY, &fd, nullptr, 0) == CELL_FS_SUCCEEDED) { cellFsLseek(fd, 0, CELL_FS_SEEK_SET, nullptr); cellFsRead(fd, data, size, nullptr); cellFsClose(fd); return true; } return false; }
bool ReplaceStr(std::wstring& str, const std::wstring& from, const std::string& to) { size_t startPos = str.find(from); if (startPos == std::wstring::npos) return false; str.replace(startPos, from.length(), std::wstring(to.begin(), to.end())); return true; }

bool IsIpTextEnabled()
{
	const char* toggleFilePath = "/dev_flash/vsh/resource/AAA/system_ip_plugin.sprx";
	int fd;

	if (cellFsOpen(toggleFilePath, CELL_FS_O_RDONLY, &fd, nullptr, 0) != CELL_FS_SUCCEEDED)
	{
		return false;
	}

	char toggleValue = '1';
	cellFsRead(fd, &toggleValue, 1, nullptr);
	cellFsClose(fd);

	return (toggleValue != '0');
}

// ===== GLOBALS =====
bool gIsDebugXmbPlugin{ false };
wchar_t gIpBuffer[512]{0};
paf::View* xmb_plugin{};
paf::View* system_plugin{};
paf::PhWidget* page_xmb_indicator{};
paf::PhWidget* page_notification{};
bool g_isIpTextDisabled = false;
ClockState g_cachedClockState = CLOCK_STANDARD;
uint64_t g_lastClockCheckTime = 0;
constexpr uint64_t CLOCK_CHECK_INTERVAL_US = 5000000;
std::wstring g_cachedIpText;
uint64_t g_lastIpTextCheckTime = 0;
constexpr uint64_t IP_TEXT_CHECK_INTERVAL_US = 3000000;
enum AnimationState { FADING_OUT, INVISIBLE, FADING_IN, VISIBLE };
AnimationState g_animationState = FADING_IN;
uint64_t g_animationStateChangeTime_us = 0;
constexpr uint64_t FADE_DURATION_US = 800000;
constexpr uint64_t VISIBLE_DURATION_US = 25000;
constexpr uint64_t INVISIBLE_DURATION_US = 600000;
float g_currentPulseAlpha = 0.0f;
bool g_is_hen = false;

// ===== GAMEBOOT GLOBALS  =====
bool g_gamebootAnimStarted = false;
uint64_t g_gamebootAnimStartTime_us = 0;
bool g_gamebootWidgetSeenThisFrame = false;
constexpr uint64_t GB_HOLD_DURATION_US = 3600000;
constexpr uint64_t GB_FADE_OUT_DURATION_US = 2000000;
constexpr uint64_t GB_FADE_OUT_START_TIME_US = GB_HOLD_DURATION_US;
constexpr uint64_t GB_ANIMATION_TOTAL_DURATION_US = GB_FADE_OUT_START_TIME_US + GB_FADE_OUT_DURATION_US;

bool LoadIpText()
{
	const char* ipTextPath = "/dev_flash/vsh/resource/explore/xmb/pro.xml";
	char fileBuffer[512]{0};
	if (!ReadFile(ipTextPath, fileBuffer, sizeof(fileBuffer)))
		return false;
	stdc::swprintf(gIpBuffer, 512, L"%s", fileBuffer);
	system_plugin = paf::View::Find("system_plugin");
	if (!system_plugin) return false;
	page_notification = system_plugin->FindWidget("page_notification");
	if (page_notification && page_notification->FindChild("ip_text", 0) != nullptr)
		gIsDebugXmbPlugin = true;
	return true;
}

paf::PhWidget* GetParent() { if (!page_xmb_indicator) return nullptr; return page_xmb_indicator->FindChild("indicator", 0); }
bool CanCreateIpText() { if (g_isIpTextDisabled) return false; paf::PhWidget* parent = GetParent(); return parent ? parent->FindChild("ip_text", 0) == nullptr : false; }

std::wstring GenerateIpText()
{
	std::wstring text(gIpBuffer);
	char ip[16]{0};
	netctl::netctl_main_9A528B81(16, ip);
	std::wstring systemIpAddress = L"System IP Address: ";
	std::wstring IpAddress = (strlen(ip) > 0) ? std::wstring(ip, ip + strlen(ip)) : L"0.0.0.0";
	std::wstring serverName;
	if (IpAddress != L"0.0.0.0") {
		xsetting_F48C0548_t* net = xsetting_F48C0548();
		if (net) {
			xsetting_F48C0548_t::net_info_t netInfo;
			net->GetNetworkConfig(&netInfo);
			std::wstring dnsPrimary(netInfo.primaryDns, netInfo.primaryDns + strlen(netInfo.primaryDns));
			std::wstring dnsSecondary(netInfo.secondaryDns, netInfo.secondaryDns + strlen(netInfo.secondaryDns));
			if (dnsPrimary == L"185.194.142.4" || dnsSecondary == L"185.194.142.4") serverName = L"PlayStation Online Network Emulated";
			else if (dnsPrimary == L"51.79.41.185" || dnsSecondary == L"51.79.41.185") serverName = L"PlayStation Online Returnal Games";
			else if (dnsPrimary == L"146.190.205.197" || dnsSecondary == L"146.190.205.197") serverName = L"PlayStation Reborn";
			else if (dnsPrimary == L"135.148.144.253" || dnsSecondary == L"135.148.144.253") serverName = L"PlayStation Rewired";
			else if (dnsPrimary == L"128.140.0.23" || dnsSecondary == L"128.140.0.23") serverName = L"Project Neptune";
			else if (dnsPrimary == L"45.7.228.197" || dnsSecondary == L"45.7.228.197") serverName = L"Open Spy";
			else if (dnsPrimary == L"142.93.245.186" || dnsSecondary == L"142.93.245.186") serverName = L"The ArchStones";
			else if (dnsPrimary == L"188.225.75.35" || dnsSecondary == L"188.225.75.35") serverName = L"WareHouse";
			else if (dnsPrimary == L"64.20.35.146" || dnsSecondary == L"64.20.35.146") serverName = L"Home Headquarters";
			else if (dnsPrimary == L"52.86.120.101" || dnsSecondary == L"52.86.120.101") serverName = L"Destination Home";
			else if (dnsPrimary == L"45.33.44.103" || dnsSecondary == L"45.33.44.103") serverName = L"Go Central";
			else if (dnsPrimary == L"198.100.158.95" || dnsSecondary == L"198.100.158.95") serverName = L"Warhawk Revived";
			else if (dnsPrimary == L"155.248.205.187" || dnsSecondary == L"155.248.202.187") serverName = L"Monster Hunter Frontier: Renewal";
			else if (dnsPrimary == L"209.74.81.7" || dnsSecondary == L"209.74.81.7") serverName = L"Rocket NET";
			else serverName = L"PlayStation™ Network";
		}
	}
	systemIpAddress += IpAddress;
	text += L"\n";
	if (!serverName.empty()) text += L"Online Server: " + serverName + L"\n";
	text += systemIpAddress;
	return text;
}

void CreateIpText()
{
	if (gIsDebugXmbPlugin)
		return;

	paf::PhWidget* parent = GetParent();
	if (!parent)
		return;

	paf::PhText* ip_text = new paf::PhText(parent, nullptr);
	if (!ip_text)
		return;

	ip_text->SetName("ip_text");
	ip_text->SetColor({ 1.f, 1.f, 1.f, 1.f });
	ip_text->SetStyle(19, 112);
	ip_text->SetLayoutPos(0x60000, 0x50000, 0, { 820.f, -465.f, 0.f, 0.f });
	ip_text->SetLayoutStyle(0, 20, 0.f);
	ip_text->SetLayoutStyle(1, 217, 0.f);
	ip_text->SetStyle(56, true);
	ip_text->SetStyle(18, 34);
	ip_text->SetStyle(49, 2);
}
// ===== HOOK =====
Detour* pafWidgetDrawThis_Detour;

int pafWidgetDrawThis_Hook(paf::PhWidget* _this, unsigned int r4, bool r5)
{
	sys_time_sec_t sec;
	sys_time_nsec_t nsec;
	sys_time_get_current_time(&sec, &nsec);
	uint64_t currentTime_us = static_cast<uint64_t>(sec)* 1000000ULL + nsec / 1000;

	if ((currentTime_us - g_lastIpTextCheckTime) > IP_TEXT_CHECK_INTERVAL_US) {
		g_cachedIpText = GenerateIpText();
		g_lastIpTextCheckTime = currentTime_us;
	}

	if (!g_is_hen && (currentTime_us - g_lastClockCheckTime) > CLOCK_CHECK_INTERVAL_US) {
		g_cachedClockState = GetClockState();
		g_lastClockCheckTime = currentTime_us;
	}

	if (_this)
	{
		const char* widgetName = _this->m_Data.name.c_str();

		if (strncmp(widgetName, "ip_text", 7) == 0)
		{
			paf::PhText* ip_text = (paf::PhText*)_this;
			paf::PhWidget* parent = GetParent();
			bool isParentVisible = parent && parent->m_Data.metaAlpha > 0.1f;

			if (g_isIpTextDisabled || vshmain::GetCooperationMode() == vshmain::CooperationMode::Game || !isParentVisible) {
				ip_text->m_Data.metaAlpha = 0.f;
			}
			else {
				ip_text->m_Data.metaAlpha = 1.f;
			}
			ip_text->SetText(g_cachedIpText, 0);
		}

		// ===== GAMEBOOT ANIMATION  =====
		if (!g_is_hen)
		{
			if (strncmp(widgetName, "enhanced_game_text", 18) == 0)
			{
				if (!g_gamebootAnimStarted)
				{
					g_cachedClockState = GetClockState();
				}

				bool shouldBeVisibleCondition = (g_cachedClockState == CLOCK_OVERCLOCK || g_cachedClockState == CLOCK_BALANCED);
				float currentAlpha = 0.0f;

				if (shouldBeVisibleCondition)
				{
					if (!g_gamebootAnimStarted)
					{
						g_gamebootAnimStarted = true;
						g_gamebootAnimStartTime_us = currentTime_us;
					}

					uint64_t elapsed_us = currentTime_us - g_gamebootAnimStartTime_us;

					if (elapsed_us < GB_FADE_OUT_START_TIME_US)
					{
						currentAlpha = 1.0f;
					}
					else if (elapsed_us < GB_ANIMATION_TOTAL_DURATION_US)
					{
						uint64_t fadeOut_elapsed_us = elapsed_us - GB_FADE_OUT_START_TIME_US;
						currentAlpha = 1.0f - (static_cast<float>(fadeOut_elapsed_us) / GB_FADE_OUT_DURATION_US);
					}
					else
					{
						currentAlpha = 0.0f;
						g_gamebootAnimStarted = false;
					}
				}
				else
				{
					g_gamebootAnimStarted = false;
					currentAlpha = 0.0f;
				}

				if (currentAlpha < 0.0f) currentAlpha = 0.0f;
				if (currentAlpha > 1.0f) currentAlpha = 1.0f;

				_this->m_Data.metaAlpha = currentAlpha;
				_this->m_Data.colorScaleRGBA.a = currentAlpha;
			}
		}

		// ===== CLOCK STATE / LOGOS =====
		if (!g_is_hen) {
			if (strncmp(widgetName, "pslogo", 6) == 0 || strncmp(widgetName, "pslogo_ring", 11) == 0 ||
				strncmp(widgetName, "performance_mode_text", 21) == 0 || strncmp(widgetName, "performance_mode_text_glow", 26) == 0 ||
				strncmp(widgetName, "balanced_mode_text", 18) == 0 || strncmp(widgetName, "balanced_mode_text_glow", 23) == 0 ||
				strncmp(widgetName, "power_saving_mode_text", 22) == 0 || strncmp(widgetName, "power_saving_mode_text_glow", 27) == 0)
			{
				paf::PhWidget* parent = GetParent();
				bool parentVisible = parent && parent->m_Data.metaAlpha > 0.1f;

				if (!parentVisible) {
					_this->m_Data.metaAlpha = 0.f;
				}
				else {
					float pslogoVis = 0.f, perfVis = 0.f, balVis = 0.f, powerVis = 0.f;

					switch (g_cachedClockState) {
					case CLOCK_OVERCLOCK: pslogoVis = 1.f; perfVis = 1.f; break;
					case CLOCK_BALANCED:  pslogoVis = 1.f; balVis = 1.f; break;
					case CLOCK_UNDERCLOCK: pslogoVis = 1.f; powerVis = 1.f; break;
					default: break;
					}

					float pulseAlpha = 0.f;
					if (pslogoVis > 0.1f) {
						uint64_t elapsed_us = currentTime_us - g_animationStateChangeTime_us;
						float progress = elapsed_us / static_cast<float>(FADE_DURATION_US);
						if (progress > 1.f) progress = 1.f;

						switch (g_animationState) {
						case FADING_IN: pulseAlpha = progress;
							if (elapsed_us >= FADE_DURATION_US) {
								g_animationState = VISIBLE;
								g_animationStateChangeTime_us = currentTime_us;
							} break;

						case VISIBLE: pulseAlpha = 1.f;
							if (elapsed_us >= VISIBLE_DURATION_US) {
								g_animationState = FADING_OUT;
								g_animationStateChangeTime_us = currentTime_us;
							} break;

						case FADING_OUT: pulseAlpha = 1.f - progress;
							if (elapsed_us >= FADE_DURATION_US) {
								g_animationState = INVISIBLE;
								g_animationStateChangeTime_us = currentTime_us;
							} break;

						case INVISIBLE: pulseAlpha = 0.f;
							if (elapsed_us >= INVISIBLE_DURATION_US) {
								g_animationState = FADING_IN;
								g_animationStateChangeTime_us = currentTime_us;
							} break;
						}
					}

					if (strncmp(widgetName, "pslogo_ring", 11) == 0) _this->m_Data.colorScaleRGBA.a = pulseAlpha;
					else if (strncmp(widgetName, "pslogo", 6) == 0) _this->m_Data.colorScaleRGBA.a = pslogoVis;
					else if (strncmp(widgetName, "performance_mode_text_glow", 26) == 0) _this->m_Data.colorScaleRGBA.a = perfVis * pulseAlpha;
					else if (strncmp(widgetName, "performance_mode_text", 21) == 0) _this->m_Data.colorScaleRGBA.a = perfVis;
					else if (strncmp(widgetName, "balanced_mode_text_glow", 23) == 0) _this->m_Data.colorScaleRGBA.a = balVis * pulseAlpha;
					else if (strncmp(widgetName, "balanced_mode_text", 18) == 0) _this->m_Data.colorScaleRGBA.a = balVis;
					else if (strncmp(widgetName, "power_saving_mode_text_glow", 27) == 0) _this->m_Data.colorScaleRGBA.a = powerVis * pulseAlpha;
					else if (strncmp(widgetName, "power_saving_mode_text", 22) == 0) _this->m_Data.colorScaleRGBA.a = powerVis;
				}
			}
		}
	}

	return pafWidgetDrawThis_Detour ? pafWidgetDrawThis_Detour->GetOriginal<int>(_this, r4, r5) : 0;
}

// ===== INSTALL / REMOVE =====
void Install()
{
	g_is_hen = IsPayloadHen();
	g_isIpTextDisabled = !IsIpTextEnabled();
	LoadIpText();
	pafWidgetDrawThis_Detour = new Detour(((opd_s*)paf::paf_63D446B8)->sub, pafWidgetDrawThis_Hook);
}

void Remove()
{
	if (pafWidgetDrawThis_Detour)
		delete pafWidgetDrawThis_Detour;
}
