#include "tinydb_vtab.h"
#include "tinydb/database.h"
#include "tinydb/limits.h"

#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t POOL_PAGES = 256;

// Values stored in a row: a tag byte, then the payload.
enum Tag : char { Null = 0, Integer = 1, Real = 2, Text = 3, Blob = 4 };

void PutBigEndian(std::string &out, std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out.push_back(static_cast<char>(value >> shift));
  }
}

auto GetBigEndian(std::string_view in) -> std::uint64_t {
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) {
    value = value << 8U | static_cast<unsigned char>(in[index]);
  }
  return value;
}

// SQLite's column type affinity, from the declared type (section 3.1 of
// https://sqlite.org/datatype3.html). Values are converted on write as a
// native table would.
enum class Affinity { Blob, Text, Numeric, Integer, Real };

auto AffinityOf(std::string_view type) -> Affinity {
  const auto has = [&](std::string_view part) {
    return type.find(part) != std::string_view::npos;
  };
  if (has("INT")) {
    return Affinity::Integer;
  }
  if (has("CHAR") || has("CLOB") || has("TEXT")) {
    return Affinity::Text;
  }
  if (has("BLOB") ||
      type.find_first_not_of(" \t\n") == std::string_view::npos) {
    return Affinity::Blob;
  }
  if (has("REAL") || has("FLOA") || has("DOUB")) {
    return Affinity::Real;
  }
  return Affinity::Numeric;
}

// The value's storage class after applying affinity; may convert the value.
auto StorageClass(sqlite3_value *value, Affinity affinity) -> int {
  int type = sqlite3_value_type(value);
  if (affinity == Affinity::Text &&
      (type == SQLITE_INTEGER || type == SQLITE_FLOAT)) {
    return SQLITE_TEXT;
  }
  if (affinity == Affinity::Blob || affinity == Affinity::Text) {
    return type;
  }
  if (type == SQLITE_TEXT) {
    type = sqlite3_value_numeric_type(value);
  }
  if (type == SQLITE_FLOAT && affinity != Affinity::Real) {
    const double real = sqlite3_value_double(value);
    if (real == std::floor(real) && real >= -0x1p63 && real < 0x1p63) {
      return SQLITE_INTEGER;
    }
  }
  if (type == SQLITE_INTEGER && affinity == Affinity::Real) {
    return SQLITE_FLOAT;
  }
  return type;
}

void AppendValue(std::string &out, sqlite3_value *value, Affinity affinity) {
  switch (const int type = StorageClass(value, affinity)) {
  case SQLITE_INTEGER: {
    const auto integer =
        sqlite3_value_type(value) == SQLITE_FLOAT
            ? static_cast<std::int64_t>(sqlite3_value_double(value))
            : sqlite3_value_int64(value);
    out.push_back(Integer);
    PutBigEndian(out, static_cast<std::uint64_t>(integer));
    break;
  }
  case SQLITE_FLOAT:
    out.push_back(Real);
    PutBigEndian(out,
                 std::bit_cast<std::uint64_t>(sqlite3_value_double(value)));
    break;
  case SQLITE_TEXT:
  case SQLITE_BLOB: {
    const bool text = type == SQLITE_TEXT;
    const auto *bytes = static_cast<const char *>(
        text ? static_cast<const void *>(sqlite3_value_text(value))
             : sqlite3_value_blob(value));
    const auto size = static_cast<std::uint32_t>(sqlite3_value_bytes(value));
    out.push_back(text ? Text : Blob);
    PutBigEndian(out, size);
    out.append(bytes ? bytes : "", size);
    break;
  }
  default:
    out.push_back(Null);
  }
}

// Offsets of each stored value in a row, so columns decode on demand.
auto SplitRow(std::string_view row, std::size_t count)
    -> std::optional<std::vector<std::string_view>> {
  std::vector<std::string_view> values;
  values.reserve(count);
  std::size_t offset = 0;
  while (values.size() < count) {
    if (offset >= row.size()) {
      return std::nullopt;
    }
    std::size_t size = 1;
    const auto tag = row[offset];
    if (tag == Integer || tag == Real) {
      size += 8;
    } else if (tag == Text || tag == Blob) {
      if (offset + 9 > row.size()) {
        return std::nullopt;
      }
      size += 8 + GetBigEndian(row.substr(offset + 1));
    } else if (tag != Null) {
      return std::nullopt;
    }
    if (offset + size > row.size()) {
      return std::nullopt;
    }
    values.push_back(row.substr(offset, size));
    offset += size;
  }
  return values;
}

