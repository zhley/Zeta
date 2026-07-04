#pragma once

#include "value.h"

namespace Zeta {

// fixed configuration
#define ZETA_GC_YOUNG_SCALE 1
#define ZETA_GC_OLD_SCALE 3
#define ZETA_GC_EDEN_SCALE 8
#define ZETA_GC_SURVIVOR_SCALE 2
#define ZETA_GC_AGE_THRESHOLD 10
#define ZETA_GC_BIG_OBJECT 256

class VM;

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
        return new (obj) T(std::forward<Args>(args)...);
    }

    Block* allocateBlock(int size);
    void writeBarrier(Object* src, Object** field, Object* value); // must be called when src->field = value.

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

    std::vector<Object**> rememberedSet; // the location of the field in old generation that points to young generation

    bool isYoung(Object* obj);
    bool isOld(Object* obj);
    bool inEden(Object* obj);
    bool inFrom(Object* obj);
    bool inTo(Object* obj);

    Object* allocateImpl(int size);
    void minorGC();
    void majorGC();
    
};

}
