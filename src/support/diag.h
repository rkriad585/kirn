#pragma once
// Diagnostics for the Kirn frontend + runtime (PLAN phase 1).
//
// Backward-compatible with the original minimal model:
//   DiagEngine::report(line, col, msg)  ->  stores Diag{line,col,message}
//   DiagEngine::diags() / ok() / count()  ->  unchanged
// so existing callers (kirnlex/kirncheck/kirnparse/kirnrun/kirn/runtime)
// keep compiling. New code should prefer the fluent Diagnostics builder and
// the SourceMap renderer described below.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace kirn {

// A rectangular source region: 1-based line, 1-based column, inclusive end.
struct SpanRange {
    uint32_t line = 0;
    uint32_t col = 1;
    uint32_t endLine = 0;
    uint32_t endCol = 0;   // column just past the last highlighted char

    static SpanRange point(uint32_t l, uint32_t c) {
        return SpanRange{l, c, l, c + 1};
    }
    bool valid() const { return line != 0; }
};

enum class Sev { Note, Warning, Error, InternalError };

// A machine-applicable source edit suggestion.
struct FixIt {
    SpanRange range;
    std::string replacement;
};

// A secondary underline / explanation attached to a primary diagnostic.
struct Note {
    SpanRange span;
    std::string message;
};

struct Diag {
    // --- legacy fields (kept for existing callers) -------------------------
    uint32_t line = 0;
    uint32_t col = 1;
    std::string message;

    // --- phase-1 structured fields (defaults preserve legacy behaviour) ----
    Sev sev = Sev::Error;
    std::string code;               // stable, e.g. "E0027" / "W0134"
    SpanRange span;                 // full extent; falls back to point(line,col)
    std::vector<Note> notes;
    std::vector<FixIt> fixIts;

    // A legacy 0-based message with a prior count of diagnostics, used only
    // for the old "N error(s)" summary — kept internal here.
    const std::string& msg() const { return message; }
};

class DiagEngine {
public:
    // --- legacy API --------------------------------------------------------
    void report(uint32_t line, uint32_t col, std::string msg) {
        Diag d;
        d.line = line;
        d.col = col;
        d.message = std::move(msg);
        d.sev = Sev::Error;
        d.span = SpanRange::point(line, col);
        diags_.push_back(std::move(d));
    }

    const std::vector<Diag>& diags() const { return diags_; }
    bool ok() const { return diags_.empty(); }
    size_t count() const { return diags_.size(); }

    // --- phase-1 fluent builder --------------------------------------------
    // Usage:
    //   engine.error(span).code("E0027").msg("undefined variable")
    //         .note(span2, "...").fixit(span3, "...");
    class Builder {
    public:
        Builder(DiagEngine& e, Sev s) : eng_(e), sev_(s) {
            d_.sev = s;
            if (s == Sev::Warning) d_.code = "W0000";
            else if (s == Sev::Note) d_.code = "N0000";
            else d_.code = "E0000";
        }

        Builder& at(SpanRange s) {
            d_.span = s;
            d_.line = s.line;
            d_.col = s.col;
            return *this;
        }
        Builder& code(std::string c) { d_.code = std::move(c); return *this; }
        Builder& msg(std::string m)   { d_.message = std::move(m); return *this; }

        Builder& note(SpanRange s, std::string m) {
            d_.notes.push_back(Note{s, std::move(m)});
            return *this;
        }
        Builder& fixit(SpanRange s, std::string r) {
            d_.fixIts.push_back(FixIt{s, std::move(r)});
            return *this;
        }

        // Emits the diagnostic; only meaningful at the end of the chain.
        void emit() {
            if (d_.span.valid() && d_.line == 0) {
                d_.line = d_.span.line;
                d_.col = d_.span.col;
            }
            eng_.diags_.push_back(std::move(d_));
        }

    private:
        DiagEngine& eng_;
        Sev sev_;
        Diag d_;
    };

    Builder error(SpanRange s)   { return Builder(*this, Sev::Error).at(s); }
    Builder warning(SpanRange s) { return Builder(*this, Sev::Warning).at(s); }
    Builder note(SpanRange s)    { return Builder(*this, Sev::Note).at(s); }

    int errorCount() const {
        int n = 0;
        for (const auto& d : diags_) if (d.sev == Sev::Error) n++;
        return n;
    }
    int warningCount() const {
        int n = 0;
        for (const auto& d : diags_) if (d.sev == Sev::Warning) n++;
        return n;
    }

private:
    std::vector<Diag> diags_;
};

