#include "include/io_handler.hpp"
#include "include/json.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

using json = nlohmann::json;

namespace {

std::string trim(const std::string& s) {
    std::string::size_type start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }

    std::string::size_type end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }

    return s.substr(start, end - start);
}

std::string to_lower_copy(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

std::vector<std::string> split_comma_separated(const std::string& s) {
    std::vector<std::string> parts;
    std::stringstream ss(s);
    std::string token;
    while (std::getline(ss, token, ',')) {
        parts.push_back(trim(token));
    }
    return parts;
}

int parse_number(const std::string& token, const std::string& context) {
    const std::string normalized = to_lower_copy(trim(token));
    if (normalized.empty()) {
        throw std::runtime_error("Missing numeric value in: " + context);
    }

    try {
        size_t idx = 0;
        int value = 0;
        if (normalized.rfind("-0x", 0) == 0) {
            const int magnitude = std::stoi(normalized.substr(3), &idx, 16);
            if (idx != normalized.size() - 3) {
                throw std::runtime_error("Invalid numeric token: " + token);
            }
            value = -magnitude;
        } else if (normalized.rfind("0x", 0) == 0) {
            value = std::stoi(normalized, &idx, 16);
            if (idx != normalized.size()) {
                throw std::runtime_error("Invalid numeric token: " + token);
            }
        } else {
            value = std::stoi(normalized, &idx, 10);
            if (idx != normalized.size()) {
                throw std::runtime_error("Invalid numeric token: " + token);
            }
        }
        return value;
    } catch (const std::exception&) {
        throw std::runtime_error("Invalid numeric value '" + token + "' in: " + context);
    }
}

bool is_register_token(const std::string& token) {
    const std::string t = to_lower_copy(trim(token));
    if (t.size() < 2) {
        return false;
    }
    if (t[0] != 'x' && t[0] != 'p') {
        return false;
    }
    for (size_t i = 1; i < t.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(t[i]))) {
            return false;
        }
    }
    return true;
}

int parse_register_id(const std::string& token, const std::string& context) {
    const std::string t = to_lower_copy(trim(token));
    if (!is_register_token(t)) {
        throw std::runtime_error("Invalid register token '" + token + "' in: " + context);
    }
    return parse_number(t.substr(1), context);
}

void parse_memory_operand(const std::string& token, const std::string& context, Instruction& instruction) {
    const std::string t = trim(token);
    const std::string::size_type open_paren = t.find('(');
    const std::string::size_type close_paren = t.find(')');
    if (open_paren == std::string::npos || close_paren == std::string::npos || close_paren <= open_paren + 1) {
        throw std::runtime_error("Invalid memory operand '" + token + "' in: " + context + ". Expected offset(base)." );
    }

    const std::string offset_token = trim(t.substr(0, open_paren));
    const std::string base_token = trim(t.substr(open_paren + 1, close_paren - open_paren - 1));

    if (offset_token.empty() || base_token.empty() || close_paren != t.size() - 1) {
        throw std::runtime_error("Invalid memory operand '" + token + "' in: " + context + ". Expected offset(base)." );
    }

    instruction.has_memory_operand = true;
    instruction.memory_offset = parse_number(offset_token, context);
    instruction.memory_base_register = parse_register_id(base_token, context);
    instruction.source_registers.push_back(instruction.memory_base_register);
}

