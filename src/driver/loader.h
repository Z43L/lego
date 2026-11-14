// CONTRATO (propietario: integrador).
// Carga el programa principal y todos los módulos importados (import util -> util.lg).
#pragma once

#include <string>
#include <vector>

#include "ast/ast.h"
#include "common/diagnostics.h"

namespace lg {

// Opciones de la carga de módulos.
struct LoadOptions {
  // Rutas de búsqueda adicionales, en orden de prioridad: las que pasa `-I` y las dependencias
  // del proyecto (lgpm). El driver añade al final la biblioteca estándar.
  std::vector<std::string> searchPaths;
};

// Lee mainFile, parsea y resuelve imports recursivamente. Las declaraciones de los módulos
// se añaden a out.decls y sus sentencias a out.stmts ANTES que las del importador
// (orden de dependencias). Cada archivo se carga una vez. Ciclos de import = error.
//
// Orden de búsqueda de `import a/b` (busca a/b.lg):
//   1. directorio del archivo que importa;
//   2. cada ruta de la variable de entorno LG_PATH (separadas por ':');
//   3. opts.searchPaths, en orden.
// Si no aparece en ninguna, el error lista las rutas buscadas.
//
// Devuelve false si hubo errores (ya registrados en diags).
bool loadProgram(const std::string& mainFile, Program& out, Diagnostics& diags,
                 const LoadOptions& opts = {});

}  // namespace lg
