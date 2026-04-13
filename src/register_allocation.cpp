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

    // we need a fast way to go from instruction id -> its row in the analysis table
    // analysis_index_by_id[5] = 3 means instruction 5's dependency info is at analysis_table[3]
    std::vector<int> analysis_index_by_id(instruction_count, -1);
    for (int i = 0; i < static_cast<int>(analysis_table.size()); ++i) {
        if (analysis_table[i].id >= 0 && analysis_table[i].id < instruction_count) {
            analysis_index_by_id[analysis_table[i].id] = i;
        }
    }

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
    std::vector<int> new_dest_reg(instruction_count, -1);

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
        const int ai = analysis_index_by_id[id];

        // if the instruction has no sources or no analysis entry, nothing to do
        if (ai < 0 || instr.source_registers.empty()) {
            new_source_regs[id] = {};
            continue;
        }

        const DependencyAnalysisTableEntry& entry = analysis_table[ai];

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

                if (instr.basic_block == BasicBlock::BB2) {
                    // BB2 prefers BB1 producers
                    if (instructions[dep_id].basic_block == BasicBlock::BB1) {
                        if (best_producer == -1 || !found_preferred) {
                            best_producer = dep_id;
                            found_preferred = true;
                        }
                    } else if (!found_preferred) {
                        best_producer = dep_id;
                    }
                } else {
                    // BB1 and BB0 prefer BB0 producers, and among BB0 pick the latest scheduled
                    if (instructions[dep_id].basic_block == BasicBlock::BB0) {
                        if (!found_preferred || scheduled_cycle[dep_id] > scheduled_cycle[best_producer]) {
                            best_producer = dep_id;
                            found_preferred = true;
                        }
                    } else if (!found_preferred) {
                        if (best_producer == -1) {
                            best_producer = dep_id;
                        }
                    }
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

        const int ai = analysis_index_by_id[id];
        if (ai < 0) continue;
        const DependencyAnalysisTableEntry& entry = analysis_table[ai];

        // for each source register this BB1 instruction reads...
        for (const int src_reg : instructions[id].source_registers) {
            int bb0_producer = -1;
            int bb1_producer = -1;

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

            search_deps(entry.local_dependencies);
            search_deps(entry.interloop_dependencies);
            search_deps(entry.loop_invariant_dependencies);

            // we only need a mov when BOTH BB0 and BB1 produce the same register
            // if only one produces it, there's no conflict to resolve
            if (bb0_producer != -1 && bb1_producer != -1 &&
                new_dest_reg[bb0_producer] != -1 && new_dest_reg[bb1_producer] != -1) {

                // don't add the same mov pair twice
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

    // now place each mov in the schedule
    // the mov reads from the BB1 producer, so it must respect the producer's latency
    // we try to place it as late as possible (near the loop instruction)
    // if there's no room, we push the loop instruction down
    for (const auto& mp : mov_pairs) {
        // the mov can't execute before the BB1 producer's result is ready
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
            if (new_source_regs[id][s] == -1) {
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

    // same lookup table as alloc_b: instruction id -> analysis table row
    std::vector<int> analysis_index_by_id(instruction_count, -1);
    for (int i = 0; i < static_cast<int>(analysis_table.size()); ++i) {
        if (analysis_table[i].id >= 0 && analysis_table[i].id < instruction_count) {
            analysis_index_by_id[analysis_table[i].id] = i;
        }
    }

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
    // TODO: Unlike alloc_b which assigns x1, x2, x3... to all instructions,
    // alloc_r only assigns registers to BB1 instructions here.
    // BB1 destinations get ROTATING registers starting from x32.
    //
    // The spacing between consecutive rotating registers is (num_stages + 1).
    // This is because a value produced in one iteration might still be alive
    // in the next iteration. With K stages, the RRB register gets incremented
    // K times between when a value is produced and when it might last be read.
    // So we need (num_stages + 1) physical registers per logical register to
    // avoid conflicts. For example with 2 stages: x32, x35, x38, x41...
    // (spacing of 3 = 2 + 1).
    //
    // Walk BB1 instructions in scheduling order (from ordered_ids, filtering
    // for BB1 only). Skip loop/loop.pip, st, and mov to LC/EC just like alloc_b.
    // For each instruction that produces a register, assign the next rotating
    // register. Keep a counter starting at 32 and increment by (num_stages + 1)
    // each time.
    //
    // Store results in new_dest_reg[id] as before.
    // Also store stage_by_id[id] for each instruction — you'll need it in Phase 3.

    int next_rotating_reg = 32;
    int rotating_stride = num_stages + 1;

    // TODO: loop through ordered_ids, assign rotating regs to BB1 destinations


    // ========================================================================
    // PHASE 2: assign static registers to loop invariants
    // ========================================================================
    // TODO: Loop invariant values are produced in BB0 and consumed in BB1
    // but never change during the loop. They don't need rotating registers —
    // a simple static register is enough.
    //
    // Walk through the analysis table and find all BB1 instructions that have
    // loop_invariant_dependencies. For each loop invariant producer (a BB0
    // instruction), assign a static register starting from x1.
    //
    // Be careful not to assign the same BB0 instruction twice if multiple
    // BB1 consumers reference it.
    //
    // Store results in new_dest_reg[id] for the BB0 producer.
    //
    // Note: some BB0 instructions are BOTH loop invariant AND interloop
    // (because dependency_analysis reclassifies them). Those should NOT
    // get a static register here — they'll be handled in Phase 4 as
    // interloop initializers. You can check: if a BB0 instruction appears
    // in any BB1 instruction's interloop_dependencies, skip it here.

    int next_static_reg = 1;

    // TODO: loop through BB1 instructions, find loop invariant producers, assign static regs


    // ========================================================================
    // PHASE 3: link source operands using stage/iteration offsets
    // ========================================================================
    // TODO: This is the biggest difference from alloc_b. Instead of just
    // looking up new_dest_reg[producer], we need to apply register rotation
    // offsets based on equations 3 and 4 from the PDF.
    //
    // For each BB1 instruction's source operand, find its producer and then:
    //
    // A) If the producer is a LOOP INVARIANT (BB0, static register):
    //    Just use new_dest_reg[producer] directly. No offset needed because
    //    static registers don't rotate.
    //
    // B) If the producer is a LOCAL dependency (same BB1, same iteration):
    //    Apply equation 3: consumed_reg = produced_reg + (St(consumer) - St(producer))
    //    where St(X) is the stage of instruction X.
    //    The stage difference accounts for RRB increments between producer and consumer.
    //    Since the producer is always before the consumer in the same iteration,
    //    the difference is always >= 0.
    //
    // C) If the producer is an INTERLOOP dependency (BB1, previous iteration):
    //    Apply equation 4: consumed_reg = produced_reg + (St(consumer) - St(producer)) + 1
    //    The +1 accounts for the extra RRB increment when going from one iteration
    //    to the next.
    //
    //    If the interloop producer is in BB0 (an initializer that was reclassified),
    //    this is handled in Phase 4, not here.
    //
    // For BB0 and BB2 instructions, handle them the same way as alloc_b for now
    // (they'll be finalized in Phase 4).
    //
    // Walk ordered_ids, and for each instruction's source registers:
    // 1. Look up the dependency table to find the producer
    // 2. Determine which category (A, B, or C) it falls into
    // 3. Compute the final register number with the appropriate offset
    // 4. Store in new_source_regs[id]

    // TODO: implement the operand linking with rotation offsets


    // ========================================================================
    // PHASE 4: allocate BB0 and BB2 registers
    // ========================================================================
    // TODO: Handle the remaining register assignments for BB0 and BB2.
    // The PDF (Section 3.3.2, fourth phase) lists these cases:
    //
    // CASE A — BB0 interloop initializer:
    //   If a BB0 instruction writes to a register that is an interloop
    //   dependent operand of some BB1 instruction C, and a BB1 instruction P
    //   also produces that operand within the loop body, then:
    //   - The BB0 instruction's destination = same base register as P's destination
    //   - iteration_offset = +1
    //   - stage_offset = -St(P)
    //   - So the actual register = dest_of_P + 1 + (-St(P)) = dest_of_P + 1 - St(P)
    //   This makes the BB0 value land in exactly the right rotated position
    //   so that BB1 consumers read it correctly on the first iteration.
    //
    // CASE B — BB0/BB2 local dependency:
    //   If a BB0 or BB2 instruction has a local dependency within BB0 or BB2,
    //   assign registers the same way as alloc_b (fresh unique static registers).
    //   But only if the destination wasn't already assigned in Phase 1 or 2.
    //
    // CASE C — BB2 post-loop dependency:
    //   If a BB2 instruction consumes a register produced in BB1, the value
    //   comes from the last iteration. The iteration offset is 0 (no new
    //   iteration started), and the stage offset is the distance from the
    //   producer's stage to the last stage: (num_stages - 1) - St(producer).
    //   So: consumed_reg = produced_reg + 0 + ((num_stages - 1) - St(producer))
    //     = produced_reg + num_stages - 1 - St(producer)
    //
    // CASE D — BB0/BB2 reads a loop invariant:
    //   Just use the static register assigned in Phase 2.
    //
    // CASE E — Orphan operands (no producer):
    //   Same as alloc_b Phase 4: assign a fresh unused static register.
    //   If two instructions read the same orphan, give them the same register.

    // TODO: implement BB0/BB2 register allocation


    // ========================================================================
    // BUNDLE REWRITE: same as alloc_b
    // ========================================================================
    // rewrite BB0 and BB1 bundle slots with new register names
    // skip BB2 (handled by rewrite_bb2_bundles after schedule_bb2)
    for (const int id : ordered_ids) {
        const Instruction& instr = instructions[id];
        if (instr.basic_block == BasicBlock::BB2) continue;

        int cycle = scheduled_cycle[id];
        if (cycle < 0) continue;

        if (instr.kind == InstructionKind::Loop || instr.kind == InstructionKind::LoopPip) {
            schedule[cycle].BRANCH = instr.opcode + " " + std::to_string(loop_beginning);
            continue;
        }

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

    // ========================================================================
    // LOOP PREPARATION (Section 3.4): collapse stages + add predication
    // ========================================================================
    // TODO: After register allocation, the schedule has multiple stages
    // (each II bundles long). We need to collapse them into a single block
    // of II bundles where instructions from different stages coexist.
    //
    // Each instruction gets a predicate based on its stage:
    //   stage 0 -> (p32), stage 1 -> (p33), stage 2 -> (p34), etc.
    //
    // The loop.pip instruction itself is NOT predicated.
    //
    // Before the loop body, insert two mov instructions:
    //   "mov p32, true"  — enables the first stage predicate
    //   "mov EC, <num_stages - 1>"  — sets the epilogue count
    // These go in the bundle right before the loop body starts.
    // If there's no room in that bundle, create a new one.
    //
    // The collapse works by taking each instruction at cycle C in stage S
    // and moving it to cycle (loop_beginning + (C - loop_beginning) % ii).
    // The instruction text gets prefixed with its predicate: "(p32) add ..."
    //
    // After collapsing, the schedule should have:
    //   BB0 bundles (unchanged)
    //   mov p32, true + mov EC, <num_stages-1> bundle
    //   II bundles of the collapsed loop body (with loop.pip in last bundle)
    //   BB2 bundles (added later by schedule_bb2)

    // TODO: implement loop preparation


    AllocResult result;
    result.new_dest_reg = new_dest_reg;
    result.new_source_regs = new_source_regs;
    result.loop_beginning = loop_beginning;
    return result;
}