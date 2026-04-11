#include "scheduler.hpp"
#include <stdexcept>
#include <algorithm>

namespace {

// all instructions have latency = 1, except for mulu 
int instruction_latency(InstructionKind kind) {
    if (kind == InstructionKind::Mulu) {
        return 3;
    }
    return 1;
}

// All these instructions belong to an ALU 
bool is_alu_kind(InstructionKind kind) {
    return kind == InstructionKind::Add || kind == InstructionKind::Addi ||
           kind == InstructionKind::Sub || kind == InstructionKind::Mov;
}

// Adds one cycle to the schedule 
bool ensure_bundle_capacity(std::vector<Bundle>& schedule, int cycle) {
    if (cycle < 0) {
        return false;
    }
    if (static_cast<int>(schedule.size()) <= cycle) {
        schedule.resize(static_cast<size_t>(cycle + 1));
    }
    return true;
}

// Checks if an instructions can be placed in a bundle 
bool can_place_in_bundle(const Bundle& bundle, InstructionKind kind) {
    if (is_alu_kind(kind)) {
        return bundle.ALU0 == "nop" || bundle.ALU1 == "nop";
    }

    switch (kind) {
        case InstructionKind::Mulu:
            return bundle.MUL == "nop";
        case InstructionKind::Ld:
        case InstructionKind::St:
            return bundle.MEM == "nop";
        case InstructionKind::Loop:
        case InstructionKind::LoopPip:
            return bundle.BRANCH == "nop";
        default:
            return false;
    }
}

// Places instruntion in the bundle and returns true / false depending on the output 
bool place_in_bundle(Bundle& bundle, InstructionKind kind, const std::string& raw_text) {
    // First we check if we have an ALU instruction (and we check if both the ALUs are occupied)
    if (is_alu_kind(kind)) {
        if (bundle.ALU0 == "nop") {
            bundle.ALU0 = raw_text;
            return true;
        }
        if (bundle.ALU1 == "nop") {
            bundle.ALU1 = raw_text;
            return true;
        }
        return false;
    }

    // Then we analyze all the other cases
    switch (kind) {
        case InstructionKind::Mulu:
            if (bundle.MUL != "nop") {
                return false;
            }
            bundle.MUL = raw_text;
            return true;
        case InstructionKind::Ld:
        case InstructionKind::St:
            if (bundle.MEM != "nop") {
                return false;
            }
            bundle.MEM = raw_text;
            return true;
        case InstructionKind::Loop:
        case InstructionKind::LoopPip:
            if (bundle.BRANCH != "nop") {
                return false;
            }
            bundle.BRANCH = raw_text;
            return true;
        default:
            return false;
    }
}

// returns the earliest ready cycle where we can schedule the instruction given the dependencies (the cycle when the producer was 
// scheduled and the latency of the producer) 
int max_ready_cycle(const std::vector<int>& deps,
                    const std::vector<int>& scheduled_cycle,
                    const std::vector<InstructionKind>& kind_by_id) {
    int earliest = 0;
    for (const int dep_id : deps) {
        if (dep_id < 0 || dep_id >= static_cast<int>(scheduled_cycle.size())) {
            continue;
        }
        if (scheduled_cycle[dep_id] < 0) {
            continue;
        }
        earliest = std::max(earliest, scheduled_cycle[dep_id] + instruction_latency(kind_by_id[dep_id]));
    }
    return earliest;
}

// Checks loop-carried recurrence constraints after a full BB1 placement attempt.
// For a distance-1 recurrence edge producer -> consumer:
// consumer_cycle >= producer_cycle + latency(producer) - II
bool interloop_constraints_satisfied(const std::vector<int>& bb1_ids,
                                     const std::vector<bool>& is_bb1,
                                     const std::vector<int>& analysis_index_by_id,
                                     const std::vector<DependencyAnalysisTableEntry>& analysis_table,
                                     const std::vector<int>& scheduled_cycle,
                                     const std::vector<InstructionKind>& kind_by_id,
                                     int ii) {

    // Sanity checks to ensure we have valid data before checking constraints
    for (const int id : bb1_ids) {
        if (id < 0 || id >= static_cast<int>(analysis_index_by_id.size())) {
            continue;
        }

        const int entry_index = analysis_index_by_id[id];
        if (entry_index < 0) {
            continue;
        }

        const DependencyAnalysisTableEntry& entry = analysis_table[entry_index];
        if (id < 0 || id >= static_cast<int>(scheduled_cycle.size()) || scheduled_cycle[id] < 0) {
            continue;
        }

        for (const int dep_id : entry.interloop_dependencies) {
            if (dep_id < 0 || dep_id >= static_cast<int>(scheduled_cycle.size())) {
                continue;
            }
            if (scheduled_cycle[dep_id] < 0) {
                continue;
            }

            // We check if the consumer is scheduled at least latency cycles after the producer, considering the modulo scheduling with II.
            const int required_cycle = scheduled_cycle[dep_id] + instruction_latency(kind_by_id[dep_id]) - ii;
            // We need to check if the producer is in BB1 to understand if we need to apply the constraint 
            // (if the producer is not in BB1, it means that it is scheduled outside the loop and we don't
            // need to apply the constraint, since the consumer will always be scheduled after the producer)

            if (scheduled_cycle[id] < required_cycle && is_bb1[dep_id]) {
                return false;
            }
        }
    }

    return true;
}

// Schedules an entry (no modulo scheduling, used for BB0 and BB2 instructions)
int schedule_entry_no_modulo(const DependencyAnalysisTableEntry& entry,
                             const std::vector<Instruction>& instructions,
                             std::vector<Bundle>& schedule,
                             std::vector<int>& scheduled_cycle,
                             const std::vector<InstructionKind>& kind_by_id,
                             const std::vector<int>& extra_dependencies = {}) {
    int earliest = max_ready_cycle(entry.local_dependencies, scheduled_cycle, kind_by_id);
    earliest = std::max(earliest, max_ready_cycle(extra_dependencies, scheduled_cycle, kind_by_id));

    int cycle = earliest;
    while (true) {
        ensure_bundle_capacity(schedule, cycle);
        if (can_place_in_bundle(schedule[cycle], entry.instruction_type)) {
            break;
        }
        ++cycle;
    }

    place_in_bundle(schedule[cycle], entry.instruction_type, instructions[entry.id].raw_text);
    scheduled_cycle[entry.id] = cycle;
    return cycle;
}

bool try_bb1_schedule_with_ii(const std::vector<int>& bb1_ids,
                              const std::vector<bool>& is_bb1,
                              const std::vector<int>& analysis_index_by_id,
                              const std::vector<DependencyAnalysisTableEntry>& analysis_table,
                              std::vector<Bundle>& schedule,
                              std::vector<int>& scheduled_cycle,
                              const std::vector<InstructionKind>& kind_by_id,
                              const std::vector<Instruction>& instructions,
                              SlotTable& slot_table,
                              int& ii,
                              int loop_beginning) {
    for (const int id : bb1_ids) {
        // With the id we find the corresponding entry index in the analysis table 
        const int entry_index = analysis_index_by_id[id];
        if (entry_index < 0) {
            continue;
        }

        // We extract the entry from the analysis table and we schedule it with modulo scheduling 
        const DependencyAnalysisTableEntry& entry = analysis_table[entry_index];

        // We calculate the earliest cycle we can schedule the instruction based on its dependencies (local, loop invariant, and interloop dependencies)
        int earliest = max_ready_cycle(entry.local_dependencies, scheduled_cycle, kind_by_id);
        earliest = std::max(earliest, max_ready_cycle(entry.loop_invariant_dependencies, scheduled_cycle, kind_by_id));

        // BB1 instructions must stay inside the loop window [loop_beginning, loop_beginning + II).
        earliest = std::max(earliest, loop_beginning);

        // If earliest >= loop_beginning + ii, it means that we cannot schedule the instruction in the current II window, so we can already return false and increase II for the next scheduling attempt
        if (earliest >= loop_beginning + ii) { 
            ii++; 
            return false;
        }

        int cycle = earliest;
        while (true) {
            if (cycle > 1000) { // Sanity check to avoid infinite loops in case of bugs
                throw std::runtime_error("Scheduling failed: exceeded reasonable cycle limit.");
            }
            ensure_bundle_capacity(schedule, cycle);
            if (slot_table.can_schedule(cycle, entry.instruction_type) &&
                can_place_in_bundle(schedule[cycle], entry.instruction_type)) {
                break;
            }
            ++cycle;
            if (cycle >= loop_beginning + ii) { 
                // Cannot fit instruction in current II window; signal failure and increase II
                ii++;
                return false;
            }
        }

        place_in_bundle(schedule[cycle], entry.instruction_type, instructions[id].raw_text);
        slot_table.reserve_resources(cycle, entry.instruction_type);
        scheduled_cycle[id] = cycle;
    }

    // Validate loop-carried dependencies once all BB1 instructions have tentative cycles.
    if (!interloop_constraints_satisfied(bb1_ids, is_bb1, analysis_index_by_id, analysis_table, scheduled_cycle, kind_by_id, ii)) {
        ii++;
        return false;
    }
    // If we reach this point, the scheduling was successful with the current II, considering all possible dependencies and resource constraints.

    return true;
}

} // namespace

