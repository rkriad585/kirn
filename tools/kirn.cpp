// kirn — driver CLI for the Kirn language.
//
//   kirn new <name>              scaffold an application project
//   kirn new lib <name>          scaffold a library package
//   kirn run [dir|file]          run a program or project
//   kirn test [dir|file...]      run *_test.kn files
//   kirn install|i [-g] <pkg>    install into ./coco_libs or ~/.coco/kirn-pkg
//   kirn update [name]           refresh installed dependencies
//   kirn remove <name>           uninstall a dependency
//   kirn build                   compile project -> standalone build/<name>.exe
//   kirn build lib               check + pack library -> build/<n>-<v>.cocolib
//   kirn doc <lib|dir> [--port]  generate API docs + serve markdown viewer
//   kirn list                    show installed libraries
//
// Projects are Rust-style but with Kirn's own folder names:
//   coco.toml | code/ | tests/ | docs/ | coco_libs/ | build/
// Packages installed globally live in ~/.coco/kirn-pkg (binaries in bin/).
#include "ast/ast.h"
#include "interp/runtime.h"
#include "lex/lexer.h"
#include "parser/parser.h"
#include "sema/checker.h"
#include "backend/native.h"
#include "util/tomlmini.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>   // GetEnvironmentVariableA (PATH auto-setup)
#else
// Minimal POSIX shims for the handful of Win32 calls used below (PATH
// auto-setup, app-shim install, and self-path resolution). Keeps the driver
// buildable on Linux dev machines; the Windows-only behaviour (registry
// PATH, .cmd shims, MSVC cl.exe pipeline) is untouched and still gated to
// _WIN32 at the call sites that matter.
#include <unistd.h>
#include <strings.h>
#include <cstring>

#ifndef MAX_PATH
#define MAX_PATH 4096
#endif

static int _stricmp(const char* a, const char* b) { return ::strcasecmp(a, b); }

static char* GetEnvironmentVariableA(const char* name, char* buf, size_t size) {
    const char* v = ::getenv(name);
    if (!v) return nullptr;
    size_t n = ::strlen(v);
    if (n >= size && size) n = size - 1;
    if (size) {
        ::memcpy(buf, v, n);
        buf[n] = 0;
    }
    return buf;
}

static unsigned long GetModuleFileNameA(void*, char* buf, unsigned long size) {
    if (!size) return 0;
    ssize_t n = ::readlink("/proc/self/exe", buf, size - 1);
    if (n < 0) n = 0;
    buf[n] = 0;
    return (unsigned long)n;
}
#endif

namespace fs = std::filesystem;

namespace {

using kirn::tomlmini::Doc;
using Deps = std::map<std::string, std::map<std::string, std::string>>;

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF &&
        (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF)
        out.erase(0, 3);   // drop a UTF-8 BOM
    return true;
}

bool writeFile(const fs::path& path, const std::string& content) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    out << content;
    return out.good();
}

std::string trimSlashes(std::string s) {
    while (!s.empty() && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    return s;
}

std::string lastSegment(const std::string& p) {
    size_t cut = p.find_last_of("/\\");
    return cut == std::string::npos ? p : p.substr(cut + 1);
}

// null device for shell probes: `nul` only means that on Windows.
const char* shellNullDevice() {
#ifdef _WIN32
    return "nul";
#else
    return "/dev/null";
#endif
}

std::string todayIso() {
    time_t t = time(nullptr);
    tm tmv{};
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char buf[32];
    strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tmv);
    return buf;
}

// ---------------------------------------------------------------------------
// manifests: coco.toml
// ---------------------------------------------------------------------------

struct Manifest {
    std::string name, version = "0.0.1-beta", type = "app", main, description,
                license;
    std::string docs = "docs/index.md";   // docs entry file ([package] docs)
    std::string author, repo, readme = "README.md", homepage;
    std::vector<std::string> authors, tags, keywords;
    // [git] — files generated for new repos
    bool gitignore = true, gitkeep = false;
    std::vector<std::string> gitIgnoreExtra;   // additional ignore patterns
    Deps deps;
};

Manifest readManifest(const fs::path& dir) {
    Manifest m;
    std::string text;
    if (!readFile((dir / "coco.toml").string(), text)) return m;
    Doc d = kirn::tomlmini::parse(text);
    m.name = kirn::tomlmini::get(d, "package", "name");
    m.version = kirn::tomlmini::get(d, "package", "version", "0.0.1-beta");
    m.type = kirn::tomlmini::get(d, "package", "type", "app");
    m.main = kirn::tomlmini::get(d, "package", "main");
    m.docs = kirn::tomlmini::get(d, "package", "docs", "docs/index.md");
    m.description = kirn::tomlmini::get(d, "package", "description");
    m.license = kirn::tomlmini::get(d, "package", "license");
    m.author = kirn::tomlmini::get(d, "package", "author");
    m.repo = kirn::tomlmini::get(d, "package", "repo");
    m.readme = kirn::tomlmini::get(d, "package", "readme", "README.md");
    m.homepage = kirn::tomlmini::get(d, "package", "homepage");
    m.gitignore = kirn::tomlmini::get(d, "git", "gitignore", "true") != "false";
    m.gitkeep = kirn::tomlmini::get(d, "git", "gitkeep", "false") == "true";
    {
        std::string extra = kirn::tomlmini::get(d, "git", "ignore", "");
        size_t a = extra.find('[');
        size_t b = extra.rfind(']');
        if (a != std::string::npos && b != std::string::npos) {
            std::string body = extra.substr(a + 1, b - a - 1), cur;
            bool q = false;
            for (char c : body) {
                if (c == '"') q = !q;
                if (c == ',' && !q) { m.gitIgnoreExtra.push_back(kirn::tomlmini::strip(cur)); cur.clear(); }
                else cur += c;
            }
            m.gitIgnoreExtra.push_back(kirn::tomlmini::strip(cur));
        }
    }
    for (const auto& [k, v] : d.kv) {
        if (k.rfind("package.authors", 0) == 0 ||
            k.rfind("package.tags", 0) == 0 ||
            k.rfind("package.keywords", 0) == 0) {
            // inline arrays ["a", "b"]
            size_t a = v.find('['), b = v.rfind(']');
            if (a == std::string::npos || b == std::string::npos) continue;
            std::string body = v.substr(a + 1, b - a - 1);
            std::string cur;
            bool q = false;
            std::vector<std::string> items;
            for (char c : body) {
                if (c == '"') q = !q;
                if (c == ',' && !q) {
                    items.push_back(cur);
                    cur.clear();
                } else
                    cur += c;
            }
            items.push_back(cur);
            for (auto& it : items) {
                std::string s = kirn::tomlmini::strip(it);
                if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
                    s = s.substr(1, s.size() - 2);
                if (!s.empty())
                    (k.rfind("package.tags", 0) == 0 ? m.tags
                     : k.rfind("package.keywords", 0) == 0 ? m.keywords
                                                           : m.authors)
                        .push_back(s);
            }
        }
    }
    for (const auto& [name, spec] : kirn::tomlmini::dependencies(d)) {
        Deps::mapped_type e;
        for (const auto& [ik, iv] : spec) e[ik] = kirn::tomlmini::unquote(iv);
        m.deps[name] = e;
    }
    return m;
}

std::string tomlArray(const std::vector<std::string>& v) {
    if (v.empty()) return "[]";
    std::string o = "[";
    for (size_t i = 0; i < v.size(); ++i)
        o += (i ? ", " : "") + kirn::tomlmini::quote(v[i]);
    return o + "]";
}

void writeManifest(const fs::path& dir, const Manifest& m) {
    std::ostringstream o;
    o << "[package]\n";
    o << "name = " << kirn::tomlmini::quote(m.name) << "\n";
    o << "version = " << kirn::tomlmini::quote(m.version) << "\n";
    o << "type = " << kirn::tomlmini::quote(m.type) << "\n";
    if (!m.main.empty()) o << "main = " << kirn::tomlmini::quote(m.main) << "\n";
    if (!m.docs.empty()) o << "docs = " << kirn::tomlmini::quote(m.docs) << "\n";
    if (!m.description.empty())
        o << "description = " << kirn::tomlmini::quote(m.description) << "\n";
    if (!m.license.empty())
        o << "license = " << kirn::tomlmini::quote(m.license) << "\n";
    if (!m.author.empty())
        o << "author = " << kirn::tomlmini::quote(m.author) << "\n";
    if (!m.authors.empty()) o << "authors = " << tomlArray(m.authors) << "\n";
    if (!m.repo.empty()) o << "repo = " << kirn::tomlmini::quote(m.repo) << "\n";
    if (!m.homepage.empty())
        o << "homepage = " << kirn::tomlmini::quote(m.homepage) << "\n";
    if (!m.readme.empty() && m.readme != "README.md")
        o << "readme = " << kirn::tomlmini::quote(m.readme) << "\n";
    if (!m.tags.empty()) o << "tags = " << tomlArray(m.tags) << "\n";
    if (!m.keywords.empty())
        o << "keywords = " << tomlArray(m.keywords) << "\n";
    if (!m.gitignore || m.gitkeep || !m.gitIgnoreExtra.empty()) {
        o << "\n[git]\n";
        if (!m.gitignore) o << "gitignore = false\n";
        if (m.gitkeep) o << "gitkeep = true\n";
        if (!m.gitIgnoreExtra.empty())
            o << "ignore = " << tomlArray(m.gitIgnoreExtra) << "\n";
    }
    o << "\n[dependencies]\n";
    for (const auto& [name, spec] : m.deps) {
        if (spec.size() == 1 && spec.count("version"))
            o << name << " = " << kirn::tomlmini::quote(spec.at("version"))
              << "\n";
        else {
            o << name << " = { ";
            bool first = true;
            for (const auto& [ik, iv] : spec) {
                if (!first) o << ", ";
                first = false;
                o << ik << " = " << kirn::tomlmini::quote(iv);
            }
            o << " }\n";
        }
    }
    writeFile(dir / "coco.toml", o.str());
}

// ---------------------------------------------------------------------------
// lockfile: coco.lock — pins exactly what is installed where
// ---------------------------------------------------------------------------

struct LockEntry {
    std::string name, source, url, tag, commit, version, installed;
};

std::vector<LockEntry> readLock(const fs::path& dir) {
    std::vector<LockEntry> out;
    std::string text;
    if (!readFile((dir / "coco.lock").string(), text)) return out;
    Doc d = kirn::tomlmini::parse(text);
    for (const auto& t : d.tables) {
        if (t.name != "lock") continue;
        LockEntry e;
        auto g = [&](const char* k) {
            auto it = t.kv.find(k);
            return it == t.kv.end() ? "" : kirn::tomlmini::unquote(it->second);
        };
        e.name = g("name");
        e.source = g("source");
        e.url = g("url");
        e.tag = g("tag");
        e.commit = g("commit");
        e.version = g("version");
        e.installed = g("installed");
        if (!e.name.empty()) out.push_back(e);
    }
    return out;
}

void writeLock(const fs::path& dir, const std::vector<LockEntry>& locks) {
    std::ostringstream o;
    o << "# coco.lock - generated by `kirn install/update`. Commit this file.\n";
    for (const auto& e : locks) {
        o << "\n[[lock]]\n";
        o << "name = " << kirn::tomlmini::quote(e.name) << "\n";
        o << "source = " << kirn::tomlmini::quote(e.source) << "\n";
        o << "url = " << kirn::tomlmini::quote(e.url) << "\n";
        o << "tag = " << kirn::tomlmini::quote(e.tag) << "\n";
        o << "commit = " << kirn::tomlmini::quote(e.commit) << "\n";
        o << "version = " << kirn::tomlmini::quote(e.version) << "\n";
        o << "installed = " << kirn::tomlmini::quote(e.installed) << "\n";
    }
    writeFile(dir / "coco.lock", o.str());
}

