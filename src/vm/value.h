#pragma once

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <vector>
#include <string>

#include "../utils/utils.h"

namespace Zeta {

struct Object;
struct String;
class GC;
struct Routine;
struct Class;
struct Value;
class VM;

struct StrView {
    const char* data;
    uint32_t length;
};

using NativeFunction = Value(*)(VM* vm, int argc, Value* argv);

// 16 Bytes
struct Value{
    static const Value Null;
    static const Value Error;

    enum class Type : uint8_t {
        Null = 0x00,
        Int,
        Float,
        Bool,
        String, // interned string
        Function,
        NativeFunc,
        Object,
        Error // flag for error, val is error code
    } type = Type::Null;
    union {
        int64_t intValue;
        bool boolValue;
        double floatValue;
        String* strValue;
        Routine* funcValue;
        NativeFunction nativeFuncValue;
        Object* ptrValue; // NOTE: 只要类型是 Object, ptrValue 就必定不是 nullptr, 也就是程序需要在任何情况下都能断言assert(ptrValue != nullptr)
        uint64_t val;
    };

    explicit Value() : type(Type::Null) {}
    explicit Value(int64_t i) : type(Type::Int), intValue(i) {}
    explicit Value(double f) : type(Type::Float), floatValue(f) {}
    explicit Value(bool b) : type(Type::Bool), boolValue(b) {}
    explicit Value(String* str) : type(Type::String), strValue(str) {}
    explicit Value(Routine* routine) : type(Type::Function), funcValue(routine) {}
    explicit Value(NativeFunction func) : type(Type::NativeFunc), nativeFuncValue(func) {}
    explicit Value(Object* obj) : type(Type::Object), ptrValue(obj) {}
    Value(Type t, uint64_t v) : type(t), val(v) {}

    // strict equality check
    bool operator==(const Value& other) const {
        return type == other.type && val == other.val;
    }

    bool operator!=(const Value& other) const {
        return !(*this == other);
    }

    explicit operator bool() const;
    bool isString() const;
    StrView asString() const;
};

struct Routine{
    std::vector<uint8_t> bytecode;
    std::vector<std::pair<uint32_t, uint32_t>> lineInfo;
    std::string moduleName; // the module this routine belongs to
    std::vector<Value> constants;
    uint32_t arity;
    uint32_t localCount;
    uint32_t maxStackSize; // max operand stack size
    Class* ownerClass = nullptr; // the class this method is defined in, nullptr for plain functions; set at module load time
};

// immutable string
// String directly managed by VM
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

// TODO: 由于这部分和 GC 强相关, 还是得加上访问控制, 尽量避免外部直接访问成员, 以避免GC写屏障遗漏
// TODO: 构造函数先0初始化一次再调用writeBarrier.

// allocated on heap, managed by GC
// 16 Bytes
struct Object {
    enum class Type : uint8_t {
        Block, // 16 bytes head + (size - 16) bytes data
        Array,
        Map,
        Class,
        Instance, 
        Iterator,
        StrObj
    };

    // head (16 bytes)
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
// (24 + size) Bytes
struct Block : public Object {
    friend class GC;

    enum class ElemType : uint8_t {
        Array,
        Map,
        StrObj
    };

    ElemType elemType;
    uint32_t size; // size of the valid data, exclude the size of the Block instance itself.

    void* getData() { return static_cast<void*>(this + 1); } // 8 bytes alignment
    
private:
    Block() = delete;
    Block(uint32_t size, ElemType elemType) : Object(Type::Block), size(size), elemType(elemType) {}
};

// 40 Bytes
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

// 48 Bytes
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

// 48 Bytes
struct Class : public Object {
    String* name;
    Class* base;
    Map* fields; // field name -> default value
    Map* methods;
      
    Class(GC* gc, String* name, Class* base, Map* fields, Map* methods);
};

// 32 Bytes
struct Instance : public Object {
    Class* cls;
    Map* fields; // field name -> value