void ResultValue(sqlite3_context *context, std::string_view value) {
  switch (value[0]) {
  case Integer:
    sqlite3_result_int64(
        context, static_cast<sqlite3_int64>(GetBigEndian(value.substr(1))));
    break;
  case Real:
    sqlite3_result_double(context,
                          std::bit_cast<double>(GetBigEndian(value.substr(1))));
    break;
  case Text:
    sqlite3_result_text(context, value.data() + 9,
                        static_cast<int>(value.size() - 9), SQLITE_TRANSIENT);
    break;
  case Blob:
    sqlite3_result_blob(context, value.data() + 9,
                        static_cast<int>(value.size() - 9), SQLITE_TRANSIENT);
    break;
  default:
    sqlite3_result_null(context);
  }
}

// One open TinyDB file, shared by every table stored in it.
struct Store {
  std::unique_ptr<tinydb::Database> database;
  std::unique_ptr<tinydb::WriteTransaction> write;
  // Cursors outside a write transaction share one read transaction.
  std::unique_ptr<tinydb::ReadTransaction> read;
  int readers = 0;

  // Undo entries let SQLite roll back to a savepoint, such as the start of a
  // failed statement, without discarding the whole TinyDB transaction.
  struct Undo {
    std::string key;
    std::optional<std::string> old;
  };
  std::vector<Undo> undo;
  std::vector<std::pair<int, std::size_t>> savepoints;

  auto Get(std::string_view key) -> tinydb::Result<std::optional<std::string>> {
    return write ? write->Get(key) : database->Get(key);
  }

  auto Remember(std::string_view key) -> tinydb::Status {
    if (savepoints.empty()) {
      return {};
    }
    auto old = write->Get(key);
    if (!old) {
      return std::move(old.error());
    }
    undo.push_back({std::string(key), std::move(*old)});
    return {};
  }

  auto Put(std::string_view key, std::string_view value) -> tinydb::Status {
    if (auto status = Remember(key); !status.Ok()) {
      return status;
    }
    return write->Put(key, value);
  }

  auto Delete(std::string_view key) -> tinydb::Status {
    if (auto status = Remember(key); !status.Ok()) {
      return status;
    }
    auto removed = write->Delete(key);
    return removed ? tinydb::Status{} : std::move(removed.error());
  }

  void EndWrite() {
    write.reset();
    undo.clear();
    savepoints.clear();
  }
};

std::mutex stores_mutex;
std::map<std::string, std::weak_ptr<Store>> stores;

auto OpenStore(const std::string &path,
               std::string &error) -> std::shared_ptr<Store> {
  std::error_code code;
  const auto canonical = std::filesystem::weakly_canonical(path, code).string();
  const auto &name = code ? path : canonical;
  std::lock_guard lock(stores_mutex);
  if (auto store = stores[name].lock()) {
    return store;
  }
  auto database = tinydb::Database::Open(name, POOL_PAGES);
  if (!database) {
    error = std::string(database.error().Message());
    return nullptr;
  }
  auto store = std::make_shared<Store>();
  store->database = std::move(*database);
  stores[name] = store;
  return store;
}

struct Table : sqlite3_vtab {
  sqlite3 *db = nullptr;
  std::shared_ptr<Store> store;
  std::string name;
  std::string prefix;
  std::size_t columns = 0;
  std::vector<Affinity> affinities;
  std::vector<bool> not_null;
  std::size_t pk = 0;
  bool integer_pk = false;
};

int Fail(sqlite3_vtab *vtab, int code, std::string_view message) {
  sqlite3_free(vtab->zErrMsg);
  vtab->zErrMsg =
      sqlite3_mprintf("%.*s", static_cast<int>(message.size()), message.data());
  return code;
}