bool copyTree(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    // refuse to copy a tree into itself (case-insensitive on win32)
    std::string f = fs::weakly_canonical(from, ec).string();
    std::string t = fs::weakly_canonical(to, ec).string();
    for (char& c : f) c = (char)tolower((unsigned char)c);
    for (char& c : t) c = (char)tolower((unsigned char)c);
    if (t.rfind(f + "/", 0) == 0 || t.rfind(f + "\\", 0) == 0 || t == f) {
        std::cerr << "kirn install: destination lies inside the source\n";
        return false;
    }
    fs::create_directories(to, ec);
    if (ec) {
        std::cerr << "kirn install: mkdir " << to.string() << ": "
                  << ec.message() << "\n";
        return false;
    }
    for (fs::directory_iterator it(from, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (ec) break;
        const fs::path& p = it->path();
        if (p.filename() == ".git") continue;   // never ship VCS metadata
        if (it->is_directory(ec)) {
            if (!copyTree(p, to / p.filename())) return false;
        } else if (it->is_regular_file(ec)) {
            fs::copy_file(p, to / p.filename(), fs::copy_options::overwrite_existing,
                          ec);
            if (ec) {
                std::cerr << "kirn install: copy " << p.string() << ": "
                          << ec.message() << "\n";
                return false;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// module/library search paths
// ---------------------------------------------------------------------------

// Convention-file resolution for a project root (used by `run` and `build`):
//   coco.toml [package] main -> code/main.kn -> main.kn -> code/pin.kn -> pin.kn
// Returns the relative path to the chosen entry, or "" when none exists.
std::string resolveEntry(const Manifest& m, const fs::path& dir) {
    std::string probe;
    if (!m.main.empty() && fs::is_regular_file(dir / m.main)) return m.main;
    const char* cands[] = {"code/main.kn", "main.kn", "code/pin.kn",
                           "pin.kn"};
    for (const char* c : cands)
        if (fs::is_regular_file(dir / c)) return c;
    return "";
}

std::string globalPkgDir() {
    const char* home = std::getenv("USERPROFILE");
    if (!home) home = std::getenv("HOME");
    return home ? std::string(home) + "/.coco/kirn-pkg" : "";
}

std::vector<std::string> libDirsFor(const std::string& script) {
    std::vector<std::string> dirs;
    if (const char* env = std::getenv("COCO_LIBS")) dirs.push_back(env);
    // The `script` is either a directory (project root) or a file path whose
    // parent is the project root. Resolve robustly regardless of trailing
    // slashes / relative prefixes like "./proj".
    fs::path sp(script);
    std::string dir =
        (script.empty() || script == ".")
            ? "."
            : (fs::is_directory(sp) ? script : sp.parent_path().string());
    // type-aware package roots: libs under <base>/libs, apps shimmed into
    // <base>/bin; the bare roots stay for backward compatibility.
    dirs.push_back(dir + "/coco_libs/libs");
    dirs.push_back(dir + "/coco_libs");
    if (!globalPkgDir().empty()) {
        dirs.push_back(globalPkgDir() + "/libs");
        dirs.push_back(globalPkgDir());
    }
    dirs.push_back(dir + "/../stdlib");
    dirs.push_back(dir + "/../../stdlib");
    if (const char* env = std::getenv("COCO_STDLIB")) dirs.push_back(env);
    dirs.push_back(dir);   // a project/package can import itself
    return dirs;
}

// install destinations, by package type:
//   lib -> <base>/libs/<name>   app -> <base>/apps/<name> (+ bin shim)
fs::path pkgBase(bool global_) {
    if (global_) {
        std::string g = globalPkgDir();
        return g.empty() ? fs::path(".kirn-pkg") : fs::path(g);
    }
    return fs::path("coco_libs");
}
fs::path pkgLibRoot(bool global_) { return pkgBase(global_) / "libs"; }

// append dir to the USER Path (registry-backed; survives new shells).
bool ensureUserPathContains(const std::string& dirRaw) {
    std::string dir = dirRaw;
    std::replace(dir.begin(), dir.end(), '/', '\\');
    char buf[32767];
    size_t n = 0;
    if (GetEnvironmentVariableA("PATH", buf, sizeof(buf)))
        for (const char* s = buf; *s;) {
            std::string part;
            while (*s && *s != ';') part += *s++;
            if (_stricmp(part.c_str(), dir.c_str()) == 0) return true;
            if (*s) ++s;
        }
    (void)n;
    std::string ps =
        "powershell -NoProfile -Command \"$p=[Environment]::"
        "GetEnvironmentVariable('Path','User'); if($p -notlike '*"
        + dir +
        "*'){[Environment]::SetEnvironmentVariable('Path', $p.TrimEnd(';')+';"
        + dir + "','User')}\"";
    return std::system(ps.c_str()) == 0;
}

// ---------------------------------------------------------------------------
// compile pipeline shared by run/test/build
// ---------------------------------------------------------------------------

// check one source; returns program or empty vector on error (diags printed)
std::vector<kirn::ast::StmtP> frontEnd(const std::string& path,
                                       const std::string& src,
                                       kirn::DiagEngine& diags) {
    auto toks = kirn::Lexer(src, path, diags).lexAll();
    if (diags.errorCount()) return {};
    auto body = kirn::Parser(toks, diags).parseProgram();
    if (diags.errorCount()) return {};
    kirn::sema::Checker chk(diags);
    chk.checkModule(body);
    return body;
}

void printDiags(const std::string& path, const kirn::DiagEngine& diags) {
    for (const auto& d : diags.diags())
        if (d.sev == kirn::Sev::Error || d.sev == kirn::Sev::InternalError)
            std::cerr << path << ":" << d.line << ":" << d.col
                      << ": error: " << d.message << "\n";
    for (const auto& d : diags.diags())
        if (d.sev == kirn::Sev::Warning || d.sev == kirn::Sev::Note)
            std::cerr << path << ":" << d.line << ":" << d.col
                      << ": warning[" << d.code << "]: " << d.message << "\n";
}

int runProgramSrc(const std::string& label, const std::string& src,
                  const std::vector<std::string>& dirs,
                  const std::map<std::string, std::string>& embedded,
                  const std::vector<std::string>& progArgs = {}) {
    kirn::DiagEngine diags;
    auto body = frontEnd(label, src, diags);
    if (diags.errorCount()) {
        printDiags(label, diags);
        return 65;
    }
    if (diags.warningCount()) printDiags(label, diags);
    kirn::ast::Stmt root;
    root.kind = kirn::ast::StKind::Pass;
    root.body = std::move(body);
    try {
        kirn::interp::Interpreter interp(root);
        for (const auto& d : dirs) interp.addStdlibDir(d);
        for (const auto& [name, esrc] : embedded)
            interp.addEmbeddedSource(name, esrc);
        interp.setProgramArgs(progArgs);
        interp.enableVm();   // bytecode VM is the default runner
        kirn::interp::Value r = interp.run();
        return r.k == kirn::interp::VK::Int ? (int)r.i : 0;
    } catch (const kirn::interp::PanicSignal& p) {
        fflush(stdout);
        fputs(("panic: " + p.msg + "\n").c_str(), stderr);
        return 70;
    } catch (const kirn::interp::SignalRaise&) {
        fflush(stdout);
        fputs("panic: uncaught raise escaped main\n", stderr);
        return 70;
    } catch (std::exception& ex) {
        std::cerr << "internal error: " << ex.what() << "\n";
        return 70;
    }
}

int runProgram(const std::string& entryPath,
               const std::vector<std::string>& dirs,
               const std::map<std::string, std::string>& embedded = {},
               const std::vector<std::string>& progArgs = {}) {
    std::string src;
    if (!readFile(entryPath, src)) {
        std::cerr << "kirn: cannot read '" << entryPath << "'\n";
        return 66;
    }
    return runProgramSrc(entryPath, src, dirs, embedded, progArgs);
}

// ---------------------------------------------------------------------------
// scaffolding: rust-style structure with Kirn folder names
// ---------------------------------------------------------------------------

int cmdNew(const std::string& name, bool lib) {
    if (name.empty() || name.find_first_of("/\\") != std::string::npos ||
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") !=
            std::string::npos) {
        std::cerr << "kirn new: '" << name << "' is not a valid package name\n";
        return 1;
    }
    const fs::path root(name);
    if (fs::exists(root)) {
        std::cerr << "kirn new: '" << name << "' already exists\n";
        return 1;
    }

    Manifest m;
    m.name = name;
    m.version = "0.0.1-beta";
    m.type = lib ? "lib" : "app";
    m.description = lib ? "A Kirn library" : "A Kirn application";
    m.license = "MIT";
    m.docs = "docs/index.md";
    m.readme = "README.md";
    m.author = "Your Name <you@example.com>";
    m.repo = "github.com/coco-lib/" + name;
    m.homepage = "https://github.com/coco-lib/" + name;
    m.authors.push_back("Your Name <you@example.com>");
    if (lib) {
        m.tags.push_back("utility");
        m.keywords.push_back("kirn");
        m.keywords.push_back(name);
    }
    m.gitIgnoreExtra.push_back(".kirn-registry-lib.toml");

    const std::string gitignore =
        "# kirn build output\n"
        "build/\n"
        "# installed dependencies\n"
        "coco_libs/\n"
        "# cached registry copy\n"
        ".kirn-registry-lib.toml\n"
        "# editor/OS noise\n"
        ".vscode/\n.idea/\n*.swp\nThumbs.db\n.DS_Store\n";

    if (lib) {
        m.main = "code/pin.kn";
        writeManifest(root, m);

        // pin.kn is the package initializer + public-API aggregator. It runs
        // once when the package is imported and re-exports the package's
        // `pub` surface (Python __init__ analogue).
        writeFile(root / "code" / "pin.kn",
                  "## " + name + " - a Kirn library.\n"
                  "##\n"
                  "## This pin.kn file is the package's public-API aggregator.\n"
                  "## It runs once when the package is imported, then the\n"
                  "## package's `pub` surface is available on the module.\n"
                  "## Doc comments starting with '##' sit above each `pub`:\n"
                  "## `kirn doc " + name + "` turns them into a browsable API\n"
                  "## reference.\n\n"
                  "## Say hello to someone.\n"
                  "pub def hello(who: string) -> string {\n"
                  "    return \"hello from " + name + ", \" + who + \"!\";\n}\n\n"
                  "## Shout it louder.\n"
                  "pub def shout(who: string) -> string {\n"
                  "    s = hello(who);\n"
                  "    out = \"\";\n"
                  "    for ch in s {\n"
                  "        c = ord(ch);\n"
                  "        if c >= 97 and c <= 122 {\n"
                  "            c = c - 32;\n"
                  "        }\n"
                  "        out = out + chr(c);\n"
                  "    }\n"
                  "    return out;\n}\n");

        writeFile(root / "tests" / (name + "_test.kn"),
                  "# tests live in tests/ and are named <file>_test.kn\n"
                  "# run them all with:  kirn test .\n"
                  "# (import the package's pin.kn initializer; installed\n"
                  "#  consumers import it by name: `import \"" + name + "\"`)\n\n"
                  "import \"code/pin.kn\" as " + name + ";\n\n"
                  "def main() {\n"
                  "    assert_eq(" + name + ".hello(\"no one\"),\n"
                  "              \"hello from " + name + ", no one!\");\n"
                  "    print(\"all tests passed\");\n}\n");

        writeFile(root / "docs" / "index.md",
                  "# " + name + "\n\n" + m.description +
                      ".\n\n## Install\n\n```bash\n"
                      "kirn install github.com/coco-lib/" + name +
                      "\n```\n\n## Usage\n\n```co\n"
                      "import \"" + name + "\"\n\n"
                      "print(" + name + ".hello(\"world\"))\n```\n");

        writeFile(root / "README.md",
                  "# " + name + "\n\n" + m.description +
                      ".\n\nDocs live in `docs/index.md` (the manifest's "
                      "`docs` entry). Regenerate the API section with:\n\n"
                      "```bash\nkirn doc " + name + "\nkirn test .\n"
                      "```\n\n## License\n\nMIT\n");
        writeFile(root / "LICENSE",
                  "MIT License\n\nCopyright (c) 2026 " + name +
                      " authors\n\nPermission is hereby granted, free of "
                      "charge, to any person obtaining a copy of this software "
                      "to deal in the Software without restriction.\n");
        writeFile(root / ".gitignore", gitignore);

        std::cout << "created library '" << name << "'\n"
                  << "  " << name << "/coco.toml      manifest\n"
                  << "  code/pin.kn   package initializer + pub API\n"
                  << "  tests/          *_test.kn files\n"
                  << "  docs/           markdown docs\n"
                  << "next:\n"
                  << "  cd " << name << " && kirn build lib && kirn test .\n";
    } else {
        m.main = "code/main.kn";
        writeManifest(root, m);

        writeFile(root / "code" / "main.kn",
                  "# " + name + " - a Kirn application.\n\n"
                  "def main() {\n"
                  "    print(\"hello from " + name + "\");\n}\n");

        writeFile(root / "tests" / "main_test.kn",
                  "# tests live in tests/ and are named <file>_test.kn\n\n"
                  "def main() {\n"
                  "    assert_eq(2 + 2, 4);\n"
                  "    print(\"all tests passed\");\n}\n");

        writeFile(root / "docs" / "index.md",
                  "# " + name + "\n\n" + m.description + ".\n\n## Run\n\n```"
                  "bash\nkirn run\nkirn test .\nkirn build\n```\n");

        writeFile(root / "README.md",
                  "# " + name + "\n\n" + m.description +
                      ".\n\n## Run\n\n```bash\nkirn run\n```\n");
        writeFile(root / ".gitignore", gitignore);

        std::cout << "created project '" << name << "'\n"
                  << "  " << name << "/coco.toml      manifest\n"
                  << "  code/main.kn    entry point\n"
                  << "  tests/          *_test.kn files\n"
                  << "  docs/           project docs\n"
                  << "next:\n"
                  << "  cd " << name << " && kirn run\n";
    }
    return 0;
}

// ---------------------------------------------------------------------------
// package sources: local path | git repo | kirn-libs registry
// ---------------------------------------------------------------------------

struct PkgRef {
    std::string spec;      // normalized repo path or local dir
    std::string tag;
    std::string destName;  // directory name under coco_libs/ or kirn-pkg/
    enum class Kind { Path, Git } kind = Kind::Git;
};

bool parsePkgRef(const std::string& raw, PkgRef& ref) {
    std::string spec = raw;
    size_t at = spec.find('@');
    if (at != std::string::npos && spec.find(':') == std::string::npos &&
        at > 0 && spec[0] != '.' && spec[0] != '/' && spec[0] != '\\') {
        ref.tag = spec.substr(at + 1);
        if (ref.tag.empty()) {
            std::cerr << "kirn install: empty version in '" << raw << "'\n";
            return false;
        }
        spec = spec.substr(0, at);
    }

    fs::path asPath(spec);
    bool looksLocal = asPath.is_absolute() ||
                      (!spec.empty() &&
                       (spec[0] == '.' || spec[0] == '/' || spec[0] == '\\'));
    if (looksLocal) {
        if (asPath.extension() == ".cocolib") return true;  // handled later
        if (!fs::is_directory(asPath)) {
            std::cerr << "kirn install: local package '" << raw
                      << "' does not exist\n";
            return false;
        }
        ref.kind = PkgRef::Kind::Path;
        ref.spec = fs::absolute(asPath).string();
        {   // prefer the manifest name over the folder name
            Manifest pm = readManifest(asPath);
            ref.destName = !pm.name.empty()
                               ? pm.name
                               : lastSegment(trimSlashes(spec));
        }
    } else {
        size_t slashes = std::count(spec.begin(), spec.end(), '/');
        if (slashes == 1) spec = "github.com/" + spec;   // user/repo shorthand
        if (std::count(spec.begin(), spec.end(), '/') < 2) {
            std::cerr << "kirn install: '" << raw
                      << "' is not user/repo, github.com/user/repo, a path,"
                         " or a registry name\n";
            return false;
        }
        ref.kind = PkgRef::Kind::Git;
        ref.spec = trimSlashes(spec);
        ref.destName = lastSegment(ref.spec);
    }
    if (ref.destName.empty() || ref.destName == "." || ref.destName == "..") {
        std::cerr << "kirn install: cannot derive a package name from '"
                  << raw << "'\n";
        return false;
    }
    return true;
}

// resolve a bare library name through the kirn-libs registry
// (github.com/coco-lib/coco-libs -> registry/lib.toml)
bool lookupRegistry(const std::string& name, std::string& url) {
    const std::string cache = ".kirn-registry-lib.toml";
    // refresh the cached registry copy; fall back to it when offline
    std::system("curl -s --max-time 15 -o .kirn-registry-lib.toml "
                "https://raw.githubusercontent.com/coco-lib/coco-libs/main/"
                "registry/lib.toml");
    std::string text;
    if (!readFile(cache, text)) return false;
    Doc d = kirn::tomlmini::parse(text);
    for (const auto& t : d.tables) {
        if (t.name != "lib") continue;
        auto n = t.kv.find("name");
        if (n == t.kv.end() || kirn::tomlmini::unquote(n->second) != name)
            continue;
        auto u = t.kv.find("url");
        if (u != t.kv.end()) url = kirn::tomlmini::unquote(u->second);
        return !url.empty();
    }
    return false;
}

std::string gitHeadSha(const fs::path& dir) {
    std::string cmd = "git -C \"" + dir.string() + "\" rev-parse HEAD 2>" + shellNullDevice();
    // capture via temp file (system() has no portable pipe-back)
    std::string tmp = ".kirn-sha";
    std::string full = cmd + " > \"" + tmp + "\"";
    if (std::system(full.c_str()) != 0) return "";
    std::string sha;
    readFile(tmp, sha);
    std::remove(tmp.c_str());
    while (!sha.empty() && (sha.back() == '\n' || sha.back() == '\r'))
        sha.pop_back();
    return sha.size() >= 7 ? sha.substr(0, 12) : sha;
}

// copy a package source into place; returns manifest of the installed copy
bool materializePackage(const PkgRef& ref, const fs::path& dest,
                        Manifest& outManifest, std::string& commitSha) {
    std::error_code ec;
    if (ref.kind == PkgRef::Kind::Path) {
        fs::remove_all(dest, ec);
        if (ec) {
            std::cerr << "kirn install: cleanup failed: " << ec.message()
                      << "\n";
            return false;
        }
        if (!copyTree(ref.spec, dest)) return false;
        commitSha.clear();
    } else {
if (std::system(("git --version >" + std::string(shellNullDevice()) + " 2>" + shellNullDevice()).c_str()) != 0) {
            std::cerr << "kirn install: git not found on PATH\n";
            return false;
        }
        fs::remove_all(dest, ec);
        fs::create_directories(dest.parent_path(), ec);
        std::string url = "https://" + ref.spec + ".git";
        std::string cmd =
            "git clone --quiet --depth 1" +
            std::string(ref.tag.empty() ? "" : " --branch " + ref.tag) + " " +
            url + " \"" + dest.string() + "\"";
        if (std::system(cmd.c_str()) != 0) {
            std::cerr << "kirn install: git clone failed for " << url << "\n";
            return false;
        }
        commitSha = gitHeadSha(dest);
        fs::remove_all(dest / ".git", ec);   // keep installs lean
    }
    outManifest = readManifest(dest);
    if (outManifest.name.empty())
        outManifest.name = ref.destName;   // manifest-less packages still work
    return true;
}

void upsertLock(std::vector<LockEntry>& locks, LockEntry e) {
    for (auto& x : locks)
        if (x.name == e.name) {
            x = e;
            return;
        }
    locks.push_back(e);
}

int installOne(const PkgRef& ref, const std::string& raw, bool global_,
               bool record, int depth = 0);

// install the [dependencies] declared by the package at pkgDir
static void installPkgDeps(const Manifest& pkg, const fs::path& pkgDir,
                           bool global_, int depth) {
    if (depth > 8) return;
    for (const auto& [name, spec] : pkg.deps) {
        // already present in the target root?
        if (fs::exists(pkgLibRoot(global_) / name)) continue;
        PkgRef dref;
        dref.destName = name;
        if (spec.count("path")) {
            fs::path p = fs::path(spec.at("path"));
            if (p.is_relative()) p = pkgDir / p;
            if (!fs::exists(p)) {
                std::cerr << "  warning: dependency '" << name
                          << "' path not found: " << p.string() << "\n";
                continue;
            }
            dref.kind = PkgRef::Kind::Path;
            dref.spec = p.string();
        } else if (spec.count("git")) {
            dref.kind = PkgRef::Kind::Git;
            dref.spec = spec.at("git");
            if (spec.count("tag")) dref.tag = spec.at("tag");
        } else if (spec.count("version")) {
            std::string url;
            if (!lookupRegistry(name, url)) continue;
            dref.kind = PkgRef::Kind::Git;
            dref.spec = url;
        } else
            continue;
        installOne(dref, dref.spec, global_, /*record=*/false, depth + 1);
    }
}

// Install one resolved reference. Returns process exit code.
// Type-aware layout (like cargo/npm hybrids):
//   lib -> <base>/libs/<name>          (importable)
//   app -> <base>/libs/<name>          (sources stay importable/runnable)
//          + <base>/bin/<name>.cmd     (shim launcher)
//          + global installs append <base>/bin to the USER Path
int installOne(const PkgRef& ref, const std::string& raw, bool global_,
               bool record, int depth) {
    fs::path base = pkgBase(global_);
    const fs::path libDest = pkgLibRoot(global_) / ref.destName;
    const fs::path binDir = base / "bin";

    Manifest pkg;
    std::string sha;
    if (!materializePackage(ref, libDest, pkg, sha)) return 1;

    // bring the package's own dependencies along (transitively)
    if (!pkg.deps.empty()) installPkgDeps(pkg, libDest, global_, depth);

    bool isApp = pkg.type == "app";
    if (isApp) {
        std::error_code ec;
        fs::create_directories(binDir, ec);
        // resolve this kirn executable so the shim works off-PATH too
        char kirnBuf[MAX_PATH * 2];
        GetModuleFileNameA(nullptr, kirnBuf, sizeof kirnBuf);
        std::string kirnExe = kirnBuf;
        for (char& c : kirnExe)
            if (c == '/') c = '\\';
        fs::path absLib = fs::weakly_canonical(libDest, ec);
        if (absLib.empty()) absLib = fs::absolute(libDest);
        std::ostringstream sh;
        sh << "@echo off\r\n"
           << "\"" << kirnExe << "\" run \""
           << absLib.string() << "\" %*\r\n";
        writeFile(binDir / (ref.destName + ".cmd"), sh.str());
        if (global_) {
            if (ensureUserPathContains((fs::absolute(binDir) / "").string()))
                std::cout << "  PATH updated - restart your shell to use '"
                          << ref.destName << "' anywhere\n";
            else
                std::cerr << "  warning: could not update PATH\n";
        } else {
            std::cout << "  app shim: "
                      << (fs::absolute(binDir) / (ref.destName + ".cmd"))
                             .string()
                      << "\n";
        }
    }

    if (!global_ && record) {
        // record dependency + lock entry in this project
        Manifest proj = readManifest(".");
        Deps::mapped_type spec;
        if (ref.kind == PkgRef::Kind::Path)
            spec["path"] = raw;
        else
            spec["git"] = ref.spec;
        if (!ref.tag.empty()) spec["tag"] = ref.tag;
        proj.deps[ref.destName] = spec;
        writeManifest(".", proj);

        std::vector<LockEntry> locks = readLock(".");
        LockEntry le;
        le.name = ref.destName;
        le.source = ref.kind == PkgRef::Kind::Path ? "path" : "git";
        le.url = ref.spec;
        le.tag = ref.tag;
        le.commit = sha;
        le.version = pkg.version;
        le.installed = todayIso();
        upsertLock(locks, le);
        writeLock(".", locks);
    }

    std::cout << (record ? "installed " : "installed ")
              << (ref.kind == PkgRef::Kind::Path ? raw : ref.spec);
    if (!ref.tag.empty()) std::cout << "@" << ref.tag;
    std::cout << " (" << (pkg.version.empty() ? "?" : pkg.version) << ", "
              << (isApp ? "app" : "lib") << ") -> " << (libDest / "").string()
              << (global_ ? "  [global]" : "") << "\n";
    return 0;
}

// resolve a raw spec (bare registry name | path | user/repo | .cocolib)
static bool resolveRaw(const std::string& raw, PkgRef& ref);
int unpackCocolib(const std::string& raw, bool global_);
int unpackCocolib(const std::string& raw, bool global_);
static bool resolveRaw(const std::string& raw, PkgRef& ref) {
    // .cocolib bundle?
    if (fs::is_regular_file(raw) && fs::path(raw).extension() == ".cocolib")
        return unpackCocolib(raw, /*global_=*/false) == 0;
    // local directory -> install by its manifest name
    if (fs::is_directory(raw)) {
        Manifest pm = readManifest(raw);
        std::string n = pm.name;
        if (n.empty()) {
            std::string base = fs::path(raw).filename().string();
            n = base.empty() ? fs::path(fs::absolute(raw)).filename().string()
                             : base;
        }
        if (n.empty()) {
            std::cerr << "kirn install: cannot derive a package name from '"
                      << raw << "'\n";
            return false;
        }
        ref.kind = PkgRef::Kind::Path;
        ref.spec = raw;
        ref.destName = n;
        return true;
    }
    // bare name -> look it up in the kirn-libs registry
    bool isBareName =
        raw.find('/') == std::string::npos &&
        raw.find('\\') == std::string::npos && raw.find(':') == std::string::npos &&
        !raw.empty() && raw[0] != '.' && !fs::is_directory(raw);
    if (isBareName) {
        std::string url;
        if (!lookupRegistry(raw, url)) {
            std::cerr << "kirn install: '" << raw
                      << "' not found in the kirn-libs registry\n"
                      << "  browse: https://github.com/coco-lib/coco-libs\n";
            return false;
        }
        ref.kind = PkgRef::Kind::Git;
        ref.spec = url;
        ref.destName = raw;
        return true;
    }
    return parsePkgRef(raw, ref);
}

int cmdInstall(const std::string& raw, bool global_) {
    PkgRef ref;
    if (!resolveRaw(raw, ref)) return 1;
    return installOne(ref, raw, global_, /*record=*/true);
}

// `kirn add` — npm-install / go-mod-tidy style sync.
//   kirn add <pkg>...   resolve + install + record each dependency
//   kirn add            (no args) tidy: install every manifest dep that is
//                       missing from coco_libs/libs
int cmdAdd(const std::vector<std::string>& pkgs) {
    if (pkgs.empty()) {
        Manifest proj = readManifest(".");
        if (proj.deps.empty()) {
            std::cout << "nothing to sync ([dependencies] is empty)\n";
            return 0;
        }
        int rc = 0, done = 0;
        for (const auto& [name, spec] : proj.deps) {
            if (fs::exists(pkgLibRoot(false) / name) ||
                fs::exists(fs::path("coco_libs") / name))
                continue;   // already present
            PkgRef ref;
            ref.destName = name;
            if (spec.count("path")) {
                ref.kind = PkgRef::Kind::Path;
                ref.spec = spec.at("path");
            } else if (spec.count("git")) {
                ref.kind = PkgRef::Kind::Git;
                ref.spec = spec.at("git");
                if (spec.count("tag")) ref.tag = spec.at("tag");
            } else if (spec.count("version")) {
                std::string url;
                if (!lookupRegistry(name, url)) {
                    std::cerr << "kirn add: '" << name
                              << "' not found in the registry\n";
                    rc = 1;
                    continue;
                }
                ref.kind = PkgRef::Kind::Git;
                ref.spec = url;
            } else
                continue;
            if (installOne(ref, ref.spec, false, /*record=*/false) == 0) ++done;
            else rc = 1;
        }
        std::cout << "synced " << done << " package"
                  << (done == 1 ? "" : "s") << "\n";
        return rc;
    }
    int rc = 0;
    for (const auto& p : pkgs) {
        PkgRef ref;
        if (!resolveRaw(p, ref)) {
            rc = 1;
            continue;
        }
        if (installOne(ref, p, false, /*record=*/true) != 0) rc = 1;
    }
    return rc;
}

int cmdUpdate(const std::string& only) {
    Manifest proj = readManifest(".");
    std::vector<LockEntry> locks = readLock(".");
    if (proj.deps.empty()) {
        std::cout << "no dependencies to update\n";
        return 0;
    }
    int rc = 0;
    for (const auto& [name, spec] : proj.deps) {
        if (!only.empty() && name != only) continue;
        PkgRef ref;
        ref.destName = name;
        if (spec.count("path")) {
            ref.kind = PkgRef::Kind::Path;
            ref.spec = spec.at("path");
        } else if (spec.count("git")) {
            ref.kind = PkgRef::Kind::Git;
            ref.spec = spec.at("git");
        } else if (spec.count("version")) {
            std::string url;
            if (!lookupRegistry(name, url)) {
                std::cerr << "kirn update: '" << name
                          << "' not found in the kirn-libs registry\n";
                rc = 1;
                continue;
            }
            ref.kind = PkgRef::Kind::Git;
            ref.spec = url;
        } else {
            continue;
        }
        ref.tag = spec.count("tag") ? spec.at("tag") : "";

        Manifest pkg;
        std::string sha;
        if (!materializePackage(ref, pkgLibRoot(false) / name, pkg, sha)) {
            rc = 1;
            continue;
        }
        LockEntry le;
        le.name = name;
        le.source = ref.kind == PkgRef::Kind::Path ? "path" : "git";
        le.url = ref.spec;
        le.tag = ref.tag;
        le.commit = sha;
        le.version = pkg.version;
        le.installed = todayIso();
        upsertLock(locks, le);
        std::cout << "updated " << name << " -> "
                  << (pkg.version.empty() ? "?" : pkg.version)
                  << (sha.empty() ? "" : " (" + sha + ")") << "\n";
    }
    writeLock(".", locks);
    return rc;
}

int cmdRemove(const std::string& name) {
    Manifest proj = readManifest(".");
    std::vector<LockEntry> locks = readLock(".");
    bool hadDep = proj.deps.erase(name) > 0;
    size_t before = locks.size();
    locks.erase(std::remove_if(locks.begin(), locks.end(),
                               [&](const LockEntry& l) {
                                   return l.name == name;
                               }),
                locks.end());
    std::error_code ec;
    fs::remove_all(pkgLibRoot(false) / name, ec);
    fs::remove_all(fs::path("coco_libs") / name, ec);   // legacy layout
    fs::remove_all(fs::path("coco_libs") / "bin" / (name + ".cmd"), ec);
    writeManifest(".", proj);
    writeLock(".", locks);
    std::cout << "removed " << name;
    if (!hadDep && locks.size() == before)
        std::cout << " (was not a recorded dependency)";
    std::cout << "\n";
    return 0;
}

int cmdList() {
    auto show = [](const char* label, const fs::path& base) {
        std::error_code ec;
        if (!fs::is_directory(base)) return;
        for (fs::directory_iterator it(base, ec), end; !ec && it != end;
             it.increment(ec)) {
            if (ec || !it->is_directory(ec)) continue;
            std::string fn = it->path().filename().string();
            if (fn == "bin" || fn == "libs") continue;   // layout roots
            Manifest m = readManifest(it->path());
            std::cout << label << fn << " "
                      << (m.version.empty() ? "?" : m.version);
            if (!m.description.empty())
                std::cout << "  # " << m.description;
            std::cout << "\n";
        }
    };
    show("", pkgLibRoot(false));
    show("", fs::path("coco_libs"));   // legacy layout
    if (!globalPkgDir().empty()) {
        show("[global] ", fs::path(globalPkgDir()) / "libs");
        show("[global] ", fs::path(globalPkgDir()));
        // globally installed apps (bin shims)
        std::error_code ec;
        fs::path bin = fs::path(globalPkgDir()) / "bin";
        if (fs::is_directory(bin))
            for (fs::directory_iterator it(bin, ec), end; !ec && it != end;
                 it.increment(ec)) {
                if (ec || !it->is_regular_file(ec)) continue;
                if (it->path().extension() == ".cmd")
                    std::cout << "[global:bin] "
                              << it->path().stem().string() << "\n";
            }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// kirn clone — clone any git repo (shorthand-aware)
//   kirn clone user/repo            -> github.com/user/repo
//   kirn clone github.com/u/r       full host forms work too
//   kirn clone https://host/u/r
//   kirn clone <spec> --full        keep full history (default: depth 1)
// ---------------------------------------------------------------------------

int cmdClone(const std::string& spec, bool full) {
    if (std::system(("git --version >" + std::string(shellNullDevice()) + " 2>" + shellNullDevice()).c_str()) != 0) {
        std::cerr << "kirn clone: git not found on PATH\n";
        return 1;
    }
    std::string url = spec;
    if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0 ||
        url.rfind("git@", 0) == 0 || url.find("://") != std::string::npos) {
        // already a URL
    } else {
        // bare user/repo -> assume github; host/u/r keeps its host
        size_t slash = url.find('/');
        std::string head =
            slash == std::string::npos ? url : url.substr(0, slash);
        bool hasHost = head.find('.') != std::string::npos ||
                       head.find(':') != std::string::npos;
        if (!hasHost) url = "github.com/" + url;
        url = "https://" + url + ".git";
    }
    // destination dir = repo name
    std::string name = spec;
    while (!name.empty() && name.back() == '/') name.pop_back();
    size_t s2 = name.find_last_of('/');
    name = s2 == std::string::npos ? name : name.substr(s2 + 1);
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".git") == 0)
        name.erase(name.size() - 4);
    if (fs::exists(name)) {
        std::cerr << "kirn clone: '" << name << "' already exists here\n";
        return 1;
    }
    std::string cmd =
        "git clone --quiet" + std::string(full ? "" : " --depth 1") + " " +
        url + " \"" + name + "\"";
    std::cout << "cloning " << url << " ...\n";
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        std::cerr << "kirn clone: failed (" << rc << ")\n";
        return rc == 0 ? 1 : rc;
    }
    Manifest m = readManifest(name);
    std::cout << "cloned into ./" << name;
    if (!m.name.empty())
        std::cout << " - " << m.name << " v"
                  << (m.version.empty() ? "?" : m.version)
                  << (m.type == "lib" ? " [lib]" : " [app]");
    std::cout << "\nnext:\n  cd " << name << " && kirn run | kirn test .\n";
    return 0;
}

// ---------------------------------------------------------------------------
// kirn list online — browse the kirn-libs registry
// ---------------------------------------------------------------------------

int cmdListOnline() {
    const std::string cache = ".kirn-registry-lib.toml";
    std::system(
        "curl -s --max-time 15 -o .kirn-registry-lib.toml "
        "https://raw.githubusercontent.com/coco-lib/coco-libs/refs/heads/"
        "main/registry/lib.toml");
    std::string text;
    if (!readFile(cache, text)) {
        std::cerr << "kirn list online: cannot reach "
                     "raw.githubusercontent.com/coco-lib/coco-libs\n";
        return 1;
    }
    Doc d = kirn::tomlmini::parse(text);
    struct Row { std::string name, desc, url, ver; };
    std::vector<Row> rows;
    // [[lib]] arrays-of-tables arrive in order in Doc::tables
    for (const auto& t : d.tables) {
        if (t.name != "lib") continue;
        auto gv = [&](const char* k) {
            auto it = t.kv.find(k);
            return it == t.kv.end() ? "" : kirn::tomlmini::unquote(it->second);
        };
        // versions = ["0.1.0", ...] -> advertise the latest
        std::string ver = gv("version");
        if (ver.empty()) {
            std::string arr = gv("versions");
            size_t a = arr.find('['), b = arr.rfind(']');
            if (a != std::string::npos && b != std::string::npos) {
                std::string body = arr.substr(a + 1, b - a - 1);
                size_t last = body.rfind('"');
                size_t prev = last == std::string::npos
                                  ? std::string::npos
                                  : body.rfind('"', last - 1 < body.size()
                                                          ? last - 1
                                                          : 0);
                if (last != std::string::npos &&
                    prev != std::string::npos && prev < last)
                    ver = body.substr(prev + 1, last - prev - 1);
            }
        }
        rows.push_back({gv("name"), gv("description"), gv("url"), ver});
    }
    if (rows.empty()) {
        std::cout << "registry is empty (be the first to publish!)\n";
        return 0;
    }
    size_t wName = 4, wVer = 7;
    for (const auto& r : rows) {
        if (r.name.size() > wName) wName = r.name.size();
        if (r.ver.size() > wVer) wVer = r.ver.size();
    }
    std::cout << "NAME" << std::string(wName - 4 + 2, ' ') << "VERSION"
              << std::string(wVer - 7 + 2, ' ') << "URL\n";
    for (const auto& r : rows) {
        std::cout << r.name << std::string(wName - r.name.size() + 2, ' ')
                  << (r.ver.empty() ? "?" : r.ver)
                  << std::string(wVer - r.ver.size() + 2, ' ') << r.url
                  << "\n";
        if (!r.desc.empty())
            std::cout << std::string(wName + wVer + 6, ' ') << "# " << r.desc
                      << "\n";
    }
    std::cout << rows.size() << " package(s)\ninstall with: kirn install "
              << "<name>\n";
    return 0;
}

bool isTestFile(const fs::path& p) {
    std::string n = p.filename().string();
    const std::string suffix = "_test.kn";
    return p.extension() == ".kn" && n.size() > suffix.size() &&
           n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0;
}

void collectTestFiles(const fs::path& dir, std::vector<fs::path>& out) {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (ec) break;
        const fs::path& p = it->path();
        std::string fn = p.filename().string();
        if (fn == "coco_libs" || fn == "build" || fn == ".git") {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(ec) && isTestFile(p)) out.push_back(p);
    }
}

int cmdTest(const std::vector<std::string>& args, size_t from) {
    std::vector<fs::path> files;
    if (from == args.size() || args[from] == ".") {
        collectTestFiles(".", files);
        if (files.empty()) {
            std::cout << "no *_test.kn files found\n";
            return 0;
        }
    } else {
        bool any = false;
        for (size_t i = from; i < args.size(); ++i) {
            fs::path p(args[i]);
            if (fs::is_directory(p)) {
                collectTestFiles(p, files);
                any = true;
            } else if (isTestFile(p)) {
                files.push_back(p);
                any = true;
            } else {
                std::cerr << "kirn test: '" << args[i]
                          << "' is not a <name>_test.kn file or directory\n";
                return 64;
            }
        }
        if (!any) return 64;
    }

    // test files live under tests/, but imports resolve from the project root
    auto dirs = libDirsFor(".");
    if (std::find(dirs.begin(), dirs.end(), ".") == dirs.end())
        dirs.push_back(".");
    // the repo ships its stdlib as Kirn source under ./stdlib; tests import it
    if (std::find(dirs.begin(), dirs.end(), "./stdlib") == dirs.end())
        dirs.push_back("./stdlib");
    int pass = 0, fail = 0;
    for (const auto& f : files) {
        std::cout << "test " << f.string() << " ... " << std::flush;
        int rc = runProgram(f.string(), dirs);
        if (rc == 0) {
            std::cout << "PASS\n";
            ++pass;
        } else {
            std::cout << "FAIL (rc=" << rc << ")\n";
            ++fail;
        }
    }
    std::cout << pass << " passed, " << fail << " failed\n";
    return fail == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// inline docs: '##' lines above a pub def -> docs/api.md
// ---------------------------------------------------------------------------

std::string extractApiDocs(const Manifest& m) {
    std::ostringstream o;
    o << "## API reference - " << m.name << "\n\n";
    if (!m.description.empty()) o << m.description << "\n\n";
    o << "_This section is regenerated by `kirn doc " << m.name
      << "` from inline `##` doc comments._\n\n";
    std::error_code ec;
    bool anyFn = false;
    std::vector<fs::path> sources;
    for (fs::directory_iterator it("code", ec), end; !ec && it != end;
         it.increment(ec))
        if (!ec && it->is_regular_file(ec) &&
            it->path().extension() == ".kn")
            sources.push_back(it->path());
    std::sort(sources.begin(), sources.end());
    for (const auto& f : sources) {
        std::string src;
        if (!readFile(f.string(), src)) continue;
        std::vector<std::string> lines;
        {
            std::istringstream in(src);
            std::string l;
            while (std::getline(in, l)) lines.push_back(l);
        }
        bool fileHeaded = false;
        std::string pendingDoc;
        for (const auto& line : lines) {
            std::string t = kirn::tomlmini::strip(line);
            if (t.rfind("##", 0) == 0 && t.compare(0, 3, "###") != 0) {
                std::string d = kirn::tomlmini::strip(t.substr(2));
                if (!d.empty()) pendingDoc += d + "\n";
                continue;
            }
            if (t.rfind("pub def ", 0) == 0) {
                if (!fileHeaded) {
                    o << "## " << lastSegment(f.string()) << "\n\n";
                    fileHeaded = true;
                }
                anyFn = true;
                o << "### `" << t.substr(8) << "`\n\n```co\n"
                  << t << "\n```\n\n";
                o << (pendingDoc.empty() ? "_no doc comment_\n" : pendingDoc);
                o << "\n";
                pendingDoc.clear();
                continue;
            }
            // any other statement or blank line detaches the doc block
            if (!pendingDoc.empty() &&
                (t.empty() || t.rfind("##", 0) != 0))
                pendingDoc.clear();
        }
    }
    if (!anyFn) o << "_No public functions found yet. Mark exports with "
                     "`pub def` and document them with `##` comments._\n";
    return o.str();
}

std::string extractApiDocs(const Manifest& m);

// merge the generated API section into the manifest's docs entry file
// (default docs/index.md), replacing any previous auto-generated block
bool regenerateDocs(const Manifest& m) {
    std::string entry = m.docs.empty() ? "docs/index.md" : m.docs;
    const char* beginMark = "<!-- kirn-docs:start -->";
    const char* endMark = "<!-- kirn-docs:end -->";
    std::string existing;
    readFile(entry, existing);
    size_t start = existing.find(beginMark);
    if (start == std::string::npos) {
        // strip a legacy auto-generated "# API reference" page content
        start = existing.size();
        existing += "\n";
    } else {
        // keep everything before the marker
    }
    size_t end = existing.find(endMark, start);
    std::string head = existing.substr(0, start);
    if (!head.empty() && head.back() != '\n') head += "\n";
    std::string tail =
        end == std::string::npos ? "" : existing.substr(end + strlen(endMark));
    std::ostringstream body;
    body << head << beginMark << "\n" << extractApiDocs(m) << endMark << "\n"
         << tail;
    return writeFile(entry, body.str());
}

// ---------------------------------------------------------------------------
// kirn doc — markdown viewer over HTTP
// ---------------------------------------------------------------------------

std::string mdEscape(const std::string& s) {
    std::string o;
    for (char c : s)
        if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else if (c == '&') o += "&amp;";
        else o += c;
    return o;
}

std::string mdInline(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '<') { o += "&lt;"; continue; }
        if (s[i] == '&') { o += "&amp;"; continue; }
        if (s[i] == '`') {
            size_t j = s.find('`', i + 1);
            if (j != std::string::npos) {
                o += "<code>" + mdEscape(s.substr(i + 1, j - i - 1)) + "</code>";
                i = j;
                continue;
            }
        }
        if (s[i] == '*' && i + 1 < s.size() && s[i + 1] == '*') {
            size_t j = s.find("**", i + 2);
            if (j != std::string::npos) {
                o += "<b>" + mdEscape(s.substr(i + 2, j - i - 2)) + "</b>";
                i = j + 1;
                continue;
            }
        }
        if (s[i] == '_' && i + 1 < s.size()) {
            size_t j = s.find('_', i + 2);
            if (j != std::string::npos && (i == 0 || !isalnum((unsigned char)s[i-1]))) {
                o += "<i>" + mdEscape(s.substr(i + 1, j - i - 1)) + "</i>";
                i = j;
                continue;
            }
        }
        o += s[i];
    }
    return o;
}

std::string mdToHtml(const std::string& md) {
    std::ostringstream o;
    bool inCode = false, inList = false;
    std::vector<std::string> lines;
    {
        std::istringstream in(md);
        std::string l;
        while (std::getline(in, l)) lines.push_back(l);
    }
    for (const auto& line : lines) {
        std::string t = kirn::tomlmini::strip(line);
        if (t.rfind("```", 0) == 0) {
            if (inList) { o << "</ul>\n"; inList = false; }
            o << (inCode ? "</code></pre>\n" : "<pre><code>");
            inCode = !inCode;
            continue;
        }
        if (inCode) { o << line << "\n"; continue; }
        if (t.empty()) {
            if (inList) { o << "</ul>\n"; inList = false; }
            continue;
        }
        int h = 0;
        while (h < (int)t.size() && t[h] == '#' && h < 6) ++h;
        if (h > 0 && h < (int)t.size() && t[h] == ' ') {
            if (inList) { o << "</ul>\n"; inList = false; }
            o << "<h" << h << ">" << mdInline(t.substr(h + 1)) << "</h" << h
              << ">\n";
            continue;
        }
        if (t == "---") {
            o << "<hr>\n";
            continue;
        }
        if ((t.rfind("- ", 0) == 0) || t.rfind("* ", 0) == 0) {
            if (!inList) { o << "<ul>\n"; inList = true; }
            o << "<li>" << mdInline(t.substr(2)) << "</li>\n";
            continue;
        }
        if (inList) { o << "</ul>\n"; inList = false; }
        o << "<p>" << mdInline(t) << "</p>\n";
    }
    if (inCode) o << "</code></pre>\n";
    if (inList) o << "</ul>\n";
    return o.str();
}

std::string pageHtml(const std::string& title, const std::string& bodyHtml,
                     const std::string& nav) {
    std::ostringstream o;
    o << "<!doctype html><html><head><meta charset=\"utf-8\">"
      << "<title>" << title << " - kirn docs</title><style>"
      << "body{font-family:Segoe UI,system-ui,sans-serif;margin:0;display:flex}"
      << "nav{width:230px;min-height:100vh;background:#1d2433;color:#cfd8e6;"
      << "padding:18px 14px;box-sizing:border-box}nav a{color:#7fb4ff;"
      << "display:block;margin:6px 0;text-decoration:none}nav b{color:#fff}"
      << "main{padding:26px 34px;max-width:860px}pre{background:#f4f6fa;"
      << "padding:12px;border-radius:6px;overflow:auto}code{background:#eef;"
      << "padding:1px 5px;border-radius:3px}pre code{background:none}"
      << "h1,h2{border-bottom:1px solid #eee;padding-bottom:6px}</style>"
      << "</head><body><nav><b>" << nav << "</b><br>" << "</nav>"
      << "<main>" << bodyHtml << "</main></body></html>";
    return o.str();
}

#ifdef _WIN32
int serveDocs(const fs::path& docsDir, int port, const std::string& libName,
              const std::string& entryFile) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "kirn doc: WSAStartup failed\n";
        return 1;
    }
    SOCKET srv = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((u_short)port);
    int reuse = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse,
               sizeof reuse);
    if (bind(srv, (sockaddr*)&addr, sizeof addr) != 0 ||
        listen(srv, 8) != 0) {
        std::cerr << "kirn doc: cannot bind port " << port << "\n";
        return 1;
    }

    // nav sidebar: home (docs entry) + every markdown file
    auto navLinks = [&]() {
        std::error_code ec;
        std::ostringstream n;
        n << libName << "<br><a href=\"/\">index</a>";
        for (fs::recursive_directory_iterator it(docsDir, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (ec || it->path().extension() != ".md") continue;
            std::string rel =
                fs::relative(it->path(), docsDir, ec).generic_string();
            if (rel == entryFile) continue;   // already linked as index
            n << "<br><a href=\"/" << rel << "\">"
              << fs::path(rel).stem().string() << "</a>";
        }
        return n.str();
    };

    std::cout << "docs for '" << libName << "' at http://localhost:" << port
              << "/  (Ctrl+C to stop)\n";
    for (;;) {
        SOCKET c = accept(srv, nullptr, nullptr);
        if (c == INVALID_SOCKET) break;
        char req[2048] = {};
        recv(c, req, sizeof req - 1, 0);
        std::string method, pathStr;
        {
            std::istringstream rl(req);
            rl >> method >> pathStr;
        }
        std::string body;
        if (method != "GET") {
            body = "405";
        } else {
            // "/" renders the manifest-declared docs entry (index.md)
            std::string p = pathStr == "/" ? "/" + entryFile : pathStr;
            for (auto& ch : p)
                if (ch == '?') { p.resize(&ch - &p[0]); break; }
            std::string clean = p.substr(1);
            for (auto& ch : clean) if (ch == '\\') ch = '/';
            if (clean.find("..") != std::string::npos) {
                body = "403 forbidden";
            } else {
                fs::path file = docsDir / clean;
                std::string md;
                if (readFile(file.string(), md))
                    body = pageHtml(clean, mdToHtml(md), navLinks());
                else
                    body = "404 not found";
            }
        }
        std::ostringstream resp;
        resp << "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
             << "Content-Length: " << body.size()
             << "\r\nConnection: close\r\n\r\n"
             << body;
        std::string out = resp.str();
        send(c, out.data(), (int)out.size(), 0);
        closesocket(c);
    }
    WSACleanup();
    return 0;
}
#else
int serveDocs(const fs::path&, int, const std::string&,
              const std::string&) {
    std::cerr << "kirn doc: http server not supported on this platform\n";
    return 1;
}
#endif

