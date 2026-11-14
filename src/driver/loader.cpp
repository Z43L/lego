#include "driver/loader.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "parser/parser.h"

namespace fs = std::filesystem;

namespace lg {
namespace {

// Divide LG_PATH por ':' ignorando entradas vacías.
std::vector<std::string> envSearchPaths() {
  std::vector<std::string> out;
  const char* env = std::getenv("LG_PATH");
  if (!env) return out;
  std::string cur;
  for (const char* p = env;; ++p) {
    if (*p == ':' || *p == '\0') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
      if (*p == '\0') break;
    } else {
      cur += *p;
    }
  }
  return out;
}

struct Loader {
  Diagnostics& diags;
  Program& out;
  const LoadOptions& opts;
  std::set<std::string> loaded;   // archivos ya cargados (ruta canónica)
  std::set<std::string> visiting; // pila para detectar ciclos
  bool ok = true;

  // Busca el módulo de `imp` según el orden documentado en loader.h.
  // Devuelve la ruta del archivo o "" (en ese caso registra el error).
  std::string resolve(const ImportDecl& imp, const fs::path& importerDir) {
    const std::string rel = imp.path + ".lg";
    std::vector<std::string> roots;
    roots.push_back(importerDir.string());
    for (auto& p : envSearchPaths()) roots.push_back(p);
    for (auto& p : opts.searchPaths) roots.push_back(p);

    std::vector<std::string> candidates;
    for (auto& root : roots) {
      fs::path cand = fs::path(root) / rel;
      std::error_code ec;
      if (fs::is_regular_file(cand, ec)) return cand.string();
      candidates.push_back(cand.string());
    }

    std::string msg = "no se puede abrir el módulo '" + imp.path + "': no se encuentra en ninguna ruta de búsqueda. Rutas buscadas:";
    for (auto& c : candidates) msg += "\n    " + c;
    diags.error(imp.loc, msg,
                "crea '" + rel + "' en una de esas carpetas, añade su carpeta con LG_PATH o "
                "'lgc -I carpeta', o declara la dependencia en lg.toml con 'lgpm añadir'");
    ok = false;
    return "";
  }

  // Carga `file`. Las declaraciones y sentencias de sus imports van primero.
  void load(const std::string& file, const Loc* importedFrom) {
    std::string canon = fs::weakly_canonical(file).string();
    if (loaded.count(canon)) return;
    if (visiting.count(canon)) {
      diags.error(importedFrom ? *importedFrom : Loc{file, 1, 1},
                  "import circular: '" + file + "' forma un ciclo de imports",
                  "elimina uno de los imports del ciclo (un módulo no puede importarse directa ni indirectamente a sí mismo)");
      ok = false;
      return;
    }

    std::ifstream in(file, std::ios::binary);
    if (!in) {
      Loc loc = importedFrom ? *importedFrom : Loc{file, 1, 1};
      diags.error(loc, "no se puede abrir el archivo '" + file + "'",
                  "comprueba la ruta y que el archivo exista");
      ok = false;
      return;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string source = ss.str();
    diags.registerSource(file, source);

    visiting.insert(canon);
    auto prog = parseProgram(file, source, diags);
    if (diags.hasErrors()) ok = false;

    fs::path dir = fs::path(file).parent_path();
    // Primero los imports de este módulo (dependencias).
    std::vector<std::unique_ptr<Decl>> kept;
    for (auto& d : prog->decls) {
      if (d->kind == DeclKind::Import) {
        auto* imp = static_cast<ImportDecl*>(d.get());
        std::string target = resolve(*imp, dir);
        if (!target.empty()) load(target, &imp->loc);
      } else {
        kept.push_back(std::move(d));
      }
    }
    for (auto& d : kept) out.decls.push_back(std::move(d));
    for (auto& s : prog->stmts) out.stmts.push_back(std::move(s));

    visiting.erase(canon);
    loaded.insert(canon);
  }
};

}  // namespace

bool loadProgram(const std::string& mainFile, Program& out, Diagnostics& diags,
                 const LoadOptions& opts) {
  out.file = mainFile;
  Loader l{diags, out, opts, {}, {}, true};
  l.load(mainFile, nullptr);
  return l.ok && !diags.hasErrors();
}

}  // namespace lg
