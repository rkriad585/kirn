// kirn — driver CLI for the Kirn language.
//
//   kirn new <name>              scaffold an application project
//   kirn new pet <name>           scaffold a package (pet)
//   kirn run [dir|file]          run a program or project
//   kirn test [dir|file...]      run *_test.kn files
//   kirn install|i [-g] <pkg>    install into ./pets or ~/.kirn/pets
//   kirn update [name]           refresh installed dependencies
//   kirn remove <name>           uninstall a dependency
//   kirn build                   compile project -> standalone build/<name>
//   kirn build pet               check + pack pet -> build/<n>-<v>.pet
//   kirn doc <lib|dir> [--port]  generate API docs + serve markdown viewer
//   kirn list                    show installed libraries
//
// Projects are Rust-style but with Kirn's own folder names:
//   kirn.toml | code/ | tests/ | docs/ | pets/ | build/
// Packages installed globally live in ~/.kirn/pets (app binaries in
// ~/.kirn/apps/bin).
//
// Phase 0 of BUILDER-PLAN.md moved the driver core into the kirn_driver
// library (src/driver/): the front end, module resolution, bundling, target
// handling, and the whole build pipeline. What remains here is the user-facing
// CLI layer — package management, scaffolding, `test`, the docs server, and
// main(). The future `kirnc` driver will link the same library, so both tools
// share one implementation of the pipeline rather than two that drift.

#include "driver/driver.h"
#include "ast/ast.h"
#include "support/diag.h"

#include "util/tomlmini.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
// ws2_32 is needed only by serveDocs() below, so it stays linked here instead
// of propagating into every consumer of the driver library.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>   // GetEnvironmentVariableA (PATH auto-setup)
#else
// Minimal POSIX shims for the handful of Win32 calls used below (PATH
// auto-setup and app-shim install). Keeps the CLI buildable on Linux dev
// machines; the Windows-only behaviour (registry PATH, .cmd shims) is still
// gated to _WIN32 at the call sites that matter.
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
#endif

namespace fs = std::filesystem;

using kirn::tomlmini::Doc;

using kirn::driver::Deps;
using kirn::driver::Manifest;
using kirn::driver::appsBinDir;
using kirn::driver::bundleBinary;
using kirn::driver::bundleTail;
using kirn::driver::buildAppShim;
using kirn::driver::cmdBuild;
using kirn::driver::cmdCheck;
using kirn::driver::cmdTargets;
using kirn::driver::collectImports;
using kirn::driver::ensureRunner;
using kirn::driver::gatherEmbedded;
using kirn::driver::globalAppsBinDir;
using kirn::driver::globalPkgDir;
using kirn::driver::hostExeSuffix;
using kirn::driver::hostTarget;
using kirn::driver::lastSegment;
using kirn::driver::libDirsFor;
using kirn::driver::pkgBase;
using kirn::driver::readFile;
using kirn::driver::readManifest;
using kirn::driver::regenerateDocs;
using kirn::driver::resolveEntry;
using kirn::driver::runProgram;
using kirn::driver::runProgramSrc;
using kirn::driver::selfExeDir;
using kirn::driver::shellNullDevice;
using kirn::driver::trimSlashes;
using kirn::driver::unpackPet;
using kirn::driver::usage;
using kirn::driver::warnLegacyEnv;
using kirn::driver::writeFile;
using kirn::driver::writeManifest;

namespace {

using kirn::tomlmini::Doc;

// A tiny dependency-free SHA-256 writer for published artifacts. Keeping the
// digest in the driver makes the sidecar format identical on POSIX and
// Windows; shelling out to certutil produced a different, human-readable
// format from sha256sum and made verification platform-dependent.
class Sha256 {
public:
    void update(const unsigned char* data, size_t len) {
        for (size_t i = 0; i < len; ++i) {
            block_[used_++] = data[i];
            if (used_ == sizeof block_) {
                transform();
                total_ += sizeof block_;
                used_ = 0;
            }
        }
    }

    std::string finish() {
        const uint64_t bits = (total_ + used_) * 8;
        block_[used_++] = 0x80;
        if (used_ > 56) {
            while (used_ < sizeof block_) block_[used_++] = 0;
            transform();
            used_ = 0;
        }
        while (used_ < 56) block_[used_++] = 0;
        for (int i = 7; i >= 0; --i) block_[used_++] =
            static_cast<unsigned char>(bits >> (i * 8));
        transform();

        static constexpr char hex[] = "0123456789abcdef";
        std::string out;
        out.reserve(64);
        for (uint32_t word : state_)
            for (int i = 7; i >= 0; --i)
                out.push_back(hex[(word >> (i * 4)) & 0xf]);
        return out;
    }

private:
    static uint32_t rotr(uint32_t x, unsigned n) {
        return (x >> n) | (x << (32 - n));
    }

