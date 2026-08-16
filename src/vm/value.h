#pragma once

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string_view>
#include <type_traits>
#include <vector>
#include <string>

#include "../utils/utils.h"

namespace Zeta {

class Object;
class String;
class GC;
struct Routine;
class Class;
struct Value;
class VM;

using NativeFunction = void(*)(VM* vm, int argc);

// 16 Bytes
struct Value {
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

    class ProxyInt {
    private:
        Value& value;
        uint32_t index;
    public:
        ProxyInt(Value& value, uint32_t index) : value(value), index(index) {}
        operator Value() const;
        ProxyInt& operator=(const Value& val);
    };
    class ProxyStr {
    private:
        Value& value;
        String* key;
    public:
        ProxyStr(Value& value, String* key) : value(value), key(key) {}
        operator Value() const;
        ProxyStr& operator=(const Value& val);
    };

    ProxyInt operator[](uint32_t index) { return ProxyInt(*this, index); }
    Value operator[](uint32_t index) const;
    ProxyStr operator[](String* key) { return ProxyStr(*this, key); }
    Value operator[](String* key) const;

    bool isNumber() const;
    bool isString() const;
    bool isFunction() const;
    bool isArray() const;
    bool isMap() const;
    bool isClass() const;
    bool isInstance() const;

    template<typename T>
    std::optional<T> as() const;
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
class String {
public:
    friend class VM;
    friend class Map;
    friend class Value;

    const char* getData() const { return data; }
    uint32_t getLength() const { return length; }
    uint32_t getHash() const { return hash; }

    ~String() {
        std::free(data);
    }

private:
    char* data;
    uint32_t length;
    uint32_t hash;

    String(const char* str, uint32_t len) : length(len) {
        data = static_cast<char*>(std::malloc(len + 1));
        std::memcpy(data, str, len);
        data[len] = '\0';
        hash = Utils::hashString(data, len);
    }
};

// allocated on heap, managed by GC
// 16 Bytes
class Object {
public:
    friend class GC;
    friend class VM;
    friend class Iterator;
    friend class Value;

    enum class Type : uint8_t {
        Block, // 16 bytes head + (size - 16) bytes data
        Array,
        Map,
        Class,
        Instance, 
        Iterator,
        StrObj
    };

    Type getType() const { return type; }

protected:
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
class Block : public Object {
public:
    friend class GC;
    friend class VM;
    friend class Array;
    friend class Map;
    friend class StrObj;
    friend class Iterator;
    friend class Object;
    friend class Value;

    enum class ElemType : uint8_t {
        Array,
        Map,
        StrObj
    };

private:
    ElemType elemType;
    uint32_t size; // size of the valid data, exclude the size of the Block instance itself.

    void* getData() { return static_cast<void*>(this + 1); } // 8 bytes alignment
    
    Block() = delete;
    Block(uint32_t size, ElemType elemType) : Object(Type::Block), size(size), elemType(elemType) {}
};

// 40 Bytes
class Array : public Object {
public:
    friend class GC;
    friend class VM;
    friend class Iterator;
    friend class Object;
    friend class Value;

    uint32_t getSize() const { return size; }
    uint32_t getCapacity() const { return capacity; }

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

private:
    uint32_t size;
    uint32_t capacity;
    Block* data;
    GC* gc;

    Array(GC* gc);
    Array(GC* gc, uint32_t size);
};

// 48 Bytes
class Map : public Object {
public:
    friend class GC;
    friend class VM;
    friend class Iterator;
    friend class Object;
    friend class Value;
    friend class Instance;

    struct Entry {
        String* key;
        Value value;
    };

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
    Block* data;
    uint32_t capacity;
    uint32_t size;
    uint32_t deletedCnt;
    GC* gc;

    inline static String* const EMPTY = nullptr;
    inline static String* const DELETED = reinterpret_cast<String*>(0x1);

    Map(GC* gc);
    Map(GC* gc, uint32_t capacity);
    void rehash(uint32_t newCapacity);
};

// 48 Bytes
class Class : public Object {
public:
    friend class GC;
    friend class VM;
    friend class Object;
    friend class Instance;

    String* getName() const { return name; }
    Class* getBase() const { return base; }
    std::optional<Value> getMethod(String* methodName) const { return methods->get(methodName); }

private:
    String* name;
    Class* base;
    Map* fields; // field name -> default value
    Map* methods;
      
