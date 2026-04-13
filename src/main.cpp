#include "data_structures.hpp"
#include "io_handler.hpp"
#include "dependency.hpp"
#include "scheduler.hpp"
#include "register_allocation.hpp"
#include <exception>
#include <iostream>

// converts our Bundle structs into the 5-slot string arrays that write_packets expects
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

    const std::string input_path = argv[1];
    const std::string loop_output = argv[2];
    const std::string looppip_output = argv[3];

    try {
        // This loads JSON instruction strings and parses them into typed records.
        std::vector<Instruction> instructions = IOHandler::read_and_parse_instructions(input_path);

        // This updates the basic block information for each instruction based on loop boundaries.
        update_instructions_bb(instructions);

        // This performs dependency analysis and fills the analysis table with dependency information.
        std::vector<DependencyAnalysisTableEntry> analysis_table;
        dependency_analysis(instructions, analysis_table);

        // basic loop: schedule BB0+BB1, allocate registers, then schedule BB2
        std::vector<Bundle> schedule_basic;
        std::vector<int> sched_cycle_basic(instructions.size(), -1);
        schedule_ASAP_basic(analysis_table, schedule_basic, instructions, sched_cycle_basic);
        AllocResult alloc_result = alloc_b(schedule_basic, analysis_table, instructions, sched_cycle_basic);
        schedule_bb2(analysis_table, schedule_basic, instructions, sched_cycle_basic);
        rewrite_bb2_bundles(schedule_basic, instructions, sched_cycle_basic, alloc_result);
        IOHandler::write_packets(loop_output, bundles_to_packets(schedule_basic));

        // loop.pip: placeholder for now, writes empty output
        std::vector<std::vector<std::string>> empty_looppip;
        IOHandler::write_packets(looppip_output, empty_looppip);

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}