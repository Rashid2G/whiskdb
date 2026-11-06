#include "client/table_format.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <format>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace database {

namespace {

idx_t Utf8CodePointCount(std::string_view s) {
    idx_t count = 0;
    for (char c : s) {
        if ((static_cast<uint8_t>(c) & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

std::string TruncateUtf8(std::string_view s, idx_t max_code_points) {
    idx_t count = 0;
    idx_t byte_idx = 0;
    while (byte_idx < s.size()) {
        if ((static_cast<uint8_t>(s[byte_idx]) & 0xC0) != 0x80) {
            if (count == max_code_points) {
                break;
            }
            ++count;
        }
        ++byte_idx;
    }
    return std::string(s.substr(0, byte_idx));
}

std::string EscapeAndCap(std::string_view s) {
    std::string escaped;
    escaped.reserve(s.size());
    for (char c : s) {
        if (c == '\n') {
            escaped += "\\n";
        } else if (c == '\r') {
            escaped += "\\r";
        } else {
            escaped += c;
        }
    }
    constexpr idx_t MAX_CELL_CHARS = 60;
    idx_t cp_count = Utf8CodePointCount(escaped);
    if (cp_count > MAX_CELL_CHARS) {
        escaped = TruncateUtf8(escaped, 57) + "...";
    }
    return escaped;
}

struct FormattedCell {
    std::string text;
    bool is_number = false;
    idx_t display_width = 0;
};

FormattedCell FormatCellValue(const Value& val) {
    FormattedCell cell;
    std::visit([&](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
            cell.text = "NULL";
            cell.is_number = false;
        } else if constexpr (std::is_same_v<T, int64_t>) {
            cell.text = std::to_string(v);
            cell.is_number = true;
        } else if constexpr (std::is_same_v<T, double>) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.15g", v);
            cell.text = buf;
            cell.is_number = true;
        } else if constexpr (std::is_same_v<T, std::string>) {
            cell.text = EscapeAndCap(v);
            cell.is_number = false;
        } else if constexpr (std::is_same_v<T, Blob>) {
            std::string hex;
            hex += "x'";
            idx_t bytes_to_show = std::min<idx_t>(v.size(), 16);
            for (idx_t i = 0; i < bytes_to_show; ++i) {
                hex += std::format("{:02x}", v[i]);
            }
            if (v.size() > 16) {
                hex += "...";
            }
            hex += "'";
            cell.text = EscapeAndCap(hex);
            cell.is_number = false;
        }
    }, val);

    cell.display_width = Utf8CodePointCount(cell.text);
    return cell;
}

} // namespace

std::string FormatResultTable(const QueryResult& qr) {
    if (qr.columns.empty()) {
        return std::format("OK, {} row(s) affected (last insert rowid {}) in {:.2f} ms\n",
                           qr.rows_affected,
                           qr.last_insert_rowid,
                           static_cast<double>(qr.exec_us) / 1000.0);
    }

    const idx_t num_cols = qr.columns.size();
    std::vector<FormattedCell> header_cells;
    header_cells.reserve(num_cols);
    std::vector<idx_t> col_widths(num_cols, 1);

    for (idx_t c = 0; c < num_cols; ++c) {
        FormattedCell h;
        h.text = EscapeAndCap(qr.columns[c]);
        h.is_number = false;
        h.display_width = Utf8CodePointCount(h.text);
        col_widths[c] = std::max(col_widths[c], h.display_width);
        header_cells.push_back(std::move(h));
    }

    std::vector<std::vector<FormattedCell>> formatted_rows;
    formatted_rows.reserve(qr.rows.size());
    for (const auto& row : qr.rows) {
        std::vector<FormattedCell> frow;
        frow.reserve(num_cols);
        for (idx_t c = 0; c < num_cols; ++c) {
            FormattedCell fcell;
            if (c < row.size()) {
                fcell = FormatCellValue(row[c]);
            } else {
                fcell = FormatCellValue(std::monostate{});
            }
            col_widths[c] = std::max(col_widths[c], fcell.display_width);
            frow.push_back(std::move(fcell));
        }
        formatted_rows.push_back(std::move(frow));
    }

    auto make_border = [&]() -> std::string {
        std::string b;
        for (idx_t c = 0; c < num_cols; ++c) {
            b += '+';
            b.append(col_widths[c] + 2, '-');
        }
        b += "+\n";
        return b;
    };

    auto format_row = [&](const std::vector<FormattedCell>& cells) -> std::string {
        std::string r;
        for (idx_t c = 0; c < num_cols; ++c) {
            r += "| ";
            idx_t pad = col_widths[c] > cells[c].display_width ? (col_widths[c] - cells[c].display_width) : 0;
            if (cells[c].is_number) {
                r.append(pad, ' ');
                r += cells[c].text;
            } else {
                r += cells[c].text;
                r.append(pad, ' ');
            }
            r += " ";
        }
        r += "|\n";
        return r;
    };

    std::string out;
    std::string border = make_border();
    out += border;
    out += format_row(header_cells);
    out += border;
    for (const auto& row : formatted_rows) {
        out += format_row(row);
    }
    if (!formatted_rows.empty()) {
        out += border;
    }
    out += std::format("{} row(s) in {:.2f} ms\n",
                       qr.rows.size(),
                       static_cast<double>(qr.exec_us) / 1000.0);
    return out;
}

std::vector<std::string> SplitStatements(std::string& buffer) {
    std::vector<std::string> statements;
    const idx_t n = buffer.size();
    idx_t stmt_start = 0;
    bool in_single_quote = false;
    idx_t last_consumed_end = 0;

    for (idx_t i = 0; i < n; ++i) {
        char c = buffer[i];
        if (c == '\'') {
            if (in_single_quote) {
                if (i + 1 < n && buffer[i + 1] == '\'') {
                    ++i; // skip escaped single quote ''
                } else {
                    in_single_quote = false;
                }
            } else {
                in_single_quote = true;
            }
        } else if (c == ';' && !in_single_quote) {
            idx_t stmt_len = i - stmt_start + 1;
            std::string_view sv(buffer.data() + stmt_start, stmt_len);

            while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) {
                sv.remove_prefix(1);
            }

            bool has_content = false;
            for (char ch : sv) {
                if (ch != ';' && !std::isspace(static_cast<unsigned char>(ch))) {
                    has_content = true;
                    break;
                }
            }
            if (has_content) {
                statements.emplace_back(sv);
            }

            last_consumed_end = i + 1;
            stmt_start = i + 1;
        }
    }

    if (last_consumed_end > 0) {
        std::string remainder = buffer.substr(last_consumed_end);
        idx_t rem_start = 0;
        while (rem_start < remainder.size() && std::isspace(static_cast<unsigned char>(remainder[rem_start]))) {
            ++rem_start;
        }
        buffer = remainder.substr(rem_start);
    }

    return statements;
}

} // namespace database
