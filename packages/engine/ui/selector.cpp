// SPDX-License-Identifier: Apache-2.0
#include "selector.h"
#include "node.h"
#include "tree_internal.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>
namespace gea::embedded::ui::selector {
namespace {
constexpr size_t maxInput = 4096, maxTerms = 256, maxDepth = 8;
bool space(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}
std::string lower(std::string s) {
  for (char &c : s)
    if (c >= 'A' && c <= 'Z')
      c += 32;
  return s;
}
bool start(unsigned char c) {
  return c == '_' || c == '-' || std::isalpha(c) || c >= 128 || c == '\\';
}
bool next(unsigned char c) { return start(c) || std::isdigit(c); }
void utf8(std::string &out, unsigned c) {
  if (!c || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
    c = 0xfffd;
  if (c < 128)
    out += char(c);
  else if (c < 2048) {
    out += char(0xc0 | (c >> 6));
    out += char(0x80 | (c & 63));
  } else if (c < 65536) {
    out += char(0xe0 | (c >> 12));
    out += char(0x80 | ((c >> 6) & 63));
    out += char(0x80 | (c & 63));
  } else {
    out += char(0xf0 | (c >> 18));
    out += char(0x80 | ((c >> 12) & 63));
    out += char(0x80 | ((c >> 6) & 63));
    out += char(0x80 | (c & 63));
  }
}
enum class Kind {
  tag,
  id,
  klass,
  attribute,
  disabled,
  enabled,
  negation,
  scope
};
struct Atom {
  Kind kind;
  std::string name, value, op;
  bool insensitive = false, explicitCase = false;
  std::shared_ptr<Program> nested;
};
struct Part {
  std::vector<Atom> atoms;
  char relation = 0;
};
using Chain = std::vector<Part>;
} // namespace
struct Program {
  std::vector<Chain> alternatives;
};
namespace {
class Parser {
  const std::string input;
  size_t pos = 0, terms = 0;
  ParseStatus error = ParseStatus::valid;
  char peek() const { return pos < input.size() ? input[pos] : 0; }
  bool ws() {
    size_t old = pos;
    while (space(peek()))
      ++pos;
    return pos != old;
  }
  void fail(ParseStatus s = ParseStatus::invalidSyntax) {
    if (error == ParseStatus::valid)
      error = s;
  }
  bool term() {
    if (++terms > maxTerms) {
      fail(ParseStatus::limitExceeded);
      return false;
    }
    return true;
  }
  bool escape(std::string &out) {
    if (peek() != '\\')
      return false;
    ++pos;
    char c = peek();
    if (!c || c == '\n' || c == '\r' || c == '\f') {
      fail();
      return false;
    }
    if (std::isxdigit(static_cast<unsigned char>(c))) {
      unsigned value = 0;
      int n = 0;
      while (n < 6 && std::isxdigit(static_cast<unsigned char>(peek()))) {
        char h = input[pos++];
        value = value * 16 +
                (h >= '0' && h <= '9'
                     ? h - '0'
                     : std::tolower(static_cast<unsigned char>(h)) - 'a' + 10);
        ++n;
      }
      if (space(peek())) {
        if (peek() == '\r' && pos + 1 < input.size() && input[pos + 1] == '\n')
          ++pos;
        ++pos;
      }
      utf8(out, value);
    } else
      out += input[pos++];
    return true;
  }
  std::string ident() {
    std::string out;
    if (!start(static_cast<unsigned char>(peek()))) {
      fail();
      return out;
    }
    if (peek() == '-' && pos + 1 < input.size() &&
        std::isdigit(static_cast<unsigned char>(input[pos + 1]))) {
      fail();
      return out;
    }
    while (next(static_cast<unsigned char>(peek()))) {
      if (peek() == '\\') {
        if (!escape(out))
          break;
      } else
        out += input[pos++];
    }
    if (out.empty() || out == "-")
      fail();
    return out;
  }
  std::string quoted() {
    char quote = input[pos++];
    std::string out;
    while (peek() && peek() != quote) {
      if (peek() == '\\') {
        if (!escape(out))
          return out;
      } else if (peek() == '\n' || peek() == '\r' || peek() == '\f') {
        fail();
        return out;
      } else
        out += input[pos++];
    }
    if (peek() != quote)
      fail();
    else
      ++pos;
    return out;
  }
  Atom attr() {
    Atom a{Kind::attribute};
    ++pos;
    ws();
    a.name = lower(ident());
    ws();
    if (peek() == ']') {
      ++pos;
      return a;
    }
    if (peek() == '=') {
      a.op = "=";
      ++pos;
    } else if (std::strchr("~|^$*", peek()) && peek()) {
      a.op += input[pos++];
      if (peek() != '=') {
        fail();
        return a;
      }
      a.op += '=';
      ++pos;
    } else {
      fail();
      return a;
    }
    ws();
    a.value = peek() == '\'' || peek() == '"' ? quoted() : ident();
    ws();
    if (peek() != ']') {
      std::string flag = lower(ident());
      if (flag != "i" && flag != "s")
        fail();
      a.insensitive = flag == "i";
      a.explicitCase = true;
      ws();
    }
    if (peek() != ']')
      fail();
    else
      ++pos;
    return a;
  }
  Part compound(size_t depth) {
    Part p;
    if (peek() == '*') {
      ++pos;
      p.atoms.push_back({Kind::tag, "*"});
      term();
    } else if (start(static_cast<unsigned char>(peek()))) {
      p.atoms.push_back({Kind::tag, lower(ident())});
      term();
    }
    while (error == ParseStatus::valid) {
      char c = peek();
      if (c == '#' || c == '.') {
        ++pos;
        Atom a{c == '#' ? Kind::id : Kind::klass};
        a.name = ident();
        p.atoms.push_back(std::move(a));
        term();
      } else if (c == '[') {
        p.atoms.push_back(attr());
        term();
      } else if (c == ':') {
        ++pos;
        std::string name = lower(ident());
        Atom a{Kind::scope};
        if (name == "disabled")
          a.kind = Kind::disabled;
        else if (name == "enabled")
          a.kind = Kind::enabled;
        else if (name == "scope")
          a.kind = Kind::scope;
        else if (name == "not") {
          a.kind = Kind::negation;
          if (peek() != '(') {
            fail();
            break;
          }
          ++pos;
          a.nested = list(depth + 1, ')');
          if (peek() != ')') {
            fail();
            break;
          }
          ++pos;
        } else {
          fail(ParseStatus::unsupportedSyntax);
          break;
        }
        p.atoms.push_back(std::move(a));
        term();
      } else
        break;
    }
    if (p.atoms.empty())
      fail();
    return p;
  }
  std::shared_ptr<Program> list(size_t depth, char end) {
    if (depth > maxDepth) {
      fail(ParseStatus::limitExceeded);
      return {};
    }
    auto program = std::make_shared<Program>();
    ws();
    if (!peek() || peek() == end) {
      fail();
      return program;
    }
    while (error == ParseStatus::valid) {
      Chain chain;
      chain.push_back(compound(depth));
      while (error == ParseStatus::valid) {
        bool spaced = ws();
        char c = peek();
        if (!c || c == ',' || c == end)
          break;
        char relation = ' ';
        if (c == '>') {
          relation = '>';
          ++pos;
          ws();
        } else if (c == '+' || c == '~' || c == '|') {
          fail(ParseStatus::unsupportedSyntax);
          break;
        } else if (!spaced) {
          fail();
          break;
        }
        Part part = compound(depth);
        part.relation = relation;
        chain.push_back(std::move(part));
      }
      program->alternatives.push_back(std::move(chain));
      if (peek() != ',')
        break;
      ++pos;
      ws();
      if (!peek() || peek() == end) {
        fail();
        break;
      }
    }
    return program;
  }

public:
  explicit Parser(const char *s)
      : input(s ? std::string(s, strnlen(s, maxInput + 1)) : "") {}
  Query run() {
    if (input.size() > maxInput)
      return {{}, ParseStatus::limitExceeded};
    auto p = list(0, 0);
    ws();
    if (peek())
      fail();
    return {error == ParseStatus::valid ? p : nullptr, error};
  }
};
bool live(const Tree &t, int n) {
  return n >= 0 && n < t.nodeCount() && NodeHandle(n).valid();
}
bool element(const Tree &t, int n) {
  return live(t, n) && static_cast<int>(NodeHandle(n).nodeType) == 1;
}
std::string tag(const Tree &t, int n) {
  const char *s = t.tagName(n);
  return lower(s ? s : "");
}
bool within(const Tree &t, int n, int parent) {
  for (int budget = t.nodeCount(); budget > 0 && live(t, n); --budget) {
    if (n == parent)
      return true;
    n = t.node(n).parent;
  }
  return false;
}
// `stepper` is the existing native SDK control (NativeStepperProps.disabled),
// not an arbitrary HTML custom element. Its enabled state must not regress.
bool disableable(const std::string &t) {
  return t == "button" || t == "input" || t == "select" || t == "textarea" ||
         t == "fieldset" || t == "option" || t == "optgroup" || t == "stepper";
}
struct Work {
  size_t left = 100000;
  void take() {
    if (left--)
      return;
#if defined(__cpp_exceptions)
    throw std::length_error("Native selector matching work limit exceeded");
#else
    std::abort();
#endif
  }
};
bool matchProgram(const Program &, const Tree &, int, int, Work &);
bool atom(const Atom &a, const Tree &t, int n, int scope, Work &work) {
  work.take();
  switch (a.kind) {
  case Kind::tag:
    return a.name == "*" || tag(t, n) == a.name ||
           (a.name == "body" && n == t.mountedRoot());
  case Kind::id:
    return t.hasAttribute(n, "id") && a.name == t.getAttribute(n, "id");
  case Kind::klass:
    return t.hasClass(n, a.name);
  case Kind::scope:
    return n == scope;
  case Kind::disabled:
    return disabled(t, n);
  case Kind::enabled:
    return disableable(tag(t, n)) && !disabled(t, n);
  case Kind::negation:
    return a.nested && !matchProgram(*a.nested, t, n, scope, work);
  case Kind::attribute: {
    if (!t.hasAttribute(n, a.name.c_str()))
      return false;
    if (a.op.empty())
      return true;
    std::string value = t.getAttribute(n, a.name.c_str()), expected = a.value;
    // HTML's legacy enumerated attribute values are ASCII insensitive even
    // when the attribute is placed on a different HTML element. Explicit `s`
    // overrides that default; data-* and arbitrary attributes remain sensitive.
    const std::string insensitiveNames =
        " accept accept-charset align alink axis bgcolor charset checked clear "
        "codetype color compact declare defer dir direction disabled enctype "
        "face "
        "frame hreflang http-equiv lang language link media method multiple "
        "nohref "
        "noresize noshade nowrap readonly rel rev rules scope scrolling "
        "selected "
        "shape target text type valign valuetype vlink ";
    const bool htmlDefault =
        !a.explicitCase &&
        insensitiveNames.find(" " + a.name + " ") != std::string::npos;
    if (a.insensitive || htmlDefault) {
      value = lower(value);
      expected = lower(expected);
    }
    if (a.op == "=")
      return value == expected;
    if (a.op == "|=")
      return value == expected || value.rfind(expected + "-", 0) == 0;
    if (expected.empty())
      return false;
    if (a.op == "^=")
      return value.rfind(expected, 0) == 0;
    if (a.op == "$=")
      return value.size() >= expected.size() &&
             value.compare(value.size() - expected.size(), expected.size(),
                           expected) == 0;
    if (a.op == "*=")
      return value.find(expected) != std::string::npos;
    if (a.op == "~=") {
      size_t p = 0;
      while (p < value.size()) {
        while (p < value.size() && space(value[p]))
          ++p;
        size_t e = p;
        while (e < value.size() && !space(value[e]))
          ++e;
        if (value.substr(p, e - p) == expected)
          return true;
        p = e;
      }
      return false;
    }
    return false;
  }
  }
  return false;
}
bool part(const Part &p, const Tree &t, int n, int scope, Work &work) {
  if (!element(t, n))
    return false;
  for (const auto &a : p.atoms)
    if (!atom(a, t, n, scope, work))
      return false;
  return true;
}
bool chain(const Chain &c, size_t i, const Tree &t, int n, int scope,
           int budget, Work &work) {
  work.take();
  if (budget <= 0 || !part(c[i], t, n, scope, work))
    return false;
  if (i == 0)
    return true;
  int parent = t.node(n).parent;
  if (c[i].relation == '>')
    return live(t, parent) &&
           chain(c, i - 1, t, parent, scope, budget - 1, work);
  for (int left = budget - 1; left > 0 && live(t, parent); --left) {
    if (chain(c, i - 1, t, parent, scope, left, work))
      return true;
    parent = t.node(parent).parent;
  }
  return false;
}
bool matchProgram(const Program &p, const Tree &t, int n, int scope,
                  Work &work) {
  for (const auto &c : p.alternatives)
    if (!c.empty() &&
        chain(c, c.size() - 1, t, n, scope, t.nodeCount() + 1, work))
      return true;
  return false;
}
} // namespace
bool disabled(const Tree &t, int n) {
  if (!element(t, n))
    return false;
  const auto name = tag(t, n);
  if (!disableable(name))
    return false;
  if (t.hasAttribute(n, "disabled"))
    return true;
  if (name == "optgroup")
    return false;
  if (name == "option") {
    int p = t.node(n).parent;
    return live(t, p) && tag(t, p) == "optgroup" &&
           t.hasAttribute(p, "disabled");
  }
  for (int p = t.node(n).parent, budget = t.nodeCount();
       budget > 0 && live(t, p); --budget, p = t.node(p).parent) {
    if (tag(t, p) != "fieldset" || !t.hasAttribute(p, "disabled"))
      continue;
    int legend = -1;
    for (int c = t.node(p).first_child, left = t.nodeCount();
         left > 0 && live(t, c); --left, c = t.node(c).next_sibling)
      if (tag(t, c) == "legend") {
        legend = c;
        break;
      }
    if (legend >= 0 && within(t, n, legend))
      continue;
    return true;
  }
  return false;
}
Query parse(const char *input) { return Parser(input).run(); }
void Query::requireValid() const {
  if (status_ == ParseStatus::valid)
    return;
#if defined(__cpp_exceptions)
  throw std::invalid_argument(status_ == ParseStatus::unsupportedSyntax
                                  ? "Unsupported native selector grammar"
                              : status_ == ParseStatus::limitExceeded
                                  ? "Native selector limit exceeded"
                                  : "Invalid native selector syntax");
#else
  std::abort();
#endif
}
bool Query::matches(const Tree &t, int node, int scope) const {
  requireValid();
  Work work;
  work.left = remainingWork_;
  bool result = program_ && matchProgram(*program_, t, node, scope, work);
  remainingWork_ = work.left;
  return result;
}
} // namespace gea::embedded::ui::selector
