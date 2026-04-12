// for this hw we always have the assumption of having enough registers.

#include "register_allocation.hpp"
#include "scheduler.hpp"
#include "data_structures.hpp"
#include <stdexcept>


void alloc_b(std::vector<Bundle>& schedule,
             const std::vector<DependencyAnalysisTableEntry>& analysis_table,
             const std::vector<Instruction>& instructions,
             const std::vector<int>& scheduled_cycle) {
    
    // TODO: Phase 1 - Walk instructions in scheduling order and assign fresh 
    //                 destination registers (x1, x2, x3...)

    const int instruction_count = static_cast<int>(instructions.size());

    // build a mapping from instruction id to its analysis table entry
    std::vector<int> analysis_index_by_id(instruction_count, -1);
    for (int i = 0; i < static_cast<int>(analysis_table.size()); ++i) {
        if (analysis_table[i].id >= 0 && analysis_table[i].id < instruction_count) {
            analysis_index_by_id[analysis_table[i].id] = i;
        }
    }

    // helper to get the slot priority for sorting within the same cycle
    auto slot_priority = [](InstructionKind kind) -> int {
        switch (kind) {
            case InstructionKind::Add:
            case InstructionKind::Addi:
            case InstructionKind::Sub:
            case InstructionKind::Mov:
                return 0; // ALU (0 or 1, but both come first)
            case InstructionKind::Mulu:
                return 2;
            case InstructionKind::Ld:
            case InstructionKind::St:
                return 3;
            case InstructionKind::Loop:
            case InstructionKind::LoopPip:
                return 4;
            default:
                return 5;
        }
    };

    // collect all scheduled instruction ids and sort them in scheduling order
    std::vector<int> ordered_ids;
    for (int i = 0; i < instruction_count; ++i) {
        if (scheduled_cycle[i] >= 0) {
            ordered_ids.push_back(i);
        }
    }
    std::sort(ordered_ids.begin(), ordered_ids.end(), [&](int a, int b) {
        if (scheduled_cycle[a] != scheduled_cycle[b])
            return scheduled_cycle[a] < scheduled_cycle[b];
        int pa = slot_priority(instructions[a].kind);
        int pb = slot_priority(instructions[b].kind);
        if (pa != pb) return pa < pb;
        return a < b; // final tie-break by program order (two ALU instructions in the same cycle)
    });

    // the first part is to assign fresh registers to each instruction's destination
    int next_reg = 1; // we start allocating from x1
    std::vector<int> new_dest_reg(instruction_count, -1); // maps instruction id with the newly assigned dest register

    for (const int id : ordered_ids) {

        const Instruction& instr = instructions[id];

        // We skip instructions that don't produce a value in a register (loop instructions, stores, and mov to special LC/EC)
        if (instr.kind == InstructionKind::Loop || instr.kind == InstructionKind::LoopPip) {
            continue;
        }
        // mov to LC or EC: destination is a special register, skip
        if (instr.kind == InstructionKind::Mov && instr.destination_register == -1) {
            continue;
        }
        // st has no destination register
        if (instr.kind == InstructionKind::St) {
            continue;
        }

        if (instr.destination_register != -1) {
            new_dest_reg[id] = next_reg; // assign a new register
            next_reg++; // increment for the next instruction
        }
    }

    
   
    
    // TODO: Phase 2 - Update source operands to use the newly assigned 
    //                 registers of their producers (using analysis_table)

    // Here we link each operand to its producer's new register
    // For each instruction, new_source_regs[id] will hold the rewritten source registers
    // in the same order as instructions[id].source_registers
    std::vector<std::vector<int>> new_source_regs(instruction_count);

    for (const int id : ordered_ids) {
        const Instruction& instr = instructions[id];
        const int ai = analysis_index_by_id[id];

        // instructions with no sources or no analysis entry
        if (ai < 0 || instr.source_registers.empty()) {
            new_source_regs[id] = {}; 
            continue;
        }

        const DependencyAnalysisTableEntry& entry = analysis_table[ai];

        // gather all dependency ids into one flat list for easier searching
        std::vector<int> all_deps;
        all_deps.insert(all_deps.end(), entry.local_dependencies.begin(), entry.local_dependencies.end());
        all_deps.insert(all_deps.end(), entry.interloop_dependencies.begin(), entry.interloop_dependencies.end());
        all_deps.insert(all_deps.end(), entry.loop_invariant_dependencies.begin(), entry.loop_invariant_dependencies.end());
        all_deps.insert(all_deps.end(), entry.post_loop_dependencies.begin(), entry.post_loop_dependencies.end());

        for (const int src_reg : instr.source_registers) {
            int best_producer = -1;
            bool found_bb0_producer = false;

            for (const int dep_id : all_deps) {
                if (dep_id < 0 || dep_id >= instruction_count) continue;
                // check if this dependency actually produces the register we're looking for
                if (instructions[dep_id].destination_register != src_reg) continue;

                if (best_producer == -1) {
                    best_producer = dep_id;
                    found_bb0_producer = (instructions[dep_id].basic_block == BasicBlock::BB0);
                } else if (instructions[dep_id].basic_block == BasicBlock::BB0 && !found_bb0_producer) {
                    // two producers exist, prefer BB0 (see Section 3.3.1 phase 2)
                    best_producer = dep_id;
                    found_bb0_producer = true;
                }
            }

            if (best_producer != -1 && new_dest_reg[best_producer] != -1) {
                new_source_regs[id].push_back(new_dest_reg[best_producer]);
            } else {
                // no producer found yet, we'll handle this in Phase 4
                new_source_regs[id].push_back(-1);
            }
        }
    }
    
    // TODO: Phase 3 - Handle interloop dependencies by inserting 'mov' 
    //                 instructions at the end of the loop body

    
    
    // TODO: Phase 4 - Assign fresh registers to any source operands that 
    //                 still have no known producer
}