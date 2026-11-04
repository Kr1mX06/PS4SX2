// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Uses the same Vulkan/ImGui presentation as the in-game HUD.
#include "GameMenu.h"
#include "InGameMenuState.h"
#include "PhysicalDisc.h"
#include "CoverDownload.h"
#include "ReleaseCheck.h"
#include "GameDatabase.h"
#include "GameList.h"
#include "CDVD/IsoFileFormats.h"
#include "common/Error.h"
#include "MTGS.h"
#include "GS/GS.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "common/Threading.h"
#include "common/Timer.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "stb_image.h"
#include <orbis/Pad.h>
#include <orbis/AudioOut.h>
#include <orbis/Sysmodule.h>
#include <orbis/SystemService.h>
#include <orbis/UserService.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cfloat>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <pthread.h>
#include <sys/stat.h>
#include <utility>
#include <vector>

extern void ProbeLog(const char* format, ...);

namespace PS4GameMenu
{
	namespace
	{
		constexpr const char* Directory = "/data/PS4SX2/roms";
		constexpr const char* CoversDirectory = "/data/PS4SX2/covers";
		constexpr const char* FavoritesFile = "/data/PS4SX2/favorites.txt";
		constexpr const char* RecentFile = "/data/PS4SX2/recent.txt";
		constexpr const char* UiAssetsDirectory = "/app0/resources/ui";
		constexpr const char* IntroDirectory = "/app0/resources/intro";
		constexpr size_t GridColumns = 8;
		constexpr size_t VisibleRows = 1;
		constexpr size_t VisibleItems = GridColumns * VisibleRows;
		enum class LibraryView : unsigned
		{
			Library,
			Disc,
			Recent,
			Favorites,
			Settings,
		};
		struct CoverImage
		{
			std::string path;
			int width = 0;
			int height = 0;
			std::vector<unsigned char> pixels;
		};
		struct Item
		{
			std::string name;
			std::string path;
			std::shared_ptr<CoverImage> cover;
			bool favorite = false;
			std::string serial;
			GamePlatform platform = GamePlatform::Unknown;
		};
		struct CoverTexture
		{
			std::shared_ptr<CoverImage> source;
			ImTextureData* texture = nullptr;
		};
		struct DiscProbe
		{
			pthread_t thread{};
			bool active = false;
			std::atomic<bool> done{false};
			std::unique_ptr<PS4Disc::Reader> reader;
			std::string error;
			bool Start()
			{
				if (active) return false;
				reader.reset();
				error.clear();
				done.store(false, std::memory_order_relaxed);
				const int result = pthread_create(&thread, nullptr, [](void* context) -> void* {
					auto* probe = static_cast<DiscProbe*>(context);
					probe->reader = PS4Disc::Reader::Open(probe->error);
					probe->done.store(true, std::memory_order_release);
					return nullptr;
				}, this);
				active = result == 0;
				if (!active) error = "Detection du disque impossible.";
				return active;
			}
			bool Collect()
			{
				if (!active || !done.load(std::memory_order_acquire)) return false;
				pthread_join(thread, nullptr);
				active = false;
				return true;
			}
			~DiscProbe() { if (active) pthread_join(thread, nullptr); }
		};
		struct UiImage
		{
			std::string name;
			std::shared_ptr<CoverImage> source;
		};
		// Only accessed by GS callbacks. The CPU owns a separate list for selection.
		std::vector<Item> s_items;
		size_t s_selected = 0;
		bool s_visible = false;
		bool s_connected = false;
		bool s_sidebar_focused = false;
		bool s_exit_dialog_visible = false;
		bool s_exit_confirm = false;
		LibraryView s_library_view = LibraryView::Library;
		bool s_loading_visible = false;
		bool s_loading_carousel = false;
		unsigned s_loading_progress = 0;
		float s_loading_carousel_phase = 0.0f;
		std::string s_loading_status;
		bool s_intro_visible = false;
		std::shared_ptr<CoverImage> s_intro_frame;
		ImTextureData* s_intro_texture = nullptr;
		// One atomic snapshot shared by CPU and GS; field widths are defined
		// in InGameMenuState.h so publication and rendering stay consistent.
		std::atomic<unsigned> s_in_game_state{0};
		std::string s_status;
		std::vector<CoverTexture> s_cover_textures;
		std::vector<UiImage> s_ui_images;

