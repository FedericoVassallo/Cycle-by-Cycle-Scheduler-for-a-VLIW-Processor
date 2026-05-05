#ifndef DEPENDENCY_HPP
#define DEPENDENCY_HPP
#include "data_structures.hpp"
#include "io_handler.hpp"


// assign each instruction to BB0, BB1 or BB2
void update_instructions_bb(std::vector<Instruction>& instructions);

// build the dependency table used by scheduling and register allocation
void dependency_analysis(const std::vector<Instruction>& instructions, std::vector<DependencyAnalysisTableEntry>& analysis_table);

#endif // DEPENDENCY_HPP