// for this hw we always have the assumption of having enough registers

#include "register_allocation.hpp"
#include "data_structures.hpp"
#include <algorithm>
#include <map>
#include <string>

namespace {

// helper functions shared by alloc_b and alloc_r

// returns true if this instruction should get a fresh destination register
// excludes loop/loop.pip, stores, and movs to LC/EC
bool instr_needs_dest_register(const Instruction& instr) {
    if (instr.kind == InstructionKind::Loop || instr.kind == InstructionKind::LoopPip) return false;
    if (instr.kind == InstructionKind::St) return false;
    if (instr.kind == InstructionKind::Mov && instr.destination_register == -1) return false;
    return instr.destination_register != -1;
}

// slot priority used when two instructions land in the same cycle
// matches the bundle layout: ALU0, ALU1, MUL, MEM, BRANCH
int slot_priority_of(InstructionKind kind) {
    switch (kind) {
        case InstructionKind::Add:
        case InstructionKind::Addi:
        case InstructionKind::Sub:
        case InstructionKind::Mov:    return 0;
        case InstructionKind::Mulu:   return 2;
        case InstructionKind::Ld:
        case InstructionKind::St:     return 3;
        case InstructionKind::Loop:
        case InstructionKind::LoopPip: return 4;
        default:                       return 5;
    }
}

// build instruction ids in schedule order: cycle, slot, then program order
// BB2 instructions are still unscheduled here, so we append them later in program order
std::vector<int> build_ordered_ids(const std::vector<Instruction>& instructions,
                                   const std::vector<int>& scheduled_cycle) {
    const int n = static_cast<int>(instructions.size());
    std::vector<int> ordered_ids;
    for (int i = 0; i < n; ++i) {
        if (scheduled_cycle[i] >= 0) ordered_ids.push_back(i);
    }
    std::sort(ordered_ids.begin(), ordered_ids.end(), [&](int a, int b) {
        if (scheduled_cycle[a] != scheduled_cycle[b]) return scheduled_cycle[a] < scheduled_cycle[b];
        int pa = slot_priority_of(instructions[a].kind);
        int pb = slot_priority_of(instructions[b].kind);
        if (pa != pb) return pa < pb;
        return a < b;
    });
    for (int i = 0; i < n; ++i) {
        if (instructions[i].basic_block == BasicBlock::BB2 && scheduled_cycle[i] < 0) {
            ordered_ids.push_back(i);
        }
    }
    return ordered_ids;
}

// build a lookup table from instruction id to analysis table row
std::vector<int> build_analysis_index_by_id(
    const std::vector<DependencyAnalysisTableEntry>& analysis_table,
    int instruction_count) {
    std::vector<int> out(instruction_count, -1);
    for (int i = 0; i < static_cast<int>(analysis_table.size()); ++i) {
        if (analysis_table[i].id >= 0 && analysis_table[i].id < instruction_count) {
            out[analysis_table[i].id] = i;
        }
    }
    return out;
}

// search a dependency list for a producer that writes target_reg in target_bb.
// returns the producer id, or -1 if none is found.
int find_producer_in(const std::vector<int>& deps,
                     int target_reg,
                     BasicBlock target_bb,
                     const std::vector<Instruction>& instructions) {
    const int n = static_cast<int>(instructions.size());
    for (const int dep_id : deps) {
        if (dep_id < 0 || dep_id >= n) continue;
        if (instructions[dep_id].destination_register != target_reg) continue;
        if (instructions[dep_id].basic_block != target_bb) continue;
        return dep_id;
    }
    return -1;
}

// search a dependency list for a producer writing target_reg in any BB.
int find_producer_any_bb(const std::vector<int>& deps,
                         int target_reg,
                         const std::vector<Instruction>& instructions) {
    const int n = static_cast<int>(instructions.size());
    for (const int dep_id : deps) {
        if (dep_id < 0 || dep_id >= n) continue;
        if (instructions[dep_id].destination_register == target_reg) return dep_id;
    }
    return -1;
}

// overwrite the bundle slot that still contains instr.raw_text with new_text
// ALU instructions can go in either ALU0 or ALU1, so we match by text
// MUL, MEM, and BRANCH each have a single slot in the bundle.
// if the slot is already rewritten or not present, do nothing
void rewrite_bundle_slot(Bundle& bundle, const Instruction& instr, const std::string& new_text) {
    if (is_alu_kind(instr.kind)) {
        if (bundle.ALU0 == instr.raw_text) bundle.ALU0 = new_text;
        else if (bundle.ALU1 == instr.raw_text) bundle.ALU1 = new_text;
    } else if (instr.kind == InstructionKind::Mulu) {
        bundle.MUL = new_text;
    } else if (instr.kind == InstructionKind::Ld || instr.kind == InstructionKind::St) {
        bundle.MEM = new_text;
    }
}

//  rewrite every placed instruction using the new registers
// BB2 instructions are skipped here and handled later by rewrite_bb2_bundles
// the loop instruction gets its branch target updated to loop_beginning
// mov to LC/EC keeps its original text
void apply_renaming_to_bundles(std::vector<Bundle>& schedule,
                               const std::vector<int>& ordered_ids,
                               const std::vector<Instruction>& instructions,
                               const std::vector<int>& scheduled_cycle,
                               const std::vector<int>& new_dest_reg,
                               const std::vector<std::vector<int>>& new_source_regs,
                               int loop_beginning,
                               bool skip_bb2) {
    for (const int id : ordered_ids) {
        const Instruction& instr = instructions[id];
        if (skip_bb2 && instr.basic_block == BasicBlock::BB2) continue;

        const int cycle = scheduled_cycle[id];
        if (cycle < 0) continue;

        // loop instruction are retarget to the start of the scheduled loop body.
        if (instr.kind == InstructionKind::Loop || instr.kind == InstructionKind::LoopPip) {
            schedule[cycle].BRANCH = instr.opcode + " " + std::to_string(loop_beginning);
            continue;
        }

        // mov to LC/EC keep the raw text.
        if (instr.kind == InstructionKind::Mov && instr.destination_register == -1) continue;

        const std::string new_text = rebuild_instruction_text(
            instr, new_dest_reg[id], new_source_regs[id]);
        rewrite_bundle_slot(schedule[cycle], instr, new_text);
    }
}

// resolve source operands that are still -1 after the main allocation phases
void resolve_orphan_sources(const std::vector<int>& ordered_ids,
                            const std::vector<Instruction>& instructions,
                            std::vector<std::vector<int>>& new_source_regs,
                            int& next_reg) {
    std::map<int, int> orphan_map;
    for (const int id : ordered_ids) {
        const Instruction& instr = instructions[id];
        for (size_t s = 0; s < new_source_regs[id].size(); ++s) {
            if (new_source_regs[id][s] != -1) continue;
            const int orig = instr.source_registers[s];
            auto it = orphan_map.find(orig);
            if (it != orphan_map.end()) {
                new_source_regs[id][s] = it->second;
            } else {
                orphan_map[orig] = next_reg;
                new_source_regs[id][s] = next_reg++;
            }
        }
    }
}

// in phase 1 we give BB1 destinations rotating registers
void r_phase1_rotating_dests(const std::vector<int>& ordered_ids,
                             const std::vector<Instruction>& instructions,
                             int num_stages,
                             std::vector<int>& new_dest_reg) {
    const int stride = num_stages + 1;
    int next_reg = 32;
    for (const int id : ordered_ids) {
        if (instructions[id].basic_block != BasicBlock::BB1) continue;
        if (!instr_needs_dest_register(instructions[id])) continue;
        new_dest_reg[id] = next_reg;
        next_reg += stride;
    }
}

// identify BB0 instructions that act as interloop initializers
// so if a BB1 consumer uses it and BB1 writes the same register again
std::vector<bool> r_find_interloop_initializers(
    const std::vector<int>& ordered_ids,
    const std::vector<Instruction>& instructions,
    const std::vector<DependencyAnalysisTableEntry>& analysis_table,
    const std::vector<int>& analysis_index_by_id) {

    const int n = static_cast<int>(instructions.size());
    std::vector<bool> is_initializer(n, false);

    for (const int id : ordered_ids) {
        if (instructions[id].basic_block != BasicBlock::BB1) continue;
        const int ai = analysis_index_by_id[id];
        if (ai < 0) continue;

        std::map<int, int> latest_bb0_per_reg; // dest reg goes to latest BB0 id
        for (const int dep_id : analysis_table[ai].interloop_dependencies) {
            if (dep_id < 0 || dep_id >= n) continue;
            if (instructions[dep_id].basic_block != BasicBlock::BB0) continue;
            const int dreg = instructions[dep_id].destination_register;
            if (dreg < 0) continue;
            auto it = latest_bb0_per_reg.find(dreg);
            if (it == latest_bb0_per_reg.end() || dep_id > it->second) {
                latest_bb0_per_reg[dreg] = dep_id;
            }
        }
        for (const auto& kv : latest_bb0_per_reg) is_initializer[kv.second] = true;
    }
    return is_initializer;
}

// in phase 2 we assign static registers to loop-invariant BB0 producers
// BB0 instructions that are interloop initializers are skipped here and handled later
void r_phase2_invariant_dests(const std::vector<int>& ordered_ids,
                              const std::vector<Instruction>& instructions,
                              const std::vector<DependencyAnalysisTableEntry>& analysis_table,
                              const std::vector<int>& analysis_index_by_id,
                              const std::vector<bool>& is_interloop_initializer,
                              std::vector<int>& new_dest_reg,
                              int& next_static_reg) {
    const int n = static_cast<int>(instructions.size());
    std::vector<bool> assigned(n, false);
    for (const int id : ordered_ids) {
        if (instructions[id].basic_block != BasicBlock::BB1) continue;
        const int ai = analysis_index_by_id[id];
        if (ai < 0) continue;
        for (const int dep_id : analysis_table[ai].loop_invariant_dependencies) {
            if (dep_id < 0 || dep_id >= n) continue;
            if (instructions[dep_id].basic_block != BasicBlock::BB0) continue;
            if (is_interloop_initializer[dep_id]) continue;
            if (assigned[dep_id]) continue;
            new_dest_reg[dep_id] = next_static_reg++;
            assigned[dep_id] = true;
        }
    }
}

// resolve one BB1 source operand against the dependency lists
// returns the new register name, or -1 if no producer is known yet
int r_resolve_bb1_source(int src_reg,
                         int consumer_stage,
                         const DependencyAnalysisTableEntry& entry,
                         const std::vector<Instruction>& instructions,
                         const std::vector<int>& new_dest_reg,
                         const std::vector<int>& stage_by_id) {

    // first try loop-invariant deps, in that case we use the static register as is
    int p = find_producer_any_bb(entry.loop_invariant_dependencies, src_reg, instructions);
    if (p != -1 && new_dest_reg[p] != -1) return new_dest_reg[p];

    // then check local deps in the same iteration
    p = find_producer_any_bb(entry.local_dependencies, src_reg, instructions);
    if (p != -1 && new_dest_reg[p] != -1) {
        return new_dest_reg[p] + (consumer_stage - stage_by_id[p]);
    }

    // if it is interloop and the producer is in BB1, add the +1 iteration offset
    p = find_producer_in(entry.interloop_dependencies, src_reg, BasicBlock::BB1, instructions);
    if (p != -1 && new_dest_reg[p] != -1) {
        return new_dest_reg[p] + (consumer_stage - stage_by_id[p]) + 1;
    }

    // if interloop points to a BB0 initializer, use the matching BB1 producer name
    // because phase 4 aligns the BB0 initializer with that BB1 producer
    int bb0 = find_producer_in(entry.interloop_dependencies, src_reg, BasicBlock::BB0, instructions);
    if (bb0 != -1) {
        p = find_producer_in(entry.interloop_dependencies, src_reg, BasicBlock::BB1, instructions);
        if (p != -1 && new_dest_reg[p] != -1) {
            return new_dest_reg[p] + (consumer_stage - stage_by_id[p]) + 1;
        }
    }

    return -1;
}

// in phase 3 we resolve source operands of BB1 instructions
// BB0/BB2 operands stay as placeholders (-1) for phase 4
void r_phase3_bb1_sources(const std::vector<int>& ordered_ids,
                          const std::vector<Instruction>& instructions,
                          const std::vector<DependencyAnalysisTableEntry>& analysis_table,
                          const std::vector<int>& analysis_index_by_id,
                          const std::vector<int>& new_dest_reg,
                          const std::vector<int>& stage_by_id,
                          std::vector<std::vector<int>>& new_source_regs) {
    for (const int id : ordered_ids) {
        const Instruction& instr = instructions[id];
        const int ai = analysis_index_by_id[id];
        if (ai < 0 || instr.source_registers.empty()) {
            new_source_regs[id] = {};
            continue;
        }
        if (instr.basic_block != BasicBlock::BB1) {
            new_source_regs[id].assign(instr.source_registers.size(), -1);
            continue;
        }
        const DependencyAnalysisTableEntry& entry = analysis_table[ai];
        const int consumer_stage = stage_by_id[id];
        for (const int src_reg : instr.source_registers) {
            new_source_regs[id].push_back(
                r_resolve_bb1_source(src_reg, consumer_stage, entry,
                                     instructions, new_dest_reg, stage_by_id));
        }
    }
}

// for a given BB0 initializer, find the matching BB1 producer of the same register
// returns the BB1 producer id, or -1 if none exists
int r_find_bb1_producer_for_initializer(int bb0_id,
                                        const std::vector<int>& ordered_ids,
                                        const std::vector<Instruction>& instructions,
                                        const std::vector<DependencyAnalysisTableEntry>& analysis_table,
                                        const std::vector<int>& analysis_index_by_id,
                                        const std::vector<int>& new_dest_reg) {
    const int n = static_cast<int>(instructions.size());
    const int orig_dest = instructions[bb0_id].destination_register;
    if (orig_dest == -1) return -1;

    for (const int consumer_id : ordered_ids) {
        if (instructions[consumer_id].basic_block != BasicBlock::BB1) continue;
        const int ai = analysis_index_by_id[consumer_id];
        if (ai < 0) continue;
        const DependencyAnalysisTableEntry& entry = analysis_table[ai];

        // only check consumers that actually list this BB0 instruction as interloop
        if (std::find(entry.interloop_dependencies.begin(),
                      entry.interloop_dependencies.end(), bb0_id) == entry.interloop_dependencies.end()) {
            continue;
        }

        for (const int other_dep : entry.interloop_dependencies) {
            if (other_dep < 0 || other_dep >= n) continue;
            if (instructions[other_dep].basic_block != BasicBlock::BB1) continue;
            if (instructions[other_dep].destination_register != orig_dest) continue;
            if (new_dest_reg[other_dep] == -1) continue;
            return other_dep;
        }
    }
    return -1;
}

// in phase 4a we assign destination registers for BB0 interloop initializers
// their name must line up with the rotating BB1 producer
void r_phase4_bb0_initializer_dests(const std::vector<int>& ordered_ids,
                                    const std::vector<Instruction>& instructions,
                                    const std::vector<DependencyAnalysisTableEntry>& analysis_table,
                                    const std::vector<int>& analysis_index_by_id,
                                    const std::vector<bool>& is_interloop_initializer,
                                    const std::vector<int>& stage_by_id,
                                    std::vector<int>& new_dest_reg) {
    const int n = static_cast<int>(instructions.size());
    for (int id = 0; id < n; ++id) {
        if (instructions[id].basic_block != BasicBlock::BB0) continue;
        if (!is_interloop_initializer[id]) continue;
        if (new_dest_reg[id] != -1) continue;

        int bb1_producer = r_find_bb1_producer_for_initializer(
            id, ordered_ids, instructions, analysis_table, analysis_index_by_id, new_dest_reg);
        if (bb1_producer != -1) {
            // dest = BB1_producer_reg + 1 - St(BB1_producer)
            new_dest_reg[id] = new_dest_reg[bb1_producer] + 1 - stage_by_id[bb1_producer];
        }
    }
}

// in phase 4b we assign static regs to the remaining BB0 and BB2 destinations.
void r_phase4_static_dests(const std::vector<int>& ordered_ids,
                           const std::vector<Instruction>& instructions,
                           std::vector<int>& new_dest_reg,
                           int& next_static_reg) {
    for (const int id : ordered_ids) {
        const BasicBlock bb = instructions[id].basic_block;
        if (bb != BasicBlock::BB0 && bb != BasicBlock::BB2) continue;
        if (new_dest_reg[id] != -1) continue;
        if (!instr_needs_dest_register(instructions[id])) continue;
        new_dest_reg[id] = next_static_reg++;
    }
}

// in phase 4c we resolve source operands of BB0 and BB2 instructions
void r_phase4_bb0_bb2_sources(const std::vector<int>& ordered_ids,
                              const std::vector<Instruction>& instructions,
                              const std::vector<DependencyAnalysisTableEntry>& analysis_table,
                              const std::vector<int>& analysis_index_by_id,
                              const std::vector<int>& new_dest_reg,
                              const std::vector<int>& stage_by_id,
                              int num_stages,
                              std::vector<std::vector<int>>& new_source_regs) {

    for (const int id : ordered_ids) {
        const Instruction& instr = instructions[id];
        if (instr.basic_block == BasicBlock::BB1) continue;

        const int ai = analysis_index_by_id[id];
        if (ai < 0 || instr.source_registers.empty()) continue;
        const DependencyAnalysisTableEntry& entry = analysis_table[ai];

        new_source_regs[id].clear();
        for (const int src_reg : instr.source_registers) {
            int resolved = -1;

            if (instr.basic_block == BasicBlock::BB0) {
                // BB0: only local deps within BB0 matter.
                int p = find_producer_any_bb(entry.local_dependencies, src_reg, instructions);
                if (p != -1 && new_dest_reg[p] != -1) resolved = new_dest_reg[p];
            } else {
                // BB2: try post-loop, then loop-invariant, then local.
                int p = find_producer_in(entry.post_loop_dependencies, src_reg, BasicBlock::BB1, instructions);
                if (p != -1 && new_dest_reg[p] != -1) {
                    // iter offset 0, stage offset (num_stages - 1) - St(producer)
                    resolved = new_dest_reg[p] + (num_stages - 1) - stage_by_id[p];
                }
                if (resolved == -1) {
                    p = find_producer_any_bb(entry.loop_invariant_dependencies, src_reg, instructions);
                    if (p != -1 && new_dest_reg[p] != -1) resolved = new_dest_reg[p];
                }
                if (resolved == -1) {
                    p = find_producer_any_bb(entry.local_dependencies, src_reg, instructions);
                    if (p != -1 && new_dest_reg[p] != -1) resolved = new_dest_reg[p];
                }
            }

            new_source_regs[id].push_back(resolved);
        }
    }
}

} 

