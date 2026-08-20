/*
 * A Status encapsulates the result of an operation
 */

namespace tinydb {

class Status {
public:
  Status() noexcept {};
  ~Status() = default;

  static Status Ok() { return Status(); }

private:
  enum Code { OK = 0 }; // just one enum for now
};
} // namespace tinydb