#include "stdafx.hpp"

#include "cli_options.hpp"
#include "embedding_utils.hpp"
#include "exe_path_utils.hpp"
#include "lua_context.hpp"
#include "sqlite3raii.hpp"
#include <memory>
#include <openai/openai.hpp>
#include <print>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

/*
static int dry_run() {
    LuaContext engine;

    // Simulando o que o Qwen/Claude enviaria dentro da tag <code>
    std::string mock_llm_code = R"lua(
        local files = fs.ls(".")
        print("Arquivos encontrados: " .. #files)

        local info = db.get_pointer("P_42")
        print("DB Info: " .. info)

        if #files > 0 then
            agent.spawn("Analisar primeiro arquivo: " .. files[1])
        end

        return "Script finalizado."
    )lua";

    auto res = engine.execute(mock_llm_code);

    if (res) {
        std::cout << "Resultado: " << *res << std::endl;
    } else {
        std::cerr << "Erro: " << res.error() << std::endl;
    }

    return 0;
}

static nlohmann::json::array_t
docs_from_id_vec(const std::span<const std::pair<int, std::string>> vec) {
    nlohmann::json::array_t arr;
    arr.reserve(vec.size());
    for (const auto &[id, content] : vec) {
        arr.push_back(content);
    }
    return arr;
}

static void test_sqlitevec_embd() {
    auto &llmConnection =
        openai::start("ollama", "", true, "http://localhost:11434/v1/");

    sqlite3_db_ptr db;
    check_sqlite_rc(db, sqlite3_open(":memory:", std::out_ptr(db)),
                    "sqlite3_open");

    std::string embedding_model = "qwen3-embedding:8b";

    try {
        check_sqlite_rc(db, sqlite3_enable_load_extension(db.get(), 1),
                        "enable load_extension");

        sqlite3_errmsg_ptr errmsg = nullptr;
        const std::string vec0_path = exe_path_utils::get_vec_extension_path();

        const int load_rc = sqlite3_load_extension(
            db.get(), vec0_path.c_str(), nullptr, std::out_ptr(errmsg));
        if (load_rc != SQLITE_OK) {
            std::string msg = "sqlite3_load_extension: ";
            msg += (errmsg ? errmsg.get() : "unknown error");
            throw std::runtime_error(msg);
        }

        check_sqlite_rc(db, sqlite3_enable_load_extension(db.get(), 0),
                        "disable load_extension");

        exec_or_throw(db, R"sql(
            create table docs(
                id integer primary key,
                content text not null
            );
        )sql");

        exec_or_throw(db, R"sql(
            create virtual table vec_docs using vec0(
                id integer primary key,
                embedding float[4096]
            );
        )sql");

        const std::vector<std::pair<int, std::string>> docs = {
            {1, "C++ templates, concepts and constexpr metaprogramming"},
            {2, "Lua bindings for C++ with sol2 and userdata exposure"},
            {3, "SQLite vector search with embeddings and semantic retrieval"},
            {4, "Rust ownership, borrowing and lifetimes"}};

        auto insert_doc_stmt = prepare_or_throw(db,
                                                R"sql(
                insert into docs(id, content) values(?, ?);
            )sql",
                                                "prepare insert docs");

        auto insert_vec_stmt = prepare_or_throw(db,
                                                R"sql(
                insert into vec_docs(id, embedding) values(?, ?);
            )sql",
                                                "prepare insert vec_docs");

        auto search_stmt = prepare_or_throw(db,
                                            R"sql(
                select d.id, d.content, v.distance
                from vec_docs as v
                join docs as d on d.id = v.id
                where v.embedding match ?
                  and k = 3
                order by v.distance asc;
            )sql",
                                            "prepare search");

        auto embeds = embedding_utils::embed_texts(
            llmConnection, embedding_model, docs_from_id_vec(docs));

        for (const auto &[doc, emb] : std::ranges::zip_view(docs, embeds)) {
            auto &[id, text] = doc;
            std::println("Embedding doc id={} text={}", id, text);

            if (emb.size() != 4096) {
                throw std::runtime_error(
                    "unexpected embedding dimension; expected 4096");
            }

            const auto emb_json = embedding_utils::to_json_array(emb);

            check_sqlite_rc(db, sqlite3_bind_int(insert_doc_stmt.get(), 1, id),
                            "bind doc id");
            check_sqlite_rc(db,
                            sqlite3_bind_text(insert_doc_stmt.get(), 2,
                                              text.c_str(), -1,
                                              SQLITE_TRANSIENT),
                            "bind doc content");
            check_sqlite_rc(db, sqlite3_step(insert_doc_stmt.get()),
                            "step insert docs");
            check_sqlite_rc(db, sqlite3_reset(insert_doc_stmt.get()),
                            "reset insert docs");
            check_sqlite_rc(db, sqlite3_clear_bindings(insert_doc_stmt.get()),
                            "clear insert docs");

            check_sqlite_rc(db, sqlite3_bind_int(insert_vec_stmt.get(), 1, id),
                            "bind vec id");
            check_sqlite_rc(db,
                            sqlite3_bind_text(insert_vec_stmt.get(), 2,
                                              emb_json.c_str(), -1,
                                              SQLITE_TRANSIENT),
                            "bind vec embedding");
            check_sqlite_rc(db, sqlite3_step(insert_vec_stmt.get()),
                            "step insert vec_docs");
            check_sqlite_rc(db, sqlite3_reset(insert_vec_stmt.get()),
                            "reset insert vec_docs");
            check_sqlite_rc(db, sqlite3_clear_bindings(insert_vec_stmt.get()),
                            "clear insert vec_docs");
        }

        const std::string query =
            "How to integrate Lua into a modern C++ project?";
        std::println("\nQuery: {}", query);

        const auto query_emb =
            embedding_utils::embed_text(llmConnection, embedding_model, query);
        if (query_emb.size() != 4096) {
            throw std::runtime_error(
                "unexpected query embedding dimension; expected 4096");
        }

        const auto query_json = embedding_utils::to_json_array(query_emb);

        check_sqlite_rc(db,
                        sqlite3_bind_text(search_stmt.get(), 1,
                                          query_json.c_str(), -1,
                                          SQLITE_TRANSIENT),
                        "bind search query");

        std::println("\nTop matches:");
        while (true) {
            const int rc = sqlite3_step(search_stmt.get());
            if (rc == SQLITE_DONE) {
                break;
            }
            check_sqlite_rc(db, rc, "step search");

            const int id = sqlite3_column_int(search_stmt.get(), 0);
            const char *content = reinterpret_cast<const char *>(
                sqlite3_column_text(search_stmt.get(), 1));
            const double distance = sqlite3_column_double(search_stmt.get(), 2);

            std::println("id={} distance={} text={}", id, distance,
                         content != nullptr ? content : "");
        }

    } catch (...) {
        throw;
    }
}

int main() {
    dry_run();
    test_sqlitevec_embd();
}*/

