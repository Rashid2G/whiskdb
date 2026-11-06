#include "sql/value_codec.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace database {

namespace {

void WriteU32LE(std::vector<uint8_t>& out, uint32_t val) {
    out.push_back(static_cast<uint8_t>(val & 0xFF));
    out.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
}

void WriteU64LE(std::vector<uint8_t>& out, uint64_t val) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<uint8_t>((val >> (i * 8)) & 0xFF));
    }
}

uint32_t ReadU32LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t ReadU64LE(const uint8_t* p) {
    uint64_t val = 0;
    for (int i = 0; i < 8; ++i) {
        val |= (static_cast<uint64_t>(p[i]) << (i * 8));
    }
    return val;
}

} // namespace

void EncodeValue(std::vector<uint8_t>& out, const Value& v) {
    switch (v.index()) {
        case 0: { // std::monostate (Null)
            out.push_back(static_cast<uint8_t>(ValueType::Null));
            break;
        }
        case 1: { // int64_t
            out.push_back(static_cast<uint8_t>(ValueType::Int64));
            int64_t val = std::get<int64_t>(v);
            uint64_t u_val = 0;
            std::memcpy(&u_val, &val, sizeof(int64_t));
            WriteU64LE(out, u_val);
            break;
        }
        case 2: { // double
            out.push_back(static_cast<uint8_t>(ValueType::Double));
            double val = std::get<double>(v);
            uint64_t u_val = 0;
            std::memcpy(&u_val, &val, sizeof(double));
            WriteU64LE(out, u_val);
            break;
        }
        case 3: { // std::string
            out.push_back(static_cast<uint8_t>(ValueType::Text));
            const auto& s = std::get<std::string>(v);
            WriteU32LE(out, static_cast<uint32_t>(s.size()));
            out.insert(out.end(), s.begin(), s.end());
            break;
        }
        case 4: { // Blob
            out.push_back(static_cast<uint8_t>(ValueType::Blob));
            const auto& b = std::get<Blob>(v);
            WriteU32LE(out, static_cast<uint32_t>(b.size()));
            out.insert(out.end(), b.begin(), b.end());
            break;
        }
        default: {
            D_ASSERT(false && "Unhandled Value index in EncodeValue");
            break;
        }
    }
}

Result<Value> DecodeValue(std::span<const uint8_t>& in) {
    if (in.empty()) {
        return MakeError(ErrorCode::ProtocolError, "Truncated value buffer");
    }

    uint8_t tag = in[0];
    in = in.subspan(1);

    switch (tag) {
        case static_cast<uint8_t>(ValueType::Null): {
            return Value(std::monostate{});
        }
        case static_cast<uint8_t>(ValueType::Int64): {
            if (in.size() < 8) {
                return MakeError(ErrorCode::ProtocolError, "Truncated int64 payload");
            }
            uint64_t u_val = ReadU64LE(in.data());
            int64_t val = 0;
            std::memcpy(&val, &u_val, sizeof(int64_t));
            in = in.subspan(8);
            return Value(val);
        }
        case static_cast<uint8_t>(ValueType::Double): {
            if (in.size() < 8) {
                return MakeError(ErrorCode::ProtocolError, "Truncated double payload");
            }
            uint64_t u_val = ReadU64LE(in.data());
            double val = 0.0;
            std::memcpy(&val, &u_val, sizeof(double));
            in = in.subspan(8);
            return Value(val);
        }
        case static_cast<uint8_t>(ValueType::Text): {
            if (in.size() < 4) {
                return MakeError(ErrorCode::ProtocolError, "Truncated text length");
            }
            uint32_t len = ReadU32LE(in.data());
            in = in.subspan(4);
            if (len > in.size()) {
                return MakeError(ErrorCode::ProtocolError, "Text length exceeds remaining buffer");
            }
            std::string s(reinterpret_cast<const char*>(in.data()), len);
            in = in.subspan(len);
            return Value(std::move(s));
        }
        case static_cast<uint8_t>(ValueType::Blob): {
            if (in.size() < 4) {
                return MakeError(ErrorCode::ProtocolError, "Truncated blob length");
            }
            uint32_t len = ReadU32LE(in.data());
            in = in.subspan(4);
            if (len > in.size()) {
                return MakeError(ErrorCode::ProtocolError, "Blob length exceeds remaining buffer");
            }
            Blob b(in.data(), in.data() + len);
            in = in.subspan(len);
            return Value(std::move(b));
        }
        default: {
            return MakeError(ErrorCode::ProtocolError, "Invalid ValueType tag");
        }
    }
}

void EncodeStatement(std::vector<uint8_t>& out, std::string_view sql, std::span<const Value> params) {
    WriteU32LE(out, static_cast<uint32_t>(sql.size()));
    out.insert(out.end(), sql.begin(), sql.end());

    WriteU32LE(out, static_cast<uint32_t>(params.size()));
    for (const auto& param : params) {
        EncodeValue(out, param);
    }
}

