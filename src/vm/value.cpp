#include "value.h"

#include "gc.h"

#include <cassert>
#include <cstring>

namespace Zeta {

const Value Value::Null = Value(Value::Type::Null, 0);
const Value Value::Error = Value(Value::Type::Error, 0);

// Array
Array::Array(GC* gc) : Object(Object::Type::Array), size(0), capacity(8), gc(gc){
    Block* blk = gc->allocateBlock(capacity * sizeof(Value));
    gc->writeBarrier(this, (Object**)(&data), blk);
}

Array::Array(GC* gc, uint32_t size) : Object(Object::Type::Array), size(size), gc(gc){
    capacity = 1;
    while(capacity < size) capacity <<= 1;
    Block* blk = gc->allocateBlock(capacity * sizeof(Value));
    gc->writeBarrier(this, (Object**)(&data), blk);
    Value* ptr = (Value*)(data->getData());
    for(uint32_t i = 0; i < size; i++){
        new (ptr + i) Value();
    }
}

void Array::add(const Value& value){
    if(size == capacity){
        capacity <<= 1;
        Block* newData = gc->allocateBlock(capacity * sizeof(Value));
        for(int i = 0; i < size; i++){
            gc->writeBarrier(newData, (Value*)(newData->getData()) + i, ((Value*)(data->getData()))[i]);
        }
        gc->writeBarrier(this, (Object**)(&data), newData);
    }
    Value* ptr = (Value*)(data->getData());
    gc->writeBarrier(data, ptr + size, value);
    size++;
}

void Array::set(uint32_t index, const Value& value){
    assert(index < size); // caller should check the index first
    Value* ptr = (Value*)(data->getData());
    gc->writeBarrier(data, ptr + index, value);
}

Value Array::get(uint32_t index) const{
    assert(index < size);
    Value* ptr = (Value*)(data->getData());
    return ptr[index];
}

// Map
Map::Map(GC* gc) : Object(Object::Type::Map), capacity(8), size(0), deletedCnt(0), gc(gc){
    Block* blk = gc->allocateBlock(capacity * sizeof(Entry));
    gc->writeBarrier(this, (Object**)(&data), blk);
    std::memset(data->getData(), 0, data->size);
} 

Map::Map(GC* gc, uint32_t minCapacity) : Object(Object::Type::Map), size(0), deletedCnt(0), gc(gc){
    capacity = 4;
    while(capacity < minCapacity) capacity <<= 1;
    Block* blk = gc->allocateBlock(capacity * sizeof(Entry));
    gc->writeBarrier(this, (Object**)(&data), blk);
    std::memset(data->getData(), 0, data->size);
}

void Map::set(String* key, const Value& value){
    if((size + deletedCnt) * 2 > capacity){
        rehash(capacity * 2);
    }
    uint32_t idx = hash(key) & (capacity - 1);
    uint32_t firstDeleted = capacity;
    Entry* entries = (Entry*)(data->getData());
    while(entries[idx].key != EMPTY){
        if(entries[idx].key == key){
            gc->writeBarrier(data, &entries[idx].value, value);
            return;
        }
        if(firstDeleted == capacity && entries[idx].key == DELETED){
            firstDeleted = idx;
        }
        idx = (idx + 1) & (capacity - 1);
    }
    if(firstDeleted < capacity){
        idx = firstDeleted;
        deletedCnt--;
    }
    entries[idx].key = key;
    gc->writeBarrier(data, &entries[idx].value, value);
    size++;
}

std::optional<Value> Map::get(String* key) const{
    uint32_t idx = hash(key) & (capacity - 1);
    Entry* entries = (Entry*)(data->getData());
    while(entries[idx].key != EMPTY){
        if(entries[idx].key == key){
            return std::optional<Value>(entries[idx].value);
        }
        idx = (idx + 1) & (capacity - 1);
    }
    return std::nullopt;
}

bool Map::contains(String* key) const {
    uint32_t idx = hash(key) & (capacity - 1);
    Entry* entries = (Entry*)(data->getData());
    while(entries[idx].key != EMPTY){
        if(entries[idx].key == key){
            return true;
        }
        idx = (idx + 1) & (capacity - 1);
    }
    return false;
}

bool Map::remove(String* key){
    uint32_t idx = hash(key) & (capacity - 1);
    Entry* entries = (Entry*)(data->getData());
    while(entries[idx].key != EMPTY){
        if(entries[idx].key == key){
            entries[idx].key = DELETED;
            size--;
            deletedCnt++;
            return true;
        }
        idx = (idx + 1) & (capacity - 1);
    }
    return false;
}

// newCapacity must be the power of 2 and larger than old capacity.
void Map::rehash(uint32_t newCapacity){
    Entry* oldEntries = (Entry*)(data->getData());
    uint32_t oldCapacity = capacity;

    capacity = newCapacity;
    Block* newData = gc->allocateBlock(capacity * sizeof(Entry));
    std::memset(newData->getData(), 0, newData->size);
    gc->writeBarrier(this, (Object**)(&data), newData);
    Entry* entries = (Entry*)(data->getData());
    size = 0;
    deletedCnt = 0;

    for(uint32_t i = 0; i < oldCapacity; i++){
        if(oldEntries[i].key != EMPTY && oldEntries[i].key != DELETED){
            uint32_t idx = hash(oldEntries[i].key) & (capacity - 1);
            while(entries[idx].key != EMPTY){
                idx = (idx + 1) & (capacity - 1);
            }
            entries[idx].key = oldEntries[i].key;
            gc->writeBarrier(data, &entries[idx].value, oldEntries[i].value);
            size++;
        }
    }
}

// Class
Class::Class(GC* gc, String* name, Class* base, Map* fields, Map* methods) : Object(Object::Type::Class), name(name) {
    gc->writeBarrier(this, (Object**)(&this->base), base);
    gc->writeBarrier(this, (Object**)(&this->fields), fields);
    gc->writeBarrier(this, (Object**)(&this->methods), methods);
}

// Instance
Instance::Instance(GC* gc, Class* cls) : Object(Object::Type::Instance) {
    gc->writeBarrier(this, (Object**)(&this->cls), cls);
    Map* fields = gc->allocate<Map>(gc, cls->fields->size);
    cls->fields->forEach([&fields](String* key, const Value& value){
        fields->set(key, value);
    });
    gc->writeBarrier(this, (Object**)(&this->fields), fields);
}

// Iterator
Iterator::Iterator(GC* gc, Object* container) : Object(Type::Iterator), index(0) {
    assert(container->type == Object::Type::Array || container->type == Object::Type::Map);
    gc->writeBarrier(this, &this->container, container);
}

// String object
StrObj::StrObj(GC* gc, const char* str, uint32_t len) : Object(Object::Type::StrObj), length(len) {
    Block* blk = gc->allocateBlock(len + 1);
    std::memcpy(blk->getData(), str, len);
    ((char*)blk->getData())[len] = '\0';
    gc->writeBarrier(this, (Object**)(&data), blk);
}

StrObj::StrObj(GC* gc, StrView str1, StrView str2) : Object(Object::Type::StrObj), length(str1.length + str2.length) {
    Block* blk = gc->allocateBlock(length + 1);
    std::memcpy(blk->getData(), str1.data, str1.length);
    std::memcpy((char*)blk->getData() + str1.length, str2.data, str2.length);
    ((char*)blk->getData())[length] = '\0';
    gc->writeBarrier(this, (Object**)(&data), blk);
}

}