int Fail(sqlite3_vtab *vtab, const tinydb::Status &status) {
  return Fail(vtab, SQLITE_ERROR, status.Message());
}

// The primary key as it is stored: a big-endian integer with the sign bit
// flipped, so unsigned byte order is numeric order, or the text bytes. Returns
// nothing when the value cannot be stored in the key column.
auto EncodeKey(const Table &table,
               sqlite3_value *value) -> std::optional<std::string> {
  std::string key = table.prefix;
  if (table.integer_pk) {
    std::int64_t integer = 0;
    switch (sqlite3_value_numeric_type(value)) {
    case SQLITE_INTEGER:
      integer = sqlite3_value_int64(value);
      break;
    case SQLITE_FLOAT: {
      const double real = sqlite3_value_double(value);
      if (real != std::floor(real) || real < -0x1p63 || real >= 0x1p63) {
        return std::nullopt;
      }
      integer = static_cast<std::int64_t>(real);
      break;
    }
    default:
      return std::nullopt;
    }
    PutBigEndian(key, static_cast<std::uint64_t>(integer) ^ (1ULL << 63));
    return key;
  }
  const auto type = sqlite3_value_type(value);
  if (type == SQLITE_NULL || type == SQLITE_BLOB) {
    return std::nullopt;
  }
  const auto *text = sqlite3_value_text(value);
  key.append(reinterpret_cast<const char *>(text),
             static_cast<std::size_t>(sqlite3_value_bytes(value)));
  return key;
}

// Parses "name TYPE constraints" well enough to find the name, the type, and
// whether the column is the primary key.
struct ColumnDef {
  std::string text;
  bool primary = false;
  bool not_null = false;
  Affinity affinity = Affinity::Blob;
  // Why the definition cannot be stored faithfully, if it cannot.
  std::string unsupported;
};

// Whether text contains word as a whole SQL word.
auto HasWord(std::string_view text, std::string_view word) -> bool {
  const auto identifier = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
  };
  for (auto found = text.find(word); found != std::string_view::npos;
       found = text.find(word, found + 1)) {
    const auto after = found + word.size();
    if ((found == 0 || !identifier(text[found - 1])) &&
        (after == text.size() || !identifier(text[after]))) {
      return true;
    }
  }
  return false;
}

auto ToUpper(std::string_view text) -> std::string {
  std::string upper(text);
  for (auto &c : upper) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return upper;
}

auto ParseColumn(std::string_view text) -> ColumnDef {
  ColumnDef column;
  column.text = text;
  const auto upper = ToUpper(text);
  // Skip the name, which may be quoted, to read the declared type.
  std::size_t start = upper.find_first_not_of(" \t\n");
  std::size_t end = start;
  if (start != std::string::npos && std::strchr("\"`[", upper[start])) {
    const char close = upper[start] == '[' ? ']' : upper[start];
    end = upper.find(close, start + 1);
    end = end == std::string::npos ? upper.size() : end + 1;
  } else if (start != std::string::npos) {
    end = upper.find_first_of(" \t\n", start);
  }
  const auto rest = end == std::string::npos
                        ? std::string_view{}
                        : std::string_view(upper).substr(end);
  const auto name = std::string_view(upper).substr(
      start == std::string::npos ? upper.size() : start,
      end == std::string::npos ? std::string::npos : end - start);
  for (const auto *keyword :
       {"PRIMARY", "UNIQUE", "CHECK", "FOREIGN", "CONSTRAINT"}) {
    if (name == keyword) {
      column.unsupported = "table constraints are not supported";
    }
  }
  column.primary = HasWord(rest, "PRIMARY");
  column.not_null = HasWord(rest, "NOT") && HasWord(rest, "NULL");
  // SQLite ignores these on virtual tables, so accepting them would silently
  // drop them.
  for (const auto *keyword :
       {"CHECK", "DEFAULT", "UNIQUE", "REFERENCES", "GENERATED", "AS"}) {
    if (HasWord(rest, keyword)) {
      column.unsupported = std::string(keyword) + " is not supported";
    }
  }
  if (column.primary && HasWord(rest, "COLLATE")) {
    column.unsupported = "COLLATE on the PRIMARY KEY is not supported";
  }
  // The type ends where constraints begin.
  auto type = rest;
  for (const auto *keyword :
       {"PRIMARY", "NOT", "NULL", "UNIQUE", "CHECK", "DEFAULT", "COLLATE",
        "REFERENCES", "CONSTRAINT", "GENERATED", "AS"}) {
    const auto found = type.find(std::string(" ") + keyword);
    if (found != std::string_view::npos) {
      type = type.substr(0, found);
    }
  }
  column.affinity = AffinityOf(type);
  return column;
}

