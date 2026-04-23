// for this hw we always have the assumption of having enough registers.

#include "register_allocation.hpp"
#include "data_structures.hpp"
#include <algorithm>
#include <map>
#include <string>

namespace {

// ============================================================================
// Small helpers shared by alloc_b and alloc_r
// ============================================================================

// Returns true if this instruction should get a fresh destination register.
// Excludes loop/loop.pip (no GP dest), mov to LC/EC (dest -1), stores (no dest).
bool instr_needs_dest_register(const Instruction& instr) {
    if (instr.kind == InstructionKind::Loop || instr.kind == InstructionKind::LoopPip) return false;
    if (instr.kind == InstructionKind::St) return false;
    if (instr.kind == InstructionKind::Mov && instr.destination_register == -1) return false;
    return instr.destination_register != -1;
}

// Slot priority for breaking ties when two instructions share a cycle.
// Matches the bundle layout: ALU0, ALU1, MUL, MEM, BRANCH.
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

// Build instruction ids in scheduling order (by cycle, then slot, then program order).
// BB2 instructions (not yet scheduled, so scheduled_cycle[i] == -1) are appended at the end
// in program order so they still get allocated.
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

// Build a lookup table: instruction id -> row index in analysis_table.
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

// Search a dependency list for a producer that writes `target_reg` and is in `target_bb`.
// Returns the producer id, or -1 if none found.
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

// Search a dependency list for a producer writing `target_reg` (any BB). Returns id or -1.
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

// Overwrite the bundle slot that currently holds `instr.raw_text` with `new_text`.
// For ALU instructions the slot might be ALU0 or ALU1 — match on raw_text.
// Other kinds (MUL, MEM, BRANCH) are unambiguous: each bundle has exactly one such slot.
// If the slot can't be found (already rewritten, or not in this bundle), do nothing.
void rewrite_bundle_slot(Bundle& bundle, const Instruction& instr, const std::string& new_text) {
    if (is_alu_kind(instr.kind)) {
        if (bundle.ALU0 == instr.raw_text) bundle.ALU0 = new_text;
        else if (bundle.ALU1 == instr.raw_text) bundle.ALU1 = new_text;
    } else if (instr.kind == InstructionKind::Mulu) {
        bundle.MUL = new_text;
    } else if (instr.kind == InstructionKind::Ld || instr.kind == InstructionKind::St) {
        bundle.MEM = new_text;
    }
    // loop/loop.pip and nop/mov-to-LC-or-EC are handled by the callers explicitly.
}

// Walk the schedule and rewrite every placed instruction's text using the new
// register names. BB2 instructions are skipped (they aren't in the schedule yet
// when alloc_b runs — rewrite_bb2_bundles handles them later).
// The loop instruction gets its branch target updated to `loop_beginning`.
// mov to LC/EC keeps its original text (no GP register to rename).
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

        // Loop instruction: branch target was relative to original program addresses;
        // retarget it to the start of the scheduled loop body.
        if (instr.kind == InstructionKind::Loop || instr.kind == InstructionKind::LoopPip) {
            schedule[cycle].BRANCH = instr.opcode + " " + std::to_string(loop_beginning);
            continue;
        }

        // mov to LC/EC has no GP register to rename — keep raw text.
        if (instr.kind == InstructionKind::Mov && instr.destination_register == -1) continue;

        const std::string new_text = rebuild_instruction_text(
            instr, new_dest_reg[id], new_source_regs[id]);
        rewrite_bundle_slot(schedule[cycle], instr, new_text);
    }
}

// Resolve source operands that are still -1 after the main allocation phases:
// they read a register nobody writes (e.g. x0, or base addresses pre-loaded).
// Two reads of the same original register must get the same new register.
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

// ============================================================================
// alloc_r phase helpers
// ============================================================================
//
// alloc_r is split into small phases that match the four phases described in
// PDF Section 3.3.2. Each helper takes the state it needs and updates
// new_dest_reg / new_source_regs. next_static_reg is threaded by reference
// because several phases draw from the same static register pool.

// Phase 1: assign rotating registers (x32, x32+stride, ...) to BB1 destinations.
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

// Identify BB0 instructions that act as interloop initializers.
// A BB0 instruction whose dest register is both in some BB1 consumer's
// interloop_dependencies AND written again inside BB1 is an initializer.
// When multiple BB0 instructions write the same register, only the latest
// one (largest id) is the true initializer.
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

        std::map<int, int> latest_bb0_per_reg; // dest reg -> latest BB0 id
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

// Phase 2: assign static registers (x1, x2, ...) to pure loop invariant BB0 producers.
// Skips BB0 instructions that are interloop initializers — those are handled in phase 4.
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

