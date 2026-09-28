#include "library.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>

namespace fs = std::filesystem;

namespace {
std::string Lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r");
    const auto e = s.find_last_not_of(" \t\r");
    return b == std::string::npos ? "" : s.substr(b, e - b + 1);
}
}  // namespace

int Library::scan(const std::string& dir) {
    tracks.clear();
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return 0;

    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        const std::string ext = Lower(entry.path().extension().string());
        if (ext != ".mp3" && ext != ".wav" && ext != ".ogg") continue;

        Track t;
        t.path = entry.path().string();
        t.fileName = entry.path().filename().string();

        const std::string stem = entry.path().stem().string();
        const auto dash = stem.find(" - ");
        if (dash != std::string::npos) {
            t.artist = Trim(stem.substr(0, dash));
            t.title = Trim(stem.substr(dash + 3));
        } else {
            t.artist = "Unknown artist";
            t.title = stem;
        }

        fs::path cover = entry.path();
        cover.replace_extension(".png");
        if (fs::exists(cover, ec)) t.coverPath = cover.string();

        tracks.push_back(std::move(t));
    }

    std::sort(tracks.begin(), tracks.end(), [](const Track& a, const Track& b) {
        return Lower(a.fileName) < Lower(b.fileName);
    });
    return static_cast<int>(tracks.size());
}

void Library::loadLoved(const std::string& file) {
    std::ifstream in(file);
    std::set<std::string> loved;
    for (std::string line; std::getline(in, line);) {
        line = Trim(line);
        if (!line.empty()) loved.insert(line);
    }
    for (auto& t : tracks) t.loved = loved.count(t.fileName) > 0;
}

void Library::saveLoved(const std::string& file) const {
    std::ofstream out(file);
    for (const auto& t : tracks)
        if (t.loved) out << t.fileName << '\n';
}