		std::shared_ptr<CoverImage> LoadImageFile(const std::string& image_path, const char* kind, bool log_success = true)
		{
			int width = 0, height = 0, channels = 0;
			unsigned char* decoded = stbi_load(image_path.c_str(), &width, &height, &channels, 4);
			if (!decoded || width <= 0 || height <= 0 || static_cast<size_t>(width) * static_cast<size_t>(height) > 8 * 1024 * 1024)
			{
				if (decoded)
					stbi_image_free(decoded);
				ProbeLog("Game menu: invalid or oversized %s %s\n", kind, image_path.c_str());
				return {};
			}
			auto cover = std::make_shared<CoverImage>();
			cover->path = image_path;
			cover->width = width;
			cover->height = height;
			cover->pixels.assign(decoded, decoded + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
			stbi_image_free(decoded);
			if (log_success)
				ProbeLog("Game menu: %s loaded %s (%dx%d)\n", kind, image_path.c_str(), width, height);
			return cover;
		}

		std::shared_ptr<CoverImage> LoadCover(const std::string& name)
		{
			const size_t dot = name.find_last_of('.');
			if (dot == std::string::npos)
				return {};
			const std::string stem = name.substr(0, dot);
			const char* const extensions[] = {".png", ".jpg", ".jpeg", ".PNG", ".JPG", ".JPEG"};
			for (const char* extension : extensions)
			{
				const std::string candidate = std::string(CoversDirectory) + "/" + stem + extension;
				struct stat info{};
				if (stat(candidate.c_str(), &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0)
					return LoadImageFile(candidate, "cover");
			}
			return {};
		}

		// Read the disc serial from the image instead of guessing from its display
		// name. This keeps automatic cover downloads working for ISO, BIN and CHD.
		void ReadImageInfo(Item& item)
		{
			if (item.path.empty())
				return;
			GameList::Entry entry;
			if (!GameList::PopulateEntryFromPath(item.path, &entry))
			{
				ProbeLog("Game menu: metadata unavailable for %s\n", item.path.c_str());
				return;
			}
			item.platform = entry.type == GameList::EntryType::PS1Disc ? GamePlatform::PS1 :
				entry.type == GameList::EntryType::PS2Disc ? GamePlatform::PS2 : GamePlatform::Unknown;
			item.serial = entry.serial;
			std::transform(item.serial.begin(), item.serial.end(), item.serial.begin(), [](unsigned char c) {
				return static_cast<char>(std::toupper(c));
			});
			ProbeLog("Game menu: image metadata %s -> serial=%s platform=%s\n", item.path.c_str(), item.serial.c_str(),
				item.platform == GamePlatform::PS1 ? "PS1" : item.platform == GamePlatform::PS2 ? "PS2" : "unknown");
		}

		void LoadUiAssets()
		{
			if (!s_ui_images.empty())
				return;
			static constexpr const char* names[] = {
				"menu_logo", "menu_biblioteca", "menu_disc", "menu_recentes", "menu_favoritos", "menu_configuracoes", "status_controle",
				"controle_x", "controle_bola", "controle_options", "controle_quadrado", "controle_triangulo", "controle_l1", "controle_r1",
				"navegacao_esquerda", "navegacao_direita", "loading_carousel_1", "loading_carousel_2",
				"loading_carousel_3", "loading_carousel_4"};
			for (const char* name : names)
			{
				const std::string path = std::string(UiAssetsDirectory) + "/" + name + ".png";
				s_ui_images.push_back({name, LoadImageFile(path, "UI asset")});
			}
			// (AI-assisted) Reuse the package background at runtime so the PS4 tile
			// and emulator loading screen always show the exact same artwork.
			s_ui_images.push_back({"loading_background", LoadImageFile("/app0/sce_sys/pic1.png", "loading background")});
			// Allow a custom 16:9 library backdrop copied by FTP, without
			// replacing the installed PKG. Fall back to packaged pic1.png.
			auto library_background = LoadImageFile("/data/PS4SX2/menu-background.png", "library background", false);
			if (!library_background)
				library_background = LoadImageFile("/app0/resources/ui/menu-background.png", "bundled library background", false);
			if (!library_background)
				library_background = s_ui_images.back().source;
			s_ui_images.push_back({"library_background", std::move(library_background)});
			ProbeLog("Game menu: loaded %zu UI button assets\n", s_ui_images.size());
		}

		std::shared_ptr<CoverImage> FindUiAsset(const char* name)
		{
			for (const UiImage& image : s_ui_images)
				if (image.name == name)
					return image.source;
			return {};
		}

		void ClearCoverTextures()
		{
			for (CoverTexture& cover : s_cover_textures)
			{
				if (!cover.texture)
					continue;
				if (GSTexture* texture = static_cast<GSTexture*>(cover.texture->BackendUserData); texture && g_gs_device)
					g_gs_device->Recycle(texture);
				if (ImGui::GetCurrentContext())
					ImGui::UnregisterUserTexture(cover.texture);
				delete cover.texture;
			}
			s_cover_textures.clear();
		}

		void ClearIntroTexture()
		{
			if (!s_intro_texture)
				return;
			if (GSTexture* texture = static_cast<GSTexture*>(s_intro_texture->BackendUserData); texture && g_gs_device)
				g_gs_device->Recycle(texture);
			if (ImGui::GetCurrentContext())
				ImGui::UnregisterUserTexture(s_intro_texture);
			delete s_intro_texture;
			s_intro_texture = nullptr;
		}

		ImTextureData* GetIntroTexture()
		{
			if (s_intro_texture || !s_intro_frame)
				return s_intro_texture;
			s_intro_texture = new ImTextureData();
			s_intro_texture->Create(ImTextureFormat_RGBA32, s_intro_frame->width, s_intro_frame->height);
			std::memcpy(s_intro_texture->Pixels, s_intro_frame->pixels.data(), s_intro_frame->pixels.size());
			s_intro_texture->UseColors = true;
			ImGui::RegisterUserTexture(s_intro_texture);
			return s_intro_texture;
		}

		ImTextureData* GetCoverTexture(const std::shared_ptr<CoverImage>& source)
		{
			if (!source)
				return nullptr;
			for (const CoverTexture& cover : s_cover_textures)
			{
				if (cover.source && cover.source->path == source->path)
					return cover.texture;
			}
			auto* texture = new ImTextureData();
			texture->Create(ImTextureFormat_RGBA32, source->width, source->height);
			std::memcpy(texture->Pixels, source->pixels.data(), source->pixels.size());
			texture->UseColors = true;
			ImGui::RegisterUserTexture(texture);
			s_cover_textures.push_back({source, texture});
			return texture;
		}

		std::vector<Item> Scan()
		{
			std::vector<Item> items;
			if (DIR* dir = opendir(Directory))
			{
				while (dirent* entry = readdir(dir))
				{
					const std::string name(entry->d_name);
					const size_t dot = name.find_last_of('.');
					if (name.empty() || name[0] == '.' || dot == std::string::npos)
						continue;
					std::string extension = name.substr(dot);
					std::transform(extension.begin(), extension.end(), extension.begin(),
						[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
					if (extension != ".iso" && extension != ".bin" && extension != ".chd")
						continue;
					const std::string path = std::string(Directory) + "/" + name;
					struct stat info{};
					if (stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0)
					{
						Item item{name.substr(0, dot), path, LoadCover(name), false, {}};
						ReadImageInfo(item);
						if (!item.cover && !item.serial.empty())
							item.cover = LoadCover(item.serial + ".iso");
						items.push_back(std::move(item));
					}
				}
				closedir(dir);
			}
			std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.name < b.name; });
			ProbeLog("Game menu: found %zu .iso/.bin/.chd files in %s\n", items.size(), Directory);
			return items;
		}

		bool ContainsPath(const std::vector<std::string>& paths, const std::string& path)
		{
			return std::find(paths.begin(), paths.end(), path) != paths.end();
		}

		std::vector<std::string> LoadPathList(const char* filename)
		{
			std::vector<std::string> paths;
			FILE* file = std::fopen(filename, "rb");
			if (!file)
				return paths;
			char line[1024];
			while (std::fgets(line, sizeof(line), file))
			{
				std::string path(line);
				while (!path.empty() && (path.back() == '\n' || path.back() == '\r'))
					path.pop_back();
				if (!path.empty() && !ContainsPath(paths, path))
					paths.push_back(std::move(path));
			}
			std::fclose(file);
			return paths;
		}

		bool SavePathList(const char* filename, const std::vector<std::string>& paths)
		{
			const std::string temporary = std::string(filename) + ".tmp";
			FILE* file = std::fopen(temporary.c_str(), "wb");
			if (!file)
				return false;
			bool success = true;
			for (const std::string& path : paths)
				success = success && std::fprintf(file, "%s\n", path.c_str()) >= 0;
			success = std::fclose(file) == 0 && success;
			if (success)
				success = std::rename(temporary.c_str(), filename) == 0;
			if (!success)
				std::remove(temporary.c_str());
			return success;
		}


		// Editable native settings. These are the same opt-in switches read by
		// the PS4 VM before initialization; no fake on-screen toggles.
		bool SettingsMarker(const char* name)
		{
			const std::string path = std::string("/data/PS4SX2/") + name;
			struct stat info{};
			return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
		}

		bool SetSettingsMarker(const char* name, bool enabled)
		{
			const std::string path = std::string("/data/PS4SX2/") + name;
			if (!enabled)
				return std::remove(path.c_str()) == 0 || !SettingsMarker(name);
			FILE* file = std::fopen(path.c_str(), "wb");
			return file && std::fclose(file) == 0;
		}

		unsigned EECycleSetting()
		{
			return SettingsMarker("vm-ee-cycle-normal") ? 0u :
				SettingsMarker("vm-ee-cycle-underclock") ? 2u : 1u;
		}

		unsigned FrameQueueSetting()
		{
			return SettingsMarker("vm-frame-queue-1") ? 1u :
				SettingsMarker("vm-frame-queue-3") ? 3u : 2u;
		}

		std::vector<Item> BuildLauncherSettings()
		{
			std::vector<Item> entries;
			entries.reserve(8);
			const char* ee[] = {"Normal (precis)", "Equilibre (-1)", "Performance (-2)"};
			entries.push_back({std::string("Cycles EE : ") + ee[EECycleSetting()], {}, {}, false,
				"Profil CPU emule. Le sous-cadencage peut gener certains jeux."});
			entries.push_back({std::string("Saut cycles EE : ") + (SettingsMarker("vm-ee-cycle-skip") ? "Experimental" : "Desactive"),
				{}, {}, false, "Saute des cycles EE : bugs et vitesse incorrecte possibles."});
			entries.push_back({std::string("Lecture rapide CDVD : ") + (SettingsMarker("vm-fast-cdvd") ? "Activee" : "Normal"),
				{}, {}, false, "Lecture disque acceleree ; normal privilegie la compatibilite."});
			entries.push_back({std::string("Cache shaders : ") + (SettingsMarker("vm-no-shader-cache") ? "Desactive" : "Active"),
				{}, {}, false, "Le cache reduit les compilations repetees de shaders."});
			entries.push_back({std::string("Demarrage rapide PS2 : ") + (SettingsMarker("vm-no-fast-boot") ? "Desactive" : "Active"),
				{}, {}, false, "Saute l'intro BIOS ; a desactiver pour jeux sensibles."});
			entries.push_back({std::string("Synchro audio : ") + (SettingsMarker("vm-audio-async") ? "AsyncMix" : "TimeStretch"),
				{}, {}, false, "AsyncMix et TimeStretch ne changent pas la vitesse reelle."});
			entries.push_back({std::string("File d'images : ") + std::to_string(FrameQueueSetting()),
				{}, {}, false, "1 menor latencia; 2 padrao; 3 pode aumentar latencia."});
			entries.push_back({std::string("Prechargement GS : ") + (SettingsMarker("vm-no-gs-preload") ? "Desactive" : "Active"),
				{}, {}, false, "A desactiver si artefacts graphiques."});
			return entries;
		}

		bool ChangeLauncherSetting(size_t index, int delta)
		{
			switch (index)
			{
			case 0:
			{
				const unsigned next = (EECycleSetting() + 3u + (delta < 0 ? 2u : 1u)) % 3u;
				return SetSettingsMarker("vm-ee-cycle-normal", next == 0) &&
					SetSettingsMarker("vm-ee-cycle-underclock", next == 2);
			}
			case 1:
				return SetSettingsMarker("vm-ee-cycle-skip", !SettingsMarker("vm-ee-cycle-skip"));
			case 2:
				return SetSettingsMarker("vm-fast-cdvd", !SettingsMarker("vm-fast-cdvd"));
			case 3:
				return SetSettingsMarker("vm-no-shader-cache", !SettingsMarker("vm-no-shader-cache"));
			case 4:
				return SetSettingsMarker("vm-no-fast-boot", !SettingsMarker("vm-no-fast-boot"));
			case 5:
				return SetSettingsMarker("vm-audio-async", !SettingsMarker("vm-audio-async"));
			case 6:
			{
				const unsigned next = static_cast<unsigned>((static_cast<int>(FrameQueueSetting()) - 1 +
					(delta < 0 ? 2 : 1)) % 3 + 1);
				return SetSettingsMarker("vm-frame-queue-1", next == 1) &&
					SetSettingsMarker("vm-frame-queue-3", next == 3);
			}
			case 7:
				return SetSettingsMarker("vm-no-gs-preload", !SettingsMarker("vm-no-gs-preload"));
			default:
				return false;
			}
		}

		std::vector<Item> BuildView(const std::vector<Item>& library, LibraryView view,
			const std::vector<std::string>& favorites, const std::vector<std::string>& recent)
		{
			std::vector<Item> items;
			if (view == LibraryView::Settings)
				return BuildLauncherSettings();
			if (view == LibraryView::Disc)
			{
				// Detection is asynchronous; the CPU publishes the result when it is ready.
			}
			else if (view == LibraryView::Recent)
			{
				for (const std::string& path : recent)
				{
					const auto found = std::find_if(library.begin(), library.end(), [&](const Item& item) { return item.path == path; });
					if (found != library.end())
					{
						items.push_back(*found);
						items.back().favorite = ContainsPath(favorites, path);
					}
				}
			}
			else if (view == LibraryView::Library || view == LibraryView::Favorites)
			{
				for (Item item : library)
				{
					item.favorite = ContainsPath(favorites, item.path);
					if (view == LibraryView::Library || item.favorite)
						items.push_back(std::move(item));
				}
			}
			if (items.empty())
			{
				const char* message = view == LibraryView::Disc      ? "Aucun disque detecte" :
				                      view == LibraryView::Recent    ? "Aucun jeu recent" :
				                      view == LibraryView::Favorites ? "Aucun favori" :
				                      view == LibraryView::Settings  ? "Reglages bientot disponibles" :
				                                                       "Aucun jeu trouve";
				items.push_back({message, {}, {}, false});
			}
			return items;
		}

		void Publish(const std::vector<Item>& items, LibraryView view)
		{
			MTGS::RunOnGSThread([items, view]() {
				ClearCoverTextures();
				s_items = items;
				s_library_view = view;
				s_loading_visible = false;
				s_loading_carousel = false;
				s_intro_visible = false;
				s_visible = true;
			});
		}

		void Present(size_t selected, const std::string& status, bool connected, bool sidebar_focused,
			bool exit_dialog_visible, bool exit_confirm)
		{
			MTGS::RunOnGSThread([selected, status, connected, sidebar_focused, exit_dialog_visible, exit_confirm]() {
				s_connected = connected;
				s_sidebar_focused = sidebar_focused;
				s_exit_dialog_visible = exit_dialog_visible;
				s_exit_confirm = exit_confirm;
				s_selected = selected;
				s_status = status;
				GSPresentCurrentFrame();
			});
			// The PS4 window is surfaceless, so MTGS's generic idle loop does not present.
			// Keep at most one menu frame queued. Transition to the VM needs console testing.
			MTGS::WaitGS(false);
		}

		void PresentStartupLoading(unsigned progress, float carousel_phase)
		{
			MTGS::RunOnGSThread([progress, carousel_phase]() {
				s_visible = false;
				s_intro_visible = false;
				s_loading_visible = true;
				s_loading_carousel = true;
				s_loading_progress = std::min(progress, 100u);
				s_loading_carousel_phase = carousel_phase;
				s_loading_status = "Chargement...";
				GSPresentCurrentFrame();
			});
			MTGS::WaitGS(false);
		}

		void PresentIntroFrame(const std::shared_ptr<CoverImage>& frame)
		{
			MTGS::RunOnGSThread([frame]() {
				ClearIntroTexture();
				s_intro_frame = frame;
				s_loading_visible = false;
				s_loading_carousel = false;
				s_intro_visible = static_cast<bool>(frame);
				GSPresentCurrentFrame();
			});
			MTGS::WaitGS(false);
		}

		void HideIntro()
		{
			MTGS::RunOnGSThread([]() {
				s_intro_visible = false;
				s_intro_frame.reset();
				ClearIntroTexture();
			});
			MTGS::WaitGS(false);
		}

		class IntroAudio
		{
		public:
			bool Start()
			{
				const std::string path = std::string(IntroDirectory) + "/audio_s16le_48k_stereo.pcm";
				FILE* file = std::fopen(path.c_str(), "rb");
				if (!file)
				{
					ProbeLog("Startup intro: audio file unavailable %s\n", path.c_str());
					return false;
				}
				std::fseek(file, 0, SEEK_END);
				const long bytes = std::ftell(file);
				std::rewind(file);
				if (bytes <= 0 || (bytes % sizeof(int16_t)) != 0)
				{
					std::fclose(file);
					ProbeLog("Startup intro: invalid audio size=%ld\n", bytes);
					return false;
				}
				m_samples.resize(static_cast<size_t>(bytes) / sizeof(int16_t));
				const size_t read = std::fread(m_samples.data(), 1, static_cast<size_t>(bytes), file);
				std::fclose(file);
				if (read != static_cast<size_t>(bytes))
				{
					ProbeLog("Startup intro: short audio read=%zu expected=%ld\n", read, bytes);
					m_samples.clear();
					return false;
				}

				const int module = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_AUDIOOUT);
				const int init = sceAudioOutInit();
				m_handle = sceAudioOutOpen(ORBIS_USER_SERVICE_USER_ID_SYSTEM, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN,
					0, Grain, 48000, ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
				ProbeLog("Startup intro: audio module=%#x init=%#x open=%#x samples=%zu\n",
					module, init, m_handle, m_samples.size());
				if (m_handle < 0)
					return false;
				int32_t levels[2] = {32768, 32768};
				sceAudioOutSetVolume(m_handle, 0x3, levels);
				pthread_attr_t attr;
				int result = pthread_attr_init(&attr);
				if (result == 0)
				{
					result = pthread_attr_setstacksize(&attr, 256 * 1024);
					if (result == 0)
						result = pthread_create(&m_thread, &attr, ThreadEntry, this);
					pthread_attr_destroy(&attr);
				}
				if (result != 0)
				{
					sceAudioOutClose(m_handle);
					m_handle = -1;
					ProbeLog("Startup intro: audio thread failed=%d\n", result);
					return false;
				}
				m_started = true;
				return true;
			}

			void Finish()
			{
				if (m_started)
				{
					pthread_join(m_thread, nullptr);
					m_started = false;
				}
				if (m_handle >= 0)
				{
					const int result = sceAudioOutClose(m_handle);
					ProbeLog("Startup intro: audio close=%#x\n", result);
					m_handle = -1;
				}
			}

			~IntroAudio() { Finish(); }

		private:
			static constexpr size_t Grain = 512;
			static void* ThreadEntry(void* context)
			{
				static_cast<IntroAudio*>(context)->Run();
				return nullptr;
			}

			void Run()
			{
				alignas(64) int16_t buffer[Grain * 2]{};
				for (size_t offset = 0; offset < m_samples.size(); offset += Grain * 2)
				{
					const size_t count = std::min(Grain * 2, m_samples.size() - offset);
					std::memset(buffer, 0, sizeof(buffer));
					std::memcpy(buffer, m_samples.data() + offset, count * sizeof(int16_t));
					const int result = sceAudioOutOutput(m_handle, buffer);
					if (result < 0)
					{
						ProbeLog("Startup intro: audio output failed=%#x offset=%zu\n", result, offset);
						break;
					}
				}
				sceAudioOutOutput(m_handle, nullptr);
			}

			std::vector<int16_t> m_samples;
			int m_handle = -1;
			pthread_t m_thread{};
			bool m_started = false;
		};

		void PlayStartupSequence()
		{
			ProbeLog("STAGE: branded startup loading\n");
			constexpr double loading_seconds = 2.35;
			Common::Timer loading_timer;
			for (;;)
			{
				const double elapsed = loading_timer.GetTimeSeconds();
				const unsigned progress = static_cast<unsigned>(std::min(elapsed / loading_seconds, 1.0) * 100.0);
				PresentStartupLoading(progress, static_cast<float>(elapsed * 275.0));
				if (elapsed >= loading_seconds)
					break;
				Threading::Sleep(16);
			}

			ProbeLog("STAGE: startup intro video\n");
			unsigned frame_count = 0;
			double fps = 0.0;
			const std::string manifest_path = std::string(IntroDirectory) + "/manifest.txt";
			if (FILE* manifest = std::fopen(manifest_path.c_str(), "r"))
			{
				char line[96];
				while (std::fgets(line, sizeof(line), manifest))
				{
					unsigned value = 0;
					double decimal = 0.0;
					if (std::sscanf(line, "frames=%u", &value) == 1)
						frame_count = value;
					else if (std::sscanf(line, "fps=%lf", &decimal) == 1)
						fps = decimal;
				}
				std::fclose(manifest);
			}
			if (frame_count == 0 || frame_count > 300 || fps < 1.0 || fps > 60.0)
			{
				ProbeLog("Startup intro: invalid manifest frames=%u fps=%.3f\n", frame_count, fps);
				HideIntro();
				return;
			}
			char path[160];
			std::snprintf(path, sizeof(path), "%s/frame_%03u.jpg", IntroDirectory, 1u);
			auto frame = LoadImageFile(path, "intro frame", false);
			if (!frame)
			{
				ProbeLog("Startup intro: first frame unavailable; continuing to library\n");
				HideIntro();
				return;
			}
			PresentIntroFrame(frame);
			IntroAudio audio;
			audio.Start();
			Common::Timer video_timer;
			for (unsigned index = 2; index <= frame_count; index++)
			{
				std::snprintf(path, sizeof(path), "%s/frame_%03u.jpg", IntroDirectory, index);
				auto next = LoadImageFile(path, "intro frame", false);
				if (!next)
					break;
				const double deadline = static_cast<double>(index - 1) / fps;
				while (video_timer.GetTimeSeconds() < deadline)
					Threading::Sleep(1);
				PresentIntroFrame(next);
			}
			while (video_timer.GetTimeSeconds() < static_cast<double>(frame_count) / fps)
				Threading::Sleep(1);
			audio.Finish();
			HideIntro();
			ProbeLog("Startup intro: finished frames=%u fps=%.3f\n", frame_count, fps);
		}
	} // namespace

	bool Select(int& pad, std::string& path, GamePlatform& platform)
	{
		platform = GamePlatform::Unknown;
		ProbeLog("STAGE: Game menu\n");
		if (!MTGS::WaitForOpen())
		{
			ProbeLog("Game menu: GS initialization failed\n");
			return false;
		}
		// Decode the packaged button artwork on the CPU thread; the GS callback
		// only uploads textures and draws them.
		LoadUiAssets();
		PresentStartupLoading(0, 0.0f);
		// Keep the system pic1 visible until the first Vulkan loading frame exists.
		sceSystemServiceHideSplashScreen();
		PlayStartupSequence();
		// (AI-assisted) Query the public release endpoint asynchronously. The
		// launcher remains usable while DNS/HTTPS is in progress.
		PS4Release::Start();
		auto library = Scan();
		auto favorites = LoadPathList(FavoritesFile);
		auto recent = LoadPathList(RecentFile);
		LibraryView view = LibraryView::Library;
		auto items = BuildView(library, view, favorites, recent);
		Publish(items, view);
		size_t selected = 0;
		OrbisPadData initial{};
		u32 previous = pad >= 0 && scePadReadState(pad, &initial) == 0 ? initial.buttons : 0;
		Common::Timer timer;
		double next_repeat = 0.0, next_pad_open = 0.0;
		u32 held_direction = 0;
		bool start = false;
		bool sidebar_focused = false;
		bool exit_dialog_visible = false;
		bool exit_confirm = false;
		std::string status;
		DiscProbe disc_probe;
		DiscCoverDownload disc_cover;
		DiscCoverDownload library_cover;
		size_t library_cover_index = 0;
		std::string disc_status;
		bool boot_disc_when_ready = false;
		const auto edit_launcher_setting = [&](int delta) {
			if (view != LibraryView::Settings || sidebar_focused || selected >= items.size())
				return;
			const bool changed = ChangeLauncherSetting(selected, delta);
			status = changed ? "Reglage enregistre : applique au prochain jeu." :
				"Erreur sauvegarde. Verifier /data/PS4SX2.";
			if (changed)
			{
				items = BuildView(library, view, favorites, recent);
				Publish(items, view);
			}
		};
		const auto pump_library_cover = [&]() {
			if (library_cover.Active())
				return;
			while (library_cover_index < library.size())
			{
				Item& item = library[library_cover_index++];
				if (item.cover || item.serial.empty())
					continue;
				if (library_cover.Start(item.serial, item.name, item.platform))
				{
					ProbeLog("Game menu: queued cover serial=%s title=%s\n", item.serial.c_str(), item.name.c_str());
					return;
				}
			}
		};
		const auto probe_disc = [&](bool boot) {
			if (disc_probe.active) return;
			boot_disc_when_ready = boot;
			const bool started = disc_probe.Start();
			items = {{started ? "Verification du DVD..." : "Detection disque echouee", {}, {}, false}};
			selected = 0;
			Publish(items, view);
			status = started ? "Lecture du lecteur. Navigation possible." : disc_probe.error;
			disc_status = status;
		};
		const auto set_view = [&](LibraryView next) {
			boot_disc_when_ready = false;
			view = next;
			items = BuildView(library, view, favorites, recent);
			selected = 0;
			Publish(items, view);
			status.clear();
			if (view == LibraryView::Disc)
			{
				if (!disc_probe.active) probe_disc(false);
				else status = "Verification du DVD...";
			}
		};
		// Library refreshes now begin a serialized cover queue. The UI remains
		// responsive while each native HTTPS request completes in its own thread.
		pump_library_cover();
		for (;;)
		{
			const double now = timer.GetTimeSeconds();
			if (library_cover.Collect())
			{
				for (Item& item : library)
				{
					if (item.serial == library_cover.Serial() && !item.cover)
						item.cover = LoadCover(item.name + ".iso");
				}
				items = BuildView(library, view, favorites, recent);
				Publish(items, view);
				pump_library_cover();
			}
			if (disc_cover.Collect() && view == LibraryView::Disc && !disc_probe.active && disc_probe.reader &&
				disc_probe.reader->GetInfo().serial == disc_cover.Serial() && items.size() == 1 && items[0].path == PS4Disc::DevicePath)
			{
				if (disc_cover.Succeeded())
				{
					items[0].cover = LoadCover(disc_cover.Serial() + ".iso");
					Publish(items, view);
				}
				status = disc_status;
				if (!disc_cover.Succeeded())
					status += " Cover unavailable. Triangle to retry.";
			}
			if (disc_probe.Collect() && view == LibraryView::Disc)
			{
				if (disc_probe.reader)
				{
					const auto& disc = disc_probe.reader->GetInfo();
					const auto* game = GameDatabase::findGame(disc.serial);
					const std::string title = game ? std::string(game->name) : "PS2 DVD - " + disc.serial;
					// Serial identifies the exact disc edition; the database title is a fallback.
					auto cover = LoadCover(disc.serial + ".iso");
					if (!cover && game && title.find_first_of("/\\") == std::string::npos)
						cover = LoadCover(title + ".iso");
					if (!cover && !boot_disc_when_ready && !disc_cover.Active())
						disc_cover.Start(disc.serial);
					items = {{title, PS4Disc::DevicePath, std::move(cover), false}};
					status = disc.serial + (disc.dual_type ? " | Dual-layer DVD | Press X to start." : " | DVD | Press X to start.");
					if (boot_disc_when_ready)
					{
						path = PS4Disc::DevicePath;
						platform = GamePlatform::PS2;
						PS4Disc::Prepare(std::move(disc_probe.reader));
						start = true;
						ProbeLog("Game menu: selected physical DVD %s\n", title.c_str());
						break;
					}
				}
				else
				{
					items = {{"DVD indisponible", {}, {}, false}};
					status = disc_probe.error;
				}
				boot_disc_when_ready = false;
				disc_status = status;
				if (disc_cover.Active())
					status += " Downloading cover...";
				selected = 0;
				Publish(items, view);
			}
			if (pad < 0 && now >= next_pad_open)
			{
				next_pad_open = now + 1.0;
				OrbisUserServiceUserId user = 0;
				if (sceUserServiceGetInitialUser(&user) == 0)
					pad = scePadOpen(user, 0, 0, nullptr);
			}
			OrbisPadData data{};
			const bool connected = pad >= 0 && scePadReadState(pad, &data) == 0 && data.connected;
			const u32 buttons = connected ? data.buttons : 0;
			const u32 pressed = buttons & ~previous;
			previous = buttons;
			if (exit_dialog_visible)
			{
				u32 dialog_navigation = 0;
				if (buttons & ORBIS_PAD_BUTTON_LEFT)
					dialog_navigation = ORBIS_PAD_BUTTON_LEFT;
				else if (buttons & ORBIS_PAD_BUTTON_RIGHT)
					dialog_navigation = ORBIS_PAD_BUTTON_RIGHT;
				else if (connected && data.leftStick.x < 72)
					dialog_navigation = ORBIS_PAD_BUTTON_LEFT;
				else if (connected && data.leftStick.x > 184)
					dialog_navigation = ORBIS_PAD_BUTTON_RIGHT;
				if (dialog_navigation && (dialog_navigation != held_direction || now >= next_repeat))
				{
					exit_confirm = dialog_navigation == ORBIS_PAD_BUTTON_LEFT;
					next_repeat = now + (dialog_navigation == held_direction ? 0.12 : 0.35);
				}
				held_direction = dialog_navigation;
				if (pressed & ORBIS_PAD_BUTTON_LEFT)
					exit_confirm = true;
				else if (pressed & ORBIS_PAD_BUTTON_RIGHT)
					exit_confirm = false;
				if (pressed & ORBIS_PAD_BUTTON_CIRCLE)
					exit_dialog_visible = false;
				else if (pressed & ORBIS_PAD_BUTTON_CROSS)
				{
					if (exit_confirm)
						break;
					exit_dialog_visible = false;
				}
				Present(selected, status, connected, sidebar_focused, exit_dialog_visible, exit_confirm);
				Threading::Sleep(16);
				continue;
			}
			if (pressed & ORBIS_PAD_BUTTON_CIRCLE)
			{
				boot_disc_when_ready = false;
				exit_dialog_visible = true;
				exit_confirm = false;
				Present(selected, status, connected, sidebar_focused, true, false);
				Threading::Sleep(16);
				continue;
			}
			if ((pressed & ORBIS_PAD_BUTTON_TRIANGLE) && view == LibraryView::Disc)
				probe_disc(false);
			else if (pressed & ORBIS_PAD_BUTTON_TRIANGLE)
			{
				const std::string old_path = items[selected].path;
				library = Scan();
				library_cover_index = 0;
				items = BuildView(library, view, favorites, recent);
				pump_library_cover();
				const auto old_item = std::find_if(items.begin(), items.end(), [&](const Item& item) { return item.path == old_path; });
				selected = old_item == items.end() ? 0 : static_cast<size_t>(old_item - items.begin());
				Publish(items, view);
				status.clear();
			}
			if (pressed & (ORBIS_PAD_BUTTON_L1 | ORBIS_PAD_BUTTON_R1))
			{
				constexpr unsigned view_count = 5;
				unsigned next = static_cast<unsigned>(view);
				next = pressed & ORBIS_PAD_BUTTON_L1 ? (next + view_count - 1) % view_count : (next + 1) % view_count;
				set_view(static_cast<LibraryView>(next));
			}
			u32 navigation = 0;
			if (buttons & ORBIS_PAD_BUTTON_LEFT)
				navigation = ORBIS_PAD_BUTTON_LEFT;
			else if (buttons & ORBIS_PAD_BUTTON_RIGHT)
				navigation = ORBIS_PAD_BUTTON_RIGHT;
			else if (buttons & ORBIS_PAD_BUTTON_UP)
				navigation = ORBIS_PAD_BUTTON_UP;
			else if (buttons & ORBIS_PAD_BUTTON_DOWN)
				navigation = ORBIS_PAD_BUTTON_DOWN;
			if (connected && navigation == 0)
			{
				if (data.leftStick.x < 72)
					navigation = ORBIS_PAD_BUTTON_LEFT;
				else if (data.leftStick.x > 184)
					navigation = ORBIS_PAD_BUTTON_RIGHT;
				else if (data.leftStick.y < 72)
					navigation = ORBIS_PAD_BUTTON_UP;
				else if (data.leftStick.y > 184)
					navigation = ORBIS_PAD_BUTTON_DOWN;
			}
			if (navigation && (navigation != held_direction || now >= next_repeat))
			{
				if (sidebar_focused)
				{
					unsigned next = static_cast<unsigned>(view);
					if (navigation == ORBIS_PAD_BUTTON_UP && next > 0)
						set_view(static_cast<LibraryView>(next - 1));
					else if (navigation == ORBIS_PAD_BUTTON_DOWN && next < 4)
						set_view(static_cast<LibraryView>(next + 1));
					else if (navigation == ORBIS_PAD_BUTTON_RIGHT)
						sidebar_focused = false;
				}
				else if (view == LibraryView::Settings)
				{
					if (navigation == ORBIS_PAD_BUTTON_UP && selected > 0)
						--selected;
					else if (navigation == ORBIS_PAD_BUTTON_DOWN && selected + 1 < items.size())
						++selected;
					else if (navigation == ORBIS_PAD_BUTTON_LEFT || navigation == ORBIS_PAD_BUTTON_RIGHT)
						edit_launcher_setting(navigation == ORBIS_PAD_BUTTON_LEFT ? -1 : 1);
				}
				else
				{
					const size_t column = selected % GridColumns;
					const size_t row = selected / GridColumns;
					const size_t row_count = (items.size() + GridColumns - 1) / GridColumns;
					if (navigation == ORBIS_PAD_BUTTON_LEFT && column == 0)
						sidebar_focused = true;
					else if (navigation == ORBIS_PAD_BUTTON_LEFT)
						--selected;
					else if (navigation == ORBIS_PAD_BUTTON_RIGHT && column + 1 < GridColumns && selected + 1 < items.size())
						++selected;
					else if (navigation == ORBIS_PAD_BUTTON_UP && selected >= GridColumns)
						selected -= GridColumns;
					else if (navigation == ORBIS_PAD_BUTTON_DOWN && row + 1 < row_count)
						selected = std::min(selected + GridColumns, items.size() - 1);
				}
				next_repeat = now + (navigation == held_direction ? 0.10 : 0.35);
				if (view != LibraryView::Settings)
					status.clear();
			}
			held_direction = navigation;
			if ((pressed & ORBIS_PAD_BUTTON_SQUARE) && !sidebar_focused && view != LibraryView::Disc)
			{
				const std::string selected_path = items[selected].path;
				if (!selected_path.empty())
				{
					const auto found = std::find(favorites.begin(), favorites.end(), selected_path);
					const bool removing = found != favorites.end();
					if (removing)
						favorites.erase(found);
					else
						favorites.push_back(selected_path);
					status = SavePathList(FavoritesFile, favorites) ?
					             (removing ? "Retire des favoris." : "Ajoute aux favoris.") :
					             "Sauvegarde favoris impossible.";
					items = BuildView(library, view, favorites, recent);
					const auto same_item = std::find_if(items.begin(), items.end(), [&](const Item& item) { return item.path == selected_path; });
					selected = same_item == items.end() ? std::min(selected, items.size() - 1) : static_cast<size_t>(same_item - items.begin());
					Publish(items, view);
				}
			}
			if (pressed & ORBIS_PAD_BUTTON_CROSS)
			{
				if (sidebar_focused)
				{
					sidebar_focused = false;
					continue;
				}
				if (view == LibraryView::Settings)
				{
					edit_launcher_setting(1);
					Present(selected, status, connected, false, false, false);
					continue;
				}
				if (view == LibraryView::Disc)
				{
					// Re-probe before launch, so an ejected/replaced DVD cannot use old metadata.
					probe_disc(true);
					Present(selected, status, connected, false, false, false);
					Threading::Sleep(16);
					continue;
				}
				path = items[selected].path;
				struct stat info{};
				if (path.empty())
					status = view == LibraryView::Disc     ? "Support disque en preparation." :
					         view == LibraryView::Settings ? "Options de reglage ici." :
					                                         "Aucun jeu dans cette vue.";
				else if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0)
					status = "Fichier indisponible. Triangle pour actualiser.";
				else
				{
					// (AI-assisted) Keep an unreadable/unsupported image in the launcher
					// with an error, instead of tearing down the UI and exiting the app.
					InputIsoFile image;
					Error image_error;
					if (!image.Open(path, &image_error))
					{
						status = "Ouverture impossible : " + image_error.GetDescription();
						ProbeLog("Game menu: launch rejected %s: %s\n", path.c_str(), image_error.GetDescription().c_str());
						Present(selected, status, connected, false, false, false);
						Threading::Sleep(16);
						continue;
					}
					image.Close();
					recent.erase(std::remove(recent.begin(), recent.end(), path), recent.end());
					recent.insert(recent.begin(), path);
					if (recent.size() > 24)
						recent.resize(24);
					if (!SavePathList(RecentFile, recent))
						ProbeLog("Game menu: could not save recent list\n");
					Present(selected, "Demarrage...", connected, false, false, false);
					// Consume the confirmation before handing the controller to the game.
					while (scePadReadState(pad, &data) == 0 && data.connected && (data.buttons & ORBIS_PAD_BUTTON_CROSS))
						Threading::Sleep(16);
					start = true;
					ProbeLog("Game menu: selected %s\n", path.c_str());
					platform = items[selected].platform;
					break;
				}
			}
			Present(selected, connected ? (view == LibraryView::Disc && status.empty() ? disc_status : status) : "Connecter une manette pour choisir un jeu.", connected,
				sidebar_focused, false, false);
			Threading::Sleep(16);
		}
		// Never destroy a live probe or leave a detached thread using menu state.
		disc_cover.Cancel();
		while (disc_cover.Active() && !disc_cover.Collect())
		{
			Present(selected, "Fin chargement jaquette...", true, sidebar_focused, false, false);
			Threading::Sleep(16);
		}
		// The driver timeout is requested per command; hardware behavior needs proper testing.
		while (disc_probe.active && !disc_probe.Collect())
		{
			Present(selected, "Fin detection disque...", true, sidebar_focused, false, false);
			Threading::Sleep(16);
		}
		if (start && path == PS4Disc::DevicePath)
		{
			OrbisPadData released{};
			while (scePadReadState(pad, &released) == 0 && released.connected && (released.buttons & ORBIS_PAD_BUTTON_CROSS))
				Threading::Sleep(16);
		}
		MTGS::RunOnGSThread([start]() {
			s_visible = false;
			s_sidebar_focused = false;
			s_exit_dialog_visible = false;
			s_exit_confirm = false;
			s_items.clear();
			s_status.clear();
			s_loading_visible = start;
			s_loading_carousel = false;
			s_loading_progress = start ? 10u : 0u;
			s_loading_status = start ? "Preparation emulateur..." : "";
			ClearCoverTextures();
			if (start)
				GSPresentCurrentFrame();
		});
		MTGS::WaitGS(false);
		return start;
	}