// Resolve a single BB1 source operand against the dependency lists.
// Returns the new register name, or -1 if no producer is known yet (orphan).
// Applies equations 3 and 4 from PDF Section 3.3.2.
int r_resolve_bb1_source(int src_reg,
                         int consumer_stage,
                         const DependencyAnalysisTableEntry& entry,
                         const std::vector<Instruction>& instructions,
                         const std::vector<int>& new_dest_reg,
                         const std::vector<int>& stage_by_id) {

    // 1. Loop invariant — use the static register directly (no offset).
    int p = find_producer_any_bb(entry.loop_invariant_dependencies, src_reg, instructions);
    if (p != -1 && new_dest_reg[p] != -1) return new_dest_reg[p];

    // 2. Local dep (same iteration) — equation 3: dest + (St(consumer) - St(producer)).
    p = find_producer_any_bb(entry.local_dependencies, src_reg, instructions);
    if (p != -1 && new_dest_reg[p] != -1) {
        return new_dest_reg[p] + (consumer_stage - stage_by_id[p]);
    }

    // 3. Interloop dep with BB1 producer — equation 4: dest + (St(consumer) - St(producer)) + 1.
    p = find_producer_in(entry.interloop_dependencies, src_reg, BasicBlock::BB1, instructions);
    if (p != -1 && new_dest_reg[p] != -1) {
        return new_dest_reg[p] + (consumer_stage - stage_by_id[p]) + 1;
    }

    // 4. Interloop dep with BB0 initializer — use the matching BB1 producer's name.
    //    The consumer physically reads the BB0-initializer's register, but because
    //    phase 4 will set that register to equal the BB1 producer's name with offsets,
    //    we compute the same expression as case 3 using the BB1 producer.
    int bb0 = find_producer_in(entry.interloop_dependencies, src_reg, BasicBlock::BB0, instructions);
    if (bb0 != -1) {
        p = find_producer_in(entry.interloop_dependencies, src_reg, BasicBlock::BB1, instructions);
        if (p != -1 && new_dest_reg[p] != -1) {
            return new_dest_reg[p] + (consumer_stage - stage_by_id[p]) + 1;
        }
    }

    return -1;
}

// Phase 3: resolve source operands of BB1 instructions.
// BB0/BB2 operands are left as placeholders (-1) for phase 4 to fill in.
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

// For a given BB0 initializer instruction, find the corresponding BB1 producer
// of the same register. Returns the BB1 producer's id, or -1 if not found.
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

        // Only check consumers that actually list this BB0 instruction as an interloop dep.
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

// Phase 4a: assign destination registers for BB0 interloop initializers.
// Their name must line up with the rotating BB1 producer so iteration 1 reads the right value.
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

// Phase 4b: assign static regs to remaining BB0 and BB2 destinations.
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

// Phase 4c: resolve source operands of BB0 and BB2 instructions.
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
                // BB2: try post-loop → loop-invariant → local (in that order).
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

// Final pass: any remaining -1 source is an orphan — handled by resolve_orphan_sources.

} // anonymous namespace

// ============================================================================
// alloc_b phase helpers
// ============================================================================