// version using loop (without loop.pip)
void schedule_ASAP_basic(const std::vector<DependencyAnalysisTableEntry>& analysis_table,
                         std::vector<Bundle>& schedule,
                         const std::vector<Instruction>& instructions) {
    
    // We clear the schedule vector for rubustness (already supposed to be empty)
    schedule.clear();

    // Compute the instruction count 
    const int instruction_count = static_cast<int>(instructions.size());

    // Preallocate vectors for which we know the size (instruction_count) and initialize them to undefined values (-1 and Unknown)
    // To easily extract information on an instruction based on the id 
    std::vector<int> analysis_index_by_id(static_cast<size_t>(instruction_count), -1);
    std::vector<InstructionKind> kind_by_id(static_cast<size_t>(instruction_count), InstructionKind::Unknown);

    // Defining the vectors where we'll store the different ids of the belonging instructions 
    std::vector<int> bb0_ids;
    std::vector<int> bb1_ids;
    std::vector<int> bb2_ids;
    std::vector<bool> is_bb1(instruction_count, false); // To quickly check if an instruction belongs to BB1

    // We analyse all the instructions in the dependency analysis table to allocate them in the different basic blocks and to extract useful information for 
    // the scheduling (the kind of the instruction and the index of the entry of the analysis table based on the id of the instruction)
    for (int analysis_index = 0; analysis_index < static_cast<int>(analysis_table.size()); ++analysis_index) {

        // We extract an entry from the dependency table 
        const DependencyAnalysisTableEntry& entry = analysis_table[analysis_index];
        if (entry.id < 0 || entry.id >= instruction_count) {
            continue;
        }
        
        // We map the analysis index and the kind to the entry id 
        analysis_index_by_id[entry.id] = analysis_index;
        kind_by_id[entry.id] = entry.instruction_type;

        // We divide all the entries in the basick blocks they belong to differentiate the scheduling strategy 
        switch (instructions[entry.id].basic_block) {
            case BasicBlock::BB0: bb0_ids.push_back(entry.id); break;
            case BasicBlock::BB1: bb1_ids.push_back(entry.id); is_bb1[entry.id] = true; break;
            case BasicBlock::BB2: bb2_ids.push_back(entry.id); break;
        }
    }

    // Preallocate the schedule cycles vector
    std::vector<int> scheduled_cycle(static_cast<size_t>(instruction_count), -1);

    // BB0 contains setup code, scheduled once before the loop body.
    for (const int id : bb0_ids) {
        const int entry_index = analysis_index_by_id[id];
        if (entry_index < 0) {
            continue;
        }

        // We extract the entry from the analysis table and we schedule it 
        const DependencyAnalysisTableEntry& entry = analysis_table[entry_index];
        schedule_entry_no_modulo(entry, instructions, schedule, scheduled_cycle, kind_by_id);
    }

    // Push loop_beginning past any BB0 latency tails to avoid bubbles at the start of the loop body (see Figure 9)
    int loop_beginning = static_cast<int>(schedule.size());

    for (const int id : bb1_ids) {
        const int entry_index = analysis_index_by_id[id];
        if (entry_index < 0) continue;
        const DependencyAnalysisTableEntry& entry = analysis_table[entry_index];

        int bb0_ready = max_ready_cycle(entry.loop_invariant_dependencies, scheduled_cycle, kind_by_id);
        for (const int dep_id : entry.interloop_dependencies) {
            if (dep_id >= 0 && dep_id < instruction_count &&
                instructions[dep_id].basic_block == BasicBlock::BB0 &&
                scheduled_cycle[dep_id] >= 0) {
                bb0_ready = std::max(bb0_ready, scheduled_cycle[dep_id] + instruction_latency(kind_by_id[dep_id]));
            }
        }
        loop_beginning = std::max(loop_beginning, bb0_ready);
    }

    if (loop_beginning > 0) {
        ensure_bundle_capacity(schedule, loop_beginning - 1);
    }

    // Separate the loop instruction from the other BB1 instructions
    int loop_id = -1;
    std::vector<int> bb1_non_loop_ids;
    for (const int id : bb1_ids) {
        if (instructions[id].kind == InstructionKind::Loop || instructions[id].kind == InstructionKind::LoopPip) {
            loop_id = id;
        } else {
            bb1_non_loop_ids.push_back(id);
        }
    }

    // Schedule BB1 instructions (except loop) ASAP from loop_beginning.
    // Without loop.pip, the loop body has a single stage and the II equals the loop body length,
    // so we don't need modulo scheduling or the SlotTable here.
    for (const int id : bb1_non_loop_ids) {
        const int entry_index = analysis_index_by_id[id];
        if (entry_index < 0) continue;
        const DependencyAnalysisTableEntry& entry = analysis_table[entry_index];

        int earliest = max_ready_cycle(entry.local_dependencies, scheduled_cycle, kind_by_id);
        earliest = std::max(earliest, max_ready_cycle(entry.loop_invariant_dependencies, scheduled_cycle, kind_by_id));
        earliest = std::max(earliest, loop_beginning);

        int cycle = earliest;
        while (true) {
            ensure_bundle_capacity(schedule, cycle);
            if (can_place_in_bundle(schedule[cycle], entry.instruction_type)) break;
            ++cycle;
        }
        place_in_bundle(schedule[cycle], entry.instruction_type, instructions[id].raw_text);
        scheduled_cycle[id] = cycle;
    }

    // Place the loop instruction at the last occupied BB1 cycle (it must close the loop body)
    int last_bb1_cycle = loop_beginning;
    for (const int id : bb1_non_loop_ids) {
        last_bb1_cycle = std::max(last_bb1_cycle, scheduled_cycle[id]);
    }

    int loop_cycle = last_bb1_cycle;
    while (true) {
        ensure_bundle_capacity(schedule, loop_cycle);
        if (can_place_in_bundle(schedule[loop_cycle], InstructionKind::Loop)) break;
        ++loop_cycle;
    }
    place_in_bundle(schedule[loop_cycle], InstructionKind::Loop, instructions[loop_id].raw_text);
    scheduled_cycle[loop_id] = loop_cycle;

    int ii = loop_cycle - loop_beginning + 1;

    // Check interloop constraints (equation 2). If violated, push the loop instruction down.
    while (!interloop_constraints_satisfied(bb1_ids, is_bb1, analysis_index_by_id, analysis_table,
                                            scheduled_cycle, kind_by_id, ii)) {
        schedule[loop_cycle].BRANCH = "nop";
        ++loop_cycle;
        ensure_bundle_capacity(schedule, loop_cycle);
        while (!can_place_in_bundle(schedule[loop_cycle], InstructionKind::Loop)) {
            ++loop_cycle;
            ensure_bundle_capacity(schedule, loop_cycle);
        }
        place_in_bundle(schedule[loop_cycle], InstructionKind::Loop, instructions[loop_id].raw_text);
        scheduled_cycle[loop_id] = loop_cycle;
        ii = loop_cycle - loop_beginning + 1;

        if (ii > 100) {
            throw std::runtime_error("Scheduling failed: exceeded reasonable II limit.");
        }
    }

    // BB2 executes after the loop, so it must respect both local and post-loop dependencies.
    for (const int id : bb2_ids) {
        const int entry_index = analysis_index_by_id[id];
        if (entry_index < 0) {
            continue;
        }
        const DependencyAnalysisTableEntry& entry = analysis_table[entry_index];
        std::vector<int> deps = entry.post_loop_dependencies;
        deps.insert(deps.end(), entry.loop_invariant_dependencies.begin(), entry.loop_invariant_dependencies.end());
        schedule_entry_no_modulo(entry, instructions, schedule, scheduled_cycle, kind_by_id, deps);
    }
}

