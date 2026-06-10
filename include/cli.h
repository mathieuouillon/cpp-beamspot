#ifndef CLI_H
#define CLI_H

// ==========================================================================
// cli.h -- a tiny header-only, argparse-style command-line parser.
//
// Bind options and flags directly to variables; parse() fills them and
// handles --help/-h, --version/-V, type conversion errors and unknown
// options (printing usage and exiting), so callers stay declarative:
//
//     cli::parser app("beamspot", "BeamSpot -- CLAS12 beam-spot analysis");
//     app.add_option("-o", "--output-dir", opts.output_dir, "Output folder");
//     app.add_flag  ("-m", "--merge",      opts.merge,      "Merge files");
//     app.add_positional("files", opts.inputs, "Input files");
//     app.set_version("beamspot 1.0");
//     app.parse(argc, argv);
//
// Supported forms: "-o value", "--long value", "--long=value", and bundled
// boolean short flags are NOT supported (kept intentionally simple).
// ==========================================================================

#include <cstdlib>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace cli {

// string -> T conversion used for option values. Specialized for std::string
// so quoted/spaced values pass through untouched.
template <typename T>
inline bool from_string(const std::string& s, T& out) {
  std::istringstream is(s);
  is >> out;
  return !is.fail() && is.eof();
}
template <>
inline bool from_string<std::string>(const std::string& s, std::string& out) {
  out = s;
  return true;
}

// T -> string for rendering defaults in the help text.
template <typename T>
inline std::string to_string(const T& v) {
  std::ostringstream os;
  os << v;
  return os.str();
}

class parser {
 public:
  parser(std::string program, std::string description)
      : program_(std::move(program)), description_(std::move(description)) {}

  // bind a value option; the default shown in help is the variable's current value
  template <typename T>
  parser& add_option(const std::string& short_flag, const std::string& long_flag,
                     T& bind, const std::string& help) {
    options_.push_back(option{
        short_flag, long_flag, help, to_string(bind), /*is_flag=*/false,
        [&bind](const std::string& v) { return from_string(v, bind); }});
    return *this;
  }

  // bind a boolean flag (presence sets it true)
  parser& add_flag(const std::string& short_flag, const std::string& long_flag,
                   bool& bind, const std::string& help) {
    options_.push_back(option{
        short_flag, long_flag, help, "", /*is_flag=*/true,
        [&bind](const std::string&) { bind = true; return true; }});
    return *this;
  }

  // collect all non-option arguments into a vector
  parser& add_positional(const std::string& name, std::vector<std::string>& bind,
                         const std::string& help) {
    positional_name_ = name;
    positional_help_ = help;
    positional_ = &bind;
    return *this;
  }

  void set_version(std::string v) { version_ = std::move(v); }

  // parse argv; prints help/version/errors and exits as appropriate
  void parse(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];

      if (arg == "-h" || arg == "--help") {
        std::cout << help();
        std::exit(0);
      }
      if (!version_.empty() && (arg == "-V" || arg == "--version")) {
        std::cout << version_ << "\n";
        std::exit(0);
      }

      // split "--long=value" into name and inline value
      std::string inline_value;
      bool has_inline = false;
      if (arg.rfind("--", 0) == 0) {
        const auto eq = arg.find('=');
        if (eq != std::string::npos) {
          inline_value = arg.substr(eq + 1);
          arg = arg.substr(0, eq);
          has_inline = true;
        }
      } else if (arg.size() > 2 && arg[0] == '-' && arg[1] != '-') {
        // split "-xVALUE" (short option with attached value) into "-x" and "VALUE"
        if (const option* o = find(arg.substr(0, 2)); o != nullptr && !o->is_flag) {
          inline_value = arg.substr(2);
          arg = arg.substr(0, 2);
          has_inline = true;
        }
      }

      const bool looks_like_option = arg.size() > 1 && arg[0] == '-';
      if (!looks_like_option) {
        if (positional_) positional_->push_back(arg);
        else fail("unexpected positional argument '" + arg + "'");
        continue;
      }

      option* opt = find(arg);
      if (!opt) fail("unknown option '" + arg + "'");

      if (opt->is_flag) {
        if (has_inline) fail("flag '" + arg + "' does not take a value");
        opt->setter("");
        continue;
      }

      std::string value;
      if (has_inline) {
        value = inline_value;
      } else if (i + 1 < argc) {
        value = argv[++i];
      } else {
        fail("option '" + arg + "' requires an argument");
      }
      if (!opt->setter(value)) fail("invalid value '" + value + "' for option '" + arg + "'");
    }
  }

  std::string usage() const {
    std::string u = "Usage: " + program_ + " [options]";
    if (positional_) u += " " + positional_name_ + "...";
    return u + "\n";
  }

  std::string help() const {
    std::ostringstream os;
    os << description_ << "\n\n" << usage() << "\nOptions:\n";
    for (const auto& o : options_) os << "  " << format_flags(o) << "\n      " << o.help
                                      << (o.is_flag || o.default_value.empty()
                                              ? ""
                                              : "  (default: " + o.default_value + ")")
                                      << "\n";
    if (!version_.empty()) os << "  -V, --version\n      Print version and exit\n";
    os << "  -h, --help\n      Show this help and exit\n";
    if (positional_) os << "\nPositional:\n  " << positional_name_ << "...\n      " << positional_help_ << "\n";
    return os.str();
  }

 private:
  struct option {
    std::string short_flag;
    std::string long_flag;
    std::string help;
    std::string default_value;
    bool is_flag;
    std::function<bool(const std::string&)> setter;
  };

  option* find(const std::string& arg) {
    for (auto& o : options_)
      if ((!o.short_flag.empty() && arg == o.short_flag) ||
          (!o.long_flag.empty() && arg == o.long_flag))
        return &o;
    return nullptr;
  }

  static std::string format_flags(const option& o) {
    std::string s;
    if (!o.short_flag.empty()) s += o.short_flag;
    if (!o.short_flag.empty() && !o.long_flag.empty()) s += ", ";
    if (!o.long_flag.empty()) s += o.long_flag;
    if (!o.is_flag) s += " ARG";
    return s;
  }

  [[noreturn]] void fail(const std::string& message) const {
    std::cerr << program_ << ": ERROR: " << message << "\n\n" << usage();
    std::exit(1);
  }

  std::string program_;
  std::string description_;
  std::string version_;
  std::vector<option> options_;
  std::string positional_name_;
  std::string positional_help_;
  std::vector<std::string>* positional_ = nullptr;
};

}  // namespace cli

#endif  // CLI_H
