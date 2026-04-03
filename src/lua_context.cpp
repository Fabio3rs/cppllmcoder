#include "lua_context.hpp"
#include "agent.hpp"
#include "json_utils.hpp"
#include <algorithm>
#include <cmath>
#include <optional>
#include <print>
#include <unordered_set>
#include <vector>

LuaContext::LuaContext() {
    // Abrimos apenas o essencial para economizar tokens e ganhar segurança
    lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::table,
                       sol::lib::math);

    setup_sandbox();
}

void LuaContext::setup_sandbox() {
    // Removemos acesso ao SO e IO nativo do Lua
    lua["os"] = sol::nil;
    lua["io"] = sol::nil;
    lua["load"] = sol::nil;
    lua["package"] = sol::nil;
    lua["loadfile"] = sol::nil;
    lua["dofile"] = sol::nil;
    lua["collectgarbage"] = sol::nil;
    lua["require"] = sol::nil;

    // Isola o ambiente global visível aos scripts
    sandbox_env = sol::environment(lua, sol::create, lua.globals());
}

void LuaContext::bindTools(
    const ToolRegistry &registry,
    std::function<std::expected<sol::object, std::string>(
        const ToolMetadata &, const ITool &, sol::variadic_args,
        sol::this_state)>
        invoker) {
    auto tools_tbl = lua.create_named_table("tools");

    auto toolInvoker = [&tools_tbl, invoker, this](const ToolMetadata &meta,
                                                   const ITool &tool) {
        auto bound_fn = [&tool, meta, invoker](sol::variadic_args va,
                                               sol::this_state s) {
            constexpr std::string_view prefix =
                "error: tool invocation failed - ";
            try {
                std::expected<sol::object, std::string> result;
                if (invoker) {
                    result = invoker(meta, tool, va, s);
                } else {
                    result = tool.invoke(va, s);
                }
                if (result) {
                    return *result;
                }
                return sol::make_object(s,
                                        std::string(prefix) + result.error());
            } catch (const std::exception &ex) {
                return sol::make_object(s, std::string(prefix) +
                                               "exception: " + ex.what());
            } catch (...) {
                return sol::make_object(s, std::string(prefix) +
                                               "unknown exception");
            }
        };

        // Always expose the flat name (e.g., "fs.read") for backwards
        // compatibility with existing Lua snippets.
        tools_tbl[meta.name] = bound_fn;

        // Also expose top-level functions (no dot) directly in the sandbox
        // environment so scripts can call done_task() without prefix.
        if (meta.name.find('.') == std::string::npos) {
            sandbox_env[meta.name] = bound_fn;
        }

        // Additionally, create nested tables when the tool name contains
        // dots so scripts can call tools.fs.read() OR fs.read().
        if (meta.name.find('.') == std::string::npos)
            return;

        auto build_nested = [&](sol::table root) {
            sol::table current = root;
            size_t start = 0;
            while (true) {
                const auto dot = meta.name.find('.', start);
                const auto part = meta.name.substr(start, dot - start);

                if (dot == std::string::npos) {
                    // Final segment: bind the function.
                    current[part] = bound_fn;
                    break;
                }

                // Intermediate segment: ensure a table exists and descend.
                if (!current[part].valid() ||
                    current[part].get_type() != sol::type::table) {
                    sol::state_view st(current.lua_state());
                    current[part] = st.create_table();
                }
                current = current[part];
                start = dot + 1;
            }
        };

        build_nested(tools_tbl);   // tools.fs.read()
        build_nested(sandbox_env); // fs.read()
    };
    registry.forEach(toolInvoker);
}

std::expected<std::string, std::string>
LuaContext::execute(std::string_view code) {
    auto result = lua.safe_script(code, sandbox_env, sol::script_pass_on_error);

    if (!result.valid()) {
        sol::error err = result;
        return std::unexpected(err.what());
    }

    // Se o script retornar algo (ex: return "OK"), capturamos
    if (result.return_count() > 0) {
        sol::object returned = result.get<sol::object>();
        if (returned.get_type() == sol::type::string) {
            return returned.as<std::string>();
        }

        auto serialized = luaObjectToJson(returned);
        if (!serialized) {
            return std::unexpected(serialized.error());
        }
        return *serialized;
    }

    return "Execução concluída com sucesso.";
}

// --- Utilidades internas ---