    Class(GC* gc, String* name, Class* base, Map* fields, Map* methods);
};

// 32 Bytes
class Instance : public Object {
public:
    friend class GC;
    friend class VM;
    friend class Object;

    Class* getClass() const { return cls; }
    std::optional<Value> getField(String* fieldName) const { return fields->get(fieldName); }
    void setField(String* fieldName, const Value& value) { fields->set(fieldName, value); }

    class Proxy {
    private:
        Instance& instance;
        String* fieldName;
    public:
        Proxy(Instance& instance, String* fieldName) : instance(instance), fieldName(fieldName) {}
        operator Value() const {
            return instance.getField(fieldName).value_or(Value::Error);
        }
        Proxy& operator=(const Value& value) {
            instance.setField(fieldName, value);
            return *this;
        }
    };

    Proxy operator[](String* fieldName) {
        return Proxy(*this, fieldName);
    }
    
    Value operator[](String* fieldName) const {
        return getField(fieldName).value_or(Value::Error);
    }

private:
    Class* cls;
    Map* fields; // field name -> value

    Instance(GC* gc, Class* cls);
};

// 32 Bytes
class Iterator : public Object {
public:
    friend class GC;
    friend class VM;
    friend class Object;

private:
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
class StrObj : public Object {
public:
    friend class GC;
    friend class VM;
    friend class Object;
    friend class Value;

    const char* getData() const { return static_cast<char*>(data->getData()); }
    uint32_t getLength() const { return length; }

private:
    Block* data;
    uint32_t length;

