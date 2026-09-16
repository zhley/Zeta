#pragma once

#include "value.h"

#include <vector>

namespace Zeta {

class VM;

//=== Hash set for rememberedSet ===
class PointerHashSet {

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

    inline static Object** const EMPTY = nullptr;
    inline static Object** const DELETED = reinterpret_cast<Object**>(0x1);

    static size_t hash(void* ptr) {
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

备注:
 - C++ 侧缓存的 Object* / T*:
   任何可能触发 GC 的操作 (分配、Array::add、Map::rehash 等) 都可能使缓存指针失效.
   跨越这类操作时, 必须 lock()/unlock() 包住, 或不缓存裸指针、改为缓存根上的 Value* 并在 GC 后重读.
   例如:
       Array* arr = POP();
       ......
       arr->add(POP());   // 内部可能 GC, arr 可能已移动
       PUSH(arr->size);   // 未定义行为
 - Object 子类的成员函数(包括构造函数): 凡是体内可能触发 GC 且之后仍会使用 this 的,
   必须在第一次可能 GC 之前加锁, 并持有到对 this 的最后一次使用.
 - offset 为 0 的地方是 eden 区, 所以 forward 字段可以用 0 来表示无效.
 - 针对 64 位架构设计, 32 位能否正常运行存疑.
*/

// fixed configuration
#define ZETA_GC_YOUNG_SCALE 1
#define ZETA_GC_OLD_SCALE 3
#define ZETA_GC_EDEN_SCALE 8
#define ZETA_GC_SURVIVOR_SCALE 2
#define ZETA_GC_AGE_THRESHOLD 10
#define ZETA_GC_BIG_OBJECT 512

class GC {
public:
    GC(VM* vm);
    ~GC();

    template<typename T, typename... Args>
    requires (std::is_base_of_v<Object, T> && !std::is_same_v<T, Block>)
    T* allocate(Args&&... args) {
        Object* obj = allocateImpl(sizeof(T));
        obj->age = 0;
        obj->gcWord.marked = false;
        obj->gcWord.forward = 0;
        if (!locked) {
            Value v(obj);
            assert(newborn == nullptr);
            newborn = &v;
            new (obj) T(std::forward<Args>(args)...); // GC may be triggered when returning here
            newborn = nullptr;
            return static_cast<T*>(v.ptrValue);
        } else {
            return new (obj) T(std::forward<Args>(args)...); 
        }
    }

    Block* allocateBlock(int size, Block::ElemType elemType);
    void writeBarrier(Object* src, Object** field, Object* value); // must be called when src->field = value.
    void writeBarrier(Object* src, Value* field, Value value);

    void lock(){
        ++locked;
    }
    void unlock(){
        --locked;
        if(locked == 0) {
            if(waitingMinorGC) {
                minorGC();
                waitingMinorGC = false;
            }
            if(waitingFullGC) {
                assert(!temp.empty());
                fullGC();
                for(auto ptr : temp) {
                    std::free(ptr);
                }
                temp.clear();
                waitingFullGC = false;
            }
        }
    }

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

    std::vector<void*> temp; 
    int locked = false;
    bool waitingMinorGC = false;
    bool waitingFullGC = false;

    Value* newborn = nullptr; // point to the object that has just been allocated, but not yet returned to the caller

    bool isYoung(void* obj);
    bool isOld(void* obj);
    bool inEden(void* obj);
    bool inFrom(void* obj);
    bool inTo(void* obj);
    bool inHeap(void* ptr);
    bool inTemp(void* ptr);

    Object* allocateImpl(int size);
    Object* allocateInOld(int size);
    bool growHeap(int minSize);

    void minorGC(); // for young generation
    void fullGC(); // for the entire heap
    
    template<typename F>
    requires std::is_invocable_v<F, Value&>
    void forEachRoot(F&& f);
};

class GCLockGuard {
public:
    explicit GCLockGuard(GC* gc) : gc(gc) {
        gc->lock();
    }
    ~GCLockGuard() noexcept(false) {
        // must ensure that there is no GCLockGuard object on any stack unwinding path of any exception, otherwise it may crash with double exceptions
        gc->unlock();
    }

private:
    GC* gc;
};

}
