#include "db_tools.hpp"

#include "agent_types.hpp"
#include "lua_args.hpp"
#include "sqlite3.h"
#include "utils/utf8_text.hpp"

#include <algorithm>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace {

// Small RAII wrapper for sqlite3_stmt lifecycle.
struct StmtHolder {
    sqlite3_stmt *stmt = nullptr;
    explicit StmtHolder(sqlite3_stmt *s) : stmt(s) {}
    ~StmtHolder() {
        if (stmt) {
            sqlite3_finalize(stmt);
        }
    }
};

class DbHeadTool final : public ITool {
  public:
    DbHeadTool(sqlite3 *db, size_t max_bytes)
        : db_(db), max_bytes_(max_bytes) {}

    struct Args {
        std::string table;
        int64_t row_id = 0;
        std::string column;
        int64_t offset_bytes = 0;
        size_t bytes_to_read = 4096;
    };

    ToolMetadata describe() const override {
        return ToolMetadata{
            .name = "db.head",
            .description = "Read a UTF-8-safe slice of a TEXT column from a "
                           "row in a DB table",
            .arguments =
                {{"table", "Table name", "string", true},
                 {"row_id", "Row id (INTEGER PRIMARY KEY)", "int", true},
                 {"column", "Column name (TEXT)", "string", true},
                 {"offset_bytes", "Start offset in bytes", "int", false},
                 {"bytes_to_read", "Max UTF-8 bytes to read", "int", false}},
            .usage_example =
                "tools.db.head('messages', 42, 'content', 0, 1024)",
            .returns = "table {data, bytes_read, eof}",
            .tags = {"db", "read", "sqlite", "memory"},
            .danger_tags = {},
            .is_sensitive = false,
            .always_show_in_prompt = true};
    }

    std::expected<sol::object, std::string>
    invoke(sol::variadic_args va, sol::this_state s) const override {
        sol::state_view lua(s);
        auto parsed = parse_pos_or_table<Args>(
            va, FieldSpec{0, "table", &Args::table, std::string{}, true},
            FieldSpec{1, "row_id", &Args::row_id, static_cast<int64_t>(0),
                      true},
            FieldSpec{2, "column", &Args::column, std::string{}, true},
            FieldSpec{3, "offset_bytes", &Args::offset_bytes,
                      static_cast<int64_t>(0), false},
            FieldSpec{4, "bytes_to_read", &Args::bytes_to_read,
                      static_cast<size_t>(4096), false});
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        const auto &args = *parsed;
        if (args.offset_bytes < 0) {
            return std::unexpected("offset_bytes must be >= 0");
        }

        const size_t to_read =
            std::min(args.bytes_to_read, static_cast<size_t>(max_bytes_));

        // Build SQL safely with identifiers whitelisted to avoid injection.
        // For now only allow messages table and TEXT columns.
        if (args.table != "messages") {
            return std::unexpected("only table 'messages' is allowed");
        }
        if (args.column != "content") {
            return std::unexpected("only column 'content' is allowed");
        }

        const char *sql = "SELECT content"
                          "  FROM messages"
                          " WHERE id = ?1";

        sqlite3_stmt *raw = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &raw, nullptr) != SQLITE_OK) {
            return std::unexpected("failed to prepare statement");
        }
        StmtHolder stmt(raw);

        sqlite3_bind_int64(stmt.stmt, 1, args.row_id);

        const int rc = sqlite3_step(stmt.stmt);
        if (rc == SQLITE_DONE) {
            return std::unexpected("row not found");
        }
        if (rc != SQLITE_ROW) {
            return std::unexpected("query failed");
        }

        const auto *text =
            reinterpret_cast<const char *>(sqlite3_column_text(stmt.stmt, 0));
        const int text_size = sqlite3_column_bytes(stmt.stmt, 0);

        std::string_view content;
        if (text && text_size > 0) {
            content = std::string_view(text, static_cast<size_t>(text_size));
        }

        const std::size_t start_offset =
            utf8::align_offset(content, static_cast<size_t>(args.offset_bytes));
        auto chunk = utf8::slice_by_bytes(
            content, static_cast<size_t>(args.offset_bytes), to_read);
        if (chunk.empty() && start_offset < content.size() && to_read > 0) {
            const std::size_t next_offset =
                utf8::align_offset(content, start_offset + 1);
            chunk = content.substr(start_offset, next_offset - start_offset);
        }
        const bool eof = (start_offset + chunk.size()) >= content.size();

        sol::table out = lua.create_table();
        out["data"] = std::string(chunk);
        out["bytes_read"] = chunk.size();
        out["eof"] = eof;
        return sol::make_object(lua, out);
    }

  private:
    sqlite3 *db_;
    size_t max_bytes_;
};

} // namespace

void registerBrainDbTools(ToolRegistry &registry, sqlite3 *db,
                          size_t max_read_bytes) {
    registry.registerTool(std::make_shared<DbHeadTool>(db, max_read_bytes));
}
