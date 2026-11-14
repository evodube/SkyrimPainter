// SkyrimPainter - paint your own pictures in Skyrim Special Edition
// Copyright (C) 2026 evodube
//
// This program is free software: you can redistribute it and/or modify it under the terms of the
// GNU General Public License as published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
// even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with this program
// (LICENSE.txt). If not, see <https://www.gnu.org/licenses/>.

#include "Store.h"

#include "Art.h"
#include "Paint.h"

#include <stb_image.h>  // implementation lives in Dds.cpp
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_WINDOWS_UTF8
#include <stb_image_write.h>

namespace fs = std::filesystem;

namespace Store
{
	namespace
	{
		std::mutex g_mtx;  // one save / copy / rotate at a time

		bool WritePng(const fs::path& a_path, const std::uint8_t* a_px, int a_w, int a_h, std::string& a_error, int a_channels = 4)
		{
			auto tmp = a_path;
			tmp += L".tmp";
			const auto utf8 = Art::ToUtf8(tmp);
			if (!stbi_write_png(utf8.c_str(), a_w, a_h, a_channels, a_px, a_w * a_channels)) {
				a_error = "could not write " + utf8;
				return false;
			}
			if (!::MoveFileExW(tmp.c_str(), a_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
				a_error = fmt::format("rename failed ({})", ::GetLastError());
				return false;
			}
			return true;
		}

		bool ReadPng(const fs::path& a_path, std::vector<std::uint8_t>& a_px, int& a_w, int& a_h, int a_channels = 4)
		{
			int  n = 0;
			auto utf8 = Art::ToUtf8(a_path);
			auto* p = stbi_load(utf8.c_str(), &a_w, &a_h, &n, a_channels);
			if (!p) {
				return false;
			}
			a_px.assign(p, p + static_cast<std::size_t>(a_w) * a_h * a_channels);
			stbi_image_free(p);
			return true;
		}
	}

	fs::path PaintingsFolder() { return Art::UserFolder() / L"Paintings"; }
	fs::path WorkFolder() { return Art::UserFolder() / L"InProgress"; }

	bool SaveDoc(const fs::path& a_dir, const Doc& a_doc, const std::vector<std::uint8_t>& a_flat, std::string& a_error)
	{
		std::lock_guard lk(g_mtx);
		std::error_code ec;
		fs::create_directories(a_dir, ec);
		std::string index;
		for (std::size_t i = 0; i < a_doc.layers.size(); ++i) {
			const auto& l = a_doc.layers[i];
			if (!WritePng(a_dir / fmt::format("layer{}.png", i), l.px.data(), a_doc.w, a_doc.h, a_error)) {
				return false;
			}
			std::string name = l.name;
			std::replace(name.begin(), name.end(), '\t', ' ');
			std::replace(name.begin(), name.end(), '\n', ' ');
			index += fmt::format("{}\t{}\t{:.3f}\t{}\t{}\n", name, l.visible ? 1 : 0, l.opacity, l.blend, l.lockAlpha ? 1 : 0);
		}
		// layers beyond the new count (a layer was deleted)
		for (std::size_t i = a_doc.layers.size(); i < Paint::kMaxLayers + 4; ++i) {
			fs::remove(a_dir / fmt::format("layer{}.png", i), ec);
		}
		{
			std::ofstream out(a_dir / L"layers.txt", std::ios::binary | std::ios::trunc);
			out << index;
			if (!out) {
				a_error = "could not write layers.txt";
				return false;
			}
		}
		// the paint's thickness (impasto) and the varnish
		if (a_doc.thickness.size() == static_cast<std::size_t>(a_doc.w) * a_doc.h) {
			if (!WritePng(a_dir / L"thickness.png", a_doc.thickness.data(), a_doc.w, a_doc.h, a_error, 1)) {
				return false;
			}
		} else {
			fs::remove(a_dir / L"thickness.png", ec);
		}
		{
			std::ofstream out(a_dir / L"meta.txt", std::ios::binary | std::ios::trunc);
			out << "varnish=" << a_doc.varnish << "\n";
			out << "strokes=" << a_doc.strokes << "\n";
			out << "seconds=" << a_doc.seconds << "\n";
		}
		return WritePng(a_dir / L"flat.png", a_flat.data(), a_doc.w, a_doc.h, a_error);
	}

	bool LoadThickness(const fs::path& a_dir, std::vector<std::uint8_t>& a_px, int& a_w, int& a_h)
	{
		return fs::exists(a_dir / L"thickness.png") && ReadPng(a_dir / L"thickness.png", a_px, a_w, a_h, 1);
	}

	int LoadVarnish(const fs::path& a_dir)
	{
		std::ifstream in(a_dir / L"meta.txt", std::ios::binary);
		for (std::string line; std::getline(in, line);) {
			if (line.rfind("varnish=", 0) == 0) {
				return std::clamp(std::atoi(line.c_str() + 8), 0, 3);
			}
		}
		return 0;
	}

