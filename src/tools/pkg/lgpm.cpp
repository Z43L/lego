// lgpm: gestor de paquetes del lenguaje.
//   lgpm nuevo NOMBRE                          crea NOMBRE/lg.toml y NOMBRE/src/main.lg
//   lgpm resolver                              resuelve dependencias y escribe lg.lock
//   lgpm construir                             llama a lgc con las rutas de dependencias -> build/NOMBRE
//   lgpm ejecutar [args...]                    construir y luego ejecutar build/NOMBRE
//   lgpm añadir NOMBRE --ruta R                añade una dependencia por ruta a lg.toml
//   lgpm añadir NOMBRE --git URL [--rev X]     añade una dependencia git a lg.toml
//
// Cada dependencia aporta una carpeta de búsqueda para lgc (-I): <raiz>/src si existe, o <raiz>.
// Las dependencias git se clonan en ~/.lgpm/cache y se fija la revisión con `git checkout`.
// Variable LGPM_LGC: ruta de lgc a usar (por defecto, la que se compiló con esta herramienta).
#include <sys/wait.h>

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "toml.h"

#ifndef LGPM_LGC_DEFAULT
#error "LGPM_LGC_DEFAULT debe definirlo CMake"
#endif

namespace fs = std::filesystem;
using lgpm::TomlTable;
using lgpm::TomlValue;

namespace {

// Error que se muestra al usuario (uso incorrecto, datos inválidos, fallo de git...).
struct CliError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

std::string shq(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') out += "'\\''";
    else out += c;
  }
  return out + "'";
}

