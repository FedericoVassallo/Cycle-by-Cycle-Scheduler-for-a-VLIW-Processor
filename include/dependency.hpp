#ifndef DEPENDENCY_HPP
#define DEPENDENCY_HPP
#include "data_structures.hpp"
#include "io_handler.hpp"

void dependency_analysis(const std::vector<Instruction>& instructions, std::vector<DependencyAnalysisTableEntry>& analysis_table);

#endif // DEPENDENCY_HPP