    void transform() {
        static constexpr uint32_t k[] = {
            0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,
            0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
            0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
            0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
            0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,
            0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
            0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,
            0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
            0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
            0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
            0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,
            0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
            0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,
            0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
            0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
            0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u,
        };
        uint32_t w[64]{};
        for (int i = 0; i < 16; ++i)
            for (int j = 0; j < 4; ++j)
                w[i] = (w[i] << 8) | block_[i * 4 + j];
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^
                                 (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^
                                 (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a=state_[0], b=state_[1], c=state_[2], d=state_[3];
        uint32_t e=state_[4], f=state_[5], g=state_[6], h=state_[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = h + S1 + ch + k[i] + w[i];
            const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = S0 + maj;
            h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        state_[0]+=a; state_[1]+=b; state_[2]+=c; state_[3]+=d;
        state_[4]+=e; state_[5]+=f; state_[6]+=g; state_[7]+=h;
    }

    uint32_t state_[8] = {0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
                          0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
    unsigned char block_[64]{};
    size_t used_ = 0;
    uint64_t total_ = 0;
};

bool writeSha256Sidecar(const fs::path& artifact, const fs::path& sidecar) {
    std::ifstream in(artifact, std::ios::binary);
    if (!in) return false;
    Sha256 sha;
    char buf[64 * 1024];
    while (in) {
        in.read(buf, sizeof buf);
        const std::streamsize got = in.gcount();
        if (got > 0)
            sha.update(reinterpret_cast<const unsigned char*>(buf),
                       static_cast<size_t>(got));
    }
    if (!in.eof()) return false;
    std::ofstream out(sidecar, std::ios::trunc);
    if (!out) return false;
    out << sha.finish() << "  " << artifact.filename().string() << "\n";
    return static_cast<bool>(out);
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

std::string kirnCacheDir() {
    if (const char* v = std::getenv("KIRNCACHE"); v && *v) return v;
    const char* home = std::getenv("USERPROFILE");
    if (!home) home = std::getenv("HOME");
    return home ? std::string(home) + "/.kirn/cache" : ".kirn/cache";
}

int cmdEnv() {
    auto show = [](const char* name, const std::string& fallback) {
        const char* value = std::getenv(name);
        std::cout << name << "=" << (value && *value ? value : fallback)
                  << "\n";
    };
    show("KIRNCACHE", kirnCacheDir());
    show("KIRN_PETS", "");
    show("KIRN_STDLIB", "");
    show("KIRN_CXX", "");
    show("KIRN_TARGET", "host");
    return 0;
}

int cmdPublish() {
    Manifest m = readManifest(".");
    if (m.name.empty()) {
        std::cerr << "kirn publish: no kirn.toml found\n";
        return 1;
    }
    std::vector<std::string> buildArgs{"build", "pet"};
    int rc = cmdBuild(buildArgs, 1);
    if (rc != 0) return rc;

    fs::path artifact;
    std::error_code ec;
    for (fs::recursive_directory_iterator it("build", ec), end;
         !ec && it != end; it.increment(ec)) {
        if (ec || !it->is_regular_file(ec) || it->path().extension() != ".pet")
            continue;
        if (it->path().filename().string().rfind(m.name + "-", 0) == 0) {
            artifact = it->path();
            break;
        }
    }
    if (artifact.empty()) {
        std::cerr << "kirn publish: build produced no .pet artifact\n";
        return 1;
    }

    fs::path registry = kirnCacheDir();
    fs::create_directories(registry, ec);
    if (ec) {
        std::cerr << "kirn publish: cannot create cache: " << ec.message() << "\n";
        return 1;
    }
    fs::path cached = registry / artifact.filename();
    fs::copy_file(artifact, cached, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::cerr << "kirn publish: cannot cache artifact: " << ec.message() << "\n";
        return 1;
    }

    // Keep a canonical checksum sidecar beside every published artifact.
    fs::path sum = cached;
    sum += ".sha256";
    if (!writeSha256Sidecar(cached, sum)) {
        std::cerr << "kirn publish: cannot write SHA-256 sidecar\n";
        return 1;
    }
    std::cout << "published " << cached << "\n"
              << "checksum " << sum << "\n";
    return 0;
}

// ---------------------------------------------------------------------------
// manifests: kirn.toml
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// lockfile: kirn.lock — pins exactly what is installed where
// ---------------------------------------------------------------------------

struct LockEntry {
    std::string name, source, url, tag, commit, version, installed;
};

std::vector<LockEntry> readLock(const fs::path& dir) {
    std::vector<LockEntry> out;
    std::string text;
    if (!readFile((dir / "kirn.lock").string(), text)) {
        if (fs::is_regular_file(dir / "kirn.lock"))
            std::cerr << "kirn: ignoring stale 'kirn.lock' (renamed to "
                         "'kirn.lock'); delete it and re-run:  rm kirn.lock\n";
        return out;
    }
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
    o << "# kirn.lock - generated by `kirn install/update`. Commit this file.\n";
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
    writeFile(dir / "kirn.lock", o.str());
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
//   kirn.toml [package] main -> code/main.kn -> main.kn -> code/pin.kn -> pin.kn
// Returns the relative path to the chosen entry, or "" when none exists.

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
    m.type = lib ? "pet" : "app";
    m.description = lib ? "A Kirn pet" : "A Kirn application";
    m.license = "MIT";
    m.docs = "docs/index.md";
    m.readme = "README.md";
    m.author = "Your Name <you@example.com>";
    m.repo = "github.com/pets-registry/" + name;
    m.homepage = "https://github.com/pets-registry/" + name;
    m.authors.push_back("Your Name <you@example.com>");
    if (lib) {
        m.tags.push_back("utility");
        m.keywords.push_back("kirn");
        m.keywords.push_back(name);
    }
    m.gitIgnoreExtra.push_back(".pets-registry.toml");
    m.gitIgnoreExtra.push_back(".pets-sha");

    const std::string gitignore =
        "# kirn build output\n"
        "build/\n"
        "# installed dependencies\n"
        "pets/\n"
        "# cached registry copy + temp git sha during install/update\n"
        ".pets-registry.toml\n"
        ".pets-sha\n"
        "# editor/OS noise\n"
        ".vscode/\n.idea/\n*.swp\nThumbs.db\n.DS_Store\n";

    if (lib) {
        m.main = "code/pin.kn";
        writeManifest(root, m);

        // pin.kn is the package initializer + public-API aggregator. It runs
        // once when the package is imported and re-exports the package's
        // `pub` surface (Python __init__ analogue).
        writeFile(root / "code" / "pin.kn",
                  "## " + name + " - a Kirn pet.\n"
                  "##\n"
                  "## This pin.kn file is the package's public-API aggregator.\n"
                  "## It runs once when the package is imported, then the\n"
                  "## package's `pub` surface is available on the module.\n"
                  "## Doc comments starting with '##' sit above each `pub`:\n"
                  "## `kirn doc " + name + "` turns them into a browsable API\n"
                  "## reference.\n\n"
                  "## Say hello to someone.\n"
                  "pub fn hello(who: Str) -> Str {\n"
                  "    return \"hello from " + name + ", \" + who + \"!\";\n}\n\n"
                  "## Shout it louder.\n"
                  "pub fn shout(who: Str) -> Str {\n"
                  "    return upper(hello(who));\n}\n");

        writeFile(root / "tests" / (name + "_test.kn"),
                  "# tests live in tests/ and are named <file>_test.kn\n"
                  "# run them all with:  kirn test .\n"
                  "# (import the package's pin.kn initializer; installed\n"
                  "#  consumers import it by name: `import \"" + name + "\"`)\n\n"
                  "import \"code/pin.kn\" as " + name + ";\n\n"
                  "fn main() -> none {\n"
                  "    assert_eq(" + name + ".hello(\"no one\"),\n"
                  "              \"hello from " + name + ", no one!\");\n"
                  "    print(\"all tests passed\");\n}\n");

        writeFile(root / "docs" / "index.md",
                  "# " + name + "\n\n" + m.description +
                      ".\n\n## Install\n\n```bash\n"
                      "kirn install github.com/pets-registry/" + name +
                      "\n```\n\n## Usage\n\n```kirn\n"
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

        std::cout << "created pet '" << name << "'\n"
                  << "  " << name << "/kirn.toml      manifest\n"
                  << "  code/pin.kn   package initializer + pub API\n"
                  << "  tests/          *_test.kn files\n"
                  << "  docs/           markdown docs\n"
                  << "next:\n"
                  << "  cd " << name << " && kirn build pet && kirn test .\n";
    } else {
        m.main = "code/main.kn";
        writeManifest(root, m);

        writeFile(root / "code" / "main.kn",
                  "# " + name + " - a Kirn application.\n\n"
                  "# The error batteries ship in the prelude: bail/ensure,\n"
                  "# ok/err, and the type-erased error need no import.\n"
                  "fn greeting(user: Str) -> result[Str] {\n"
                  "    ensure(user != \"\", \"a user is required\");\n"
                  "    if user == \"root\" { bail(\"root is not allowed\"); }\n"
                  "    return f\"hello from " + name + ", {user}!\";\n"
                  "}\n\n"
                  "fn main() -> none {\n"
                  "    print(\"hello from " + name + "\");\n"
                  "    match greeting(\"world\") {\n"
                  "        case ok(msg) { print(msg); }\n"
                  "        case err(e) { print(error.display(e)); }\n"
                  "    }\n"
                  "}\n");

        writeFile(root / "tests" / "main_test.kn",
                  "# tests live in tests/ and are named <file>_test.kn\n"
                  "# run them all with:  kirn test .\n\n"
                  "fn main() -> none {\n"
                  "    assert_eq(2 + 2, 4);\n"
                  "    # prelude error batteries need no import here either\n"
                  "    match ok(7) {\n"
                  "        case ok(v) { assert_eq(v, 7); }\n"
                  "        case err(_) { assert(false); }\n"
                  "    }\n"
                  "    print(\"all tests passed\");\n}\n");

        writeFile(root / "docs" / "index.md",
                  "# " + name + "\n\n" + m.description + ".\n\n## Run\n\n```"
                  "bash\nkirn run\nkirn test .\nkirn build\n```\n");

        writeFile(root / "README.md",
                  "# " + name + "\n\n" + m.description +
                      ".\n\n## Run\n\n```bash\nkirn run\n```\n");
        writeFile(root / ".gitignore", gitignore);

        std::cout << "created project '" << name << "'\n"
                  << "  " << name << "/kirn.toml      manifest\n"
                  << "  code/main.kn    entry point\n"
                  << "  tests/          *_test.kn files\n"
                  << "  docs/           project docs\n"
                  << "next:\n"
                  << "  cd " << name << " && kirn run\n";
    }
    return 0;
}

// ---------------------------------------------------------------------------
// package sources: local path | git repo | Pets Registry
// ---------------------------------------------------------------------------

struct PkgRef {
    std::string spec;      // normalized repo path or local dir
    std::string tag;
    std::string destName;  // directory name under pets/ or ~/.kirn/pets
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
        if (asPath.extension() == ".pet") return true;  // handled later
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

// repo = "owner/repo" -> "github.com/owner/repo", the shorthand the clone
// pipeline expects ("https://" + spec + ".git")
static std::string petUrl(const std::string& repo) {
    std::string u = repo;
    if (u.find('/') != std::string::npos && u.rfind("github.com/", 0) != 0)
        u = "github.com/" + u;
    return u;
}

// enumerate [pets."<name>"] entries from a Pets Registry index (registry.toml).
// tomlmini folds section headers into flat kv keys ("pets.<name>.<key>"), so
// unlike the old [[lib]] rows these arrive in Doc::kv, not Doc::tables.
static std::vector<std::pair<std::string, std::map<std::string, std::string>>>
enumPets(const Doc& d) {
    std::vector<std::pair<std::string, std::map<std::string, std::string>>> out;
    const std::string prefix = "pets.";
    for (const auto& [k, raw] : d.kv) {
        if (k.rfind(prefix, 0) != 0) continue;
        size_t last = k.rfind('.');
        if (last == std::string::npos) continue;
        std::string name =
            kirn::tomlmini::unquote(k.substr(prefix.size(), last - prefix.size()));
        if (name.empty()) continue;
        std::string key = k.substr(last + 1);
        auto it = std::find_if(out.begin(), out.end(),
                               [&](const auto& p) { return p.first == name; });
        if (it == out.end()) {
            out.push_back({name, {}});
            it = out.end() - 1;
        }
        it->second[key] = kirn::tomlmini::unquote(raw);
    }
    return out;
}

// resolve a bare library name through the Pets Registry
// (github.com/pets-registry/pets -> registry.toml, [pets."<name>"] sections)
bool lookupRegistry(const std::string& name, std::string& url) {
    const std::string cache = ".pets-registry.toml";
    // refresh the cached registry copy; fall back to it when offline
    std::system("curl -s --max-time 15 -o .pets-registry.toml "
                "https://raw.githubusercontent.com/pets-registry/pets/main/"
                "registry.toml");
    std::string text;
    if (!readFile(cache, text)) return false;
    Doc d = kirn::tomlmini::parse(text);
    for (const auto& [pname, kv] : enumPets(d)) {
        if (pname != name) continue;
        auto u = kv.find("repo");
        if (u != kv.end() && !u->second.empty()) {
            url = petUrl(u->second);
            return true;
        }
        return false;
    }
    return false;
}

std::string gitHeadSha(const fs::path& dir) {
    std::string cmd = "git -C \"" + dir.string() + "\" rev-parse HEAD 2>" + shellNullDevice();
    // capture via temp file (system() has no portable pipe-back)
    std::string tmp = ".pets-sha";
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
// (driver/tool warnings print as `warning[W90xx]`; see the band list in
//  src/driver/driver.cpp above installPkgDeps)
static void installPkgDeps(const Manifest& pkg, const fs::path& pkgDir,
                           bool global_, int depth) {
    if (depth > 8) return;
    for (const auto& [name, spec] : pkg.deps) {
        // already present in the target root?
        if (fs::exists(pkgBase(global_) / name)) continue;
        PkgRef dref;
        dref.destName = name;
        if (spec.count("path")) {
            fs::path p = fs::path(spec.at("path"));
            if (p.is_relative()) p = pkgDir / p;
            if (!fs::exists(p)) {
                std::cerr << "  warning[W9001]: dependency '" << name
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

// directory containing this kirn executable (for exe-relative stdlib etc.)

// Install one resolved reference. Returns process exit code.
// Package layout (like cargo/npm hybrids):
//   pet -> <base>/<name>              (importable, registered in the lock)
//   app -> <base>/<name>              (sources stay importable/runnable)
//          + <bin>/<name>              real bundled executable: local
//          ./pets/bin, global ~/.kirn/apps/bin (+ PATH entry for global)
int installOne(const PkgRef& ref, const std::string& raw, bool global_,
               bool record, int depth) {
    fs::path base = pkgBase(global_);
    const fs::path libDest = pkgBase(global_) / ref.destName;
    const fs::path appBin = appsBinDir(global_);

    Manifest pkg;
    std::string sha;
    if (!materializePackage(ref, libDest, pkg, sha)) return 1;

    // bring the package's own dependencies along (transitively)
    if (!pkg.deps.empty()) installPkgDeps(pkg, libDest, global_, depth);

    bool isApp = pkg.type == "app";
    if (isApp) {
        // Install a REAL executable (prebuilt runner + appended app sources),
        // like `go install`. The old Windows-only .cmd batch shim was dead
        // weight on POSIX.
        std::error_code ec;
        fs::create_directories(appBin, ec);
        std::string binFile =
            (appBin / (ref.destName + hostExeSuffix())).generic_string();
        std::string entry = resolveEntry(pkg, libDest);
        if (entry.empty()) {
            std::cerr << "  warning[W9002]: app '" << ref.destName
                      << "' has no entry point; sources installed but not "
                         "launchable\n";
        } else {
            std::string mainSrc;
            if (!readFile((libDest / entry).generic_string(), mainSrc)) {
                std::cerr << "  warning[W9003]: cannot read " << entry << "\n";
            } else {
                // resolve the app's local modules from inside its tree
                std::string old = fs::current_path().generic_string();
                std::error_code cec;
                fs::current_path(libDest, cec);
                std::map<std::string, std::string> embedded;
                // gatherEmbedded's probing dirs resolve against CWD (= libDest
                // here); hand it the tool's own stdlib explicitly
                gatherEmbedded(entry, mainSrc, embedded,
                               {(fs::path(selfExeDir()) / ".." / "stdpet")
                                    .generic_string()});
                fs::current_path(old, cec);
                std::string runner = ensureRunner();
                std::string err;
                if (runner.empty())
                    std::cerr
                        << "  warning[W9004]: interpreter runner unavailable; app "
                           "sources installed but not launchable\n";
                else if (!bundleBinary(runner, binFile,
                                       bundleTail(pkg.name, pkg.version,
                                                  hostTarget(), entry,
                                                  mainSrc, embedded),
                                       err))
                    std::cerr << "  warning[W9005]: " << err << "\n";
            }
        }
        if (global_) {
            if (ensureUserPathContains((fs::absolute(appBin) / "").string()))
                std::cout << "  PATH updated - restart your shell to use '"
                          << ref.destName << "' anywhere\n";
            else
                std::cerr << "  warning[W9006]: could not update PATH\n";
        } else {
            std::cout << "  app binary: "
                      << fs::absolute(appBin).generic_string() << "/"
                      << ref.destName << hostExeSuffix() << "\n";
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
              << (isApp ? "app" : "pet") << ") -> " << (libDest / "").string()
              << (global_ ? "  [global]" : "") << "\n";
    return 0;
}

// resolve a raw spec (bare registry name | path | user/repo | .pet)
static bool resolveRaw(const std::string& raw, PkgRef& ref);
static bool resolveRaw(const std::string& raw, PkgRef& ref) {
    // .pet bundle?
    if (fs::is_regular_file(raw) && fs::path(raw).extension() == ".pet")
        return unpackPet(raw, /*global_=*/false) == 0;
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
    // bare name -> look it up in the Pets Registry
    bool isBareName =
        raw.find('/') == std::string::npos &&
        raw.find('\\') == std::string::npos && raw.find(':') == std::string::npos &&
        !raw.empty() && raw[0] != '.' && !fs::is_directory(raw);
    if (isBareName) {
        std::string url;
        if (!lookupRegistry(raw, url)) {
            std::cerr << "kirn install: '" << raw
                      << "' not found in the Pets Registry\n"
                      << "  browse: https://github.com/pets-registry/pets\n";
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
//                       missing from pets/
int cmdAdd(const std::vector<std::string>& pkgs) {
    if (pkgs.empty()) {
        Manifest proj = readManifest(".");
        if (proj.deps.empty()) {
            std::cout << "nothing to sync ([dependencies] is empty)\n";
            return 0;
        }
        int rc = 0, done = 0;
        for (const auto& [name, spec] : proj.deps) {
if (fs::exists(pkgBase(false) / name)) continue;
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
                              << "' not found in the Pets Registry\n";
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

// `kirn update` supports global pets/apps too
int cmdUpdate(const std::string& only) {
    bool global_ = false;
    // try global pets dir first if not in a project with kirn.toml
    if (!fs::exists(fs::path("kirn.toml")) && only.empty()) {
        // check global
    }
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
                          << "' not found in the Pets Registry\n";
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
        if (!materializePackage(ref, pkgBase(false) / name, pkg, sha)) {
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
    fs::remove_all(pkgBase(false) / name, ec);
    fs::remove_all(fs::path("pets") / "bin" / name, ec);
    fs::remove_all(fs::path("pets") / "bin" / (name + ".exe"), ec);
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
            if (fn == "bin") continue;   // app bins
            Manifest m = readManifest(it->path());
            std::cout << label << fn << " "
                      << (m.version.empty() ? "?" : m.version);
            if (!m.description.empty())
                std::cout << "  # " << m.description;
            std::cout << "\n";
        }
    };
    show("", fs::path("pets"));
    if (!globalPkgDir().empty()) {
        show("[global] ", fs::path(globalPkgDir()));
        // globally installed apps (real binaries under ~/.kirn/apps/bin)
        std::error_code ec;
        fs::path bin(globalAppsBinDir());
        if (fs::is_directory(bin))
            for (fs::directory_iterator it(bin, ec), end; !ec && it != end;
                 it.increment(ec)) {
                if (ec || !it->is_regular_file(ec)) continue;
                std::string fn = it->path().filename().string();
                if (fn == "kirnrt" || fn == "kirnrt.exe") continue;
                if (fn.ends_with(".exe"))
                    fn = fn.substr(0, fn.size() - 4);
                std::cout << "[global:bin] " << fn << "\n";
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
                  << (m.type == "pet" ? " [pet]" : " [app]");
    std::cout << "\nnext:\n  cd " << name << " && kirn run | kirn test .\n";
    return 0;
}

// ---------------------------------------------------------------------------
// kirn list online — browse the Pets Registry
// ---------------------------------------------------------------------------

int cmdListOnline() {
    const std::string cache = ".pets-registry.toml";
    std::system("curl -s --max-time 15 -o .pets-registry.toml "
                "https://raw.githubusercontent.com/pets-registry/pets/main/"
                "registry.toml");
    std::string text;
    if (!readFile(cache, text)) {
        std::cerr << "kirn list online: cannot reach "
                     "raw.githubusercontent.com/pets-registry/pets\n";
        return 1;
    }
    Doc d = kirn::tomlmini::parse(text);
    struct Row { std::string name, desc, url, ver; };
    std::vector<Row> rows;
    // [pets."<name>"] sections arrive as flat kv keys ("pets.<name>.<key>")
    for (const auto& [pname, kv] : enumPets(d)) {
        auto g = [&](const char* k) {
            auto it = kv.find(k);
            return it == kv.end() ? "" : it->second;
        };
        // advertise the latest release (optional per-pet)
        rows.push_back({pname, g("license"), petUrl(g("repo")), g("latest")});
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
        if (fn == "pets" || fn == "build" || fn == ".git") {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(ec) && isTestFile(p)) out.push_back(p);
    }
    // Sort before returning. recursive_directory_iterator yields readdir order,
    // which is filesystem-dependent -- ext4 happens to hand back a creation
    // order that looks alphabetical here, but ext3, APFS, NTFS and overlayfs do
    // not. Without this, `kirn test` output (and therefore the recorded golden
    // for it) changes when the same tree is checked out on a different machine.
    // This is the same rule as reproducible-builds commandment 4: never let
    // filesystem-readdir order reach an output.
    std::sort(out.begin(), out.end());
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
    if (std::find(dirs.begin(), dirs.end(), "./stdpet") == dirs.end())
        dirs.push_back("./stdpet");
    if (std::find(dirs.begin(), dirs.end(), "stdpet") == dirs.end())
        dirs.push_back("stdpet");
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

// Phase 8 slice: a conservative, deterministic formatter foundation.  It does
// not rewrite syntax yet; it normalizes the file-level invariants that every
// later AST formatter can preserve: LF endings, no trailing horizontal
// whitespace, and exactly one final newline.  Keeping this operation
// idempotent gives CI and editors a useful `kirn fmt --check` today without
// risking semantic changes in the unfinished pretty-printer.
std::string formatSource(const std::string& src) {
    std::string out;
    out.reserve(src.size() + 1);
    size_t lineStart = 0;
    while (lineStart < src.size()) {
        size_t end = src.find('\n', lineStart);
        if (end == std::string::npos) end = src.size();
        size_t lineEnd = end;
        if (lineEnd > lineStart && src[lineEnd - 1] == '\r') --lineEnd;
        while (lineEnd > lineStart &&
               (src[lineEnd - 1] == ' ' || src[lineEnd - 1] == '\t'))
            --lineEnd;
        out.append(src, lineStart, lineEnd - lineStart);
        out.push_back('\n');
        lineStart = end == src.size() ? src.size() : end + 1;
    }
    if (out.empty() && !src.empty()) out = "\n";
    while (out.size() > 1 && out[out.size() - 1] == '\n' &&
           out[out.size() - 2] == '\n')
        out.pop_back();
    return out;
}

bool isFmtSource(const fs::path& p) {
    return fs::is_regular_file(p) && p.extension() == ".kn";
}

void collectFmtFiles(const fs::path& root, std::vector<fs::path>& out) {
    if (isFmtSource(root)) {
        out.push_back(root);
        return;
    }
    if (!fs::is_directory(root)) return;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (ec) break;
        const fs::path& p = it->path();
        std::string name = p.filename().string();
        if (name == ".git" || name == "build" || name == ".cache" ||
            name == "node_modules" || name == "pets") {
            it.disable_recursion_pending();
            continue;
        }
        if (isFmtSource(p)) out.push_back(p);
    }
}

void printFmtDiff(const fs::path& path, const std::string& oldText,
                  const std::string& newText) {
    std::cout << "--- " << path.string() << "\n"
              << "+++ " << path.string() << " (formatted)\n";
    std::istringstream oldLines(oldText), newLines(newText);
    std::string line;
    while (std::getline(oldLines, line)) std::cout << "-" << line << "\n";
    while (std::getline(newLines, line)) std::cout << "+" << line << "\n";
}

int cmdFmt(const std::vector<std::string>& args) {
    bool check = false, write = false, diff = false;
    std::vector<fs::path> roots;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--check") check = true;
        else if (args[i] == "-w" || args[i] == "--write") write = true;
        else if (args[i] == "-d" || args[i] == "--diff") diff = true;
        else if (!args[i].empty() && args[i][0] == '-') {
            std::cerr << "kirn fmt: unknown option '" << args[i] << "'\n";
            return 64;
        } else roots.emplace_back(args[i]);
    }
    if (roots.empty()) roots.emplace_back(".");
    if (!write && !check && !diff && roots.size() != 1) {
        std::cerr << "kirn fmt: multiple inputs require --check, --diff, or -w\n";
        return 64;
    }

    std::vector<fs::path> files;
    for (const auto& root : roots) {
        if (!fs::exists(root)) {
            std::cerr << "kirn fmt: no such file or directory: " << root.string()
                      << "\n";
            return 66;
        }
        collectFmtFiles(root, files);
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());

    int changed = 0;
    for (const auto& path : files) {
        std::string oldText;
        if (!readFile(path.string(), oldText)) {
            std::cerr << "kirn fmt: cannot read " << path.string() << "\n";
            return 66;
        }
        std::string newText = formatSource(oldText);
        if (oldText == newText) {
            if (!check && !write && !diff) std::cout << oldText;
            continue;
        }
        ++changed;
        if (check) std::cout << "would reformat " << path.string() << "\n";
        if (diff) printFmtDiff(path, oldText, newText);
        if (write && !writeFile(path, newText)) {
            std::cerr << "kirn fmt: cannot write " << path.string() << "\n";
            return 66;
        }
        if (!check && !write && !diff) std::cout << newText;
    }
    if (check) return changed == 0 ? 0 : 1;
    return 0;
}

bool replStatementLike(const std::string& text) {
    size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return true;
    std::string head = text.substr(first);
    for (const char* kw : {"var ", "const ", "local ", "global ", "temp ",
                           "bucket ", "if ", "while ", "for ", "match ",
                           "try ", "defer ", "return ", "raise ", "print("})
        if (head.rfind(kw, 0) == 0) return true;
    size_t eq = head.find('=');
    return eq != std::string::npos &&
           (eq + 1 == head.size() || head[eq + 1] != '=') &&
           (eq == 0 || head[eq - 1] != '=' && head[eq - 1] != '<' &&
            head[eq - 1] != '>');
}

int replBraceDelta(const std::string& text) {
    int depth = 0;
    bool string = false, escape = false;
    for (char c : text) {
        if (escape) { escape = false; continue; }
        if (c == '\\' && string) { escape = true; continue; }
        if (c == '"' && !string) { string = true; continue; }
        if (c == '"' && string) { string = false; continue; }
        if (!string && c == '{') ++depth;
        if (!string && c == '}') --depth;
    }
    return depth;
}

std::string replNormalizeStatements(const std::string& text) {
    std::istringstream in(text);
    std::ostringstream out;
    std::string line;
    while (std::getline(in, line)) {
        size_t first = line.find_first_not_of(" \t");
        size_t last = line.find_last_not_of(" \t\r");
        if (first != std::string::npos && last != std::string::npos &&
            line.compare(first, 6, "print(") == 0 && line[last] == ')')
            line.push_back(';');
        out << line << '\n';
    }
    return out.str();
}

int cmdRepl() {
    std::cout << "Kirn REPL (:help for help, :quit to exit)\n";
    std::string buffer;
    std::string declarations;
    int braces = 0;
    auto dirs = libDirsFor(".");
    std::string line;
    while (std::getline(std::cin, line)) {
        if (buffer.empty() && line == ":help") {
            std::cout << ":help  show this help\n:quit  exit the REPL\n"
                         "Enter an expression to print its value, or a Kirn statement.\n";
            continue;
        }
        if (buffer.empty() && (line == ":quit" || line == ":q")) break;
        if (buffer.empty() && line.empty()) continue;

        buffer += line;
        buffer.push_back('\n');
        braces += replBraceDelta(line);
        if (braces > 0) continue;

        bool hasMain = buffer.find("fn main") != std::string::npos ||
                       buffer.find("async fn main") != std::string::npos;
        bool declaration = buffer.find("fn ") == 0 ||
                           buffer.find("struct ") == 0 ||
                           buffer.find("enum ") == 0 ||
                           buffer.find("trait ") == 0;
        std::string source;
        if (hasMain) source = buffer;
        else if (declaration) {
            declarations += buffer;
            source = declarations + "\nfn main() -> none {}\n";
        } else if (replStatementLike(buffer))
            source = declarations + "fn main() -> none {\n" +
                    replNormalizeStatements(buffer) + "}\n";
        else
            source = declarations + "fn main() -> none {\nprint(" + buffer + ");\n}\n";

        int rc = runProgramSrc("<repl>", source, dirs, {});
        if (rc != 0) std::cout << "repl: evaluation failed (rc=" << rc << ")\n";
        buffer.clear();
        braces = 0;
    }
    return 0;
}

int cmdGetScan() {
    Manifest manifest = readManifest(".");
    if (manifest.name.empty()) {
        std::cerr << "kirn get --scan: no kirn.toml found\n";
        return 66;
    }
    std::set<std::string> used;
    size_t files = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(".", ec), end; !ec && it != end;
         it.increment(ec)) {
        if (ec) break;
        const fs::path& path = it->path();
        std::string name = path.filename().string();
        if (name == ".git" || name == "build" || name == ".cache" ||
            name == "node_modules" || name == "pets") {
            it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec) || path.extension() != ".kn") continue;
        std::string src;
        if (!readFile(path.string(), src)) continue;
        ++files;
        std::vector<std::string> imports;
        collectImports(path.string(), src, imports);
        for (const auto& imp : imports) {
            size_t dot = imp.find('.');
            used.insert(dot == std::string::npos ? imp : imp.substr(0, dot));
        }
    }
    int unused = 0;
    std::cout << "scanned " << files << " source file(s)\n";
    for (const auto& dep : manifest.deps) {
        if (used.count(dep.first)) continue;
        std::cout << "unused dependency: " << dep.first << "\n";
        ++unused;
    }
    if (unused == 0) std::cout << "no unused dependencies\n";
    return 0;
}

std::string lspJsonEscape(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) {
            char buf[7];
            snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
        } else out.push_back((char)c);
    }
    return out;
}

std::string lspStringField(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t p = json.find(needle);
    if (p == std::string::npos) return {};
    p = json.find(':', p + needle.size());
    if (p == std::string::npos) return {};
    p = json.find('"', p + 1);
    if (p == std::string::npos) return {};
    std::string out;
    bool esc = false;
    for (++p; p < json.size(); ++p) {
        char c = json[p];
        if (esc) {
            if (c == 'n') out.push_back('\n');
            else if (c == 'r') out.push_back('\r');
            else if (c == 't') out.push_back('\t');
            else out.push_back(c);
            esc = false;
        } else if (c == '\\') esc = true;
        else if (c == '"') break;
        else out.push_back(c);
    }
    return out;
}

std::string lspId(const std::string& json) {
    size_t p = json.find("\"id\"");
    if (p == std::string::npos) return "null";
    p = json.find(':', p + 4);
    if (p == std::string::npos) return "null";
    ++p;
    while (p < json.size() && std::isspace((unsigned char)json[p])) ++p;
    if (p < json.size() && json[p] == '"') {
        size_t e = json.find('"', p + 1);
        return e == std::string::npos ? "null" : json.substr(p, e - p + 1);
    }
    size_t e = p;
    while (e < json.size() && (std::isdigit((unsigned char)json[e]) ||
                               json[e] == '-')) ++e;
    return e == p ? "null" : json.substr(p, e - p);
}

int lspIntField(const std::string& json, const std::string& key, int fallback) {
    std::string needle = "\"" + key + "\"";
    size_t p = json.find(needle);
    if (p == std::string::npos) return fallback;
    p = json.find(':', p + needle.size());
    if (p == std::string::npos) return fallback;
    ++p;
    while (p < json.size() && std::isspace((unsigned char)json[p])) ++p;
    char* end = nullptr;
    long value = std::strtol(json.c_str() + p, &end, 10);
    return end == json.c_str() + p ? fallback : (int)value;
}

std::string lspWordAt(const std::string& source, int line, int character) {
    size_t start = 0;
    for (int i = 0; i < line && start < source.size(); ++i) {
        size_t nl = source.find('\n', start);
        if (nl == std::string::npos) return {};
        start = nl + 1;
    }
    if (start > source.size()) return {};
    size_t pos = std::min(start + (size_t)std::max(character, 0), source.size());
    size_t left = pos;
    while (left > start && (std::isalnum((unsigned char)source[left - 1]) ||
                            source[left - 1] == '_')) --left;
    size_t right = pos;
    while (right < source.size() && (std::isalnum((unsigned char)source[right]) ||
                                     source[right] == '_')) ++right;
    return source.substr(left, right - left);
}

std::string lspCompletionItems(const std::string& prefix) {
    static const char* names[] = {
        "fn", "var", "const", "if", "else", "while", "for", "match",
        "return", "raise", "try", "catch", "defer", "struct", "enum",
        "trait", "impl", "import", "pub", "true", "false", "none",
        "print", "len", "range", "panic", "spawn", "await"
    };
    std::string out = "[";
    bool first = true;
    for (const char* name : names) {
        if (!prefix.empty() && std::string(name).rfind(prefix, 0) != 0) continue;
        if (!first) out += ",";
        first = false;
        out += "{\"label\":\"" + std::string(name) + "\",\"kind\":14}";
    }
    return out + "]";
}

std::string lspHover(const std::string& word) {
    static const std::map<std::string, std::string> docs = {
        {"fn", "Declares a function: `fn name(params) -> Type { ... }`."},
        {"var", "Declares a mutable local binding."},
        {"const", "Declares an immutable compile-time binding."},
        {"print", "Prints values using Kirn's `toStr` representation."},
        {"len", "Returns the length of a collection or string."},
        {"range", "Creates an integer range for iteration."},
        {"spawn", "Starts a task and returns a task handle."},
        {"await", "Waits for a task or timer and yields its result."}
    };
    auto it = docs.find(word);
    if (it == docs.end()) return "null";
    return "{\"contents\":{\"kind\":\"markdown\",\"value\":\"" +
           lspJsonEscape(it->second) + "\"}}";
}

void lspReply(const std::string& body) {
    std::cout << "Content-Length: " << body.size() << "\r\n\r\n" << body
              << std::flush;
}

int cmdLsp() {
    std::map<std::string, std::string> open;
    std::string header, payload;
    while (std::getline(std::cin, header)) {
        if (header.rfind("Content-Length:", 0) != 0) continue;
        size_t len = std::stoul(header.substr(15));
        while (std::getline(std::cin, header) && !header.empty() && header != "\r") {}
        payload.assign(len, '\0');
        std::cin.read(payload.data(), (std::streamsize)len);
        if ((size_t)std::cin.gcount() != len) break;

        std::string method = lspStringField(payload, "method");
        std::string id = lspId(payload);
        if (method == "initialize") {
            lspReply("{\"jsonrpc\":\"2.0\",\"id\":" + id +
                     ",\"result\":{\"capabilities\":{\"textDocumentSync\":1,\"completionProvider\":{},\"hoverProvider\":true,\"documentFormattingProvider\":true}}}");
        } else if (method == "shutdown") {
            lspReply("{\"jsonrpc\":\"2.0\",\"id\":" + id +
                     ",\"result\":null}");
        } else if (method == "exit") {
            return 0;
        } else if (method == "textDocument/didOpen") {
            std::string uri = lspStringField(payload, "uri");
            std::string text = lspStringField(payload, "text");
            if (!uri.empty()) open[uri] = std::move(text);
        } else if (method == "textDocument/completion") {
            std::string uri = lspStringField(payload, "uri");
            int line = lspIntField(payload, "line", 0);
            int character = lspIntField(payload, "character", 0);
            std::string prefix = lspWordAt(open[uri], line, character);
            lspReply("{\"jsonrpc\":\"2.0\",\"id\":" + id +
                     ",\"result\":{\"isIncomplete\":false,\"items\":" +
                     lspCompletionItems(prefix) + "}}");
        } else if (method == "textDocument/hover") {
            std::string uri = lspStringField(payload, "uri");
            int line = lspIntField(payload, "line", 0);
            int character = lspIntField(payload, "character", 0);
            lspReply("{\"jsonrpc\":\"2.0\",\"id\":" + id +
                     ",\"result\":" + lspHover(lspWordAt(open[uri], line, character)) + "}");
        } else if (method == "textDocument/formatting") {
            std::string uri = lspStringField(payload, "uri");
            const std::string& source = open[uri];
            std::string formatted = formatSource(source);
            std::string edits = "[]";
            if (source != formatted) {
                int endLine = 0;
                for (char c : source) if (c == '\n') ++endLine;
                int endChar = 0;
                size_t last = source.rfind('\n');
                if (last == std::string::npos) endChar = (int)source.size();
                else endChar = (int)(source.size() - last - 1);
                edits = "[{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":" +
                        std::to_string(endLine) + ",\"character\":" +
                        std::to_string(endChar) + "}},\"newText\":\"" +
                        lspJsonEscape(formatted) + "\"}]";
            }
            lspReply("{\"jsonrpc\":\"2.0\",\"id\":" + id +
                     ",\"result\":" + edits + "}");
        } else if (method == "textDocument/diagnostic") {
            std::string uri = lspStringField(payload, "uri");
            std::string source = open[uri];
            kirn::DiagEngine diags;
            if (!source.empty()) kirn::driver::frontEnd(uri, source, diags);
            std::string items = "[";
            for (size_t i = 0; i < diags.diags().size(); ++i) {
                const auto& d = diags.diags()[i];
                if (i) items += ",";
                int severity = d.sev == kirn::Sev::Warning ? 2 : 1;
                items += "{\"range\":{\"start\":{\"line\":" +
                         std::to_string(d.line ? d.line - 1 : 0) +
                         ",\"character\":" + std::to_string(d.col ? d.col - 1 : 0) +
                         "},\"end\":{\"line\":" +
                         std::to_string(d.span.endLine ? d.span.endLine - 1 : 0) +
                         ",\"character\":" + std::to_string(d.span.endCol ? d.span.endCol - 1 : 0) +
                         "}},\"severity\":" + std::to_string(severity) +
                         ",\"message\":\"" + lspJsonEscape(d.message) +
                         "\",\"source\":\"kirn\"}";
            }
            items += "]";
            lspReply("{\"jsonrpc\":\"2.0\",\"id\":" + id +
                     ",\"result\":{\"kind\":\"full\",\"items\":" + items + "}}");
        }
    }
    return 0;
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
        fs::path local = fs::path("pets") / target;
        fs::path glob;
        if (!globalPkgDir().empty()) glob = fs::path(globalPkgDir()) / target;
        if (fs::is_directory(local))
            libDir = local;
        else if (!glob.empty() && fs::is_directory(glob))
            libDir = glob;
        else {
            std::cerr << "kirn doc: no project or installed pet '"
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
// kirn build — app: standalone bundle (prebuilt runtime + appended sources;
//              native/A-san/cross variants go through the C++ toolchain)
//              pet: type-check all sources + pack distributable .pet
// ---------------------------------------------------------------------------

// resolve a module name to a source file across the given dirs (loader rules)

// ---- build options ---------------------------------------------------------
// Output layout is cargo-flavored but Kirn-named:
//   build/<profile>/<target>/...      profile: debug | release
// Targets: <os>-<arch>, os in {windows,linux,darwin}, arch in {amd64,arm64}



// Phase 8: the KIRN_* environment variables are renamed to KIRN_* and are a
// hard cut - the old names are never read. If a user/script still exports one,
// say so once (aggregated) rather than silently discarding it: a stale var
// losing effect quietly is exactly how module paths and toolchains silently
// stop applying (the IOWarp "unset variable is not an error" failure mode).

// every runtime source needed to build a self-contained launcher from scratch

// Serialize the KIRNBUNDLE tail: a @meta section, the entry section, then one
// section per imported module. The runner (tools/runner.cpp) parses it back.

// resolve module sources transitively from the entry file

// ---- -S: human-readable pseudo-assembly listing -----------------------------
// KIRN-SASM v1: a linear, labeled listing derived from the AST. Great for
// reading what your program does; not (yet) an executable ISA.


// Core build pipeline shared by project mode (kirn.toml) and single-file
// mode (`kirn build main.kn`): type-check, then emit sasm / self-contained
// launcher and compile it with the best available pipeline.



}  // namespace


int main(int argc, char** argv) {
    warnLegacyEnv();
    std::vector<std::string> args(argv + (argc > 0 ? 1 : 0), argv + argc);
    if (args.empty()) {
        usage();
        return 64;
    }
    const std::string& cmd = args[0];
    auto rest = [&](size_t i) {
        return i < args.size() ? args[i] : std::string();
    };

    if ((cmd == "run" || cmd == "r") && !args.empty()) {
        // bare `kirn run` targets the current directory (the scaffold hint
        // tells users to `cd <project> && kirn run`).
        bool gc = args.size() >= 2 && args[1] == "--gc";
        size_t targetIndex = gc ? 2 : 1;
        bool hasTarget = args.size() > targetIndex;
        std::string target = hasTarget ? args[targetIndex] : std::string(".");
        std::vector<std::string> progArgs(args.begin() + (hasTarget ? targetIndex + 1 : targetIndex),
                                          args.end());
        fs::path file(target);
        if (fs::is_directory(file)) {
            Manifest m = readManifest(file);
            std::string entry = resolveEntry(m, file);
            if (entry.empty()) {
                std::cerr
                    << "kirn run: no entry point found in '" << target
                    << "'\n"
<< "  looked for (in order): kirn.toml [package] main, "
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
        return runProgram(file.string(), dirs, {}, progArgs, gc);
    }
    if (cmd == "new" && args.size() >= 2) {
        if (args[1] == "lib") {
            std::cerr << "kirn new: `new lib` was renamed to `new pet`\n";
            return 64;
        }
        bool pet = args[1] == "pet";
        size_t nameIdx = pet ? 2 : 1;
        if ((pet && args.size() != 3) || (!pet && args.size() != 2)) {
            usage();
            return 64;
        }
        return cmdNew(rest(nameIdx), pet);
    }
    if (cmd == "test" && args.size() >= 1) return cmdTest(args, 1);
    if (cmd == "fmt") return cmdFmt(args);
    if (cmd == "check") {
        std::vector<std::string> checkArgs(args.begin() + 1, args.end());
        return cmdCheck(checkArgs, "kirn check");
    }
    if (cmd == "lint") {
        std::vector<std::string> lintArgs(args.begin() + 1, args.end());
        return cmdCheck(lintArgs, "kirn lint");
    }
    if (cmd == "repl") {
        if (args.size() != 1) {
            std::cerr << "usage: kirn repl\n";
            return 64;
        }
        return cmdRepl();
    }
    if (cmd == "env") {
        if (args.size() != 1) {
            std::cerr << "usage: kirn env\n";
            return 64;
        }
        return cmdEnv();
    }
    if (cmd == "publish") {
        if (args.size() != 1) {
            std::cerr << "usage: kirn publish\n";
            return 64;
        }
        return cmdPublish();
    }
    if (cmd == "lsp") {
        if (args.size() != 1) {
            std::cerr << "usage: kirn lsp\n";
            return 64;
        }
        return cmdLsp();
    }
    if (cmd == "install" || cmd == "i") {
        bool global_ = false;
        std::string pkg;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "-g" || args[i] == "--global") global_ = true;
            else pkg = args[i];
        }
        if (pkg.empty()) {
            if (!global_ && fs::exists(fs::path("kirn.toml"))) {
                std::vector<std::string> empty;
                return cmdAdd(empty);
            }
            usage();
            return 64;
        }
        if (fs::is_regular_file(pkg) &&
            fs::path(pkg).extension() == ".pet")
            return kirn::driver::unpackPet(pkg, global_);
        return cmdInstall(pkg, global_);
    }
    if (cmd == "get") {
        bool global_ = false;
        std::string pkg;
        bool scan = false;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "-g" || args[i] == "--global") global_ = true;
            else if (args[i] == "--scan") scan = true;
            else pkg = args[i];
        }
        if (scan) {
            if (!pkg.empty() || global_) {
                std::cerr << "kirn get: --scan cannot be combined with a package or --global\n";
                return 64;
            }
            return cmdGetScan();
        }
        if (pkg.empty()) {
            std::cerr << "usage: kirn get [-g] <pkg>\n";
            return 64;
        }
        return cmdInstall(pkg, global_);
    }
    if (cmd == "update") {
        std::string only;
        bool global_ = false;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "-g" || args[i] == "--global") global_ = true;
            else only = args[i];
        }
        return cmdUpdate(only);
    }
    if (cmd == "sync") {
        if (!fs::exists(fs::path("kirn.toml"))) {
            std::cerr << "kirn sync: no kirn.toml found\n";
            return 1;
        }
        std::vector<std::string> empty;
        return cmdAdd(empty);
    }
    if (cmd == "remove") {
        bool global_ = false;
        std::string name;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "-g" || args[i] == "--global") global_ = true;
            else name = args[i];
        }
        if (name.empty()) { usage(); return 64; }
        return cmdRemove(name);
    }
    if (cmd == "add") {
        std::vector<std::string> packages(args.begin() + 1, args.end());
        return cmdAdd(packages);
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
