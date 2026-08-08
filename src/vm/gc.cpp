#include "gc.h"

#include "vm.h"
#include "vm/value.h"

#include <cassert>
#include <iostream>

#pragma GCC optimize("no-strict-aliasing")

namespace Zeta {

#define ALIGN8(size) (((size) + 7) & ~7)
#define HEAP_OFFSET(ptr) ((uint64_t)((char*)(ptr) - (char*)heap))
#define HEAP_PTR(offset) ((void*)((char*)heap + (offset)))

GC::GC(VM* vm) : vm(vm), rememberedSet(512) {
    maxHeapSize = vm->config.maxHeapSize == -1 ? -1 : vm->config.maxHeapSize * 1024;
    heapSize = vm->config.initHeapSize * 1024;
    heap = std::malloc(heapSize);
    if (!heap) {
        vm->errorHandler({VM::Error::VMError, 0, "Failed to allocate heap memory"});
        throw VMException(VMException::Type::OutOfMemory);
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
    if(!temp.empty()) {
        for(auto ptr : temp) {
            std::free(ptr);
        }
        temp.clear();
    }
}

Block* GC::allocateBlock(int size, Block::ElemType elemType) {
    size = ALIGN8(size);
    Object* obj = allocateImpl(sizeof(Block) + size);
    obj->age = 0;
    obj->gcWord.marked = false;
    obj->gcWord.forward = 0;
    Block* block = new (obj) Block(size, elemType);
    return block;
}

void GC::writeBarrier(Object* src, Object** field, Object* value) {
    // if src or value is in temp, rememberedSet is not needed, because fullGC will be triggered soon.
    if (isOld(src)) {
        if (isYoung(value)) {
            rememberedSet.insert(field);
        } else if (isYoung(*field)) {
            rememberedSet.erase(field);
        }
        // if (isYoung(value)) {
        //     rememberedSet.insert(field);
        // } else {
        //     rememberedSet.erase(field);
        // }
    }
    *field = value;
}

void GC::writeBarrier(Object* src, Value* field, Value value) {
    if (isOld(src)) {
        if(value.type == Value::Type::Object && isYoung(value.ptrValue)) {
            rememberedSet.insert(&field->ptrValue);
        } else if(field->type == Value::Type::Object && isYoung(field->ptrValue)) {
            rememberedSet.erase(&field->ptrValue);
        }
        // if(value.type == Value::Type::Object && isYoung(value.ptrValue)) {
        //     rememberedSet.insert(&field->ptrValue);
        // } else {
        //     rememberedSet.erase(&field->ptrValue);
        // }
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
        if(locked) {
            waitingMinorGC = true;
            return allocateInOld(size);
        } else{
            minorGC();
            ptr = curEdenPtr;
            curEdenPtr = (char*)(curEdenPtr) + size;
            if(curEdenPtr > (char*)edenEnd) {
                curEdenPtr = (char*)(curEdenPtr) - size; // undo the bump
                return allocateInOld(size);
            }
        }
    }
    return (Object*)ptr;
}

Object* GC::allocateInOld(int size) {
    void* ptr = curOldPtr;
    curOldPtr = (char*)curOldPtr + size;
    if(curOldPtr > (char*)oldEnd) {
        if(locked){
            waitingFullGC = true;
            curOldPtr = ptr;
            ptr = std::malloc(size);
            temp.push_back(ptr);
        } else {
            fullGC();
            ptr = curOldPtr;
            curOldPtr = (char*)curOldPtr + size;
            if(curOldPtr > (char*)oldEnd) {
                curOldPtr = (char*)curOldPtr - size;
                if(!growHeap(size * (ZETA_GC_YOUNG_SCALE + ZETA_GC_OLD_SCALE) / ZETA_GC_OLD_SCALE)) {
                    vm->errorHandler({VM::Error::VMError, 0, std::format("Heap limit exceeded [current heap size: {} bytes, max heap size: {} bytes]", heapSize, maxHeapSize)});
                    throw VMException(VMException::Type::HeapLimitExceeded);
                    return nullptr;
                }
                ptr = curOldPtr;
                curOldPtr = (char*)curOldPtr + size;
                assert(curOldPtr <= (char*)oldEnd);
            }
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
    if (!newHeap) throw VMException(VMException::Type::OutOfMemory);
    int newYoungSize = ALIGN8(newSize * ZETA_GC_YOUNG_SCALE / (ZETA_GC_YOUNG_SCALE + ZETA_GC_OLD_SCALE));
    char* newOldStart = (char*)newHeap + newYoungSize;
    int offset = (char*)newOldStart - (char*)oldStart;
    char* ptr = (char*)oldStart;
    while(ptr < (char*)curOldPtr) {
        Object* obj = (Object*)ptr;
        obj->trace([&offset](Object** child){
            assert(*child != nullptr);
            *child = (Object*)((char*)(*child) + offset);
        });
        ptr += obj->getSize();
    }
    forEachRoot([this, &offset](Value& v){
        if(v.type == Value::Type::Object) {
            assert(isOld(v.ptrValue));
            v.ptrValue = (Object*)((char*)v.ptrValue + offset);
        }
    });
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
    // std::cerr << "Minor GC" << std::endl; // TODO: 调试用, 正式输出时注释掉
    assert(!locked);
    std::vector<Object*> worklist;
    // get root
    std::vector<Object**> roots;
    forEachRoot([this, &roots, &worklist](Value& v){
        if(v.type == Value::Type::Object && isYoung(v.ptrValue)) {
            roots.push_back(&v.ptrValue);
        }
    });

    // mark and copy
    for(auto& root: roots) {
        if(!(*root)->gcWord.marked) {
            (*root)->gcWord.marked = true;
            worklist.push_back(*root);
        }
    }
    rememberedSet.forEach([this, &worklist](Object** field){
        if(!(*field)->gcWord.marked) {
            assert(isYoung(*field));
            (*field)->gcWord.marked = true;
            worklist.push_back(*field);
        }
    });
    
    int p = 0;
    char* toPtr = (char*)toStart;
    char* prevOldPtr = (char*)curOldPtr;
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
        // copy to to-space / old generation
        Object* newObj = (Object*)targetPtr;
        std::memcpy(newObj, obj, size);
        obj->gcWord.forward = HEAP_OFFSET(newObj);
        obj->trace([&worklist, this, &newObj](Object** child){
            if(isYoung(*child) && !(*child)->gcWord.marked) {
                (*child)->gcWord.marked = true;
                worklist.push_back(*child);
            }
        });

        p++;
    }
    if(overflow) {
        // clear marked and forward for worklist
        curOldPtr = prevOldPtr;
        for(int i = 0; i < worklist.size(); ++i) {
            worklist[i]->gcWord.marked = false;
            worklist[i]->gcWord.forward = 0;
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
    scan = prevOldPtr;
    while(scan < (char*)curOldPtr) {
        Object* obj = (Object*)scan;
        obj->trace([this](Object** child){
            if(isYoung(*child)) {
                assert((*child)->gcWord.forward);
                *child = (Object*)HEAP_PTR((*child)->gcWord.forward);
                if (isYoung(*child)) {
                    rememberedSet.insert(child);
                }
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

// recycle the entire heap.
void GC::fullGC(){
    // std::cerr << "Full GC" << std::endl; // TODO: 调试用, 正式输出时注释掉
    assert(!locked);
    std::vector<Object*> worklist;
    std::vector<Object**> roots;

    // collect and mark
    forEachRoot([&roots, &worklist](Value& v){
        if(v.type == Value::Type::Object) {
            roots.push_back(&v.ptrValue);
            if(!v.ptrValue->gcWord.marked) {
                v.ptrValue->gcWord.marked = true;
                worklist.push_back(v.ptrValue);
            }
        }
    });
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
    // sort: old - young - temp
    std::sort(worklist.begin(), worklist.end(), [this](Object* a, Object* b){
        if(inHeap(a) && !inHeap(b)) return true;
        if(!inHeap(a) && inHeap(b)) return false;
        if(isOld(a) && isYoung(b)) return true;
        if(isYoung(a) && isOld(b)) return false;
        return a < b;
    });
    int totalSize = 0;
    for(auto& obj : worklist){
        totalSize += obj->getSize();
    }
    void* prevHeap = heap;
    if((char*)oldStart + totalSize > (char*)oldEnd){
        // grow heap
        int minSize = ALIGN8(totalSize * (ZETA_GC_YOUNG_SCALE + ZETA_GC_OLD_SCALE) / ZETA_GC_OLD_SCALE);
        bool unlimited = (maxHeapSize == -1);
        if(!unlimited && heapSize >= maxHeapSize) {
            vm->errorHandler({VM::Error::VMError, 0, std::format("Heap limit exceeded [current heap size: {} bytes, max heap size: {} bytes]", heapSize, maxHeapSize)});
            throw VMException(VMException::Type::HeapLimitExceeded);
            return;
        }
        int newSize = heapSize * 2;
        if(newSize < heapSize + minSize) {
            newSize = heapSize + minSize + 1024 * 1024; // at least 1 MiB more
        }
        if(!unlimited && newSize > maxHeapSize) {
            newSize = maxHeapSize;
        }
        if(newSize < heapSize + minSize) {
            vm->errorHandler({VM::Error::VMError, 0, std::format("Heap limit exceeded [current heap size: {} bytes, max heap size: {} bytes]", heapSize, maxHeapSize)});
            throw VMException(VMException::Type::HeapLimitExceeded);
            return;
        }
        void* newHeap = std::malloc(newSize);
        if (!newHeap) {
            vm->errorHandler({VM::Error::VMError, 0, "Out of memory"});
            throw VMException(VMException::Type::OutOfMemory);
        }
        int newYoungSize = ALIGN8(newSize * ZETA_GC_YOUNG_SCALE / (ZETA_GC_YOUNG_SCALE + ZETA_GC_OLD_SCALE));
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
    }
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
        Object* newObj = (Object*)HEAP_PTR(obj->gcWord.forward);
        std::memmove(newObj, obj, obj->getSize());
        newObj->gcWord.marked = false;
        newObj->gcWord.forward = 0;
    }

    curOldPtr = compactPtr;
    curEdenPtr = edenStart;
    curFromPtr = fromStart;
    rememberedSet.clear();

    if(prevHeap != heap) {
        std::free(prevHeap);
    }
}

template<typename F>
requires std::is_invocable_v<F, Value&>
void GC::forEachRoot(F&& f) {
    for(auto& routine : vm->routines) {
        for(auto& constVal : routine->constants) {
            f(constVal);
        }
    }
    for(auto& gv : vm->global) {
        f(gv);
    }
    for(auto& frame : vm->stackFrames) {
        for(Value* ptr = frame.base; ptr < frame.top; ++ptr) {
            f(*ptr);
        }
    }
    for(auto& tmpRoot : vm->tempRoots) {
        f(*tmpRoot);
    }
}

}
