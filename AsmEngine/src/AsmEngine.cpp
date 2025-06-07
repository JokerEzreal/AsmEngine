// Enhanced AssemblyEngine.cpp using AsmJit
#include "AssemblyEngine.h"
#include <regex>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <map>

namespace AsmEngine {

    using namespace asmjit;

    AssemblyEngine::AssemblyEngine(SymbolManager* symbolManager,
        CaptureStorage* captureStorage)
        : symbolManager_(symbolManager), captureStorage_(captureStorage) {

        // Initialize AsmJit runtime
        runtime_ = std::make_unique<JitRuntime>();
    }

    AssemblyEngine::~AssemblyEngine() = default;

    // Enhanced assembly method that handles labels and multiple instructions
    std::optional<AssembledCode> AssemblyEngine::Assemble(const std::string& assembly,
        AddressType baseAddress) {

        AssembledCode result;

        // Create code holder and assembler
        CodeHolder code;
        code.init(runtime_->environment(), baseAddress);

        x86::Assembler assembler(&code);

        // Label management
        std::map<std::string, Label> labelMap;
        std::map<std::string, std::vector<size_t>> unresolvedJumps;
        std::vector<std::pair<size_t, std::string>> jumpFixups;

        // Parse assembly line by line
        std::istringstream stream(assembly);
        std::string line;

        while (std::getline(stream, line)) {
            // Trim whitespace
            line.erase(0, line.find_first_not_of(" \t"));
            line.erase(line.find_last_not_of(" \t") + 1);

            // Skip empty lines and comments
            if (line.empty() || line[0] == ';') {
                continue;
            }

            // Check for labels
            if (line.back() == ':') {
                std::string labelName = line.substr(0, line.length() - 1);

                // Create or get label
                if (labelMap.find(labelName) == labelMap.end()) {
                    labelMap[labelName] = assembler.newLabel();
                }

                // Bind label to current position
                assembler.bind(labelMap[labelName]);

                // Store label address
                result.labels[labelName] = baseAddress + assembler.offset();

                continue;
            }

            // Preprocess the line
            std::string processed = PreprocessAssembly(line, baseAddress + assembler.offset());

            // Assemble the instruction
            if (!AssembleInstructionAsmJit(assembler, processed, labelMap, jumpFixups)) {
                std::cout << "[ERROR] Failed to assemble: " << line << std::endl;
                std::cout << "[DEBUG] Processed: " << processed << std::endl;
            }
        }

        // Finalize the code
        Error err = code.flatten();
        if (err) {
            std::cout << "[ERROR] Failed to flatten code: " << DebugUtils::errorAsString(err) << std::endl;
            return std::nullopt;
        }

        err = code.resolveUnresolvedLinks();
        if (err) {
            std::cout << "[ERROR] Failed to resolve links: " << DebugUtils::errorAsString(err) << std::endl;
            return std::nullopt;
        }

        // Get the assembled code
        CodeBuffer& buffer = code.sectionById(0)->buffer();
        if (buffer.size() == 0) {
            return std::nullopt;
        }

        result.machineCode.assign(buffer.data(), buffer.data() + buffer.size());
        result.codeSize = buffer.size();

        return result;
    }

