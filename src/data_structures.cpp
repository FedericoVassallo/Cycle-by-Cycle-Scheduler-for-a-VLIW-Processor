#include "data_structures.hpp"
#include <stdexcept>

// since the scheduling might fail for a given II, we need to be able to set a new II and reset the table for the new scheduling attempt
void SlotTable::init_reset(int ii) {
    currentII = ii; // The current Initiation Interval (II) being evaluated
    table.clear();
    table.resize(ii); // Initialize the table with 'ii' entries, each representing a slot for one iteration
}

// we check if we can schedule an instruction in the current slot (which is determined by the current iteration and the current II) by checking the hardware resources used in that slot
// we get as input the kind of the instruction we want to schedule and we check if the corresponding hardware resource is available in the current slot
// to get the current slot we get it from the actual cycle we are trying to schedule the instruction by doing the modulo of the current cycle with the current II (currentII)
// the modulo we do it using the remainder op %
bool SlotTable::can_schedule(int actual_cycle, InstructionKind instr_kind) const {
    int target_row = actual_cycle % currentII; // Get the target row based on the actual cycle and the current II
    const HardwareResources& resources = table[target_row]; // Get the hardware resources for the target row

    switch (instr_kind) {
        case InstructionKind::Add:
        case InstructionKind::Addi:
        case InstructionKind::Sub:
        case InstructionKind::Mov:
            return !resources.alu0_used || !resources.alu1_used; // Can schedule if either ALU0 or ALU1 is available
        case InstructionKind::Mulu:
            return !resources.mul_used; // Can schedule if the MUL unit is available
        case InstructionKind::Ld:
        case InstructionKind::St:
            return !resources.mem_used; // Can schedule if the MEM unit is available
        case InstructionKind::Loop:
        case InstructionKind::LoopPip:
            return !resources.branch_used; // Can schedule if the BRANCH unit is available
        case InstructionKind::Nop:
            return true; // NOP can always be scheduled
        default:
            return false; // For unknown instruction types, we cannot schedule
    }
}

void SlotTable::reserve_resources(int actual_cycle, InstructionKind instr_kind) {
    int target_row = actual_cycle % currentII; // Get the target row based on the actual cycle and the current II
    HardwareResources& resources = table[target_row]; // Get the hardware resources for the target row

    switch (instr_kind) {
        case InstructionKind::Add:
        case InstructionKind::Addi:
        case InstructionKind::Sub:
        case InstructionKind::Mov:
            if (!resources.alu0_used) {
                resources.alu0_used = true; // Reserve ALU0 if it's available
            } else {
                resources.alu1_used = true; // Otherwise, reserve ALU1
            }
            break;
        case InstructionKind::Mulu:
            resources.mul_used = true; // Reserve the MUL unit
            break;
        case InstructionKind::Ld:
        case InstructionKind::St:
            resources.mem_used = true; // Reserve the MEM unit
            break;
        case InstructionKind::Loop:
        case InstructionKind::LoopPip:
            resources.branch_used = true; // Reserve the BRANCH unit
            break;
        case InstructionKind::Nop:
            // NOP doesn't use any resources, so we don't need to do anything
            break;
        default:
            break; // For unknown instruction types, do nothing
    }
}

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

// Places instruntion in the bundle and returns true / false depending on if there was space or not
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
        // these first two check are just sanity checks (to make sure data is valid)
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

    for (const int id : bb1_ids) { // look at all the instr in BB1 (since only they can have interloop dependencies with other BB1 instructions)
        // Sanity checks to ensure we have valid data before checking constraints
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
            break; // so if it is possible to place in the bundle, we break and get out of the while loop
        }
        ++cycle; // if is not possible, we try to place the instruction in the next cycle (we keep increasing the cycle until we find a cycle where we can place the instruction)
    }

    // we place the instruction in the bundle and we update the scheduled cycle for that instruction id
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

