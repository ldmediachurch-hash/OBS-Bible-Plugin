/*
LD Scripture for OBS
Copyright (C) 2026 LD Media Church

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <QString>

#include <cstdint>
#include <mutex>
#include <vector>

struct DisplayEntry {
	QString label; // translation abbreviation, e.g. "KJV"
	QString text;
};

struct DisplayContent {
	bool visible = false;
	QString reference; // "Romans 8:28"
	std::vector<DisplayEntry> entries;

	bool sameText(const DisplayContent &o) const
	{
		if (reference != o.reference || entries.size() != o.entries.size())
			return false;
		for (size_t i = 0; i < entries.size(); i++) {
			if (entries[i].label != o.entries[i].label || entries[i].text != o.entries[i].text)
				return false;
		}
		return true;
	}
};

// What every "LD Scripture Lower Third" source shows. Written by the dock, read by the sources.
class DisplayState {
public:
	static DisplayState &get()
	{
		static DisplayState s;
		return s;
	}
	void set(const DisplayContent &c)
	{
		std::lock_guard<std::mutex> lock(m);
		content = c;
		version++;
	}
	void setVisible(bool v)
	{
		std::lock_guard<std::mutex> lock(m);
		if (content.visible == v)
			return;
		content.visible = v;
		version++;
	}
	DisplayContent snapshot(uint64_t *ver)
	{
		std::lock_guard<std::mutex> lock(m);
		if (ver)
			*ver = version;
		return content;
	}

private:
	std::mutex m;
	DisplayContent content;
	uint64_t version = 1;
};