    // Enhanced instruction assembler using AsmJit
    bool AssemblyEngine::AssembleInstructionAsmJit(x86::Assembler& assembler,
        const std::string& instruction,
        std::map<std::string, Label>& labelMap,
        std::vector<std::pair<size_t, std::string>>& jumpFixups) {

        // Parse instruction
        std::istringstream iss(instruction);
        std::string mnemonic;
        iss >> mnemonic;

        // Convert to lowercase
        std::transform(mnemonic.begin(), mnemonic.end(), mnemonic.begin(), ::tolower);

        // Get operands
        std::string operandsStr;
        std::getline(iss, operandsStr);

        // Parse operands
        std::vector<std::string> operands;
        if (!operandsStr.empty()) {
            operandsStr.erase(0, operandsStr.find_first_not_of(" \t"));

            std::string current;
            bool inBrackets = false;

            for (char c : operandsStr) {
                if (c == '[') inBrackets = true;
                else if (c == ']') inBrackets = false;

                if (c == ',' && !inBrackets) {
                    if (!current.empty()) {
                        // Trim
                        current.erase(0, current.find_first_not_of(" \t"));
                        current.erase(current.find_last_not_of(" \t") + 1);
                        operands.push_back(current);
                        current.clear();
                    }
                }
                else {
                    current += c;
                }
            }

            if (!current.empty()) {
                current.erase(0, current.find_first_not_of(" \t"));
                current.erase(current.find_last_not_of(" \t") + 1);
                operands.push_back(current);
            }
        }

        // Handle different instructions
        if (mnemonic == "push") {
            return HandlePush(assembler, operands);
        }
        else if (mnemonic == "pop") {
            return HandlePop(assembler, operands);
        }
        else if (mnemonic == "mov") {
            return HandleMov(assembler, operands);
        }
        else if (mnemonic == "movss") {
            return HandleMovss(assembler, operands);
        }
        else if (mnemonic == "lea") {
            return HandleLea(assembler, operands);
        }
        else if (mnemonic == "add") {
            return HandleAdd(assembler, operands);
        }
        else if (mnemonic == "sub") {
            return HandleSub(assembler, operands);
        }
        else if (mnemonic == "test") {
            return HandleTest(assembler, operands);
        }
        else if (mnemonic == "cmp") {
            return HandleCmp(assembler, operands);
        }
        else if (mnemonic == "jmp") {
            return HandleJmp(assembler, operands, labelMap);
        }
        else if (mnemonic == "call") {
            return HandleCall(assembler, operands, labelMap);
        }
        else if (mnemonic == "je" || mnemonic == "jz") {
            return HandleJcc(assembler, x86::kCondE, operands, labelMap);
        }
        else if (mnemonic == "jne" || mnemonic == "jnz") {
            return HandleJcc(assembler, x86::kCondNE, operands, labelMap);
        }
        else if (mnemonic == "jg") {
            return HandleJcc(assembler, x86::kCondG, operands, labelMap);
        }
        else if (mnemonic == "jge") {
            return HandleJcc(assembler, x86::kCondGE, operands, labelMap);
        }
        else if (mnemonic == "jl") {
            return HandleJcc(assembler, x86::kCondL, operands, labelMap);
        }
        else if (mnemonic == "jle") {
            return HandleJcc(assembler, x86::kCondLE, operands, labelMap);
        }
        else if (mnemonic == "ja") {
            return HandleJcc(assembler, x86::kCondA, operands, labelMap);
        }
        else if (mnemonic == "jae") {
            return HandleJcc(assembler, x86::kCondAE, operands, labelMap);
        }
        else if (mnemonic == "jb") {
            return HandleJcc(assembler, x86::kCondB, operands, labelMap);
        }
        else if (mnemonic == "jbe") {
            return HandleJcc(assembler, x86::kCondBE, operands, labelMap);
        }
        else if (mnemonic == "nop") {
            assembler.nop();
            return true;
        }
        else if (mnemonic == "ret") {
            assembler.ret();
            return true;
        }

        return false;
    }

    // Parse register
    x86::Gp AssemblyEngine::ParseRegister(const std::string& str) {
        using namespace asmjit::x86;

        // 64-bit registers
        if (str == "rax") return rax;
        if (str == "rbx") return rbx;
        if (str == "rcx") return rcx;
        if (str == "rdx") return rdx;
        if (str == "rsi") return rsi;
        if (str == "rdi") return rdi;
        if (str == "rbp") return rbp;
        if (str == "rsp") return rsp;
        if (str == "r8") return r8;
        if (str == "r9") return r9;
        if (str == "r10") return r10;
        if (str == "r11") return r11;
        if (str == "r12") return r12;
        if (str == "r13") return r13;
        if (str == "r14") return r14;
        if (str == "r15") return r15;

        // 32-bit registers
        if (str == "eax") return eax;
        if (str == "ebx") return ebx;
        if (str == "ecx") return ecx;
        if (str == "edx") return edx;
        if (str == "esi") return esi;
        if (str == "edi") return edi;
        if (str == "ebp") return ebp;
        if (str == "esp") return esp;

        // 16-bit registers
        if (str == "ax") return ax;
        if (str == "bx") return bx;
        if (str == "cx") return cx;
        if (str == "dx") return dx;

        // 8-bit registers
        if (str == "al") return al;
        if (str == "bl") return bl;
        if (str == "cl") return cl;
        if (str == "dl") return dl;

        return x86::Gp(); // Invalid
    }

