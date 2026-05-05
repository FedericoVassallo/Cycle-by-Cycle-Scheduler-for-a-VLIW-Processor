#ifndef IO_HANDLER_HPP
#define IO_HANDLER_HPP

#include "data_structures.hpp"
#include <string>
#include <vector>

namespace IOHandler {
    // Reads the input JSON file and returns a list of instructions as strings.
    // Throws std::runtime_error if the file cannot be opened or parsed.
    std::vector<std::string> read_instructions(const std::string& filepath);

    // Parses a single instruction line into a typed record.
    // Throws std::runtime_error if the syntax is invalid.
    Instruction parse_instruction(const std::string& instruction_text);

    // Parses a list of instruction lines into typed records.
    // Throws std::runtime_error if any instruction is invalid.
    std::vector<Instruction> parse_instructions(const std::vector<std::string>& instruction_lines);

    // Reads instruction lines from JSON input and parses them into typed records.
    // Throws std::runtime_error on file/JSON/parsing errors.
    std::vector<Instruction> read_and_parse_instructions(const std::string& filepath);

    // Writes the synthesized VLIW packets to the output JSON file.
    // Throws std::runtime_error if the file cannot be opened for writing.
    void write_packets(const std::string& filepath, const std::vector<std::vector<std::string>>& packets);
}

#endif // IO_HANDLER_HPP
