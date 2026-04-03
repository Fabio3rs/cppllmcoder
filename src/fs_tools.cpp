#include "fs_tools.hpp"

#include "agent.hpp"
#include "done_task_tool.hpp"
#include "options.hpp"
#include "tool_registry.hpp"

#include <algorithm>
#include <chrono>
#include <expected>
#include <filesystem>
#include <fstream>
#include <sol/sol.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string iso_time(const fs::file_time_type &tp) {
    using namespace std::chrono;
    // Convert to system_clock for portability
    const auto sctp = clock_cast<system_clock>(tp);
    const auto tt = system_clock::to_time_t(sctp);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    char buf[32] = {};
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}

bool starts_with_path(const fs::path &base, const fs::path &target) {
    const auto base_str = base.string();
    const auto target_str = target.string();
    if (base_str.empty())
        return false;
    if (target_str.size() < base_str.size())
        return false;
    if (target_str.compare(0, base_str.size(), base_str) != 0)
        return false;
    if (target_str.size() == base_str.size())
        return true;
    const char sep = fs::path::preferred_separator;
    return target_str[base_str.size()] == sep;
}

class FsToolBase {
  public:
    FsToolBase(std::string root, size_t max_read_bytes)
        : root_(fs::weakly_canonical(fs::path(root))),
          max_read_bytes_(max_read_bytes) {}

  protected:
    std::expected<fs::path, std::string>
    resolve_inside_root(std::string_view rel) const {
        fs::path candidate = root_ / fs::path(rel);
        std::error_code ec;
        fs::path canonical = fs::weakly_canonical(candidate, ec);
        if (ec) {
            return std::unexpected("failed to canonicalize path: " +
                                   ec.message());
        }
        if (!starts_with_path(root_, canonical)) {
            return std::unexpected("path escapes root");
        }
        return canonical;
    }

    std::string rel_from_root(const fs::path &p) const {
        std::error_code ec;
        auto rel = fs::relative(p, root_, ec);
        if (ec) {
            return p.filename().string();
        }
        return rel.lexically_normal().string();
    }

    fs::path root_;
    size_t max_read_bytes_;
};

class FsSizeTool final : public ITool, private FsToolBase {
  public:
    FsSizeTool(std::string root, size_t max_read_bytes)
        : FsToolBase(std::move(root), max_read_bytes) {}

    ToolMetadata describe() const override {
        return ToolMetadata{
            .name = "fs.size",
            .description = "Return file size in bytes",
            .arguments = {{"path", "Path relative to workdir", "string", true}},
            .usage_example = "tools.fs.size('file.txt')",
            .returns = "integer size",
            .danger_tags = {},
            .is_sensitive = false,
            .always_show_in_prompt = true};
    }

    std::expected<sol::object, std::string>
    invoke(const sol::object &lua_args) const override {
        sol::state_view lua(lua_args.lua_state());
        std::string path;
        if (lua_args.is<std::string>()) {
            path = lua_args.as<std::string>();
        } else if (lua_args.is<sol::table>()) {
            sol::table tbl = lua_args.as<sol::table>();
            path = tbl.get_or<std::string>("path", "");
        } else {
            return std::unexpected("expected string or table argument");
        }

        if (path.empty()) {
            return std::unexpected("path is required");
        }

        auto resolved = resolve_inside_root(path);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }

        std::error_code ec;
        if (!fs::exists(*resolved, ec) || ec) {
            return std::unexpected("file does not exist");
        }
        if (!fs::is_regular_file(*resolved, ec) || ec) {
            return std::unexpected("path is not a regular file");
        }

        const auto size = fs::file_size(*resolved, ec);
        if (ec) {
            return std::unexpected("failed to get file size: " + ec.message());
        }

        return sol::make_object(lua, static_cast<double>(size));
    }
};

class FsReadTool final : public ITool, private FsToolBase {
  public:
    FsReadTool(std::string root, size_t max_read_bytes)
        : FsToolBase(std::move(root), max_read_bytes) {}

    ToolMetadata describe() const override {
        return ToolMetadata{
            .name = "fs.read",
            .description = "Read a slice of a file",
            .arguments = {{"path", "Path relative to workdir", "string", true},
                          {"offset", "Byte offset (>=0)", "int", false},
                          {"max_bytes",
                           "Maximum bytes to read (will be capped)", "int",
                           false}},
            .usage_example =
                "tools.fs.read({path='file.txt', offset=0, max_bytes=512})",
            .returns = "table {data, bytes_read, eof}",
            .danger_tags = {},
            .is_sensitive = false,
            .always_show_in_prompt = true};
    }

