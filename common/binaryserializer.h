#ifndef __BINARY_SERIALIZER__
#define __BINARY_SERIALIZER__

#include <string>
#include <vector>

#include "table.h"

namespace swss {

// Serialize into a thread-local, automatically growing buffer, which is returned.
// The returned buffer is overwritten when the function is called again.
const std::vector<char> &serializeBuffer(const std::string &dbName, const std::string &tableName,
                                         const std::vector<swss::KeyOpFieldsValuesTuple> &kcos);

// Serialize into a caller-provided buffer. Throws an exception is there is not enough capacity.
size_t serializeBuffer(char *buffer, size_t size, const std::string &dbName,
                       const std::string &tableName,
                       const std::vector<swss::KeyOpFieldsValuesTuple> &kcos);

void deserializeBuffer(const char *buffer, size_t size, std::vector<swss::FieldValueTuple> &values);

void deserializeBuffer(const char *buffer, const size_t size, std::string &dbName,
                       std::string &tableName,
                       std::vector<std::shared_ptr<KeyOpFieldsValuesTuple>> &kcos);

} // namespace swss
#endif