int cmdDoc(const std::string& target, int port) {
    // resolve target: dir | installed name
    fs::path libDir(target);
    if (!fs::is_directory(libDir)) {
        fs::path local = pkgLibRoot(false) / target;
        fs::path legacy = fs::path("coco_libs") / target;
        fs::path glob, globLegacy;
        if (!globalPkgDir().empty()) {
            glob = fs::path(globalPkgDir()) / "libs" / target;
            globLegacy = fs::path(globalPkgDir()) / target;
        }
        if (fs::is_directory(local))
            libDir = local;
        else if (fs::is_directory(legacy))
            libDir = legacy;
        else if (!glob.empty() && fs::is_directory(glob))
            libDir = glob;
        else if (!globLegacy.empty() && fs::is_directory(globLegacy))
            libDir = globLegacy;
        else {
            std::cerr << "kirn doc: no project or installed library '"
                      << target << "'\n";
            return 1;
        }
    }
    // serve/generate with absolute paths (server loop changes no cwd)
    std::error_code lec;
    libDir = fs::weakly_canonical(fs::absolute(libDir), lec);

    Manifest m = readManifest(libDir);
    if (m.name.empty()) m.name = lastSegment(libDir.string());

    // regenerate the API section inside the manifest-declared docs entry
    std::string entryRel = m.docs.empty() ? "docs/index.md" : m.docs;
    for (char& c : entryRel)
        if (c == '\\') c = '/';
    {
        std::string oldPwd = fs::current_path().string();
        std::error_code ec;
        fs::current_path(libDir, ec);
        regenerateDocs(m);
        fs::current_path(oldPwd, ec);
    }
    fs::path entryPath = libDir / fs::path(entryRel);
    std::cout << "docs entry: " << entryPath.string() << "\n";
    return serveDocs(entryPath.parent_path(), port, m.name,
                     entryPath.filename().string());
}