    std::expected<sol::object, std::string>
    invoke(const sol::object &lua_args) const override {
        sol::state_view lua(lua_args.lua_state());
        std::string path;
        int64_t offset = 0;
        size_t max_bytes_req = 4096;

        if (lua_args.is<std::string>()) {
            path = lua_args.as<std::string>();
        } else if (lua_args.is<sol::table>()) {
            sol::table tbl = lua_args.as<sol::table>();
            path = tbl.get_or<std::string>("path", "");
            offset = tbl.get<sol::optional<int64_t>>("offset").value_or(0);
            if (auto mb = tbl.get<sol::optional<int64_t>>("max_bytes")) {
                if (*mb > 0) {
                    max_bytes_req = static_cast<size_t>(*mb);
                }
            }
        } else {
            return std::unexpected("expected string or table argument");
        }

        if (path.empty()) {
            return std::unexpected("path is required");
        }
        if (offset < 0) {
            return std::unexpected("offset must be non-negative");
        }

        auto resolved = resolve_inside_root(path);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }

        std::error_code ec;
        if (!fs::exists(*resolved, ec) || ec) {
            return std::unexpected("file does not exist");
        }
        if (!fs::is_regular_file(*resolved, ec) || ec) {
            return std::unexpected("path is not a regular file");
        }

        const auto file_size = fs::file_size(*resolved, ec);
        if (ec) {
            return std::unexpected("failed to get file size: " + ec.message());
        }
        if (static_cast<uint64_t>(offset) > file_size) {
            return std::unexpected("offset beyond end of file");
        }

        const size_t to_read = std::min(max_bytes_req, max_read_bytes_);
        std::string buffer;
        buffer.resize(to_read);

        std::ifstream ifs(*resolved, std::ios::binary);
        if (!ifs) {
            return std::unexpected("failed to open file");
        }
        ifs.seekg(offset, std::ios::beg);
        if (!ifs) {
            return std::unexpected("failed to seek");
        }
        ifs.read(buffer.data(), static_cast<std::streamsize>(to_read));
        const auto read_count = static_cast<size_t>(ifs.gcount());
        buffer.resize(read_count);

        const bool eof =
            (static_cast<uint64_t>(offset) + read_count) >= file_size;

        sol::table out = lua.create_table();
        out["data"] = buffer;
        out["bytes_read"] = static_cast<int>(read_count);
        out["eof"] = eof;
        return sol::make_object(lua, out);
    }
};

struct DirEntryRow {
    std::string name;
    std::string kind;
    uintmax_t size = 0;
    std::string mtime;
    std::string relpath;
};

sol::table ensureDirEntryMetatable(sol::state_view lua) {
    const char *kMetaKey = "__direntry_meta";
    if (lua[kMetaKey].valid()) {
        return lua[kMetaKey];
    }

    sol::table meta = lua.create_table();
    meta["is_dir"] = [](const sol::table &self) {
        return self.get_or<std::string>("kind", "") == "dir";
    };
    meta["is_file"] = [](const sol::table &self) {
        return self.get_or<std::string>("kind", "") == "file";
    };
    meta["is_symlink"] = [](const sol::table &self) {
        return self.get_or<std::string>("kind", "") == "symlink";
    };
    meta["type"] = [](const sol::table &self) {
        return self.get_or<std::string>("kind", "");
    };
    meta[sol::meta_function::to_string] = [](const sol::table &self) {
        return self.get_or<std::string>("name", "");
    };

    lua[kMetaKey] = meta;
    return meta;
}

class FsListTool final : public ITool, private FsToolBase {
  public:
    FsListTool(std::string root, size_t max_read_bytes)
        : FsToolBase(std::move(root), max_read_bytes) {}

    ToolMetadata describe() const override {
        return ToolMetadata{
            .name = "fs.ls",
            .description = "List directory contents",
            .arguments = {{"dir", "Directory relative to workdir", "string",
                           false},
                          {"depth", "Recursion depth (>=0)", "int", false},
                          {"include_files", "Include files", "bool", false},
                          {"include_dirs", "Include dirs", "bool", false},
                          {"include_symlinks", "Include symlinks", "bool",
                           false}},
            .usage_example =
                "tools.fs.ls({dir='.', depth=1, include_files=true})",
            .returns = "array of tables (fields: name, kind, size, mtime, "
                       "relpath) with metatable methods is_dir/is_file/"
                       "is_symlink/type() and tostring()->name, sorted",
            .danger_tags = {},
            .is_sensitive = false,
            .always_show_in_prompt = true};
    }

