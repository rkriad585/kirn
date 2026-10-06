// driver.h — the public surface of kirn_driver, the driver core extracted from
// tools/kirn.cpp (BUILDER-PLAN.md phase 0).
//
// The implementation deliberately lives in a single translation unit
// (src/driver/driver.cpp) that keeps its original anonymous namespace. That is
// not laziness: an unnamed namespace has internal linkage *per translation
// unit*, so splitting the 3112-line block across several .cpp files would force
// every cross-file symbol to acquire external linkage, and with it ODR, inline,
// and static-initialization-order risk. One TU preserves linkage exactly, which
// is what makes this refactor behavior-preserving by construction.
//
// The split is a *library* split, not a *compilation* split: kirn and the
// future kirnc both link this archive and call the same code. New compiler
// stages (see the plan's phases 2-5) get added to driver.cpp, not forked out of
// it, so there is one implementation of the pipeline rather than two that drift.
#ifndef KIRN_DRIVER_DRIVER_H
#define KIRN_DRIVER_DRIVER_H

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace kirn {
namespace ast {
struct Stmt;
using StmtP = std::unique_ptr<Stmt>;
}  // namespace ast

class DiagEngine;

namespace mir {
struct Program;
}  // namespace mir

namespace sema {
// Only ever held behind a std::unique_ptr reference parameter, so an incomplete
// type is enough here; the pointee is complete in driver.cpp, which is the only
// place the unique_ptr is destroyed.
class Checker;
}  // namespace sema
}  // namespace kirn