auto Unquote(std::string_view text) -> std::string {
  const auto start = text.find_first_not_of(" \t\n");
  const auto end = text.find_last_not_of(" \t\n");
  if (start == std::string_view::npos) {
    return {};
  }
  text = text.substr(start, end - start + 1);
  if (text.size() >= 2 && (text.front() == '\'' || text.front() == '"') &&
      text.back() == text.front()) {
    std::string out;
    for (std::size_t index = 1; index + 1 < text.size(); ++index) {
      out.push_back(text[index]);
      if (text[index] == text.front() && text[index + 1] == text.front()) {
        ++index;
      }
    }
    return out;
  }
  return std::string(text);
}

int Connect(sqlite3 *db, void *, int argc, const char *const *argv,
            sqlite3_vtab **out, char **error) {
  const auto fail = [&](std::string_view message) {
    *error = sqlite3_mprintf("%.*s", static_cast<int>(message.size()),
                             message.data());
    return SQLITE_ERROR;
  };
  if (argc < 5) {
    return fail("usage: USING tinydb('path', column definitions...)");
  }
  auto table = std::make_unique<Table>();
  table->db = db;
  table->name = argv[2];
  table->prefix = table->name + '\0';

  std::string declaration = "CREATE TABLE x(";
  bool found = false;
  for (int index = 4; index < argc; ++index) {
    const auto column = ParseColumn(argv[index]);
    if (!column.unsupported.empty()) {
      return fail(std::string("tinydb: ") + argv[index] + ": " +
                  column.unsupported);
    }
    if (column.primary) {
      if (found) {
        return fail("tinydb tables need exactly one PRIMARY KEY column");
      }
      if (column.affinity != Affinity::Integer &&
          column.affinity != Affinity::Text) {
        return fail("the tinydb PRIMARY KEY column must be INTEGER or TEXT");
      }
      found = true;
      table->pk = static_cast<std::size_t>(index - 4);
      table->integer_pk = column.affinity == Affinity::Integer;
    }
    table->affinities.push_back(column.affinity);
    table->not_null.push_back(column.not_null);
    declaration += (index > 4 ? ", " : "") + column.text;
  }
  if (!found) {
    return fail("tinydb tables need exactly one PRIMARY KEY column");
  }
  table->columns = static_cast<std::size_t>(argc - 4);
  declaration += ") WITHOUT ROWID";

  std::string message;
  table->store = OpenStore(Unquote(argv[3]), message);
  if (!table->store) {
    return fail("cannot open tinydb database: " + message);
  }
  if (const int code = sqlite3_declare_vtab(db, declaration.c_str());
      code != SQLITE_OK) {
    return code;
  }
  sqlite3_vtab_config(db, SQLITE_VTAB_CONSTRAINT_SUPPORT, 1);
  *out = table.release();
  return SQLITE_OK;
}

int Disconnect(sqlite3_vtab *vtab) {
  delete static_cast<Table *>(vtab);
  return SQLITE_OK;
}