    std::expected<sol::object, std::string>
    invoke(const sol::object &lua_args) const override {
        sol::state_view lua(lua_args.lua_state());
        sol::table direntry_meta = ensureDirEntryMetatable(lua);
        std::string dir = ".";
        int depth = 1;
        bool include_files = true;
        bool include_dirs = true;
        bool include_symlinks = false;

        if (lua_args.is<std::string>()) {
            dir = lua_args.as<std::string>();
        } else if (lua_args.is<sol::table>()) {
            sol::table tbl = lua_args.as<sol::table>();
            dir = tbl.get_or<std::string>("dir", ".");
            depth = tbl.get<sol::optional<int>>("depth").value_or(1);
            include_files =
                tbl.get<sol::optional<bool>>("include_files").value_or(true);
            include_dirs =
                tbl.get<sol::optional<bool>>("include_dirs").value_or(true);
            include_symlinks = tbl.get<sol::optional<bool>>("include_symlinks")
                                   .value_or(false);
        } else if (!lua_args.is<sol::nil_t>()) {
            return std::unexpected("expected string, table or nil argument");
        }

        if (depth < 0) {
            return std::unexpected("depth must be non-negative");
        }

        auto resolved = resolve_inside_root(dir);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }

        std::error_code ec;
        if (!fs::exists(*resolved, ec) || ec) {
            return std::unexpected("directory does not exist");
        }
        if (!fs::is_directory(*resolved, ec) || ec) {
            return std::unexpected("path is not a directory");
        }

        std::vector<DirEntryRow> rows;
        std::error_code iter_ec;
        fs::directory_options opts =
            fs::directory_options::skip_permission_denied;
        fs::recursive_directory_iterator it(*resolved, opts, iter_ec);
        const auto end = fs::recursive_directory_iterator();
        for (; it != end; it.increment(iter_ec)) {
            if (iter_ec) {
                return std::unexpected("iteration error: " + iter_ec.message());
            }
            const int current_depth = it.depth();
            if (current_depth >= depth) {
                it.disable_recursion_pending();
                continue;
            }
            const fs::directory_entry &entry = *it;
            DirEntryRow row;
            const fs::file_status st = entry.symlink_status(iter_ec);
            if (iter_ec) {
                return std::unexpected("status error: " + iter_ec.message());
            }
            const auto kind = st.type();
            if (kind == fs::file_type::directory) {
                if (!include_dirs)
                    continue;
                row.kind = "dir";
            } else if (kind == fs::file_type::regular) {
                if (!include_files)
                    continue;
                row.kind = "file";
            } else if (kind == fs::file_type::symlink) {
                if (!include_symlinks)
                    continue;
                row.kind = "symlink";
            } else {
                // skip other types
                continue;
            }

            row.name = entry.path().filename().string();
            row.relpath = rel_from_root(entry.path());
            if (kind == fs::file_type::regular) {
                row.size = entry.file_size(iter_ec);
                if (iter_ec) {
                    row.size = 0;
                    iter_ec.clear();
                }
            }
            std::error_code time_ec;
            const auto ftime = entry.last_write_time(time_ec);
            if (!time_ec) {
                row.mtime = iso_time(ftime);
            }
            rows.push_back(std::move(row));
        }

        std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
            return a.relpath < b.relpath;
        });

        sol::table arr = lua.create_table(static_cast<int>(rows.size()), 0);
        int idx = 1;
        for (const auto &r : rows) {
            sol::table t = lua.create_table();
            t["name"] = r.name;
            t["kind"] = r.kind;
            t["size"] = static_cast<double>(r.size);
            t["mtime"] = r.mtime;
            t["relpath"] = r.relpath;
            t[sol::metatable_key] = direntry_meta;
            arr[idx++] = t;
        }
        return sol::make_object(lua, arr);
    }
};

} // namespace

void registerFilesystemTools(ToolRegistry &registry, const std::string &root,
                             size_t max_read_bytes) {
    registry.registerTool(std::make_shared<FsListTool>(root, max_read_bytes));
    registry.registerTool(std::make_shared<FsSizeTool>(root, max_read_bytes));
    registry.registerTool(std::make_shared<FsReadTool>(root, max_read_bytes));
}

std::pair<std::shared_ptr<ToolRegistry>, std::shared_ptr<DoneTaskSignal>>
buildDefaultToolRegistry(const app::Options &opts, size_t max_read_bytes) {
    auto reg = std::make_shared<DefaultToolRegistry>();
    registerFilesystemTools(*reg, opts.workdir, max_read_bytes);
    auto signal = registerDoneTaskTool(*reg);
    return {reg, signal};
}
