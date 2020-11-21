/*
 * Copyright 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <queue>
#include <regex>
#include <sstream>
#include <vector>

#include "declarations.h"
#include "struct_parser_generator.h"

#include "language_y.h"

void yylex_init(void**);
void yylex_destroy(void*);
void yyset_debug(int, void*);
void yyset_in(FILE*, void*);

bool generate_cpp_headers_one_file(
    const Declarations& decls,
    const std::filesystem::path& input_file,
    const std::filesystem::path& include_dir,
    const std::filesystem::path& out_dir,
    const std::string& root_namespace);

bool generate_pybind11_sources_one_file(
    const Declarations& decls,
    const std::filesystem::path& input_file,
    const std::filesystem::path& include_dir,
    const std::filesystem::path& out_dir,
    const std::string& root_namespace,
    size_t num_shards);

bool generate_rust_source_one_file(
    const Declarations& decls,
    const std::filesystem::path& input_file,
    const std::filesystem::path& include_dir,
    const std::filesystem::path& out_dir,
    const std::string& root_namespace);

bool parse_declarations_one_file(const std::filesystem::path& input_file, Declarations* declarations) {
  void* scanner;
  yylex_init(&scanner);

  FILE* in_file = fopen(input_file.string().c_str(), "r");
  if (in_file == nullptr) {
    std::cerr << "can't open " << input_file << ": " << strerror(errno) << std::endl;
    return false;
  }

  yyset_in(in_file, scanner);

  int ret = yy::parser(scanner, declarations).parse();
  if (ret != 0) {
    std::cerr << "yylex parsing failed: returned " << ret << std::endl;
    return false;
  }

  yylex_destroy(scanner);

  fclose(in_file);

  // Set endianess before returning
  for (auto& s : declarations->type_defs_queue_) {
    if (s.second->GetDefinitionType() == TypeDef::Type::STRUCT) {
      auto* struct_def = dynamic_cast<StructDef*>(s.second);
      struct_def->SetEndianness(declarations->is_little_endian);
    }
  }

  for (auto& packet_def : declarations->packet_defs_queue_) {
    packet_def.second.SetEndianness(declarations->is_little_endian);
  }

  return true;
}

// TODO(b/141583809): stop leaks
extern "C" const char* __asan_default_options() {
  return "detect_leaks=0";
}

int main(int argc, const char** argv) {
  std::filesystem::path out_dir;
  std::filesystem::path include_dir;
  std::string root_namespace = "bluetooth";
  // Number of shards per output pybind11 cc file
  size_t num_shards = 1;
  bool generate_rust = false;
  std::queue<std::filesystem::path> input_files;
  const std::string arg_out = "--out=";
  const std::string arg_include = "--include=";
  const std::string arg_namespace = "--root_namespace=";
  const std::string arg_num_shards = "--num_shards=";
  const std::string arg_rust = "--rust";

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg.find(arg_out) == 0) {
      out_dir = std::filesystem::current_path() / std::filesystem::path(arg.substr(arg_out.size()));
    } else if (arg.find(arg_include) == 0) {
      include_dir = std::filesystem::current_path() / std::filesystem::path(arg.substr(arg_include.size()));
    } else if (arg.find(arg_namespace) == 0) {
      root_namespace = arg.substr(arg_namespace.size());
    } else if (arg.find(arg_num_shards) == 0) {
      num_shards = std::stoul(arg.substr(arg_num_shards.size()));
    } else if (arg.find(arg_rust) == 0) {
      generate_rust = true;
    } else {
      input_files.emplace(std::filesystem::current_path() / std::filesystem::path(arg));
    }
  }
  if (out_dir == std::filesystem::path() || include_dir == std::filesystem::path() || num_shards == 0) {
    std::cerr << "Usage: bt-packetgen --out=OUT --include=INCLUDE --root_namespace=NAMESPACE --num_shards=NUM_SHARDS "
              << "input_files..." << std::endl;
    return 1;
  }

  std::cout << "out dir: " << out_dir << std::endl;

  while (!input_files.empty()) {
    Declarations declarations;
    std::cout << "parsing: " << input_files.front() << std::endl;
    if (!parse_declarations_one_file(input_files.front(), &declarations)) {
      std::cerr << "Cannot parse " << input_files.front() << " correctly" << std::endl;
      return 2;
    }
    if (generate_rust) {
      std::cout << "generating rust" << std::endl;
      if (!generate_rust_source_one_file(declarations, input_files.front(), include_dir, out_dir, root_namespace)) {
        std::cerr << "Didn't generate rust source for " << input_files.front() << std::endl;
        return 5;
      }
    } else {
      std::cout << "generating c++ and pybind11" << std::endl;
      if (!generate_cpp_headers_one_file(declarations, input_files.front(), include_dir, out_dir, root_namespace)) {
        std::cerr << "Didn't generate cpp headers for " << input_files.front() << std::endl;
        return 3;
      }
      if (!generate_pybind11_sources_one_file(
              declarations, input_files.front(), include_dir, out_dir, root_namespace, num_shards)) {
        std::cerr << "Didn't generate pybind11 sources for " << input_files.front() << std::endl;
        return 4;
      }
    }
    input_files.pop();
  }

  return 0;
}