// Conversão mínima (string, number, bool, table->JSON object, array-like->
// JSON array). Não cobre userdata/thread/function.
std::expected<std::string, std::string>
LuaContext::luaObjectToJson(const sol::object &obj) {
    constexpr int kMaxDepth = 32;
    std::unordered_set<const void *> visited;

    std::function<std::expected<std::string, std::string>(const sol::object &,
                                                          int)>
        serialize = [&](const sol::object &o,
                        int depth) -> std::expected<std::string, std::string> {
        if (depth > kMaxDepth) {
            return std::unexpected("maximum JSON depth exceeded");
        }

        switch (o.get_type()) {
        case sol::type::string:
            return std::string{"\""} +
                   json_utils::escapeJson(o.as<std::string>()) + "\"";
        case sol::type::number:
            return std::to_string(o.as<double>());
        case sol::type::boolean:
            return o.as<bool>() ? "true" : "false";
        case sol::type::table: {
            sol::table tbl = o;

            const void *ptr = tbl.pointer();
            if (visited.contains(ptr)) {
                return std::unexpected(
                    "cycle detected during JSON serialization");
            }
            visited.insert(ptr);

            size_t numeric_count = 0;
            size_t max_index = 0;
            bool has_string_key = false;
            bool has_invalid_key = false;

            std::vector<std::pair<size_t, sol::object>> numeric_entries;
            std::vector<std::pair<std::string, sol::object>> object_entries;

            for (auto &kv : tbl) {
                const sol::object &key = kv.first;
                const sol::object &value = kv.second;

                if (key.get_type() == sol::type::number) {
                    double as_number = key.as<double>();
                    if (as_number < 1 || std::floor(as_number) != as_number) {
                        has_invalid_key = true;
                        break;
                    }
                    const size_t idx = static_cast<size_t>(as_number);
                    max_index = std::max(max_index, idx);
                    numeric_count++;
                    numeric_entries.emplace_back(idx, value.as<sol::object>());
                } else if (key.get_type() == sol::type::string) {
                    has_string_key = true;
                    object_entries.emplace_back(key.as<std::string>(),
                                                value.as<sol::object>());
                } else {
                    has_invalid_key = true;
                    break;
                }
            }

            if (has_invalid_key) {
                visited.erase(ptr);
                return std::unexpected(
                    "only string keys supported in JSON object mapping");
            }

            const bool is_array = !has_string_key && numeric_count == max_index;

            std::string json;
            if (is_array) {
                std::sort(numeric_entries.begin(), numeric_entries.end(),
                          [](const auto &a, const auto &b) {
                              return a.first < b.first;
                          });
                json += "[";
                bool first = true;
                size_t expected_index = 1;
                for (const auto &entry : numeric_entries) {
                    if (entry.first != expected_index) {
                        visited.erase(ptr);
                        return std::unexpected(
                            "sparse arrays are not supported for JSON mapping");
                    }
                    if (!first) {
                        json += ",";
                    }
                    first = false;
                    auto elem_json = serialize(entry.second, depth + 1);
                    if (!elem_json) {
                        visited.erase(ptr);
                        return std::unexpected(elem_json.error());
                    }
                    json += *elem_json;
                    expected_index++;
                }
                json += "]";
            } else {
                if (!numeric_entries.empty()) {
                    visited.erase(ptr);
                    return std::unexpected("mixed or numeric keys are not "
                                           "supported in JSON objects");
                }
                std::sort(object_entries.begin(), object_entries.end(),
                          [](const auto &a, const auto &b) {
                              return a.first < b.first;
                          });

                json += "{";
                bool first = true;
                for (const auto &entry : object_entries) {
                    if (!first) {
                        json += ",";
                    }
                    first = false;
                    json += "\"" + json_utils::escapeJson(entry.first) + "\":";
                    auto val_json = serialize(entry.second, depth + 1);
                    if (!val_json) {
                        visited.erase(ptr);
                        return std::unexpected(val_json.error());
                    }
                    json += *val_json;
                }
                json += "}";
            }

            visited.erase(ptr);
            return json;
        }
        case sol::type::nil:
            return "null";
        default: {
            // Tenta converter via tostring (suporta __tostring de metatables)
            sol::state_view lua(o.lua_state());
            auto maybe_tostring =
                lua["tostring"].get<std::optional<sol::protected_function>>();
            if (maybe_tostring) {
                sol::protected_function_result r = (*maybe_tostring)(o);
                if (r.valid()) {
                    sol::object s = r.get<sol::object>();
                    if (s.get_type() == sol::type::string) {
                        return std::string{"\""} +
                               json_utils::escapeJson(s.as<std::string>()) +
                               "\"";
                    }
                } else {
                    r.abandon();
                }
            }
            return std::unexpected(
                "unsupported Lua type for JSON serialization");
        }
        }
    };

    return serialize(obj, 0);
}

std::expected<std::string, std::string>
LuaContext::luaVariadicToJson(sol::variadic_args va, sol::this_state s) {
    sol::state_view lua(s);
    sol::stack_guard guard(lua); // keep stack balanced while we inspect args
    try {
        std::string json = "[";
        bool first = true;
        for (auto v : va) {
            sol::object obj(v.lua_state(), v.stack_index());
            auto item = luaObjectToJson(obj);
            if (!item) {
                return std::unexpected(item.error());
            }
            if (!first) {
                json += ",";
            }
            first = false;
            json += *item;
        }
        json += "]";
        return json;
    } catch (const std::exception &ex) {
        return std::unexpected(std::string{"variadic_to_json exception: "} +
                               ex.what());
    }
}

// escape_json moved to json_utils.hpp