int Destroy(sqlite3_vtab *vtab) {
  auto &table = *static_cast<Table *>(vtab);
  auto &store = *table.store;
  // Deleting invalidates cursors, so collect the keys first.
  const auto remove = [&](auto &transaction) -> tinydb::Status {
    auto cursor = transaction.Seek(table.prefix);
    if (!cursor) {
      return std::move(cursor.error());
    }
    std::vector<std::string> keys;
    for (; cursor->Valid() && cursor->Key().starts_with(table.prefix);) {
      keys.emplace_back(cursor->Key());
      if (auto status = cursor->Next(); !status.Ok()) {
        return status;
      }
    }
    for (const auto &key : keys) {
      if (auto removed = transaction.Delete(key); !removed) {
        return std::move(removed.error());
      }
    }
    return {};
  };
  if (store.write) {
    if (auto status = remove(*store.write); !status.Ok()) {
      return Fail(vtab, status);
    }
  } else {
    auto write = store.database->BeginWrite();
    if (!write) {
      return Fail(vtab, write.error());
    }
    if (auto status = remove(**write); !status.Ok()) {
      return Fail(vtab, status);
    }
    if (auto status = (*write)->Commit(); !status.Ok()) {
      return Fail(vtab, status);
    }
  }
  return Disconnect(vtab);
}

// idxNum bits describing the chosen access path.
enum Plan : int {
  Equal = 1,
  Lower = 2,
  LowerInclusive = 4,
  Upper = 8,
  UpperInclusive = 16,
};

int BestIndex(sqlite3_vtab *vtab, sqlite3_index_info *info) {
  const auto &table = *static_cast<Table *>(vtab);
  int equal = -1;
  int lower = -1;
  int upper = -1;
  for (int index = 0; index < info->nConstraint; ++index) {
    const auto &constraint = info->aConstraint[index];
    if (!constraint.usable ||
        constraint.iColumn != static_cast<int>(table.pk)) {
      continue;
    }
    switch (constraint.op) {
    case SQLITE_INDEX_CONSTRAINT_EQ:
      equal = index;
      break;
    case SQLITE_INDEX_CONSTRAINT_GT:
    case SQLITE_INDEX_CONSTRAINT_GE:
      lower = index;
      break;
    case SQLITE_INDEX_CONSTRAINT_LT:
    case SQLITE_INDEX_CONSTRAINT_LE:
      upper = index;
      break;
    default:
      break;
    }
  }

  // SQLite rechecks every constraint (omit stays 0), so a bound that cannot
  // be applied safely at run time is dropped instead of changing results.
  int plan = 0;
  int argument = 0;
  if (equal >= 0) {
    plan = Equal;
    info->aConstraintUsage[equal].argvIndex = ++argument;
    info->estimatedCost = 10;
    info->estimatedRows = 1;
    info->idxFlags = SQLITE_INDEX_SCAN_UNIQUE;
  } else {
    double cost = 1e6;
    if (lower >= 0) {
      plan |= Lower;
      if (info->aConstraint[lower].op == SQLITE_INDEX_CONSTRAINT_GE) {
        plan |= LowerInclusive;
      }
      info->aConstraintUsage[lower].argvIndex = ++argument;
      cost /= 4;
    }
    if (upper >= 0) {
      plan |= Upper;
      if (info->aConstraint[upper].op == SQLITE_INDEX_CONSTRAINT_LE) {
        plan |= UpperInclusive;
      }
      info->aConstraintUsage[upper].argvIndex = ++argument;
      cost /= 4;
    }
    info->estimatedCost = cost;
    info->estimatedRows = static_cast<sqlite3_int64>(cost);
  }
  info->idxNum = plan;
  // Every access path returns rows in primary key order.
  if (info->nOrderBy == 1 &&
      info->aOrderBy[0].iColumn == static_cast<int>(table.pk) &&
      !info->aOrderBy[0].desc) {
    info->orderByConsumed = 1;
  }
  return SQLITE_OK;
}

struct Cursor : sqlite3_vtab_cursor {
  // A scan walks a TinyDB cursor; an equality lookup holds its one row.
  std::optional<tinydb::Cursor> scan;
  std::optional<std::string> point_key;
  std::string point_value;
  std::optional<std::string> upper;
  bool upper_inclusive = false;
  bool reading = false;
  bool eof = true;
  std::optional<std::vector<std::string_view>> row;

  auto &Table() const { return *static_cast<::Table *>(pVtab); }

  auto Key() const -> std::string_view {
    return scan ? scan->Key() : std::string_view(*point_key);
  }
  auto Value() const -> std::string_view {
    return scan ? scan->Value() : std::string_view(point_value);
  }

