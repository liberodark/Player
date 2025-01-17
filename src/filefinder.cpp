/*
 * This file is part of EasyRPG Player.
 *
 * EasyRPG Player is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * EasyRPG Player is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with EasyRPG Player. If not, see <http://www.gnu.org/licenses/>.
 */

// Headers
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <string>
#include <vector>
#include <sstream>

#ifdef _WIN32
#  include <windows.h>
#  include <shlobj.h>
#endif

#if defined(USE_SDL) && defined(__ANDROID__)
#   include <jni.h>
#   include <SDL_system.h>
#endif

#include "system.h"
#include "options.h"
#include "utils.h"
#include "filefinder.h"
#include "fileext_guesser.h"
#include "output.h"
#include "player.h"
#include "registry.h"
#include "rtp.h"
#include "main_data.h"
#include <lcf/reader_util.h>
#include "platform.h"

#ifdef USE_LIBRETRO
#include "platform/libretro/libretro_ui.h"
#endif

// MinGW shlobj.h does not define this
#ifndef SHGFP_TYPE_CURRENT
#define SHGFP_TYPE_CURRENT 0
#endif

namespace {
#ifdef SUPPORT_MOVIES
	const char* const MOVIE_TYPES[] = { ".avi", ".mpg" };
#endif

	using search_path_list = std::vector<std::shared_ptr<FileFinder::DirectoryTree>>;

	std::shared_ptr<FileFinder::DirectoryTree> game_directory_tree;
	std::string fonts_path;

	struct {
		/** all RTP search paths */
		search_path_list search_paths;
		/** RTP was disabled with --disable-rtp */
		bool disable_rtp = true;
		/** Game has FullPackageFlag=1, RTP will still be used as RPG_RT does */
		bool game_has_full_package_flag = false;
		/** warning about "game has FullPackageFlag=1 but needs RTP" shown */
		bool warning_broken_rtp_game_shown = false;
		/** RTP candidates per search_path */
		std::vector<RTP::RtpHitInfo> detected_rtp;
		/** the RTP the game uses, when only one left the RTP of the game is known */
		std::vector<RTP::Type> game_rtp;
	} rtp_state;

	std::string FindFile(FileFinder::DirectoryTree const& tree,
										  const std::string& dir,
										  const std::string& name,
										  char const* exts[],
										  bool translate=false)
	{
		using namespace FileFinder;

		// Avoid searching entirely if there is no active translation
		if (translate && Tr::GetCurrentTranslationId().empty()) { return ""; }

#ifdef EMSCRIPTEN
		// The php filefinder should have given us an useable path
		std::string em_file = MakePath(dir, name);

		if (Exists(em_file))
			return em_file;
#endif

		std::string corrected_dir = lcf::ReaderUtil::Normalize(translate?Tr::GetTranslationDir():dir);
		std::string const escape_symbol = Player::escape_symbol;
		std::string corrected_name = lcf::ReaderUtil::Normalize(translate?MakePath(MakePath(Tr::GetCurrentTranslationId(), dir), name):name);

		std::string combined_path = MakePath(corrected_dir, corrected_name);
		std::string canon = MakeCanonical(combined_path, 1);
		if (combined_path != canon) {
			// Very few games (e.g. Yume2kki) use path traversal (..) in the filenames to point
			// to files outside of the actual directory.
			// Fix the path and continue searching.
			size_t pos = canon.find_first_of("/");
			if (pos == std::string::npos) {
				for (char const** c = exts; *c != NULL; ++c) {
					std::string res = FindDefault(tree, canon + *c);
					if (!res.empty()) {
						return res;
					}
				}
				return "";
			} else {
				corrected_dir = canon.substr(0, pos);
				corrected_name = canon.substr(pos + 1);
			}
		}

#ifdef _WIN32
		std::replace(corrected_name.begin(), corrected_name.end(), '/', '\\');
		if (escape_symbol != "\\") {
#endif
			std::size_t escape_pos = corrected_name.find(escape_symbol);
			while (escape_pos != std::string::npos) {
				corrected_name.erase(escape_pos, escape_symbol.length());
				corrected_name.insert(escape_pos, "/");
				escape_pos = corrected_name.find(escape_symbol);
			}
#ifdef _WIN32
		}
#endif

		string_map::const_iterator dir_it = tree.directories.find(corrected_dir);
		if(dir_it == tree.directories.end()) { return ""; }

		string_map const& dir_map = tree.sub_members.find(corrected_dir)->second;

		for(char const** c = exts; *c != NULL; ++c) {
			string_map::const_iterator const name_it = dir_map.find(corrected_name + *c);
			if(name_it != dir_map.end()) {
				return MakePath
					(std::string(tree.directory_path).append("/")
					 .append(dir_it->second), name_it->second);
			}
		}

		return "";
	}