// ---------------------------------------------------------------------------
// kirn build — app: standalone .exe (sources embedded, interpreter linked)
//              lib: type-check all sources + pack distributable .cocolib
// ---------------------------------------------------------------------------

// resolve a module name to a source file across the given dirs (loader rules)
bool resolveSource(const std::string& dotted,
                   const std::vector<std::string>& dirs, std::string& path,
                   std::string& src) {
    std::string name = dotted;
    if (name.size() > 3 && name.compare(name.size() - 3, 3, ".kn") == 0)
        name.erase(name.size() - 3);   // explicit suffix form
    std::string rel;
    for (char c : name) rel += (c == '.' || c == '/') ? '/' : c;
    rel += ".kn";
    for (const auto& d : dirs) {
        std::string cand = d + "/" + rel;
        if (readFile(cand, src)) {
            path = cand;
            return true;
        }
        std::string pkgDir = d + "/" + rel.substr(0, rel.size() - 3);
        if (!pkgDir.empty() && fs::is_directory(pkgDir)) {
            // package entry: coco.toml main / mod.kn / <dir>.kn / lone *.kn
            Manifest pm = readManifest(pkgDir);
            std::vector<std::string> cands;
            if (!pm.main.empty()) cands.push_back(pm.main);
            cands.push_back("mod.kn");
            cands.push_back(lastSegment(trimSlashes(pkgDir)) + ".kn");
            for (const auto& rel2 : cands)
                if (readFile((fs::path(pkgDir) / rel2).string(), src)) {
                    path = (fs::path(pkgDir) / rel2).string();
                    return true;
                }
            std::error_code ec;
            std::vector<fs::path> tops;
            for (fs::directory_iterator it(pkgDir, ec), end; !ec && it != end;
                 it.increment(ec))
                if (!ec && it->is_regular_file(ec) &&
                    it->path().extension() == ".kn")
                    tops.push_back(it->path());
            if (tops.size() == 1 && readFile(tops[0].string(), src)) {
                path = tops[0].string();
                return true;
            }
        }
    }
    return false;
}