  // Ends the scan when the key leaves this table or passes the upper bound.
  void Check() {
    row.reset();
    if (!scan) {
      return;
    }
    eof = !scan->Valid() || !scan->Key().starts_with(Table().prefix);
    if (!eof && upper) {
      const auto order = scan->Key().compare(*upper);
      eof = upper_inclusive ? order > 0 : order >= 0;
    }
  }

  void Release() {
    scan.reset();
    if (reading) {
      reading = false;
      auto &store = *Table().store;
      if (--store.readers == 0) {
        store.read.reset();
      }
    }
  }
};

int Open(sqlite3_vtab *, sqlite3_vtab_cursor **out) {
  *out = new Cursor{};
  return SQLITE_OK;
}

int Close(sqlite3_vtab_cursor *base) {
  auto *cursor = static_cast<Cursor *>(base);
  cursor->Release();
  delete cursor;
  return SQLITE_OK;
}

int Filter(sqlite3_vtab_cursor *base, int plan, const char *, int argc,
           sqlite3_value **argv) {
  auto &cursor = *static_cast<Cursor *>(base);
  auto &table = cursor.Table();
  auto &store = *table.store;
  cursor.Release();
  cursor.point_key.reset();
  cursor.upper.reset();
  cursor.row.reset();
  cursor.eof = true;

  int argument = 0;
  std::optional<std::string> equal;
  std::optional<std::string> lower;
  if ((plan & Equal) != 0 && argument < argc) {
    equal = EncodeKey(table, argv[argument++]);
  }
  if ((plan & Lower) != 0 && argument < argc) {
    lower = EncodeKey(table, argv[argument++]);
  }
  if ((plan & Upper) != 0 && argument < argc) {
    cursor.upper = EncodeKey(table, argv[argument++]);
    cursor.upper_inclusive = (plan & UpperInclusive) != 0;
  }

  if (equal) {
    auto value = store.Get(*equal);
    if (!value) {
      return Fail(&table, value.error());
    }
    if (*value) {
      cursor.point_key = std::move(*equal);
      cursor.point_value = std::move(**value);
      cursor.eof = false;
    }
    return SQLITE_OK;
  }

  const std::string start = lower ? *lower : table.prefix;
  tinydb::Result<tinydb::Cursor> scan =
      [&]() -> tinydb::Result<tinydb::Cursor> {
    if (store.write) {
      return store.write->Seek(start);
    }
    if (!store.read) {
      auto read = store.database->BeginRead();
      if (!read) {
        return tinydb::Err(std::move(read.error()));
      }
      store.read = std::move(*read);
    }
    ++store.readers;
    cursor.reading = true;
    return store.read->Seek(start);
  }();
  if (!scan) {
    cursor.Release();
    return Fail(&table, scan.error());
  }
  cursor.scan.emplace(std::move(*scan));
  cursor.Check();
  if (!cursor.eof && lower && (plan & LowerInclusive) == 0 &&
      cursor.scan->Key() == *lower) {
    if (auto status = cursor.scan->Next(); !status.Ok()) {
      return Fail(&table, status);
    }
    cursor.Check();
  }
  return SQLITE_OK;
}

int Next(sqlite3_vtab_cursor *base) {
  auto &cursor = *static_cast<Cursor *>(base);
  if (!cursor.scan) {
    cursor.eof = true;
    return SQLITE_OK;
  }
  if (auto status = cursor.scan->Next(); !status.Ok()) {
    return Fail(&cursor.Table(), status);
  }
  cursor.Check();
  return SQLITE_OK;
}

int Eof(sqlite3_vtab_cursor *base) {
  return static_cast<Cursor *>(base)->eof ? 1 : 0;
}

