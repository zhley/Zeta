#pragma once

namespace Zeta {

// base AST node
class Node{
public:
    int line;
    int column;

    Node() = default;
    virtual ~Node() = default;
};

} // namespace Zeta