void collectImports(const std::string& path, const std::string& src,
                    std::vector<std::string>& names) {
    kirn::DiagEngine diags;
    auto toks = kirn::Lexer(src, path, diags).lexAll();
    if (diags.errorCount()) return;
    auto body = kirn::Parser(toks, diags).parseProgram();
    for (const auto& s : body)
        if (s->kind == kirn::ast::StKind::Import &&
            !s->fromImport && !s->moduleName.empty())
            names.push_back(s->moduleName);
}

std::string cppRawLiteral(const std::string& s) {
    return "R\"KIRN(" + s + ")KIRN\"";
}

// match the runtime the prebuilt kirn_interp.lib was compiled with
// (read CMAKE_BUILD_TYPE from the build tree next to our executable;
//  CMake's default MSVC runtime is /MD release, /MDd debug)
std::string detectRuntimeFlags(const std::string& binRoot) {
    std::string cache;
    if (!readFile(binRoot + "/CMakeCache.txt", cache)) return "/MD /O2";
    std::istringstream in(cache);
    std::string line;
    while (std::getline(in, line))
        if (line.rfind("CMAKE_BUILD_TYPE:", 0) == 0) {
            std::string v = line.substr(line.find('=') + 1);
            for (char& c : v) c = (char)tolower((unsigned char)c);
            if (v.find("debug") != std::string::npos) return "/MDd /Od";
            return "/MD /O2";
        }
    return "/MD /O2";
}

// ---- build options ---------------------------------------------------------
// Output layout is cargo-flavored but Kirn-named:
//   build/<profile>/<target>/...      profile: debug | release
// Targets: <os>-<arch>, os in {windows,linux,darwin}, arch in {amd64,arm64}

struct BuildOpts {
    bool release = false;
    bool wantLib = false;
    bool sasm = false;       // -S  human-readable assembly listing (.sasm)
    bool obj = false;        // -O  native object file (.obj + .lib via lib.exe)
    bool singleFile = false; // `kirn build file.kn` (Go-style, no manifest)
    bool native_ = false;    // --native  lower scalar user fns to real C++
    bool asan = false;       // --asan    build with AddressSanitizer
    std::string target;      // --target=<os>-<arch>; empty -> $COCO_TARGET -> host
    std::string outPath;     // -o <path> (Go build -o)
    std::string defaultOut;  // single-file: extensionless default in CWD
};

std::string hostTarget() {
#if defined(_WIN32)
    std::string os = "windows";
#elif defined(__APPLE__)
    std::string os = "darwin";
#elif defined(__linux__)
    std::string os = "linux";
#else
    std::string os = "unknown";
#endif
#if defined(_M_ARM64) || defined(__aarch64__)
    std::string arch = "arm64";
#else
    std::string arch = "amd64";
#endif
    return os + "-" + arch;
}

// ---- target matrix (Go's GOOS/GOARCH model) ---------------------------------
// Like `go tool dist list`, this table is the source of truth for what can be
// built. Each entry knows its binary suffix and which cross C++ toolchains can
// produce it; users override via COCO_CXX_<TARGET> (e.g. COCO_CXX_LINUX_AMD64).
struct TargetInfo {
    const char* triple;
    const char* exeExt;      // ".exe" or ""
    bool isWindows;
    std::vector<std::string> cxxCandidates;
};

const std::vector<TargetInfo>& targetMatrix() {
    static const std::vector<TargetInfo> kTargets = {
        {"windows-amd64", ".exe", true,
         {"x86_64-w64-mingw32-g++", "x86_64-w64-mingw32-clang++"}},
        {"windows-arm64", ".exe", true,
         {"aarch64-w64-mingw32-g++", "aarch64-w64-mingw32-clang++"}},
        {"linux-amd64", "", false,
         {"x86_64-linux-gnu-g++", "x86_64-linux-gnu-c++",
          "x86_64-linux-musl-g++"}},
        {"linux-arm64", "", false,
         {"aarch64-linux-gnu-g++", "aarch64-linux-gnu-c++",
          "aarch64-linux-musl-g++"}},
        {"darwin-amd64", "", false,
         {"o64-clang++", "x86_64-apple-darwin-clang++"}},
        {"darwin-arm64", "", false,
         {"oa64-clang++", "aarch64-apple-darwin-clang++"}},
    };
    return kTargets;
}

const TargetInfo* findTarget(const std::string& t) {
    for (const auto& ti : targetMatrix())
        if (t == ti.triple) return &ti;
    return nullptr;
}

bool validTarget(const std::string& t) { return findTarget(t) != nullptr; }

bool toolchainWorks(const std::string& cxx) {
    std::string probe =
        "cd . && \"" + cxx + "\" --version >" + shellNullDevice() + " 2>" + shellNullDevice();
    return std::system(probe.c_str()) == 0;
}

// CC_FOR_<GOOS>_<GOARCH>-style override: COCO_CXX_<TRIPLE>, then PATH probing
std::string resolveCrossCxx(const TargetInfo* ti) {
    std::string envName = "COCO_CXX_";
    for (const char* p = ti->triple; *p; ++p)
        envName += (*p == '-') ? '_' : (char)toupper((unsigned char)*p);
    if (const char* env = std::getenv(envName.c_str())) {
        if (toolchainWorks(env)) return env;
        std::cerr << "kirn build: warning: $" << envName << "=" << env
                  << " is not runnable, probing PATH\n";
    }
    if (const char* any = std::getenv("COCO_CXX"))
        if (toolchainWorks(any)) return any;
    for (const auto& cand : ti->cxxCandidates)
        if (toolchainWorks(cand)) return cand;
    return "";
}

// Find the MSVC cl.exe the host build uses (COCO_CL -> hardcoded toolchain ->
// PATH `where cl`). Empty string means no MSVC toolchain is reachable.
std::string resolveHostCl() {
    if (const char* env = std::getenv("COCO_CL"))
        if (*env) return env;
    const std::string hard =
        "C:/msvc/VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64/cl.exe";
    if (toolchainWorks(hard)) return hard;
#ifdef _WIN32
    // PATH probe: `where cl` returns zero if found; on POSIX `nul` is not the
    // null device, so probing would litter a stray `nul` file and can never
    // locate cl.exe anyway.
    if (std::system("where cl >nul 2>nul") == 0) return "cl";
#endif
    return "";
}

