// for this hw we always have the assumption of having enough registers.

#include "register_allocation.hpp"
#include "scheduler.hpp"
#include "data_structures.hpp"
#include <stdexcept>


void alloc_b(std::vector<Bundle>& schedule, 
                   const std::vector<DependencyAnalysisTableEntry>& analysis_table,
                   const std::vector<Instruction>& instructions) {
    
    // TODO: Phase 1 - Walk instructions in scheduling order and assign fresh 
    //                 destination registers (x1, x2, x3...)

    for (Bundle& bundle : schedule) {
        // For each instruction in the bundle, we would:
        switch (bundle.ALU0) {
            case "nop": break;
            case "add":
            case "addi":
            case "sub":
            case "mov":
        }
        switch (bundle.ALU1) {
            case "nop": break;
        // 1. Identify the instruction and its destination register (using analysis_table).
        // 2. Assign a new register to the destination (e.g., x1, x2, ...).
        // 3. Update the instruction's raw_text to reflect the new destination register.
        // 4. Store the mapping from old destination register to new register for later use when updating source operands.
    }
    
    // TODO: Phase 2 - Update source operands to use the newly assigned 
    //                 registers of their producers (using analysis_table)
    
    // TODO: Phase 3 - Handle interloop dependencies by inserting 'mov' 
    //                 instructions at the end of the loop body
    
    // TODO: Phase 4 - Assign fresh registers to any source operands that 
    //                 still have no known producer
}