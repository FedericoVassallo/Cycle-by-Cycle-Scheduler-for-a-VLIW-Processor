#include "include/dependency.hpp"
#include "include/data_structures.hpp"
#include "include/io_handler.hpp"

void update_instruction_bb(std::vector<Instruction>& instructions) {
    BasicBlock current_bb = BasicBlock::BB0;
    for (int i = 0; i < static_cast<int>(instructions.size()); ++i) {
        Instruction& instr = instructions[i];
        instr.basic_block = current_bb;

        if (instr.kind == InstructionKind::Loop || instr.kind == InstructionKind::LoopPip) {
            current_bb = BasicBlock::BB1; 
            // When we find the loop instruction, we can start assigning instructions to BB1
            instr.basic_block = current_bb; // We set the BB of the loop instruction itself 
            int loop_destination_id = instr.loop_target; // We identify the target of the loop 
            for (int j = loop_destination_id; j < i; ++j) { 
                // We set the BB of all the instructions in beetween to BB1
                instructions[j].basic_block = current_bb;
            }

            current_bb = BasicBlock::BB2; // From now on, instrutions will belong to BB2
        }
    }
}

void dependency_analysis(const std::vector<Instruction>& instructions, std::vector<DependencyAnalysisTableEntry>& analysis_table) {

   
    // This function should analyze the instructions and fill the analysis_table with dependency information.
    for (int i = 0; i < static_cast<int>(instructions.size()); ++i) {
        const Instruction& instr = instructions[i];
        DependencyAnalysisTableEntry entry;
        entry.address = i;
        entry.id = i; // For simplicity, we can use the instruction index as its ID.
        entry.instruction_type = instr.kind;

        if (entry.instruction_type == InstructionKind::Loop || entry.instruction_type == InstructionKind::LoopPip) {
            // Loop instructions do not have a destination register, so we can skip them.
            analysis_table.push_back(entry);
            continue;
        }
        else {
            // For other instructions, we can set the destination register if it exists.
            entry.destination_register = instr.destination_register;
        }

        /*
        // Local dependencies: Check previous instructions for register usage.
        for (int j = i - 1; j >= 0; --j) {
            const Instruction& prev_instr = instructions[j];
            if (prev_instr.destination_register != -1) {
                // Check if current instruction reads from a register that was written by a previous instruction.
                if (std::find(instr.source_registers.begin(), instr.source_registers.end(), prev_instr.destination_register) != instr.source_registers.end()) {
                    entry.local_dependencies.push_back(j);
                }
            }
        }
        */

        // Interloop, loop invariant, and post loop dependencies would require more complex analysis involving control flow and loops, which is not implemented here.

        analysis_table.push_back(entry);
    }

    // Once that all the instructions have been inserted into the analysis table, we can perform 
    // additional passes to identify interloop dependencies, loop invariant dependencies, 
    // and post loop dependencies based on control flow and loop structures. 


}