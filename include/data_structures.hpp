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
    int currentII = 0; 

    // DECLARATIONS ONLY (Notice the semicolons at the end!)
    void init_reset(int ii);
    bool can_schedule(int actual_cycle, InstructionKind instr_kind) const;
    void reserve_resources(int actual_cycle, InstructionKind instr_kind);
};

#endif // DATA_STRUCTURES_HPP


