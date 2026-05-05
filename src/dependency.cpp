#include "dependency.hpp"
#include "data_structures.hpp"
#include "io_handler.hpp"
#include <algorithm>
#include <stdexcept>

void update_instructions_bb(std::vector<Instruction>& instructions) {
    BasicBlock current_bb = BasicBlock::BB0; // We start assuming that we are in BB0, since the first instructions before the loop belong to BB0
    for (int i = 0; i < static_cast<int>(instructions.size()); ++i) {
        Instruction& instr = instructions[i];
        instr.basic_block = current_bb;

        if (instr.kind == InstructionKind::Loop || instr.kind == InstructionKind::LoopPip) {
            current_bb = BasicBlock::BB1; 
            // When we find the loop instruction, we can start assigning instructions to BB1
            instr.basic_block = current_bb; // We correct the BB of the loop instruction itself 
            if (instr.has_loop_target && instr.loop_target >= 0 && instr.loop_target < i) {
                int loop_destination_id = instr.loop_target; // We identify the target of the loop 
                for (int j = loop_destination_id; j < i; ++j) { 
                    // We set the BB of all the instructions in beetween to BB1
                    instructions[j].basic_block = current_bb;
                }
            }
            else {
                // invalid loop target means we cannot identify BB1 correctly
                throw std::runtime_error("Loop instruction at index " + std::to_string(i) + " does not have a valid loop target.");
            }

            // everything after the loop marker is BB2
            current_bb = BasicBlock::BB2;
        }
    }
}

void dependency_analysis(const std::vector<Instruction>& instructions, std::vector<DependencyAnalysisTableEntry>& analysis_table) {

   
    // this function analyze the instructions and fill the analysis_table with dependency information
    for (int i = 0; i < static_cast<int>(instructions.size()); ++i) {
        const Instruction& instr = instructions[i];
        DependencyAnalysisTableEntry entry;
        entry.address = i;
        entry.id = i; // for simplicity, we can use the instruction index as its ID
        entry.instruction_type = instr.kind;

        if (entry.instruction_type == InstructionKind::Loop || entry.instruction_type == InstructionKind::LoopPip) {
            // loop instructions do not write a GP destination register
            analysis_table.push_back(entry);
            continue;
        }
        else {
            // for regular instructions this is the written register (or -1 if none)
            entry.destination_register = instr.destination_register;
        }

        // pass 1: look backward for producers that already happened
        for (int j = i - 1; j >= 0; --j) {
            const Instruction& prev_instr = instructions[j];
            if (prev_instr.destination_register != -1) {

                // does current instruction read what prev_instr writes?
                if (std::find(instr.source_registers.begin(), instr.source_registers.end(), prev_instr.destination_register) != instr.source_registers.end()) {
                    if (prev_instr.basic_block == instr.basic_block) {
                        // if they are in the same basic block, it's a local dependency
                        entry.local_dependencies.push_back(j);
                    }
                    else if (prev_instr.basic_block == BasicBlock::BB0 && (instr.basic_block == BasicBlock::BB1 || instr.basic_block == BasicBlock::BB2)) {
                        // if the previous instruction (producer) is in BB0 and the current instruction is in BB1 or BB2, it's a loop invariant dependency
                        entry.loop_invariant_dependencies.push_back(j);
                    }
                    else if (prev_instr.basic_block == BasicBlock::BB1 && instr.basic_block == BasicBlock::BB2) {
                        // if the previous instruction (producer) is in BB1 and the current instruction is in BB2, it's a post loop dependency
                        entry.post_loop_dependencies.push_back(j);
                    }
                    // If the producer is in BB1 (as the consumer) but is after the consumer in the loop, we have an interloop dependency
                    // We will have to look for interloop dependencies in a second pass, once we have identified all the instructions that belong to the loop (BB1).
                }
            }
        }

        // pass 2: look forward to catch interloop deps (BB1 consumer value ot the next iteration)
        // if a source is already satisfied by a local BB1 producer, do not also tag it as interloop
        std::vector<int> local_bb1_satisfied_regs;
        if (instr.basic_block == BasicBlock::BB1) {
            for (int k : entry.local_dependencies) {
                if (k >= 0 && k < static_cast<int>(instructions.size()) &&
                    instructions[k].destination_register != -1) {
                    local_bb1_satisfied_regs.push_back(instructions[k].destination_register);
                }
            }
        }

        for (int j = i; j < static_cast<int>(instructions.size()); ++j) { // we start from i because we want to include the current instruction as well, in case it is a producer for itself in the next loop 
            const Instruction& next_instr = instructions[j];
            if (next_instr.destination_register != -1) {
                // check if the next instruction reads from a register that is written by the current instruction
                if (std::find(instr.source_registers.begin(), instr.source_registers.end(), next_instr.destination_register) != instr.source_registers.end()) {
                    if (next_instr.basic_block == BasicBlock::BB1 && instr.basic_block == BasicBlock::BB1 && j >= i) {

                        // skip if a local BB1 producer already satisfies this source register in the current iteration
                        if (std::find(local_bb1_satisfied_regs.begin(), local_bb1_satisfied_regs.end(),
                                      next_instr.destination_register) != local_bb1_satisfied_regs.end()) {
                            continue;
                        }

                        // BB1 producer appears at/after consumer so it's interloop
                        entry.interloop_dependencies.push_back(j);

                        // if the producer (initializer) is in BB0 and the consumer is in BB1 and gets updated every loop, the producer in BB0
                        // is also considered as an interloop dependency (see example in PDF)
                        // We have to check the previously assigned loop invariant dependencies
                        std::vector<int> to_remove;
                        for (int k : entry.loop_invariant_dependencies) {
                            if (instructions[k].basic_block == BasicBlock::BB0 && instructions[k].destination_register == next_instr.destination_register) {
                                entry.interloop_dependencies.push_back(k);
                                to_remove.push_back(k);
                            }
                        }
                        // now erase after iteration is complete
                        for (int k : to_remove) {
                            entry.loop_invariant_dependencies.erase(std::remove(entry.loop_invariant_dependencies.begin(), entry.loop_invariant_dependencies.end(), k), entry.loop_invariant_dependencies.end());
                        }

                    } 
                    
                }
            }
        }

        analysis_table.push_back(entry);
    }

}