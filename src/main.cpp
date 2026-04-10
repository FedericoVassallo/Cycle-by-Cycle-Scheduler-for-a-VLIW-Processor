#include "include/state.hpp"
#include "include/io_handler.hpp"
#include <exception>
#include <iostream>


int main(int argc, char* argv[]) {
	if (argc < 2) {
		std::cerr << "Usage: " << argv[0] << " <input.json>" << std::endl;
		return 1;
	}

	const std::string input_path = argv[1];

	try {
		// This loads JSON instruction strings and parses them into typed records.
		std::vector<Instruction> instructions = IOHandler::read_and_parse_instructions(input_path);

		std::cout << "Loaded " << instructions.size() << " instructions from " << input_path << std::endl;

		// Scheduler logic can now use `instructions`.
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "Error: " << e.what() << std::endl;
		return 1;
	}
}