	bool Render()
	{
		if (s_intro_visible)
		{
			ImDrawList* draw = ImGui::GetBackgroundDrawList();
			const ImVec2 size = ImGui::GetIO().DisplaySize;
			draw->AddRectFilled(ImVec2(0, 0), size, IM_COL32(0, 0, 0, 255));
			if (ImTextureData* frame = GetIntroTexture())
			{
				const float scale = std::min(size.x / static_cast<float>(frame->Width), size.y / static_cast<float>(frame->Height));
				const ImVec2 frame_size(frame->Width * scale, frame->Height * scale);
				const ImVec2 origin((size.x - frame_size.x) * 0.5f, (size.y - frame_size.y) * 0.5f);
				draw->AddImage(frame->GetTexRef(), origin, ImVec2(origin.x + frame_size.x, origin.y + frame_size.y));
			}
			return true;
		}
		if (s_loading_visible)
		{
			ImDrawList* draw = ImGui::GetBackgroundDrawList();
			ImFont* font = ImGui::GetFont();
			const ImVec2 size = ImGui::GetIO().DisplaySize;
			const float scale = std::min(size.x / 1920.0f, size.y / 1080.0f);
			const ImVec2 origin((size.x - 1920.0f * scale) * 0.5f, (size.y - 1080.0f * scale) * 0.5f);
			const auto point = [&](float x, float y) { return ImVec2(origin.x + x * scale, origin.y + y * scale); };
			draw->AddRectFilled(ImVec2(0, 0), size, IM_COL32(2, 7, 15, 255));
			if (ImTextureData* background = GetCoverTexture(FindUiAsset("loading_background")))
				draw->AddImage(background->GetTexRef(), point(0, 0), point(1920, 1080));
			if (s_loading_carousel)
			{
				constexpr float travel = 2380.0f;
				constexpr float spacing = 520.0f;
				constexpr float art_size = 220.0f;
				for (unsigned index = 0; index < 4; index++)
				{
					const float x = std::fmod(s_loading_carousel_phase + static_cast<float>(index) * spacing, travel) - 230.0f;
					char name[32];
					std::snprintf(name, sizeof(name), "loading_carousel_%u", index + 1);
					if (ImTextureData* art = GetCoverTexture(FindUiAsset(name)))
						draw->AddImage(art->GetTexRef(), point(x, 475), point(x + art_size, 475 + art_size),
							ImVec2(0, 0), ImVec2(1, 1), IM_COL32(225, 240, 255, 235));
				}
			}

			// (AI-assisted) CyberTV-style loading bar: compact dark rail, neon glow,
			// blue/cyan fill, highlight and a real stage percentage. Needs proper testing
			// on a 720p console output in addition to the normal 1080p path.
			constexpr float bx = 630.0f, bw = 660.0f, bh = 14.0f;
			const float by = s_loading_carousel ? 748.0f : 674.0f;
			const float progress = static_cast<float>(std::min(s_loading_progress, 100u)) / 100.0f;
			const float fill_width = std::max(0.0f, (bw - 4.0f) * progress);
			for (int glow = 4; glow >= 1; --glow)
			{
				const float spread = static_cast<float>(glow) * 5.0f;
				draw->AddRectFilled(point(bx - spread, by - spread), point(bx + bw + spread, by + bh + spread),
					IM_COL32(0, 126, 255, 12 + (4 - glow) * 7), (7.0f + spread) * scale);
			}
			draw->AddRectFilled(point(bx, by), point(bx + bw, by + bh), IM_COL32(5, 18, 35, 245), 7.0f * scale);
			draw->AddRect(point(bx, by), point(bx + bw, by + bh), IM_COL32(49, 94, 143, 235), 7.0f * scale,
				1.0f * scale, 0);
			if (fill_width > 0.0f)
			{
				draw->AddRectFilledMultiColor(point(bx + 2, by + 2), point(bx + 2 + fill_width, by + bh - 2),
					IM_COL32(0, 88, 255, 255), IM_COL32(0, 220, 255, 255),
					IM_COL32(0, 178, 255, 255), IM_COL32(34, 126, 255, 255));
				if (fill_width > 8.0f)
					draw->AddRectFilled(point(bx + 4, by + 3), point(bx + fill_width - 2, by + 6),
						IM_COL32(220, 250, 255, 125), 1.5f * scale);
			}
			char percentage[16];
			std::snprintf(percentage, sizeof(percentage), "%u%%", std::min(s_loading_progress, 100u));
			const float percent_width = font->CalcTextSizeA(17.0f * scale, FLT_MAX, 0.0f, percentage).x / scale;
			draw->AddText(font, 17.0f * scale, point(960.0f - percent_width * 0.5f, by + 29),
				IM_COL32(190, 218, 241, 255), percentage);
			if (!s_loading_status.empty())
			{
				const float status_width = font->CalcTextSizeA(18.0f * scale, FLT_MAX, 0.0f, s_loading_status.c_str()).x / scale;
				draw->AddText(font, 18.0f * scale, point(960.0f - status_width * 0.5f, by + 62),
					IM_COL32(213, 232, 249, 255), s_loading_status.c_str());
			}
			return true;
		}
		const auto menu_state = UnpackInGameMenuState(s_in_game_state.load(std::memory_order_acquire));
		if (menu_state.visible)
		{
			const unsigned selected_row = menu_state.selected;
			const unsigned max_upscale = menu_state.max_upscale;
			const unsigned upscale = menu_state.upscale;
			const unsigned aspect = menu_state.aspect;
			const bool show_performance_overlay = menu_state.show_overlay;
			const unsigned ee_profile = menu_state.ee_profile;
			const bool async_audio = menu_state.async_audio;
			const unsigned ee_skip = menu_state.ee_skip;
			const unsigned frame_queue = menu_state.frame_queue;
			ImDrawList* draw = ImGui::GetBackgroundDrawList();
			ImFont* font = ImGui::GetFont();
			const ImVec2 size = ImGui::GetIO().DisplaySize;
			const float scale = std::min(size.x / 1920.0f, size.y / 1080.0f);
			const auto point = [scale](float x, float y) { return ImVec2(x * scale, y * scale); };
			const auto image = [&](const char* name, float x, float y, float w, float h) {
				if (ImTextureData* texture = GetCoverTexture(FindUiAsset(name)))
					draw->AddImage(texture->GetTexRef(), point(x, y), point(x + w, y + h));
			};
			const ImU32 white = IM_COL32(239, 246, 253, 255);
			const ImU32 muted = IM_COL32(145, 171, 193, 255);
			const ImU32 accent = IM_COL32(45, 153, 255, 255);
			const ImU32 panel = IM_COL32(7, 24, 40, 218);
			draw->AddRectFilled(ImVec2(0, 0), size, IM_COL32(0, 5, 14, 158));
			draw->AddRectFilled(point(64, 38), point(1856, 1042), panel, 24.0f * scale);
			draw->AddRect(point(64, 38), point(1856, 1042), IM_COL32(36, 127, 204, 235), 24.0f * scale,
				2.0f * scale, 0);
			draw->AddRectFilled(point(64, 38), point(1856, 170), IM_COL32(4, 19, 34, 205), 24.0f * scale);
			draw->AddLine(point(64, 170), point(1856, 170), IM_COL32(38, 103, 157, 150), 1.0f * scale);
			image("status_controle", 126, 82, 40, 34);
			draw->AddText(font, 34.0f * scale, point(196, 82), white, "Menu emulateur");
			draw->AddText(font, 18.0f * scale, point(138, 205), muted, "JEU EN PAUSE");
			char resolution[96];
			std::snprintf(resolution, sizeof(resolution), "Resolution interne :  < %ux >%s", upscale,
				upscale == 1 ? "  (native)" : "");
			static constexpr const char* aspect_names[] = {"Stretch", "Auto", "4:3", "16:9", "10:7"};
			char aspect_label[96];
			std::snprintf(aspect_label, sizeof(aspect_label), "Format image :  < %s >", aspect_names[std::min(aspect, 4u)]);
			char performance_label[96];
			std::snprintf(performance_label, sizeof(performance_label), "Affichage perf :  < %s >",
				show_performance_overlay ? "On" : "Off");
			char ee_label[96];
			static constexpr const char* ee_names[] = {"Normal", "Equilibre", "Performance"};
			std::snprintf(ee_label, sizeof(ee_label), "Cycles EE : < %s >", ee_names[std::min(ee_profile, 2u)]);
			char audio_label[96];
			std::snprintf(audio_label, sizeof(audio_label), "Synchro audio : < %s >", async_audio ? "AsyncMix" : "TimeStretch");
			char skip_label[96];
			std::snprintf(skip_label, sizeof(skip_label), "EE cycle skip: < %s >", ee_skip ? "Leger" : "Off");
			char queue_label[96];
			std::snprintf(queue_label, sizeof(queue_label), "File d'images : < %u >", frame_queue);
			const char* labels[] = {"Reprendre jeu", resolution, aspect_label, performance_label,
				ee_label, skip_label, queue_label, audio_label, "Retour menu principal", "Quitter application"};
			const char* descriptions[] = {
				"Reprendre emulation depuis image actuelle",
				"1x prioritizes FPS; 2x balances sharpness; 3x/4x cost more GPU time",
				"Choisir format affichage du rendu GS",
				"Garder FPS et vitesse visibles pour comparer",
				"Normal = accurate; Balanced = mild EE underclock; Performance = stronger underclock (may cause slow motion)",
				"Off = accurate; Light may improve some CPU-limited games but can stutter (next launch)",
				"1 = less latency; 2 = default; 3 = deeper buffering (next launch)",
				"Audio TimeStretch ou AsyncMix (prochain lancement)",
				"Fermer jeu et rouvrir bibliotheque PS4SX2",
				"Fermer jeu et quitter PS4SX2"};
			const auto draw_option_icon = [&](unsigned row, float cx, float cy, bool selected) {
				const ImU32 icon_bg = selected ? IM_COL32(34, 127, 214, 255) : IM_COL32(16, 55, 86, 255);
				const ImU32 icon_line = selected ? IM_COL32(231, 247, 255, 255) : IM_COL32(155, 204, 235, 255);
				draw->AddCircleFilled(point(cx, cy), 29.0f * scale, icon_bg, 32);
				draw->AddCircle(point(cx, cy), 29.0f * scale, selected ? accent : IM_COL32(49, 111, 155, 255), 32, 1.5f * scale);
				if (row == 0)
				{
					draw->AddTriangleFilled(point(cx - 7, cy - 11), point(cx - 7, cy + 11), point(cx + 13, cy), icon_line);
				}
				else if (row == 1)
				{
					draw->AddRect(point(cx - 13, cy - 13), point(cx + 13, cy + 13), icon_line, 3 * scale, 2 * scale, 0);
					draw->AddLine(point(cx - 7, cy + 6), point(cx + 7, cy + 6), icon_line, 2 * scale);
				}
				else if (row == 2)
				{
					draw->AddRect(point(cx - 15, cy - 10), point(cx + 15, cy + 10), icon_line, 3 * scale, 2 * scale, 0);
					draw->AddLine(point(cx - 8, cy - 15), point(cx + 8, cy - 15), icon_line, 2 * scale);
				}
				else if (row == 3)
				{
					draw->AddRect(point(cx - 15, cy - 9), point(cx + 15, cy + 9), icon_line, 3 * scale, 2 * scale, 0);
					draw->AddLine(point(cx - 9, cy), point(cx + 9, cy), icon_line, 2 * scale);
					draw->AddCircle(point(cx, cy), 4 * scale, icon_line, 16, 2 * scale);
				}
				else if (row == 4)
				{
					draw->AddLine(point(cx - 12, cy), point(cx + 10, cy), icon_line, 3 * scale);
					draw->AddLine(point(cx - 12, cy), point(cx - 2, cy - 9), icon_line, 3 * scale);
					draw->AddLine(point(cx - 12, cy), point(cx - 2, cy + 9), icon_line, 3 * scale);
				}
				else
				{
					draw->AddLine(point(cx - 10, cy - 12), point(cx + 10, cy + 12), icon_line, 3 * scale);
					draw->AddLine(point(cx + 10, cy - 12), point(cx - 10, cy + 12), icon_line, 3 * scale);
				}
			};
			for (unsigned row = 0; row < 10; row++)
			{
				const float y = 194.0f + row * 58.0f;
				const bool selected = selected_row == row;
				draw->AddRectFilled(point(112, y), point(1808, y + 57),
					selected ? IM_COL32(20, 77, 125, 255) : IM_COL32(11, 33, 52, 255), 10.0f * scale);
				if (selected)
					draw->AddRect(point(112, y), point(1808, y + 57), accent, 10.0f * scale, 2.0f * scale, 0);
				draw_option_icon(row, 168, y + 32, selected);
				draw->AddText(font, 26.0f * scale, point(226, y + 10), white, labels[row]);
				draw->AddText(font, 16.0f * scale, point(226, y + 43), muted, descriptions[row]);
				if (row >= 1 && row <= 7)
					draw->AddText(font, 20.0f * scale, point(1580, y + 29), selected ? white : muted,
						row == 1 ? "REGLER" : (row == 3 ? "BASCULER" : "CHOISIR"));
			}
			char hint[128];
			std::snprintf(hint, sizeof(hint), "Gauche/Droite : changer valeur   |   Resolution : 1x-%ux", max_upscale);
		draw->AddText(font, 17.0f * scale, point(138, 790), muted, hint);
		draw->AddText(font, 17.0f * scale, point(138, 816), muted, "For demanding games (Prince of Persia / Downhill), test 1x before 2x-4x.");
		draw->AddText(font, 17.0f * scale, point(138, 842), muted, "Up/Down: select   Left/Right: change   Square: performance preset   Circle: resume");
			image("controle_x", 1480, 904, 30, 30);
			draw->AddText(font, 20.0f * scale, point(1524, 909), white, "Valider");
			image("controle_bola", 1690, 904, 30, 30);
			draw->AddText(font, 20.0f * scale, point(1734, 909), muted, "Reprendre");
			return true;
		}
		if (!s_visible || s_items.empty())
			return false;
		// (AI-assisted) The launcher is drawn by the same Vulkan/ImGui device as
		// the emulator. Keep this path free of file I/O so a library refresh never
		// stalls the GS thread. Cover files will plug into the art slot later.
		ImDrawList* draw = ImGui::GetBackgroundDrawList();
		ImFont* font = ImGui::GetFont();
		const ImVec2 size = ImGui::GetIO().DisplaySize;
		const float scale = std::min(size.x / 1920.0f, size.y / 1080.0f);
		const ImVec2 origin((size.x - 1920 * scale) * 0.5f, (size.y - 1080 * scale) * 0.5f);
		const auto point = [&](float x, float y) { return ImVec2(origin.x + x * scale, origin.y + y * scale); };
		const auto fill = [&](float x, float y, float w, float h, ImU32 color, float radius = 12.0f) {
			draw->AddRectFilled(point(x, y), point(x + w, y + h), color, radius * scale);
		};
		const auto text = [&](float x, float baseline, float pixels, ImU32 color, const char* value) {
			draw->AddText(font, pixels * scale, point(x, baseline - pixels * 0.82f), color, value);
		};
		const auto centered_text = [&](float center_x, float baseline, float pixels, ImU32 color, const char* value) {
			const float width = font->CalcTextSizeA(pixels * scale, FLT_MAX, 0, value).x / scale;
			text(center_x - width * 0.5f, baseline, pixels, color, value);
		};
		const auto image = [&](const char* name, float x, float y, float w, float h) {
			if (ImTextureData* texture = GetCoverTexture(FindUiAsset(name)))
				draw->AddImage(texture->GetTexRef(), point(x, y), point(x + w, y + h));
		};
		const auto fit = [&](std::string value, float pixels, float width) {
			if (font->CalcTextSizeA(pixels * scale, FLT_MAX, 0, value.c_str()).x > width * scale)
			{
				do
				{
					size_t last = value.size() - 1;
					while (last > 0 && (static_cast<unsigned char>(value[last]) & 0xc0) == 0x80)
						--last;
					value.resize(last);
				} while (!value.empty() && font->CalcTextSizeA(pixels * scale, FLT_MAX, 0, (value + "…").c_str()).x > width * scale);
				value += "…";
			}
			return value;
		};
		const ImU32 white = IM_COL32(239, 246, 253, 255);
		const ImU32 muted = IM_COL32(145, 171, 193, 255);
		const ImU32 faint = IM_COL32(82, 117, 145, 255);
		const ImU32 accent = IM_COL32(45, 153, 255, 255);
		const ImU32 card_bg = IM_COL32(12, 33, 52, 255);
		const ImU32 art_a[] = {IM_COL32(29, 76, 116, 255), IM_COL32(92, 49, 58, 255),
			IM_COL32(104, 58, 35, 255), IM_COL32(38, 91, 87, 255), IM_COL32(72, 58, 120, 255)};
		const ImU32 art_b[] = {IM_COL32(8, 25, 46, 255), IM_COL32(23, 37, 63, 255),
			IM_COL32(33, 24, 47, 255), IM_COL32(14, 42, 46, 255), IM_COL32(25, 25, 67, 255)};
		draw->AddRectFilled(ImVec2(0, 0), size, IM_COL32(3, 13, 25, 255));
		// Layered navy background and restrained diagonal lighting give the menu
		// depth without adding a texture or a frame-time allocation.
		if (ImTextureData* backdrop = GetCoverTexture(FindUiAsset("library_background")))
		{
			// A full-bleed 16:9 background, drawn behind every interactive element.
			draw->AddImage(backdrop->GetTexRef(), point(0, 0), point(1920, 1080));
			// Readability scrims: retain artwork while protecting text legibility.
			fill(0, 0, 1920, 1080, IM_COL32(0, 8, 23, 55), 0);
		}
		else
		{
			draw->AddRectFilledMultiColor(point(0, 0), point(1920, 1080),
				IM_COL32(18, 19, 26, 255), IM_COL32(5, 7, 12, 255),
				IM_COL32(9, 11, 17, 255), IM_COL32(2, 3, 8, 255));
		}
		fill(0, 0, 286, 1080, IM_COL32(5, 6, 11, 175), 0);
		fill(286, 0, 1634, 92, IM_COL32(6, 7, 14, 170), 0);
		draw->AddLine(point(286, 92), point(1920, 92), IM_COL32(38, 103, 157, 110), 1.0f * scale);
		image("menu_logo", 32, 0, 220, 73);
		centered_text(142, 82, 11, muted, "PS4SX2 V12  /  PLAYSTATION 2");
		// Centered channel signature: the crown and luminous strokes are vector UI,
		// so no per-frame image decoding or GPU texture uploads are needed.
		// Lightweight animation: alpha pulses only, no new textures or allocations.
		const float glow_phase = static_cast<float>(ImGui::GetTime()) * 1.25f;
		const float glow = 0.5f + 0.5f * std::sin(glow_phase);
		const ImU32 crown_gold = IM_COL32(255, 198, 76, 255);
		const ImU32 neon_glow = IM_COL32(48, 163, 255, static_cast<int>(95 + 105 * glow));
		draw->AddLine(point(805, 79), point(1090, 79), neon_glow, (2.0f + 1.5f * glow) * scale);
		fill(700, 7, 565, 77, IM_COL32(4, 17, 41, 210), 12);
		draw->AddRect(point(700, 7), point(1265, 84), IM_COL32(37, 140, 255, 195), 12 * scale, 1.5f * scale);
		const ImVec2 crown_points[] = {point(728, 51), point(718, 20), point(741, 36),
			point(755, 12), point(770, 36), point(793, 20), point(783, 51)};
		for (int i = 0; i < 6; ++i)
			draw->AddLine(crown_points[i], crown_points[i + 1], crown_gold, 3.0f * scale);
		draw->AddLine(point(728, 53), point(783, 53), crown_gold, 4.0f * scale);
		text(814, 29, 15, IM_COL32(190, 221, 255, 255), "POD DO");
		text(807, 68, 43, IM_COL32(239, 248, 255, 255), "HIMURA");
		draw->AddLine(point(812, 74), point(1085, 74), IM_COL32(23, 173, 255, 245), 3.0f * scale);
		text(1100, 62, 16, accent, "BIBLIOTHEQUE PS2");
		text(1540, 40, 30, white, "PS4SX2");
		text(1705, 64, 32, accent, "V12");

		const size_t selected = std::min(s_selected, s_items.size() - 1);
		const Item& current = s_items[selected];
		// The selected game title lives in the details panel, leaving room for the channel logo.
		if (PS4Release::HasUpdate())
		{
			fill(1480, 22, 408, 50, IM_COL32(8, 54, 91, 235), 10);
			draw->AddRect(point(1480, 22), point(1888, 72), IM_COL32(42, 163, 255, 225), 10 * scale,
				1.0f * scale, 0);
			char update_text[96];
			std::snprintf(update_text, sizeof(update_text), "Mise a jour dispo : %s", PS4Release::LatestVersion());
			text(1502, 54, 17, white, update_text);
		}
		// The sidebar changes the data source while retaining the same grid.
		static constexpr const char* sidebar_icons[] = {
			"menu_biblioteca", "menu_disc", "menu_recentes", "menu_favoritos", "menu_configuracoes"};
		static constexpr const char* sidebar_labels[] = {"Bibliotheque", "Disque", "Recents", "Favoris", "Reglages"};
		const unsigned active_view = static_cast<unsigned>(s_library_view);
		for (unsigned i = 0; i < 5; ++i)
		{
			const float y = 164.0f + static_cast<float>(i) * 72.0f;
			const bool active = i == active_view;
			if (active)
			{
				fill(24, y, 228, 58, s_sidebar_focused ? IM_COL32(19, 91, 158, 255) : IM_COL32(13, 62, 109, 225), 10);
				draw->AddRect(point(24, y), point(252, y + 58), s_sidebar_focused ? IM_COL32(47, 164, 255, 255) : IM_COL32(35, 116, 181, 210),
					10 * scale, s_sidebar_focused ? 2.0f * scale : 1.0f * scale, 0);
			}
			image(sidebar_icons[i], 43, y + 12, 34, 34);
			text(91, y + 37, active ? 19 : 18, active ? white : muted, sidebar_labels[i]);
		}
		// A genuine settings screen with interactive controls, not placeholder cards.
		if (s_library_view == LibraryView::Settings)
		{
			fill(294, 128, 1592, 848, IM_COL32(3, 17, 40, 224), 16);
			draw->AddRect(point(294, 128), point(1886, 976), IM_COL32(35, 126, 224, 190), 16 * scale);
			text(342, 181, 31, white, "REGLAGES EMULATEUR");
			text(342, 213, 17, muted, "Direcional: selecionar e alterar  |  X: proxima opcao  |  L1/R1: trocar aba");
			for (size_t i = 0; i < s_items.size(); ++i)
			{
				const float y = 235.0f + 84.0f * static_cast<float>(i);
				const bool focused = i == selected && !s_sidebar_focused;
				fill(336, y, 1502, 77, focused ? IM_COL32(18, 83, 151, 250) : IM_COL32(9, 37, 65, 235), 8);
				if (focused)
					draw->AddRect(point(336, y), point(1838, y + 77), accent, 8 * scale, 2 * scale);
				text(366, y + 31, 22, focused ? white : IM_COL32(211, 230, 250, 255), s_items[i].name.c_str());
				text(366, y + 59, 15, muted, s_items[i].serial.c_str());
				text(1770, y + 40, 19, focused ? white : muted, "< >");
			}
			if (!s_status.empty())
				text(345, 954, 16, accent, fit(s_status, 16, 1450).c_str());
			text(345, 1009, 17, muted, "Opcoes experimentais podem reduzir a compatibilidade. Teste jogo por jogo.");
			return true;
		}
		// PS2 V12 showcase: hero case, printed disc, game details and horizontal shelf.
		// All artwork uses the existing decoded-cover cache on the GS thread.
		const bool disc_view = s_library_view == LibraryView::Disc;
		const auto draw_cover = [&](const Item& item, float x, float y, float w, float h) {
			fill(x, y, w, h, IM_COL32(5, 16, 37, 165), 2);
			if (ImTextureData* art = GetCoverTexture(item.cover))
			{
				const float ratio = std::min(w / static_cast<float>(art->Width), h / static_cast<float>(art->Height));
				const float iw = art->Width * ratio, ih = art->Height * ratio;
				draw->AddImage(art->GetTexRef(), point(x + (w - iw) * 0.5f, y + (h - ih) * 0.5f),
					point(x + (w + iw) * 0.5f, y + (h + ih) * 0.5f));
			}
			else
			{
				draw->AddRectFilledMultiColor(point(x, y), point(x + w, y + h),
					IM_COL32(34, 74, 123, 255), IM_COL32(8, 21, 45, 255),
					IM_COL32(8, 21, 45, 255), IM_COL32(24, 48, 85, 255));
				text(x + 14, y + h * 0.5f, 19, white, fit(item.name, 19, w - 28).c_str());
			}
		};
		if (!disc_view && !current.path.empty())
		{
			// The printed disc is circular and uses the same art as the game case.
			constexpr float case_x = 326, case_y = 154, case_w = 347, case_h = 448;
			fill(case_x - 4, case_y - 4, case_w + 8, case_h + 8, IM_COL32(2, 12, 30, 135), 5);
			draw->AddRect(point(case_x - 4, case_y - 4), point(case_x + case_w + 4, case_y + case_h + 4),
				IM_COL32(31, 150, 255, 200), 5 * scale, 1.5f * scale);
			draw_cover(current, case_x, case_y + 37, case_w, case_h - 37);
			fill(case_x, case_y, case_w, 37, IM_COL32(5, 9, 19, 255), 0);
			text(case_x + 16, case_y + 28, 23, white, "PlayStation 2");
			constexpr float dx = 697, dy = 210, diameter = 360;
			const ImVec2 center = point(dx + diameter * 0.5f, dy + diameter * 0.5f);
			draw->AddCircleFilled(point(dx + diameter * 0.5f + 6, dy + diameter * 0.5f + 9),
				(diameter * 0.5f + 3) * scale, IM_COL32(0, 0, 0, 145), 96);
			draw->AddCircleFilled(center, diameter * 0.5f * scale, IM_COL32(31, 33, 40, 255), 96);
			if (ImTextureData* art = GetCoverTexture(current.cover))
			{
				const float w = static_cast<float>(art->Width), h = static_cast<float>(art->Height);
				const float side = std::min(w, h);
				draw->AddImageRounded(art->GetTexRef(), point(dx + 5, dy + 5),
					point(dx + diameter - 5, dy + diameter - 5),
					ImVec2((w - side) / (2 * w), (h - side) / (2 * h)),
					ImVec2((w + side) / (2 * w), (h + side) / (2 * h)),
					IM_COL32(255, 255, 255, 255), 172 * scale);
			}
			// Printed-disc edge: neutral silver instead of the old blue center.
			draw->AddCircle(center, 172 * scale, IM_COL32(185, 191, 203, 255), 96, 2 * scale);
			draw->AddCircle(center, 168 * scale, IM_COL32(236, 240, 249, 70), 96, 1.0f * scale);
			draw->AddCircle(center, 164 * scale, IM_COL32(15, 19, 26, 120), 96, 1.0f * scale);
			if (!current.cover)
			{
				centered_text(dx + diameter * 0.5f, dy + 145, 23, white, "PLAYSTATION 2");
				centered_text(dx + diameter * 0.5f, dy + 235, 17, muted, "NO DISC ART");
			}
			draw->AddCircleFilled(center, 25 * scale, IM_COL32(24, 26, 31, 255), 48);
			draw->AddCircle(center, 25 * scale, IM_COL32(210, 213, 220, 255), 48, 3 * scale);
			draw->AddCircleFilled(center, 11 * scale, IM_COL32(1, 3, 7, 255), 32);
			text(1100, 224, 31, white, fit(current.name, 31, 440).c_str());
			text(1102, 268, 19, muted, "PLAYSTATION 2  /  GAME DETAILS");
			if (!current.serial.empty())
				text(1102, 320, 19, white, ("Serial: " + current.serial).c_str());
			const size_t dot = current.path.find_last_of('.');
			const std::string format = dot == std::string::npos ? "Disc image" : current.path.substr(dot + 1);
			text(1102, 362, 19, white, ("Format: " + format).c_str());
			text(1102, 404, 19, muted, "Local PS2 game image");
			text(1102, 469, 19, accent, "PER-GAME SETTINGS");
			text(1102, 508, 18, muted, "Resolution / EE profile: available in game");
			text(1102, 545, 18, muted, "Memory Card: PS2 virtual card");
			text(1102, 584, 17, accent, "Pause game + Square: cycle performance presets");
			// Right-side actions echo the reference artwork. These labels describe
			// existing controls only; no fake clickable settings are introduced.
			fill(1535, 258, 356, 330, IM_COL32(3, 17, 40, 216), 12);
			draw->AddRect(point(1535, 258), point(1891, 588),
				IM_COL32(30, 116, 215, 220), 12 * scale, 1.5f * scale);
			fill(1550, 276, 326, 57, IM_COL32(12, 89, 181, 225), 8);
			draw->AddRect(point(1550, 276), point(1876, 333), accent, 8 * scale, 2.0f * scale);
			text(1571, 312, 23, white, "X  INICIAR JOGO");
			text(1571, 382, 20, white, "O  Voltar / Sair");
			text(1571, 435, 20, white, "[]  Favoritos");
			text(1571, 488, 20, white, "/\\  Atualizar jogos");
			text(1571, 548, 16, muted, "Ajustes: pause durante o jogo");
		}
		else if (disc_view)
		{
			text(390, 270, 34, white, "Physical PS2 DVD");
			text(390, 324, 20, muted, current.path.empty() ?
				"Insert a PS2 disc and refresh." : "Disc detected. Press X to start.");
			draw_cover(current, 880, 180, 295, 412);
		}
		// Horizontal shelf with selected case highlighted, like the PS2 mockup.
		fill(286, 627, 1634, 331, IM_COL32(4, 5, 10, 145), 0);
		fill(300, 635, 1600, 3, IM_COL32(35, 127, 207, 110), 0);
		draw->AddLine(point(286, 627), point(1920, 627), IM_COL32(22, 141, 242, 200), 2 * scale);
		if (!disc_view)
		{
			const size_t shelf_count = std::min(VisibleItems, s_items.size());
			const size_t first = (selected / VisibleItems) * VisibleItems;
			constexpr float shelf_x = 306, shelf_y = 663, shelf_step = 200, shelf_w = 183, shelf_h = 248;
			for (size_t slot = 0; slot < shelf_count && first + slot < s_items.size(); ++slot)
			{
				const size_t index = first + slot;
				const float x = shelf_x + slot * shelf_step;
				const bool focused = index == selected;
				if (focused)
				{
					// Narrow selected-game halo; no bulky tile behind the cover.
					fill(x - 3, shelf_y - 5, shelf_w + 6, shelf_h + 10,
						IM_COL32(10, 65, 145, static_cast<int>(105 + 60 * glow)), 3);
					draw->AddRect(point(x - 3, shelf_y - 5), point(x + shelf_w + 3, shelf_y + shelf_h + 5),
						accent, 3 * scale, 2 * scale);
				}
				draw_cover(s_items[index], x, shelf_y + 24, shelf_w, shelf_h - 24);
				fill(x, shelf_y, shelf_w, 24, IM_COL32(4, 9, 19, 255), 0);
				text(x + 7, shelf_y + 19, 15, white, "PlayStation 2");
				if (s_items[index].favorite)
					image("menu_favoritos", x + shelf_w - 28, shelf_y + 29, 24, 24);
			}
		}
		if (!s_status.empty())
			text(346, 936, 17, accent, fit(s_status, 17, 1496).c_str());
		image("controle_x", 346, 1001, 30, 30);
		text(384, 1023, 16, white, "Start");
		if (!disc_view)
		{
			image("controle_quadrado", 500, 1001, 30, 30);
			text(538, 1023, 16, current.path.empty() ? muted : white,
				current.favorite ? "Remove favorite" : "Add favorite");
		}
		// Disc has no favorite action: use the next Library slots without an empty gap.
		const float refresh_x = disc_view ? 500.0f : 760.0f;
		const float exit_x = disc_view ? 760.0f : 1035.0f;
		image("controle_triangulo", refresh_x, 1001, 30, 30);
		text(refresh_x + 38, 1023, 16, white, disc_view ? "Refresh disc" : "Refresh library");
		image("controle_bola", exit_x, 1001, 30, 30);
		text(exit_x + 38, 1023, 16, muted, "Exit");
		if (s_exit_dialog_visible)
		{
			draw->AddRectFilled(ImVec2(0, 0), size, IM_COL32(0, 0, 0, 165));
			fill(398, 282, 1124, 516, IM_COL32(0, 0, 0, 120), 20);
			fill(438, 322, 1044, 436, IM_COL32(4, 17, 31, 252), 12);
			draw->AddRect(point(438, 322), point(1482, 758), IM_COL32(35, 105, 161, 225), 12 * scale,
				1.5f * scale, 0);
			image("menu_logo", 790, 338, 340, 113);
			centered_text(960, 480, 32, white, "Close PS4SX2?");
			centered_text(960, 520, 18, muted, "Are you sure you want to exit the application?");
			const auto dialog_button = [&](float x, const char* label, bool active) {
				fill(x, 580, 260, 72, active ? IM_COL32(18, 88, 153, 255) : IM_COL32(11, 34, 54, 255), 10);
				draw->AddRect(point(x, 580), point(x + 260, 652),
					active ? IM_COL32(48, 170, 255, 255) : IM_COL32(37, 80, 113, 190), 10 * scale,
					active ? 2.0f * scale : 1.0f * scale, 0);
				centered_text(x + 130, 626, 20, active ? white : muted, label);
			};
			dialog_button(670, "Exit PS4SX2", s_exit_confirm);
			dialog_button(990, "Cancel", !s_exit_confirm);
			image("controle_x", 780, 694, 28, 28);
			text(816, 715, 16, white, "Valider");
			image("controle_bola", 995, 694, 28, 28);
			text(1031, 715, 16, muted, "Back");
		}
		return true;
	}

