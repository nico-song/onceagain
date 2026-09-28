#pragma once
#include <string>
#include <vector>

struct Track {
    std::string path;      // music/TWICE - FANCY.mp3
    std::string fileName;  // TWICE - FANCY.mp3
    std::string title;     // FANCY
    std::string artist;    // TWICE
    std::string coverPath; // music/TWICE - FANCY.png (optional)
    bool loved = false;
};

class Library {
public:
    // finds audio files in dir, parses "Artist - Title.ext" names
    int scan(const std::string& dir);
    void loadLoved(const std::string& file);
    void saveLoved(const std::string& file) const;

    std::vector<Track> tracks;
};
