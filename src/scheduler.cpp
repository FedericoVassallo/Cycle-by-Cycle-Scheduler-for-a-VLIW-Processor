#include "data_structures.hpp"
#include <algorithm>

// version using loop (without loop.pip)
void schedule_ASAP_basic()  {

    // TODO

}

// version using loop.pip
void schedule_ASAP_advanced()  {

    // TODO

}

int calculate_II_res(const std::vector<Instruction>& instructions) {

    int alu_count = 0;
    int mul_count = 0;
    int mem_count = 0;

    // we count the number of ALU, MUL, and MEM instructions in BB1 to determine the II_res
    // they will be useful for the formula calculattion for II_res 
    for (const auto& instr : instructions) {
        
        if (instr.basic_block != BasicBlock::BB1) {
            continue; // Only consider instructions in BB1
        }

        switch (instr.kind) {
            case InstructionKind::Add:
            case InstructionKind::Addi:
            case InstructionKind::Sub:
            case InstructionKind::Mov:
                alu_count++;
                break;
            case InstructionKind::Mulu:
                mul_count++;
                break;
            case InstructionKind::Ld:
            case InstructionKind::St:
                mem_count++;
                break;
            default:
                break; // Ignore other instruction types
        }
    }

    // we use the alu_count + 1 for doing the ceiling of the division by 2
    int ii_alu = (alu_count + 1) / 2; // Each bundle can have up to 2 ALU instructions
    int ii_mul = mul_count; // Each bundle can have 1 MUL instruction
    int ii_mem = mem_count; // Each bundle can have 1 MEM instruction

    // The II_res is the maximum of the three calculated values
    int ii_res = std::max({ii_alu, ii_mul, ii_mem, 1}); // (the 1 is the ensure is at least 1)

    return ii_res;
}