// =============================================================================
// SourceMap + renderer: turns a raw source buffer and a Diag into a GCC/Clang
// style "file:line:col: error[CODE]: msg" block with a caret-underlined line.
// =============================================================================

class SourceMap {
public:
    SourceMap() = default;
    explicit SourceMap(std::string src) { setSource(std::move(src)); }

    void setSource(std::string src) { src_ = std::move(src); buildIndex(); }
    const std::string& source() const { return src_; }

    // Returns the text of a 1-based line (without trailing newline), or "".
    std::string lineText(uint32_t line) const {
        if (line == 0 || line > starts_.size()) return "";
        size_t begin = starts_[line - 1];
        size_t end = (line < starts_.size()) ? starts_[line] : src_.size();
        // strip trailing \r and \n
        while (end > begin && (src_[end - 1] == '\n' || src_[end - 1] == '\r')) end--;
        return src_.substr(begin, end - begin);
    }

private:
    void buildIndex() {
        starts_.clear();
        starts_.push_back(0);
        for (size_t i = 0; i < src_.size(); i++) {
            if (src_[i] == '\n') starts_.push_back(i + 1);
        }
    }

    std::string src_;
    std::vector<size_t> starts_;
};

// Renders all diagnostics of `diags` for `path` into `out` using `src`.
// Mirrors the legacy plain format when `plain` is true (no caret/color).
inline void renderDiags(const std::string& path, const SourceMap& src,
                        const std::vector<Diag>& diags, bool color, bool plain,
                        std::string& out) {
    auto esc = [&](const char* c) { return color ? std::string(c) : std::string(""); };
    const char* C_RESET  = "\x1b[0m";
    const char* C_BOLD   = "\x1b[1m";
    const char* C_RED    = "\x1b[31m";
    const char* C_GREEN  = "\x1b[32m";
    const char* C_YELLOW = "\x1b[33m";
    const char* C_CYAN   = "\x1b[36m";

    for (const auto& d : diags) {
        uint32_t line = d.line;
        uint32_t col  = d.col;
        uint32_t endCol = (d.span.valid() && d.span.endLine == d.span.line)
                              ? d.span.endCol : (col + 1);

        const char* sevColor = C_RED;
        const char* sevWord  = "error";
        if (d.sev == Sev::Warning)      { sevColor = C_YELLOW; sevWord = "warning"; }
        else if (d.sev == Sev::Note)    { sevColor = C_CYAN;  sevWord = "note"; }
        else if (d.sev == Sev::InternalError) { sevColor = C_RED; sevWord = "internal error"; }

        std::string code;
        if (!d.code.empty() && d.code[0] != 'E') code = "[" + d.code + "]";

        std::string header = path + ":" + std::to_string(line) + ":" +
                             std::to_string(col) + ": " + sevWord + code + ": " +
                             d.message;
        out += esc(C_BOLD) + header + esc(C_RESET) + "\n";

        if (plain) continue;

        std::string text = src.lineText(line);
        out += " " + std::to_string(line) + " | " + text + "\n";
        if (col >= 1 && col <= text.size() + 1) {
            std::string caret;
            size_t width = (endCol > col) ? (size_t)(endCol - col) : 1;
            for (size_t i = 0; i < (size_t)(col - 1); i++)
                caret += (i < text.size() && (unsigned char)text[i] >= 0x80) ? "  " : " ";
            caret += esc(sevColor) + esc(C_BOLD);
            caret += std::string(width < 1 ? 1 : width, '^');
            caret += esc(C_RESET);
            out += "   | " + caret + "\n";
        }

        for (const auto& n : d.notes) {
            std::string ntxt = n.message;
            if (n.span.valid())
                ntxt = (n.message.empty() ? "note" : n.message);
            out += esc(C_CYAN) + " = note: " + esc(C_RESET) + ntxt + "\n";
            if (!plain && n.span.valid() && n.span.line != 0) {
                std::string ntext = src.lineText(n.span.line);
                if (!ntext.empty()) out += " " + std::to_string(n.span.line) + " | " + ntext + "\n";
            }
        }
        for (const auto& f : d.fixIts) {
            out += esc(C_GREEN) + " = help: " + esc(C_RESET)
                 + "replace with `" + f.replacement + "`\n";
        }
    }
}

} // namespace kirn
