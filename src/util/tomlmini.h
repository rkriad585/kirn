// Minimal TOML-subset reader shared by the interpreter's module loader and
// the kirn CLI. Supports exactly what manifests and lockfiles need:
//   # comments
//   [section.sub]            scalar keys under dotted sections
//   [[array]]                arrays of tables (kept in order)
//   key = "str" | 42 | true | { k = "v", ... }   (inline tables kept raw)
#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace kirn {
namespace tomlmini {

struct Entry {
    std::string name;                          // [[name]]
    std::map<std::string, std::string> kv;
};

struct Doc {
    std::map<std::string, std::string> kv;     // "section.key" -> raw value
    std::vector<Entry> tables;
};

inline std::string strip(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

inline std::string quote(const std::string& s) {
    std::string r = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') r += '\\';
        r += c;
    }
    return r + "\"";
}
inline std::string unquote(const std::string& s) {
    std::string t = strip(s);
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"') {
        std::string r;
        for (size_t i = 1; i + 1 < t.size(); ++i)
            if (t[i] == '\\' && i + 2 < t.size())
                r += t[++i];      // keep escaped char, drop the backslash
            else
                r += t[i];
        return r;
    }
    return t;
}

// strip a trailing comment that is not inside quotes
inline std::string cutComment(const std::string& line) {
    bool q = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '"' && (i == 0 || line[i - 1] != '\\')) q = !q;
        if (c == '#' && !q) return line.substr(0, i);
    }
    return line;
}

inline Doc parse(const std::string& text) {
    Doc d;
    std::string section;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = strip(cutComment(text.substr(pos, eol - pos)));
        pos = eol + 1;
        if (line.empty()) continue;

        if (line.front() == '[') {
            bool array = line.size() > 1 && line[1] == '[';
            size_t close = line.find(array ? "]]" : "]");
            std::string name =
                strip(line.substr(1 + (array ? 1 : 0),
                                  close == std::string::npos
                                      ? std::string::npos
                                      : close - 1 - (array ? 1 : 0)));
            if (array) {
                d.tables.push_back(Entry{name, {}});
                section.clear();
            } else {
                section = name;
            }
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = strip(line.substr(0, eq));
        std::string val = strip(line.substr(eq + 1));
        if (key.empty()) continue;
        if (!d.tables.empty() && section.empty() &&
            &d.tables.back() == &d.tables.back())
            // inside an [[array]] block (no [section] seen since)
            d.tables.back().kv[key] = val;
        else
            d.kv[section.empty() ? key : section + "." + key] = val;
    }
    return d;
}

// scalar lookup; unquotes "..." values automatically
inline std::string get(const Doc& d, const std::string& section,
                       const std::string& key, const std::string& fb = "") {
    auto it = d.kv.find(section.empty() ? key : section + "." + key);
    return it == d.kv.end() ? fb : unquote(it->second);
}

// parse `{ k = "v", n = 3 }` style inline tables
inline std::map<std::string, std::string> inlineTable(const std::string& raw) {
    std::map<std::string, std::string> out;
    std::string t = strip(raw);
    if (t.size() < 2 || t.front() != '{' || t.back() != '}') return out;
    t = t.substr(1, t.size() - 2);
    bool q = false;
    std::vector<std::string> parts;
    std::string cur;
    for (char c : t) {
        if (c == '"') q = !q;
        if (c == ',' && !q) {
            parts.push_back(cur);
            cur.clear();
        } else
            cur += c;
    }
    if (!strip(cur).empty()) parts.push_back(cur);
    for (const std::string& p : parts) {
        size_t eq = p.find('=');
        if (eq == std::string::npos) continue;
        out[strip(p.substr(0, eq))] = strip(p.substr(eq + 1));
    }
    return out;
}

// dependency entries of a manifest: name -> {path|git|tag|version|registry}
inline std::vector<std::pair<std::string, std::map<std::string, std::string>>>
dependencies(const Doc& d) {
    std::vector<std::pair<std::string, std::map<std::string, std::string>>>
        out;
    for (const auto& [k, raw] : d.kv) {
        const std::string prefix = "dependencies.";
        if (k.rfind(prefix, 0) != 0) continue;
        std::string name = k.substr(prefix.size());
        if (name.find('.') != std::string::npos) continue;
        std::string v = strip(raw);
        if (!v.empty() && v.front() == '{') {
            out.push_back({name, inlineTable(v)});
        } else {
            out.push_back(
                {name, std::map<std::string, std::string>{
                           {"version", unquote(v)}}});
        }
    }
    return out;
}

} // namespace tomlmini
} // namespace kirn
