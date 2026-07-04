#pragma once

#include "utils/utils.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace Zeta {

struct Object;

struct Value{
    enum class Type : uint8_t {
        Null = 0x00,
        Int,
        Float,
        Bool,
        Object
    } type = Type::Null;
    union {
        int64_t intValue;
        double floatValue;
        Object* ptrValue;
    };

    Value() : type(Type::Null) {}
    Value(int64_t i) : type(Type::Int), intValue(i) {}
    Value(double f) : type(Type::Float), floatValue(f) {}
    Value(bool b) : type(Type::Bool), intValue(b ? 1 : 0) {}
    Value(Object* obj) : type(Type::Object), ptrValue(obj) {}
};

struct Routine{
    std::vector<uint8_t> bytecode;
    std::vector<Value> constants;
    uint32_t arity;
    uint32_t localCount;
    uint32_t maxStackSize;
};

struct Object {
    enum class Type : uint8_t {
        Block, // 8 bytes head + (size - 8) bytes data
        String,
        Array,
        Map,
        Function,
        Class,
        Instance
    };
    // head (8 bytes)
    Type type;
    uint8_t age;
    struct {
        uint64_t forward: 62;
        bool marked : 1;
        bool remembered : 1; // valid for young objects, indicates whether the field corresponding to the object in the old generation is in the remembered set
    } gcWord; // for GC

    Object(Type t) : type(t) {}
    int getSize() const;

    template<typename F>
    void trace(F&& f);
};

struct Block : public Object {
    int64_t size;

    Block(int64_t size) : Object(Type::Block), size(size) {}
    void* getData() { return static_cast<void*>(this + 1); }
};

// immutable string
struct String : public Object {
    char* data;
    uint32_t length;
    uint32_t hash;

    String(const char* str, uint32_t len) : Object(Type::String), length(len) {
        data = static_cast<char*>(std::malloc(len + 1));
        std::memcpy(data, str, len);
        data[len] = '\0';
        hash = Utils::hashString(data, len);
    }
    ~String() {
        std::free(data);
    }
};

struct Array : public Object {
    Value* elements;
    uint32_t length;
};

struct StringHash {
    size_t operator()(const String* str) const {
        return str->hash;
    }
};
struct StringEqual {
    bool operator()(const String* a, const String* b) const {
        // String interning ensures that the second path is generally not triggered
        return a == b || (a->length == b->length && std::memcmp(a->data, b->data, a->length) == 0); 
    }
};
template <typename T>
using StringMap = std::unordered_map<String*, T, StringHash, StringEqual>;

struct Map : public Object {
    StringMap<Value> entries;
};

struct Function : public Object {
    Routine* routine;

    Function() : Object(Type::Function), routine(nullptr) {}
    Function(Routine* r) : Object(Type::Function), routine(r) {}
};

struct Class : public Object {
    String* name;
    Class* base;
    StringMap<Value> fields; // field name -> default value
    StringMap<Value> methods;

    Class() : Object(Type::Class), name(nullptr), base(nullptr) {}
};

struct Instance : public Object {
    Class* cls;
    StringMap<Value> fields;
};

inline int Object::getSize() const {
    switch (type) {
        case Object::Type::Block:       return static_cast<const Block*>(this)->size;
        case Object::Type::String:      return sizeof(String);
        case Object::Type::Array:       return sizeof(Array);
        case Object::Type::Map:         return sizeof(Map);
        case Object::Type::Function:    return sizeof(Function);
        case Object::Type::Class:       return sizeof(Class);
        case Object::Type::Instance:    return sizeof(Instance);
    }
}

template<typename F>
inline void Object::trace(F&& f) {
    switch (type) {
        // TODO
    }
}

}
