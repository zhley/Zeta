#pragma once

#include "utils/utils.h"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <vector>

namespace Zeta {

struct Object;
struct String;
struct GC;

struct Value{
    static const Value Null;
    static const Value Error;

    enum class Type : uint8_t {
        Null = 0x00,
        Int,
        Float,
        Bool,
        String, // interned string
        Object,
        Error // flag for error, val is error code
    } type = Type::Null;
    union {
        int64_t intValue;
        bool boolValue;
        double floatValue;
        Object* ptrValue;
        String* strValue;
        uint64_t val;
    };

    explicit Value() : type(Type::Null) {}
    explicit Value(int64_t i) : type(Type::Int), intValue(i) {}
    explicit Value(double f) : type(Type::Float), floatValue(f) {}
    explicit Value(bool b) : type(Type::Bool), boolValue(b) {}
    explicit Value(Object* obj) : type(Type::Object), ptrValue(obj) {}
    explicit Value(String* str) : type(Type::String), strValue(str) {}
    Value(Type t, uint64_t v) : type(t), val(v) {}
    Value(const Value& other) : type(other.type), val(other.val) {}

    // strict equality check
    bool operator==(const Value& other) const {
        return type == other.type && val == other.val;
    }

    bool operator!=(const Value& other) const {
        return !(*this == other);
    }
};

struct Routine{
    std::vector<uint8_t> bytecode;
    std::vector<Value> constants;
    uint32_t arity;
    uint32_t localCount;
    uint32_t maxStackSize; // max operand stack size
};

// allocated on heap, managed by GC
struct Object {
    enum class Type : uint8_t {
        Block, // 8 bytes head + (size - 8) bytes data
        Array,
        Map,
        Function,
        Class,
        Instance, 
        Iterator,
    };

    // head (8 bytes)
    Type type;
    uint8_t age;
    struct {
        uint64_t forward: 63;
        bool marked : 1;
    } gcWord; // for GC

    Object(Type t) : type(t) {}
    int getSize() const;

    template<typename F>
    requires std::is_invocable_v<F, Object**>
    void trace(F&& f);
};

// Block can only be allocated by GC::allocateBlock().
struct Block : public Object {
    friend class GC;

    int64_t size; // size of the valid data, exclude the size of the Block instance itself.

    void* getData() { return static_cast<void*>(this + 1); } // 8 bytes alignment
    
private:
    Block() = delete;
    Block(int64_t size) : Object(Type::Block), size(size) {}
};

// immutable string
// TODO: 这个是VM直接管理的字符串, GC管理的字符串要改实现. 目前 String* 都是VM直接管理的常量字符串
// TODO: 实现对象级字符串后, VM::execute()的一些指令逻辑要改
struct String {
    char* data;
    uint32_t length;
    uint32_t hash;

    String(const char* str, uint32_t len) : length(len) {
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
    uint32_t size;
    uint32_t capacity;
    Block* data;
    GC* gc;

    Array(GC* gc);
    Array(GC* gc, uint32_t size);

    void add(const Value& value);
    void set(uint32_t index, const Value& value);
    Value get(uint32_t index) const;

    // function f should not modify the array because write barrier must be called when elements are modified.
    template<typename F>
    requires std::is_invocable_v<F, const Value&> || std::is_invocable_v<F, Value>
    void forEach(F&& f) const {
        Value* entries = (Value*)(data->getData());
        for(uint32_t i = 0; i < size; i++){
            f(entries[i]);
        }
    }
};

struct Map : public Object {
    struct Entry {
        String* key;
        Value value;
    };
    
    Block* data;
    uint32_t capacity;
    uint32_t size;
    uint32_t deletedCnt;
    GC* gc;

    inline static String* const EMPTY = nullptr;
    inline static String* const DELETED = reinterpret_cast<String*>(0x1);

    Map(GC* gc);
    Map(GC* gc, uint32_t capacity);
    void set(String* key, const Value& value);
    std::optional<Value> get(String* key) const;
    bool contains(String* key) const;
    bool remove(String* key);

