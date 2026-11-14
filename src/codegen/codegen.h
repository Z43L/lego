// Generación de código LLVM (propietario: agente de codegen).
// Entrada: AST ya anotado por sema (tipos, CallKind, targets, mangled names).
//
// Contrato de memoria (ver comentarios en codegen.cpp):
//   - Toda expresión produce un valor POSEÍDO (+1) si su tipo es heap.
//   - Las funciones reciben parámetros prestados y devuelven valores poseídos.
#pragma once

#include <string>

#include "ast/ast.h"
#include "common/diagnostics.h"
#include "sema/types.h"

namespace lg {

// optLevel: 0..3 (-O0..-O3). Genera el módulo para el host, optimiza y emite un .o nativo.
// debug: añade información DWARF (lgc -g); con false el módulo es el de siempre.
bool emitObjectFile(const Program& prog, const TypeTable& types, const std::string& objPath,
                    int optLevel, Diagnostics& diags, bool debug = false);

// Devuelve el IR textual del módulo (sin optimizar). debug: ver emitObjectFile.
bool emitLLVMIR(const Program& prog, const TypeTable& types, std::string& irOut, Diagnostics& diags,
                bool debug = false);

}  // namespace lg
