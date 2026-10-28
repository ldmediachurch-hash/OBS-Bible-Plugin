/*
LD Scripture for OBS
Copyright (C) 2026 LD Media Church

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "bible.hpp"
#include "dock.hpp"
#include "lower-third.hpp"
#include "lyrics.hpp"
#include "lyrics-dock.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QDir>
#include <QFontDatabase>
#include <QMainWindow>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static ScriptureDock *g_dock = nullptr;
static LyricsDock *g_lyrics_dock = nullptr;

static QString moduleFile(const char *rel)
{
	char *p = obs_module_file(rel);
	QString s = p ? QString::fromUtf8(p) : QString();
	bfree(p);
	return s;
}

static void registerFonts()
{
	const QString dir = moduleFile("fonts");
	for (const QString &f : QDir(dir).entryList({QStringLiteral("*.otf"), QStringLiteral("*.ttf")}, QDir::Files)) {
		if (QFontDatabase::addApplicationFont(QDir(dir).filePath(f)) < 0)
			obs_log(LOG_WARNING, "could not load font %s", f.toUtf8().constData());
	}
}

static void frontendEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_EXIT) {
		if (g_dock)
			g_dock->saveConfig();
		if (g_lyrics_dock)
			g_lyrics_dock->saveConfig();
	}
}

bool obs_module_load(void)
{
	registerFonts();

	char *userDir = obs_module_config_path("bibles");
	BibleLibrary::instance().load(moduleFile("bibles"), QString::fromUtf8(userDir));
	bfree(userDir);

	char *songsDir = obs_module_config_path("songs");
	SongLibrary::instance().load(moduleFile("songs-samples"), QString::fromUtf8(songsDir));
	bfree(songsDir);

	register_lower_third_source();
	register_lyrics_lower_third_source();

	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	g_dock = new ScriptureDock(main);
	if (!obs_frontend_add_dock_by_id("ld_scripture_dock", obs_module_text("LDScripture.Dock"), g_dock))
		obs_log(LOG_WARNING, "could not add the LD Scripture dock");

	g_lyrics_dock = new LyricsDock(main);
	if (!obs_frontend_add_dock_by_id("ld_lyrics_dock", obs_module_text("LDScripture.LyricsDock"), g_lyrics_dock))
		obs_log(LOG_WARNING, "could not add the LD Lyrics dock");

	obs_frontend_add_event_callback(frontendEvent, nullptr);

	obs_log(LOG_INFO, "LD Scripture loaded (version %s), %d Bibles, %d Songs", PLUGIN_VERSION,
		static_cast<int>(BibleLibrary::instance().bibles().size()),
		static_cast<int>(SongLibrary::instance().songs().size()));
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(frontendEvent, nullptr);
	g_dock = nullptr;
	g_lyrics_dock = nullptr;
	obs_log(LOG_INFO, "LD Scripture unloaded");
}