Result<DecodedStatement> DecodeStatement(std::span<const uint8_t> in) {
    if (in.size() < 4) {
        return MakeError(ErrorCode::ProtocolError, "Truncated statement buffer");
    }

    uint32_t sql_len = ReadU32LE(in.data());
    in = in.subspan(4);

    if (sql_len > in.size()) {
        return MakeError(ErrorCode::ProtocolError, "SQL length exceeds remaining buffer");
    }

    std::string sql(reinterpret_cast<const char*>(in.data()), sql_len);
    in = in.subspan(sql_len);

    if (in.size() < 4) {
        return MakeError(ErrorCode::ProtocolError, "Truncated parameter count");
    }

    uint32_t nparams = ReadU32LE(in.data());
    in = in.subspan(4);

    if (nparams > in.size()) {
        return MakeError(ErrorCode::ProtocolError, "Parameter count exceeds remaining buffer bytes");
    }

    std::vector<Value> params;
    params.reserve(std::min<size_t>(nparams, 1024));

    for (uint32_t i = 0; i < nparams; ++i) {
        auto val_res = DecodeValue(in);
        if (!val_res) {
            return MakeError(val_res.error().Code(), val_res.error().Message());
        }
        params.push_back(std::move(*val_res));
    }

    if (!in.empty()) {
        return MakeError(ErrorCode::ProtocolError, "Trailing garbage in statement");
    }

    return DecodedStatement{
        .sql = std::move(sql),
        .params = std::move(params)
    };
}

void EncodeQueryResult(std::vector<uint8_t>& out, const QueryResult& r) {
    WriteU32LE(out, static_cast<uint32_t>(r.columns.size()));
    for (const auto& col : r.columns) {
        WriteU32LE(out, static_cast<uint32_t>(col.size()));
        out.insert(out.end(), col.begin(), col.end());
    }

    WriteU32LE(out, static_cast<uint32_t>(r.rows.size()));
    for (const auto& row : r.rows) {
        for (const auto& val : row) {
            EncodeValue(out, val);
        }
    }

    WriteU64LE(out, r.rows_affected);
    WriteU64LE(out, static_cast<uint64_t>(r.last_insert_rowid));
    WriteU64LE(out, r.exec_us);
    out.push_back(r.is_mutation ? 1 : 0);
}

Result<QueryResult> DecodeQueryResult(std::span<const uint8_t> in) {
    if (in.size() < 4) {
        return MakeError(ErrorCode::ProtocolError, "Truncated QueryResult buffer");
    }

    uint32_t ncols = ReadU32LE(in.data());
    in = in.subspan(4);

    if (ncols > in.size()) {
        return MakeError(ErrorCode::ProtocolError, "Column count exceeds buffer bytes");
    }

    std::vector<std::string> columns;
    columns.reserve(std::min<size_t>(ncols, 1024));

    for (uint32_t i = 0; i < ncols; ++i) {
        if (in.size() < 4) {
            return MakeError(ErrorCode::ProtocolError, "Truncated column name length");
        }
        uint32_t name_len = ReadU32LE(in.data());
        in = in.subspan(4);
        if (name_len > in.size()) {
            return MakeError(ErrorCode::ProtocolError, "Column name length exceeds buffer");
        }
        columns.emplace_back(reinterpret_cast<const char*>(in.data()), name_len);
        in = in.subspan(name_len);
    }

    if (in.size() < 4) {
        return MakeError(ErrorCode::ProtocolError, "Truncated row count");
    }

    uint32_t nrows = ReadU32LE(in.data());
    in = in.subspan(4);

    constexpr size_t TRAILER_SIZE = 8 + 8 + 8 + 1; // rows_affected + last_insert_rowid + exec_us + is_mutation
    if (in.size() < TRAILER_SIZE) {
        return MakeError(ErrorCode::ProtocolError, "Truncated QueryResult trailer");
    }

    size_t available_for_cells = in.size() - TRAILER_SIZE;
    if (ncols > 0 && nrows > 0 && static_cast<uint64_t>(ncols) * nrows > available_for_cells) {
        return MakeError(ErrorCode::ProtocolError, "Row/column cell count exceeds available bytes");
    }

    std::vector<std::vector<Value>> rows;
    rows.reserve(std::min<size_t>(nrows, 1024));

    for (uint32_t r_idx = 0; r_idx < nrows; ++r_idx) {
        std::vector<Value> row;
        row.reserve(ncols);
        for (uint32_t c_idx = 0; c_idx < ncols; ++c_idx) {
            auto val_res = DecodeValue(in);
            if (!val_res) {
                return MakeError(val_res.error().Code(), val_res.error().Message());
            }
            row.push_back(std::move(*val_res));
        }
        rows.push_back(std::move(row));
    }

    if (in.size() < TRAILER_SIZE) {
        return MakeError(ErrorCode::ProtocolError, "Truncated QueryResult trailer");
    }

    idx_t rows_affected = ReadU64LE(in.data());
    in = in.subspan(8);

    int64_t last_insert_rowid = 0;
    uint64_t raw_rowid = ReadU64LE(in.data());
    std::memcpy(&last_insert_rowid, &raw_rowid, sizeof(int64_t));
    in = in.subspan(8);

    uint64_t exec_us = ReadU64LE(in.data());
    in = in.subspan(8);

    bool is_mutation = (in[0] != 0);
    in = in.subspan(1);

    if (!in.empty()) {
        return MakeError(ErrorCode::ProtocolError, "Trailing garbage in QueryResult");
    }

    return QueryResult{
        .columns = std::move(columns),
        .rows = std::move(rows),
        .rows_affected = rows_affected,
        .last_insert_rowid = last_insert_rowid,
        .exec_us = exec_us,
        .is_mutation = is_mutation
    };
}

} // namespace database
