#include "include/dependency.hpp"
#include "include/data_structures.hpp"
#include "include/io_handler.hpp"

void dependency_analysis(const std::vector<Instruction>& instructions, std::vector<DependencyAnalysisTableEntry>& analysis_table) {
   
    // This function should analyze the instructions and fill the analysis_table with dependency information.
    for (int i = 0; i < static_cast<int>(instructions.size()); ++i) {
        const Instruction& instr = instructions[i];
        DependencyAnalysisTableEntry entry;
        entry.address = i;
        entry.id = i; // For simplicity, we can use the instruction index as its ID.
        entry.instruction_type = instr.kind;
        entry.destination_register = instr.destination_register;

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

        // Interloop, loop invariant, and post loop dependencies would require more complex analysis involving control flow and loops, which is not implemented here.

        analysis_table.push_back(entry);
    }

    // Once that all the instructions have been inserted into the analysis table, we can perform 
    // additional passes to identify interloop dependencies, loop invariant dependencies, 
    // and post loop dependencies based on control flow and loop structures. 

    
}