#include "data_structures.hpp"

// since the scheduling might fail for a given II, we need to be able to set a new II and reset the table for the new scheduling attempt
void SlotTable::init_reset(int ii) {
    currentII = ii; // The current Initiation Interval (II) being evaluated
    table.clear();
    table.resize(ii); // Initialize the table with 'ii' entries, each representing a slot for one iteration
}

// we check if we can schedule an instruction in the current slot (which is determined by the current iteration and the current II) by checking the hardware resources used in that slot
// we get as input the kind of the instruction we want to schedule and we check if the corresponding hardware resource is available in the current slot
// to get the current slot we get it from the actual cycle we are trying to schedule the instruction by doing the modulo of the current cycle with the current II (currentII)
// the modulo we do it using the remainder op %
bool SlotTable::can_schedule(int actual_cycle, InstructionKind instr_kind) const {
    int target_row = actual_cycle % currentII; // Get the target row based on the actual cycle and the current II
    const HardwareResources& resources = table[target_row]; // Get the hardware resources for the target row

    switch (instr_kind) {
        case InstructionKind::Add:
        case InstructionKind::Addi:
        case InstructionKind::Sub:
        case InstructionKind::Mov:
            return !resources.alu0_used || !resources.alu1_used; // Can schedule if either ALU0 or ALU1 is available
        case InstructionKind::Mulu:
            return !resources.mul_used; // Can schedule if the MUL unit is available
        case InstructionKind::Ld:
        case InstructionKind::St:
            return !resources.mem_used; // Can schedule if the MEM unit is available
        case InstructionKind::Loop:
        case InstructionKind::LoopPip:
            return !resources.branch_used; // Can schedule if the BRANCH unit is available
        case InstructionKind::Nop:
            return true; // NOP can always be scheduled
        default:
            return false; // For unknown instruction types, we cannot schedule
    }
}

void SlotTable::reserve_resources(int actual_cycle, InstructionKind instr_kind) {
    int target_row = actual_cycle % currentII; // Get the target row based on the actual cycle and the current II
    HardwareResources& resources = table[target_row]; // Get the hardware resources for the target row

    switch (instr_kind) {
        case InstructionKind::Add:
        case InstructionKind::Addi:
        case InstructionKind::Sub:
        case InstructionKind::Mov:
            if (!resources.alu0_used) {
                resources.alu0_used = true; // Reserve ALU0 if it's available
            } else {
                resources.alu1_used = true; // Otherwise, reserve ALU1
            }
            break;
        case InstructionKind::Mulu:
            resources.mul_used = true; // Reserve the MUL unit
            break;
        case InstructionKind::Ld:
        case InstructionKind::St:
            resources.mem_used = true; // Reserve the MEM unit
            break;
        case InstructionKind::Loop:
        case InstructionKind::LoopPip:
            resources.branch_used = true; // Reserve the BRANCH unit
            break;
        case InstructionKind::Nop:
            // NOP doesn't use any resources, so we don't need to do anything
            break;
        default:
            break; // For unknown instruction types, do nothing
    }
}