// Esboço do Loop Principal na PoC
#include "agents/agent_action.hpp"
#include "lua_context.hpp"

int main(int argc, char *argv[]) {
    // 1. Parse CLI Options
    auto parser = app::create_parser();
    auto result = parser.parse(argc, argv);

    switch (result.status) {
    case cli::ParseStatus::ShowHelp:
        std::cout << parser.generate_help(argv[0]);
        return 0;

    case cli::ParseStatus::ShowHelpVerbose:
        std::cout << parser.generate_help_verbose(argv[0]);
        return 0;

    case cli::ParseStatus::ShowVersion:
        std::print("CPP-LLM-CODER v0.1.0-alpha\n");
        return 0;

    case cli::ParseStatus::ShowCompletion:
        // Completion já foi tratada internamente, apenas sai
        return 0;

    case cli::ParseStatus::Error:
        std::cerr << "Erro: " << result.error_message << "\n";
        std::cerr << "Use --help para ver as opções disponíveis.\n";
        return 1;

    case cli::ParseStatus::Ok:
        break;
    }

    auto &cfg = *result.config;

    // 2. Init Core
    LuaContext lua;
    auto &openai = openai::start("ollama", "", true, cfg.endpoint);

    std::string user_input;
    std::string system_context =
        R"(Você é o CPP-LLM-CODER. Você opera via scripts LUA v5.4.
Sempre que precisar interagir com o sistema (arquivos, banco de dados, busca vetorial),
use o seguinte formato:

Pensamento: [Seu raciocínio aqui]
<code>
  -- Seu script Lua aqui
  local result = fs.ls(".")
  return result[1]
</code>

Ferramentas disponíveis (funções Lua):
- fs.read_range(path, start, size)
- db.search_vector(query, k)
- db.add_pointer(id, summary, metadata)
- agent.spawn(task_description)
)";

    while (true) {
        std::print("\n[User]> ");
        if (!std::getline(std::cin, user_input) || user_input == "exit")
            break;

        // Turno do Agente
        std::println("\n[Pensando...]");
        nlohmann::json request;
        request["model"] = cfg.model;
        request["messages"] = {
            {{"role", "system"}, {"content", system_context}},
            {{"role", "user"}, {"content", user_input}}};

        // Dica de Debug: Se quiser ver exatamente o que está sendo enviado
        std::println("Payload: {}", request.dump(2));

        // Chamada ao Ollama
        auto chat = openai.chat.create(request);

        std::string response = chat["choices"][0]["message"]["content"];

        // 3. Extração e Execução
        auto action = AgentAction::parse(response);
        std::println("\n[🤖 Pensamento]: {}", action.thought);

        if (action.has_code) {
            std::println("\n[🛠️ Executando Lua]:\n{}", action.lua_code);

            auto lua_res = lua.execute(action.lua_code);

            if (lua_res) {
                std::println("\n[✅ Retorno Lua]: {}", *lua_res);
                // Aqui você reinjetaria o retorno no próximo turno se quiser
                // automação
            } else {
                std::println("\n[❌ Erro Lua]: {}", lua_res.error());
            }
        }
    }
}

