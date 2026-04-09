#ifndef IO_HANDLER_HPP
#define IO_HANDLER_HPP

#include <string>
#include <vector>

namespace IOHandler {
    // Reads the input JSON file and returns a list of instructions as strings.
    // Throws std::runtime_error if the file cannot be opened or parsed.
    std::vector<std::string> read_instructions(const std::string& filepath);

    // Writes the synthesized VLIW packets to the output JSON file.
    // Throws std::runtime_error if the file cannot be opened for writing.
    void write_packets(const std::string& filepath, const std::vector<std::vector<std::string>>& packets);
}

#endif // IO_HANDLER_HPP
