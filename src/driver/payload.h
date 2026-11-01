#ifndef KIRN_DRIVER_PAYLOAD_H
#define KIRN_DRIVER_PAYLOAD_H

// Shared on-disk payload contracts.  The compiler driver writes these formats
// and the standalone runtime reads them, so keeping the framing here prevents
// a writer/reader drift when a section or metadata field changes.

#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace kirn::payload {

struct Bundle {
    std::string name;
    std::string version;
    std::string target;
    std::string entry;
    std::vector<std::pair<std::string, std::string>> sections;
};

using Sections = std::vector<std::pair<std::string, std::string>>;

// PETLIB uses the same section framing as KIRNBUNDLE, but has no executable
// prefix or @meta record. Keep both the writer and installer on this shared
// framing so section termination and newline behavior cannot drift.
inline std::string archiveText(const std::string& magic,
                               const Sections& sections) {
    std::ostringstream out;
    out << magic << "\n";
    for (const auto& [key, source] : sections)
        out << "@@FILE " << key << "\n" << source << "@@END\n";
    return out.str();
}

inline bool parseArchive(const std::string& data, const std::string& magic,
                         Sections& sections) {
    std::istringstream input(data);
    std::string line, currentKey, current;
    if (!std::getline(input, line) || line != magic) return false;
    auto finish = [&]() {
        if (!currentKey.empty()) sections.emplace_back(currentKey, current);
        currentKey.clear();
        current.clear();
    };
    while (std::getline(input, line)) {
        if (line.rfind("@@FILE ", 0) == 0) {
            if (!currentKey.empty()) finish();
            currentKey = line.substr(7);
        } else if (line == "@@END") {
            finish();
        } else if (!currentKey.empty()) {
            current += line + "\n";
        }
    }
    if (!currentKey.empty()) finish();
    return !sections.empty();
}

inline std::string bundleTail(
    const std::string& name, const std::string& version,
    const std::string& target, const std::string& entry,
    const std::string& entrySource,
    const std::map<std::string, std::string>& embedded) {
    std::ostringstream out;
    out << "\nKIRNBUNDLE/1\n"
        << "@@FILE @meta\n" << name << "\n"
        << (version.empty() ? "0.0.0" : version) << "\n"
        << target << "\n" << entry << "\n@@END\n"
        << "@@FILE " << entry << "\n" << entrySource << "\n@@END\n";
    for (const auto& [key, source] : embedded)
        if (key != entry)
            out << "@@FILE " << key << "\n" << source << "\n@@END\n";
    return out.str();
}

// The last marker wins deliberately: appending a new payload to a copied
// runner must not make an older marker inside the executable authoritative.
inline bool parseBundle(const std::string& data, Bundle& out) {
    const std::string marker = "\nKIRNBUNDLE/1\n";
    const size_t found = data.rfind(marker);
    if (found == std::string::npos) return false;

    std::istringstream input(data.substr(found + marker.size()));
    std::string line, currentKey, current;
    auto finish = [&]() {
        if (currentKey == "@meta") {
            std::istringstream meta(current);
            std::getline(meta, out.name);
            std::getline(meta, out.version);
            std::getline(meta, out.target);
            std::getline(meta, out.entry);
        } else if (!currentKey.empty()) {
            out.sections.emplace_back(currentKey, current);
        }
        currentKey.clear();
        current.clear();
    };
    while (std::getline(input, line)) {
        if (line.rfind("@@FILE ", 0) == 0) {
            if (!currentKey.empty()) finish();
            currentKey = line.substr(7);
        } else if (line == "@@END") {
            finish();
        } else if (!currentKey.empty()) {
            current += line + "\n";
        }
    }
    if (!currentKey.empty()) finish();
    return !out.entry.empty();
}

}  // namespace kirn::payload

#endif