    // Parse XMM register
    x86::Xmm AssemblyEngine::ParseXmmRegister(const std::string& str) {
        using namespace asmjit::x86;

        if (str == "xmm0") return xmm0;
        if (str == "xmm1") return xmm1;
        if (str == "xmm2") return xmm2;
        if (str == "xmm3") return xmm3;
        if (str == "xmm4") return xmm4;
        if (str == "xmm5") return xmm5;
        if (str == "xmm6") return xmm6;
        if (str == "xmm7") return xmm7;

        return x86::Xmm(); // Invalid
    }

    // Parse memory operand
    x86::Mem AssemblyEngine::ParseMemory(const std::string& str) {
        // Remove brackets
        std::string expr = str.substr(1, str.length() - 2);

        x86::Gp base;
        x86::Gp index;
        uint32_t scale = 1;
        int32_t disp = 0;
        bool hasBase = false;
        bool hasIndex = false;

        // Simple parser for [base+index*scale+disp] format
        std::regex memRegex(R"(([a-zA-Z0-9]+)?(?:\s*\+\s*([a-zA-Z0-9]+)(?:\s*\*\s*(\d+))?)?(?:\s*([+-])\s*0x([0-9A-Fa-f]+))?(?:\s*([+-])\s*(\d+))?)");
        std::smatch match;

        // For simple absolute addresses like [0x12345678]
        if (expr.find_first_of("+-*") == std::string::npos && !ParseRegister(expr).isValid()) {
            // Pure address
            uint64_t addr = ParseImmediate(expr);
            return x86::ptr(addr);
        }

        // Parse complex expressions
        std::string token;
        bool expectOp = false;
        char lastOp = '+';

        for (size_t i = 0; i < expr.length(); i++) {
            char c = expr[i];

            if (c == ' ' || c == '\t') continue;

            if (c == '+' || c == '-' || c == '*') {
                if (!token.empty()) {
                    // Process token
                    if (ParseRegister(token).isValid()) {
                        if (!hasBase) {
                            base = ParseRegister(token);
                            hasBase = true;
                        }
                        else {
                            index = ParseRegister(token);
                            hasIndex = true;
                        }
                    }
                    else {
                        // It's a displacement
                        int32_t val = ParseImmediate(token);
                        if (lastOp == '-') val = -val;
                        disp += val;
                    }
                    token.clear();
                }
                lastOp = c;
                expectOp = false;
            }
            else {
                token += c;
            }
        }

        // Process last token
        if (!token.empty()) {
            if (ParseRegister(token).isValid()) {
                if (!hasBase) {
                    base = ParseRegister(token);
                    hasBase = true;
                }
                else {
                    index = ParseRegister(token);
                    hasIndex = true;
                }
            }
            else {
                int32_t val = ParseImmediate(token);
                if (lastOp == '-') val = -val;
                disp += val;
            }
        }

        // Create memory operand
        if (hasBase && hasIndex) {
            return x86::ptr(base, index, scale, disp);
        }
        else if (hasBase) {
            return x86::ptr(base, disp);
        }
        else {
            return x86::ptr(disp);
        }
    }

    // Parse immediate value
    uint64_t AssemblyEngine::ParseImmediate(const std::string& str) {
        if (str.empty()) return 0;

        // Handle hex
        if (str.size() > 2 && str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
            return std::stoull(str, nullptr, 16);
        }

        // Default to decimal
        return std::stoull(str, nullptr, 10);
    }

    // Instruction handlers
    bool AssemblyEngine::HandleMov(x86::Assembler& assembler, const std::vector<std::string>& operands) {
        if (operands.size() != 2) return false;

        const std::string& dst = operands[0];
        const std::string& src = operands[1];

        // reg, reg
        x86::Gp dstReg = ParseRegister(dst);
        x86::Gp srcReg = ParseRegister(src);

        if (dstReg.isValid() && srcReg.isValid()) {
            assembler.mov(dstReg, srcReg);
            return true;
        }

        // reg, imm
        if (dstReg.isValid() && !srcReg.isValid() && src[0] != '[') {
            uint64_t imm = ParseImmediate(src);
            assembler.mov(dstReg, imm);
            return true;
        }

        // reg, mem
        if (dstReg.isValid() && src[0] == '[') {
            assembler.mov(dstReg, ParseMemory(src));
            return true;
        }

        // mem, reg
        if (dst[0] == '[' && srcReg.isValid()) {
            assembler.mov(ParseMemory(dst), srcReg);
            return true;
        }

        // mem, imm - special handling
        if (dst[0] == '[' && !srcReg.isValid() && src[0] != '[') {
            x86::Mem mem = ParseMemory(dst);
            uint64_t imm = ParseImmediate(src);

            // Determine size based on immediate value
            if (imm <= 0xFFFFFFFF) {
                mem.setSize(4); // dword
                assembler.mov(mem, static_cast<uint32_t>(imm));
            }
            else {
                // For 64-bit immediates, we need to use a temporary register
                assembler.push(x86::rax);
                assembler.mov(x86::rax, imm);
                assembler.mov(mem, x86::rax);
                assembler.pop(x86::rax);
            }
            return true;
        }

        return false;
    }