// every runtime source needed to build a self-contained launcher from scratch
std::vector<std::string> collectRuntimeSources(const std::string& srcRoot) {
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(srcRoot, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec) &&
            it->path().extension() == ".cpp")
            out.push_back(it->path().generic_string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

// resolve module sources transitively from the entry file
bool gatherEmbedded(const std::string& entry, const std::string& mainSrc,
                    std::map<std::string, std::string>& embedded,
                    const std::vector<std::string>& extraDirs = {}) {
    std::vector<std::string> dirs = extraDirs;
    dirs.push_back("code");                 // sibling modules of the entry
    dirs.push_back("coco_libs/libs");
    dirs.push_back("coco_libs");
    if (!globalPkgDir().empty()) {
        dirs.push_back(globalPkgDir() + "/libs");
        dirs.push_back(globalPkgDir());
    }
    dirs.push_back("../stdlib");
    dirs.push_back("../../stdlib");

    std::set<std::string> seen;
    std::vector<std::pair<std::string, std::string>> queue;
    {
        std::vector<std::string> names;
        collectImports(entry, mainSrc, names);
        for (const auto& n : names) queue.push_back({n, ""});
    }
    while (!queue.empty()) {
        auto [nameRaw, _] = queue.back();
        queue.pop_back();
        // normalize exactly like Interpreter::loadModuleFile: drop an
        // explicit ".kn" suffix FIRST, then '.'/'/' -> '/'
        std::string name = nameRaw;
        if (name.size() > 3 && name.compare(name.size() - 3, 3, ".kn") == 0)
            name.erase(name.size() - 3);
        std::string key;
        for (char c : name) key += (c == '.' || c == '/') ? '/' : c;
        if (seen.count(key)) continue;
        seen.insert(key);
        std::string p, s;
        if (!resolveSource(name, dirs, p, s)) continue;   // native stub etc.
        embedded[key] = s;
        std::vector<std::string> names;
        collectImports(p, s, names);
        for (const auto& n : names) queue.push_back({n, ""});
    }
    return true;
}

// ---- -S: human-readable pseudo-assembly listing -----------------------------
// KIRN-SASM v1: a linear, labeled listing derived from the AST. Great for
// reading what your program does; not (yet) an executable ISA.

struct SasmEmitter {
    std::ostringstream out;
    int nextLabel = 0;
    std::vector<std::pair<std::string, std::string>> loopLabels;   // brk, cont

    // compact printable form of a Type node
    static std::string tyName(const kirn::ast::Type* t) {
        if (!t) return "<type>";
        switch (t->kind) {
            case kirn::ast::TyKind::Name:
                return t->name +
                       (t->generics.empty()
                            ? ""
                            : "[" + std::to_string(t->generics.size()) + "]");
            case kirn::ast::TyKind::Pointer:
                return "*" + tyName(t->inner.get());
            case kirn::ast::TyKind::Ref:
                return (t->refMut ? "&mut " : "&") + tyName(t->inner.get());
            case kirn::ast::TyKind::Optional:
                return tyName(t->inner.get()) + "?";
            case kirn::ast::TyKind::Fn:
                return "fn[" + std::to_string(t->params.size()) + " args]";
            case kirn::ast::TyKind::Tuple:
                return "(" + std::to_string(t->params.size()) + ")";
        }
        return "<type>";
    }

    std::string nl(const std::string& s) { return "    " + s + "\n"; }
    std::string quote(const std::string& s) {
        return kirn::tomlmini::quote(s);
    }
    void expr(const kirn::ast::Expr& e) {
        using ET = kirn::ast::ExKind;
        switch (e.kind) {
            case ET::Int: out << nl("PUSH_INT " + e.text); break;
            case ET::Float: out << nl("PUSH_FLOAT " + e.text); break;
            case ET::Str: out << nl("PUSH_STR " + quote(e.text)); break;
            case ET::CharLit: out << nl("PUSH_CHAR " + quote(e.text)); break;
            case ET::Ident: out << nl("LOAD " + e.text); break;
            case ET::Unary:
                expr(*e.rhs);
                out << nl("UNOP " + e.op);
                break;
            case ET::Binary:
                expr(*e.lhs);
                expr(*e.rhs);
                out << nl("BINOP " + e.op);
                break;
            case ET::Call: {
                for (const auto& a : e.args) expr(*a.value);
                if (e.lhs && e.lhs->kind == ET::Ident)
                    out << nl("CALL " + e.lhs->text + " " +
                              std::to_string(e.args.size()));
                else {
                    expr(*e.lhs);
                    out << nl("CALL_INDIRECT " +
                              std::to_string(e.args.size()));
                }
                break;
            }
            case ET::Index:
                expr(*e.lhs);
                expr(*e.rhs);
                out << nl("INDEX_GET");
                break;
            case ET::Slice:
                expr(*e.lhs);
                if (e.cond) expr(*e.cond);   // step slot
                out << nl("SLICE_GET");
                break;
            case ET::Member:
                expr(*e.lhs);
                out << nl("MEMBER ." + e.text +
                          (e.nilSafe ? " (nil-safe)" : ""));
                break;
            case ET::Try:
                expr(*e.lhs);
                out << nl("TRY_PROPAGATE");
                break;
            case ET::Cond: {   // c ? a : b
                int lElse = nextLabel++, lEnd = nextLabel++;
                expr(*e.cond);
                out << nl("JMP_FALSE L" + std::to_string(lElse));
                expr(*e.lhs);
                out << nl("JMP L" + std::to_string(lEnd));
                out << "L" + std::to_string(lElse) + ":\n";
                expr(*e.rhs);
                out << "L" + std::to_string(lEnd) + ":\n";
                break;
            }
            case ET::Lambda: {
                out << nl("CLOSURE (" + std::to_string(
                                          e.lambdaParams.size()) +
                          " params)");
                break;
            }
            case ET::List:
            case ET::Set:
            case ET::Tuple:
                for (const auto& el : e.elems) expr(*el);
                out << nl(e.kind == ET::List ? "PACK_LIST "
                           : e.kind == ET::Set ? "PACK_SET "
                                               : "PACK_TUPLE ") <<
                    "[" << std::to_string(e.elems.size()) << "]";
                out << "\n";
                break;
            case ET::Dict:
                for (const auto& [k, v] : e.pairs) {
                    expr(*k);
                    expr(*v);
                }
                out << nl("PACK_DICT [" + std::to_string(e.pairs.size()) +
                          "]");
                break;
            case ET::FString:
                for (const auto& p : e.parts) {
                    if (p.isExpr && p.expr)
                        expr(*p.expr);
                    else
                        out << nl("PUSH_STR " + quote(p.text));
                }
                out << nl("FSTRING_BUILD [" +
                          std::to_string(e.parts.size()) + "]");
                break;
            case ET::ListComp:
            case ET::Generator:
                out << nl("; comprehension elided");
                break;
            case ET::New:
                for (const auto& a : e.args) expr(*a.value);
                out << nl("NEW " + tyName(e.newType.get()));
                break;
            case ET::Cast:
                expr(*e.lhs);
                out << nl("CAST " + tyName(e.newType.get()));
                break;
        }
    }

    void stmts(const std::vector<kirn::ast::StmtP>& body, int depth);

    void stmt(const kirn::ast::Stmt& s, int depth) {
        using ST = kirn::ast::StKind;
        std::string ind(depth * 2, ' ');
        switch (s.kind) {
            case ST::FuncDef: {
                out << ind << ".func " << s.name << " argc="
                    << std::to_string(s.params.size()) << "\n";
                stmts(s.body, depth + 1);
                out << ind << ".endfunc\n";
                break;
            }
            case ST::StructDef:
                out << ind << "; struct " << s.name << "\n";
                break;
            case ST::EnumDef:
                out << ind << "; enum " << s.name << "\n";
                break;
            case ST::TraitDef:
                out << ind << "; trait " << s.name << "\n";
                break;
            case ST::ImplDef:
                out << ind << "; impl " << s.name << "\n";
                stmts(s.body, depth + 1);
                break;
            case ST::ConstDecl:
            case ST::VarDecl:
                if (s.value) expr(*s.value);
                out << nl(std::string(s.kind == ST::VarDecl ? "STORE_MUT "
                                                            : "STORE") +
                          (s.target && s.target->kind == kirn::ast::ExKind::Ident
                               ? s.target->text
                               : "<pat>"));
                break;
            case ST::ExprStmt:
                for (const auto& x : s.exprs) {
                    expr(*x);
                    out << nl("POP_DISCARD");
                }
                break;
            case ST::Assign: {
                size_t nT = s.exprs.size() / 2;
                for (size_t i = 0; i < nT; ++i) {
                    const auto& val = s.exprs[nT + i];
                    const auto& tgt = s.exprs[i];
                    if (val) expr(*val);
                    if (tgt && tgt->kind == kirn::ast::ExKind::Ident)
                        out << nl("STORE " + tgt->text);
                    else if (tgt && tgt->kind == kirn::ast::ExKind::Index) {
                        expr(*tgt->lhs);
                        expr(*tgt->rhs);
                        out << nl("INDEX_SET");
                    } else if (tgt)
                        out << nl("STORE <complex>");
                }
                break;
            }
            case ST::AugAssign: {
                if (!s.exprs.empty()) {
                    const auto& tgt = s.exprs[0];
                    if (tgt && tgt->kind == kirn::ast::ExKind::Ident) {
                        out << nl("LOAD " + tgt->text);
                        if (s.exprs.size() > 1) expr(*s.exprs.back());
                        out << nl("BINOP " + s.augOp);
                        out << nl("STORE " + tgt->text);
                    }
                }
                break;
            }
            case ST::Return:
                if (!s.exprs.empty()) expr(*s.exprs[0]);
                out << nl("RET");
                break;
            case ST::Raise:
                if (!s.exprs.empty()) expr(*s.exprs[0]);
                out << nl("RAISE");
                break;
            case ST::Break:
                if (!loopLabels.empty())
                    out << nl("JMP " + loopLabels.back().first);
                break;
            case ST::Continue:
                if (!loopLabels.empty())
                    out << nl("JMP " + loopLabels.back().second);
                break;
            case ST::Defer:
                out << ind << "; defer\n";
                if (!s.exprs.empty()) expr(*s.exprs[0]);
                break;
            case ST::If: {
                int lElse = nextLabel++, lEnd = nextLabel++;
                if (!s.exprs.empty()) expr(*s.exprs[0]);   // condition
                out << nl("JMP_FALSE L" + std::to_string(lElse));
                stmts(s.body, depth);
                out << nl("JMP L" + std::to_string(lEnd));
                out << ind << "L" + std::to_string(lElse) + ":\n";
                // elif chains are nested If statements in elseBody
                if (!s.elseBody.empty()) stmts(s.elseBody, depth);
                out << ind << "L" + std::to_string(lEnd) + ":\n";
                break;
            }
            case ST::While: {
                int lTop = nextLabel++, lEnd = nextLabel++;
                const std::string brk = "L" + std::to_string(lEnd),
                                  cont = "L" + std::to_string(lTop);
                out << ind << "L" + std::to_string(lTop) + ":\n";
                if (!s.exprs.empty()) expr(*s.exprs[0]);   // condition
                out << nl("JMP_FALSE " + brk);
                loopLabels.push_back({brk, cont});
                stmts(s.body, depth);
                loopLabels.pop_back();
                out << nl("JMP " + cont);
                out << ind << brk + ":\n";
                break;
            }
            case ST::For: {
                int lTop = nextLabel++, lEnd = nextLabel++;
                const std::string brk = "L" + std::to_string(lEnd),
                                  cont = "L" + std::to_string(lTop);
                if (!s.exprs.empty()) expr(*s.exprs[0]);   // iterable
                out << nl("ITER_NEW");
                out << ind << "L" + std::to_string(lTop) + ":\n";
                out << nl("ITER_NEXT");
                out << nl("JMP_FALSE " + brk);
                out << nl("BIND_ITER_ITEM");
                loopLabels.push_back({brk, cont});
                stmts(s.body, depth);
                loopLabels.pop_back();
                out << nl("JMP " + cont);
                out << ind << brk + ":\n";
                break;
            }
            case ST::Match: {
                out << ind << "; match arms: " +
                           std::to_string(s.arms.size()) + "\n";
                for (const auto& arm : s.arms) {
                    out << ind << ".arm\n";
                    if (arm.guard) expr(*arm.guard);
                    stmts(arm.body, depth + 1);
                }
                break;
            }
            case ST::Select:
                out << ind << "; select (" + std::to_string(s.selArms.size()) +
                           " arms)\n";
                break;
            case ST::Unsafe:
                stmts(s.body, depth);
                break;
            case ST::Spawn:
                if (!s.exprs.empty()) expr(*s.exprs[0]);
                out << nl("SPAWN");
                break;
            case ST::Import:
                out << ind << "; import " << s.moduleName << "\n";
                break;
            case ST::Export:
                out << ind << "; export\n";
                break;
            case ST::Pass:
                out << nl("NOP");
                break;
        }
    }
};

void SasmEmitter::stmts(const std::vector<kirn::ast::StmtP>& body, int depth) {
    for (const auto& st : body)
        if (st) stmt(*st, depth);
}

std::string emitSasm(const std::string& srcPath,
                     const std::vector<kirn::ast::StmtP>& body) {
    SasmEmitter em;
    em.out << "; KIRN-SASM v1 - pseudo-assembly listing (generated by `kirn"
              " build -S`)\n"
           << "; source: " << srcPath << "\n"
           << "; this is a readable IR dump, not machine code\n\n";
    em.stmts(body, 0);
    return em.out.str();
}

// Core build pipeline shared by project mode (coco.toml) and single-file
// mode (`kirn build main.kn`): type-check, then emit sasm / self-contained
// launcher and compile it with the best available pipeline.
int buildProgram(const std::string& name, const std::string& version,
                 const std::string& entry, const std::string& mainSrc,
                 const std::map<std::string, std::string>& embedded,
                 BuildOpts& opts) {
    const std::string profile = opts.release ? "release" : "debug";
    const fs::path outDir = fs::path("build") / profile / opts.target;
    std::error_code ec;
    fs::create_directories(outDir, ec);

    // Go-style decision point: a cross GNU toolchain produces a real native
    // static binary for non-host targets; without one the build hard-fails
    // (the old portable .cob bundle fallback was removed).
    const bool isHost = opts.target == hostTarget();
    const TargetInfo* ti = findTarget(opts.target);
    std::string crossCxx = ti ? resolveCrossCxx(ti) : "";
    // -O objects need the MSVC host toolchain; keep that path reachable.
    if (!isHost && opts.obj) crossCxx = "";

    if (opts.sasm) {
        std::vector<kirn::ast::StmtP> parsed;
        {
            kirn::DiagEngine diags;
            frontEnd(entry, mainSrc, diags);
            if (diags.errorCount()) {
                printDiags(entry, diags);
                return 65;
            }
            auto toks = kirn::Lexer(mainSrc, entry, diags).lexAll();
            if (!diags.errorCount())
                parsed = kirn::Parser(toks, diags).parseProgram();
            if (diags.errorCount()) {
                printDiags(entry, diags);
                return 65;
            }
        }
        const std::string out =
            opts.outPath.empty()
                ? (outDir / (name + ".sasm")).generic_string()
                : opts.outPath;
        writeFile(out, emitSasm(entry, parsed));
        std::cout << "wrote assembly listing " << out << "\n";
        return 0;
    }

    if (!isHost && crossCxx.empty()) {
        // No cross toolchain for this target: no portable .cob bundle exists
        // anymore (bytecode bundles were removed), so this is a hard failure.
        std::string envName = "COCO_CXX_";
        for (char c : opts.target)
            envName +=
                (c == '-') ? '_' : (char)toupper((unsigned char)c);
        std::cerr << "kirn build: no cross toolchain for '" << opts.target
                  << "'\n  install e.g. llvm-mingw / aarch64-linux-gnu-g++"
                  << " or set " << envName << "=<path-to-g++>\n";
        return 1;
    }

    // --native: parse+check once at build time, lower scalar user fns to C++
    std::unique_ptr<kirn::sema::Checker> natChk;
    kirn::DiagEngine natDiags;
    std::vector<kirn::ast::StmtP> natBody;
    if (opts.native_) {
        auto toks = kirn::Lexer(mainSrc, entry, natDiags).lexAll();
        natBody = kirn::Parser(toks, natDiags).parseProgram();
        if (!natDiags.errorCount()) {
            natChk = std::make_unique<kirn::sema::Checker>(natDiags);
            natChk->checkModule(natBody);
        }
    }

    // emit launcher .cpp embedding every reachable source
    std::ostringstream o;
    o << "// generated by kirn build - standalone Kirn program\n";
    o << "#define COCO_APP_NAME " << kirn::tomlmini::quote(name) << "\n"
      << "#define COCO_APP_VERSION "
      << kirn::tomlmini::quote(version.empty() ? "0.0.0" : version) << "\n"
      << "#define COCO_APP_TARGET " << kirn::tomlmini::quote(opts.target)
      << "\n\n";
    o << "#include \"interp/runtime.h\"\n"
      << "#include \"lex/lexer.h\"\n"
      << "#include \"parser/parser.h\"\n"
      << "#include \"sema/checker.h\"\n\n"
      << "#include <cstdio>\n#include <cstring>\n#include <fstream>\n"
      << "#include <iostream>\n"
      << "#include <sstream>\n\n";
    o << "static const char* kMainSrc = " << cppRawLiteral(mainSrc) << ";\n";
    auto cIdent = [](const std::string& k) {
        std::string r = "M";
        for (char c : k)
            r += isalnum((unsigned char)c) ? c : '_';
        return r;
    };
    for (const auto& [key, src] : embedded)
        o << "static const char* kMod_" << cIdent(key) << " = "
          << cppRawLiteral(src) << ";\n";
    o << "\nstruct Embed { const char* key; const char* src; };\n"
      << "static const Embed kEmbed[] = {\n";
    if (embedded.empty())
        o << "    {\"\", \"\"},\n";   // MSVC rejects size-0 arrays; sentinel kept
    else
        for (const auto& [key, src] : embedded)
            o << "    {\"" << key << "\", kMod_" << cIdent(key) << "},\n";
    o << "};\n\n";
    // --native: emit lowered C++ scalar function bodies + registration helper
    if (opts.native_ && natChk && !natDiags.errorCount()) {
        std::ostringstream nativeOut;
        auto prog = kirn::backend::emitNative(nativeOut, natBody, *natChk);
        if (prog.any) {
            o << "#define COCO_HAS_NATIVE 1\n";
            o << "// ---- native scalar functions (build-time lowered) ----\n";
            o << nativeOut.str();
            o << "\n";
        }
    }
    o << "int main(int argc, char** argv) {\n"
      << "    if (argc > 1 && (std::strcmp(argv[1], \"--version\") == 0 ||\n"
      << "                     std::strcmp(argv[1], \"-V\") == 0)) {\n"
      << "        std::printf(\"%s v%s (%s)\\n\", COCO_APP_NAME,\n"
      << "                    COCO_APP_VERSION, COCO_APP_TARGET);\n"
      << "        return 0;\n"
      << "    }\n"
      << "    kirn::DiagEngine diags;\n"
<< "    auto toks = kirn::Lexer(kMainSrc, \"main.kn\", diags).lexAll();\n"
       << "    if (diags.errorCount()) { std::cerr << \"embedded source error\\n\"; "
          "return 65; }\n"
       << "    auto body = kirn::Parser(toks, diags).parseProgram();\n"
       << "    if (diags.errorCount()) { return 65; }\n"
       << "    { kirn::sema::Checker chk(diags); chk.checkModule(body); }\n"
       << "    if (diags.errorCount()) { return 65; }\n"
      << "    kirn::ast::Stmt root;\n"
      << "    root.kind = kirn::ast::StKind::Pass;\n"
      << "    root.body = std::move(body);\n"
      << "    try {\n"
      << "        kirn::interp::Interpreter interp(root);\n"
      << "        for (const auto& e : kEmbed) "
         "if (e.key[0]) interp.addEmbeddedSource(e.key, e.src);\n"
      << "        interp.enableVm();   // bytecode VM is the default runner\n"
      << "#ifdef COCO_HAS_NATIVE\n"
      << "        kirn_native_register(interp, root.body);\n"
      << "#endif\n"
      << "        auto r = interp.run();\n"
      << "        return r.k == kirn::interp::VK::Int ? (int)r.i : 0;\n"
      << "    } catch (const kirn::interp::PanicSignal& p) {\n"
      << "        fflush(stdout);\n"
      << "        fputs((\"panic: \" + p.msg + \"\\n\").c_str(), stderr);\n"
      << "        return 70;\n"
      << "    } catch (const kirn::interp::SignalRaise&) {\n"
      << "        fputs(\"panic: uncaught raise\\n\", stderr);\n"
      << "        return 70;\n"
      << "    }\n"
      << "}\n";

    writeFile((outDir / (name + ".cpp")).generic_string(), o.str());

    // ---- Go-style cross build: one self-contained static binary ----------
    // Compiles the generated launcher TOGETHER WITH the whole Kirn runtime
    // (src/**/*.cpp) using the target's C++ toolchain - the analogue of
    // CGO_ENABLED=0: no prebuilt host libs, nothing external.
    // crossCxx is non-empty exactly when we want the GNU pipeline: any
    // target whose toolchain resolves (env override or PATH probe).
    // Cross targets (non-host) use the GNU pipeline: compile the whole runtime
    // with the target's cross C++ toolchain into a self-contained static
    // binary. Host builds skip this and use the fast MSVC pipeline below
    // (prebuilt runtime .libs + cl.exe) - the "go build"-like path that needs
    // no cross/mingw toolchain.
    char exeDirBuf[MAX_PATH * 2];
    GetModuleFileNameA(nullptr, exeDirBuf, sizeof exeDirBuf);
    std::string exePath2(exeDirBuf);
    size_t ds2 = exePath2.find_last_of("/\\");
    std::string binRoot = ds2 == std::string::npos ? "." : exePath2.substr(0, ds2);
    const bool hostHasMsvc = isHost && resolveHostCl() != "" &&
                             fs::exists(fs::path(binRoot) / "kirn_interp.lib");
    // ASan instruments the WHOLE runtime, so it always uses the whole-runtime
    // pipeline (GNU) rather than the fast launcher-only MSVC path (mixing an
    // instrumented launcher with uninstrumented prebuilt libs is not linkable).
    const bool useGnuCross = !isHost || !hostHasMsvc || opts.asan;
    if (useGnuCross && !crossCxx.empty()) {
        char xbuf[MAX_PATH * 2];
        GetModuleFileNameA(nullptr, xbuf, sizeof xbuf);
        std::string xroot(xbuf);
        size_t xs = xroot.find_last_of("/\\");
        std::string binRoot2 = xs == std::string::npos ? "." : xroot.substr(0, xs);
        const std::string srcRoot = (fs::path(binRoot2) / ".." / "src").generic_string();
        auto srcs = collectRuntimeSources(srcRoot);
        if (srcs.empty()) {
            std::cerr << "kirn build: runtime sources not found at "
                      << srcRoot << " (needed for cross builds)\n";
            return 1;
        }
        const std::string outName =
            !opts.outPath.empty()
                ? opts.outPath
                : !opts.defaultOut.empty()
                    ? opts.defaultOut +
                          (ti && ti->isWindows ? ".exe" : "")
                    : (outDir / (name + (ti ? ti->exeExt : ".exe")))
                          .generic_string();
        {
            fs::path op(outName);
            if (op.has_parent_path()) fs::create_directories(op.parent_path());
        }

        // response file keeps the command line short
        const std::string rsp = (outDir / "cross.rsp").generic_string();
        const std::string launcherCpp =
            (outDir / (name + ".cpp")).generic_string();
        {
            std::ostringstream r;
            r << "-std=c++20 -D_CRT_SECURE_NO_WARNINGS ";
            r << (opts.release ? "-O2 -s -w" : "-O1 -g") << " ";
            if (opts.asan) r << "-fsanitize=address -g ";
            if (ti && ti->isWindows)
                r << "-static-libgcc -static-libstdc++ ";
            r << "-I\"" << srcRoot << "\" ";
            r << "-o \"" << fs::absolute(outName).generic_string() << "\" ";
            r << "\"" << fs::absolute(launcherCpp).generic_string() << "\"";
            for (const auto& s : srcs) r << " \"" << s << "\"";
            if (ti && ti->isWindows) r << " -lws2_32";
            writeFile(rsp, r.str());
        }
        std::cout << "compiling with " << crossCxx << " -> "
                  << outName << " (" << srcs.size() + 1
                  << " translation units)\n";
        std::string cmd =
            "cd . && \"" + crossCxx + "\" @\"" +
            fs::absolute(rsp).generic_string() + "\"";
        if (std::getenv("COCO_VERBOSE")) std::cerr << "[cmd] " << cmd << "\n";
        int rc = std::system(cmd.c_str());
        if (rc != 0) {
            std::cerr << "kirn build: cross-compilation failed (" << rc
                      << ")\n";
            return rc == 0 ? 1 : rc;
        }
        std::cout << "built " << outName << "\n";
        return 0;
    }

    // ---- host build via the prebuilt MSVC runtime libs -------------------
    // compile with the same toolchain cmake uses
    std::string clPath = std::getenv("COCO_CL") && *std::getenv("COCO_CL")
                             ? std::getenv("COCO_CL")
                             : resolveHostCl();

    // NOTE: must not start with '"' — cmd /c strips leading quotes.
    // NOTE: no quoted paths ending in '\' (argv would eat the quote).
    const std::string genCpp = (outDir / (name + ".cpp")).generic_string();
    const std::string outBase =
        !opts.outPath.empty()
            ? opts.outPath
            : !opts.defaultOut.empty() ? opts.defaultOut + ".exe"
                                       : (outDir / name).generic_string();
    if (opts.obj) {
        // -O: object file (+ static .lib via lib.exe); host toolchain only
        if (opts.target != hostTarget()) {
            std::cerr << "kirn build -O: native objects for '" << opts.target
                      << "' need that platform's toolchain\n";
            return 1;
        }
        std::string cmd =
            "cd . && \"" + clPath + "\" /nologo /EHsc /c " +
            detectRuntimeFlags(binRoot) + " /std:c++20" + " /I\"" + binRoot +
            "\\..\\src\"" + " /Fo\"" + outBase + ".obj\" " + genCpp;
        if (std::getenv("COCO_VERBOSE")) std::cerr << "[cmd] " << cmd << "\n";
        std::cout << "compiling " << outBase << ".obj ...\n";
        int rc = std::system(cmd.c_str());
        if (rc != 0) {
            std::cerr << "kirn build: compilation failed (" << rc << ")\n";
            return rc == 0 ? 1 : rc;
        }
        const char* libTool = std::getenv("COCO_LIB_TOOL");
        std::string libPath =
            libTool ? libTool
                    : clPath.substr(0, clPath.find_last_of("/\\")) +
                          "/lib.exe";
        std::string arc =
            "cd . && \"" + libPath + "\" /nologo /OUT:" + outBase + ".lib \"" +
            outBase + ".obj\"";
        std::cout << "archiving " << outBase << ".lib ...\n";
        int rc2 = std::system(arc.c_str());
        if (rc2 != 0) {
            std::cerr << "kirn build: lib.exe failed (" << rc2 << ")\n";
            return rc2 == 0 ? 1 : rc2;
        }
        std::cout << "built " << outBase << ".obj + .lib\n";
        return 0;
    }

    std::string flags = detectRuntimeFlags(binRoot);
    if (opts.release) {
        // --release: force -O2 on the emitted C++ even if the prebuilt runtime
        // libs are a debug build (optimized launcher, same lib set). A release
        // cache of the runtime yields the full win; see kirn build --release.
        if (flags.find("/O2") == std::string::npos &&
            flags.find("/Ox") == std::string::npos)
            flags += " /O2";
    }
    std::string exeOut = outBase;
    if (exeOut.size() < 4 ||
        _stricmp(exeOut.c_str() + exeOut.size() - 4, ".exe") != 0)
        exeOut += ".exe";
    {
        fs::path op(exeOut);
        if (op.has_parent_path()) fs::create_directories(op.parent_path());
    }
    std::string cmd =
        "cd . && \"" + clPath + "\" /nologo /EHsc " + flags + " /std:c++20" +
        " /I\"" + binRoot + "\\..\\src\""
        " /Fobuild\\ " + genCpp +
        " /Fe:" + exeOut + " /link /LIBPATH:\"" + binRoot +
        "\" kirn_interp.lib kirn_vm.lib kirn_sema.lib kirn_parser.lib"
        " kirn_ast.lib kirn_lex.lib";
    if (std::getenv("COCO_VERBOSE")) std::cerr << "[cmd] " << cmd << "\n";
    std::cout << "compiling " << exeOut << " ...\n";
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        std::cerr << "kirn build: compilation failed (" << rc << ")\n";
        return rc == 0 ? 1 : rc;
    }
    std::cout << "built " << exeOut << "\n";
    return 0;
}

