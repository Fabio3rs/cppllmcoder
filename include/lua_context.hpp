#pragma once
#include <expected>
#include <functional>
#include <sol/sol.hpp>
#include <string>
#include <string_view>

class ToolRegistry; // Forward declaration to avoid circular include
struct ToolMetadata;
class ITool;

class LuaContext {
  public:
    LuaContext();
    ~LuaContext() = default;

    // Executa um bloco de código vindo do LLM
    std::expected<std::string, std::string> execute(std::string_view code);

    // Faz o binding dinâmico das ferramentas do registro para a tabela Lua.
    // invoker permite que o Agent intercepte consentimento/log antes de chamar
    // a ferramenta; se nulo, chama tool.invoke diretamente.
    void bindTools(const ToolRegistry &registry,
                   std::function<std::expected<sol::object, std::string>(
                       const ToolMetadata &, const ITool &, sol::variadic_args,
                       sol::this_state)>
                       invoker = {});

    // Serialização simples de argumentos Lua -> JSON (restrita, mas suficiente
    // para logging/consent e diagnósticos).
    static std::expected<std::string, std::string>
    luaObjectToJson(const sol::object &obj);

    // Serializa variadic_args como array JSON (limitado a tipos simples),
    // reaproveitando luaObjectToJson para cada elemento.
    static std::expected<std::string, std::string>
    luaVariadicToJson(sol::variadic_args va, sol::this_state s);

  private:
    sol::state lua;
    sol::environment sandbox_env;

    // Injeção de dependências (Tools)
    void setup_sandbox();
};
