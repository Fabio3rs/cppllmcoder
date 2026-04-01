#!/bin/bash
# Script para aplicar clang-format em arquivos C++ modificados

echo "Formatando arquivos C++ no staging area..."
git diff --cached --name-only --diff-filter=AM | grep -E "\.(cpp|hpp|h|cc|cxx)$" | xargs -r clang-format -i

echo "Formatando arquivos C++ modificados mas não commitados..."
git diff --name-only --diff-filter=M | grep -E "\.(cpp|hpp|h|cc|cxx)$" | xargs -r clang-format -i

echo "Formatando arquivos C++ não monitorados (novos)..."
git ls-files --others --exclude-standard | grep -E "\.(cpp|hpp|h|cc|cxx)$" | xargs -r clang-format -i

echo "Formatação concluída!"

