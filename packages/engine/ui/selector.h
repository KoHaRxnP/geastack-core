// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <memory>
#include <string>
namespace gea::embedded::ui {
class Tree;
namespace selector {
enum class ParseStatus {
  valid,
  invalidSyntax,
  unsupportedSyntax,
  limitExceeded
};
struct Program;
// A bounded selector program. Invalid/unsupported input never produces a
// partially usable program. Public DOM entry points call requireValid().
class Query {
  std::shared_ptr<const Program> program_;
  ParseStatus status_;
  mutable size_t remainingWork_ = 1000000;

public:
  Query(std::shared_ptr<const Program> program, ParseStatus status)
      : program_(std::move(program)), status_(status) {}
  ParseStatus status() const { return status_; }
  void requireValid() const;
  bool matches(const Tree &tree, int node, int scope = -1) const;
};
Query parse(const char *input);
bool disabled(const Tree &tree, int node);
} // namespace selector
} // namespace gea::embedded::ui
