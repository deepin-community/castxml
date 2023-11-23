/*
  Copyright Kitware, Inc.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      https://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#include "Detect.h"
#include "Options.h"
#include "Utils.h"

#include "llvm/Config/llvm-config.h"

#if LLVM_VERSION_MAJOR >= 17
#  include "llvm/TargetParser/Host.h"
#  include "llvm/TargetParser/Triple.h"
#else
#  include "llvm/ADT/Triple.h"
#  include "llvm/Support/Host.h"
#endif

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FileSystem.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string.h>

static std::string getClangBuiltinIncludeDir()
{
  return getClangResourceDir() + "/include";
}

static bool failedCC(const char* id, std::vector<const char*> const& args,
                     std::string const& out, std::string const& err,
                     std::string const& msg)
{
  std::cerr << "error: '--castxml-cc-" << id
            << "' compiler command failed:\n\n";
  for (std::vector<const char*>::const_iterator i = args.begin(),
                                                e = args.end();
       i != e; ++i) {
    std::cerr << " '" << *i << "'";
  }
  std::cerr << "\n";
  if (!msg.empty()) {
    std::cerr << msg << "\n";
  } else {
    std::cerr << out << "\n";
    std::cerr << err << "\n";
  }
  return false;
}

static void fixPredefines(std::string& pd, std::string const& rm)
{
  std::string::size_type beg = 0;
  while ((beg = pd.find(rm, beg), beg != std::string::npos)) {
    std::string::size_type end = pd.find('\n', beg);
    if (end != std::string::npos) {
      pd.erase(beg, end + 1 - beg);
    } else {
      pd.erase(beg);
    }
  }
}

static void fixPredefines(Options& opts)
{
  // Remove any detected conflicting definition of a Clang builtin macro.
  fixPredefines(opts.Predefines, "#define __bool ");
  fixPredefines(opts.Predefines, "#define __builtin_vsx_");
  fixPredefines(opts.Predefines, "#define __has_");
  fixPredefines(opts.Predefines, "#define __pixel ");
  fixPredefines(opts.Predefines, "#define __vector ");
}

static void setTriple(Options& opts)
{
  std::string const& pd = opts.Predefines;
  llvm::Triple triple(llvm::sys::getDefaultTargetTriple());
  if (pd.find("#define __x86_64__ 1") != pd.npos ||
      pd.find("#define _M_X64 ") != pd.npos) {
    triple.setArchName("x86_64");
  } else if (pd.find("#define __amd64__ 1") != pd.npos ||
             pd.find("#define _M_AMD64 ") != pd.npos) {
    triple.setArchName("amd64");
  } else if (pd.find("#define __i386__ 1") != pd.npos ||
             pd.find("#define _M_IX86 ") != pd.npos) {
    triple.setArchName("i386");
  } else if (pd.find("#define __aarch64__ 1") != pd.npos ||
             pd.find("#define _M_ARM64 ") != pd.npos) {
    triple.setArchName("aarch64");
  } else if (pd.find("#define __arm__ 1") != pd.npos ||
             pd.find("#define _M_ARM ") != pd.npos) {
    triple.setArchName("arm");
  }
  if (pd.find("#define _WIN32 1") != pd.npos) {
    triple.setVendorName("pc");
    triple.setOSName("windows");
  }
  if (pd.find("#define _MSC_VER ") != pd.npos) {
    triple.setEnvironmentName("msvc");
  }
  if (pd.find("#define __MINGW32__ 1") != pd.npos) {
    triple.setEnvironmentName("gnu");
  }
  opts.Triple = triple.getTriple();
}

static bool isBuiltinIncludeDir(std::string const& inc)
{
  // FIXME: Intrinsics headers are platform-specific.
  // Is there a better way to detect this directory?
  return (llvm::sys::fs::exists(inc + "/emmintrin.h")   // x86_64
          || llvm::sys::fs::exists(inc + "/altivec.h")  // ppc64
          || llvm::sys::fs::exists(inc + "/arm_neon.h") // aarch64
  );
}

static bool detectCC_GNU(const char* const* argBeg, const char* const* argEnd,
                         Options& opts, const char* id, const char* ext)
{
  std::string const fwExplicitSuffix = " (framework directory)";
  std::string const fwImplicitSuffix = "/Frameworks";
  std::vector<const char*> cc_args(argBeg, argEnd);
  std::string empty_cpp = getResourceDir() + "/empty." + ext;
  int ret;
  std::string out;
  std::string err;
  std::string msg;
  cc_args.push_back("-E");
  cc_args.push_back("-dM");
  cc_args.push_back("-v");
  cc_args.push_back(empty_cpp.c_str());
  if (runCommand(int(cc_args.size()), &cc_args[0], ret, out, err, msg) &&
      ret == 0) {
    opts.Predefines = out;
    const char* start_line = "#include <...> search starts here:";
    if (const char* c = strstr(err.c_str(), start_line)) {
      if ((c = strchr(c, '\n'), c++)) {
        while (*c++ == ' ') {
          if (const char* e = strchr(c, '\n')) {
            const char* s = c;
            c = e + 1;
            if (*(e - 1) == '\r') {
              --e;
            }
            std::string inc(s, e - s);
            std::replace(inc.begin(), inc.end(), '\\', '/');
            bool fw = ((inc.size() > fwExplicitSuffix.size()) &&
                       (inc.substr(inc.size() - fwExplicitSuffix.size()) ==
                        fwExplicitSuffix));
            if (fw) {
              inc = inc.substr(0, inc.size() - fwExplicitSuffix.size());
            } else {
              fw = ((inc.size() > fwImplicitSuffix.size()) &&
                    (inc.substr(inc.size() - fwImplicitSuffix.size()) ==
                     fwImplicitSuffix));
            }
            // Replace the compiler builtin include directory with ours.
            if (!fw && isBuiltinIncludeDir(inc)) {
              inc = getClangBuiltinIncludeDir();
            }
            opts.Includes.push_back(Options::Include(inc, fw));
          }
        }
      }
    }
    fixPredefines(opts);
    setTriple(opts);
    return true;
  } else {
    return failedCC(id, cc_args, out, err, msg);
  }
}

static bool detectCC_MSVC(const char* const* argBeg, const char* const* argEnd,
                          Options& opts, const char* id, const char* ext)
{
  std::vector<const char*> cc_args(argBeg, argEnd);
  std::string detect_vs_cpp = getResourceDir() + "/detect_vs." + ext;
  int ret;
  std::string out;
  std::string err;
  std::string msg;

  // Create a temporary directory to hold some temporary files.
  llvm::SmallString<128> tmpDir;
  if (std::error_code e =
        llvm::sys::fs::createUniqueDirectory("castxml", tmpDir)) {
    msg = e.message();
    return failedCC(id, cc_args, out, err, msg);
  }

  // Tell MSVC to put the object file in the temporary directory.
  // We previously used '-FoNUL' but this does not work on Windows 11.
  llvm::SmallString<128> tmpObj = tmpDir;
  tmpObj.append("/detect_vs.obj");
  llvm::SmallString<128> argFo("-Fo");
  argFo.append(tmpObj);

  // Extend the original command line with our source and object.
  cc_args.push_back("-c");
  cc_args.push_back(detect_vs_cpp.c_str());
  cc_args.push_back(argFo.c_str());

  // Run the compiler.
  bool success =
    runCommand(int(cc_args.size()), &cc_args[0], ret, out, err, msg) &&
    ret == 0;

  // Remove temporary object file and directory.
  llvm::sys::fs::remove(llvm::Twine(tmpObj));
  llvm::sys::fs::remove(llvm::Twine(tmpDir));

  // Check results.
  if (success) {
    if (const char* predefs = strstr(out.c_str(), "\n#define")) {
      opts.Predefines = predefs + 1;
    }
    if (const char* includes_str = std::getenv("INCLUDE")) {
      llvm::SmallVector<llvm::StringRef, 8> includes;
      llvm::StringRef includes_ref(includes_str);
      includes_ref.split(includes, ";", -1, false);
      for (llvm::StringRef i : includes) {
        if (!i.empty()) {
          std::string inc(i);
          std::replace(inc.begin(), inc.end(), '\\', '/');
          opts.Includes.push_back(inc);
        }
      }
    }
    fixPredefines(opts);
    setTriple(opts);
    return true;
  } else {
    return failedCC(id, cc_args, out, err, msg);
  }
}

bool detectCC(const char* id, const char* const* argBeg,
              const char* const* argEnd, Options& opts)
{
  if (strcmp(id, "gnu") == 0) {
    return detectCC_GNU(argBeg, argEnd, opts, id, "cpp");
  } else if (strcmp(id, "gnu-c") == 0) {
    return detectCC_GNU(argBeg, argEnd, opts, id, "c");
  } else if (strcmp(id, "msvc") == 0) {
    return detectCC_MSVC(argBeg, argEnd, opts, id, "cpp");
  } else if (strcmp(id, "msvc-c") == 0) {
    return detectCC_MSVC(argBeg, argEnd, opts, id, "c");
  } else {
    std::cerr << "error: '--castxml-cc-" << id << "' not known!\n";
    return false;
  }
}
