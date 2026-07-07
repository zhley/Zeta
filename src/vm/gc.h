#pragma once

#include "value.h"

#include <vector>

namespace Zeta {

class VM;

//=== Hash set for rememberedSet ===
class PointerHashSet {
#define EMPTY nullptr
#define DELETED (reinterpret_cast<Object**>(0x1))
public:
    explicit PointerHashSet(size_t size) : size(0), deletedCount(0) {
        capacity = 1;
        while (capacity < size * 2) {
            capacity <<= 1;
        }
        data.resize(capacity, EMPTY);
    }

    void clear() {
        std::fill(data.begin(), data.end(), EMPTY);
        size = 0;
        deletedCount = 0;
    }

    bool contains(Object** ptr) const {
        if(ptr == nullptr) return false;
        size_t idx = hash(ptr) & (capacity - 1);
        while (data[idx] != EMPTY) {
            if (data[idx] == ptr) return true;
            idx = (idx + 1) & (capacity - 1);
        }
        return false;
    }

    void insert(Object** ptr) {
        if(ptr == nullptr) return;
        if ((size + deletedCount) * 2 > capacity) {
            rehash(capacity * 2);
        }
        size_t idx = hash(ptr) & (capacity - 1);
        size_t firstDeleted = capacity;
        while (data[idx] != EMPTY) {
            if(data[idx] == ptr) return;
            if(data[idx] == DELETED && firstDeleted == capacity) {
                firstDeleted = idx;
            }
            idx = (idx + 1) & (capacity - 1);
        }
        if(firstDeleted != capacity) {
            idx = firstDeleted;
            deletedCount--;
        }
        data[idx] = ptr;
        size++;
    }

    void erase(Object** ptr) {
        if(ptr == nullptr) return;
        size_t idx = hash(ptr) & (capacity - 1);
        while (data[idx] != EMPTY) {
            if (data[idx] == ptr) {
                data[idx] = DELETED;
                size--;
                deletedCount++;
                return;
            }
            idx = (idx + 1) & (capacity - 1);
        }
    }

    size_t getSize() const {
        return size;
    }

    bool empty() const {
        return size == 0;
    }

    template<typename F>
    void forEach(F f) const {
        for (const auto& p : data) {
            if (p != EMPTY && p != DELETED) {
                f(p);
            }
        }
    }

    template<typename F>
    void eraseIf(F f) {
        for (size_t i = 0; i < capacity; ++i) {
            Object** p = data[i];
            if (p != EMPTY && p != DELETED && f(p)) {
                data[i] = DELETED;
                size--;
                deletedCount++;
            }
        }
    }

private:
    std::vector<Object**> data;
    size_t capacity;
    size_t size;
    size_t deletedCount;

    static size_t hash(Object** ptr) {
        return reinterpret_cast<size_t>(ptr) >> 3; // 8-byte alignment
    }

    void rehash(size_t newCapacity) {
        std::vector<Object**> oldData = std::move(data);
        size_t oldCapacity = capacity;

        capacity = newCapacity;
        data.assign(capacity, EMPTY);
        size = 0;
        deletedCount = 0;

        for (size_t i = 0; i < oldCapacity; ++i) {
            Object** p = oldData[i];
            if (p != EMPTY && p != DELETED) {
                size_t idx = hash(p) & (capacity - 1);
                while (data[idx] != EMPTY) {
                    idx = (idx + 1) & (capacity - 1);
                }
                data[idx] = p;
                size++;
            }
        }
    }

#undef EMPTY
#undef DELETED
};

//=== GC ===

/*

分代回收策略:
新生代:
    采用复制算法, 分Eden区和Survivor区. 对象在Eden区创建, Survivor区存放活下来的对象, 分两个半区, 只有一个半区存放对象.
    Minor GC时, 将Eden区和Survivor半区中存活的对象复制到空闲Survivor半区.
老年代:
    采用标记-压缩式算法.

堆区 8 字节对齐

堆区结构:
+------------------------+----------------+----------------+--------------------+
|         eden           |     from(to)   |     to(from)   |        old         |
+------------------------+----------------+----------------+--------------------+

*/

// fixed configuration
#define ZETA_GC_YOUNG_SCALE 1
#define ZETA_GC_OLD_SCALE 3
#define ZETA_GC_EDEN_SCALE 8
#define ZETA_GC_SURVIVOR_SCALE 2
#define ZETA_GC_AGE_THRESHOLD 10
#define ZETA_GC_BIG_OBJECT 256

class GC {
public:
    GC(VM* vm);
    ~GC();

    template<typename T, typename... Args>
    T* allocate(Args&&... args) {
        static_assert(std::is_base_of_v<Object, T>);
        static_assert(!std::is_same_v<T, Block>, "Use allocateBlock() to allocate Block");
        Object* obj = allocateImpl(sizeof(T));
        obj->age = 0;
        obj->gcWord.marked = false;
        obj->gcWord.remembered = false;
        obj->gcWord.forward = 0;
        return new (obj) T(std::forward<Args>(args)...);
    }

    Block* allocateBlock(int size);
    void writeBarrier(Object* src, Object** field, Object* value); // must be called when src->field = value.
    void writeBarrier(Object* src, Value* field, Value value);

private:
    VM* vm;
    int maxHeapSize; // byte; -1 for unlimited

    int heapSize;
    void* heap;

    void* heapEnd;
    void* oldStart;
    void* oldEnd;
    void* youngStart;
    void* youngEnd;
    void* edenStart;
    void* edenEnd;
    void* fromStart;
    void* fromEnd;
    void* toStart;
    void* toEnd;

    void* curEdenPtr;
    void* curOldPtr;
    void* curFromPtr; // end of live objects in from-space (survivors after last minor GC)

    PointerHashSet rememberedSet; // the location of the field in old generation that points to young generation

    bool isYoung(void* obj);
    bool isOld(void* obj);
    bool inEden(void* obj);
    bool inFrom(void* obj);
    bool inTo(void* obj);
    bool inHeap(void* ptr);

    Object* allocateImpl(int size);
    Object* allocateInOld(int size);
    bool growHeap(int minSize);

    void minorGC(); // for young generation
    void fullGC(); // for the entire heap
};

}