std::string rebuild_instruction_text(const Instruction& instr, int new_dest, const std::vector<int>& new_sources) {
    std::string text;

    // add predicate prefix if present, e.g. "(p32) "
    if (!instr.predicate_register.empty()) {
        text += "(" + instr.predicate_register + ") ";
    }

    text += instr.opcode;

    // nop has no operands
    if (instr.kind == InstructionKind::Nop) {
        return text;
    }

    // loop/loop.pip: the target address gets set separately by the caller
    if (instr.kind == InstructionKind::Loop || instr.kind == InstructionKind::LoopPip) {
        text += " " + std::to_string(instr.loop_target);
        return text;
    }

    // mov to LC or EC: nothing changes, keep original text
    if (instr.kind == InstructionKind::Mov && instr.destination_register == -1) {
        return instr.raw_text;
    }

    // extracts the last comma-separated token from the original text
    // used to preserve hex formatting of immediates like 0x1000
    auto extract_original_immediate = [](const std::string& raw) -> std::string {
        auto pos = raw.rfind(',');
        if (pos == std::string::npos) return "";
        std::string token = raw.substr(pos + 1);
        size_t start = token.find_first_not_of(" \t");
        size_t end = token.find_last_not_of(" \t");
        if (start == std::string::npos) return "";
        return token.substr(start, end - start + 1);
    };

    // extracts the offset part from a memory operand like "0x1000(x2)"
    // we need this to preserve hex formatting of offsets
    auto extract_original_offset = [](const std::string& raw) -> std::string {
        auto paren = raw.rfind('(');
        if (paren == std::string::npos || paren == 0) return "0";
        size_t end = paren;
        size_t start = end - 1;
        while (start > 0 && raw[start] == ' ') start--;
        size_t comma = raw.rfind(',', start);
        size_t space = raw.rfind(' ', start);
        size_t tok_start = 0;
        if (comma != std::string::npos) tok_start = comma + 1;
        if (space != std::string::npos && space > tok_start) tok_start = space + 1;
        while (tok_start < end && raw[tok_start] == ' ') tok_start++;
        return raw.substr(tok_start, end - tok_start);
    };

    // destination register
    if (new_dest != -1) {
        text += " x" + std::to_string(new_dest);
    }

    int src_idx = 0;

    // mov dest, source_or_immediate
    if (instr.kind == InstructionKind::Mov) {
        if (instr.has_immediate) {
            text += ", " + extract_original_immediate(instr.raw_text);
        } else if (src_idx < static_cast<int>(new_sources.size())) {
            text += ", x" + std::to_string(new_sources[src_idx]);
            src_idx++;
        }
        return text;
    }

    // st source, offset(base) — no destination register
    if (instr.kind == InstructionKind::St) {
        // rebuild from scratch since st has no dest
        text = instr.opcode;
        if (!instr.predicate_register.empty()) {
            text = "(" + instr.predicate_register + ") " + text;
        }
        if (src_idx < static_cast<int>(new_sources.size())) {
            text += " x" + std::to_string(new_sources[src_idx]);
            src_idx++;
        }
        if (instr.has_memory_operand && src_idx < static_cast<int>(new_sources.size())) {
            text += ", " + extract_original_offset(instr.raw_text) + "(x" + std::to_string(new_sources[src_idx]) + ")";
            src_idx++;
        }
        return text;
    }

    // ld dest, offset(base)
    if (instr.kind == InstructionKind::Ld) {
        if (instr.has_memory_operand && src_idx < static_cast<int>(new_sources.size())) {
            text += ", " + extract_original_offset(instr.raw_text) + "(x" + std::to_string(new_sources[src_idx]) + ")";
            src_idx++;
        }
        return text;
    }

    // arithmetic: add, addi, sub, mulu — dest, src1, src2_or_imm
    if (src_idx < static_cast<int>(new_sources.size())) {
        text += ", x" + std::to_string(new_sources[src_idx]);
        src_idx++;
    }
    if (instr.has_immediate) {
        text += ", " + extract_original_immediate(instr.raw_text);
    } else if (src_idx < static_cast<int>(new_sources.size())) {
        text += ", x" + std::to_string(new_sources[src_idx]);
        src_idx++;
    }

    return text;
}