	// returns empty string when the file is not belonging to an RTP
	const std::string rtp_lookup(const std::string& dir, const std::string& name, const char* exts[], bool& is_rtp_asset) {
		int version = Player::EngineVersion();

		auto normal_search = [&]() -> std::string {
			is_rtp_asset = false;
			for (const auto& path : rtp_state.search_paths) {
				const std::string ret = FindFile(*path, dir, name, exts);
				if (!ret.empty()) {
					return ret;
				}
			}
			return std::string();
		};

		// Detect the RTP version the game uses, when only one candidate is left the RTP is known
		if (rtp_state.game_rtp.size() != 1) {
			auto candidates = RTP::LookupAnyToRtp(dir, name, version);

			// Prevent Don Miguel RTP addon data from being detected as game RTP because a game can only have one RTP
			// and using this one will break the whole lookup table logic.
			auto addon_it = std::find(candidates.begin(), candidates.end(), RTP::Type::RPG2000_DonMiguelAddon);
			if (addon_it != candidates.end()) {
				candidates.erase(addon_it);
			}

			// when empty the requested asset does not belong to any (known) RTP
			if (!candidates.empty()) {
				if (rtp_state.game_rtp.empty()) {
					rtp_state.game_rtp = candidates;
				} else {
					// Strategy: Remove all RTP that are not candidates by comparing with all previous candidates
					// as the used RTP can only be the one that contains all by now requested assets
					for (auto it = rtp_state.game_rtp.begin(); it != rtp_state.game_rtp.end();) {
						if (std::find(candidates.begin(), candidates.end(), *it) == candidates.end()) {
							it = rtp_state.game_rtp.erase(it);
						} else {
							++it;
						}
					}
				}

				if (rtp_state.game_rtp.size() == 1) {
					// From now on the RTP lookups should be perfect
					Output::Debug("Game uses RTP \"{}\"", RTP::Names[(int) rtp_state.game_rtp[0]]);
				}
			}
		}

		if (rtp_state.game_rtp.empty()) {
			// The game RTP is currently unknown because all requested assets by now were not in any RTP
			// -> fallback to direct search
			is_rtp_asset = false;
			return normal_search();
		}

		// Search across all RTP
		for (const auto& rtp : rtp_state.detected_rtp) {
			for (RTP::Type game_rtp : rtp_state.game_rtp) {
				std::string rtp_entry = RTP::LookupRtpToRtp(dir, name, game_rtp, rtp.type, &is_rtp_asset);
				if (!rtp_entry.empty()) {
					const std::string ret = FindFile(*rtp.tree, dir, rtp_entry, exts);
					if (!ret.empty()) {
						is_rtp_asset = true;
						return ret;
					}
				}
			}
		}

		// Asset is missing or not a RTP asset -> fallback to direct search
		return normal_search();
	}

