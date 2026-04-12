// for this hw we always have the assumption of having enough registers.

#include "register_allocation.hpp"
#include "scheduler.hpp"
#include "data_structures.hpp"
#include <stdexcept>
#include <map>

/* 
When the scheduler finishes, it has placed all instructions into specific cycles (Bundles).
However, the instructions are still using their original "virtual" register names (like v1, v2).
If a loop executes multiple times, or if instructions are moved around, these original names can clash 
(e.g., two different operations trying to write to v1 at the same time).
The goal of alloc_b is to give every destination a brand new, unique physical register name (x1, x2, x3...) 
and then reconnect all the "wires" (source operands) so the data flows correctly without conflicts.
*/

void alloc_b(std::vector<Bundle>& schedule,
             const std::vector<DependencyAnalysisTableEntry>& analysis_table,
             const std::vector<Instruction>& instructions,
             const std::vector<int>& scheduled_cycle) {
    
    // TODO: Phase 1 - Walk instructions in scheduling order and assign fresh 
    //                 destination registers (x1, x2, x3...)

    const int instruction_count = static_cast<int>(instructions.size());

    // build a mapping from instruction id to its analysis table entry
    // we create a lookup vector where the index is the instruction id and
    //  the value is the index of its entry in the analysis table. This way
    // if I want to find the analysis entry for instruction with id = 5, 
    // I can just do analysis_table[analysis_index_by_id[5]] (that might for example be in row 2 of the table)
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

    // Phase 3: insert mov instructions for interloop dependencies
    // Find the loop instruction position and the loop body range
    int loop_id = -1;
    int loop_cycle = -1;
    int loop_beginning = -1;
    for (int i = 0; i < instruction_count; ++i) {
        if (instructions[i].kind == InstructionKind::Loop || instructions[i].kind == InstructionKind::LoopPip) {
            loop_id = i;
            loop_cycle = scheduled_cycle[i];
        }
    }
    // find loop_beginning: first BB1 instruction cycle
    for (const int id : ordered_ids) {
        if (instructions[id].basic_block == BasicBlock::BB1) {
            loop_beginning = scheduled_cycle[id];
            break;
        }
    }

    // collect all interloop mov pairs: (dest = bb0 register, src = bb1 register, bb1 producer id)
    struct MovPair {
        int dest_reg;   // the BB0 producer's new register (where consumers read from)
        int src_reg;    // the BB1 producer's new register (the actual value to propagate)
        int bb1_prod_id; // the BB1 producer instruction id (needed for latency check)
    };
    std::vector<MovPair> mov_pairs;

    for (const int id : ordered_ids) {
        if (instructions[id].basic_block != BasicBlock::BB1) continue;
        const int ai = analysis_index_by_id[id];
        if (ai < 0) continue;
        const DependencyAnalysisTableEntry& entry = analysis_table[ai];

        for (const int src_reg : instructions[id].source_registers) {
            int bb0_producer = -1;
            int bb1_producer = -1;

            // search all dependency lists for producers of this source register
            auto search_deps = [&](const std::vector<int>& deps) {
                for (const int dep_id : deps) {
                    if (dep_id < 0 || dep_id >= instruction_count) continue;
                    if (instructions[dep_id].destination_register != src_reg) continue;
                    if (instructions[dep_id].basic_block == BasicBlock::BB0) {
                        bb0_producer = dep_id;
                    } else if (instructions[dep_id].basic_block == BasicBlock::BB1) {
                        bb1_producer = dep_id;
                    }
                }
            };

            search_deps(entry.local_dependencies);
            search_deps(entry.interloop_dependencies);
            search_deps(entry.loop_invariant_dependencies);

            // we need a mov only when both BB0 and BB1 produce the same register
            if (bb0_producer != -1 && bb1_producer != -1 &&
                new_dest_reg[bb0_producer] != -1 && new_dest_reg[bb1_producer] != -1) {
                // avoid duplicates
                bool already_added = false;
                for (const auto& mp : mov_pairs) {
                    if (mp.dest_reg == new_dest_reg[bb0_producer] && mp.src_reg == new_dest_reg[bb1_producer]) {
                        already_added = true;
                        break;
                    }
                }
                if (!already_added) {
                    mov_pairs.push_back({new_dest_reg[bb0_producer], new_dest_reg[bb1_producer], bb1_producer});
                }
            }
        }
    }

    // place the mov instructions at the end of the loop body
    for (const auto& mp : mov_pairs) {
        // earliest cycle this mov can execute (respects latency of the BB1 producer)
        int earliest_mov = scheduled_cycle[mp.bb1_prod_id] + instruction_latency(instructions[mp.bb1_prod_id].kind);

        // try to place in the last bundle before the loop instruction
        // if it doesn't fit, push the loop instruction down
        while (true) {
            int target_cycle = loop_cycle; // try the same bundle as loop first
            // but the mov must respect the producer's latency
            if (target_cycle < earliest_mov) {
                // push loop down to make room
                schedule[loop_cycle].BRANCH = "nop";
                loop_cycle = earliest_mov;
                ensure_bundle_capacity(schedule, loop_cycle);
                while (!can_place_in_bundle(schedule[loop_cycle], InstructionKind::Loop)) {
                    ++loop_cycle;
                    ensure_bundle_capacity(schedule, loop_cycle);
                }
                schedule[loop_cycle].BRANCH = instructions[loop_id].raw_text;
                scheduled_cycle[loop_id] = loop_cycle;
            }

            // check if there's a free ALU slot in any bundle between earliest_mov and loop_cycle
            bool placed = false;
            for (int c = std::max(earliest_mov, loop_beginning); c <= loop_cycle; ++c) {
                ensure_bundle_capacity(schedule, c);
                if (can_place_in_bundle(schedule[c], InstructionKind::Mov)) {
                    std::string mov_text = "mov x" + std::to_string(mp.dest_reg) + ", x" + std::to_string(mp.src_reg);
                    place_in_bundle(schedule[c], InstructionKind::Mov, mov_text);
                    placed = true;
                    break;
                }
            }

            if (placed) break;

            // no ALU slot available, push loop down by one
            schedule[loop_cycle].BRANCH = "nop";
            ++loop_cycle;
            ensure_bundle_capacity(schedule, loop_cycle);
            while (!can_place_in_bundle(schedule[loop_cycle], InstructionKind::Loop)) {
                ++loop_cycle;
                ensure_bundle_capacity(schedule, loop_cycle);
            }
            schedule[loop_cycle].BRANCH = instructions[loop_id].raw_text;
            scheduled_cycle[loop_id] = loop_cycle;
        }
    }

    // Phase 4: assign unused registers to operands with no known producer
    // if two instructions read the same original register and neither has a producer,
    // they should get the same new register
    std::map<int, int> orphan_reg_map; // original register -> newly assigned register

    for (const int id : ordered_ids) {
        const Instruction& instr = instructions[id];
        for (int s = 0; s < static_cast<int>(new_source_regs[id].size()); ++s) {
            if (new_source_regs[id][s] == -1) {
                int orig_reg = instr.source_registers[s];
                if (orphan_reg_map.find(orig_reg) != orphan_reg_map.end()) {
                    new_source_regs[id][s] = orphan_reg_map[orig_reg];
                } else {
                    orphan_reg_map[orig_reg] = next_reg;
                    new_source_regs[id][s] = next_reg;
                    next_reg++;
                }
            }
        }
    }

    // Rewrite all bundle slots with new register names
    for (const int id : ordered_ids) {
        const Instruction& instr = instructions[id];
        int cycle = scheduled_cycle[id];
        if (cycle < 0) continue;

        // loop instruction: update the target to loop_beginning
        if (instr.kind == InstructionKind::Loop || instr.kind == InstructionKind::LoopPip) {
            schedule[cycle].BRANCH = instr.opcode + " " + std::to_string(loop_beginning);
            continue;
        }

        // skip mov to LC/EC (text stays as-is)
        if (instr.kind == InstructionKind::Mov && instr.destination_register == -1) continue;

        std::string new_text = rebuild_instruction_text(instr, new_dest_reg[id], new_source_regs[id]);

        // place in the correct slot
        Bundle& b = schedule[cycle];
        if (is_alu_kind(instr.kind)) {
            if (b.ALU0 == instr.raw_text) b.ALU0 = new_text;
            else if (b.ALU1 == instr.raw_text) b.ALU1 = new_text;
        } else if (instr.kind == InstructionKind::Mulu) {
            b.MUL = new_text;
        } else if (instr.kind == InstructionKind::Ld || instr.kind == InstructionKind::St) {
            b.MEM = new_text;
        }
    }
}