// version using loop.pip
void schedule_ASAP_advanced(const std::vector<DependencyAnalysisTableEntry>& analysis_table,
                            std::vector<Bundle>& schedule,
                            const std::vector<Instruction>& instructions) {

    schedule.clear();
    const int instruction_count = static_cast<int>(instructions.size());

    std::vector<int> analysis_index_by_id(static_cast<size_t>(instruction_count), -1);
    std::vector<InstructionKind> kind_by_id(static_cast<size_t>(instruction_count), InstructionKind::Unknown);

    std::vector<int> bb0_ids;
    std::vector<int> bb1_ids;
    std::vector<int> bb2_ids;
    std::vector<bool> is_bb1(instruction_count, false);

    for (int analysis_index = 0; analysis_index < static_cast<int>(analysis_table.size()); ++analysis_index) {
        const DependencyAnalysisTableEntry& entry = analysis_table[analysis_index];
        if (entry.id < 0 || entry.id >= instruction_count) continue;

        analysis_index_by_id[entry.id] = analysis_index;
        kind_by_id[entry.id] = entry.instruction_type;

        switch (instructions[entry.id].basic_block) {
            case BasicBlock::BB0: bb0_ids.push_back(entry.id); break;
            case BasicBlock::BB1: bb1_ids.push_back(entry.id); is_bb1[entry.id] = true; break;
            case BasicBlock::BB2: bb2_ids.push_back(entry.id); break;
        }
    }

    std::vector<int> scheduled_cycle(static_cast<size_t>(instruction_count), -1);

    // BB0 scheduling
    for (const int id : bb0_ids) {
        const int entry_index = analysis_index_by_id[id];
        if (entry_index < 0) continue;
        const DependencyAnalysisTableEntry& entry = analysis_table[entry_index];
        schedule_entry_no_modulo(entry, instructions, schedule, scheduled_cycle, kind_by_id);
    }

    // Push loop_beginning past any BB0 latency tails to avoid bubbles 
    int loop_beginning = static_cast<int>(schedule.size());

    for (const int id : bb1_ids) {
        const int entry_index = analysis_index_by_id[id];
        if (entry_index < 0) continue;
        const DependencyAnalysisTableEntry& entry = analysis_table[entry_index];

        int bb0_ready = max_ready_cycle(entry.loop_invariant_dependencies, scheduled_cycle, kind_by_id);
        for (const int dep_id : entry.interloop_dependencies) {
            if (dep_id >= 0 && dep_id < instruction_count &&
                instructions[dep_id].basic_block == BasicBlock::BB0 &&
                scheduled_cycle[dep_id] >= 0) {
                bb0_ready = std::max(bb0_ready, scheduled_cycle[dep_id] + instruction_latency(kind_by_id[dep_id]));
            }
        }
        loop_beginning = std::max(loop_beginning, bb0_ready);
    }
    if (loop_beginning > 0) {
        ensure_bundle_capacity(schedule, loop_beginning - 1);
    }

    // Separate loop.pip from other BB1 instructions
    int loop_id = -1;
    std::vector<int> bb1_non_loop_ids;
    for (const int id : bb1_ids) {
        if (instructions[id].kind == InstructionKind::Loop || instructions[id].kind == InstructionKind::LoopPip) {
            loop_id = id;
        } else {
            bb1_non_loop_ids.push_back(id);
        }
    }

    // BB1 modulo scheduling with increasing II
    int ii = std::max(1, calculate_II_res(instructions));

    while (true) {
        SlotTable slot_table;
        slot_table.init_reset(ii);

        for (const int id : bb1_ids) {
            scheduled_cycle[id] = -1;
        }
        schedule.resize(loop_beginning);

        bool failed = false;

        for (const int id : bb1_non_loop_ids) {
            const int entry_index = analysis_index_by_id[id];
            if (entry_index < 0) continue;
            const DependencyAnalysisTableEntry& entry = analysis_table[entry_index];

            int earliest = max_ready_cycle(entry.local_dependencies, scheduled_cycle, kind_by_id);
            earliest = std::max(earliest, max_ready_cycle(entry.loop_invariant_dependencies, scheduled_cycle, kind_by_id));
            earliest = std::max(earliest, loop_beginning);

            // TODO: also account for interloop dependencies where the producer is
            // already scheduled in BB1, using equation 2 rearranged as a lower bound
            // on the consumer cycle. Skip BB0 producers and unscheduled producers.

            // TODO: find the first cycle >= earliest where both the SlotTable allows
            // scheduling (modulo resource check) and the bundle has a free slot.
            // Unlike basic, there is no upper cycle bound since instructions can
            // span multiple stages. Place the instruction and reserve resources.
            // If you want, you can set a sanity-check upper bound and set failed=true.
        }

        if (failed) {
            ii++;
            if (ii > 100) throw std::runtime_error("Scheduling failed: exceeded reasonable II limit.");
            continue;
        }

        // Validate all interloop constraints that couldn't be checked during scheduling
        // (producers that appear after their consumer in program order)
        if (!interloop_constraints_satisfied(bb1_ids, is_bb1, analysis_index_by_id, analysis_table,
                                             scheduled_cycle, kind_by_id, ii)) {
            ii++;
            if (ii > 100) throw std::runtime_error("Scheduling failed: exceeded reasonable II limit.");
            continue;
        }

        break;
    }

    // TODO: place loop.pip in the Branch slot at the last bundle of the first stage,
    // and update its loop target to point to loop_beginning

    // TODO: compute the number of stages and each instruction's stage index —
    // these will be needed later for register allocation and predication

    // BB2 scheduling
    for (const int id : bb2_ids) {
        const int entry_index = analysis_index_by_id[id];
        if (entry_index < 0) continue;
        const DependencyAnalysisTableEntry& entry = analysis_table[entry_index];
        std::vector<int> deps = entry.post_loop_dependencies;
        deps.insert(deps.end(), entry.loop_invariant_dependencies.begin(), entry.loop_invariant_dependencies.end());
        schedule_entry_no_modulo(entry, instructions, schedule, scheduled_cycle, kind_by_id, deps);
    }
}