	std::string FindFile(const std::string &dir, const std::string& name, const char* exts[], bool tryTranslate=false) {
		// Search for translated resources first.
		const std::shared_ptr<FileFinder::DirectoryTree> tree = FileFinder::GetDirectoryTree();
		if (tryTranslate) {
			std::string ret = FindFile(*tree, dir, name, exts, true);
			if (!ret.empty()) {
				return ret;
			}
		}

		// Try without translating.
		std::string ret = FindFile(*tree, dir, name, exts, false);
		if (!ret.empty()) {
			return ret;
		}

		// True RTP if enabled and available
		if (!rtp_state.disable_rtp) {
			bool is_rtp_asset;
			ret = rtp_lookup(lcf::ReaderUtil::Normalize(dir), lcf::ReaderUtil::Normalize(name), exts, is_rtp_asset);

			std::string lcase = lcf::ReaderUtil::Normalize(dir);
			bool is_audio_asset = lcase == "music" || lcase == "sound";

			if (is_rtp_asset) {
				if (!ret.empty() && rtp_state.game_has_full_package_flag && !rtp_state.warning_broken_rtp_game_shown && !is_audio_asset) {
					rtp_state.warning_broken_rtp_game_shown = true;
					Output::Warning("This game claims it does not need the RTP, but actually uses files from it!");
				} else if (ret.empty() && !rtp_state.game_has_full_package_flag && !is_audio_asset) {
					std::string msg = "Cannot find: {}/{}. " +
						std::string(rtp_state.search_paths.empty() ?
						"Install RTP {} to resolve this warning." : "RTP {} was probably not installed correctly.");
					Output::Warning(msg, dir, name, Player::EngineVersion());
				}
			}
		}

		if (ret.empty()) {
			Output::Debug("Cannot find: {}/{}", dir, name);
		}

		return ret;
	}
} // anonymous namespace

const std::shared_ptr<FileFinder::DirectoryTree> FileFinder::GetDirectoryTree() {
	return game_directory_tree;
}

const std::shared_ptr<FileFinder::DirectoryTree> FileFinder::CreateSaveDirectoryTree() {
	std::string save_path = Main_Data::GetSavePath();

	if (!(Exists(save_path) && IsDirectory(save_path, true))) { return std::shared_ptr<DirectoryTree>(); }

	std::shared_ptr<DirectoryTree> tree = std::make_shared<DirectoryTree>();
	tree->directory_path = save_path;

	Directory mem = GetDirectoryMembers(tree->directory_path, FILES);

	for (auto& i : mem.files) {
		tree->files[i.first] = i.second;
	}
	for (auto& i : mem.directories) {
		tree->directories[i.first] = i.second;
	}

	return tree;
}

void FileFinder::SetDirectoryTree(std::shared_ptr<DirectoryTree> directory_tree) {
	game_directory_tree = directory_tree;
}

std::shared_ptr<FileFinder::DirectoryTree> FileFinder::CreateDirectoryTree(const std::string& p, Mode mode) {
	if(! (Exists(p) && IsDirectory(p, true))) { return std::shared_ptr<DirectoryTree>(); }
	std::shared_ptr<DirectoryTree> tree = std::make_shared<DirectoryTree>();
	tree->directory_path = p;

	bool recursive = false;
	if (mode == RECURSIVE) {
		mode = ALL;
		recursive = true;
	}

	Directory mem = GetDirectoryMembers(tree->directory_path, mode);
	for (auto& i : mem.files) {
		tree->files[i.first] = i.second;
	}
	for (auto& i : mem.directories) {
		tree->directories[i.first] = i.second;
	}

	if (recursive) {
		for (auto& i : mem.directories) {
			GetDirectoryMembers(MakePath(tree->directory_path, i.second), RECURSIVE).files.swap(tree->sub_members[i.first]);
		}
	}
	return tree;
}

std::string FileFinder::MakePath(StringView dir, StringView name) {
	std::string str = dir.empty()? std::string(name) : std::string(dir) + "/" + std::string(name);
#ifdef _WIN32
	std::replace(str.begin(), str.end(), '/', '\\');
#else
	std::replace(str.begin(), str.end(), '\\', '/');
#endif
	return str;
}

std::string FileFinder::MakeCanonical(const std::string& path, int initial_deepness) {
	std::vector<std::string> path_components = SplitPath(path);
	std::vector<std::string> path_can;

	for (std::string path_comp : path_components) {
		if (path_comp == "..") {
			if (path_can.size() > 0) {
				path_can.pop_back();
			} else if (initial_deepness > 0) {
				// Ignore, we are in root
				--initial_deepness;
			} else {
				Output::Debug("Path traversal out of game directory: {}", path);
			}
		} else if (path_comp.empty() || path_comp == ".") {
			// ignore
		} else {
			path_can.push_back(path_comp);
		}
	}

	std::string ret;
	for (std::string s : path_can) {
		ret = MakePath(ret, s);
	}

	return ret;
}

