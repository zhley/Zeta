#pragma once

#include "utils/utils.h"
#include <cstdint>
#include <cstring>
#include <unordered_map>

namespace Zeta {

struct Proto;
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
};

struct Object{
    enum class Type : uint8_t {
        String,
        Array,
        Map,
        Function,
        Class,
        Instance
    } type;
};

// immutable string
struct String : public Object {
    char* data;
    uint32_t length;
    uint32_t hash;

    String(const char* str, uint32_t len) : length(len) {
        type = Type::String;
        data = new char[len + 1];
        std::memcpy(data, str, len);
        data[len] = '\0';
        hash = Utils::hashString(data, len);
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
    Proto* proto;
};

struct Class : public Object {
    String* name;
    Class* base;
    StringMap<Value> fields; // field name -> default value
    StringMap<Value> methods;
};

struct Instance : public Object {
    Class* cls;
    StringMap<Value> fields;
};

}
