#include <boost/cast.hpp>
#include <cstring>

#include <iostream>

#include "binaryserializer.h"
#include "rediscommand.h"

using namespace std;

template <class T> static inline T read_unaligned(const char *buffer) {
    T t;
    std::memcpy(&t, buffer, sizeof(T));
    return t;
}

template <class T> static inline void write_unaligned(char *buffer, T t) {
    std::memcpy(buffer, &t, sizeof(T));
}

// A helper class that mainly provides .setKeyAndValue() to append a key-value pair to the buffer
// it holds a pointer to. If it runs out of buffer space, it throws an error.
class Serializer {
  protected:
    char *m_buffer;
    size_t m_buffer_size;
    char *m_current_position;
    size_t m_kvp_count;

    size_t serialized_data_len() {
        return boost::numeric_cast<size_t>(m_current_position - m_buffer);
    }

    void reset() {
        // Reserve space for m_kvp_count at the start of the buffer
        m_current_position = m_buffer + sizeof(size_t);
        m_kvp_count = 0;
    }

  public:
    Serializer(char *buffer, size_t size) : m_buffer(buffer), m_buffer_size(size) {
        reset();
    }

    void setKeyAndValue(const char *key, size_t klen, const char *value, size_t vlen) {
        setData(key, klen);
        setData(value, vlen);
        m_kvp_count++;
    }

    size_t finalize() {
        // Write key/value pair count to the beginning of the buffer
        write_unaligned(m_buffer, m_kvp_count);

        // return total serialized length
        return static_cast<size_t>(m_current_position - m_buffer);
    }

    void setData(const char *data, size_t dataLen) {
        if (serialized_data_len() + dataLen + sizeof(size_t) > m_buffer_size) {
            SWSS_LOG_THROW("There is not enough buffer to serialize: current key count: %zu, "
                           "current data length: %zu, buffer size: %zu",
                           m_kvp_count, dataLen, m_buffer_size);
        }

        write_unaligned(m_current_position, dataLen);
        m_current_position += sizeof(size_t);

        std::memcpy(m_current_position, data, dataLen);
        m_current_position += dataLen;
    }
};

// A helper class that adapts Serializer into using a vector for a buffer, which will automatically
// grow as more buffer space is needed. reset() should be called before every use.
class VectorSerializer : public Serializer {
  protected:
    vector<char> m_vec;

  public:
    VectorSerializer() : Serializer(nullptr, 0) {
        m_vec.reserve(4096);
    }

    void reset() {
        Serializer::reset();
        resize(0);
    }

    const vector<char> &vec() {
        return m_vec;
    }

    void resize(size_t neededSize) {
        std::cout << neededSize << std::endl;
        m_vec.resize(neededSize);
        m_current_position = m_vec.data() + serialized_data_len();
        m_buffer_size = m_vec.size();
        m_buffer = m_vec.data();
    }

    void setData(const char *data, size_t dataLen) {
        resize(serialized_data_len() + dataLen + sizeof(size_t));
        Serializer::setData(data, dataLen);
    }
};

template <class S>
static size_t serializeBufferImpl(S &serializer, const string &dbName, const string &tableName,
                                  const vector<swss::KeyOpFieldsValuesTuple> &kcos) {
    // Set the first pair as DB name and table name.
    serializer.setKeyAndValue(dbName.c_str(), dbName.length(), tableName.c_str(),
                              tableName.length());
    for (auto &kco : kcos) {
        auto &key = kfvKey(kco);
        auto &fvs = kfvFieldsValues(kco);
        std::string fvs_len = std::to_string(fvs.size());
        // For each request, the first pair is the key and the number of attributes,
        // followed by the attribute pairs.
        // The operation is not set, when there is no attribute, it is a DEL request.
        serializer.setKeyAndValue(key.c_str(), key.length(), fvs_len.c_str(), fvs_len.length());
        for (auto &fv : fvs) {
            auto &field = fvField(fv);
            auto &value = fvValue(fv);
            serializer.setKeyAndValue(field.c_str(), field.length(), value.c_str(), value.length());
        }
    }

    return serializer.finalize();
}

namespace swss {

const vector<char> &serializeBuffer(const string &dbName, const string &tableName,
                                    const vector<swss::KeyOpFieldsValuesTuple> &kcos) {
    static thread_local VectorSerializer globalSerializer;
    globalSerializer.reset();
    serializeBufferImpl(globalSerializer, dbName, tableName, kcos);
    return globalSerializer.vec();
}

size_t serializeBuffer(char *buffer, size_t size, const std::string &dbName,
                       const std::string &tableName,
                       const std::vector<swss::KeyOpFieldsValuesTuple> &kcos) {
    auto serializer = Serializer(buffer, size);
    return serializeBufferImpl(serializer, dbName, tableName, kcos);
}

void deserializeBuffer(const char *buffer, size_t size,
                       std::vector<swss::FieldValueTuple> &values) {
    size_t kvp_count = read_unaligned<size_t>(buffer);
    auto tmp_buffer = buffer + sizeof(size_t);
    while (kvp_count > 0) {
        kvp_count--;

        // read field name
        size_t keylen = read_unaligned<size_t>(tmp_buffer);
        tmp_buffer += sizeof(size_t);
        if ((size_t)(tmp_buffer - buffer) + keylen > size) {
            SWSS_LOG_THROW(
                "serialized key data was truncated, key length: %zu, increase buffer size: %zu",
                keylen, size);
        }
        auto pkey = string(tmp_buffer, keylen);
        tmp_buffer += keylen;

        // read value
        size_t vallen = read_unaligned<size_t>(tmp_buffer);
        tmp_buffer += sizeof(size_t);
        if ((size_t)(tmp_buffer - buffer) + vallen > size) {
            SWSS_LOG_THROW("serialized value data was truncated, value length: %zu increase "
                           "buffer size: %zu",
                           vallen, size);
        }
        auto pval = string(tmp_buffer, vallen);
        tmp_buffer += vallen;

        values.push_back(std::make_pair(pkey, pval));
    }
}

void deserializeBuffer(const char *buffer, const size_t size, std::string &dbName,
                       std::string &tableName,
                       std::vector<std::shared_ptr<KeyOpFieldsValuesTuple>> &kcos) {
    std::vector<FieldValueTuple> values;
    deserializeBuffer(buffer, size, values);
    int fvs_size = -1;
    KeyOpFieldsValuesTuple kco;
    auto &key = kfvKey(kco);
    auto &op = kfvOp(kco);
    auto &fvs = kfvFieldsValues(kco);
    for (auto &fv : values) {
        auto &field = fvField(fv);
        auto &value = fvValue(fv);
        // The first pair is the DB name and the table name.
        if (fvs_size < 0) {
            dbName = field;
            tableName = value;
            fvs_size = 0;
            continue;
        }
        // This is the beginning of a request.
        // The first pair is the key and the number of attributes.
        // If the attribute count is zero, it is a DEL request.
        if (fvs_size == 0) {
            key = field;
            fvs_size = std::stoi(value);
            op = (fvs_size == 0) ? DEL_COMMAND : SET_COMMAND;
            fvs.clear();
        }
        // This is an attribute pair.
        else {
            fvs.push_back(fv);
            --fvs_size;
        }
        // We got the last attribute pair. This is the end of a request.
        if (fvs_size == 0) {
            kcos.push_back(std::make_shared<KeyOpFieldsValuesTuple>(kco));
        }
    }
}

} // namespace swss
