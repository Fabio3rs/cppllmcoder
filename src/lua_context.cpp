#include "lua_context.hpp"
#include <iostream>

LuaContext::LuaContext() {
    // Abrimos apenas o essencial para economizar tokens e ganhar segurança
    lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::table,
                       sol::lib::math);

    setup_sandbox();
    register_fs_tools();
    register_db_tools();
    register_agent_tools();
}

void LuaContext::setup_sandbox() {
    // Removemos acesso ao SO e IO nativo do Lua
    lua["os"] = sol::nil;
    lua["io"] = sol::nil;
    lua["load"] = sol::nil;
    lua["package"] = sol::nil;
}

void LuaContext::register_fs_tools() {
    auto fs = lua.create_named_table("fs");

    // Exemplo de ferramenta de leitura cirúrgica (RLM Style)
    fs["read_range"] = []([[maybe_unused]] std::string path, size_t start,
                          [[maybe_unused]] size_t end) -> std::string {
        // Aqui entraria o código C++23 com std::ifstream e spans
        return "[CONTEÚDO DO BINÁRIO NO RANGE " + std::to_string(start) + "]";
    };

    fs["ls"] =
        []([[maybe_unused]] std::string path) -> std::vector<std::string> {
        return {"firmware.bin", "bootloader.asm", "config.json"};
    };
}

void LuaContext::register_db_tools() {
    auto db = lua.create_named_table("db");

    // A ponte para o SQLite que discutimos
    db["get_pointer"] = [](std::string id, sol::this_state s) {
        // O sol::this_state permite criar objetos vinculados ao estado atual da
        // VM
        std::string result =
            "Resumo do ponteiro " + id + ": Tabela de vetores IRQ.";

        // Retornamos como um objeto sol, que o Lua entende perfeitamente
        return sol::make_object(s, result);
    };
}

void LuaContext::register_agent_tools() {
    auto agent = lua.create_named_table("agent");

    agent["spawn"] = [](std::string task) {
        std::cout << "[MOTOR] Spawnando subagente para: " << task << std::endl;
        return "ID_SUB_001";
    };
}

std::expected<std::string, std::string>
LuaContext::execute(std::string_view code) {
    auto result = lua.safe_script(code, sol::script_pass_on_error);

    if (!result.valid()) {
        sol::error err = result;
        return std::unexpected(err.what());
    }

    // Se o script retornar algo (ex: return "OK"), capturamos
    if (result.return_count() > 0) {
        return result.get<std::string>();
    }

    return "Execução concluída com sucesso.";
}
