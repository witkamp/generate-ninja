// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tools/gn/ninja_target_writer.h"

#include <sstream>

#include "base/files/file_util.h"
#include "base/strings/string_util.h"
#include "tools/gn/err.h"
#include "tools/gn/escape.h"
#include "tools/gn/filesystem_utils.h"
#include "tools/gn/ninja_action_target_writer.h"
#include "tools/gn/ninja_binary_target_writer.h"
#include "tools/gn/ninja_bundle_data_target_writer.h"
#include "tools/gn/ninja_copy_target_writer.h"
#include "tools/gn/ninja_create_bundle_target_writer.h"
#include "tools/gn/ninja_group_target_writer.h"
#include "tools/gn/ninja_utils.h"
#include "tools/gn/output_file.h"
#include "tools/gn/scheduler.h"
#include "tools/gn/string_utils.h"
#include "tools/gn/substitution_writer.h"
#include "tools/gn/target.h"
#include "tools/gn/trace.h"

NinjaTargetWriter::NinjaTargetWriter(const Target* target,
                                     std::ostream& out)
    : settings_(target->settings()),
      target_(target),
      out_(out),
      path_output_(settings_->build_settings()->build_dir(),
                   settings_->build_settings()->root_path_utf8(),
                   ESCAPE_NINJA) {
}

NinjaTargetWriter::~NinjaTargetWriter() {
}

// static
void NinjaTargetWriter::RunAndWriteFile(const Target* target) {
  const Settings* settings = target->settings();

  ScopedTrace trace(TraceItem::TRACE_FILE_WRITE,
                    target->label().GetUserVisibleName(false));
  trace.SetToolchain(settings->toolchain_label());

  base::FilePath ninja_file(settings->build_settings()->GetFullPath(
      GetNinjaFileForTarget(target)));

  if (g_scheduler->verbose_logging())
    g_scheduler->Log("Writing", FilePathToUTF8(ninja_file));

  base::CreateDirectory(ninja_file.DirName());

  // It's ridiculously faster to write to a string and then write that to
  // disk in one operation than to use an fstream here.
  std::stringstream file;

  // Call out to the correct sub-type of writer.
  if (target->output_type() == Target::BUNDLE_DATA) {
    NinjaBundleDataTargetWriter writer(target, file);
    writer.Run();
  } else if (target->output_type() == Target::CREATE_BUNDLE) {
    NinjaCreateBundleTargetWriter writer(target, file);
    writer.Run();
  } else if (target->output_type() == Target::COPY_FILES) {
    NinjaCopyTargetWriter writer(target, file);
    writer.Run();
  } else if (target->output_type() == Target::ACTION ||
             target->output_type() == Target::ACTION_FOREACH) {
    NinjaActionTargetWriter writer(target, file);
    writer.Run();
  } else if (target->output_type() == Target::GROUP) {
    NinjaGroupTargetWriter writer(target, file);
    writer.Run();
  } else if (target->IsBinary()) {
    NinjaBinaryTargetWriter writer(target, file);
    writer.Run();
  } else {
    CHECK(0) << "Output type of target not handled.";
  }

  WriteFileIfChanged(ninja_file, file.str(), nullptr);
}

void NinjaTargetWriter::WriteEscapedSubstitution(SubstitutionType type) {
  EscapeOptions opts;
  opts.mode = ESCAPE_NINJA;

  out_ << kSubstitutionNinjaNames[type] << " = ";
  EscapeStringToStream(out_,
      SubstitutionWriter::GetTargetSubstitution(target_, type),
      opts);
  out_ << std::endl;
}

void NinjaTargetWriter::WriteSharedVars(const SubstitutionBits& bits) {
  bool written_anything = false;

  // Target label.
  if (bits.used[SUBSTITUTION_LABEL]) {
    WriteEscapedSubstitution(SUBSTITUTION_LABEL);
    written_anything = true;
  }

  // Target label name
  if (bits.used[SUBSTITUTION_LABEL_NAME]) {
    WriteEscapedSubstitution(SUBSTITUTION_LABEL_NAME);
    written_anything = true;
  }

  // Root gen dir.
  if (bits.used[SUBSTITUTION_ROOT_GEN_DIR]) {
    WriteEscapedSubstitution(SUBSTITUTION_ROOT_GEN_DIR);
    written_anything = true;
  }

  // Root out dir.
  if (bits.used[SUBSTITUTION_ROOT_OUT_DIR]) {
    WriteEscapedSubstitution(SUBSTITUTION_ROOT_OUT_DIR);
    written_anything = true;
  }

  // Target gen dir.
  if (bits.used[SUBSTITUTION_TARGET_GEN_DIR]) {
    WriteEscapedSubstitution(SUBSTITUTION_TARGET_GEN_DIR);
    written_anything = true;
  }

  // Target out dir.
  if (bits.used[SUBSTITUTION_TARGET_OUT_DIR]) {
    WriteEscapedSubstitution(SUBSTITUTION_TARGET_OUT_DIR);
    written_anything = true;
  }

  // Target output name.
  if (bits.used[SUBSTITUTION_TARGET_OUTPUT_NAME]) {
    WriteEscapedSubstitution(SUBSTITUTION_TARGET_OUTPUT_NAME);
    written_anything = true;
  }

  // If we wrote any vars, separate them from the rest of the file that follows
  // with a blank line.
  if (written_anything)
    out_ << std::endl;
}