int packLib(const Manifest& m, const BuildOpts& opts) {
    // type-check every source under code/
    int bad = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it("code", ec), end; !ec && it != end;
         it.increment(ec)) {
        if (ec || !it->is_regular_file(ec) ||
            it->path().extension() != ".kn")
            continue;
        std::string src;
        readFile(it->path().string(), src);
        kirn::DiagEngine diags;
        frontEnd(it->path().string(), src, diags);
        if (diags.errorCount()) {
            printDiags(it->path().string(), diags);
            ++bad;
        }
    }
    if (bad) {
        std::cerr << "kirn build lib: " << bad << " file(s) failed checks\n";
        return 1;
    }
    regenerateDocs(m);

    // pack manifest + code/ + docs/ into a single distributable
    const fs::path outDir =
        fs::path("build") / (opts.release ? "release" : "debug") / opts.target;
    fs::create_directories(outDir, ec);
    const std::string outPath =
        (outDir / (m.name + "-" + m.version + ".cocolib")).generic_string();
    std::ostringstream o;
    o << "COCOLIB/1\n";
    std::vector<fs::path> files;
    files.push_back(fs::path("coco.toml"));
    for (fs::recursive_directory_iterator it("code", ec), end; !ec && it != end;
         it.increment(ec))
        if (!ec && it->is_regular_file(ec)) files.push_back(it->path());
    for (fs::recursive_directory_iterator it("docs", ec), end; !ec && it != end;
         it.increment(ec))
        if (!ec && it->is_regular_file(ec)) files.push_back(it->path());
    if (fs::is_regular_file("README.md")) files.push_back("README.md");
    if (fs::is_regular_file("LICENSE")) files.push_back("LICENSE");
    for (const auto& f : files) {
        std::string content;
        if (!readFile(f.string(), content)) continue;
        o << "@@FILE " << f.generic_string() << "\n" << content << "@@END\n";
    }
    writeFile(outPath, o.str());
    std::cout << "packed " << outPath << " (" << files.size()
              << " files) - ready to publish\n";
    return 0;
}

