#include "core/LineFramer.h"

namespace drinkbot {

void LineFramer::feed(const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    char c = static_cast<char>(data[i]);
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      if (!discarding_ && onLine_) {
        onLine_(buf_);
      }
      buf_.clear();
      discarding_ = false;
      continue;
    }
    if (discarding_) {
      continue;
    }
    if (buf_.size() >= kMaxLine) {
      buf_.clear();
      discarding_ = true;
      if (onOverflow_) {
        onOverflow_();
      }
      continue;
    }
    buf_.push_back(c);
  }
}

void LineFramer::reset() {
  buf_.clear();
  discarding_ = false;
}

}  // namespace drinkbot