OutputFile NinjaTargetWriter::WriteInputDepsStampAndGetDep(
    const std::vector<const Target*>& extra_hard_deps) const {
  CHECK(target_->toolchain())
      << "Toolchain not set on target "
      << target_->label().GetUserVisibleName(true);

  // ----------
  // Collect all input files that are input deps of this target. Knowing the
  // number before writing allows us to either skip writing the input deps
  // stamp or optimize it. Use pointers to avoid copies here.
  std::vector<const SourceFile*> input_deps_sources;
  input_deps_sources.reserve(32);

  // Actions get implicit dependencies on the script itself.
  if ((target_->output_type() == Target::ACTION ||
       target_->output_type() == Target::ACTION_FOREACH) &&
      !target_->action_values().script().is_null())
    input_deps_sources.push_back(&target_->action_values().script());

  // Input files.
  for (const auto& input : target_->inputs())
    input_deps_sources.push_back(&input);

  // For an action (where we run a script only once) the sources are the same
  // as the inputs. For action_foreach, the sources will be operated on
  // separately so don't handle them here.
  if (target_->output_type() == Target::ACTION) {
    for (const auto& source : target_->sources())
      input_deps_sources.push_back(&source);
  }

  // ----------
  // Collect all target input dependencies of this target as was done for the
  // files above.
  std::vector<const Target*> input_deps_targets;
  input_deps_targets.reserve(32);

  // Hard dependencies that are direct or indirect dependencies.
  // These are large (up to 100s), hence why we check other
  const std::set<const Target*>& hard_deps(target_->recursive_hard_deps());
  for (const Target* target : hard_deps)
    input_deps_targets.push_back(target);

  // Extra hard dependencies passed in. These are usually empty or small, and
  // we don't want to duplicate the explicit hard deps of the target.
  for (const Target* target : extra_hard_deps) {
    if (!hard_deps.count(target))
      input_deps_targets.push_back(target);
  }

  // Toolchain dependencies. These must be resolved before doing anything.
  // This just writes all toolchain deps for simplicity. If we find that
  // toolchains often have more than one dependency, we could consider writing
  // a toolchain-specific stamp file and only include the stamp here.
  // Note that these are usually empty/small.
  const LabelTargetVector& toolchain_deps = target_->toolchain()->deps();
  for (const auto& toolchain_dep : toolchain_deps) {
    // This could theoretically duplicate dependencies already in the list,
    // but it shouldn't happen in practice, is inconvenient to check for,
    // and only results in harmless redundant dependencies listed.
    input_deps_targets.push_back(toolchain_dep.ptr);
  }

  // ---------
  // Write the outputs.

  if (input_deps_sources.size() + input_deps_targets.size() == 0)
    return OutputFile();  // No input dependencies.

  // If we're only generating one input dependency, return it directly instead
  // of writing a stamp file for it.
  if (input_deps_sources.size() == 1 && input_deps_targets.size() == 0)
    return OutputFile(settings_->build_settings(), *input_deps_sources[0]);
  if (input_deps_sources.size() == 0 && input_deps_targets.size() == 1) {
    const OutputFile& dep = input_deps_targets[0]->dependency_output_file();
    DCHECK(!dep.value().empty());
    return dep;
  }

  // Make a stamp file.
  OutputFile input_stamp_file(
      RebasePath(GetTargetOutputDir(target_).value(),
                 settings_->build_settings()->build_dir(),
                 settings_->build_settings()->root_path_utf8()));
  input_stamp_file.value().append(target_->label().name());
  input_stamp_file.value().append(".inputdeps.stamp");

  out_ << "build ";
  path_output_.WriteFile(out_, input_stamp_file);
  out_ << ": "
       << GetNinjaRulePrefixForToolchain(settings_)
       << Toolchain::ToolTypeToName(Toolchain::TYPE_STAMP);

  // File input deps.
  for (const SourceFile* source : input_deps_sources) {
    out_ << " ";
    path_output_.WriteFile(out_, *source);
  }

  // Target input deps. Sort by label so the output is deterministic (otherwise
  // some of the targets will have gone through std::sets which will have
  // sorted them by pointer).
  std::sort(
      input_deps_targets.begin(), input_deps_targets.end(),
      [](const Target* a, const Target* b) { return a->label() < b->label(); });
  for (const auto& dep : input_deps_targets) {
    DCHECK(!dep->dependency_output_file().value().empty());
    out_ << " ";
    path_output_.WriteFile(out_, dep->dependency_output_file());
  }

  out_ << "\n";
  return input_stamp_file;
}

void NinjaTargetWriter::WriteStampForTarget(
    const std::vector<OutputFile>& files,
    const std::vector<OutputFile>& order_only_deps) {
  const OutputFile& stamp_file = target_->dependency_output_file();

  // First validate that the target's dependency is a stamp file. Otherwise,
  // we shouldn't have gotten here!
  CHECK(base::EndsWith(stamp_file.value(), ".stamp",
                       base::CompareCase::INSENSITIVE_ASCII))
      << "Output should end in \".stamp\" for stamp file output. Instead got: "
      << "\"" << stamp_file.value() << "\"";

  out_ << "build ";
  path_output_.WriteFile(out_, stamp_file);

  out_ << ": "
       << GetNinjaRulePrefixForToolchain(settings_)
       << Toolchain::ToolTypeToName(Toolchain::TYPE_STAMP);
  path_output_.WriteFiles(out_, files);

  if (!order_only_deps.empty()) {
    out_ << " ||";
    path_output_.WriteFiles(out_, order_only_deps);
  }
  out_ << std::endl;
}
