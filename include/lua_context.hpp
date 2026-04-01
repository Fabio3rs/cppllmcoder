#pragma once
#include <expected>
#include <sol/sol.hpp>
#include <string>
#include <string_view>

class LuaContext {
  public:
    LuaContext();
    ~LuaContext() = default;

    // Executa um bloco de código vindo do LLM
    std::expected<std::string, std::string> execute(std::string_view code);

  private:
    sol::state lua;

    // Injeção de dependências (Tools)
    void setup_sandbox();
    void register_fs_tools();
    void register_db_tools();
    void register_agent_tools();

    // Handlers internos que o Lua chamará
    static std::string lua_read_range(std::string path, size_t start,
                                      size_t end);
};
