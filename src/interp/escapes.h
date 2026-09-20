#pragma once
// Shared escape decoding for string/char literal values.
// Single source of truth so the tree-walker and the bytecode-VM emitter
// produce identical runtime values.
#include <cstdint>
#include <cstdlib>
#include <string>

namespace kirn {

// Decodes lexer escape sequences (\n \t \r \\ \' \" \0 \xHH \u{HEX}).
inline std::string decodeEscapes(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '\\' || i + 1 >= in.size()) { out += in[i]; continue; }
        char e = in[++i];
        switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case '0': out += '\0'; break;
            case '\\': out += '\\'; break;
            case '\'': out += '\''; break;
            case '"': out += '"'; break;
            case 'x': {
                if (i + 2 < in.size()) {
                    out += (char)strtol(in.substr(i + 1, 2).c_str(), nullptr, 16);
                    i += 2;
                }
                break;
            }
            case 'u': {
                if (i + 1 < in.size() && in[i + 1] == '{') {
                    size_t close = in.find('}', i + 2);
                    if (close != std::string::npos) {
                        unsigned cp =
                            strtoul(in.substr(i + 2, close - i - 2).c_str(), nullptr, 16);
                        // encode UTF-8
                        if (cp < 0x80) out += (char)cp;
                        else if (cp < 0x800) {
                            out += (char)(0xC0 | (cp >> 6));
                            out += (char)(0x80 | (cp & 0x3F));
                        } else {
                            out += (char)(0xE0 | (cp >> 12));
                            out += (char)(0x80 | ((cp >> 6) & 0x3F));
                            out += (char)(0x80 | (cp & 0x3F));
                        }
                        i = close;
                    }
                }
                break;
            }
            default: out += e;
        }
    }
    return out;
}

inline char32_t decodeCharText(const std::string& in) {
    std::string d = decodeEscapes(in);
    if (d.empty()) return '?';
    unsigned char b0 = (unsigned char)d[0];
    if (b0 < 0x80) return b0;
    if ((b0 & 0xE0) == 0xC0 && d.size() >= 2)
        return ((b0 & 0x1Fu) << 6) | (((unsigned char)d[1]) & 0x3Fu);
    if ((b0 & 0xF0) == 0xE0 && d.size() >= 3)
        return ((b0 & 0x0Fu) << 12) | ((((unsigned char)d[1]) & 0x3Fu) << 6) |
               (((unsigned char)d[2]) & 0x3Fu);
    return b0;
}

} // namespace kirn