    bool AssemblyEngine::HandleMovss(x86::Assembler& assembler, const std::vector<std::string>& operands) {
        if (operands.size() != 2) return false;

        const std::string& dst = operands[0];
        const std::string& src = operands[1];

        x86::Xmm dstXmm = ParseXmmRegister(dst);
        x86::Xmm srcXmm = ParseXmmRegister(src);

        // xmm, xmm
        if (dstXmm.isValid() && srcXmm.isValid()) {
            assembler.movss(dstXmm, srcXmm);
            return true;
        }

        // xmm, mem
        if (dstXmm.isValid() && src[0] == '[') {
            assembler.movss(dstXmm, ParseMemory(src));
            return true;
        }

        // mem, xmm
        if (dst[0] == '[' && srcXmm.isValid()) {
            assembler.movss(ParseMemory(dst), srcXmm);
            return true;
        }

        return false;
    }

    bool AssemblyEngine::HandleCmp(x86::Assembler& assembler, const std::vector<std::string>& operands) {
        if (operands.size() != 2) return false;

        const std::string& op1 = operands[0];
        const std::string& op2 = operands[1];

        x86::Gp reg1 = ParseRegister(op1);
        x86::Gp reg2 = ParseRegister(op2);

        // reg, reg
        if (reg1.isValid() && reg2.isValid()) {
            assembler.cmp(reg1, reg2);
            return true;
        }

        // reg, imm
        if (reg1.isValid() && !reg2.isValid() && op2[0] != '[') {
            uint64_t imm = ParseImmediate(op2);
            assembler.cmp(reg1, imm);
            return true;
        }

        // reg, mem
        if (reg1.isValid() && op2[0] == '[') {
            assembler.cmp(reg1, ParseMemory(op2));
            return true;
        }

        // mem, reg
        if (op1[0] == '[' && reg2.isValid()) {
            assembler.cmp(ParseMemory(op1), reg2);
            return true;
        }

        // mem, imm
        if (op1[0] == '[' && !reg2.isValid() && op2[0] != '[') {
            x86::Mem mem = ParseMemory(op1);
            uint64_t imm = ParseImmediate(op2);

            // Set appropriate size
            if (imm <= 0xFF) {
                mem.setSize(1);
                assembler.cmp(mem, static_cast<uint8_t>(imm));
            }
            else if (imm <= 0xFFFF) {
                mem.setSize(2);
                assembler.cmp(mem, static_cast<uint16_t>(imm));
            }
            else {
                mem.setSize(4);
                assembler.cmp(mem, static_cast<uint32_t>(imm));
            }
            return true;
        }

        return false;
    }

    bool AssemblyEngine::HandleJmp(x86::Assembler& assembler,
        const std::vector<std::string>& operands,
        std::map<std::string, Label>& labelMap) {
        if (operands.size() != 1) return false;

        const std::string& target = operands[0];

        // Check if it's a register
        x86::Gp reg = ParseRegister(target);
        if (reg.isValid()) {
            assembler.jmp(reg);
            return true;
        }

        // Check if it's an immediate address
        if (target[0] == '0' && target[1] == 'x') {
            uint64_t addr = ParseImmediate(target);
            assembler.jmp(addr);
            return true;
        }

        // It's a label
        if (labelMap.find(target) == labelMap.end()) {
            labelMap[target] = assembler.newLabel();
        }
        assembler.jmp(labelMap[target]);
        return true;
    }

