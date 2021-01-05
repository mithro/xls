// Copyright 2020 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "xls/dslx/import_routines.h"

#include "xls/common/file/filesystem.h"
#include "xls/common/file/get_runfile_path.h"
#include "xls/common/status/ret_check.h"
#include "xls/dslx/parser.h"
#include "xls/dslx/scanner.h"

namespace xls::dslx {

static absl::StatusOr<std::filesystem::path> FindExistingPath(
    const ImportTokens& subject,
    absl::Span<const std::string> additional_search_paths) {
  absl::Span<std::string const> pieces = subject.pieces();
  std::string subject_path;
  absl::optional<std::string> subject_parent_path;
  if (pieces.size() == 1 && (pieces[0] == "std" || pieces[0] == "float32" ||
                             pieces[0] == "bfloat16")) {
    subject_path = absl::StrFormat("xls/dslx/stdlib/%s.x", pieces[0]);
  } else {
    subject_path = absl::StrJoin(pieces, "/") + ".x";
    subject_parent_path = absl::StrJoin(pieces.subspan(1), "/") + ".x";
  }

  std::vector<std::string> attempted;

  // Helper that tries to see if "path" is present relative to "base".
  auto try_path = [&attempted](absl::string_view base, absl::string_view path)
      -> absl::optional<std::filesystem::path> {
    auto full_path = std::filesystem::path(base) / path;
    XLS_VLOG(3) << "Trying path: " << full_path;
    attempted.push_back(std::string(full_path));
    if (FileExists(full_path).ok()) {
      XLS_VLOG(3) << "Found existing file for import path: " << full_path;
      return full_path;
    }
    return absl::nullopt;
  };
  // Helper that tries to see if the path/parent_path are present
  auto try_paths =
      [&try_path, subject_path, subject_parent_path](
          absl::string_view base) -> absl::optional<std::filesystem::path> {
    if (auto result = try_path(base, subject_path)) {
      return *result;
    }
    if (subject_parent_path.has_value()) {
      if (auto result = try_path(base, *subject_parent_path)) {
        return *result;
      }
    }
    return absl::nullopt;
  };

  // Try to find a path that actually exists -- when we do we place it here.
  absl::optional<std::filesystem::path> existing_path;

  XLS_VLOG(3) << "Attempting CWD-relative import path.";
  if (absl::optional<std::filesystem::path> cwd_relative_path =
          try_path("", subject_path)) {
    return *cwd_relative_path;
  }
  XLS_VLOG(3) << "Attempting runfile-based import path via " << subject_path;
  if (absl::StatusOr<std::string> runfile_path =
          GetXlsRunfilePath(subject_path);
      runfile_path.ok() && FileExists(*runfile_path).ok()) {
    return *runfile_path;
  }
  if (subject_parent_path.has_value()) {
    // This one is generally required for genrules in-house, where the first
    // part of the path under the depot root is stripped off for some reason.
    XLS_VLOG(3) << "Attempting CWD-based parent import path via "
                << *subject_parent_path;
    if (absl::optional<std::filesystem::path> cwd_relative_path =
            try_path("", *subject_parent_path)) {
      return *cwd_relative_path;
    }
    XLS_VLOG(3) << "Attempting runfile-based parent import path via "
                << *subject_parent_path;
    if (absl::StatusOr<std::string> runfile_path =
            GetXlsRunfilePath(*subject_parent_path);
        runfile_path.ok() && FileExists(*runfile_path).ok()) {
      return *runfile_path;
    }
  }

  // Look through the externally-supplied additional search paths.
  for (const std::string& search_path : additional_search_paths) {
    XLS_VLOG(3) << "Attempting search path root: " << search_path;
    if (auto found = try_paths(search_path)) {
      return *found;
    }
  }

  return absl::NotFoundError(absl::StrFormat(
      "Could not find DSLX file for import; attempted: [ %s ]; working "
      "directory: %s",
      absl::StrJoin(attempted, " :: "), GetCurrentDirectory().value()));
}

absl::StatusOr<const ModuleInfo*> DoImport(
    const TypecheckFn& ftypecheck, const ImportTokens& subject,
    absl::Span<const std::string> additional_search_paths, ImportCache* cache) {
  XLS_RET_CHECK(cache != nullptr);
  if (cache->Contains(subject)) {
    return cache->Get(subject);
  }

  XLS_VLOG(3) << "DoImport (uncached) subject: " << subject.ToString();

  XLS_ASSIGN_OR_RETURN(std::filesystem::path found_path,
                       FindExistingPath(subject, additional_search_paths));
  XLS_ASSIGN_OR_RETURN(std::string contents, GetFileContents(found_path));

  absl::Span<std::string const> pieces = subject.pieces();
  std::string fully_qualified_name = absl::StrJoin(pieces, ".");
  XLS_VLOG(3) << "Parsing and typechecking " << fully_qualified_name
              << ": start";

  Scanner scanner(found_path, contents);
  Parser parser(/*module_name=*/fully_qualified_name, &scanner);
  XLS_ASSIGN_OR_RETURN(std::shared_ptr<Module> module, parser.ParseModule());
  XLS_ASSIGN_OR_RETURN(std::shared_ptr<TypeInfo> type_info, ftypecheck(module));
  return cache->Put(subject,
                    ModuleInfo{std::move(module), std::move(type_info)});
}

}  // namespace xls::dslx
