#pragma once

#include <string>
#include <fstream>
#include <sstream>
#include <regex>
#include <mutex>
#include <cstdio>
#include <cstdlib>

namespace runtime_cfg {

struct values {
  int batch_size  = 4000;
  int num_reads   = 0;      // <=0 => derive from batch_size * 2
  int max_seq_len = 10000;
};

inline values& state()            { static values v; return v; }
inline std::once_flag& once_flag(){ static std::once_flag f; return f; }

// ---------- JSON helpers (no external library) ----------

inline int parse_int(const std::string& text, const char* key, int fallback, bool* found=nullptr) {
  std::regex re(std::string("\"") + key + R"("\s*:\s*(-?\d+))");
  std::smatch m;
  if (std::regex_search(text, m, re)) {
    if (found) *found = true;
    try { return std::stoi(m[1].str()); } catch (...) {}
  }
  if (found) *found = false;
  return fallback;
}

inline void load_json_into(const std::string& path, values& out) {
  if (path.empty()) return;
  std::ifstream f(path);
  if (!f) {
    std::fprintf(stderr, "[runtime_cfg] Warning: cannot open %s; keeping defaults.\n", path.c_str());
    return;
  }
  std::stringstream ss; ss << f.rdbuf();
  std::string s = ss.str();
  bool ok=false; int x=0;

  x = parse_int(s, "batch_size",  out.batch_size,  &ok); if (ok) out.batch_size  = x;
  x = parse_int(s, "num_reads",   out.num_reads,   &ok); if (ok) out.num_reads   = x;
  x = parse_int(s, "max_seq_len", out.max_seq_len, &ok); if (ok) out.max_seq_len = x;
}

// ---------- CLI parsing ----------

struct cli_overrides {
  std::string config_path;
  bool has_batch_size  = false; int batch_size  = 0;
  bool has_num_reads   = false; int num_reads   = 0;
  bool has_max_seq_len = false; int max_seq_len = 0;
};

inline bool is_flag(const char* s, const char* longf) {
  return std::strcmp(s, longf) == 0;
}
inline bool is_flag(const char* s, const char* shortf, const char* longf) {
  return std::strcmp(s, shortf) == 0 || std::strcmp(s, longf) == 0;
}

inline bool parse_arg_kv(const char* arg, const char* key, std::string& out) {
  // matches --key=value
  const std::string pref = std::string("--") + key + "=";
  if (std::strncmp(arg, pref.c_str(), pref.size()) == 0) {
    out = std::string(arg + pref.size());
    return true;
  }
  return false;
}

inline bool parse_arg_kv_int(const char* arg, const char* key, int& out) {
  std::string tmp;
  if (!parse_arg_kv(arg, key, tmp)) return false;
  try { out = std::stoi(tmp); } catch (...) { return false; }
  return true;
}

inline cli_overrides parse_cli(int argc, char** argv) {
  cli_overrides o;
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];

    // --config=FILE or -c FILE / --config FILE
    if (parse_arg_kv(a, "config", o.config_path)) continue;
    if (is_flag(a, "-c", "--config")) {
      if (i + 1 < argc) o.config_path = argv[++i];
      else std::fprintf(stderr, "[runtime_cfg] Warning: --config requires a path\n");
      continue;
    }

    // --batch_size=VAL or --batch_size VAL
    int v = 0;
    if (parse_arg_kv_int(a, "batch_size", v)) { o.has_batch_size = true; o.batch_size = v; continue; }
    if (is_flag(a, "--batch_size")) {
      if (i + 1 < argc) { o.has_batch_size = true; o.batch_size = std::atoi(argv[++i]); }
      else std::fprintf(stderr, "[runtime_cfg] Warning: --batch_size requires a value\n");
      continue;
    }

    // --num_reads=VAL or --num_reads VAL
    if (parse_arg_kv_int(a, "num_reads", v)) { o.has_num_reads = true; o.num_reads = v; continue; }
    if (is_flag(a, "--num_reads")) {
      if (i + 1 < argc) { o.has_num_reads = true; o.num_reads = std::atoi(argv[++i]); }
      else std::fprintf(stderr, "[runtime_cfg] Warning: --num_reads requires a value\n");
      continue;
    }

    // --max_seq_len=VAL or --max_seq_len VAL
    if (parse_arg_kv_int(a, "max_seq_len", v)) { o.has_max_seq_len = true; o.max_seq_len = v; continue; }
    if (is_flag(a, "--max_seq_len")) {
      if (i + 1 < argc) { o.has_max_seq_len = true; o.max_seq_len = std::atoi(argv[++i]); }
      else std::fprintf(stderr, "[runtime_cfg] Warning: --max_seq_len requires a value\n");
      continue;
    }
  }
  return o;
}

// ---------- Finalization ----------

inline void finalize(values& v) {
  if (v.num_reads <= 0) {
    v.num_reads = v.batch_size;
  } 
}

// ---------- Public init variants ----------

inline void init_once(const std::string& json_path = "config.json") {
  std::call_once(once_flag(), [&]{
    values v;                 // defaults
    load_json_into(json_path, v);
    finalize(v);
    state() = v;
  });
}

inline void init_once(int argc, char** argv, const std::string& default_json = "config.json") {
  std::call_once(once_flag(), [&]{
    values v;                 // defaults
    cli_overrides o = parse_cli(argc, argv);
    const std::string json_path = o.config_path.empty() ? default_json : o.config_path;

    load_json_into(json_path, v);       // JSON (if present)
    if (o.has_batch_size)  v.batch_size  = o.batch_size;    // CLI overrides
    if (o.has_num_reads)   v.num_reads   = o.num_reads;
    if (o.has_max_seq_len) v.max_seq_len = o.max_seq_len;

    finalize(v);
    state() = v;
  });
}

// ---------- Getters ----------

inline int batch_size()  { return state().batch_size; }
inline int num_reads()   { return state().num_reads; }
inline int max_seq_len() { return state().max_seq_len; }

} // namespace runtime_cfg
