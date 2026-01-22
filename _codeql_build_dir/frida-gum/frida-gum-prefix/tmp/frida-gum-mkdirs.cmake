# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/home/runner/work/peak/peak/_codeql_build_dir/frida-gum/frida-gum-src")
  file(MAKE_DIRECTORY "/home/runner/work/peak/peak/_codeql_build_dir/frida-gum/frida-gum-src")
endif()
file(MAKE_DIRECTORY
  "/home/runner/work/peak/peak/_codeql_build_dir/frida-gum/frida-gum-prefix/src/frida-gum-build"
  "/home/runner/work/peak/peak/_codeql_build_dir/frida-gum/frida-gum-prefix"
  "/home/runner/work/peak/peak/_codeql_build_dir/frida-gum/frida-gum-prefix/tmp"
  "/home/runner/work/peak/peak/_codeql_build_dir/frida-gum/frida-gum-prefix/src/frida-gum-stamp"
  "/home/runner/work/peak/peak/_codeql_build_dir/frida-gum/frida-gum-prefix/src"
  "/home/runner/work/peak/peak/_codeql_build_dir/frida-gum/frida-gum-prefix/src/frida-gum-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/runner/work/peak/peak/_codeql_build_dir/frida-gum/frida-gum-prefix/src/frida-gum-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/runner/work/peak/peak/_codeql_build_dir/frida-gum/frida-gum-prefix/src/frida-gum-stamp${cfgdir}") # cfgdir has leading slash
endif()