/*#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <string>
#include <vector>

using namespace ftxui;

int main(int argc, char **argv) {
    auto parser = app::create_parser();
    auto result = parser.parse(argc, argv);

    switch (result.status) {
    case cli::ParseStatus::ShowHelp:
        std::cout << parser.generate_help(argv[0]);
        return 0;

    case cli::ParseStatus::ShowHelpVerbose:
        std::cout << parser.generate_help_verbose(argv[0]);
        return 0;

    case cli::ParseStatus::ShowVersion:
        std::print("CPP-LLM-CODER v0.1.0-alpha\n");
        return 0;

    case cli::ParseStatus::ShowCompletion:
        // Completion já foi tratada internamente, apenas sai
        return 0;

    case cli::ParseStatus::Error:
        std::cerr << "Erro: " << result.error_message << "\n";
        std::cerr << "Use --help para ver as opções disponíveis.\n";
        return 1;

    case cli::ParseStatus::Ok:
        break;
    }

    std::vector<std::string> logs = {"SISTEMA: Motor C++23 inicializado.",
                                     "LUA: VM carregada com sucesso.",
                                     "DB: Conectado a .cppllmcoder/brain.db"};

    std::vector<std::string> tasks = {"[X] Init DB", "[ ] Analisar Firmware",
                                      "[ ] Extrair Símbolos"};

    std::string input_value;
    auto screen = ScreenInteractive::Fullscreen();

    Component input_field =
        Input(&input_value, " Digita um comando (ex: scan)...");

    input_field = CatchEvent(input_field, [&](Event event) {
        if (event == Event::Return) {
            if (!input_value.empty()) {
                logs.push_back("USER: " + input_value);
                logs.push_back("MOTOR: Executando script Lua para '" +
                               input_value + "'...");
                input_value.clear();
                return true;
            }
        }
        return false;
    });

    auto renderer = Renderer(input_field, [&] {
        Elements log_elements;
        for (const auto &log : logs) {
            log_elements.push_back(text(log));
        }

        Elements task_elements;
        for (const auto &task : tasks) {
            task_elements.push_back(text(task) |
                                    color(task.size() > 1 && task[1] == 'X'
                                              ? Color::Green
                                              : Color::Yellow));
        }

        Element header =
            hbox({
                text("   CPP-LLM-CODER ") | bold | color(Color::Cyan),
                filler(),
                text("v0.1.0-alpha ") | dim,
            }) |
            border;

        Element tasks_panel =
            window(text(" Tarefas "),
                   vbox(task_elements) | size(WIDTH, EQUAL, 25), LIGHT);

        Element logs_panel =
            window(text(" Log de Execução "), vbox(log_elements) | flex, LIGHT);

        Element body = hbox({
                           tasks_panel,
                           logs_panel,
                       }) |
                       flex;

        Element footer = hbox({
                             text(" ❯ ") | bold | color(Color::Magenta),
                             input_field->Render() | flex,
                         }) |
                         border;

        return vbox({
            header,
            body,
            footer,
        });
    });

    screen.Loop(renderer);
    return 0;
}
*/
