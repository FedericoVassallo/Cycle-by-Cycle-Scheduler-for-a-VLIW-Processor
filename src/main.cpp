#include "data_structures.hpp"
#include "io_handler.hpp"
#include "dependency.hpp"
#include "scheduler.hpp"
#include "register_allocation.hpp"
#include <exception>
#include <iostream>

// convert bundles into the 5-slot packet format expected by write_packets
// slot order is fixed: ALU0, ALU1, MUL, MEM, BRANCH
std::vector<std::vector<std::string>> bundles_to_packets(const std::vector<Bundle>& schedule) {
    std::vector<std::vector<std::string>> packets;
    for (const auto& b : schedule) {
        packets.push_back({b.ALU0, b.ALU1, b.MUL, b.MEM, b.BRANCH});
    }
    return packets;
}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <input.json> <loop.json> <looppip.json>" << std::endl;
        return 1;
    }

    const std::string input_path = argv[1];      // input json
    const std::string loop_output = argv[2];     // output for normal loop schedule
    const std::string looppip_output = argv[3];  // output for loop.pip schedule

    try {
        // read and parse instructions
        std::vector<Instruction> instructions = IOHandler::read_and_parse_instructions(input_path);

        // tag each instruction as BB0 / BB1 / BB2
        update_instructions_bb(instructions);

        // build dependency table used by both schedulers
        std::vector<DependencyAnalysisTableEntry> analysis_table;
        dependency_analysis(instructions, analysis_table);

        // basic loop: schedule BB0+BB1, allocate registers, then schedule BB2
        std::vector<Bundle> schedule_basic;
        std::vector<int> sched_cycle_basic(instructions.size(), -1);
        schedule_ASAP_basic(analysis_table, schedule_basic, instructions, sched_cycle_basic);
        AllocResult alloc_result = alloc_b(schedule_basic, analysis_table, instructions, sched_cycle_basic);
        // BB2 is scheduled after allocation, then rewritten with the allocated regs
        schedule_bb2(analysis_table, schedule_basic, instructions, sched_cycle_basic);
        rewrite_bb2_bundles(schedule_basic, instructions, sched_cycle_basic, alloc_result);
        IOHandler::write_packets(loop_output, bundles_to_packets(schedule_basic)); // write normal schedule

        // if there is no loop/loop.pip instruction, advanced scheduling is skipped
        bool has_loop = false;
        for (const auto& instr : instructions) {
            if (instr.kind == InstructionKind::Loop || instr.kind == InstructionKind::LoopPip) {
                has_loop = true;
                break;
            }
        }

        // build loop.pip output
        if (has_loop) {
            std::vector<Bundle> schedule_adv;
            std::vector<int> sched_cycle_adv(instructions.size(), -1);
            int ii_adv, loop_begin_adv, num_stages_adv;
            std::vector<int> stage_by_id_adv;
            schedule_ASAP_advanced(analysis_table, schedule_adv, instructions, sched_cycle_adv,
                                   ii_adv, loop_begin_adv, num_stages_adv, stage_by_id_adv);
            AllocResult alloc_r_result = alloc_r(schedule_adv, analysis_table, instructions,
                                                  sched_cycle_adv, ii_adv, loop_begin_adv,
                                                  num_stages_adv, stage_by_id_adv);
            schedule_bb2(analysis_table, schedule_adv, instructions, sched_cycle_adv);
            rewrite_bb2_bundles(schedule_adv, instructions, sched_cycle_adv, alloc_r_result);
            IOHandler::write_packets(looppip_output, bundles_to_packets(schedule_adv));
        } else {
            // if there is no loop, just reuse the basic schedule for looppip output too
            std::vector<Bundle> schedule_noloop;
            std::vector<int> sched_cycle_noloop(instructions.size(), -1);
            schedule_ASAP_basic(analysis_table, schedule_noloop, instructions, sched_cycle_noloop);
            AllocResult alloc_noloop = alloc_b(schedule_noloop, analysis_table, instructions, sched_cycle_noloop);
            schedule_bb2(analysis_table, schedule_noloop, instructions, sched_cycle_noloop);
            rewrite_bb2_bundles(schedule_noloop, instructions, sched_cycle_noloop, alloc_noloop);
            IOHandler::write_packets(looppip_output, bundles_to_packets(schedule_noloop));
        }

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}