// Ejecuta un comando del shell y devuelve su código de salida (-1 si no se pudo ejecutar).
int run(const std::string& cmd) {
  int st = std::system(cmd.c_str());
  if (st == -1) return -1;
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

// Ejecuta un comando y devuelve su salida estándar sin espacios finales.
std::string capture(const std::string& cmd) {
  std::string out;
  FILE* p = popen(cmd.c_str(), "r");
  if (!p) return out;
  char buf[256];
  while (size_t n = fread(buf, 1, sizeof buf, p)) out.append(buf, n);
  pclose(p);
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
  return out;
}

std::string fnv1a(const std::string& s) {
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  char buf[17];
  std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
  return buf;
}

bool nombreValido(const std::string& n) {
  if (n.empty()) return false;
  for (char c : n) {
    bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

std::string leerTexto(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) throw CliError("no se puede leer '" + p.string() + "'");
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void escribirTexto(const fs::path& p, const std::string& text) {
  std::ofstream out(p, std::ios::binary);
  if (!out) throw CliError("no se puede escribir '" + p.string() + "'");
  out << text;
}

// ---------- Manifiesto lg.toml ----------

struct Dep {
  std::string nombre, ruta, git, rev;
};

struct Manifest {
  fs::path dir;  // carpeta del proyecto (la que contiene lg.toml)
  std::string nombre, version;
  std::vector<Dep> deps;
};

const TomlValue* buscar(const TomlTable& t, const std::string& clave) {
  auto it = t.find(clave);
  return it == t.end() ? nullptr : &it->second;
}

std::string textoDe(const TomlTable& t, const std::string& clave, const std::string& contexto) {
  const TomlValue* v = buscar(t, clave);
  if (!v) return "";
  if (v->kind != TomlValue::Kind::String)
    throw CliError(contexto + ": '" + clave + "' debe ser una cadena entre comillas");
  return v->str;
}

Manifest leerManifest(const fs::path& dir) {
  fs::path file = dir / "lg.toml";
  TomlTable t;
  try {
    t = lgpm::parseToml(leerTexto(file));
  } catch (lgpm::TomlError& e) {
    throw CliError(file.string() + ": " + e.what());
  }

  Manifest m;
  m.dir = dir;
  const TomlValue* proy = buscar(t, "proyecto");
  if (!proy || proy->kind != TomlValue::Kind::Table)
    throw CliError(file.string() + ": falta la tabla [proyecto]");
  m.nombre = textoDe(proy->table, "nombre", file.string() + " [proyecto]");
  if (!nombreValido(m.nombre))
    throw CliError(file.string() + " [proyecto]: falta 'nombre' o no es válido (letras, cifras, '_' o '-')");
  m.version = textoDe(proy->table, "version", file.string() + " [proyecto]");
  if (m.version.empty()) m.version = "0.0.0";

  const TomlValue* deps = buscar(t, "dependencias");
  if (deps) {
    if (deps->kind != TomlValue::Kind::Table) throw CliError(file.string() + ": [dependencias] debe ser una tabla");
    for (auto& [nombre, valor] : deps->table) {
      std::string ctx = file.string() + " [dependencias] " + nombre;
      if (!nombreValido(nombre)) throw CliError(ctx + ": nombre de dependencia no válido");
      if (valor.kind != TomlValue::Kind::Table)
        throw CliError(ctx + ": debe ser una tabla en línea, p. ej. { ruta = \"../" + nombre + "\" }");
      for (auto& [clave, valorClave] : valor.table) {
        if (clave != "ruta" && clave != "git" && clave != "rev")
          throw CliError(ctx + ": clave desconocida '" + clave + "' (válidas: ruta, git, rev)");
      }
      Dep d;
      d.nombre = nombre;
      d.ruta = textoDe(valor.table, "ruta", ctx);
      d.git = textoDe(valor.table, "git", ctx);
      d.rev = textoDe(valor.table, "rev", ctx);
      if (d.ruta.empty() == d.git.empty())
        throw CliError(ctx + ": indica 'ruta' o 'git' (exactamente una)");
      if (!d.rev.empty() && d.git.empty()) throw CliError(ctx + ": 'rev' solo se usa con 'git'");
      m.deps.push_back(d);
    }
  }
  return m;
}

// Sube desde `desde` hasta encontrar un directorio con lg.toml.
fs::path buscarProyecto(const fs::path& desde) {
  fs::path p = fs::absolute(desde);
  while (true) {
    if (fs::exists(p / "lg.toml")) return p;
    if (!p.has_relative_path() || p.parent_path() == p) break;
    p = p.parent_path();
  }
  throw CliError("no se encuentra lg.toml en esta carpeta ni en las superiores (usa 'lgpm nuevo')");
}

// ---------- Resolución de dependencias ----------

struct Resuelta {
  std::string nombre;
  fs::path raiz;   // carpeta del paquete (ruta o caché git)
  fs::path src;    // carpeta de búsqueda de imports para lgc (-I)
  bool esGit = false;
  std::string origen;  // ruta absoluta (dep por ruta) o URL git
  std::string rev, commit;
};

fs::path carpetaFuente(const fs::path& raiz) {
  fs::path s = raiz / "src";
  std::error_code ec;
  return fs::is_directory(s, ec) ? s : raiz;
}

// ~/.lgpm/cache/<nombre>-<hash(url, rev)>
fs::path dirCache(const std::string& url, const std::string& rev) {
  const char* home = std::getenv("HOME");
  if (!home) throw CliError("no está definida la variable HOME (necesaria para ~/.lgpm/cache)");
  std::string base = url;
  while (!base.empty() && base.back() == '/') base.pop_back();
  if (base.size() > 4 && base.compare(base.size() - 4, 4, ".git") == 0) base.resize(base.size() - 4);
  size_t slash = base.find_last_of("/:");
  std::string name = slash == std::string::npos ? base : base.substr(slash + 1);
  for (char& c : name)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') c = '_';
  if (name.empty()) name = "paquete";
  return fs::path(home) / ".lgpm" / "cache" / (name + "-" + fnv1a(url + "\n" + rev));
}

Resuelta resolverDep(const Manifest& m, const Dep& d) {
  Resuelta r;
  r.nombre = d.nombre;
  if (!d.ruta.empty()) {
    fs::path raiz = fs::weakly_canonical(m.dir / d.ruta);
    std::error_code ec;
    if (!fs::is_directory(raiz, ec))
      throw CliError("la dependencia '" + d.nombre + "' apunta a '" + raiz.string() +
                     "', que no es una carpeta. Corrige 'ruta' en lg.toml o usa 'lgpm añadir'");
    r.raiz = raiz;
    r.src = carpetaFuente(raiz);
    r.origen = raiz.string();
    return r;
  }

  fs::path cache = dirCache(d.git, d.rev);
  std::error_code ec;
  if (!fs::is_directory(cache, ec)) {
    fs::create_directories(cache.parent_path());
    if (run("git clone --quiet " + shq(d.git) + " " + shq(cache.string())) != 0) {
      fs::remove_all(cache, ec);
      throw CliError("no se pudo clonar '" + d.git + "' para la dependencia '" + d.nombre +
                     "'. Comprueba la URL y el acceso a la red");
    }
  }
  if (!d.rev.empty()) {
    std::string co = "git -C " + shq(cache.string()) + " checkout --quiet " + shq(d.rev);
    if (run(co + " 2>/dev/null") != 0) {
      // La revisión puede ser nueva en el remoto: se actualiza la caché una vez y se reintenta.
      run("git -C " + shq(cache.string()) + " fetch --quiet --tags origin 2>/dev/null");
      if (run(co) != 0)
        throw CliError("no existe la revisión '" + d.rev + "' en '" + d.git + "' (dependencia '" + d.nombre +
                       "'). Comprueba el nombre de la etiqueta, rama o commit en lg.toml");
    }
  }
  r.raiz = cache;
  r.src = carpetaFuente(cache);
  r.origen = d.git;
  r.esGit = true;
  r.rev = d.rev;
  r.commit = capture("git -C " + shq(cache.string()) + " rev-parse HEAD");
  return r;
}

std::vector<Resuelta> resolverTodas(const Manifest& m) {
  std::vector<Resuelta> out;
  for (auto& d : m.deps) out.push_back(resolverDep(m, d));
  return out;
}

std::string lgcPath() {
  if (const char* env = std::getenv("LGPM_LGC")) return env;
  return LGPM_LGC_DEFAULT;
}

// Compila el proyecto y devuelve la ruta del ejecutable generado.
fs::path construir(const Manifest& m) {
  fs::path main = m.dir / "src" / "main.lg";
  std::error_code ec;
  if (!fs::is_regular_file(main, ec))
    throw CliError("falta el archivo de entrada '" + main.string() + "'");

  std::vector<Resuelta> rs = resolverTodas(m);
  fs::path build = m.dir / "build";
  fs::create_directories(build);
  fs::path exe = build / m.nombre;

  std::string cmd = shq(lgcPath()) + " " + shq(main.string()) + " -o " + shq(exe.string());
  for (auto& r : rs) cmd += " -I " + shq(r.src.string());
  std::cerr << "lgpm: compilando " << m.nombre << " " << m.version << " con " << rs.size()
            << " dependencia(s)\n";
  if (run(cmd) != 0) throw CliError("falló la compilación de '" + m.nombre + "'");
  std::cerr << "lgpm: construido " << fs::relative(exe, m.dir).string() << "\n";
  return exe;
}

// ---------- Comandos ----------

int cmdNuevo(const std::vector<std::string>& args) {
  if (args.size() != 1) throw CliError("uso: lgpm nuevo NOMBRE");
  const std::string& nombre = args[0];
  if (!nombreValido(nombre)) throw CliError("nombre de proyecto no válido: '" + nombre + "' (letras, cifras, '_' o '-')");
  fs::path dir = fs::current_path() / nombre;
  std::error_code ec;
  if (fs::exists(dir, ec)) throw CliError("ya existe '" + nombre + "'");
  fs::create_directories(dir / "src");
  escribirTexto(dir / "lg.toml",
                "[proyecto]\nnombre = " + lgpm::tomlQuote(nombre) + "\nversion = \"0.1.0\"\n\n[dependencias]\n");
  escribirTexto(dir / "src" / "main.lg", "print(\"Hola desde " + nombre + "\")\n");
  std::cout << "proyecto '" << nombre << "' creado en " << nombre << "/\n";
  std::cout << "  construir: cd " << nombre << " && lgpm construir\n";
  return 0;
}

int cmdResolver() {
  fs::path dir = buscarProyecto(fs::current_path());
  Manifest m = leerManifest(dir);
  std::vector<Resuelta> rs = resolverTodas(m);

  std::string lock = "# lg.lock generado por lgpm. No editar a mano.\n";
  for (auto& r : rs) {
    lock += "\n[" + r.nombre + "]\n";
    if (!r.esGit) {
      lock += "ruta = " + lgpm::tomlQuote(r.raiz.string()) + "\n";
    } else {
      lock += "git = " + lgpm::tomlQuote(r.origen) + "\n";
      if (!r.rev.empty()) lock += "rev = " + lgpm::tomlQuote(r.rev) + "\n";
      if (!r.commit.empty()) lock += "commit = " + lgpm::tomlQuote(r.commit) + "\n";
      lock += "ruta = " + lgpm::tomlQuote(r.raiz.string()) + "\n";
    }
    std::cout << "  " << r.nombre << " -> " << r.raiz.string() << "\n";
  }
  escribirTexto(dir / "lg.lock", lock);
  std::cout << "lg.lock escrito (" << rs.size() << " dependencia(s))\n";
  return 0;
}

int cmdConstruir() {
  Manifest m = leerManifest(buscarProyecto(fs::current_path()));
  construir(m);
  return 0;
}

int cmdEjecutar(const std::vector<std::string>& args) {
  Manifest m = leerManifest(buscarProyecto(fs::current_path()));
  fs::path exe = construir(m);
  std::string cmd = shq(exe.string());
  for (auto& a : args) cmd += " " + shq(a);
  int rc = run(cmd);
  return rc < 0 ? 1 : rc;
}

int cmdAnadir(const std::vector<std::string>& args) {
  const std::string uso = "uso: lgpm añadir NOMBRE --ruta R | --git URL [--rev X]";
  if (args.empty()) throw CliError(uso);
  const std::string nombre = args[0];
  if (!nombreValido(nombre)) throw CliError("nombre de dependencia no válido: '" + nombre + "'");

  std::string ruta, git, rev;
  bool tieneRev = false;
  for (size_t i = 1; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a == "--ruta" && i + 1 < args.size()) ruta = args[++i];
    else if (a == "--git" && i + 1 < args.size()) git = args[++i];
    else if (a == "--rev" && i + 1 < args.size()) { rev = args[++i]; tieneRev = true; }
    else throw CliError("opción no reconocida '" + a + "'. " + uso);
  }
  if (ruta.empty() == git.empty()) throw CliError("indica --ruta o --git (exactamente una). " + uso);
  if (tieneRev && git.empty()) throw CliError("--rev solo se usa con --git");

  fs::path dir = buscarProyecto(fs::current_path());
  Manifest m = leerManifest(dir);
  for (auto& d : m.deps)
    if (d.nombre == nombre) throw CliError("ya existe la dependencia '" + nombre + "' en lg.toml");

  std::string linea;
  if (!ruta.empty()) {
    // La ruta se da desde la carpeta actual; en lg.toml se guarda relativa al proyecto.
    fs::path abs = fs::weakly_canonical(fs::current_path() / ruta);
    std::error_code ec;
    if (!fs::is_directory(abs, ec)) throw CliError("'" + abs.string() + "' no es una carpeta");
    std::string rel = fs::relative(abs, fs::weakly_canonical(dir)).generic_string();
    if (rel.empty()) rel = ".";
    linea = nombre + " = { ruta = " + lgpm::tomlQuote(rel) + " }";
  } else {
    linea = nombre + " = { git = " + lgpm::tomlQuote(git);
    if (tieneRev) linea += ", rev = " + lgpm::tomlQuote(rev);
    linea += " }";
  }

  // Edición de texto: se conservan comentarios y formato del resto del archivo.
  fs::path file = dir / "lg.toml";
  std::string texto = leerTexto(file);
  std::vector<std::string> lineas;
  {
    std::istringstream ss(texto);
    std::string l;
    while (std::getline(ss, l)) lineas.push_back(l);
  }
  bool insertada = false;
  for (size_t i = 0; i < lineas.size(); ++i) {
    std::string t = lineas[i];
    while (!t.empty() && (t.back() == ' ' || t.back() == '\r' || t.back() == '\t')) t.pop_back();
    if (t == "[dependencias]") {
      lineas.insert(lineas.begin() + static_cast<std::ptrdiff_t>(i) + 1, linea);
      insertada = true;
      break;
    }
  }
  if (!insertada) {
    lineas.push_back("");
    lineas.push_back("[dependencias]");
    lineas.push_back(linea);
  }
  std::string nuevo;
  for (auto& l : lineas) nuevo += l + "\n";

  // Comprobar que el resultado sigue siendo un lg.toml válido antes de escribir.
  try {
    lgpm::parseToml(nuevo);
  } catch (lgpm::TomlError& e) {
    throw CliError("no se puede añadir la dependencia: " + std::string(e.what()));
  }
  escribirTexto(file, nuevo);
  std::cout << "dependencia '" << nombre << "' añadida a " << file.string() << "\n";
  std::cout << "  " << linea << "\n";
  return 0;
}

void usage() {
  std::cerr << "uso: lgpm <comando>\n"
               "  nuevo NOMBRE                       crea un proyecto nuevo\n"
               "  resolver                           resuelve dependencias y escribe lg.lock\n"
               "  construir                          compila el proyecto con lgc -> build/NOMBRE\n"
               "  ejecutar [args...]                 construye y ejecuta el programa\n"
               "  añadir NOMBRE --ruta R             añade una dependencia por ruta\n"
               "  añadir NOMBRE --git URL [--rev X]  añade una dependencia git\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  std::string cmd = argv[1];
  std::vector<std::string> args(argv + 2, argv + argc);
  try {
    if (cmd == "nuevo") return cmdNuevo(args);
    if (cmd == "resolver") return cmdResolver();
    if (cmd == "construir") return cmdConstruir();
    if (cmd == "ejecutar") return cmdEjecutar(args);
    if (cmd == "añadir" || cmd == "anadir") return cmdAnadir(args);
    if (cmd == "-h" || cmd == "--help") {
      usage();
      return 0;
    }
    std::cerr << "lgpm: comando desconocido '" << cmd << "'\n";
    usage();
    return 2;
  } catch (CliError& e) {
    std::cerr << "lgpm: error: " << e.what() << "\n";
    return 1;
  } catch (fs::filesystem_error& e) {
    std::cerr << "lgpm: error: " << e.what() << "\n";
    return 1;
  }
}
