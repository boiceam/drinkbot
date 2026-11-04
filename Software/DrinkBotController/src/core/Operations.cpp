#include "core/Operations.h"

namespace drinkbot {

const char* dispenseStatusName(DispenseProgress::Status status) {
  switch (status) {
    case DispenseProgress::Status::Starting: return "STARTING";
    case DispenseProgress::Status::Done: return "DONE";
    case DispenseProgress::Status::Complete: return "COMPLETE";
    case DispenseProgress::Status::Aborted: return "ABORTED";
  }
  return "ABORTED";
}

const char* bulkOpName(BulkOp op) {
  switch (op) {
    case BulkOp::Prime: return "prime";
    case BulkOp::Purge: return "purge";
    case BulkOp::PrimeAll: return "primeAll";
    case BulkOp::PurgeAll: return "purgeAll";
  }
  return "prime";
}

const char* bulkStatusName(BulkProgress::Status status) {
  switch (status) {
    case BulkProgress::Status::Starting: return "STARTING";
    case BulkProgress::Status::Running: return "RUNNING";
    case BulkProgress::Status::Done: return "DONE";
    case BulkProgress::Status::Skipped: return "SKIPPED";
    case BulkProgress::Status::Complete: return "COMPLETE";
    case BulkProgress::Status::Aborted: return "ABORTED";
  }
  return "ABORTED";
}

}  // namespace drinkbot
