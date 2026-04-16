// for this hw we always have the assumption of having enough registers.

#include "register_allocation.hpp"
#include "data_structures.hpp"
#include <algorithm>
#include <map>
#include <string>

AllocResult alloc_b(std::vector<Bundle>& schedule,
                    const std::vector<DependencyAnalysisTableEntry>& analysis_table,
                    const std::vector<Instruction>& instructions,
                    std::vector<int>& scheduled_cycle) { // not const because Phase 3 may push the loop down and update its cycle

    const int instruction_count = static_cast<int>(instructions.size());

    // when two instructions are scheduled in the same cycle, we need to know
    // which one comes first. the bundle has a fixed slot order:
    // ALU0, ALU1, MUL, MEM, BRANCH
    // this lambda returns a priority number matching that order
    auto slot_priority = [](InstructionKind kind) -> int {
        switch (kind) {
            case InstructionKind::Add:
            case InstructionKind::Addi:
            case InstructionKind::Sub:
            case InstructionKind::Mov:
                return 0; // ALU instructions go first
            case InstructionKind::Mulu:
                return 2; // MUL slot is after ALU
            case InstructionKind::Ld:
            case InstructionKind::St:
                return 3; // MEM slot
            case InstructionKind::Loop:
            case InstructionKind::LoopPip:
                return 4; // BRANCH slot is last
            default:
                return 5;
        }
    };

    // collect all instruction ids that have been placed in the schedule
    // (scheduled_cycle[id] >= 0 means the scheduler assigned it a cycle)
    std::vector<int> ordered_ids;
    for (int i = 0; i < instruction_count; ++i) {
        if (scheduled_cycle[i] >= 0) {
            ordered_ids.push_back(i);
        }
    }

    // sort them in the order they appear in the schedule:
    // first by cycle number, then by slot priority within the same cycle,
    // then by program order as a final tiebreak (e.g. two ALU instrs in same cycle)
    std::sort(ordered_ids.begin(), ordered_ids.end(), [&](int a, int b) {
        if (scheduled_cycle[a] != scheduled_cycle[b])
            return scheduled_cycle[a] < scheduled_cycle[b];
        int pa = slot_priority(instructions[a].kind);
        int pb = slot_priority(instructions[b].kind);
        if (pa != pb) return pa < pb;
        return a < b;
    });

    // BB2 instructions aren't scheduled yet (that happens after alloc_b)
    // but we still need to assign them registers and compute their sources
    // so we append them at the end in program order
    for (int i = 0; i < instruction_count; ++i) {
        if (instructions[i].basic_block == BasicBlock::BB2 && scheduled_cycle[i] < 0) {
            ordered_ids.push_back(i);
        }
    }

    // ========================================================================
    // PHASE 1: assign a fresh unique register to each instruction's destination
    // ========================================================================
    // we walk instructions in scheduling order and hand out x1, x2, x3...
    // this eliminates all anti-dependencies and output dependencies
    // because every instruction now writes to a different register
    int next_reg = 1;
    std::vector<int> new_dest_reg(instruction_count, -1); // pre-allocate with -1 meaning "no register assigned" for sanity checks

    for (const int id : ordered_ids) {
        const Instruction& instr = instructions[id];

        // loop/loop.pip don't write to a general-purpose register
        if (instr.kind == InstructionKind::Loop || instr.kind == InstructionKind::LoopPip) continue;

        // "mov LC, 100" or "mov EC, 1" write to special registers, not xN
        // destination_register is -1 for these because the parser couldn't parse LC/EC as a register number
        if (instr.kind == InstructionKind::Mov && instr.destination_register == -1) continue;

        // store instructions don't produce a register value
        if (instr.kind == InstructionKind::St) continue;

        // everything else (add, addi, sub, mulu, ld, mov xN) gets a fresh register
        if (instr.destination_register != -1) {
            new_dest_reg[id] = next_reg;
            next_reg++;
        }
    }

    // ========================================================================
    // PHASE 2: link each source operand to the new register of its producer
    // ========================================================================
    // for each instruction, we look at what registers it reads (source_registers)
    // then we search the dependency table to find which instruction wrote that register
    // and we replace the source with that producer's newly assigned register from Phase 1
    std::vector<std::vector<int>> new_source_regs(instruction_count);

    for (const int id : ordered_ids) {
        const Instruction& instr = instructions[id];

        // if the instruction has no sources or no analysis entry, nothing to do
        if (id < 0 || id >= static_cast<int>(analysis_table.size()) || instr.source_registers.empty()) {
            new_source_regs[id] = {};
            continue;
        }

        const DependencyAnalysisTableEntry& entry = analysis_table[id];

        // merge all four dependency lists into one flat list
        // this makes the search below simpler — we just scan one list
        std::vector<int> all_deps;
        all_deps.insert(all_deps.end(), entry.local_dependencies.begin(), entry.local_dependencies.end());
        all_deps.insert(all_deps.end(), entry.interloop_dependencies.begin(), entry.interloop_dependencies.end());
        all_deps.insert(all_deps.end(), entry.loop_invariant_dependencies.begin(), entry.loop_invariant_dependencies.end());
        all_deps.insert(all_deps.end(), entry.post_loop_dependencies.begin(), entry.post_loop_dependencies.end());

        // for each source register this instruction reads...
        for (const int src_reg : instr.source_registers) {
            int best_producer = -1;
            bool found_preferred = false;

            // BB1 consumers with two producers (BB0 + BB1) should prefer BB0
            // because the mov at end of loop body will copy BB1's value into BB0's register
            //
            // BB2 consumers should prefer BB1 producers
            // because BB2 runs after the last iteration and wants the final value from BB1
            BasicBlock preferred_bb = (instr.basic_block == BasicBlock::BB2) ? BasicBlock::BB1 : BasicBlock::BB0;

            // scan all dependencies to find who produces this source register
            for (const int dep_id : all_deps) {
                if (dep_id < 0 || dep_id >= instruction_count) continue;
                if (instructions[dep_id].destination_register != src_reg) continue;
                if (instructions[dep_id].basic_block == preferred_bb) {
                    // For BB0 preference, choose the latest-scheduled producer.
                    // For BB1 preference, first match is enough.
                    if (!found_preferred ||
                        (preferred_bb == BasicBlock::BB0 && scheduled_cycle[dep_id] > scheduled_cycle[best_producer])) {
                        best_producer = dep_id;
                        found_preferred = true;
                    }
                } else if (!found_preferred && best_producer == -1) {
                    // Keep one fallback candidate until a preferred producer is found.
                    best_producer = dep_id;
                }
            }

            // if we found a producer and it got a register in Phase 1, use it
            if (best_producer != -1 && new_dest_reg[best_producer] != -1) {
                new_source_regs[id].push_back(new_dest_reg[best_producer]);
            } else {
                // mark as unresolved — Phase 4 will handle it
                new_source_regs[id].push_back(-1);
            }
        }
    }

    // ========================================================================
    // PHASE 3: insert mov instructions for interloop dependencies
    // ========================================================================
    // the problem: in Phase 2, BB1 consumers with two producers (BB0 + BB1)
    // got wired to the BB0 producer's register. that works for iteration 1,
    // but in iteration 2+ the value should come from the BB1 producer.
    //
    // solution: at the end of each iteration, copy the BB1 result into the
    // BB0 register using a mov instruction. example:
    // B (BB0) writes x1, I (BB1) writes x4, consumers read x1
    // -> insert "mov x1, x4" at end of loop body

    // first find where the loop instruction and loop body are
    int loop_id = -1;
    int loop_cycle = -1;
    int loop_beginning = -1;

    for (int i = 0; i < instruction_count; ++i) {
        if (instructions[i].kind == InstructionKind::Loop || instructions[i].kind == InstructionKind::LoopPip) {
            loop_id = i;
            loop_cycle = scheduled_cycle[i];
        }
    }

    // loop_beginning = the cycle where the first BB1 instruction sits
    for (const int id : ordered_ids) {
        if (instructions[id].basic_block == BasicBlock::BB1) {
            loop_beginning = scheduled_cycle[id];
            break;
        }
    }

    // collect all (BB0_register, BB1_register) pairs that need a mov
    struct MovPair {
        int dest_reg;    // BB0 producer's new register (consumers read from this)
        int src_reg;     // BB1 producer's new register (holds the actual new value)
        int bb1_prod_id; // BB1 producer instruction id (need its latency for timing)
    };
    std::vector<MovPair> mov_pairs;

    for (const int id : ordered_ids) {
        // only BB1 instructions can have interloop dependencies
        if (instructions[id].basic_block != BasicBlock::BB1) continue;

        if (id < 0 || id >= static_cast<int>(analysis_table.size())) continue;
        const DependencyAnalysisTableEntry& entry = analysis_table[id];

        // for each source register this BB1 instruction reads...
        for (const int src_reg : instructions[id].source_registers) {
            int bb0_producer = -1;
            int bb1_producer = -1;

            // We define the producer of this register as the instruction that writes to it and is closest to the consumer in the 
            // schedule order (i.e. the latest scheduled producer in BB0, in BB1 it doesn't count).
            auto search_deps = [&](const std::vector<int>& deps) {
                for (const int dep_id : deps) {
                    if (dep_id < 0 || dep_id >= instruction_count) continue;
                    if (instructions[dep_id].destination_register != src_reg) continue;
                    if (instructions[dep_id].basic_block == BasicBlock::BB0) {
                        // if multiple BB0 instructions write the same register,
                        // pick the one scheduled latest — that's the value BB1 actually sees
                        if (bb0_producer == -1 || scheduled_cycle[dep_id] > scheduled_cycle[bb0_producer]) {
                            bb0_producer = dep_id;
                        }
                    } else if (instructions[dep_id].basic_block == BasicBlock::BB1) {
                        bb1_producer = dep_id;
                    }
                }
            };

            // We update bb0_producer and bb1_producer by looking at all the dependencies of the consumer instruction (local, interloop, and loop invariant) 
            // to find which instruction produces the value read by the consumer in BB0 and in BB1.
            search_deps(entry.local_dependencies);
            search_deps(entry.interloop_dependencies);
            search_deps(entry.loop_invariant_dependencies);

            // we only need a mov when BOTH BB0 and BB1 producers produce the same register (we have a conflict!!)
            // if only one produces it, there's no conflict to resolve
            if (bb0_producer != -1 && bb1_producer != -1 &&
                new_dest_reg[bb0_producer] != -1 && new_dest_reg[bb1_producer] != -1) {

                // don't add the same mov pair twice, so we check if it's already in the list before adding 
                // (in case multiple instructions read the same register and have the same producers, we only need one mov)
                bool already_added = false;
                for (const auto& mp : mov_pairs) {
                    // These are the two regusters that are moved (we check if they are already in the list, 
                    // otherwise we put them in the list to add the mov later)
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

    // WE PLACE THE MOV IN THE SCHEDULE:
    // the mov reads from the BB1 producer, so it must respect the producer's latency
    // we try to place it as late as possible (near the loop instruction)
    // if there's no room, we push the loop instruction down
    for (const auto& mp : mov_pairs) {
        // the mov can't execute before the BB1 producer's result is ready
        // it's this result that we have to mov later into the BB0 register, so we have to wait for it to be ready.
        int earliest_mov = scheduled_cycle[mp.bb1_prod_id] + instruction_latency(instructions[mp.bb1_prod_id].kind);

        while (true) {
            // if the loop instruction is before the earliest possible mov cycle,
            // we need to push the loop down to make room
            if (loop_cycle < earliest_mov) {
                // clear the loop from its current position
                schedule[loop_cycle].BRANCH = "nop";
                // move it to at least earliest_mov
                loop_cycle = earliest_mov;
                ensure_bundle_capacity(schedule, loop_cycle);
                // find a cycle with a free BRANCH slot
                while (!can_place_in_bundle(schedule[loop_cycle], InstructionKind::Loop)) {
                    ++loop_cycle;
                    ensure_bundle_capacity(schedule, loop_cycle);
                }
                // place the loop at its new position
                schedule[loop_cycle].BRANCH = instructions[loop_id].raw_text;
                scheduled_cycle[loop_id] = loop_cycle;
            }

            // search backward from loop_cycle to find a free ALU slot
            // we want the mov as late as possible in the loop body
            // so that it doesn't overwrite the register before other instructions read it
            bool placed = false;
            for (int c = loop_cycle; c >= std::max(earliest_mov, loop_beginning); --c) {
                ensure_bundle_capacity(schedule, c);
                if (can_place_in_bundle(schedule[c], InstructionKind::Mov)) {
                    std::string mov_text = "mov x" + std::to_string(mp.dest_reg) + ", x" + std::to_string(mp.src_reg);
                    place_in_bundle(schedule[c], InstructionKind::Mov, mov_text);
                    placed = true;
                    break;
                }
            }

            if (placed) break;

            // couldn't find a free ALU slot anywhere in the loop body
            // push the loop instruction down by one to create more room
            schedule[loop_cycle].BRANCH = "nop";
            ++loop_cycle;
            ensure_bundle_capacity(schedule, loop_cycle);
            while (!can_place_in_bundle(schedule[loop_cycle], InstructionKind::Loop)) {
                ++loop_cycle;
                ensure_bundle_capacity(schedule, loop_cycle);
            }
            schedule[loop_cycle].BRANCH = instructions[loop_id].raw_text;
            scheduled_cycle[loop_id] = loop_cycle;
            // loop back and try again with the extra room
        }
    }

    // ========================================================================
    // PHASE 4: handle orphan operands (source registers with no known producer)
    // ========================================================================
    // some instructions read registers that no other instruction in the program writes to
    // (e.g. function arguments, or base addresses assumed to already be in the register file)
    // we assign them fresh registers, but if two instructions read the same orphan register
    // they must get the same new register (they're reading the same pre-existing value)
    std::map<int, int> orphan_reg_map; // maps original register number -> new register

    for (const int id : ordered_ids) {
        const Instruction& instr = instructions[id];
        for (int s = 0; s < static_cast<int>(new_source_regs[id].size()); ++s) {
            if (new_source_regs[id][s] == -1) { // this source register was unresolved in Phase 2, so it's an orphan that needs a new register
                int orig_reg = instr.source_registers[s];
                // check if we already assigned a register for this orphan
                if (orphan_reg_map.find(orig_reg) != orphan_reg_map.end()) {
                    new_source_regs[id][s] = orphan_reg_map[orig_reg];
                } else {
                    // first time seeing this orphan register — assign a fresh one
                    orphan_reg_map[orig_reg] = next_reg;
                    new_source_regs[id][s] = next_reg;
                    next_reg++;
                }
            }
        }
    }

    // ========================================================================
    // BUNDLE REWRITE: replace original instruction text with new register names
    // ========================================================================
    // up to this point the bundles still contain the original text like "mulu x6, x5, x4"
    // now we overwrite each slot with the rebuilt text using the new registers
    // we skip BB2 instructions because they haven't been placed in the schedule yet
    // (schedule_bb2 runs after alloc_b, and rewrite_bb2_bundles handles them)
    for (const int id : ordered_ids) {
        const Instruction& instr = instructions[id];

        // BB2 isn't in the schedule yet — skip it
        if (instr.basic_block == BasicBlock::BB2) continue;

        int cycle = scheduled_cycle[id];
        if (cycle < 0) continue;

        // loop instruction: just update the branch target to point to loop_beginning
        // (the original target was based on the input program's addresses, not the schedule's)
        if (instr.kind == InstructionKind::Loop || instr.kind == InstructionKind::LoopPip) {
            schedule[cycle].BRANCH = instr.opcode + " " + std::to_string(loop_beginning);
            continue;
        }

        // mov to LC/EC: the text is something like "mov LC, 100" — nothing to change
        // These kinds of instructions have destination_register == -1.
        if (instr.kind == InstructionKind::Mov && instr.destination_register == -1) continue;

        // rebuild the instruction text with the new registers
        std::string new_text = rebuild_instruction_text(instr, new_dest_reg[id], new_source_regs[id]);

        // find which slot in the bundle this instruction occupies and overwrite it
        // we match by raw_text because that's what was originally written into the slot
        Bundle& b = schedule[cycle];
        if (is_alu_kind(instr.kind)) {
            // ALU instructions could be in either ALU0 or ALU1
            if (b.ALU0 == instr.raw_text) b.ALU0 = new_text;
            else if (b.ALU1 == instr.raw_text) b.ALU1 = new_text;
        } else if (instr.kind == InstructionKind::Mulu) {
            b.MUL = new_text;
        } else if (instr.kind == InstructionKind::Ld || instr.kind == InstructionKind::St) {
            b.MEM = new_text;
        }
    }

    // return the allocation results so that BB2 can be rewritten later
    // after schedule_bb2 places BB2 instructions into the schedule
    AllocResult result;
    result.new_dest_reg = new_dest_reg;
    result.new_source_regs = new_source_regs;
    result.loop_beginning = loop_beginning;
    return result;
}

// called after schedule_bb2 places BB2 instructions into the schedule
// rewrites their bundle slots with the new register names computed by alloc_b
void rewrite_bb2_bundles(std::vector<Bundle>& schedule,
                         const std::vector<Instruction>& instructions,
                         const std::vector<int>& scheduled_cycle,
                         const AllocResult& alloc) {

    const int instruction_count = static_cast<int>(instructions.size());

    for (int id = 0; id < instruction_count; ++id) {
        const Instruction& instr = instructions[id];

        // only interested in BB2 instructions
        if (instr.basic_block != BasicBlock::BB2) continue;

        int cycle = scheduled_cycle[id];
        if (cycle < 0) continue; // shouldn't happen but just in case

        // rebuild instruction text with the new registers from alloc_b
        std::string new_text = rebuild_instruction_text(instr, alloc.new_dest_reg[id], alloc.new_source_regs[id]);

        // find the slot and overwrite
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

AllocResult alloc_r(std::vector<Bundle>& schedule,
                    const std::vector<DependencyAnalysisTableEntry>& analysis_table,
                    const std::vector<Instruction>& instructions,
                    std::vector<int>& scheduled_cycle,
                    int ii,
                    int loop_beginning,
                    int num_stages,
                    const std::vector<int>& stage_by_id) {

    const int instruction_count = static_cast<int>(instructions.size());

    // same slot priority and ordering as alloc_b
    auto slot_priority = [](InstructionKind kind) -> int {
        switch (kind) {
            case InstructionKind::Add:
            case InstructionKind::Addi:
            case InstructionKind::Sub:
            case InstructionKind::Mov:
                return 0;
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
        return a < b;
    });

    // append BB2 instructions at the end (not scheduled yet)
    for (int i = 0; i < instruction_count; ++i) {
        if (instructions[i].basic_block == BasicBlock::BB2 && scheduled_cycle[i] < 0) {
            ordered_ids.push_back(i);
        }
    }

    std::vector<int> new_dest_reg(instruction_count, -1);
    std::vector<std::vector<int>> new_source_regs(instruction_count);

    // ========================================================================
    // PHASE 1: assign rotating registers to BB1 destinations
    // ========================================================================
    // BB1 destinations get rotating registers starting from x32
    // spaced by (num_stages + 1) to avoid conflicts across iterations
    int rotating_stride = num_stages + 1;
    int next_rotating_reg = 32;

    for (const int id : ordered_ids) {
        if (instructions[id].basic_block != BasicBlock::BB1) continue;
        const Instruction& instr = instructions[id];

        // same skips as alloc_b
        if (instr.kind == InstructionKind::Loop || instr.kind == InstructionKind::LoopPip) continue;
        if (instr.kind == InstructionKind::Mov && instr.destination_register == -1) continue;
        if (instr.kind == InstructionKind::St) continue;

        if (instr.destination_register != -1) {
            new_dest_reg[id] = next_rotating_reg;
            next_rotating_reg += rotating_stride;
        }
    }

    // ========================================================================
    // PHASE 2: assign static registers to loop invariants
    // ========================================================================
    // loop invariant producers are BB0 instructions that feed BB1 but whose
    // value never changes during the loop. they get simple static registers.
    // however if a BB0 instruction is also an interloop initializer (it appears
    // in some BB1 instruction's interloop_dependencies), we skip it here —
    // it gets handled in Phase 4 with rotation offsets.
    int next_static_reg = 1;

    // first build a set of BB0 instruction ids that appear in interloop deps
    // these are interloop initializers, not pure loop invariants.
    // IMPORTANT: when multiple BB0 instructions write the same register and all
    // appear in interloop_dependencies, only the LAST one (latest in program order)
    // is the actual initializer. Earlier ones just feed later BB0 instructions.
    std::vector<bool> is_interloop_initializer(instruction_count, false);
    for (const int id : ordered_ids) {
        if (instructions[id].basic_block != BasicBlock::BB1) continue;
        if (id < 0 || id >= static_cast<int>(analysis_table.size())) continue;
        const DependencyAnalysisTableEntry& entry = analysis_table[id];

        // group BB0 interloop deps by the register they write
        // only the latest one per register is the true initializer
        std::map<int, int> latest_bb0_per_reg; // dest_register -> latest BB0 instruction id
        for (const int dep_id : entry.interloop_dependencies) {
            if (dep_id < 0 || dep_id >= instruction_count) continue;
            if (instructions[dep_id].basic_block != BasicBlock::BB0) continue;
            int dreg = instructions[dep_id].destination_register;
            if (dreg < 0) continue;
            if (latest_bb0_per_reg.find(dreg) == latest_bb0_per_reg.end() || dep_id > latest_bb0_per_reg[dreg]) {
                latest_bb0_per_reg[dreg] = dep_id;
            }
        }
        for (const auto& kv : latest_bb0_per_reg) {
            is_interloop_initializer[kv.second] = true;
        }
    }

    // now assign static regs to pure loop invariant producers
    // track which BB0 ids we've already assigned to avoid duplicates
    std::vector<bool> invariant_assigned(instruction_count, false);
    for (const int id : ordered_ids) {
        if (instructions[id].basic_block != BasicBlock::BB1) continue;
        if (id < 0 || id >= static_cast<int>(analysis_table.size())) continue;
        const DependencyAnalysisTableEntry& entry = analysis_table[id];

        for (const int dep_id : entry.loop_invariant_dependencies) {
            if (dep_id < 0 || dep_id >= instruction_count) continue;
            if (instructions[dep_id].basic_block != BasicBlock::BB0) continue;
            // skip if this BB0 instruction is also an interloop initializer
            if (is_interloop_initializer[dep_id]) continue;
            // skip if already assigned
            if (invariant_assigned[dep_id]) continue;

            new_dest_reg[dep_id] = next_static_reg;
            next_static_reg++;
            invariant_assigned[dep_id] = true;
        }
    }

    // ========================================================================
    // PHASE 3: link BB1 source operands using stage/iteration offsets
    // ========================================================================
    // for each BB1 instruction's source, find the producer and apply:
    //   loop invariant -> just use the static register from Phase 2
    //   local dep (same iteration) -> equation 3: reg + (St(consumer) - St(producer))
    //   interloop dep (prev iteration, BB1 producer) -> equation 4: reg + (St(consumer) - St(producer)) + 1
    //   interloop dep pointing to BB0 initializer -> skip here, resolved via the BB0 dest in Phase 4

    for (const int id : ordered_ids) {
        const Instruction& instr = instructions[id];

        if (id < 0 || id >= static_cast<int>(analysis_table.size()) || instr.source_registers.empty()) {
            new_source_regs[id] = {};
            continue;
        }

        // BB0 and BB2 operands are handled in Phase 4
        if (instr.basic_block != BasicBlock::BB1) {
            // placeholder — Phase 4 will fill these
            for (int s = 0; s < static_cast<int>(instr.source_registers.size()); ++s) {
                new_source_regs[id].push_back(-1);
            }
            continue;
        }

        const DependencyAnalysisTableEntry& entry = analysis_table[id];
        int consumer_stage = stage_by_id[id];

        for (const int src_reg : instr.source_registers) {
            int resolved_reg = -1;

            // check loop invariant dependencies first
            for (const int dep_id : entry.loop_invariant_dependencies) {
                if (dep_id < 0 || dep_id >= instruction_count) continue;
                if (instructions[dep_id].destination_register != src_reg) continue;
                // pure loop invariant — use static register directly
                if (new_dest_reg[dep_id] != -1) {
                    resolved_reg = new_dest_reg[dep_id];
                }
                break;
            }

            // check local dependencies (same iteration, same BB1)
            if (resolved_reg == -1) {
                for (const int dep_id : entry.local_dependencies) {
                    if (dep_id < 0 || dep_id >= instruction_count) continue;
                    if (instructions[dep_id].destination_register != src_reg) continue;
                    if (new_dest_reg[dep_id] == -1) continue;

                    int producer_stage = stage_by_id[dep_id];
                    // equation 3: produced_reg + (St(consumer) - St(producer))
                    resolved_reg = new_dest_reg[dep_id] + (consumer_stage - producer_stage);
                    break;
                }
            }

            // check interloop dependencies (previous iteration)
            if (resolved_reg == -1) {
                for (const int dep_id : entry.interloop_dependencies) {
                    if (dep_id < 0 || dep_id >= instruction_count) continue;
                    if (instructions[dep_id].destination_register != src_reg) continue;

                    if (instructions[dep_id].basic_block == BasicBlock::BB1) {
                        if (new_dest_reg[dep_id] == -1) continue;
                        int producer_stage = stage_by_id[dep_id];
                        // equation 4: produced_reg + (St(consumer) - St(producer)) + 1
                        resolved_reg = new_dest_reg[dep_id] + (consumer_stage - producer_stage) + 1;
                        break;
                    }
                    // BB0 interloop initializer — the consumer reads the same register
                    // as the BB1 interloop producer but with the +1 offset.
                    // we need to find the corresponding BB1 producer for this register.
                    if (instructions[dep_id].basic_block == BasicBlock::BB0) {
                        // find the BB1 instruction that also produces this register
                        // (it's in the interloop deps too)
                        for (const int other_dep : entry.interloop_dependencies) {
                            if (other_dep < 0 || other_dep >= instruction_count) continue;
                            if (instructions[other_dep].destination_register != src_reg) continue;
                            if (instructions[other_dep].basic_block == BasicBlock::BB1) {
                                if (new_dest_reg[other_dep] == -1) continue;
                                int producer_stage = stage_by_id[other_dep];
                                // same as equation 4
                                resolved_reg = new_dest_reg[other_dep] + (consumer_stage - producer_stage) + 1;
                                break;
                            }
                        }
                        if (resolved_reg != -1) break;
                    }
                }
            }

            new_source_regs[id].push_back(resolved_reg);
        }
    }

    // ========================================================================
    // PHASE 4: allocate BB0 and BB2 registers
    // ========================================================================

    // CASE A: BB0 interloop initializers
    // if a BB0 instruction is an interloop initializer, its dest register
    // must match the BB1 producer's register with offsets so that the first
    // iteration reads the correct value
    for (int id = 0; id < instruction_count; ++id) {
        if (instructions[id].basic_block != BasicBlock::BB0) continue;
        if (!is_interloop_initializer[id]) continue;
        if (new_dest_reg[id] != -1) continue; // already assigned

        int orig_dest = instructions[id].destination_register;
        if (orig_dest == -1) continue;

        // find the BB1 instruction that produces the same register and is
        // listed as an interloop dependency consumer of this BB0 instruction
        for (const int consumer_id : ordered_ids) {
            if (instructions[consumer_id].basic_block != BasicBlock::BB1) continue;
            if (consumer_id < 0 || consumer_id >= static_cast<int>(analysis_table.size())) continue;
            const DependencyAnalysisTableEntry& entry = analysis_table[consumer_id];

            for (const int dep_id : entry.interloop_dependencies) {
                if (dep_id != id) continue; // we want the entry that lists THIS bb0 instruction

                // now find the BB1 producer of the same register in the interloop deps
                for (const int other_dep : entry.interloop_dependencies) {
                    if (other_dep < 0 || other_dep >= instruction_count) continue;
                    if (instructions[other_dep].basic_block != BasicBlock::BB1) continue;
                    if (instructions[other_dep].destination_register != orig_dest) continue;
                    if (new_dest_reg[other_dep] == -1) continue;

                    int producer_stage = stage_by_id[other_dep];
                    // dest = produced_reg + 1 - St(producer)
                    new_dest_reg[id] = new_dest_reg[other_dep] + 1 - producer_stage;
                    goto done_bb0_initializer;
                }
            }
        }
        done_bb0_initializer:;
    }

    // CASE B: BB0 instructions with local dependencies that haven't been assigned yet
    // (non-interloop, non-invariant BB0 instructions like "mov LC, 100")
    // assign them static registers if they have a dest
    for (const int id : ordered_ids) {
        if (instructions[id].basic_block != BasicBlock::BB0) continue;
        if (new_dest_reg[id] != -1) continue; // already assigned
        const Instruction& instr = instructions[id];
        if (instr.kind == InstructionKind::Mov && instr.destination_register == -1) continue;
        if (instr.kind == InstructionKind::St) continue;
        if (instr.destination_register != -1) {
            new_dest_reg[id] = next_static_reg;
            next_static_reg++;
        }
    }

    // BB2 destinations — assign static regs
    for (const int id : ordered_ids) {
        if (instructions[id].basic_block != BasicBlock::BB2) continue;
        if (new_dest_reg[id] != -1) continue;
        const Instruction& instr = instructions[id];
        if (instr.kind == InstructionKind::St) continue;
        if (instr.kind == InstructionKind::Mov && instr.destination_register == -1) continue;
        if (instr.destination_register != -1) {
            new_dest_reg[id] = next_static_reg;
            next_static_reg++;
        }
    }

    // now resolve BB0 and BB2 source operands
    for (const int id : ordered_ids) {
        const Instruction& instr = instructions[id];
        if (instr.basic_block == BasicBlock::BB1) continue; // already done in Phase 3

        if (id < 0 || id >= static_cast<int>(analysis_table.size()) || instr.source_registers.empty()) {
            if (new_source_regs[id].empty()) new_source_regs[id] = {};
            continue;
        }

        const DependencyAnalysisTableEntry& entry = analysis_table[id];

        // rebuild source regs for this instruction
        new_source_regs[id].clear();

        for (const int src_reg : instr.source_registers) {
            int resolved_reg = -1;

            if (instr.basic_block == BasicBlock::BB0) {
                // BB0 sources: check local deps within BB0
                for (const int dep_id : entry.local_dependencies) {
                    if (dep_id < 0 || dep_id >= instruction_count) continue;
                    if (instructions[dep_id].destination_register != src_reg) continue;
                    if (new_dest_reg[dep_id] != -1) {
                        resolved_reg = new_dest_reg[dep_id];
                        break;
                    }
                }
            }

            if (instr.basic_block == BasicBlock::BB2) {
                // CASE C: post-loop dependency — value from last iteration of BB1
                for (const int dep_id : entry.post_loop_dependencies) {
                    if (dep_id < 0 || dep_id >= instruction_count) continue;
                    if (instructions[dep_id].destination_register != src_reg) continue;
                    if (instructions[dep_id].basic_block == BasicBlock::BB1 && new_dest_reg[dep_id] != -1) {
                        int producer_stage = stage_by_id[dep_id];
                        // iteration_offset = 0, stage_offset = (num_stages - 1) - St(producer)
                        resolved_reg = new_dest_reg[dep_id] + (num_stages - 1) - producer_stage;
                        break;
                    }
                }

                // CASE D: loop invariant read from BB2
                if (resolved_reg == -1) {
                    for (const int dep_id : entry.loop_invariant_dependencies) {
                        if (dep_id < 0 || dep_id >= instruction_count) continue;
                        if (instructions[dep_id].destination_register != src_reg) continue;
                        if (new_dest_reg[dep_id] != -1) {
                            resolved_reg = new_dest_reg[dep_id];
                            break;
                        }
                    }
                }

                // local dep within BB2
                if (resolved_reg == -1) {
                    for (const int dep_id : entry.local_dependencies) {
                        if (dep_id < 0 || dep_id >= instruction_count) continue;
                        if (instructions[dep_id].destination_register != src_reg) continue;
                        if (new_dest_reg[dep_id] != -1) {
                            resolved_reg = new_dest_reg[dep_id];
                            break;
                        }
                    }
                }
            }

            new_source_regs[id].push_back(resolved_reg);
        }
    }

    // CASE E: orphan operands — same as alloc_b Phase 4
    std::map<int, int> orphan_reg_map;
    for (const int id : ordered_ids) {
        const Instruction& instr = instructions[id];
        for (int s = 0; s < static_cast<int>(new_source_regs[id].size()); ++s) {
            if (new_source_regs[id][s] == -1) {
                int orig_reg = instr.source_registers[s];
                if (orphan_reg_map.find(orig_reg) != orphan_reg_map.end()) {
                    new_source_regs[id][s] = orphan_reg_map[orig_reg];
                } else {
                    orphan_reg_map[orig_reg] = next_static_reg;
                    new_source_regs[id][s] = next_static_reg;
                    next_static_reg++;
                }
            }
        }
    }

    // ========================================================================
    // LOOP PREPARATION (Section 3.4)
    // ========================================================================
    // collapse multi-stage loop body into II bundles with predication
    // and insert mov p32,true + mov EC,<num_stages-1> before the loop

    // step 1: collect all BB1 instructions with their cycle and slot info
    // we need to move them from their current multi-stage positions into
    // a single II-bundle block starting at loop_beginning
    // ========================================================================
    // LOOP PREPARATION (Section 3.4)
    // ========================================================================
    // collapse multi-stage loop body into II bundles with predication
    // and insert mov p32,true + mov EC,<num_stages-1> before the loop

    // collect all BB1 instructions with their stage and position info
    struct LoopInstr {
        int id;
        int stage;
        int slot_in_ii; // which bundle within the II window: (cycle - loop_beginning) % ii
        InstructionKind kind;
        std::string text;
    };
    std::vector<LoopInstr> loop_instrs;

    for (const int id : ordered_ids) {
        if (instructions[id].basic_block != BasicBlock::BB1) continue;
        int cycle = scheduled_cycle[id];
        if (cycle < loop_beginning) continue;

        const Instruction& instr = instructions[id];
        int stage = stage_by_id[id];
        int slot_in_ii = (cycle - loop_beginning) % ii;

        std::string text;
        if (instr.kind == InstructionKind::Loop || instr.kind == InstructionKind::LoopPip) {
            // we'll set the real text at placement time when we know new_loop_beginning
            text = "LOOP_PLACEHOLDER";
        } else if (instr.kind == InstructionKind::Mov && instr.destination_register == -1) {
            text = instr.raw_text;
        } else {
            // build register-allocated text then add stage predicate
            text = rebuild_instruction_text(instr, new_dest_reg[id], new_source_regs[id]);
            int pred_reg = 32 + stage;
            text = "(p" + std::to_string(pred_reg) + ") " + text;
        }

        loop_instrs.push_back({id, stage, slot_in_ii, instr.kind, text});
    }

    // figure out how far the current loop body extends so we can clear it
    int max_loop_cycle = loop_beginning;
    for (const int id : ordered_ids) {
        if (instructions[id].basic_block == BasicBlock::BB1 && scheduled_cycle[id] >= loop_beginning) {
            max_loop_cycle = std::max(max_loop_cycle, scheduled_cycle[id]);
        }
    }

    // clear the loop body area
    for (int c = loop_beginning; c <= max_loop_cycle; ++c) {
        if (c < static_cast<int>(schedule.size())) {
            schedule[c] = Bundle();
        }
    }

    // insert mov p32,true and mov EC,<num_stages-1> before the loop
    // Per Section 3.4: "The two movs responsible for the initialization of EC and p32
    // are inserted in the bundle right before the loop.pip instruction.
    // If there are not enough ALU slots in that bundle, a new bundle is created."
    //
    // Strategy: place EC/p32 in the bundle at loop_beginning-1 if it's empty or has room.
    // If the bundle at loop_beginning-1 has a free ALU slot, pack EC there and p32 in the
    // other slot (or a new bundle). The loop body stays at loop_beginning.
    // Only if loop_beginning-1 can't accommodate them, shift the loop body forward.
    std::string ec_text = "mov EC, " + std::to_string(num_stages - 1);
    std::string p32_text = "mov p32, true";

    int new_loop_beginning;

    if (loop_beginning > 0) {
        Bundle& prev = schedule[loop_beginning - 1];
        int free_alu = (prev.ALU0 == "nop" ? 1 : 0) + (prev.ALU1 == "nop" ? 1 : 0);

        if (free_alu >= 2) {
            // Two free ALU slots in the bundle before the loop — put both there
            place_in_bundle(prev, InstructionKind::Mov, ec_text);
            place_in_bundle(prev, InstructionKind::Mov, p32_text);
            new_loop_beginning = loop_beginning;
        } else if (free_alu == 1) {
            // One free slot — pack EC there, p32 needs its own bundle
            place_in_bundle(prev, InstructionKind::Mov, ec_text);
            // shift loop body forward by 1 to make room for p32 bundle
            new_loop_beginning = loop_beginning + 1;
            ensure_bundle_capacity(schedule, new_loop_beginning + ii - 1);
            for (int c = loop_beginning; c < new_loop_beginning + ii; ++c) {
                if (c < static_cast<int>(schedule.size())) schedule[c] = Bundle();
            }
            schedule[loop_beginning].ALU0 = p32_text;
        } else {
            // No free ALU slots — need a new bundle for both, shift loop forward
            new_loop_beginning = loop_beginning + 1;
            ensure_bundle_capacity(schedule, new_loop_beginning + ii - 1);
            for (int c = loop_beginning; c < new_loop_beginning + ii; ++c) {
                if (c < static_cast<int>(schedule.size())) schedule[c] = Bundle();
            }
            schedule[loop_beginning].ALU0 = ec_text;
            schedule[loop_beginning].ALU1 = p32_text;
        }
    } else {
        // no BB0 at all — create a bundle for EC/p32 at cycle 0
        new_loop_beginning = 1;
        ensure_bundle_capacity(schedule, new_loop_beginning + ii - 1);
        for (int c = 0; c < new_loop_beginning + ii; ++c) {
            if (c < static_cast<int>(schedule.size())) schedule[c] = Bundle();
        }
        schedule[0].ALU0 = ec_text;
        schedule[0].ALU1 = p32_text;
    }

    // clear the loop body area (new_loop_beginning .. new_loop_beginning + ii - 1)
    for (int c = new_loop_beginning; c < new_loop_beginning + ii; ++c) {
        ensure_bundle_capacity(schedule, c);
        schedule[c] = Bundle();
    }

    // place collapsed loop instructions into the II bundles
    for (const auto& li : loop_instrs) {
        int target_cycle = new_loop_beginning + li.slot_in_ii;
        ensure_bundle_capacity(schedule, target_cycle);
        Bundle& b = schedule[target_cycle];

        if (li.kind == InstructionKind::Loop || li.kind == InstructionKind::LoopPip) {
            // now we know new_loop_beginning, so we can write the correct text
            b.BRANCH = "loop.pip " + std::to_string(new_loop_beginning);
        } else if (is_alu_kind(li.kind)) {
            if (b.ALU0 == "nop") b.ALU0 = li.text;
            else b.ALU1 = li.text;
        } else if (li.kind == InstructionKind::Mulu) {
            b.MUL = li.text;
        } else if (li.kind == InstructionKind::Ld || li.kind == InstructionKind::St) {
            b.MEM = li.text;
        }

        scheduled_cycle[li.id] = target_cycle;
    }

    // rewrite BB0 instruction texts with new registers
    for (const int id : ordered_ids) {
        const Instruction& instr = instructions[id];
        if (instr.basic_block != BasicBlock::BB0) continue;
        int cycle = scheduled_cycle[id];
        if (cycle < 0) continue;

        if (instr.kind == InstructionKind::Mov && instr.destination_register == -1) continue;

        std::string new_text = rebuild_instruction_text(instr, new_dest_reg[id], new_source_regs[id]);

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

    // trim schedule to BB0 + ec/p32 bundle + II loop bundles
    // BB2 will be added later by schedule_bb2
    schedule.resize(new_loop_beginning + ii);

    // update loop_beginning for the return value
    loop_beginning = new_loop_beginning;

    // update the loop instruction's scheduled_cycle to its final position
    for (int i = 0; i < instruction_count; ++i) {
        if (instructions[i].kind == InstructionKind::Loop || instructions[i].kind == InstructionKind::LoopPip) {
            scheduled_cycle[i] = new_loop_beginning + ii - 1;
        }
    }

    AllocResult result;
    result.new_dest_reg = new_dest_reg;
    result.new_source_regs = new_source_regs;
    result.loop_beginning = new_loop_beginning;
    return result;
}