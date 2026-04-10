#ifndef DATA_STRUCTURES_HPP
#define DATA_STRUCTURES_HPP

#include <string>
#include <vector>

enum class InstructionKind {
    Unknown,
    Nop,
    Mov,
    Add,
    Addi,
    Sub,
    Mulu,
    Ld,
    St,
    Loop,
    LoopPip
};

enum class BasicBlock {
    BB0,
    BB1,
    BB2,
};

struct Bundle {
    std::string ALU0 = "nop";
    std::string ALU1 = "nop";
    std::string MUL = "nop";
    std::string MEM = "nop";
    std::string BRANCH = "nop";
};

struct Instruction {
    std::string raw_text;
    std::string predicate_register;
    std::string opcode;
    InstructionKind kind = InstructionKind::Unknown;

    int destination_register = -1;
    std::vector<int> source_registers;

    bool has_immediate = false;
    int immediate_value = 0;

    bool has_memory_operand = false;
    int memory_offset = 0;
    int memory_base_register = -1;

    bool has_loop_target = false;
    int loop_target = -1;

    BasicBlock basic_block = BasicBlock::BB0;
};

struct DependencyAnalysisTableEntry {
    int address = -1;
    int id = -1;
    InstructionKind instruction_type = InstructionKind::Unknown;
    int destination_register = -1;
    std::vector<int> local_dependencies;
    std::vector<int> interloop_dependencies;
    std::vector<int> loop_invariant_dependencies;
    std::vector<int> post_loop_dependencies;
};

// it rappresent the [j] entry of the S[s][i][j] of section 3.2.2 

struct HardwareResources {
    bool alu0_used = false; // Indicates if ALU0 is used in the current bundle
    bool alu1_used = false; // Indicates if ALU1 is used in the current bundle
    bool mul_used = false;  // Indicates if the MUL unit is used in the current bundle
    bool mem_used = false;  // Indicates if the MEM unit is used in the current bundle
    bool branch_used = false; // Indicates if the BRANCH unit is used in the current bundle
};

struct SlotTable {

    std::vector<HardwareResources> table;

    int currentII = 0; // The current Initiation Interval (II) being evaluated

    // since the scheduling might fail for a given II, we need to be able to set a new II and reset the table for the new scheduling attempt
    void init_reset(int ii) {
        currentII = ii;
        table.clear();
        table.resize(ii); // Initialize the table with 'ii' entries, each representing a slot for one iteration
    }

    // we check if we can schedule an instruction in the current slot (which is determined by the current iteration and the current II) by checking the hardware resources used in that slot
    // we get as input the kind of the instruction we want to schedule and we check if the corresponding hardware resource is available in the current slot
    // to get the current slot we get it from the actual cycle we are trying to schedule the instruction by doing the modulo of the current cycle with the current II (currentII)
    // the modulo we do it using the remainder op %
    bool can_schedule (int actual_cycle, InstructionKind instr_kind) {
        int target_row = actual_cycle % currentII; // Get the target row based on the actual cycle and the current II
        HardwareResources& resources = table[target_row]; // Get the hardware resources for the target row

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

    void reserve_resources(int actual_cycle, InstructionKind instr_kind) {
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
};

#endif // DATA_STRUCTURES_HPP