	void LoadEffort(const fs::path& a_dir, int& a_strokes, int& a_seconds)
	{
		a_strokes = a_seconds = -1;
		std::ifstream in(a_dir / L"meta.txt", std::ios::binary);
		for (std::string line; std::getline(in, line);) {
			if (line.rfind("strokes=", 0) == 0) {
				a_strokes = std::max(0, std::atoi(line.c_str() + 8));
			} else if (line.rfind("seconds=", 0) == 0) {
				a_seconds = std::max(0, std::atoi(line.c_str() + 8));
			}
		}
	}

	std::optional<Doc> LoadDoc(const fs::path& a_dir)
	{
		std::lock_guard lk(g_mtx);
		std::ifstream   in(a_dir / L"layers.txt", std::ios::binary);
		if (!in) {
			return std::nullopt;
		}
		Doc doc;
		int i = 0;
		for (std::string line; std::getline(in, line); ++i) {
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			if (line.empty()) {
				continue;
			}
			LayerFile  l;
			const auto t1 = line.find('\t'), t2 = line.find('\t', t1 == std::string::npos ? 0 : t1 + 1);
			l.name = line.substr(0, t1);
			if (t1 != std::string::npos) {
				l.visible = line.substr(t1 + 1, t2 - t1 - 1) != "0";
			}
			if (t2 != std::string::npos) {
				l.opacity = std::clamp(static_cast<float>(std::atof(line.c_str() + t2 + 1)), 0.0f, 1.0f);
				// later columns (older paintings don't have them): blend, lock alpha
				if (const auto t3 = line.find('\t', t2 + 1); t3 != std::string::npos) {
					l.blend = std::clamp(std::atoi(line.c_str() + t3 + 1), 0, 1);
					if (const auto t4 = line.find('\t', t3 + 1); t4 != std::string::npos) {
						l.lockAlpha = std::atoi(line.c_str() + t4 + 1) != 0;
					}
				}
			}
			int w = 0, h = 0;
			if (!ReadPng(a_dir / fmt::format("layer{}.png", i), l.px, w, h)) {
				spdlog::warn("Layer {} of {} missing", i, Art::ToUtf8(a_dir));
				continue;
			}
			if (doc.layers.empty()) {
				doc.w = w;
				doc.h = h;
			} else if (w != doc.w || h != doc.h) {
				spdlog::warn("Layer {} of {} has another size, skipped", i, Art::ToUtf8(a_dir));
				continue;
			}
			doc.layers.push_back(std::move(l));
		}
		if (doc.layers.empty()) {
			return std::nullopt;
		}
		int tw = 0, th = 0;
		if (!fs::exists(a_dir / L"thickness.png") || !ReadPng(a_dir / L"thickness.png", doc.thickness, tw, th, 1) || tw != doc.w || th != doc.h) {
			doc.thickness.clear();
		}
		doc.varnish = LoadVarnish(a_dir);
		LoadEffort(a_dir, doc.strokes, doc.seconds);
		return doc;
	}

	bool LoadFlat(const fs::path& a_dir, std::vector<std::uint8_t>& a_px, int& a_w, int& a_h)
	{
		std::lock_guard lk(g_mtx);
		return ReadPng(a_dir / L"flat.png", a_px, a_w, a_h);
	}

	bool HasDoc(const fs::path& a_dir)
	{
		std::error_code ec;
		return fs::exists(a_dir / L"layers.txt", ec);
	}

	bool CopyDoc(const fs::path& a_from, const fs::path& a_to)
	{
		std::lock_guard lk(g_mtx);
		std::error_code ec;
		fs::remove_all(a_to, ec);
		fs::create_directories(a_to, ec);
		fs::copy(a_from, a_to, fs::copy_options::overwrite_existing | fs::copy_options::recursive, ec);
		if (ec) {
			spdlog::error("Copy {} -> {} failed: {}", Art::ToUtf8(a_from), Art::ToUtf8(a_to), ec.message());
			return false;
		}
		return true;
	}

	void RemoveDoc(const fs::path& a_dir)
	{
		std::lock_guard lk(g_mtx);
		std::error_code ec;
		fs::remove_all(a_dir, ec);
	}

	bool RotateDoc(const fs::path& a_dir, bool a_clockwise)
	{
		auto doc = LoadDoc(a_dir);
		if (!doc) {
			return true;  // nothing painted yet
		}
		for (auto& l : doc->layers) {
			l.px = Paint::Rotate90(l.px, doc->w, doc->h, a_clockwise);
		}
		if (doc->thickness.size() == static_cast<std::size_t>(doc->w) * doc->h) {
			doc->thickness = Paint::Rotate90Gray(doc->thickness, doc->w, doc->h, a_clockwise);
		}
		std::vector<std::uint8_t> flat;
		int                       fw = 0, fh = 0;
		LoadFlat(a_dir, flat, fw, fh);
		if (fw == doc->w && fh == doc->h) {
			flat = Paint::Rotate90(flat, fw, fh, a_clockwise);
		}
		std::swap(doc->w, doc->h);
		if (flat.size() != static_cast<std::size_t>(doc->w) * doc->h * 4) {
			flat.assign(static_cast<std::size_t>(doc->w) * doc->h * 4, 255);
		}
		std::string err;
		if (!SaveDoc(a_dir, *doc, flat, err)) {
			spdlog::error("Turning {} failed: {}", Art::ToUtf8(a_dir), err);
			return false;
		}
		return true;
	}
}