// find the producer for each source register and wire the source
// to the producer's new register
// when a register has two producers, BB1 consumers prefer
// the BB0 producer (keeping the latest one if multiple exist), while BB2 consumers
// prefer the BB1 producer
void b_phase2_resolve_sources(const std::vector<int>& ordered_ids,
                              const std::vector<Instruction>& instructions,
                              const std::vector<DependencyAnalysisTableEntry>& analysis_table,
                              const std::vector<int>& analysis_index_by_id,
                              const std::vector<int>& scheduled_cycle,
                              const std::vector<int>& new_dest_reg,
                              std::vector<std::vector<int>>& new_source_regs) {

    const int n = static_cast<int>(instructions.size());

    for (const int id : ordered_ids) {
        const Instruction& instr = instructions[id];
        const int ai = analysis_index_by_id[id];
        if (ai < 0 || instr.source_registers.empty()) {
            new_source_regs[id] = {};
            continue;
        }

        // collect all deps into one flat list 
        const DependencyAnalysisTableEntry& entry = analysis_table[ai];
        std::vector<int> all_deps;
        all_deps.insert(all_deps.end(), entry.local_dependencies.begin(),          entry.local_dependencies.end());
        all_deps.insert(all_deps.end(), entry.interloop_dependencies.begin(),      entry.interloop_dependencies.end());
        all_deps.insert(all_deps.end(), entry.loop_invariant_dependencies.begin(), entry.loop_invariant_dependencies.end());
        all_deps.insert(all_deps.end(), entry.post_loop_dependencies.begin(),      entry.post_loop_dependencies.end());

        // preferred means the basic block we try first when searching a producer
        // BB2 instructions should read from BB1 first
        // BB0 and BB1 instructions should read from BB0 first
        const BasicBlock preferred = (instr.basic_block == BasicBlock::BB2)
                                     ? BasicBlock::BB1 : BasicBlock::BB0;
        // if we do not find preferred, use fallback
        // BB1/BB0 keep first fallback, BB2 keeps last fallback
        const bool fallback_keeps_first = (instr.basic_block != BasicBlock::BB2);

        for (const int src_reg : instr.source_registers) {
            int best = -1;
            bool best_is_preferred = false;

            for (const int dep_id : all_deps) {
                if (dep_id < 0 || dep_id >= n) continue;
                if (instructions[dep_id].destination_register != src_reg) continue;

                const BasicBlock dep_bb = instructions[dep_id].basic_block;

                if (dep_bb == preferred) {
                    // preferred producer wins
                    // BB1 keeps latest preferred, BB2 keeps first preferred
                    if (!best_is_preferred) {
                        best = dep_id;
                        best_is_preferred = true;
                    } else if (preferred == BasicBlock::BB0 &&
                               scheduled_cycle[dep_id] > scheduled_cycle[best]) {
                        best = dep_id;
                    }
                } else if (!best_is_preferred) {
                    if (fallback_keeps_first) {
                        if (best == -1) best = dep_id;
                    } else {
                        best = dep_id; // BB2: last non-preferred wins
                    }
                }
            }

            new_source_regs[id].push_back(
                (best != -1 && new_dest_reg[best] != -1) ? new_dest_reg[best] : -1);
        }
    }
}

