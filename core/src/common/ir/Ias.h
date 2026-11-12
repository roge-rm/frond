#ifndef FROND_IR_IAS_H
#define FROND_IR_IAS_H

// The IAS server on LSAP 0 (IrLMP 1.1 section 4.x, Information Access Service): answers
// GetValueByClass for the classes and attributes added to it.

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "IrLmp.h"

namespace frondIr {

class IasServer : public LmService {
   public:
    static constexpr uint8_t lsap = 0x00;
    // GetValueByClass return codes.
    static constexpr uint8_t success = 0, noSuchClass = 1, noSuchAttribute = 2;

    explicit IasServer(IrLmp& lmp);

    void AddInteger(const std::string& className, const std::string& attribute, uint32_t value);
    void AddOctets(const std::string& className, const std::string& attribute, const Bytes& value);
    void AddString(const std::string& className, const std::string& attribute, const std::string& value);

    // The last queries answered, for tests; older ones are let go.
    struct Query {
        std::string className, attribute;
        uint8_t result;
    };
    static constexpr size_t keptQueries = 16;
    const std::deque<Query>& Queries() const { return queries; }
    // The longest request taken: the opcode's arguments, a class and an attribute name of up to
    // 255 bytes each with their lengths. A longer one is dropped whole.
    static constexpr size_t maxRequest = 2 + 2 * 255;

    bool Connect(const Bytes& data, Bytes& reply, uint64_t now) override;
    void Data(const Bytes& data, uint64_t now) override;
    void Disconnect(uint64_t now) override;

   private:
    struct Attribute {
        std::string className, name;
        uint16_t object;
        Bytes value;  // type byte then the encoded value
    };

    void Add(const std::string& className, const std::string& attribute, Bytes value);
    void Request(const Bytes& request);

    IrLmp& lmp;
    std::vector<Attribute> attributes;
    std::vector<std::string> classes;
    Bytes pending;
    bool overflow{false};
    std::deque<Query> queries;
};

}  // namespace frondIr

#endif
