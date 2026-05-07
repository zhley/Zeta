#pragma once

#include <cstdint>
namespace Zeta {

// base AST node
class Node{
public:
    int line;
    int column;
    union {
        int64_t int_;
        double double_;
        const char* str;
    };

    Node() = default;
    virtual ~Node();
};

}
