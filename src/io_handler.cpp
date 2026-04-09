#include "include/io_handler.hpp"
#include "include/json.hpp"
#include <fstream>
#include <stdexcept>

using json = nlohmann::json;

namespace IOHandler {

    std::vector<std::string> read_instructions(const std::string& filepath) {
        std::ifstream file(filepath);
        if (!file.is_open()) {
            throw std::runtime_error("Could not open input file: " + filepath);
        }

        json data;
        try {
            file >> data;
        } catch (const json::parse_error& e) {
            throw std::runtime_error("JSON parse error in file " + filepath + ": " + e.what());
        }

        if (!data.is_array()) {
            throw std::runtime_error("Invalid JSON format in " + filepath + ": Expected an array of strings.");
        }

        std::vector<std::string> instructions;
        for (const auto& item : data) {
            if (!item.is_string()) {
                throw std::runtime_error("Invalid JSON format in " + filepath + ": Array elements must be strings.");
            }
            instructions.push_back(item.get<std::string>());
        }

        return instructions;
    }

    void write_packets(const std::string& filepath, const std::vector<std::vector<std::string>>& packets) {
        std::ofstream file(filepath);
        if (!file.is_open()) {
            throw std::runtime_error("Could not open output file: " + filepath);
        }

        // Implicit conversion from std::vector<std::vector<std::string>> to json
        json data = packets; 
        
        // Dump the JSON object to the specified output file with 4-space indentation
        file << data.dump(4) << std::endl;
    }

}
