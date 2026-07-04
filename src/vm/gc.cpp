#include "gc.h"

#include "vm.h"

namespace Zeta {

#define ALIGN8(size) (((size) + 7) & ~7)
#define HEAP_OFFSET(ptr) ((uint64_t)((char*)(ptr) - (char*)heap))
#define HEAP_PTR(offset) ((void*)((char*)heap + (offset)))

GC::GC(VM* vm) : vm(vm) {
    maxHeapSize = vm->config.maxHeapSize * 1024;
    heapSize = vm->config.initHeapSize * 1024;
    heap = std::malloc(heapSize);
    if(!heap) {
        vm->errorHandler({VM::Error::VMError, "Failed to allocate heap memory"});
    }
    heapEnd = static_cast<char*>(heap) + heapSize;
    int youngSize = ALIGN8(heapSize * ZETA_GC_YOUNG_SCALE / (ZETA_GC_YOUNG_SCALE + ZETA_GC_OLD_SCALE));
    int halfSurvivorSize = ALIGN8(youngSize * ZETA_GC_SURVIVOR_SCALE / (ZETA_GC_EDEN_SCALE + ZETA_GC_SURVIVOR_SCALE));
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
}

GC::~GC() {
    std::free(heap);
}

Block* GC::allocateBlock(int size) {
    Object* obj = allocateImpl(sizeof(Block) + size);
    Block* block = new (obj) Block(size);
    return block;
}

void GC::writeBarrier(Object* src, Object** field, Object* value) {
    if(isOld(src) && isYoung(value)) {
        if((*field)->gcWord.remembered) {
            (*field)->gcWord.remembered = false;
            value->gcWord.remembered = true;
        } else {
            value->gcWord.remembered = true;
            rememberedSet.push_back(field);
        }
    }
    *field = value;
}

bool GC::isYoung(Object* obj) {
    return (void*)obj >= youngStart && (void*)obj < youngEnd;
}

bool GC::isOld(Object* obj) {
    return (void*)obj >= oldStart && (void*)obj < oldEnd;
}

bool GC::inEden(Object* obj) {
    return (void*)obj >= edenStart && (void*)obj < edenEnd;
}

bool GC::inFrom(Object* obj) {
    return (void*)obj >= fromStart && (void*)obj < fromEnd;
}

bool GC::inTo(Object* obj) {
    return (void*)obj >= toStart && (void*)obj < toEnd;
}

Object* GC::allocateImpl(int size) {
    // 8 bytes alignment
    size = (size + 7) & ~7;
    if(size > ZETA_GC_BIG_OBJECT) {
        // allocate in old generation
        void* ptr = curOldPtr;
        curOldPtr = (char*)(curOldPtr) + size;
        if(curOldPtr > (char*)oldEnd) {
            // GC
        }else {
            return (Object*)(ptr);
        }
    } else {
        // allocate in young generation
        void* ptr = curEdenPtr;
        curEdenPtr = (char*)(curEdenPtr) + size;
        if(curEdenPtr > (char*)edenEnd) {
            // GC
        }else {
            return (Object*)(ptr);
        }
    }

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
        for(int i = 0; i < frame.proto->localCount + frame.proto->maxStackSize; ++i) {
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
    for(auto& field: rememberedSet) {
        if(!(*field)->gcWord.marked) {
            (*field)->gcWord.marked = true;
            worklist.push_back(*field);
        }
    }
    int p = 0;
    char* toPtr = static_cast<char*>(toStart);
    while(p < worklist.size()) {
        Object* obj = worklist[p];

        // copy to to-space
        int size = obj->getSize();
        std::memcpy(toPtr, obj, size);
        toPtr += size;
        Object* newObj = (Object*)toPtr;
        // TODO: 处理to区溢出的情况
        // TODO: 晋升
        obj->gcWord.forward = HEAP_OFFSET(newObj);
        newObj->trace([&worklist, this](Object** child){
            if(isYoung(*child) && !(*child)->gcWord.marked) {
                (*child)->gcWord.marked = true;
                worklist.push_back(*child);
            }
        });

        p++;
    }

    // update references
    for(auto& root: roots) {
        *root = (Object*)HEAP_PTR((*root)->gcWord.forward);
    }
    for(auto& field: rememberedSet) {
        *field = (Object*)HEAP_PTR((*field)->gcWord.forward);
    }
    char* toEndPtr = toPtr;
    char* ptr = static_cast<char*>(toStart);
    while(ptr < toEndPtr) {
        Object* obj = (Object*)ptr;
        obj->trace([this](Object** child){
            if(inEden(*child) || inFrom(*child)) {
                *child = (Object*)HEAP_PTR((*child)->gcWord.forward);
            }
        });
        obj->gcWord.marked = false;
        obj->gcWord.forward = 0;
        ptr += obj->getSize();
    }

    // swap from and to
    std::swap(fromStart, toStart);
    std::swap(fromEnd, toEnd);
    curEdenPtr = edenStart;
}

}