int buildAppShim(const Manifest& m, BuildOpts& opts) {
    std::string entry = resolveEntry(m, ".");
    if (entry.empty()) {
        std::cerr << "kirn build: no entry point found in this directory\n"
                  << "  looked for (in order): coco.toml [package] main, "
                     "code/main.kn, main.kn, code/pin.kn, pin.kn\n"
                  << "  fix-it: create code/main.kn, or run `kirn new "
                     "<name>`\n";
        return 1;
    }
    std::string mainSrc;
    if (!readFile(entry, mainSrc)) {
        std::cerr << "kirn build: entry '" << entry << "' not found\n";
        return 1;
    }
    std::map<std::string, std::string> embedded;
    gatherEmbedded(entry, mainSrc, embedded);
    return buildProgram(m.name,
                        m.version.empty() ? "0.0.1-beta" : m.version, entry,
                        mainSrc, embedded, opts);
}

int cmdBuild(const std::vector<std::string>& args, size_t from) {
    BuildOpts opts;
    std::string positional;
    for (size_t i = from; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "lib") opts.wantLib = true;
        else if (a == "--release" || a == "-r") opts.release = true;
        else if (a == "--debug") opts.release = false;
        else if (a == "-S" || a == "-s" || a == "--asm") opts.sasm = true;
        else if (a == "-O" || a == "--obj") opts.obj = true;
        else if (a == "--native" || a == "-N") opts.native_ = true;
        else if (a == "--asan") opts.asan = true;
        else if (a.rfind("--target=", 0) == 0)
            opts.target = a.substr(9);
        else if (a == "--target" && i + 1 < args.size())
            opts.target = args[++i];
        else if (a == "-o" && i + 1 < args.size())
            opts.outPath = args[++i];
        else if (a.rfind("--output=", 0) == 0)
            opts.outPath = a.substr(9);
        else if (a.size() && a[0] != '-' && positional.empty())
            positional = a;                     // file.kn | . | <dir>
        else {
            std::cerr << "kirn build: unknown option '" << a << "'\n";
            return 64;
        }
    }
    if (opts.target.empty() && std::getenv("COCO_TARGET"))
        opts.target = std::getenv("COCO_TARGET");   // GOOS/GOARCH analogue
    if (opts.target.empty()) opts.target = hostTarget();
    if (!validTarget(opts.target)) {
        std::cerr << "kirn build: unknown target '" << opts.target
                  << "' (like 'unknown GOOS' in Go)\n"
                  << "run 'kirn targets' for the supported matrix\n";
        return 64;
    }

    // ---- Go-style single-file mode: kirn build path/to/prog.kn ----------
    // No manifest needed; the binary defaults to ./<stem>.exe in the CWD.
    if (!positional.empty() &&
        positional.rfind(".kn") == positional.size() - 3) {
        fs::path p(positional);
        if (!fs::is_regular_file(p)) {
            std::cerr << "kirn build: file not found: " << positional << "\n";
            return 1;
        }
        p = fs::absolute(p).lexically_normal();
        std::string mainSrc;
        if (!readFile(p.generic_string(), mainSrc)) {
            std::cerr << "kirn build: cannot read '" << positional << "'\n";
            return 1;
        }
        opts.singleFile = true;
        if (opts.outPath.empty())
            opts.defaultOut = p.stem().string();    // ./<stem>[.exe]
        std::map<std::string, std::string> embedded;
        gatherEmbedded(p.generic_string(), mainSrc, embedded,
                       {p.parent_path().generic_string()});
        return buildProgram(p.stem().string(), "0.0.0",
                            p.generic_string(), mainSrc, embedded, opts);
    }
    if (!positional.empty() && positional != ".") {
        std::cerr << "kirn build: '" << positional
                  << "' is neither a .kn file nor a project directory\n";
        return 64;
    }

    Manifest m = readManifest(".");
    if (m.name.empty()) {
        std::cerr << "kirn build: no coco.toml in this directory\n"
                  << "(or compile a single file: kirn build main.kn)\n";
        return 1;
    }
    if (opts.wantLib || m.type == "lib") {
        if (!fs::is_directory("code")) {
            std::cerr << "kirn build lib: no code/ directory\n";
            return 1;
        }
        return packLib(m, opts);
    }
    return buildAppShim(m, opts);
}

int unpackCocolib(const std::string& raw, bool global_) {
    std::string text;
    if (!readFile(raw, text)) {
        std::cerr << "kirn install: cannot read bundle '" << raw << "'\n";
        return 1;
    }
    std::istringstream in(text);
    std::string header;
    std::getline(in, header);
    if (header.rfind("COCOLIB/", 0) != 0) {
        std::cerr << "kirn install: not a valid .cocolib bundle\n";
        return 1;
    }

    // collect sections first so we can learn the package name
    struct Section { std::string path, body; };
    std::vector<Section> sections;
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("@@FILE ", 0) == 0) {
            sections.push_back(Section{line.substr(7), ""});
        } else if (line == "@@END") {
            // section closed
        } else if (!sections.empty()) {
            sections.back().body += line + "\n";
        }
    }
    std::string name;
    for (const auto& s : sections)
        if (s.path == "coco.toml") {
            Doc d = kirn::tomlmini::parse(s.body);
            name = kirn::tomlmini::get(d, "package", "name");
        }
    if (name.empty()) {
        // greet-0.1.0.cocolib -> greet
        std::string stem = fs::path(raw).stem().string();
        size_t dash = stem.find('-');
        name = dash == std::string::npos ? stem : stem.substr(0, dash);
    }
    if (name.empty()) {
        std::cerr << "kirn install: cannot determine package name\n";
        return 1;
    }

    fs::path base = pkgLibRoot(global_);
    for (const auto& s : sections)
        writeFile(base / name / fs::path(s.path).generic_string(),
                  s.body);
    std::cout << "installed " << raw << " -> "
              << (base / name / "").string() << " (" << sections.size()
              << " files)\n";
    return 0;
}

// ---------------------------------------------------------------------------

// kirn targets - the `go tool dist list` analogue: every supported
// GOOS/GOARCH-style triple, marked with what this machine can produce.
int cmdTargets() {
    const std::string host = hostTarget();
    std::cout << "TARGET" << std::string(14, ' ') << "BINARY   TOOLCHAIN\n";
    for (const auto& ti : targetMatrix()) {
        std::string t = ti.triple;
        std::cout << t << std::string(20 - t.size(), ' ')
                  << (ti.isWindows ? "<name>.exe" : "<name>")
                  << std::string(7, ' ');
        if (t == host) {
            std::string cxx = resolveCrossCxx(&ti);
            if (!cxx.empty())
                std::cout << cxx << "\n";
            else
                std::cout << "(host - prebuilt MSVC libs)\n";
        } else {
            std::string cxx = resolveCrossCxx(&ti);
            if (!cxx.empty())
                std::cout << cxx << "\n";
            else {
                std::string envName = "COCO_CXX_";
                for (const char* p = ti.triple; *p; ++p)
                    envName +=
                        (*p == '-') ? '_' : (char)toupper((unsigned char)*p);
                std::cout << "- no toolchain"
                          << " (set " << envName << "=<g++> for native)\n";
            }
        }
    }
    return 0;
}

void usage() {
    std::cout
        << "kirn - the Kirn language driver\n\n"
        << "usage:\n"
        << "  kirn run [dir|file]              run a program or project\n"
        << "  kirn run <file.kn>              run a script or project\n"
        << "  kirn new <name>                  scaffold an application\n"
        << "  kirn new lib <name>              scaffold a library package\n"
        << "  kirn test [.|file|dir ...]       run *_test.kn files\n"
        << "  kirn install|i [-g] <pkg>        install into ./coco_libs/libs\n"
        << "      pkg := [github.com/]user/repo[@tag] | <path> | <registry"
               "-name> | file.cocolib\n"
        << "      -g installs globally into ~/.coco/kirn-pkg/libs\n"
        << "         (apps also get a bin shim + PATH entry)\n"
        << "  kirn add <pkg>...                install + record dependencies"
               "\n"
        << "  kirn add                         sync: install missing deps"
               " (go mod tidy)\n"
        << "  kirn update [name]               refresh installed deps\n"
        << "  kirn remove <name>               uninstall a dependency\n"
        << "  kirn clone <repo> [--full]       clone any repo (shorthand:"
               " user/repo)\n"
        << "  kirn build                       compile project (needs "
               "coco.toml)\n"
        << "  kirn build <file.kn>             compile one file -> ./<stem>"
               ".exe\n"
        << "           [--release|--debug]     optimization profile\n"
        << "           [--target=<os>-<arch>]    like GOOS/GOARCH; default "
               "$COCO_TARGET or host\n"
         << "           [-S|-O]                    -S asm listing, -O .obj+."
               "lib\n"
         << "           [--native]                 lower scalar user fns to "
               "real C++\n"
         << "           [--asan]                   build with AddressSanitizer\n"
         << "           [-o <path>]               output path (Go build -o)\n"
        << "  kirn targets                     list all supported target "
               "triples\n"
        << "  kirn build lib                   check + pack -> "
               "build/<profile>/<t>/<n>-<v>.cocolib\n"
        << "  kirn doc <lib|dir> [--port N]    serve markdown docs + API ref\n"
        << "  kirn list                        list installed libraries\n"
        << "  kirn list online                 browse the kirn-libs registry\n";
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + (argc > 0 ? 1 : 0), argv + argc);
    if (args.empty()) {
        usage();
        return 64;
    }
    const std::string& cmd = args[0];
    auto rest = [&](size_t i) {
        return i < args.size() ? args[i] : std::string();
    };

    if ((cmd == "run" || cmd == "r") && args.size() >= 2) {
        std::string target = args[1];
        std::vector<std::string> progArgs(args.begin() + 2, args.end());
        fs::path file(target);
        if (fs::is_directory(file)) {
            Manifest m = readManifest(file);
            std::string entry = resolveEntry(m, file);
            if (entry.empty()) {
                std::cerr
                    << "kirn run: no entry point found in '" << target
                    << "'\n"
                    << "  looked for (in order): coco.toml [package] main, "
                       "code/main.kn, main.kn, code/pin.kn, pin.kn\n"
                    << "  fix-it: create code/main.kn, or run `kirn new <name>` "
                       "to scaffold a project\n";
                return 66;
            }
            file /= entry;
        }
        // project root is cwd (or the dir containing the entry)
        std::string scriptDir = fs::is_directory(target)
                                    ? target
                                    : file.parent_path().string();
        auto dirs = libDirsFor(scriptDir);
        // sibling modules of the entry file (code/) are importable too:
        // `import "util.kn"` or `import util` inside code/main.kn
        {
            std::string parent = file.parent_path().string();
            if (!parent.empty() && parent != "." && parent != scriptDir)
                dirs.insert(dirs.begin(), parent);
        }
        return runProgram(file.string(), dirs, {}, progArgs);
    }
    if (cmd == "new" && args.size() >= 2) {
        bool lib = args[1] == "lib";
        size_t nameIdx = lib ? 2 : 1;
        if ((lib && args.size() != 3) || (!lib && args.size() != 2)) {
            usage();
            return 64;
        }
        return cmdNew(rest(nameIdx), lib);
    }
    if (cmd == "test" && args.size() >= 1) return cmdTest(args, 1);
    if (cmd == "install" || cmd == "i") {
        bool global_ = false;
        std::string pkg;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "-g" || args[i] == "--global") global_ = true;
            else pkg = args[i];
        }
        if (pkg.empty()) {
            usage();
            return 64;
        }
        if (fs::is_regular_file(pkg) &&
            fs::path(pkg).extension() == ".cocolib")
            return unpackCocolib(pkg, global_);
        return cmdInstall(pkg, global_);
    }
    if (cmd == "update" && args.size() <= 2)
        return cmdUpdate(args.size() == 2 ? args[1] : "");
    if (cmd == "remove" && args.size() >= 2)
        return cmdRemove(args[1]);
    if (cmd == "add") {   // npm-install style sync; no args = tidy
        std::vector<std::string> pkgs(args.begin() + 1, args.end());
        return cmdAdd(pkgs);
    }
    if (cmd == "clone" && (args.size() == 2 ||
                           (args.size() == 3 && args[2] == "--full")))
        return cmdClone(args[1], args.size() == 3);
    if (cmd == "list") {
        if (args.size() == 1) return cmdList();
        if (args.size() == 2 && args[1] == "online") return cmdListOnline();
    }
    if (cmd == "targets" || cmd == "dist")
        return cmdTargets();
    if (cmd == "build") return cmdBuild(args, 1);
    if (cmd == "doc" && (args.size() == 2 ||
                         (args.size() == 4 && args[2] == "--port"))) {
        int port = args.size() == 4 ? atoi(args[3].c_str()) : 8080;
        return cmdDoc(args[1], port);
    }
    usage();
    return cmd == "help" || cmd == "--help" || cmd == "-h" ? 0 : 64;
}