// alloc_b Phase 2: for each instruction, find the producer of each source register
// in any of the four dependency lists and wire the source to the producer's new reg.
// Preference rules (these matter when a register has two producers):
//   * BB1 consumers: prefer the BB0 producer (loop body will mov BB1-result into it).
//                    Among BB0 producers, pick the one scheduled latest.
//                    Among non-BB0 producers: keep the first one seen.
//   * BB2 consumers: prefer the BB1 producer (BB2 wants the final iteration's value).
//                    Among non-BB1 producers: keep the last one seen (overwrites each pass).
// If no producer is found the source is left as -1 (resolved as an orphan later).
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

        // Collect all deps into one flat list so the search below is a simple scan.
        const DependencyAnalysisTableEntry& entry = analysis_table[ai];
        std::vector<int> all_deps;
        all_deps.insert(all_deps.end(), entry.local_dependencies.begin(),          entry.local_dependencies.end());
        all_deps.insert(all_deps.end(), entry.interloop_dependencies.begin(),      entry.interloop_dependencies.end());
        all_deps.insert(all_deps.end(), entry.loop_invariant_dependencies.begin(), entry.loop_invariant_dependencies.end());
        all_deps.insert(all_deps.end(), entry.post_loop_dependencies.begin(),      entry.post_loop_dependencies.end());

        const BasicBlock preferred = (instr.basic_block == BasicBlock::BB2)
                                     ? BasicBlock::BB1 : BasicBlock::BB0;
        // For BB1/BB0 consumers: fallback picks the FIRST non-preferred dep (break ties early).
        // For BB2 consumers: fallback picks the LAST non-preferred dep (overwrite on each pass).
        const bool fallback_keeps_first = (instr.basic_block != BasicBlock::BB2);

        for (const int src_reg : instr.source_registers) {
            int best = -1;
            bool best_is_preferred = false;

            for (const int dep_id : all_deps) {
                if (dep_id < 0 || dep_id >= n) continue;
                if (instructions[dep_id].destination_register != src_reg) continue;

                const BasicBlock dep_bb = instructions[dep_id].basic_block;

                if (dep_bb == preferred) {
                    // Preferred producer wins. For BB1 consumers (preferred=BB0), keep the
                    // latest-scheduled one; for BB2 consumers, just keep the first one.
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
    std::vector<std::vector<int>> new_source_regs(instruction_count);
    b_phase2_resolve_sources(ordered_ids, instructions, analysis_table,
                             analysis_index_by_id, scheduled_cycle,
                             new_dest_reg, new_source_regs);

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
    // PHASE 4: handle orphan operands (sources with no known producer)
    // ========================================================================
    resolve_orphan_sources(ordered_ids, instructions, new_source_regs, next_reg);

    // ========================================================================
    // BUNDLE REWRITE: replace original instruction text with new register names
    // ========================================================================
    // BB2 instructions aren't in the schedule yet — rewrite_bb2_bundles handles them
    // after schedule_bb2 places them.
    apply_renaming_to_bundles(schedule, ordered_ids, instructions, scheduled_cycle,
                              new_dest_reg, new_source_regs, loop_beginning, /*skip_bb2=*/true);

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

    // ------------------------------------------------------------------------
    // Register allocation for loop.pip (allocr in PDF Section 3.3.2).
    // The function is organized in four phases matching the PDF:
    //   Phase 1: rotating registers for BB1 destinations
    //   Phase 2: static registers for pure loop invariant BB0 producers
    //   Phase 3: resolve BB1 source operands (equations 3 and 4)
    //   Phase 4: allocate BB0/BB2 destinations and sources
    // After the four phases we collapse the loop body into II bundles with
    // predication and insert the mov EC / mov p32 setup (Section 3.4).
    // ------------------------------------------------------------------------

    const int instruction_count = static_cast<int>(instructions.size());
    const std::vector<int> analysis_index_by_id =
        build_analysis_index_by_id(analysis_table, instruction_count);
    const std::vector<int> ordered_ids = build_ordered_ids(instructions, scheduled_cycle);

    std::vector<int> new_dest_reg(instruction_count, -1);
    std::vector<std::vector<int>> new_source_regs(instruction_count);
    int next_static_reg = 1;

    // Phase 1: rotating registers for BB1 destinations.
    r_phase1_rotating_dests(ordered_ids, instructions, num_stages, new_dest_reg);

    // Identify BB0 interloop initializers — needed by phase 2 (to skip them) and phase 4.
    const std::vector<bool> is_interloop_initializer =
        r_find_interloop_initializers(ordered_ids, instructions, analysis_table, analysis_index_by_id);

    // Phase 2: static registers for pure loop invariant BB0 producers.
    r_phase2_invariant_dests(ordered_ids, instructions, analysis_table, analysis_index_by_id,
                             is_interloop_initializer, new_dest_reg, next_static_reg);

    // Phase 3: resolve source operands of BB1 instructions.
    r_phase3_bb1_sources(ordered_ids, instructions, analysis_table, analysis_index_by_id,
                         new_dest_reg, stage_by_id, new_source_regs);

    // Phase 4a: BB0 interloop initializer destinations must match their BB1 producer.
    r_phase4_bb0_initializer_dests(ordered_ids, instructions, analysis_table, analysis_index_by_id,
                                   is_interloop_initializer, stage_by_id, new_dest_reg);

    // Phase 4b: assign static regs to any remaining BB0 and BB2 destinations.
    r_phase4_static_dests(ordered_ids, instructions, new_dest_reg, next_static_reg);

    // Phase 4c: resolve source operands of BB0 and BB2 instructions.
    r_phase4_bb0_bb2_sources(ordered_ids, instructions, analysis_table, analysis_index_by_id,
                             new_dest_reg, stage_by_id, num_stages, new_source_regs);

    // Orphan sources: registers not written by any instruction get fresh static regs.
    resolve_orphan_sources(ordered_ids, instructions, new_source_regs, next_static_reg);

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

    // Rewrite BB0 instruction texts with the new registers.
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