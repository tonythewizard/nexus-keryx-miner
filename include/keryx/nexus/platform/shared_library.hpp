#pragma once
#include <filesystem>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif
namespace keryx::nexus::platform {
inline void* open_library(const std::filesystem::path& path) {
#ifdef _WIN32
  if(!path.is_absolute())return nullptr;
  // Dependencies may come from the library's directory and system32, never
  // from an arbitrary current working directory.
  return reinterpret_cast<void*>(LoadLibraryExW(path.c_str(),nullptr,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32));
#else
  return dlopen(path.c_str(),RTLD_NOW|RTLD_LOCAL);
#endif
}
inline void* library_symbol(void* library,const char* name) {
#ifdef _WIN32
  return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library),name));
#else
  return dlsym(library,name);
#endif
}
inline void close_library(void* library) {
  if(!library)return;
#ifdef _WIN32
  FreeLibrary(static_cast<HMODULE>(library));
#else
  dlclose(library);
#endif
}
}