namespace kirn {
namespace driver {

namespace fs = std::filesystem;

// --- package model ---------------------------------------------------------
// Shared with the install/new/docs verbs in tools/kirn.cpp, so it is part of
// the library's public types rather than an implementation detail.
using Deps = std::map<std::string, std::map<std::string, std::string>>;

struct Manifest {
    std::string name;
    std::string version = "0.0.1-beta";
    std::string type = "app";
    std::string main;
    std::string description;
    std::string license;
    std::string docs = "docs/index.md";
    std::string author;
    std::string repo;
    std::string readme = "README.md";
    std::string homepage;
    std::vector<std::string> authors;
    std::vector<std::string> tags;
    std::vector<std::string> keywords;
    bool gitignore = true;
    bool gitkeep = false;
    std::vector<std::string> gitIgnoreExtra;
    Deps deps;
};

// Build options for `kirn build` / `kirnc build`. A plain value type with no
// methods, so it is safe to hand across the library boundary.
struct BuildOpts {
    bool release = false;
    bool wantPet = false;
    bool sasm = false;
    bool obj = false;
    bool singleFile = false;
    bool native_ = false;
    bool asan = false;
    std::string target;
    std::string outPath;
    std::string defaultOut;
};

struct TargetInfo {
    const char* triple;
    const char* exeExt;      // ".exe" or ""
    bool isWindows;
    std::vector<std::string> cxxCandidates;
};

// --- file and path primitives ---------------------------------------------
bool readFile(const std::string& path, std::string& out);
bool writeFile(const fs::path& path, const std::string& content);
std::string trimSlashes(std::string s);
std::string lastSegment(const std::string& p);
const char* shellNullDevice();

// --- manifest --------------------------------------------------------------
Manifest readManifest(const fs::path& dir);
void writeManifest(const fs::path& dir, const Manifest& m);
std::string resolveEntry(const Manifest& m, const fs::path& dir);

// --- search roots ----------------------------------------------------------
std::string globalPkgDir();
std::string globalAppsBinDir();

// THE resolver. `kirn run`, `kirn test` and `kirnrun` all funnel through this
// one ordered list (phase 0 §5.2): a project imports itself, the project's own
// directories win over the global install, and an env override wins over all.
std::vector<std::string> libDirsFor(const std::string& script);
fs::path pkgBase(bool global_);
fs::path appsBinDir(bool global_);
std::string selfExeDir();

// --- front end and run -----------------------------------------------------
std::vector<kirn::ast::StmtP> frontEnd(const std::string& path,
                                       const std::string& src,
                                       kirn::DiagEngine& diags);
// Same pipeline, but returns the Checker that produced the types. The Checker
// used to be a local that died with frontEnd, so every consumer that needed
// resolved types had to re-lex and re-parse to obtain one.
std::vector<kirn::ast::StmtP> frontEnd(const std::string& path,
                                       const std::string& src,
                                       kirn::DiagEngine& diags,
                                       std::unique_ptr<kirn::sema::Checker>& chkOut);
void printDiags(const std::string& path, const kirn::DiagEngine& diags);
int runProgramSrc(const std::string& label, const std::string& src,
                  const std::vector<std::string>& dirs,
                  const std::map<std::string, std::string>& embedded,
                  const std::vector<std::string>& progArgs = {});
int runProgram(const std::string& entryPath,
               const std::vector<std::string>& dirs,
               const std::map<std::string, std::string>& embedded = {},
               const std::vector<std::string>& progArgs = {});

// --- stage-oracle verbs (phase 1) -------------------------------------------
// Extracted from the kirnlex / kirnparse / kirncheck main()s so `kirnc
// lex|parse|check` and the legacy binaries share one implementation. `args` is
// the argv tail (everything after the program name) and `toolName` is the
// invoked name, used for usage/error text; pass "kirnlex", "kirnparse",
// "kirncheck" or "kirnc" to reproduce that tool's exact bytes.
//
// These deliberately do NOT reuse readFile(): it strips a UTF-8 BOM, and the
// recorded golden output for the 7 BOM'd files in tests/negative/ contains
// those bytes in the echoed source line. They also write diagnostics to stdout
// via renderDiags, not to stderr via printDiags.
int cmdLex(const std::vector<std::string>& args, const char* toolName);
int cmdParse(const std::vector<std::string>& args, const char* toolName);
int cmdCheck(const std::vector<std::string>& args, const char* toolName);
// -E: preprocess only, print the expanded token stream. Phase 2.
int cmdPreprocess(const std::vector<std::string>& args, const char* toolName);
// --emit-hir: lower to HIR, verify it structurally, and print the dump. Phase 3.
int cmdHir(const std::vector<std::string>& args, const char* toolName);
// rt / --emit-mir: lower through HIR to MIR, verify the CFG, print the dump.
// `rt` is the MIR dumper for this phase; BUILDER-PLAN section 6 gives `rt` two
// incompatible meanings and resolves it as the dumper here, because the phase-3
// text says "kirnc rt dumps MIR". The runner meaning is deferred to the phase
// that retires the bundle.
int cmdRt(const std::vector<std::string>& args, const char* toolName);

// --- MIR pipeline (shared by `rt` and `--emit-llvm`) ------------------------
// Front-end -> HIR -> verify -> MIR -> verify, with all diagnostics captured so
// the two verbs print bit-identical bytes. Errors are rendered into `err`; the
// stream that renders them differs per stage (syntax/HIR errors and verifier
// reports are pipes the caller decides), so the caller picks stdout vs stderr:
// rc 66 -> stderr (cannot open the file), rc 65 / 1 -> stdout, rc 0 -> success.
struct MirOutcome {
    int rc = 0;               // 0 ok | 1 source/HIR errors | 65 verifier | 66 no file
    std::string src;          // source text when rc == 0
    std::string err;          // user-facing error text when rc != 0
    // When rc == 0, `prog` carries the verified MIR (unique_ptr so the header
    // stays free of a mir/mir.h include, matching the Checker pattern above).
    std::unique_ptr<kirn::mir::Program> prog;
    MirOutcome() = default;
    MirOutcome(MirOutcome&&) = default;
    MirOutcome& operator=(MirOutcome&&) = default;
    // Defined out-of-line in driver.cpp: the implicit one would instantiate
    // default_delete<Program> in every TU that holds a MirOutcome, but only
    // driver.cpp sees the complete type.
    ~MirOutcome();
};
MirOutcome produceMir(const std::string& file, const char* toolName);

// --- source graph / embedding ---------------------------------------------
bool resolveSource(const std::string& dotted,
                   const std::vector<std::string>& dirs, std::string& path,
                   std::string& src);
void collectImports(const std::string& path, const std::string& src,
                    std::vector<std::string>& names);
bool gatherEmbedded(const std::string& entry, const std::string& mainSrc,
                    std::map<std::string, std::string>& embedded,
                    const std::vector<std::string>& extraDirs = {});

// --- targets and toolchains ------------------------------------------------
std::string hostTarget();
const std::vector<TargetInfo>& targetMatrix();
const TargetInfo* findTarget(const std::string& t);
bool validTarget(const std::string& t);
std::string hostExeSuffix();
bool toolchainWorks(const std::string& cxx);
std::string resolveCrossCxx(const TargetInfo* ti);
std::string resolveHostCl();
std::string detectRuntimeFlags(const std::string& binRoot);
std::vector<std::string> collectRuntimeSources(const std::string& srcRoot);

// --- bundle and runner -----------------------------------------------------
std::string bundleTail(const std::string& name, const std::string& version,
                       const std::string& target, const std::string& entry,
                       const std::string& entrySrc,
                       const std::map<std::string, std::string>& embedded);
bool bundleBinary(const std::string& runner, const fs::path& out,
                  const std::string& payload, std::string& err);
std::string ensureRunner();

// --- assembly listing ------------------------------------------------------
std::string emitSasm(const std::string& srcPath,
                     const std::vector<kirn::ast::StmtP>& body);

// --- inline API docs -------------------------------------------------------
// Lives in the library because `packLib` regenerates docs/api.md as part of
// building a .pet, while the `kirn doc` verb triggers the same generator.
std::string extractApiDocs(const Manifest& m);
bool regenerateDocs(const Manifest& m);

// --- build -----------------------------------------------------------------
int buildProgram(const std::string& name, const std::string& version,
                 const std::string& entry, const std::string& mainSrc,
                 const std::map<std::string, std::string>& embedded,
                 BuildOpts& opts);
int packLib(const Manifest& m, const BuildOpts& opts);
int buildAppShim(const Manifest& m, BuildOpts& opts);
int cmdBuild(const std::vector<std::string>& args, size_t from);
int unpackPet(const std::string& raw, bool global_);

// --- verbs that stay in tools/kirn.cpp but reach into the driver ----------
int cmdTargets();
void warnLegacyEnv();
void usage();

}  // namespace driver
}  // namespace kirn

#endif  // KIRN_DRIVER_DRIVER_H