    StrObj(GC* gc, const char* str, uint32_t len);
    StrObj(GC* gc, std::string_view str1, std::string_view str2);
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

inline Value::ProxyInt::operator Value() const {
    if (value.type == Value::Type::Object && value.ptrValue->type == Object::Type::Array) {
        Array* arr = static_cast<Array*>(value.ptrValue);
        return arr->get(index);
    }
    return Value::Error;
}

inline Value::ProxyInt& Value::ProxyInt::operator=(const Value& val) {
    if (value.type == Value::Type::Object && value.ptrValue->type == Object::Type::Array) {
        Array* arr = static_cast<Array*>(value.ptrValue);
        arr->set(index, val);
    }
    return *this;
}

inline Value::ProxyStr::operator Value() const {
    if (value.type == Value::Type::Object && value.ptrValue->type == Object::Type::Map) {
        Map* map = static_cast<Map*>(value.ptrValue);
        return map->get(key).value_or(Value::Error);
    } else if (value.type == Value::Type::Object && value.ptrValue->type == Object::Type::Instance) {
        Instance* inst = static_cast<Instance*>(value.ptrValue);
        return inst->getField(key).value_or(Value::Error);
    }
    return Value::Error;
}

inline Value::ProxyStr& Value::ProxyStr::operator=(const Value& val) {
    if (value.type == Value::Type::Object && value.ptrValue->type == Object::Type::Map) {
        Map* map = static_cast<Map*>(value.ptrValue);
        map->set(key, val);
    } else if (value.type == Value::Type::Object && value.ptrValue->type == Object::Type::Instance) {
        Instance* inst = static_cast<Instance*>(value.ptrValue);
        inst->setField(key, val);
    }
    return *this;
}

inline Value Value::operator[](uint32_t index) const {
    if (type == Value::Type::Object && ptrValue->type == Object::Type::Array) {
        Array* arr = static_cast<Array*>(ptrValue);
        return arr->get(index);
    }
    return Value::Error;
}

inline Value Value::operator[](String* key) const {
    if (type == Value::Type::Object && ptrValue->type == Object::Type::Map) {
        Map* map = static_cast<Map*>(ptrValue);
        return map->get(key).value_or(Value::Error);
    } else if (type == Value::Type::Object && ptrValue->type == Object::Type::Instance) {
        Instance* inst = static_cast<Instance*>(ptrValue);
        return inst->getField(key).value_or(Value::Error);
    }
    return Value::Error;
}

inline bool Value::isNumber() const {
    return type == Type::Int || type == Type::Float;
}

inline bool Value::isString() const {
    return type == Type::String || (type == Type::Object && ptrValue->type == Object::Type::StrObj);
}

inline bool Value::isFunction() const {
    return type == Type::Function || type == Type::NativeFunc;
}

inline bool Value::isArray() const {
    return type == Type::Object && ptrValue->type == Object::Type::Array;
}

inline bool Value::isMap() const {
    return type == Type::Object && ptrValue->type == Object::Type::Map;
}

inline bool Value::isClass() const {
    return type == Type::Object && ptrValue->type == Object::Type::Class;
}

inline bool Value::isInstance() const {
    return type == Type::Object && ptrValue->type == Object::Type::Instance;
}

// NOTE: 指针类型和 string_view 需要注意生命周期和潜在的 GC 时机, 避免悬空. 不要长期保存.
template<typename T>
inline std::optional<T> Value::as() const {
    if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>) {
        if (type == Value::Type::Int) {
            return std::make_optional(static_cast<T>(intValue));
        } else if (type == Value::Type::Float) {
            return std::make_optional(static_cast<T>(floatValue));
        } else {
            return std::nullopt;
        }
    } else if constexpr (std::is_floating_point_v<T>) {
        if (type == Value::Type::Float) {
            return std::make_optional(static_cast<T>(floatValue));
        } else if (type == Value::Type::Int) {
            return std::make_optional(static_cast<T>(intValue));
        } else {
            return std::nullopt;
        }
    } else if constexpr (std::is_same_v<T, bool>) {
        return type == Value::Type::Bool ? std::make_optional(boolValue) : std::nullopt;
    } else if constexpr (std::is_same_v<T, String*>) {
        return type == Value::Type::String ? std::make_optional(strValue) : std::nullopt;
    } else if constexpr (std::is_same_v<T, Routine*>) {
        return type == Value::Type::Function ? std::make_optional(funcValue) : std::nullopt;
    } else if constexpr (std::is_same_v<T, NativeFunction>) {
        return type == Value::Type::NativeFunc ? std::make_optional(nativeFuncValue) : std::nullopt;
    } else if constexpr (std::is_same_v<T, Object*>) {
        return type == Value::Type::Object ? std::make_optional(ptrValue) : std::nullopt;
    } else if constexpr (std::is_same_v<T, Block*>) {
        return (type == Value::Type::Object && ptrValue->type == Object::Type::Block) ? std::make_optional(static_cast<Block*>(ptrValue)) : std::nullopt;
    } else if constexpr (std::is_same_v<T, Array*>) {
        return (type == Value::Type::Object && ptrValue->type == Object::Type::Array) ? std::make_optional(static_cast<Array*>(ptrValue)) : std::nullopt;
    } else if constexpr (std::is_same_v<T, Map*>) {
        return (type == Value::Type::Object && ptrValue->type == Object::Type::Map) ? std::make_optional(static_cast<Map*>(ptrValue)) : std::nullopt;
    } else if constexpr (std::is_same_v<T, Class*>) {
        return (type == Value::Type::Object && ptrValue->type == Object::Type::Class) ? std::make_optional(static_cast<Class*>(ptrValue)) : std::nullopt;
    } else if constexpr (std::is_same_v<T, Instance*>) {
        return (type == Value::Type::Object && ptrValue->type == Object::Type::Instance) ? std::make_optional(static_cast<Instance*>(ptrValue)) : std::nullopt;
    } else if constexpr (std::is_same_v<T, Iterator*>) {
        return (type == Value::Type::Object && ptrValue->type == Object::Type::Iterator) ? std::make_optional(static_cast<Iterator*>(ptrValue)) : std::nullopt;
    } else if constexpr (std::is_same_v<T, StrObj*>) {
        return (type == Value::Type::Object && ptrValue->type == Object::Type::StrObj) ? std::make_optional(static_cast<StrObj*>(ptrValue)) : std::nullopt;
    } else if constexpr (std::is_same_v<T, std::string_view>) {
        if (type == Value::Type::String) {
            return std::make_optional(std::string_view(strValue->data, strValue->length));
        } else if (type == Value::Type::Object && ptrValue->type == Object::Type::StrObj) {
            StrObj* strObj = static_cast<StrObj*>(ptrValue);
            return std::make_optional(std::string_view(strObj->getData(), strObj->length));
        } else {
            return std::nullopt;
        }
    } else if constexpr (std::is_same_v<T, std::string>) {
        if (type == Value::Type::String) {
            return std::make_optional(std::string(strValue->data, strValue->length));
        } else if (type == Value::Type::Object && ptrValue->type == Object::Type::StrObj) {
            StrObj* strObj = static_cast<StrObj*>(ptrValue);
            return std::make_optional(std::string(strObj->getData(), strObj->length));
        } else {
            return std::nullopt;
        }
    } else {
        static_assert(false, "Unsupported type for Value::as<T>()");
    }
}

}
