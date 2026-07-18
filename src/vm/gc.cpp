#include "gc.h"

#include "vm.h"
#include "vm/value.h"

#include <cassert>

#pragma GCC optimize("no-strict-aliasing")

namespace Zeta {

#define ALIGN8(size) (((size) + 7) & ~7)
#define HEAP_OFFSET(ptr) ((uint64_t)((char*)(ptr) - (char*)heap))
#define HEAP_PTR(offset) ((void*)((char*)heap + (offset)))

GC::GC(VM* vm) : vm(vm), rememberedSet(512) {
    maxHeapSize = vm->config.maxHeapSize * 1024;
    heapSize = vm->config.initHeapSize * 1024;
    heap = std::malloc(heapSize);
    if (!heap) {
        vm->errorHandler({VM::Error::VMError, "Failed to allocate heap memory"});
        return;
    }
    heapEnd = static_cast<char*>(heap) + heapSize;
    int youngSize = ALIGN8(heapSize * ZETA_GC_YOUNG_SCALE / (ZETA_GC_YOUNG_SCALE + ZETA_GC_OLD_SCALE));
    int halfSurvivorSize = ALIGN8(youngSize * ZETA_GC_SURVIVOR_SCALE / (ZETA_GC_EDEN_SCALE + ZETA_GC_SURVIVOR_SCALE) / 2);
    int edenSize = youngSize - halfSurvivorSize * 2;
    youngStart = heap;
    youngEnd = static_cast<char*>(youngStart) + youngSize;
    edenStart = youngStart;
    edenEnd = static_cast<char*>(edenStart) + edenSize;
    fromStart = edenEnd;
    fromEnd = static_cast<char*>(fromStart) + halfSurvivorSize;
    toStart = fromEnd;
    toEnd = youngEnd;
    oldStart = youngEnd;
    oldEnd = heapEnd;

    curEdenPtr = edenStart;
    curOldPtr = oldStart;
    curFromPtr = fromStart;
}

GC::~GC() {
    std::free(heap);
}

Block* GC::allocateBlock(int size) {
    size = ALIGN8(size);
    Object* obj = allocateImpl(sizeof(Block) + size);
    Block* block = new (obj) Block(size);
    return block;
}

void GC::writeBarrier(Object* src, Object** field, Object* value) {
    if (isOld(src) && isYoung(value)) {
        rememberedSet.insert(field);
    }
    *field = value;
}

void GC::writeBarrier(Object* src, Value* field, Value value) {
    if (isOld(src)) {
        if(value.type == Value::Type::Object && isYoung(value.ptrValue)) {
            rememberedSet.insert(&field->ptrValue);
        } else if(value.type != Value::Type::Object && field->type == Value::Type::Object) {
            rememberedSet.erase(&field->ptrValue);
        }
    }
    *field = value;
}

bool GC::isYoung(void* obj) {
    return obj >= youngStart && obj < youngEnd;
}

bool GC::isOld(void* obj) {
    return obj >= oldStart && obj < oldEnd;
}

bool GC::inEden(void* obj) {
    return obj >= edenStart && obj < edenEnd;
}

bool GC::inFrom(void* obj) {
    return obj >= fromStart && obj < fromEnd;
}

bool GC::inTo(void* obj) {
    return obj >= toStart && obj < toEnd;
}

bool GC::inHeap(void* ptr) {
    return ptr >= heap && ptr < heapEnd;
}

Object* GC::allocateImpl(int size) {
    // 8 bytes alignment
    size = ALIGN8(size);
    if(size > ZETA_GC_BIG_OBJECT) {
        // allocate in old generation
        return allocateInOld(size);
    }
    // allocate in young generation
    void* ptr = curEdenPtr;
    curEdenPtr = (char*)curEdenPtr + size;
    if(curEdenPtr > (char*)edenEnd) {
        minorGC();
        ptr = curEdenPtr;
        curEdenPtr = (char*)(curEdenPtr) + size;
        if(curEdenPtr > (char*)edenEnd) {
            curEdenPtr = (char*)(curEdenPtr) - size; // undo the bump
            return allocateInOld(size);
        }
    }
    return (Object*)ptr;
}

Object* GC::allocateInOld(int size) {
    void* ptr = curOldPtr;
    curOldPtr = (char*)curOldPtr + size;
    if(curOldPtr > (char*)oldEnd) {
        fullGC();
        ptr = curOldPtr;
        curOldPtr = (char*)curOldPtr + size;
        if(curOldPtr > (char*)oldEnd) {
            curOldPtr = (char*)curOldPtr - size;
            if(!growHeap(size * (ZETA_GC_YOUNG_SCALE + ZETA_GC_OLD_SCALE) / ZETA_GC_OLD_SCALE)) {
                vm->errorHandler({VM::Error::VMError, "Out of memory"}); // TODO: 详细一点
                return nullptr;
            }
            ptr = curOldPtr;
            curOldPtr = (char*)curOldPtr + size;
            assert(curOldPtr <= (char*)oldEnd);
        }
    }
    return (Object*)ptr;
}

// minSize: the minimum size to grow.
// only called after fullGC().
bool GC::growHeap(int minSize) {
    minSize = ALIGN8(minSize);
    bool unlimited = (maxHeapSize == -1);
    if(!unlimited && heapSize >= maxHeapSize) {
        return false;
    }
    int newSize = heapSize * 2;
    if(newSize < heapSize + minSize) {
        newSize = heapSize + minSize + 1024 * 1024; // at least 1 MiB more
    }
    if(!unlimited && newSize > maxHeapSize) {
        newSize = maxHeapSize;
    }
    if(newSize < heapSize + minSize) {
        return false;
    }

    newSize = ALIGN8(newSize);
    // all survivors are in old generation.
    void* newHeap = std::malloc(newSize);
    int newYoungSize = ALIGN8(newSize * ZETA_GC_YOUNG_SCALE / (ZETA_GC_YOUNG_SCALE + ZETA_GC_OLD_SCALE));
    char* newOldStart = (char*)newHeap + newYoungSize;
    int offset = (char*)newOldStart - (char*)oldStart;
    char* ptr = (char*)oldStart;
    while(ptr < (char*)curOldPtr) {
        Object* obj = (Object*)ptr;
        obj->trace([&offset](Object** child){
            *child = (Object*)((char*)(*child) + offset);
        });
        ptr += obj->getSize();
    }
    for(auto& gv: vm->global) {
        if(gv.type == Value::Type::Object && inHeap(gv.ptrValue)) {
            assert(isOld(gv.ptrValue));
            gv.ptrValue = (Object*)((char*)gv.ptrValue + offset);
        }
    }
    for(auto& frame: vm->stackFrames) {
        for(int i = 0; i < frame.routine->localCount + frame.routine->maxStackSize; ++i) {
            Value& v = frame.base[i];
            if(v.type == Value::Type::Object && inHeap(v.ptrValue)) {
                assert(isOld(v.ptrValue));
                v.ptrValue = (Object*)((char*)v.ptrValue + offset);
            }
        }
    }
    assert(rememberedSet.empty());
    int totalOldSize = (char*)curOldPtr - (char*)oldStart;
    std::memcpy(newOldStart, oldStart, totalOldSize);
    std::free(heap);
    int newHalfSurvivorSize = ALIGN8(newYoungSize * ZETA_GC_SURVIVOR_SCALE / (ZETA_GC_EDEN_SCALE + ZETA_GC_SURVIVOR_SCALE) / 2);
    int newEdenSize = newYoungSize - newHalfSurvivorSize * 2;
    heapSize = newSize;
    heap = newHeap;
    heapEnd = (char*)heap + heapSize;
    youngStart = heap;
    youngEnd = (char*)youngStart + newYoungSize;
    edenStart = youngStart;
    edenEnd = (char*)edenStart + newEdenSize;
    fromStart = edenEnd;
    fromEnd = (char*)fromStart + newHalfSurvivorSize;
    toStart = fromEnd;
    toEnd = youngEnd;
    oldStart = youngEnd;
    oldEnd = heapEnd;

    curEdenPtr = edenStart;
    curOldPtr = (char*)oldStart + totalOldSize;
    curFromPtr = fromStart;
    return true;
}

void GC::minorGC() {
    std::vector<Object*> worklist;
    // get root
    std::vector<Object**> roots;
    for(auto& gv: vm->global) {
        if(gv.type == Value::Type::Object && isYoung(gv.ptrValue)) {
            roots.push_back(&gv.ptrValue);
        }
    }
    for(auto& frame: vm->stackFrames) {
        for(int i = 0; i < frame.routine->localCount + frame.routine->maxStackSize; ++i) {
            Value& v = frame.base[i];
            if(v.type == Value::Type::Object && isYoung(v.ptrValue)) {
                roots.push_back(&v.ptrValue);
            }
        }
    }

    // mark and copy
    for(auto& root: roots) {
        if(!(*root)->gcWord.marked) {
            (*root)->gcWord.marked = true;
            worklist.push_back(*root);
        }
    }
    rememberedSet.forEach([this](Object** field){
        if(!(*field)->gcWord.marked) {
            assert(isYoung(*field));
            (*field)->gcWord.marked = true;
        }
    });
    
    int p = 0;
    char* toPtr = (char*)toStart;
    bool overflow = false;
    while(p < worklist.size()) {
        Object* obj = worklist[p];
        int size = obj->getSize();
        obj->age++;

        char* targetPtr;
        if(obj->age < ZETA_GC_AGE_THRESHOLD && toPtr + size <= (char*)toEnd) {
            targetPtr = toPtr;
            toPtr += size;
        } else {
            targetPtr = (char*)curOldPtr;
            curOldPtr = (char*)curOldPtr + size;
            if(curOldPtr > (char*)oldEnd) {
                curOldPtr = targetPtr; // undo the bump
                overflow = true;
                break;
            }
        }
        // copy to to-space
        Object* newObj = (Object*)targetPtr;
        std::memcpy(newObj, obj, size);
        obj->gcWord.forward = HEAP_OFFSET(newObj);
        newObj->trace([&worklist, this](Object** child){
            if(isYoung(*child) && !(*child)->gcWord.marked) {
                (*child)->gcWord.marked = true;
                worklist.push_back(*child);
            }
        });

        p++;
    }
    if(overflow) {
        // clear marked and forward for worklist
        for(int i = 0; i < p; ++i) {
            worklist[i]->gcWord.marked = false;
            worklist[i]->gcWord.forward = 0;
        }
        for(int i = p; i < worklist.size(); ++i) {
            worklist[i]->gcWord.marked = false;
        }
        fullGC();
        return;
    }

    // update references
    for(auto& root: roots) {
        assert((*root)->gcWord.forward);
        *root = (Object*)HEAP_PTR((*root)->gcWord.forward);
    }
    rememberedSet.forEach([this](Object** field){
        assert((*field)->gcWord.forward);
        *field = (Object*)HEAP_PTR((*field)->gcWord.forward);
    });
    rememberedSet.eraseIf([this](Object** field){
        return !isYoung(*field);
    });
    char* toEndPtr = toPtr;
    char* scan = static_cast<char*>(toStart);
    while(scan < toEndPtr) {
        Object* obj = (Object*)scan;
        obj->trace([this](Object** child){
            if(isYoung(*child)) {
                assert((*child)->gcWord.forward);
                *child = (Object*)HEAP_PTR((*child)->gcWord.forward);
            }
        });
        obj->gcWord.marked = false;
        obj->gcWord.forward = 0;
        scan += obj->getSize();
    }

    // swap from and to
    std::swap(fromStart, toStart);
    std::swap(fromEnd, toEnd);
    curEdenPtr = edenStart;
    curFromPtr = toEndPtr;
}

// TODO: 需要考虑一下对驻留字符串的引用是否需要特殊处理
// recycle the entire heap.
void GC::fullGC(){
    std::vector<Object*> worklist;
    std::vector<Object**> roots;

    // collect and mark
    for(auto& gv : vm->global){
        if(gv.type == Value::Type::Object){
            roots.push_back(&gv.ptrValue);
            if(!gv.ptrValue->gcWord.marked){
                gv.ptrValue->gcWord.marked = true;
                worklist.push_back(gv.ptrValue);
            }
        }
    }
    for(auto& frame : vm->stackFrames){
        for(int i = 0; i < frame.routine->localCount + frame.routine->maxStackSize; ++i){
            Value& v = frame.base[i];
            if (v.type == Value::Type::Object) {
                roots.push_back(&v.ptrValue);
                if(!v.ptrValue->gcWord.marked){
                    v.ptrValue->gcWord.marked = true;
                    worklist.push_back(v.ptrValue);
                }
            }
        }
    }
    int p = 0;
    while(p < worklist.size()){
        Object* obj = worklist[p];
        obj->trace([&worklist](Object** child){
            assert(*child);
            if(!(*child)->gcWord.marked){
                (*child)->gcWord.marked = true;
                worklist.push_back(*child);
            }
        });
        p++;
    }

    // set forwarding
    std::sort(worklist.begin(), worklist.end(), [this](Object* a, Object* b){
        if(isOld(a) && isYoung(b)) return true;
        return a < b;
    });
    char* compactPtr = (char*)oldStart;
    for(auto& obj : worklist){
        int size = obj->getSize();
        obj->gcWord.forward = HEAP_OFFSET(compactPtr);
        compactPtr += size;
    }

    // update references
    for(auto& root : roots){
        *root = (Object*)HEAP_PTR((*root)->gcWord.forward);
    }
    for(auto& obj : worklist){
        obj->trace([this](Object** child){
            *child = (Object*)HEAP_PTR((*child)->gcWord.forward);
        });
    }

    // compact
    for(auto& obj : worklist){
        std::memmove(HEAP_PTR(obj->gcWord.forward), obj, obj->getSize());
        obj->gcWord.marked = false;
        obj->gcWord.forward = 0;
    }

    curOldPtr = compactPtr;
    curEdenPtr = edenStart;
    curFromPtr = fromStart;
    rememberedSet.clear();
}

}