    template<typename F>
    requires std::is_invocable_v<F, String*, const Value&> || std::is_invocable_v<F, String*, Value>
    void forEach(F&& f) const {
        Entry* entries = (Entry*)(data->getData());
        for(uint32_t i = 0; i < capacity; i++){
            if(entries[i].key != EMPTY && entries[i].key != DELETED){
                f(entries[i].key, entries[i].value);
            }
        }
    }

    static uint32_t hash(const String* str) {
        return str->hash;
    }
    static bool equal(const String* a, const String* b) {
        // String interning ensures that the second path is generally not triggered
        return a == b || (a->length == b->length && std::memcmp(a->data, b->data, a->length) == 0); 
    }
private:
    void rehash(uint32_t newCapacity);
};

struct Function : public Object {
    Routine* routine;

    Function() : Object(Type::Function), routine(nullptr) {}
    Function(Routine* r) : Object(Type::Function), routine(r) {}
};

struct Class : public Object {
    String* name;
    Class* base;
    Map* fields; // field name -> default value
    Map* methods;
      
    Class(GC* gc, String* name, Class* base, Map* fields, Map* methods);
};

struct Instance : public Object {
    Class* cls;
    Map* fields; // field name -> value

    Instance(GC* gc, Class* cls);
};

struct Iterator : public Object {
    Object* container; // array or map;
    int index;

    explicit Iterator(GC* gc, Object* container);

    Value next() {
        if(container->type == Object::Type::Array){
            Array* arr = static_cast<Array*>(container);
            if(index < arr->size){
                return arr->get(index++);
            } else {
                return Value::Error;
            }
        } else if(container->type == Object::Type::Map){
            Map* map = static_cast<Map*>(container);
            while(index < map->capacity){
                Map::Entry* entries = (Map::Entry*)(map->data->getData());
                if(entries[index].key != Map::EMPTY && entries[index].key != Map::DELETED){
                    return Value(entries[index++].key);
                }
                index++;
            }
            return Value::Error;
        }
        assert(false);
        return Value::Error;
    }
};

inline int Object::getSize() const {
    switch (type) {
        case Object::Type::Block:       return static_cast<const Block*>(this)->size + sizeof(Block);
        case Object::Type::Array:       return sizeof(Array);
        case Object::Type::Map:         return sizeof(Map);
        case Object::Type::Function:    return sizeof(Function);
        case Object::Type::Class:       return sizeof(Class);
        case Object::Type::Instance:    return sizeof(Instance);
        case Object::Type::Iterator:    return sizeof(Iterator);
    }
    return 0;
}

template<typename F>
requires std::is_invocable_v<F, Object**>
inline void Object::trace(F&& f) {
    switch (type) {
        case Object::Type::Block: break; // actually, Block may contain references to other objects, but it does not know how to trace them, so they will be traced by the owner of the Block. (The owner of the Block is responsible for tracing the references in the Block.)
        case Object::Type::Array: {
            Array* arr = static_cast<Array*>(this);
            f((Object**)(&(arr->data)));
            // equivalent to arr->data->trace(f)
            Value* elem = (Value*)(arr->data->getData());
            for(uint32_t i = 0; i < arr->size; i++){
                if(elem->type == Value::Type::Object){
                    f(&elem->ptrValue);
                }
            }
            break;
        }
        case Object::Type::Map: {
            Map* map = static_cast<Map*>(this);
            f((Object**)(&(map->data)));
            // equivalent to map->data->trace(f)
            map->forEach([&f](const String* key, Value value){
                if(value.type == Value::Type::Object){
                    f(&value.ptrValue);
                }
            });
            break;
        }
        case Object::Type::Function: break;
        case Object::Type::Class: {
            Class* cls = static_cast<Class*>(this);
            f((Object**)(&(cls->base)));
            f((Object**)(&(cls->fields)));
            f((Object**)(&(cls->methods)));
            break;
        }
        case Object::Type::Instance: {
            Instance* instance = static_cast<Instance*>(this);
            f((Object**)(&(instance->cls)));
            f((Object**)(&(instance->fields)));
            break;
        }
        case Object::Type::Iterator: {
            Iterator* iter = static_cast<Iterator*>(this);
            f(&(iter->container));
            break;
        }
    }
}

}