int Column(sqlite3_vtab_cursor *base, sqlite3_context *context, int column) {
  auto &cursor = *static_cast<Cursor *>(base);
  const auto &table = cursor.Table();
  const auto index = static_cast<std::size_t>(column);
  if (index == table.pk) {
    const auto key = cursor.Key().substr(table.prefix.size());
    if (table.integer_pk) {
      sqlite3_result_int64(context, static_cast<sqlite3_int64>(
                                        GetBigEndian(key) ^ (1ULL << 63)));
    } else {
      sqlite3_result_text(context, key.data(), static_cast<int>(key.size()),
                          SQLITE_TRANSIENT);
    }
    return SQLITE_OK;
  }
  if (!cursor.row) {
    cursor.row = SplitRow(cursor.Value(), table.columns - 1);
    if (!cursor.row) {
      return Fail(base->pVtab, SQLITE_CORRUPT, "malformed tinydb row");
    }
  }
  ResultValue(context, (*cursor.row)[index < table.pk ? index : index - 1]);
  return SQLITE_OK;
}

int Rowid(sqlite3_vtab_cursor *, sqlite3_int64 *) { return SQLITE_ERROR; }

int Begin(sqlite3_vtab *vtab) {
  auto &store = *static_cast<Table *>(vtab)->store;
  if (store.write) {
    return SQLITE_OK;
  }
  if (store.readers > 0) {
    return Fail(vtab, SQLITE_BUSY,
                "tinydb cannot start writing while a read cursor is open");
  }
  store.read.reset();
  auto write = store.database->BeginWrite();
  if (!write) {
    return Fail(vtab, write.error());
  }
  store.write = std::move(*write);
  return SQLITE_OK;
}

int Sync(sqlite3_vtab *vtab) {
  auto &store = *static_cast<Table *>(vtab)->store;
  if (!store.write) {
    return SQLITE_OK;
  }
  auto status = store.write->Commit();
  store.EndWrite();
  return status.Ok() ? SQLITE_OK : Fail(vtab, status);
}

int Commit(sqlite3_vtab *) { return SQLITE_OK; }

int Rollback(sqlite3_vtab *vtab) {
  static_cast<Table *>(vtab)->store->EndWrite();
  return SQLITE_OK;
}

int Savepoint(sqlite3_vtab *vtab, int level) {
  auto &store = *static_cast<Table *>(vtab)->store;
  // Every table in the store receives the same savepoint; record it once.
  if (store.savepoints.empty() || store.savepoints.back().first < level) {
    store.savepoints.emplace_back(level, store.undo.size());
  }
  return SQLITE_OK;
}

int Release(sqlite3_vtab *vtab, int level) {
  auto &store = *static_cast<Table *>(vtab)->store;
  while (!store.savepoints.empty() && store.savepoints.back().first >= level) {
    store.savepoints.pop_back();
  }
  if (store.savepoints.empty()) {
    store.undo.clear();
  }
  return SQLITE_OK;
}

int RollbackTo(sqlite3_vtab *vtab, int level) {
  auto &store = *static_cast<Table *>(vtab)->store;
  auto target = store.undo.size();
  while (!store.savepoints.empty() && store.savepoints.back().first >= level) {
    target = store.savepoints.back().second;
    if (store.savepoints.back().first == level) {
      break;
    }
    store.savepoints.pop_back();
  }
  while (store.undo.size() > target) {
    const auto &entry = store.undo.back();
    auto status = entry.old ? store.write->Put(entry.key, *entry.old) : [&] {
      auto removed = store.write->Delete(entry.key);
      return removed ? tinydb::Status{} : std::move(removed.error());
    }();
    if (!status.Ok()) {
      return Fail(vtab, status);
    }
    store.undo.pop_back();
  }
  return SQLITE_OK;
}

