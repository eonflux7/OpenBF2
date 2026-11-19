// [TOOLS] A map's view file (docs/eon64/tools.md#1-views): <mod>/settings/eon64-views/<map>.txt.
// Not part of retail BF2; client image only.
//
// The file is edited by hand and by the tool, so the tool never rewrites it whole: it keeps the
// file's lines, parses views and sets out of them, and an edit replaces, inserts or removes only
// the lines of the view or set it changes. Comments, blank lines, order and line endings stay.
#pragma once

#include "../../Core/BuildConfig.h"

#if !DEDICATED

#include <string>
#include <vector>

namespace dice {
namespace hfe {

struct ViewDef
{
    std::string name;
    float x, y, z;          // world metres, y up
    float yaw, pitch;       // degrees, the game's convention (pitch negative looking down)
    float fov;              // vertical degrees; valid when hasFov
    bool  hasFov;
    std::string note;       // the text after '|', trimmed
    std::vector<std::string> runs;   // console lines run on entering the view

    // Where the view sits in the file.
    int header;             // the "view" line
    std::vector<int> runLines;
};

struct ViewSet
{
    std::string name;
    std::vector<std::string> views;
    int line;
};

class ViewFile
{
public:
    ViewFile();

    // Parses the file at path. A missing file is an empty view file, not an error. Returns false
    // only when the file exists and cannot be read.
    bool load(const std::string& path);
    // True when the file on disk differs from what was last loaded (or the path changed).
    bool changedOnDisk(const std::string& path) const;
    // Writes the lines back to the path loaded last.
    bool write(std::string& error);

    const std::string& path() const { return m_path; }
    bool exists() const { return m_exists; }
    const std::vector<ViewDef>& views() const { return m_views; }
    const std::vector<ViewSet>& sets() const { return m_sets; }
    // "line N: ..." for each line that could not be parsed.
    const std::vector<std::string>& problems() const { return m_problems; }

    const ViewDef* find(const std::string& name) const;

    // Adds the view, or updates the camera fields of the one with that name (its other fields,
    // run lines and, when note is empty, its note stay).
    void saveView(const std::string& name, float x, float y, float z, float yaw, float pitch,
                  float fov, const std::string& note);
    // Removes the view and its run lines, and the view from every set; a set left empty is
    // removed. Returns false when there is no such view.
    bool deleteView(const std::string& name);
    // Swaps the view's lines (its view line through its last run line) with those of the view
    // before it (delta < 0) or after it (delta > 0); lines between the two stay in place. Returns
    // false when there is no such view or no neighbour on that side.
    bool moveView(const std::string& name, int delta);
    // Replaces the view's note; an empty note removes it. False when there is no such view.
    bool setNote(const std::string& name, const std::string& note);
    // Replaces the set's views, or adds the set after the last one (at the end when there is
    // none); no views removes the set.
    void setSet(const std::string& name, const std::vector<std::string>& views);

    // A valid name: no whitespace, '|' or '#', not empty.
    static bool validName(const std::string& name);
    // "x,y,z" or a number with at most `decimals` places, trailing zeros dropped.
    static std::string formatNumber(float value, int decimals);

private:
    void parse();
    static std::string readFile(const std::string& path, bool& exists);

    std::string m_path;
    std::string m_raw;              // the file as last read or written
    bool m_exists;
    bool m_crlf;
    std::vector<std::string> m_lines;
    std::vector<ViewDef> m_views;
    std::vector<ViewSet> m_sets;
    std::vector<std::string> m_problems;
};

} // namespace hfe
} // namespace dice

#endif // !DEDICATED
