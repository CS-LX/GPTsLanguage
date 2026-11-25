#include "../src/lua_prose.hpp"
#include "lopcodes.h"
#include <iostream>
int main() {
    using namespace gpt::lua54;
    for (unsigned op = 0; op < NUM_OPCODES; ++op) {
        for (unsigned bits : {0u, 0xffffff80u, 0x12345680u, 0x80ff7f00u, 0x7fff8000u}) {
            auto word = bits | op;
            if (assemble(narrate(word)) != word)
                return 1;
        }
    }
    std::cout << NUM_OPCODES << " instruction forms round-tripped at operand boundaries\n";
}