int Update(sqlite3_vtab *vtab, int argc, sqlite3_value **argv,
           sqlite3_int64 *) {
  auto &table = *static_cast<Table *>(vtab);
  auto &store = *table.store;
  if (!store.write) {
    if (const int code = Begin(vtab); code != SQLITE_OK) {
      return code;
    }
  }

  std::optional<std::string> old_key;
  if (sqlite3_value_type(argv[0]) != SQLITE_NULL) {
    old_key = EncodeKey(table, argv[0]);
    if (!old_key) {
      return SQLITE_OK;
    }
  }
  if (argc == 1) {
    auto status = store.Delete(*old_key);
    return status.Ok() ? SQLITE_OK : Fail(vtab, status);
  }

  // OR IGNORE skips a row that violates any constraint.
  const bool ignore = sqlite3_vtab_on_conflict(table.db) == SQLITE_IGNORE;
  sqlite3_value *pk = argv[2 + table.pk];
  if (sqlite3_value_type(pk) == SQLITE_NULL) {
    return ignore ? SQLITE_OK
                  : Fail(vtab, SQLITE_CONSTRAINT_NOTNULL,
                         "NOT NULL constraint failed: " + table.name +
                             " primary key");
  }
  // Keys are stored in their declared type, as in a STRICT table.
  const auto key = EncodeKey(table, pk);
  if (!key) {
    return ignore ? SQLITE_OK
                  : Fail(vtab, SQLITE_CONSTRAINT_DATATYPE,
                         "cannot store " +
                             std::string(sqlite3_value_type(pk) == SQLITE_FLOAT
                                             ? "REAL"
                                         : sqlite3_value_type(pk) == SQLITE_BLOB
                                             ? "BLOB"
                                             : "TEXT") +
                             " value in " + table.name + " primary key");
  }
  std::string row;
  for (std::size_t index = 0; index < table.columns; ++index) {
    if (table.not_null[index] &&
        sqlite3_value_type(argv[2 + index]) == SQLITE_NULL) {
      return ignore ? SQLITE_OK
                    : Fail(vtab, SQLITE_CONSTRAINT_NOTNULL,
                           "NOT NULL constraint failed: " + table.name);
    }
    if (index != table.pk) {
      AppendValue(row, argv[2 + index], table.affinities[index]);
    }
  }
  if (key->size() + row.size() > tinydb::MAX_ENTRY_SIZE) {
    return Fail(vtab, SQLITE_TOOBIG,
                "row exceeds TinyDB's " +
                    std::to_string(tinydb::MAX_ENTRY_SIZE) + "-byte limit");
  }

  if (key != old_key) {
    auto existing = store.write->Get(*key);
    if (!existing) {
      return Fail(vtab, existing.error());
    }
    if (*existing) {
      switch (sqlite3_vtab_on_conflict(table.db)) {
      case SQLITE_REPLACE:
        break;
      case SQLITE_IGNORE:
        return SQLITE_OK;
      default:
        return Fail(vtab, SQLITE_CONSTRAINT_PRIMARYKEY,
                    "UNIQUE constraint failed: " + table.name + " primary key");
      }
    }
    if (old_key) {
      if (auto status = store.Delete(*old_key); !status.Ok()) {
        return Fail(vtab, status);
      }
    }
  }
  auto status = store.Put(*key, row);
  return status.Ok() ? SQLITE_OK : Fail(vtab, status);
}

sqlite3_module MODULE = {
    /* iVersion */ 2,
    /* xCreate */ Connect,
    /* xConnect */ Connect,
    /* xBestIndex */ BestIndex,
    /* xDisconnect */ Disconnect,
    /* xDestroy */ Destroy,
    /* xOpen */ Open,
    /* xClose */ Close,
    /* xFilter */ Filter,
    /* xNext */ Next,
    /* xEof */ Eof,
    /* xColumn */ Column,
    /* xRowid */ Rowid,
    /* xUpdate */ Update,
    /* xBegin */ Begin,
    /* xSync */ Sync,
    /* xCommit */ Commit,
    /* xRollback */ Rollback,
    /* xFindFunction */ nullptr,
    /* xRename */ nullptr,
    /* xSavepoint */ Savepoint,
    /* xRelease */ Release,
    /* xRollbackTo */ RollbackTo,
    /* xShadowName */ nullptr,
    /* xIntegrity */ nullptr,
};

} // namespace

extern "C" int tinydb_sqlite_register(sqlite3 *db) {
  return sqlite3_create_module(db, "tinydb", &MODULE, nullptr);
}

extern "C" int sqlite3_tinydb_init(sqlite3 *db, char **,
                                   const sqlite3_api_routines *api) {
  SQLITE_EXTENSION_INIT2(api);
  return tinydb_sqlite_register(db);
}
