#pragma once

#include <expected>
#include <optional>
#include <sol/sol.hpp>
#include <string>
#include <type_traits>

// FieldSpec define como ler um campo de Args tanto por posição quanto por
// chave de tabela. O tipo é deduzido do ponteiro-para-membro.
template <class Args, class Member> struct FieldSpec {
    size_t pos;           // índice posicional (0-based)
    const char *key;      // chave na table { key = ... }
    Member Args::*member; // ponteiro para membro tipado
    Member default_value{};
    bool required = false;
};

// CTAD guide
template <class Args, class Member>
FieldSpec(size_t, const char *, Member Args::*, Member,
          bool) -> FieldSpec<Args, Member>;
template <class Args, class Member>
FieldSpec(size_t, const char *, Member Args::*,
          Member) -> FieldSpec<Args, Member>;

// parse_pos_or_table converte variadic_args em um Args, aceitando tanto
// chamada posicional (fs.ls(".", 2, true)) quanto table (fs.ls{dir="."}).
// Usa sol::stack::check_get para mensagens de erro melhores.
template <class Args, class... Specs>
std::expected<Args, std::string> parse_pos_or_table(sol::variadic_args va,
                                                    Specs... specs) {
    Args out{};
    // defaults
    ((out.*(specs.member) = specs.default_value), ...);

    // Table case: um único argumento tabela
    if (va.size() == 1 && va[0].is<sol::table>()) {
        sol::table t = va[0];
        auto read = [&](auto &spec) -> std::optional<std::string> {
            using T = std::remove_reference_t<decltype(out.*(spec.member))>;
            auto opt = t.get<sol::optional<T>>(spec.key);
            if (opt) {
                out.*(spec.member) = *opt;
                return std::nullopt;
            }
            if (spec.required) {
                return std::string("missing field: ") + spec.key;
            }
            return std::nullopt;
        };
        std::optional<std::string> err;
        auto visit = [&](auto &spec) {
            if (err)
                return;
            err = read(spec);
        };
        (visit(specs), ...);
        if (err) {
            return std::unexpected(*err);
        }
        return out;
    }

    // Posicional
    auto get = [&](size_t i) -> sol::object {
        if (i >= va.size()) {
            return sol::object{};
        }
        auto it = va.begin();
        std::advance(it, static_cast<std::ptrdiff_t>(i));
        return sol::object(*it);
    };
    auto read_pos = [&](auto &spec) -> std::optional<std::string> {
        if (spec.pos >= va.size()) {
            if (spec.required) {
                return std::string("missing positional arg at index ") +
                       std::to_string(spec.pos);
            }
            return std::nullopt;
        }
        sol::object o = get(spec.pos);
        using T = std::remove_reference_t<decltype(out.*(spec.member))>;
        if (!o.is<T>()) {
            return std::string("type mismatch at index ") +
                   std::to_string(spec.pos);
        }
        out.*(spec.member) = o.as<T>();
        return std::nullopt;
    };
    std::optional<std::string> err;
    auto visit = [&](auto &spec) {
        if (err)
            return;
        err = read_pos(spec);
    };
    (visit(specs), ...);
    if (err) {
        return std::unexpected(*err);
    }
    return out;
}
