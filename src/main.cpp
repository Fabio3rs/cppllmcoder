#include "stdafx.hpp"

#include "exe_path_utils.hpp"
#include "lua_context.hpp"
#include "sqlite3raii.hpp"
#include <memory>
#include <openai/openai.hpp>
#include <print>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
/*
static std::vector<float> embed_text(const std::string &text) {
    auto res = openai::embedding().create(
        {{"model", "qwen3-embedding:8b"}, {"input", text}});

    const auto &emb = res["data"][0]["embedding"];

    std::vector<float> out;
    out.reserve(emb.size());
    for (const auto &x : emb) {
        out.push_back(x.get<float>());
    }
    return out;
}

static std::string to_json_array(const std::vector<float> &v) {
    std::string s;
    s.reserve(v.size() * 12);
    s.push_back('[');

    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i != 0) {
            s.push_back(',');
        }
        s += std::to_string(v[i]);
    }

    s.push_back(']');
    return s;
}

static int dry_run() {
    LuaContext engine;

    // Simulando o que o Qwen/Claude enviaria dentro da tag <code>
    std::string mock_llm_code = R"(
        local files = fs.ls(".")
        print("Arquivos encontrados: " .. #files)

        local info = db.get_pointer("P_42")
        print("DB Info: " .. info)

        if #files > 0 then
            agent.spawn("Analisar primeiro arquivo: " .. files[1])
        end

        return "Script finalizado."
    )";

    auto res = engine.execute(mock_llm_code);

    if (res) {
        std::cout << "Resultado: " << *res << std::endl;
    } else {
        std::cerr << "Erro: " << res.error() << std::endl;
    }

    return 0;
}

static void test_sqlitevec_embd() {
    openai::start("ollama", "", true, "http://localhost:11434/v1/");

    sqlite3_db_ptr db;
    check_sqlite_rc(db, sqlite3_open(":memory:", std::out_ptr(db)),
                    "sqlite3_open");

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

        for (const auto &[id, text] : docs) {
            std::println("Embedding doc id={} text={}", id, text);

            const auto emb = embed_text(text);
            if (emb.size() != 4096) {
                throw std::runtime_error(
                    "unexpected embedding dimension; expected 4096");
            }

            const auto emb_json = to_json_array(emb);

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

        const auto query_emb = embed_text(query);
        if (query_emb.size() != 4096) {
            throw std::runtime_error(
                "unexpected query embedding dimension; expected 4096");
        }

        const auto query_json = to_json_array(query_emb);

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
}
 */
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <string>
#include <vector>

using namespace ftxui;

int main() {
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