std::vector<std::string> FileFinder::SplitPath(const std::string& path) {
	// Tokens are patch delimiters ("/" and encoding aware "\")
	std::function<bool(char32_t)> f = [](char32_t t) {
		char32_t escape_char_back = '\0';
		if (!Player::escape_symbol.empty()) {
			escape_char_back = Utils::DecodeUTF32(Player::escape_symbol).front();
		}
		char32_t escape_char_forward = Utils::DecodeUTF32("/").front();
		return t == escape_char_back || t == escape_char_forward;
	};
	return Utils::Tokenize(path, f);
}

std::string FileFinder::GetPathInsidePath(const std::string& path_to, const std::string& path_in) {
	if (!ToStringView(path_in).starts_with(path_to)) {
		return path_in;
	}

	std::string path_out = path_in.substr(path_to.size());
	if (!path_out.empty() && (path_out[0] == '/' || path_out[0] == '\\')) {
		path_out = path_out.substr(1);
	}

	return path_out;
}

std::string FileFinder::GetPathInsideGamePath(const std::string& path_in) {
	return FileFinder::GetPathInsidePath(GetDirectoryTree()->directory_path, path_in);
}

#if defined(_WIN32) && !defined(_ARM_)
std::string GetFontsPath() {
	static std::string fonts_path = "";
	static bool init = false;

	if (init) {
		return fonts_path;
	} else {
		// Retrieve the Path of the Font Directory
		TCHAR path[MAX_PATH];

		if (SHGetFolderPath(NULL, CSIDL_FONTS, NULL, SHGFP_TYPE_CURRENT, path) == S_OK)	{
			char fpath[MAX_PATH];
#ifdef UNICODE
			WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS | WC_COMPOSITECHECK, path, MAX_PATH, fpath, MAX_PATH, NULL, NULL);
#endif
			fonts_path = FileFinder::MakePath(fpath, "");
		}

		init = true;

		return fonts_path;
	}
}