	void SetInGameMenu(bool visible, unsigned selected, unsigned upscale, unsigned max_upscale, unsigned aspect,
		bool show_performance_overlay, unsigned ee_profile, bool async_audio,
		unsigned ee_skip, unsigned frame_queue)
	{
		const unsigned state = PackInGameMenuState({visible, selected, upscale, max_upscale, aspect,
			show_performance_overlay, ee_profile, async_audio, ee_skip, frame_queue});
		s_in_game_state.store(state, std::memory_order_release);
		// Paused emulation produces no VSync. Queue the redraw on the GS thread
		// so every value change is visible immediately in the panel.
		MTGS::RunOnGSThread([]() {
			GSPresentCurrentFrame();
		});
		MTGS::WaitGS(false);
	}

	void ShowLoading(unsigned progress, const char* status)
	{
		const std::string text = status ? status : "";
		MTGS::RunOnGSThread([progress, text]() {
			s_visible = false;
			s_loading_visible = true;
			s_loading_carousel = false;
			s_loading_progress = std::min(progress, 100u);
			s_loading_status = text;
			GSPresentCurrentFrame();
		});
		MTGS::WaitGS(false);
	}

	void HideLoading()
	{
		MTGS::RunOnGSThread([]() {
			s_loading_visible = false;
			s_loading_carousel = false;
			s_loading_progress = 0;
			s_loading_status.clear();
		});
		MTGS::WaitGS(false);
	}
} // namespace PS4GameMenu
