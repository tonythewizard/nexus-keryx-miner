#pragma once
#include <string>
#include <string_view>
namespace keryx::nexus {
// One exact notification per stream. Never deduplicate across different changes.
class ChainNoticeFilter {
 public:
  enum class Result { ChainChanged, RefreshOnly, Duplicate };
  void clear() {have_=false;last_.clear();}
  Result observe(std::string_view wire, bool chain_changed) {
    if(have_ && wire==last_) return Result::Duplicate;
    have_=wire.size()<=1024*1024;
    if(have_)last_.assign(wire);else last_.clear();
    return chain_changed?Result::ChainChanged:Result::RefreshOnly;
  }
 private:
  bool have_{};
  std::string last_;
};
}
