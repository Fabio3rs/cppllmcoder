#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>
#include <string_view>

// UTF-8 decoder conforme Unicode Standard §3.9 "Best Practices for Using
// U+FFFD"
//
// Política de erro: 1 byte consumido por byte inválido, U+FFFD emitido.
// Isso segue a recomendação de "substitution of maximal subparts" do padrão
// Unicode — nunca engole bytes que poderiam iniciar um codepoint válido.
//
// Rejeita:
//   - continuação bytes soltos
//   - sequências truncadas (boundary check obrigatório via parâmetro 'end')
//   - overlong encodings
//   - surrogates (U+D800–U+DFFF)
//   - valores acima de U+10FFFF

namespace utf8 {

static constexpr uint32_t REPLACEMENT_CHAR = 0xFFFD;
static constexpr uint32_t UNICODE_MAX = 0x10FFFF;

struct CodepointResult {
    uint32_t codepoint; // valor Unicode decodificado, ou REPLACEMENT_CHAR
    unsigned size;      // bytes avançados no buffer (sempre >= 1)
};

// 'end' é obrigatório — não existe versão sem boundary check.
inline CodepointResult decode(const char *p, const char *end) noexcept {
    auto byte = [](const char *c) -> unsigned char {
        return static_cast<unsigned char>(*c);
    };
    auto isCont = [&](const char *c) -> bool {
        return (byte(c) & 0xC0u) == 0x80u;
    };

    if (!p || p >= end)
        return {0, 0};

    unsigned char c = byte(p);

    // ── ASCII ────────────────────────────────────────────────────────────────
    if (c < 0x80u)
        return {c, 1};

    // ── 2-byte: 110xxxxx 10xxxxxx ────────────────────────────────────────────
    if ((c & 0xE0u) == 0xC0u) {
        if (p + 1 >= end || !isCont(p + 1))
            return {REPLACEMENT_CHAR, 1};
        uint32_t cp = ((c & 0x1Fu) << 6) | (byte(p + 1) & 0x3Fu);
        if (cp < 0x80u)
            return {REPLACEMENT_CHAR, 1}; // overlong → avança 1 byte
        return {cp, 2};
    }

    // ── 3-byte: 1110xxxx 10xxxxxx 10xxxxxx ───────────────────────────────────
    if ((c & 0xF0u) == 0xE0u) {
        if (p + 1 >= end || !isCont(p + 1))
            return {REPLACEMENT_CHAR, 1};
        if (p + 2 >= end || !isCont(p + 2))
            return {REPLACEMENT_CHAR, 1};
        uint32_t cp = ((c & 0x0Fu) << 12) | ((byte(p + 1) & 0x3Fu) << 6) |
                      (byte(p + 2) & 0x3Fu);
        if (cp < 0x0800u)
            return {REPLACEMENT_CHAR, 1}; // overlong
        if (cp >= 0xD800u && cp <= 0xDFFFu)
            return {REPLACEMENT_CHAR, 1}; // surrogate
        return {cp, 3};
    }

    // ── 4-byte: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
    // ───────────────────────────
    if ((c & 0xF8u) == 0xF0u) {
        if (p + 1 >= end || !isCont(p + 1))
            return {REPLACEMENT_CHAR, 1};
        if (p + 2 >= end || !isCont(p + 2))
            return {REPLACEMENT_CHAR, 1};
        if (p + 3 >= end || !isCont(p + 3))
            return {REPLACEMENT_CHAR, 1};
        uint32_t cp = ((c & 0x07u) << 18) | ((byte(p + 1) & 0x3Fu) << 12) |
                      ((byte(p + 2) & 0x3Fu) << 6) | (byte(p + 3) & 0x3Fu);
        if (cp < 0x10000u)
            return {REPLACEMENT_CHAR, 1}; // overlong
        if (cp > UNICODE_MAX)
            return {REPLACEMENT_CHAR, 1}; // fora do Unicode
        return {cp, 4};
    }

    // Byte inválido (continuation byte solto, 0xFE ou 0xFF)
    return {REPLACEMENT_CHAR, 1};
}

// ─────────────────────────────────────────────────────────────────────────────

struct iterator {
    const char *ptr;
    const char *end_ptr;

    constexpr iterator(const char *p, const char *e) noexcept
        : ptr(p), end_ptr(e) {}

    explicit iterator(std::string_view sv) noexcept
        : iterator(sv.data(), sv.data() + sv.size()) {}

    explicit iterator(const std::string &s) noexcept
        : iterator(s.data(), s.data() + s.size()) {}

    constexpr iterator(const iterator &) = default;
    constexpr iterator &operator=(const iterator &) = default;

    constexpr iterator begin() const noexcept { return *this; }
    constexpr iterator end() const noexcept { return {end_ptr, end_ptr}; }