    Instance(GC* gc, Class* cls);
};

// 32 Bytes
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

// normal immutable string object, allocated on heap, managed by GC 
// 32 Bytes
struct StrObj : public Object {
    Block* data;
    uint32_t length;

    StrObj(GC* gc, const char* str, uint32_t len);
    StrObj(GC* gc, StrView str1, StrView str2);
};

inline int Object::getSize() const {
    switch (type) {
        case Object::Type::Block:       return static_cast<const Block*>(this)->size + sizeof(Block);
        case Object::Type::Array:       return sizeof(Array);
        case Object::Type::Map:         return sizeof(Map);
        case Object::Type::Class:       return sizeof(Class);
        case Object::Type::Instance:    return sizeof(Instance);
        case Object::Type::Iterator:    return sizeof(Iterator);
        case Object::Type::StrObj:      return sizeof(StrObj);
    }
    return 0;
}

// nullptr should not be traced.
template<typename F>
requires std::is_invocable_v<F, Object**>
inline void Object::trace(F&& f) {
    switch (type) {
        case Object::Type::Block: {
            Block* blk = static_cast<Block*>(this);
            switch (blk->elemType) {
                case Block::ElemType::Array: {
                    Value* elem = (Value*)(blk->getData());
                    uint32_t capacity = (blk->size) / sizeof(Value);
                    for(uint32_t i = 0; i < capacity; i++){
                        if(elem[i].type == Value::Type::Object){
                            f(&elem[i].ptrValue);
                        }
                    }
                    break;
                }
                case Block::ElemType::Map: {
                    Map::Entry* entries = (Map::Entry*)(blk->getData());
                    uint32_t capacity = (blk->size) / sizeof(Map::Entry);
                    for(uint32_t i = 0; i < capacity; i++){
                        if(entries[i].key != Map::EMPTY && entries[i].key != Map::DELETED && entries[i].value.type == Value::Type::Object){
                            f(&entries[i].value.ptrValue);
                        }
                    }
                    break;
                }
                case Block::ElemType::StrObj: break; // StrObj does not contain any Object*
                default: assert(false); break;
            }
            break;
        }
        case Object::Type::Array: {
            Array* arr = static_cast<Array*>(this);
            f((Object**)(&(arr->data)));
            break;
        }
        case Object::Type::Map: {
            Map* map = static_cast<Map*>(this);
            f((Object**)(&(map->data)));
            break;
        }
        case Object::Type::Class: {
            Class* cls = static_cast<Class*>(this);
            if(cls->base) {
                f((Object**)(&cls->base));
            }
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
        case Object::Type::StrObj: {
            StrObj* strObj = static_cast<StrObj*>(this);
            f((Object**)(&(strObj->data)));
            break;
        }
    }
}

inline Value::operator bool() const  {
    switch (type) {
        case Type::Null: return false;
        case Type::Int: return static_cast<bool>(intValue);
        case Type::Float: return static_cast<bool>(floatValue);
        case Type::Bool: return boolValue;
        case Type::String: return strValue != nullptr && strValue->length > 0;
        case Type::Function: return true;
        case Type::NativeFunc: return true;
        case Type::Object: {
            switch(ptrValue->type) {
                case Object::Type::Block: return true;
                case Object::Type::Array: return static_cast<Array*>(ptrValue)->size > 0;
                case Object::Type::Map: return static_cast<Map*>(ptrValue)->size > 0;
                case Object::Type::Class: return true;
                case Object::Type::Instance: return true;
                case Object::Type::Iterator: return true;
                case Object::Type::StrObj: return static_cast<StrObj*>(ptrValue)->length > 0;
            }
        }
        case Type::Error: return true;
    }
    return false;
}


inline bool Value::isString() const {
    return type == Type::String || (type == Type::Object && ptrValue->type == Object::Type::StrObj);
}

inline StrView Value::asString() const {
    assert(isString());
    if(type == Type::String){
        return {strValue->data, strValue->length};
    } else {
        StrObj* strObj = static_cast<StrObj*>(ptrValue);
        return {static_cast<const char*>(strObj->data->getData()), strObj->length};
    }
}

}
