#include "value.h"

#include "gc.h"
#include <cassert>
#include <cstring>

namespace Zeta {

// Array
Array::Array() : Object(Object::Type::Array), size(0), capacity(8){
    Block* blk = Object::gc->allocateBlock(capacity * sizeof(Value));
    Object::gc->writeBarrier(this, (Object**)(&data), blk);
}

Array::Array(uint32_t size) : Object(Object::Type::Array), size(size){
    capacity = 1;
    while(capacity < size) capacity <<= 1;
    Block* blk = Object::gc->allocateBlock(capacity * sizeof(Value));
    Object::gc->writeBarrier(this, (Object**)(&data), blk);
    Value* ptr = (Value*)(data->getData());
    for(uint32_t i = 0; i < size; i++){
        new (ptr + i) Value();
    }
}

void Array::add(const Value& value){
    if(size == capacity){
        capacity <<= 1;
        Block* newData = Object::gc->allocateBlock(capacity * sizeof(Value));
        for(int i = 0; i < size; i++){
            Object::gc->writeBarrier(newData, (Value*)(newData->getData()) + i, ((Value*)(data->getData()))[i]);
        }
        Object::gc->writeBarrier(this, (Object**)(&data), newData);
    }
    Value* ptr = (Value*)(data->getData());
    Object::gc->writeBarrier(data, ptr + size, value);
    size++;
}

void Array::set(uint32_t index, const Value& value){
    assert(index < size); // caller should check the index first
    Value* ptr = (Value*)(data->getData());
    Object::gc->writeBarrier(data, ptr + index, value);
}

Value Array::get(uint32_t index) const{
    assert(index < size);
    Value* ptr = (Value*)(data->getData());
    return ptr[index];
}

// Map
Map::Map() : Object(Object::Type::Map), capacity(8), size(0), deletedCnt(0){
    Block* blk = Object::gc->allocateBlock(capacity * sizeof(Entry));
    Object::gc->writeBarrier(this, (Object**)(&data), blk);
    std::memset(data->getData(), 0, data->size);
} 

Map::Map(uint32_t minCapacity) : Object(Object::Type::Map), capacity(minCapacity), size(0), deletedCnt(0){
    capacity = 1;
    while(capacity < minCapacity) capacity <<= 1;
    Block* blk = Object::gc->allocateBlock(capacity * sizeof(Entry));
    Object::gc->writeBarrier(this, (Object**)(&data), blk);
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
            Object::gc->writeBarrier(data, &entries[idx].value, value);
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
    Object::gc->writeBarrier(data, &entries[idx].value, value);
    size++;
}

Value Map::get(String* key) const{
    uint32_t idx = hash(key) & (capacity - 1);
    Entry* entries = (Entry*)(data->getData());
    while(entries[idx].key != EMPTY){
        if(entries[idx].key == key){
            return entries[idx].value;
        }
        idx = (idx + 1) & (capacity - 1);
    }
    return Value();
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
    Block* newData = Object::gc->allocateBlock(capacity * sizeof(Entry));
    std::memset(newData->getData(), 0, newData->size);
    Object::gc->writeBarrier(this, (Object**)(&data), newData);
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
            Object::gc->writeBarrier(data, &entries[idx].value, oldEntries[i].value);
            size++;
        }
    }
}

}