    // Retorna a string_view dos bytes crus do codepoint atual.
    [[nodiscard]] std::string_view raw_bytes() const noexcept {
        if (ptr >= end_ptr)
            return {ptr, 0};
        auto [cp, sz] = decode(ptr, end_ptr);
        return {ptr, sz};
    }

    // ── Iterator traits (input iterator) ─────────────────────────────────────
    using difference_type = std::ptrdiff_t;
    using value_type = uint32_t;
    using pointer = const uint32_t *;
    using reference = uint32_t; // retornado por valor — sem referência pendente
    using iterator_category = std::input_iterator_tag;

    // Conta codepoints. O(n) — não chame em loop crítico.
    // [[nodiscard]] para evitar chamada acidental sem uso do resultado.
    [[nodiscard]] std::ptrdiff_t count_codepoints() const noexcept { // O(n)
        return std::distance(begin(), end());
    }

    constexpr iterator &operator++() noexcept {
        if (ptr < end_ptr) {
            auto [cp, sz] = decode(ptr, end_ptr);
            ptr += sz; // sz >= 1 garantido por decode()
        }
        return *this;
    }

    constexpr iterator operator++(int) noexcept {
        auto copy = *this;
        ++(*this);
        return copy;
    }

    // Contrato: só compare iteradores do mesmo range.
    // Compara apenas ptr (semantica de input_iterator, igual a
    // std::istream_iterator). Dois end() de ranges distintos com mesmo end_ptr
    // são iguais — comportamento correto para o uso pretendido (loops range-for
    // e std::distance).
    constexpr bool operator==(const iterator &o) const noexcept {
        return ptr == o.ptr;
    }
    constexpr bool operator!=(const iterator &o) const noexcept {
        return ptr != o.ptr;
    }

    constexpr uint32_t operator*() const noexcept {
        if (ptr >= end_ptr)
            return 0;
        return decode(ptr, end_ptr).codepoint;
    }
};

// Retorna uma string_view dos bytes correspondentes a
// 'count' codepoints a partir do índice 'pos' (base 0).
// Complexidade: O(pos + count) — percorre o range uma vez.
// Retorna string_view vazia se pos >= tamanho total.
[[nodiscard]] inline std::string_view
substr(std::string_view utf8str, std::size_t pos,
       std::size_t count = std::string_view::npos) noexcept {
    const char *base = utf8str.data();
    const char *end = base + utf8str.size();

    // Avança até 'pos'
    const char *start_ptr = base;
    for (std::size_t i = 0; i < pos; ++i) {
        if (start_ptr >= end)
            return {end, 0}; // pos além do fim
        auto [cp, sz] = decode(start_ptr, end);
        start_ptr += sz;
    }

    // Avança 'count' codepoints a partir de start_ptr
    const char *end_ptr = start_ptr;
    for (std::size_t i = 0; i < count; ++i) {
        if (end_ptr >= end)
            break; // trunca no fim da string
        auto [cp, sz] = decode(end_ptr, end);
        end_ptr += sz;
    }

    return {start_ptr, static_cast<std::size_t>(end_ptr - start_ptr)};
}

// ─────────────────────────────────────────────────────────────────────────────

// View UTF-8 com cache lazy de contagem de codepoints.
// SIZE_MAX é o sentinel "ainda não calculado": distingue string vazia
// (count==0) de string não calculada.
struct string_view_utf8 {
    std::string_view internalstr;
    mutable std::size_t cached_count{SIZE_MAX}; // SIZE_MAX = não calculado

    explicit string_view_utf8(std::string_view sv) noexcept : internalstr(sv) {}
    explicit string_view_utf8(const std::string &s) noexcept : internalstr(s) {}

    iterator begin() const noexcept {
        return iterator(internalstr.data(),
                        internalstr.data() + internalstr.size());
    }
    iterator end() const noexcept {
        auto e = internalstr.data() + internalstr.size();
        return iterator(e, e);
    }

    // O(n) na primeira chamada, O(1) depois.
    // String vazia retorna 0 corretamente sem recalcular.
    [[nodiscard]] std::size_t
    count_codepoints() const noexcept { // O(1) amortizado
        if (cached_count == SIZE_MAX)
            cached_count = static_cast<std::size_t>(begin().count_codepoints());
        return cached_count;
    }

    // Tamanho em bytes (barato, sempre O(1))
    [[nodiscard]] constexpr std::size_t byte_size() const noexcept {
        return internalstr.size();
    }

    [[nodiscard]] std::string_view
    substr(std::size_t pos,
           std::size_t count = std::string_view::npos) const noexcept {
        return utf8::substr(internalstr, pos, count);
    }
};

} // namespace utf8