    bool AssemblyEngine::HandleJcc(x86::Assembler& assembler,
        asmjit::CondCode cond,
        const std::vector<std::string>& operands,
        std::map<std::string, Label>& labelMap) {
        if (operands.size() != 1) return false;

        const std::string& target = operands[0];

        // It's a label
        if (labelMap.find(target) == labelMap.end()) {
            labelMap[target] = assembler.newLabel();
        }

        // Use generic conditional jump
        assembler.j(cond, labelMap[target]);
        return true;
    }

    bool AssemblyEngine::HandleCall(x86::Assembler& assembler,
        const std::vector<std::string>& operands,
        std::map<std::string, Label>& labelMap) {
        if (operands.size() != 1) return false;

        const std::string& target = operands[0];

        // Check if it's a register
        x86::Gp reg = ParseRegister(target);
        if (reg.isValid()) {
            assembler.call(reg);
            return true;
        }

        // Check if it's an immediate address
        if (target[0] == '0' && target[1] == 'x') {
            uint64_t addr = ParseImmediate(target);
            assembler.call(addr);
            return true;
        }

        // It's a label
        if (labelMap.find(target) == labelMap.end()) {
            labelMap[target] = assembler.newLabel();
        }
        assembler.call(labelMap[target]);
        return true;
    }

    bool AssemblyEngine::HandleLea(x86::Assembler& assembler, const std::vector<std::string>& operands) {
        if (operands.size() != 2) return false;

        const std::string& dst = operands[0];
        const std::string& src = operands[1];

        x86::Gp dstReg = ParseRegister(dst);

        if (dstReg.isValid() && src[0] == '[') {
            assembler.lea(dstReg, ParseMemory(src));
            return true;
        }

        return false;
    }

    bool AssemblyEngine::HandleTest(x86::Assembler& assembler, const std::vector<std::string>& operands) {
        if (operands.size() != 2) return false;

        const std::string& op1 = operands[0];
        const std::string& op2 = operands[1];

        x86::Gp reg1 = ParseRegister(op1);
        x86::Gp reg2 = ParseRegister(op2);

        if (reg1.isValid() && reg2.isValid()) {
            assembler.test(reg1, reg2);
            return true;
        }

        if (reg1.isValid() && !reg2.isValid()) {
            uint64_t imm = ParseImmediate(op2);
            assembler.test(reg1, imm);
            return true;
        }

        return false;
    }

    bool AssemblyEngine::HandlePush(x86::Assembler& assembler, const std::vector<std::string>& operands) {
        if (operands.size() != 1) return false;

        const std::string& op = operands[0];
        x86::Gp reg = ParseRegister(op);

        if (reg.isValid()) {
            assembler.push(reg);
            return true;
        }

        // Immediate
        uint64_t imm = ParseImmediate(op);
        assembler.push(Imm(imm));
        return true;
    }

    bool AssemblyEngine::HandlePop(x86::Assembler& assembler, const std::vector<std::string>& operands) {
        if (operands.size() != 1) return false;

        const std::string& op = operands[0];
        x86::Gp reg = ParseRegister(op);

        if (reg.isValid()) {
            assembler.pop(reg);
            return true;
        }

        return false;
    }

    bool AssemblyEngine::HandleAdd(x86::Assembler& assembler, const std::vector<std::string>& operands) {
        if (operands.size() != 2) return false;

        const std::string& dst = operands[0];
        const std::string& src = operands[1];

        x86::Gp dstReg = ParseRegister(dst);
        x86::Gp srcReg = ParseRegister(src);

        if (dstReg.isValid() && srcReg.isValid()) {
            assembler.add(dstReg, srcReg);
            return true;
        }

        if (dstReg.isValid() && !srcReg.isValid()) {
            uint64_t imm = ParseImmediate(src);
            assembler.add(dstReg, imm);
            return true;
        }

        return false;
    }

    bool AssemblyEngine::HandleSub(x86::Assembler& assembler, const std::vector<std::string>& operands) {
        if (operands.size() != 2) return false;

        const std::string& dst = operands[0];
        const std::string& src = operands[1];

        x86::Gp dstReg = ParseRegister(dst);
        x86::Gp srcReg = ParseRegister(src);

        if (dstReg.isValid() && srcReg.isValid()) {
            assembler.sub(dstReg, srcReg);
            return true;
        }

        if (dstReg.isValid() && !srcReg.isValid()) {
            uint64_t imm = ParseImmediate(src);
            assembler.sub(dstReg, imm);
            return true;
        }

        return false;
    }

} // namespace AsmEngine