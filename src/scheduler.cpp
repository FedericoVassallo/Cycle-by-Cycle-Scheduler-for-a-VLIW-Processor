#include "scheduler.hpp"

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

} // namespace

// version using loop (without loop.pip)
void schedule_ASAP_basic(std::vector<DependencyAnalysisTableEntry>& analysis_table, std::vector<Bundle>& schedule, std::vector<Instruction>& instructions) {
    
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

    // We analyse all the instructions in the dependency analysis table 
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
            case BasicBlock::BB0:
                bb0_ids.push_back(entry.id);
                break;
            case BasicBlock::BB1:
                bb1_ids.push_back(entry.id);
                break;
            case BasicBlock::BB2:
                bb2_ids.push_back(entry.id);
                break;
        }
    }

    // Preallocate the schedule cycles vector
    std::vector<int> scheduled_cycle(static_cast<size_t>(instruction_count), -1);

    // BB0 contains setup code, scheduled once before the loop body.
    for (const int id : bb0_ids) {
        const int entry_index = analysis_index_by_id[id]; // Index of the entry of the analysis table 
        if (entry_index < 0) {
            continue;
        }

        // We extract the entry from the analysis table and we schedule it 
        const DependencyAnalysisTableEntry& entry = analysis_table[entry_index];
        schedule_entry_no_modulo(entry, instructions, schedule, scheduled_cycle, kind_by_id);
    }

    // BB1 is scheduled with modulo resource constraints through SlotTable.
    const int ii = std::max(1, calculate_II_res(instructions));
    SlotTable slot_table;
    slot_table.init_reset(ii);

    for (const int id : bb1_ids) {
        const int entry_index = analysis_index_by_id[id];
        if (entry_index < 0) {
            continue;
        }
        const DependencyAnalysisTableEntry& entry = analysis_table[entry_index];

        int earliest = max_ready_cycle(entry.local_dependencies, scheduled_cycle, kind_by_id);
        earliest = std::max(earliest, max_ready_cycle(entry.loop_invariant_dependencies, scheduled_cycle, kind_by_id));

        // First implementation: treat interloop deps conservatively like normal dependencies.
        earliest = std::max(earliest, max_ready_cycle(entry.interloop_dependencies, scheduled_cycle, kind_by_id));

        int cycle = earliest;
        while (true) {
            ensure_bundle_capacity(schedule, cycle);
            if (slot_table.can_schedule(cycle, entry.instruction_type) &&
                can_place_in_bundle(schedule[cycle], entry.instruction_type)) {
                break;
            }
            ++cycle;
        }

        place_in_bundle(schedule[cycle], entry.instruction_type, instructions[id].raw_text);
        slot_table.reserve_resources(cycle, entry.instruction_type);
        scheduled_cycle[id] = cycle;
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
void schedule_ASAP_advanced(std::vector<DependencyAnalysisTableEntry>& analysis_table, std::vector<Bundle>& schedule, std::vector<Instruction>& instructions) {

    // TODO

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