AllocResult alloc_b(std::vector<Bundle>& schedule,
                    const std::vector<DependencyAnalysisTableEntry>& analysis_table,
                    const std::vector<Instruction>& instructions,
                    std::vector<int>& scheduled_cycle) { // not const because phase 3 may push the loop down and update its cycle

    const int instruction_count = static_cast<int>(instructions.size());

    // map instruction id to row in analysis table
    std::vector<int> analysis_index_by_id(instruction_count, -1);
    for (int i = 0; i < static_cast<int>(analysis_table.size()); ++i) {
        if (analysis_table[i].id >= 0 && analysis_table[i].id < instruction_count) {
            analysis_index_by_id[analysis_table[i].id] = i;
        }
    }

    // when two instructions share a cycle,
    // slot order is ALU0, ALU1, MUL, MEM, BRANCH
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

    // collect ids that already have a cycle
    std::vector<int> ordered_ids;
    for (int i = 0; i < instruction_count; ++i) {
        if (scheduled_cycle[i] >= 0) {
            ordered_ids.push_back(i);
        }
    }

    // sort by cycle, slot, then program order
    std::sort(ordered_ids.begin(), ordered_ids.end(), [&](int a, int b) {
        if (scheduled_cycle[a] != scheduled_cycle[b])
            return scheduled_cycle[a] < scheduled_cycle[b];
        int pa = slot_priority(instructions[a].kind);
        int pb = slot_priority(instructions[b].kind);
        if (pa != pb) return pa < pb;
        return a < b;
    });

    // BB2 gets registers now even if scheduled later
    // append BB2 ids at the end in program order
    for (int i = 0; i < instruction_count; ++i) {
        if (instructions[i].basic_block == BasicBlock::BB2 && scheduled_cycle[i] < 0) {
            ordered_ids.push_back(i);
        }
    }

    // we assign a fresh unique register to each instruction destination

    // walk instructions in schedule order and assign x1, x2, x3
    // this removes anti and output deps
    int next_reg = 1;
    std::vector<int> new_dest_reg(instruction_count, -1);

    for (const int id : ordered_ids) {
        const Instruction& instr = instructions[id];

        // loop/loop.pip do not write a general-purpose register
        if (instr.kind == InstructionKind::Loop || instr.kind == InstructionKind::LoopPip) continue;

        // mov to LC/EC writes special regs
        // destination_register is -1 for LC/EC
        if (instr.kind == InstructionKind::Mov && instr.destination_register == -1) continue;

        // store instructions do not produce a register value
        if (instr.kind == InstructionKind::St) continue;

        // everything else gets a fresh register
        if (instr.destination_register != -1) {
            new_dest_reg[id] = next_reg;
            next_reg++;
        }
    }

  
    // we link each source operand to the new register of its producer

    std::vector<std::vector<int>> new_source_regs(instruction_count);
    b_phase2_resolve_sources(ordered_ids, instructions, analysis_table,
                             analysis_index_by_id, scheduled_cycle,
                             new_dest_reg, new_source_regs);

    // we insert mov instructions for interloop dependencies
 

    // BB1 reads from BB0 and this works for the first loop iteration
    // after that, values should come from BB1 of the previous iteration
    // so we add mov instructions to copy BB1 results back into BB0 registers
    // first find the loop instruction and loop body range
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

    // collect all (BB0 register, BB1 register) pairs that need a mov
    struct MovPair {
        int dest_reg;    // BB0 producer's new register
        int src_reg;     // BB1 producer's new register
        int bb1_prod_id; // BB1 producer id, used for latency timing
    };
    std::vector<MovPair> mov_pairs;

    for (const int id : ordered_ids) {
        // only BB1 instructions can have interloop dependencies
        if (instructions[id].basic_block != BasicBlock::BB1) continue;

        const int ai = analysis_index_by_id[id];
        if (ai < 0) continue;
        const DependencyAnalysisTableEntry& entry = analysis_table[ai];

        // check each source register of this BB1 instruction
        for (const int src_reg : instructions[id].source_registers) {
            int bb0_producer = -1;
            int bb1_producer = -1;

            auto search_deps = [&](const std::vector<int>& deps) {
                for (const int dep_id : deps) {
                    if (dep_id < 0 || dep_id >= instruction_count) continue;
                    if (instructions[dep_id].destination_register != src_reg) continue;
                    if (instructions[dep_id].basic_block == BasicBlock::BB0) {
                        // if multiple BB0 instructions write the same register
                        // if many BB0 writers, keep latest one
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

            // need mov only when BB0 and BB1 both produce same register
            // if only one producer exists, no conflict
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

    // place each mov in the schedule mov depends on BB1 producer latency try to place it late near loop if no room, push loop down
    for (const auto& mp : mov_pairs) {
        // the mov can't execute before the BB1 producer's result is ready
        int earliest_mov = scheduled_cycle[mp.bb1_prod_id] + instruction_latency(instructions[mp.bb1_prod_id].kind);

        while (true) {
            // if the loop instruction is before the earliest possible mov cycle,
            // push loop down to make room
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

            // search backward for a free ALU slot keep mov as late as possible
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

            // no free ALU slot in loop body push loop down by one and retry
            schedule[loop_cycle].BRANCH = "nop";
            ++loop_cycle;
            ensure_bundle_capacity(schedule, loop_cycle);
            while (!can_place_in_bundle(schedule[loop_cycle], InstructionKind::Loop)) {
                ++loop_cycle;
                ensure_bundle_capacity(schedule, loop_cycle);
            }
            schedule[loop_cycle].BRANCH = instructions[loop_id].raw_text;
            scheduled_cycle[loop_id] = loop_cycle;
            // try again with extra room
        }
    }


    // we handle orphan operands (sources with no known producer)
    resolve_orphan_sources(ordered_ids, instructions, new_source_regs, next_reg);

    // we replace original instruction text with new register names
  
    // BB2 instructions aren't in the schedule yet, rewrite_bb2_bundles handles them after schedule_bb2 places them
    apply_renaming_to_bundles(schedule, ordered_ids, instructions, scheduled_cycle,
                              new_dest_reg, new_source_regs, loop_beginning, true);

    // return values used later to rewrite BB2
    AllocResult result;
    result.new_dest_reg = new_dest_reg;
    result.new_source_regs = new_source_regs;
    result.loop_beginning = loop_beginning;
    return result;
}

// called after schedule_bb2 places BB2 instructions rewrites BB2 bundle slots with new names from alloc_b
void rewrite_bb2_bundles(std::vector<Bundle>& schedule,
                         const std::vector<Instruction>& instructions,
                         const std::vector<int>& scheduled_cycle,
                         const AllocResult& alloc) {
    for (int id = 0; id < static_cast<int>(instructions.size()); ++id) {
        const Instruction& instr = instructions[id];
        if (instr.basic_block != BasicBlock::BB2) continue;
        const int cycle = scheduled_cycle[id];
        if (cycle < 0) continue;

        const std::string new_text = rebuild_instruction_text(
            instr, alloc.new_dest_reg[id], alloc.new_source_regs[id]);
        rewrite_bundle_slot(schedule[cycle], instr, new_text);
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


    // register allocation for loop.pip 

    const int instruction_count = static_cast<int>(instructions.size());
    const std::vector<int> analysis_index_by_id =
        build_analysis_index_by_id(analysis_table, instruction_count);
    const std::vector<int> ordered_ids = build_ordered_ids(instructions, scheduled_cycle);

    std::vector<int> new_dest_reg(instruction_count, -1);
    std::vector<std::vector<int>> new_source_regs(instruction_count);
    int next_static_reg = 1;

    // rotating regs for BB1 destinations
    r_phase1_rotating_dests(ordered_ids, instructions, num_stages, new_dest_reg);

    // find BB0 interloop initializers 
    const std::vector<bool> is_interloop_initializer =
        r_find_interloop_initializers(ordered_ids, instructions, analysis_table, analysis_index_by_id);

    // static regs for loop-invariant BB0 producers
    r_phase2_invariant_dests(ordered_ids, instructions, analysis_table, analysis_index_by_id,
                             is_interloop_initializer, new_dest_reg, next_static_reg);

    // resolve BB1 sources
    r_phase3_bb1_sources(ordered_ids, instructions, analysis_table, analysis_index_by_id,
                         new_dest_reg, stage_by_id, new_source_regs);

    // BB0 initializer destination must match BB1 producer
    r_phase4_bb0_initializer_dests(ordered_ids, instructions, analysis_table, analysis_index_by_id,
                                   is_interloop_initializer, stage_by_id, new_dest_reg);

    // static regs for remaining BB0 and BB2 destinations
    r_phase4_static_dests(ordered_ids, instructions, new_dest_reg, next_static_reg);

    // resolve BB0 and BB2 sources
    r_phase4_bb0_bb2_sources(ordered_ids, instructions, analysis_table, analysis_index_by_id,
                             new_dest_reg, stage_by_id, num_stages, new_source_regs);

    // orphan sources get fresh static regs
    resolve_orphan_sources(ordered_ids, instructions, new_source_regs, next_static_reg);


    // pack the multi-stage loop body into II bundles add mov p32,true and mov EC,

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
            // set branch text later when new_loop_beginning is known
            text = "LOOP_PLACEHOLDER";
        } else if (instr.kind == InstructionKind::Mov && instr.destination_register == -1) {
            text = instr.raw_text;
        } else {
            // build renamed text and add stage predicate
            text = rebuild_instruction_text(instr, new_dest_reg[id], new_source_regs[id]);
            int pred_reg = 32 + stage;
            text = "(p" + std::to_string(pred_reg) + ") " + text;
        }

        loop_instrs.push_back({id, stage, slot_in_ii, instr.kind, text});
    }

    // find how far current loop body extends
    int max_loop_cycle = loop_beginning;
    for (const int id : ordered_ids) {
        if (instructions[id].basic_block == BasicBlock::BB1 && scheduled_cycle[id] >= loop_beginning) {
            max_loop_cycle = std::max(max_loop_cycle, scheduled_cycle[id]);
        }
    }

    // clear current loop body area
    for (int c = loop_beginning; c <= max_loop_cycle; ++c) {
        if (c < static_cast<int>(schedule.size())) {
            schedule[c] = Bundle();
        }
    }

    // insert mov p32,true and mov EC, if bundle before loop has room use it, else shift loop
    std::string ec_text = "mov EC, " + std::to_string(num_stages - 1);
    std::string p32_text = "mov p32, true";

    int new_loop_beginning;

    if (loop_beginning > 0) {
        Bundle& prev = schedule[loop_beginning - 1];
        int free_alu = (prev.ALU0 == "nop" ? 1 : 0) + (prev.ALU1 == "nop" ? 1 : 0);

        if (free_alu >= 2) {
            // two free ALU slots, place both there
            place_in_bundle(prev, InstructionKind::Mov, ec_text);
            place_in_bundle(prev, InstructionKind::Mov, p32_text);
            new_loop_beginning = loop_beginning;
        } else if (free_alu == 1) {
            // one free slot: put EC there, p32 needs a new bundle
            place_in_bundle(prev, InstructionKind::Mov, ec_text);
            // shift loop body by 1 to make room for p32 bundle
            new_loop_beginning = loop_beginning + 1;
            ensure_bundle_capacity(schedule, new_loop_beginning + ii - 1);
            for (int c = loop_beginning; c < new_loop_beginning + ii; ++c) {
                if (c < static_cast<int>(schedule.size())) schedule[c] = Bundle();
            }
            schedule[loop_beginning].ALU0 = p32_text;
        } else {
            // no free ALU slots, make a new bundle for both
            new_loop_beginning = loop_beginning + 1;
            ensure_bundle_capacity(schedule, new_loop_beginning + ii - 1);
            for (int c = loop_beginning; c < new_loop_beginning + ii; ++c) {
                if (c < static_cast<int>(schedule.size())) schedule[c] = Bundle();
            }
            schedule[loop_beginning].ALU0 = ec_text;
            schedule[loop_beginning].ALU1 = p32_text;
        }
    } else {
        // no BB0, create a bundle for EC/p32 at cycle 0
        new_loop_beginning = 1;
        ensure_bundle_capacity(schedule, new_loop_beginning + ii - 1);
        for (int c = 0; c < new_loop_beginning + ii; ++c) {
            if (c < static_cast<int>(schedule.size())) schedule[c] = Bundle();
        }
        schedule[0].ALU0 = ec_text;
        schedule[0].ALU1 = p32_text;
    }

    // clear new II loop area
    for (int c = new_loop_beginning; c < new_loop_beginning + ii; ++c) {
        ensure_bundle_capacity(schedule, c);
        schedule[c] = Bundle();
    }

    // place collapsed loop instructions into II bundles
    for (const auto& li : loop_instrs) {
        int target_cycle = new_loop_beginning + li.slot_in_ii;
        ensure_bundle_capacity(schedule, target_cycle);
        Bundle& b = schedule[target_cycle];

        if (li.kind == InstructionKind::Loop || li.kind == InstructionKind::LoopPip) {
            // now we know new_loop_beginning, so write final branch text
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
        const int cycle = scheduled_cycle[id];
        if (cycle < 0) continue;
        if (instr.kind == InstructionKind::Mov && instr.destination_register == -1) continue;

        const std::string new_text = rebuild_instruction_text(
            instr, new_dest_reg[id], new_source_regs[id]);
        rewrite_bundle_slot(schedule[cycle], instr, new_text);
    }

    // trim schedule to BB0 + EC/P32 + II loop bundles BB2 will be added later by schedule_bb2
    schedule.resize(new_loop_beginning + ii);

    // update loop_beginning for return value
    loop_beginning = new_loop_beginning;

    // update loop instruction cycle to final position
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