void parse_destination_register(const std::string& token, const std::string& context, Instruction& instruction) {
    if (!is_register_token(token)) {
        return;
    }
    instruction.destination_register = parse_register_id(token, context);
}

} // namespace

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

    Instruction parse_instruction(const std::string& instruction_text) {
        Instruction instruction;
        instruction.raw_text = instruction_text;

        std::string line = trim(instruction_text);
        if (line.empty()) {
            throw std::runtime_error("Instruction line is empty.");
        }

        if (line[0] == '(') {
            const std::string::size_type close_paren = line.find(')');
            if (close_paren == std::string::npos || close_paren <= 1) {
                throw std::runtime_error("Invalid predicate syntax in instruction: " + instruction_text);
            }
            instruction.predicate_register = to_lower_copy(trim(line.substr(1, close_paren - 1)));
            if (!is_register_token(instruction.predicate_register)) {
                throw std::runtime_error("Invalid predicate register in instruction: " + instruction_text);
            }
            line = trim(line.substr(close_paren + 1));
        }

        std::stringstream ss(line);
        std::string opcode;
        ss >> opcode;
        if (opcode.empty()) {
            throw std::runtime_error("Missing opcode in instruction: " + instruction_text);
        }

        opcode = to_lower_copy(opcode);
        instruction.opcode = opcode;
        const std::string remaining = trim(line.substr(opcode.size()));
        const std::vector<std::string> args = split_comma_separated(remaining);

        if (opcode == "nop") {
            instruction.kind = InstructionKind::Nop;
            if (!remaining.empty()) {
                throw std::runtime_error("nop takes no operands: " + instruction_text);
            }
            return instruction;
        }

        if (opcode == "mov") {
            instruction.kind = InstructionKind::Mov;
            if (args.size() != 2) {
                throw std::runtime_error("mov expects 2 operands: " + instruction_text);
            }

            parse_destination_register(args[0], instruction_text, instruction);

            const std::string src = to_lower_copy(args[1]);
            if (is_register_token(src)) {
                instruction.source_registers.push_back(parse_register_id(src, instruction_text));
            } else if (src == "true") {
                instruction.has_immediate = true;
                instruction.immediate_value = 1;
            } else if (src == "false") {
                instruction.has_immediate = true;
                instruction.immediate_value = 0;
            } else {
                instruction.has_immediate = true;
                instruction.immediate_value = parse_number(src, instruction_text);
            }

            return instruction;
        }

        if (opcode == "add" || opcode == "addi" || opcode == "sub" || opcode == "mulu") {
            if (opcode == "add") {
                instruction.kind = InstructionKind::Add;
            } else if (opcode == "addi") {
                instruction.kind = InstructionKind::Addi;
            } else if (opcode == "sub") {
                instruction.kind = InstructionKind::Sub;
            } else {
                instruction.kind = InstructionKind::Mulu;
            }

            if (args.size() != 3) {
                throw std::runtime_error(opcode + " expects 3 operands: " + instruction_text);
            }

            instruction.destination_register = parse_register_id(args[0], instruction_text);
            instruction.source_registers.push_back(parse_register_id(args[1], instruction_text));

            if (is_register_token(args[2])) {
                instruction.source_registers.push_back(parse_register_id(args[2], instruction_text));
            } else {
                instruction.has_immediate = true;
                instruction.immediate_value = parse_number(args[2], instruction_text);
            }

            return instruction;
        }

        if (opcode == "ld") {
            instruction.kind = InstructionKind::Ld;
            if (args.size() != 2) {
                throw std::runtime_error("ld expects 2 operands: " + instruction_text);
            }

            instruction.destination_register = parse_register_id(args[0], instruction_text);
            parse_memory_operand(args[1], instruction_text, instruction);
            return instruction;
        }

        if (opcode == "st") {
            instruction.kind = InstructionKind::St;
            if (args.size() != 2) {
                throw std::runtime_error("st expects 2 operands: " + instruction_text);
            }

            instruction.source_registers.push_back(parse_register_id(args[0], instruction_text));
            parse_memory_operand(args[1], instruction_text, instruction);
            return instruction;
        }

        if (opcode == "loop" || opcode == "loop.pip") {
            instruction.kind = (opcode == "loop") ? InstructionKind::Loop : InstructionKind::LoopPip;
            if (args.size() != 1) {
                throw std::runtime_error(opcode + " expects 1 operand: " + instruction_text);
            }
            instruction.has_loop_target = true;
            instruction.loop_target = parse_number(args[0], instruction_text);
            return instruction;
        }

        throw std::runtime_error("Unsupported opcode '" + opcode + "' in instruction: " + instruction_text);
    }

    std::vector<Instruction> parse_instructions(const std::vector<std::string>& instruction_lines) {
        std::vector<Instruction> parsed;
        parsed.reserve(instruction_lines.size());
        for (const auto& line : instruction_lines) {
            parsed.push_back(parse_instruction(line));
        }
        return parsed;
    }

    std::vector<Instruction> read_and_parse_instructions(const std::string& filepath) {
        return parse_instructions(read_instructions(filepath));
    }

    void write_packets(const std::string& filepath, const std::vector<std::vector<std::string>>& packets) {
        std::ofstream file(filepath);
        if (!file.is_open()) {
            throw std::runtime_error("Could not open output file: " + filepath);
        }

        for (size_t bundle_index = 0; bundle_index < packets.size(); ++bundle_index) {
            if (packets[bundle_index].size() != 5U) {
                throw std::runtime_error("Invalid packet width at bundle " + std::to_string(bundle_index) + ": expected 5 slots.");
            }
        }

        // Implicit conversion from std::vector<std::vector<std::string>> to json
        json data = packets; 
        
        // Dump the JSON object to the specified output file with 4-space indentation
        file << data.dump(4) << std::endl;
    }

}