int calculate_II_res(const std::vector<Instruction>& instructions) {

    int alu_count = 0;
    int mul_count = 0;
    int mem_count = 0;

    // we count the number of ALU, MUL, and MEM instructions in BB1 to determine the II_res
    // they will be useful for the formula calculattion for II_res 
    for (const auto& instr : instructions) {
        
        if (instr.basic_block != BasicBlock::BB1) {
            continue; // Only consider instructions in BB1
        }

        switch (instr.kind) {
            case InstructionKind::Add:
            case InstructionKind::Addi:
            case InstructionKind::Sub:
            case InstructionKind::Mov:
                alu_count++;
                break;
            case InstructionKind::Mulu:
                mul_count++;
                break;
            case InstructionKind::Ld:
            case InstructionKind::St:
                mem_count++;
                break;
            default:
                break; // Ignore other instruction types
        }
    }

    // we use the alu_count + 1 for doing the ceiling of the division by 2
    int ii_alu = (alu_count + 1) / 2; // Each bundle can have up to 2 ALU instructions
    int ii_mul = mul_count; // Each bundle can have 1 MUL instruction
    int ii_mem = mem_count; // Each bundle can have 1 MEM instruction

    // The II_res is the maximum of the three calculated values
    int ii_res = std::max({ii_alu, ii_mul, ii_mem, 1}); // (the 1 is the ensure is at least 1)

    return ii_res;
}