std::string GetFontFilename(const std::string& name) {
	std::string real_name = Registry::ReadStrValue(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts", name + " (TrueType)");
	if (real_name.length() > 0) {
		if (FileFinder::Exists(real_name))
			return real_name;
		if (FileFinder::Exists(GetFontsPath() + real_name))
			return GetFontsPath() + real_name;
	}

	real_name = Registry::ReadStrValue(HKEY_LOCAL_MACHINE, "Software\\Microsoft\\Windows\\CurrentVersion\\Fonts", name + " (TrueType)");
	if (real_name.length() > 0) {
		if (FileFinder::Exists(real_name))
			return real_name;
		if (FileFinder::Exists(GetFontsPath() + real_name))
			return GetFontsPath() + real_name;
	}

	return name;
}
#endif

std::string FileFinder::FindFont(const std::string& name) {
	static const char* FONTS_TYPES[] = {
		".ttf", ".ttc", ".otf", ".fon", NULL, };
	std::string path = FindFile("Font", name, FONTS_TYPES);

#if defined(_WIN32) && !defined(_ARM_)
	if (!path.empty()) {
		return path;
	}

	std::string folder_path = "";
	std::string filename = name;

	size_t separator_pos = path.rfind('\\');
	if (separator_pos != std::string::npos) {
		folder_path = path.substr(0, separator_pos);
		filename = path.substr(separator_pos, path.length() - separator_pos);
	}

	std::string font_filename = GetFontFilename(filename);
	if (!font_filename.empty()) {
		if (FileFinder::Exists(folder_path + font_filename))
			return folder_path + font_filename;

		if (FileFinder::Exists(fonts_path + font_filename))
			return fonts_path + font_filename;
	}

	return "";
#else
	return path;
#endif
}

static void add_rtp_path(const std::string& p) {
	using namespace FileFinder;
	std::shared_ptr<DirectoryTree> tree(CreateDirectoryTree(p));
	if (tree) {
		Output::Debug("Adding {} to RTP path", p);
		rtp_state.search_paths.push_back(tree);

		auto hit_info = RTP::Detect(tree, Player::EngineVersion());

		if (hit_info.empty()) {
			Output::Debug("The folder does not contain a known RTP!");
		}

		// Only consider the best RTP hits (usually 100% if properly installed)
		float best = 0.0;
		for (const auto& hit : hit_info) {
			float rate = (float)hit.hits / hit.max;
			if (rate >= best) {
				Output::Debug("RTP is \"{}\" ({}/{})", hit.name, hit.hits, hit.max);
				rtp_state.detected_rtp.emplace_back(hit);
				best = rate;
			}
		}
	} else {
		Output::Debug("RTP path {} is invalid, not adding", p);
	}
}

#if defined(USE_WINE_REGISTRY) || defined(_WIN32)
static void read_rtp_registry(const std::string& company, const std::string& product, const std::string& key) {
	std::string rtp_path = Registry::ReadStrValue(HKEY_CURRENT_USER, "Software\\" + company + "\\" + product, key, KEY32);
	if (!rtp_path.empty()) {
		add_rtp_path(rtp_path);
	}

	rtp_path = Registry::ReadStrValue(HKEY_LOCAL_MACHINE, "Software\\" + company + "\\" + product, key, KEY32);
	if (!rtp_path.empty()) {
		add_rtp_path(rtp_path);
	}
}
#endif

void FileFinder::InitRtpPaths(bool no_rtp, bool no_rtp_warnings) {
	rtp_state = {};

#ifdef EMSCRIPTEN
	// No RTP support for emscripten at the moment.
	rtp_state.disable_rtp = true;
#else
	rtp_state.disable_rtp = no_rtp;
#endif
	rtp_state.game_has_full_package_flag = no_rtp_warnings;

	if (rtp_state.disable_rtp) {
		Output::Debug("RTP support is disabled.");
		return;
	}

	std::string const version_str =	Player::GetEngineVersion();
	assert(!version_str.empty());

#ifdef GEKKO
	add_rtp_path("sd:/data/rtp/" + version_str);
	add_rtp_path("usb:/data/rtp/" + version_str);
#elif defined(__SWITCH__)
	add_rtp_path("./rtp/" + version_str);
	add_rtp_path("/switch/easyrpg-player/rtp/" + version_str);
#elif defined(_3DS)
	add_rtp_path("romfs:/data/rtp/" + version_str);
	add_rtp_path("sdmc:/data/rtp/" + version_str);
#elif defined(PSP2)
	add_rtp_path("ux0:/data/easyrpg-player/rtp/" + version_str);
#elif defined(USE_LIBRETRO)
	const char* dir = nullptr;
	if (LibretroUi::environ_cb(RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY, &dir) && dir) {
		add_rtp_path(std::string(dir) + "/rtp/" + version_str);
	}
	if (LibretroUi::environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir) {
		add_rtp_path(std::string(dir) + "/rtp/" + version_str);
	}
#elif defined(__ANDROID__)
	// Invoke "String getRtpPath()" in EasyRPG Activity via JNI
	JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
	jobject sdl_activity = (jobject)SDL_AndroidGetActivity();
	jclass cls = env->GetObjectClass(sdl_activity);
	jmethodID jni_getRtpPath = env->GetMethodID(cls , "getRtpPath", "()Ljava/lang/String;");
	jstring return_string = (jstring)env->CallObjectMethod(sdl_activity, jni_getRtpPath);

	const char *js = env->GetStringUTFChars(return_string, NULL);
	std::string cs(js);

	env->ReleaseStringUTFChars(return_string, js);
	env->DeleteLocalRef(sdl_activity);
	env->DeleteLocalRef(cls);
	add_rtp_path(cs + "/" + version_str);
#elif defined(USE_WINE_REGISTRY) || defined(_WIN32)
	std::string const product = "RPG" + version_str;
	if (Player::IsRPG2k()) {
		// Prefer original 2000 RTP over Kadokawa, because there is no
		// reliable way to detect this engine and much more 2k games
		// use the non-English version
		read_rtp_registry("ASCII", product, "RuntimePackagePath");
		read_rtp_registry("KADOKAWA", product, "RuntimePackagePath");
	}
	else if (Player::IsRPG2k3E()) {
		// Prefer Kadokawa RTP over Enterbrain for new RPG2k3
		read_rtp_registry("KADOKAWA", product, "RuntimePackagePath");
		read_rtp_registry("Enterbrain", product, "RUNTIMEPACKAGEPATH");
	}
	else if (Player::IsRPG2k3()) {
		// Original 2003 RTP installer registry key is upper case
		// and Wine registry is case insensitive but new 2k3v1.10 installer is not
		// Prefer Enterbrain RTP over Kadokawa for old RPG2k3 (search order)
		read_rtp_registry("Enterbrain", product, "RUNTIMEPACKAGEPATH");
		read_rtp_registry("KADOKAWA", product, "RuntimePackagePath");
	}

	// Our RTP is for all engines
	read_rtp_registry("EasyRPG", "RTP", "path");
#else
	// Fallback for unknown platforms
	add_rtp_path("/data/rtp/" + version_str);
#endif
	std::vector<std::string> env_paths;

	// Windows paths are split by semicolon, Unix paths by colon
	std::function<bool(char32_t)> f = [](char32_t t) {
#ifdef _WIN32
		return t == ';';
#else
		return t == ':';
#endif
	};

	if (Player::IsRPG2k() && getenv("RPG2K_RTP_PATH"))
		env_paths = Utils::Tokenize(getenv("RPG2K_RTP_PATH"), f);
	else if (Player::IsRPG2k3() && getenv("RPG2K3_RTP_PATH"))
		env_paths = Utils::Tokenize(getenv("RPG2K3_RTP_PATH"), f);

	if (getenv("RPG_RTP_PATH")) {
		std::vector<std::string> tmp = Utils::Tokenize(getenv("RPG_RTP_PATH"), f);
		env_paths.insert(env_paths.end(), tmp.begin(), tmp.end());
	}

#ifdef USE_XDG_RTP
	std::string xdg_rtp;

	// Search in the local data directory
	xdg_rtp = getenv("XDG_DATA_HOME") ? std::string(getenv("XDG_DATA_HOME")) :
		std::string(getenv("HOME")) + "/.local/share";
	xdg_rtp += "/rtp/" + version_str;
	if (Exists(xdg_rtp)) {
		env_paths.push_back(xdg_rtp);
	}

	// Search in the global data directories
	xdg_rtp = getenv("XDG_DATA_DIRS") ? std::string(getenv("XDG_DATA_DIRS")) :
		std::string("/usr/local/share/:/usr/share/");
	std::vector<std::string> tmp = Utils::Tokenize(xdg_rtp, f);
	for (const std::string& p : tmp) {
		xdg_rtp = p + (p.back() == '/' ? "" : "/") + "rtp/" + version_str;
		if (Exists(xdg_rtp)) {
			env_paths.push_back(xdg_rtp);
		}
	}
#endif

	// Add all found paths from the environment
	for (const std::string& p : env_paths) {
		add_rtp_path(p);
	}
}

void FileFinder::Quit() {
	rtp_state = {};
	game_directory_tree.reset();
}


Filesystem_Stream::InputStream FileFinder::OpenInputStream(const std::string& name, std::ios_base::openmode m) {
	auto* buf = new std::filebuf();
	buf->open(
#ifdef _MSC_VER
		Utils::ToWideString(name).c_str(),
#else
		name.c_str(),
#endif
		m);

	if (!buf->is_open()) {
		delete buf;
		return Filesystem_Stream::InputStream();
	}

	Filesystem_Stream::InputStream is(buf);
	return is;
}

Filesystem_Stream::OutputStream FileFinder::OpenOutputStream(const std::string& name, std::ios_base::openmode m) {
	auto* buf = new std::filebuf();
	buf->open(
#ifdef _MSC_VER
		Utils::ToWideString(name).c_str(),
#else
		name.c_str(),
#endif
		m);

	if (!buf->is_open()) {
		delete buf;
		return Filesystem_Stream::OutputStream();
	}

	Filesystem_Stream::OutputStream os(buf);
	return os;
}

std::string FileFinder::FindImage(const std::string& dir, const std::string& name) {
#ifdef EMSCRIPTEN
	return FindDefault(dir, name);
#endif

	static const char* IMG_TYPES[] = { ".bmp",  ".png", ".xyz", NULL };
	return FindFile(dir, name, IMG_TYPES, true);
}

std::string FileFinder::FindDefault(const std::string& dir, const std::string& name) {
	static const char* no_exts[] = {"", NULL};
	return FindFile(dir, name, no_exts);
}

std::string FileFinder::FindDefault(const std::string& name) {
	return FindDefault(*GetDirectoryTree(), name);
}

std::string FileFinder::FindDefault(const DirectoryTree& tree, const std::string& dir, const std::string& name) {
	static const char* no_exts[] = { "", NULL };

	return FindFile(tree, dir, name, no_exts);
}

std::string FileFinder::FindDefault(const DirectoryTree& tree, const std::string& name) {
	DirectoryTree const& p = tree;

	std::vector<std::string> path_comps = SplitPath(name);
	if (path_comps.size() > 1) {
		// When the searched name contains a directory search in this directory
		// instead of the root

		std::string f;
		for (auto it = path_comps.begin() + 1; it != path_comps.end(); ++it) {
			f = MakePath(f, *it);
		}

		return FindDefault(path_comps[0], f);
	}

	string_map const& files = p.files;

	string_map::const_iterator const it = files.find(lcf::ReaderUtil::Normalize(name));

	return(it != files.end()) ? MakePath(p.directory_path, it->second) : "";
}

bool FileFinder::IsValidProject(DirectoryTree const & dir) {
	return IsRPG2kProject(dir) || IsEasyRpgProject(dir) || IsRPG2kProjectWithRenames(dir);
}

std::string FileFinder::FindDefault(FileFinder::DirectoryTree const &tree, const std::string &dir, const std::string &name,	const char **exts) {
	return FindFile(tree, dir, name, exts);
}

bool FileFinder::IsRPG2kProject(DirectoryTree const& dir) {
	string_map::const_iterator const
		ldb_it = dir.files.find(Utils::LowerCase(DATABASE_NAME)),
		lmt_it = dir.files.find(Utils::LowerCase(TREEMAP_NAME));

	return(ldb_it != dir.files.end() && lmt_it != dir.files.end());
}

bool FileFinder::IsEasyRpgProject(DirectoryTree const& dir){
	string_map::const_iterator const
		ldb_it = dir.files.find(Utils::LowerCase(DATABASE_NAME_EASYRPG)),
		lmt_it = dir.files.find(Utils::LowerCase(TREEMAP_NAME_EASYRPG));

	return(ldb_it != dir.files.end() && lmt_it != dir.files.end());
}

bool FileFinder::IsRPG2kProjectWithRenames(DirectoryTree const& dir) {
	return !FileExtGuesser::GetRPG2kProjectWithRenames(dir).Empty();
}

bool FileFinder::HasSavegame() {
	return GetSavegames() > 0;
}

int FileFinder::GetSavegames() {
	int count = 0;

	std::shared_ptr<FileFinder::DirectoryTree> tree = FileFinder::CreateSaveDirectoryTree();

	for (int i = 1; i <= 15; i++) {
		std::stringstream ss;
		ss << "Save" << (i <= 9 ? "0" : "") << i << ".lsd";
		std::string filename = FileFinder::FindDefault(*tree, ss.str());

		if (!filename.empty()) {
			++count;
		}
	}
	return count;
}

std::string FileFinder::FindMusic(const std::string& name) {
#ifdef EMSCRIPTEN
	return FindDefault("Music", name);
#endif

	static const char* MUSIC_TYPES[] = {
		".opus", ".oga", ".ogg", ".wav", ".mid", ".midi", ".mp3", ".wma", nullptr };
	return FindFile("Music", name, MUSIC_TYPES);
}

std::string FileFinder::FindSound(const std::string& name) {
#ifdef EMSCRIPTEN
	return FindDefault("Sound", name);
#endif

	static const char* SOUND_TYPES[] = {
		".opus", ".oga", ".ogg", ".wav", ".mp3", ".wma", nullptr };
	return FindFile("Sound", name, SOUND_TYPES);
}

bool FileFinder::Exists(const std::string& filename) {
	return Platform::File(filename).Exists();
}

bool FileFinder::IsDirectory(const std::string& dir, bool follow_symlinks) {
	return Platform::File(dir).IsDirectory(follow_symlinks);
}

FileFinder::Directory FileFinder::GetDirectoryMembers(const std::string& path, FileFinder::Mode const m, const std::string& parent) {
	assert(FileFinder::Exists(path));
	assert(FileFinder::IsDirectory(path, true));

	Directory result;

	result.base = path;

	Platform::Directory dir(path);
	if (!dir) {
		Output::Debug("Error opening dir {}: {}", path, ::strerror(errno));
		return result;
	}

	while (dir.Read()) {
		const std::string name = dir.GetEntryName();
		Platform::FileType type = dir.GetEntryType();

		static bool has_fast_dir_stat = true;
		bool is_directory = false;
		if (has_fast_dir_stat) {
			if (type == Platform::FileType::Unknown) {
				has_fast_dir_stat = false;
			} else {
				is_directory = type == Platform::FileType::Directory;
			}
		}

		if (!has_fast_dir_stat) {
			is_directory = IsDirectory(MakePath(path, name), true);
		}

		if (name == "." || name == "..") {
			continue;
		}

		switch(m) {
		case FILES:
			if (is_directory) { continue; }
		    break;
		case DIRECTORIES:
			if (!is_directory) { continue; }
			break;
		case ALL:
			break;
		case RECURSIVE:
			if (is_directory) {
				Directory rdir = GetDirectoryMembers(MakePath(path, name), RECURSIVE, MakePath(parent, name));
				result.files.insert(rdir.files.begin(), rdir.files.end());
				result.directories.insert(rdir.directories.begin(), rdir.directories.end());
				continue;
			}

			result.files[lcf::ReaderUtil::Normalize(MakePath(parent, name))] = MakePath(parent, name);
			continue;
		}
		std::string name_norm = lcf::ReaderUtil::Normalize(name);
		if (is_directory) {
			if (result.directories.find(name_norm) != result.directories.end()) {
				Output::Warning("This game provides the folder \"{}\" twice.", name);
				Output::Warning("This can lead to file not found errors. Merge the directories manually in a file browser.");
			}
			result.directories[name_norm] = name;
		} else {
			result.files[name_norm] = name;
		}
	}

	return result;
}

int64_t FileFinder::GetFileSize(const std::string& file) {
	return Platform::File(file).GetSize();
}

bool FileFinder::IsMajorUpdatedTree() {
	// Find an MP3 music file only when official Harmony.dll exists
	// in the gamedir or the file doesn't exist because
	// the detection doesn't return reliable results for games created with
	// "RPG2k non-official English translation (older engine) + MP3 patch"
	bool find_mp3 = true;
	std::string harmony = FindDefault("Harmony.dll");
	if (!harmony.empty()) {
		auto size = GetFileSize(harmony);
		if (size != -1 && size != KnownFileSize::OFFICIAL_HARMONY_DLL) {
			Output::Debug("Non-official Harmony.dll found, skipping MP3 test");
			find_mp3 = false;
		}
	}
	if (find_mp3) {
		const std::shared_ptr<DirectoryTree> tree = GetDirectoryTree();
		string_map::const_iterator const music_it = tree->directories.find("music");
		if (music_it != tree->directories.end()) {
			string_map mem = tree->sub_members["music"];
			for (auto& i : mem) {
				std::string file = mem[i.first];
				if (ToStringView(Utils::LowerCase(file)).ends_with(".mp3")) {
					Output::Debug("MP3 file ({}) found", file);
					return true;
				}
			}
		}
	}

	// Compare the size of RPG_RT.exe with threshold
	std::string rpg_rt = FindDefault("RPG_RT.exe");
	if (!rpg_rt.empty()) {
		auto size = GetFileSize(rpg_rt);
		if (size != -1) {
			return size > (Player::IsRPG2k() ? RpgrtMajorUpdateThreshold::RPG2K : RpgrtMajorUpdateThreshold::RPG2K3);
		}
	}
	Output::Debug("Could not get the size of RPG_RT.exe");

	// Assume the most popular version
	// Japanese or RPG2k3 games: newer engine
	// non-Japanese RPG2k games: older engine
	bool assume_newer = Player::IsCP932() || Player::IsRPG2k3();
	Output::Debug("Assuming {} engine", assume_newer ? "newer" : "older");
	return assume_newer;
}
