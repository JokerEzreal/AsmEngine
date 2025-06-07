#include "AssemblyEngine.h"
#include <regex>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cctype>

namespace AsmEngine {

    using namespace asmjit;
    using namespace asmjit::x86;

    // AssemblyContext implementation
    AssemblyContext::AssemblyContext(JitRuntime* runtime, AddressType base)
        : assembler(&code), baseAddress(base) {

        code.init(runtime->environment());

        // Set base address if provided
        if (base != 0) {
            code.setBaseAddress(base);
        }
    }

    Label AssemblyContext::GetOrCreateLabel(const std::string& name) {
        auto it = labels.find(name);
        if (it != labels.end()) {
            return it->second;
        }

        Label label = assembler.newLabel();
        labels[name] = label;
        return label;
    }

    void AssemblyContext::BindLabel(const std::string& name) {
        Label label = GetOrCreateLabel(name);
        assembler.bind(label);

        // Store resolved address
        resolvedAddresses[name] = baseAddress + assembler.offset();
    }

    Imm AssemblyContext::ResolveImmediate(const std::string& value) {
        // Parse hex, decimal, or symbol
        if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
            // Hex value
            uint64_t val = std::stoull(value, nullptr, 16);
            return imm(val);
        }
        else if (std::all_of(value.begin(), value.end(), ::isdigit)) {
            // Decimal value
            uint64_t val = std::stoull(value);
            return imm(val);
        }

        // Could be a symbol - return 0 for now
        return imm(0);
    }

    // AssemblyEngine implementation
    AssemblyEngine::AssemblyEngine(SymbolManager* symbolManager, CaptureStorage* captureStorage)
        : symbolManager_(symbolManager), captureStorage_(captureStorage) {

        runtime_ = std::make_unique<JitRuntime>();
    }

    AssemblyEngine::~AssemblyEngine() = default;

    std::optional<AssembledCode> AssemblyEngine::Assemble(const std::string& assembly, AddressType address) {
        AssemblyContext ctx(runtime_.get(), address);

        std::istringstream stream(assembly);
        std::string line;

        std::cout << "[DEBUG] Starting assembly at address 0x" << std::hex << address << std::dec << std::endl;

        while (std::getline(stream, line)) {
            // Trim whitespace
            line.erase(0, line.find_first_not_of(" \t"));
            line.erase(line.find_last_not_of(" \t") + 1);

            if (line.empty() || line[0] == ';' || (line.size() >= 2 && line[0] == '/' && line[1] == '/')) {
                continue;
            }

            // Check for label definition
            if (line.back() == ':') {
                std::string labelName = line.substr(0, line.length() - 1);
                ctx.BindLabel(labelName);

                std::cout << "[DEBUG] Bound label '" << labelName << "' at offset "
                    << ctx.assembler.offset() << std::endl;

                // Register in symbol manager if available
                if (symbolManager_) {
                    symbolManager_->RegisterLabel(labelName,
                        address + ctx.assembler.offset());
                }
                continue;
            }

            // Preprocess and assemble instruction
            std::string processed = PreprocessLine(line, ctx);

            std::cout << "[DEBUG] Assembling: " << processed << std::endl;

            if (!ParseAndAssembleInstruction(ctx, processed)) {
                std::cerr << "[ERROR] Failed to assemble: " << line << std::endl;
                return std::nullopt;
            }
        }

        // Finalize code
        Error err = ctx.code.flatten();
        if (err) {
            std::cerr << "[ERROR] Failed to flatten code" << std::endl;
            return std::nullopt;
        }

        err = ctx.code.resolveUnresolvedLinks();
        if (err) {
            std::cerr << "[ERROR] Failed to resolve links" << std::endl;
            return std::nullopt;
        }

        // Extract machine code
        AssembledCode result;
        CodeBuffer& buffer = ctx.code.sectionById(0)->buffer();
        result.machineCode.assign(buffer.data(), buffer.data() + buffer.size());
        result.codeSize = buffer.size();
        result.labels = ctx.resolvedAddresses;

        std::cout << "[DEBUG] Assembly complete: " << result.codeSize << " bytes generated" << std::endl;

        return result;
    }

    bool AssemblyEngine::ParseAndAssembleInstruction(AssemblyContext& ctx, const std::string& instruction) {
        std::istringstream iss(instruction);
        std::string mnemonic;
        iss >> mnemonic;

        // Convert to lowercase for comparison
        std::transform(mnemonic.begin(), mnemonic.end(), mnemonic.begin(), ::tolower);

        // Collect operands
        std::vector<std::string> operands;
        std::string operandLine;
        std::getline(iss, operandLine);

        if (!operandLine.empty()) {
            // Parse operands considering commas and brackets
            std::string currentOp;
            int bracketDepth = 0;

            for (char c : operandLine) {
                if (c == '[') {
                    bracketDepth++;
                    currentOp += c;
                }
                else if (c == ']') {
                    bracketDepth--;
                    currentOp += c;
                }
                else if (c == ',' && bracketDepth == 0) {
                    // Trim and add operand
                    currentOp.erase(0, currentOp.find_first_not_of(" \t"));
                    currentOp.erase(currentOp.find_last_not_of(" \t") + 1);
                    if (!currentOp.empty()) {
                        operands.push_back(currentOp);
                    }
                    currentOp.clear();
                }
                else if (c != ' ' && c != '\t' || bracketDepth > 0 || !currentOp.empty()) {
                    currentOp += c;
                }
            }

            // Add last operand
            currentOp.erase(0, currentOp.find_first_not_of(" \t"));
            currentOp.erase(currentOp.find_last_not_of(" \t") + 1);
            if (!currentOp.empty()) {
                operands.push_back(currentOp);
            }
        }

        try {
            // Handle data directives
            if (mnemonic == "db" || mnemonic == "dw" || mnemonic == "dd" || mnemonic == "dq") {
                return HandleDataDirective(ctx, mnemonic, operands);
            }

            // Handle special instructions
            if (HandleSpecialInstruction(ctx, mnemonic, operands)) {
                return true;
            }

            // MOV instruction
            if (mnemonic == "mov") {
                if (operands.size() != 2) return false;

                if (operands[0][0] == '[') {
                    // Memory destination
                    Mem mem = ParseMemoryOperand(ctx, operands[0]);

                    if (ParseGpRegister(operands[1]).isValid()) {
                        ctx.assembler.mov(mem, ParseGpRegister(operands[1]));
                    }
                    else if (ParseXmmRegister(operands[1]).isValid()) {
                        // Can't directly mov xmm to memory, use movd/movq
                        Xmm xmm = ParseXmmRegister(operands[1]);
                        ctx.assembler.movd(mem, xmm);
                    }
                    else {
                        // Immediate to memory
                        Imm imm = ResolveImmediate(ctx, operands[1]);
                        ctx.assembler.mov(mem, imm);
                    }
                }
                else if (ParseGpRegister(operands[0]).isValid()) {
                    // Register destination
                    Gp reg1 = ParseGpRegister(operands[0]);

                    if (operands[1][0] == '[') {
                        // mov reg, [mem]
                        Mem mem = ParseMemoryOperand(ctx, operands[1]);
                        ctx.assembler.mov(reg1, mem);
                    }
                    else if (ParseGpRegister(operands[1]).isValid()) {
                        // mov reg, reg
                        ctx.assembler.mov(reg1, ParseGpRegister(operands[1]));
                    }
                    else {
                        // mov reg, imm
                        Imm imm = ResolveImmediate(ctx, operands[1]);
                        ctx.assembler.mov(reg1, imm);
                    }
                }
                else if (ParseXmmRegister(operands[0]).isValid()) {
                    // XMM register destination
                    Xmm xmm1 = ParseXmmRegister(operands[0]);

                    if (operands[1][0] == '[') {
                        ctx.assembler.movd(xmm1, ParseMemoryOperand(ctx, operands[1]));
                    }
                    else if (ParseXmmRegister(operands[1]).isValid()) {
                        ctx.assembler.movaps(xmm1, ParseXmmRegister(operands[1]));
                    }
                }
                else {
                    return false;
                }
                return true;
            }

            // MOVSS instruction
            else if (mnemonic == "movss") {
                if (operands.size() != 2) return false;

                if (ParseXmmRegister(operands[0]).isValid()) {
                    Xmm xmm = ParseXmmRegister(operands[0]);

                    if (operands[1][0] == '[') {
                        ctx.assembler.movss(xmm, ParseMemoryOperand(ctx, operands[1]));
                    }
                    else if (ParseXmmRegister(operands[1]).isValid()) {
                        ctx.assembler.movss(xmm, ParseXmmRegister(operands[1]));
                    }
                }
                else if (operands[0][0] == '[' && ParseXmmRegister(operands[1]).isValid()) {
                    ctx.assembler.movss(ParseMemoryOperand(ctx, operands[0]), ParseXmmRegister(operands[1]));
                }
                return true;
            }

            // JMP instruction
            else if (mnemonic == "jmp") {
                if (operands.empty()) return false;

                if (ParseGpRegister(operands[0]).isValid()) {
                    // jmp reg
                    ctx.assembler.jmp(ParseGpRegister(operands[0]));
                }
                else if (operands[0][0] == '[') {
                    // jmp [mem]
                    ctx.assembler.jmp(ParseMemoryOperand(ctx, operands[0]));
                }
                else {
                    // jmp label/address
                    // First try as immediate address
                    if (operands[0].find("0x") == 0 || std::all_of(operands[0].begin(), operands[0].end(), ::isxdigit)) {
                        uint64_t addr = std::stoull(operands[0], nullptr, 16);
                        ctx.assembler.jmp(imm(addr));
                    }
                    else {
                        // It's a label
                        Label label = ctx.GetOrCreateLabel(operands[0]);
                        ctx.assembler.jmp(label);
                    }
                }
                return true;
            }

            // CALL instruction
            else if (mnemonic == "call") {
                if (operands.empty()) return false;

                if (ParseGpRegister(operands[0]).isValid()) {
                    ctx.assembler.call(ParseGpRegister(operands[0]));
                }
                else if (operands[0][0] == '[') {
                    ctx.assembler.call(ParseMemoryOperand(ctx, operands[0]));
                }
                else {
                    // Try as immediate address first
                    if (operands[0].find("0x") == 0 || std::all_of(operands[0].begin(), operands[0].end(), ::isxdigit)) {
                        uint64_t addr = std::stoull(operands[0], nullptr, 16);
                        ctx.assembler.call(imm(addr));
                    }
                    else {
                        Label label = ctx.GetOrCreateLabel(operands[0]);
                        ctx.assembler.call(label);
                    }
                }
                return true;
            }

            // PUSH/POP
            else if (mnemonic == "push") {
                if (operands.empty()) return false;

                if (ParseGpRegister(operands[0]).isValid()) {
                    ctx.assembler.push(ParseGpRegister(operands[0]));
                }
                else if (operands[0][0] == '[') {
                    ctx.assembler.push(ParseMemoryOperand(ctx, operands[0]));
                }
                else {
                    ctx.assembler.push(ResolveImmediate(ctx, operands[0]));
                }
                return true;
            }
            else if (mnemonic == "pop") {
                if (operands.empty()) return false;

                if (ParseGpRegister(operands[0]).isValid()) {
                    ctx.assembler.pop(ParseGpRegister(operands[0]));
                }
                else if (operands[0][0] == '[') {
                    ctx.assembler.pop(ParseMemoryOperand(ctx, operands[0]));
                }
                return true;
            }

            // ADD/SUB
            else if (mnemonic == "add" || mnemonic == "sub") {
                if (operands.size() != 2) return false;

                if (ParseGpRegister(operands[0]).isValid()) {
                    Gp reg = ParseGpRegister(operands[0]);

                    if (ParseGpRegister(operands[1]).isValid()) {
                        if (mnemonic == "add")
                            ctx.assembler.add(reg, ParseGpRegister(operands[1]));
                        else
                            ctx.assembler.sub(reg, ParseGpRegister(operands[1]));
                    }
                    else if (operands[1][0] == '[') {
                        if (mnemonic == "add")
                            ctx.assembler.add(reg, ParseMemoryOperand(ctx, operands[1]));
                        else
                            ctx.assembler.sub(reg, ParseMemoryOperand(ctx, operands[1]));
                    }
                    else {
                        if (mnemonic == "add")
                            ctx.assembler.add(reg, ResolveImmediate(ctx, operands[1]));
                        else
                            ctx.assembler.sub(reg, ResolveImmediate(ctx, operands[1]));
                    }
                }
                else if (operands[0][0] == '[') {
                    Mem mem = ParseMemoryOperand(ctx, operands[0]);

                    if (ParseGpRegister(operands[1]).isValid()) {
                        if (mnemonic == "add")
                            ctx.assembler.add(mem, ParseGpRegister(operands[1]));
                        else
                            ctx.assembler.sub(mem, ParseGpRegister(operands[1]));
                    }
                    else {
                        if (mnemonic == "add")
                            ctx.assembler.add(mem, ResolveImmediate(ctx, operands[1]));
                        else
                            ctx.assembler.sub(mem, ResolveImmediate(ctx, operands[1]));
                    }
                }
                return true;
            }

            // LEA
            else if (mnemonic == "lea") {
                if (operands.size() != 2) return false;

                if (ParseGpRegister(operands[0]).isValid() && operands[1][0] == '[') {
                    ctx.assembler.lea(ParseGpRegister(operands[0]), ParseMemoryOperand(ctx, operands[1]));
                    return true;
                }
            }

            // TEST/CMP
            else if (mnemonic == "test" || mnemonic == "cmp") {
                if (operands.size() != 2) return false;

                if (ParseGpRegister(operands[0]).isValid()) {
                    Gp reg = ParseGpRegister(operands[0]);

                    if (ParseGpRegister(operands[1]).isValid()) {
                        if (mnemonic == "test")
                            ctx.assembler.test(reg, ParseGpRegister(operands[1]));
                        else
                            ctx.assembler.cmp(reg, ParseGpRegister(operands[1]));
                    }
                    else if (operands[1][0] == '[') {
                        if (mnemonic == "test")
                            ctx.assembler.test(reg, ParseMemoryOperand(ctx, operands[1]));
                        else
                            ctx.assembler.cmp(reg, ParseMemoryOperand(ctx, operands[1]));
                    }
                    else {
                        if (mnemonic == "test")
                            ctx.assembler.test(reg, ResolveImmediate(ctx, operands[1]));
                        else
                            ctx.assembler.cmp(reg, ResolveImmediate(ctx, operands[1]));
                    }
                }
                else if (operands[0][0] == '[') {
                    Mem mem = ParseMemoryOperand(ctx, operands[0]);

                    if (ParseGpRegister(operands[1]).isValid()) {
                        if (mnemonic == "test")
                            ctx.assembler.test(mem, ParseGpRegister(operands[1]));
                        else
                            ctx.assembler.cmp(mem, ParseGpRegister(operands[1]));
                    }
                    else {
                        if (mnemonic == "test")
                            ctx.assembler.test(mem, ResolveImmediate(ctx, operands[1]));
                        else
                            ctx.assembler.cmp(mem, ResolveImmediate(ctx, operands[1]));
                    }
                }
                return true;
            }

            // XOR
            else if (mnemonic == "xor") {
                if (operands.size() != 2) return false;

                if (ParseGpRegister(operands[0]).isValid()) {
                    Gp reg1 = ParseGpRegister(operands[0]);

                    if (ParseGpRegister(operands[1]).isValid()) {
                        ctx.assembler.xor_(reg1, ParseGpRegister(operands[1]));
                    }
                    else if (operands[1][0] == '[') {
                        ctx.assembler.xor_(reg1, ParseMemoryOperand(ctx, operands[1]));
                    }
                    else {
                        ctx.assembler.xor_(reg1, ResolveImmediate(ctx, operands[1]));
                    }
                }
                return true;
            }

            // INC/DEC
            else if (mnemonic == "inc" || mnemonic == "dec") {
                if (operands.empty()) return false;

                if (ParseGpRegister(operands[0]).isValid()) {
                    if (mnemonic == "inc")
                        ctx.assembler.inc(ParseGpRegister(operands[0]));
                    else
                        ctx.assembler.dec(ParseGpRegister(operands[0]));
                }
                else if (operands[0][0] == '[') {
                    if (mnemonic == "inc")
                        ctx.assembler.inc(ParseMemoryOperand(ctx, operands[0]));
                    else
                        ctx.assembler.dec(ParseMemoryOperand(ctx, operands[0]));
                }
                return true;
            }

            // Conditional jumps
            else if (mnemonic == "je" || mnemonic == "jz") {
                if (operands.empty()) return false;
                ctx.assembler.je(ctx.GetOrCreateLabel(operands[0]));
                return true;
            }
            else if (mnemonic == "jne" || mnemonic == "jnz") {
                if (operands.empty()) return false;
                ctx.assembler.jne(ctx.GetOrCreateLabel(operands[0]));
                return true;
            }
            else if (mnemonic == "jg") {
                if (operands.empty()) return false;
                ctx.assembler.jg(ctx.GetOrCreateLabel(operands[0]));
                return true;
            }
            else if (mnemonic == "ja") {
                if (operands.empty()) return false;
                ctx.assembler.ja(ctx.GetOrCreateLabel(operands[0]));
                return true;
            }
            else if (mnemonic == "jae") {
                if (operands.empty()) return false;
                ctx.assembler.jae(ctx.GetOrCreateLabel(operands[0]));
                return true;
            }
            else if (mnemonic == "jb") {
                if (operands.empty()) return false;
                ctx.assembler.jb(ctx.GetOrCreateLabel(operands[0]));
                return true;
            }
            else if (mnemonic == "jbe") {
                if (operands.empty()) return false;
                ctx.assembler.jbe(ctx.GetOrCreateLabel(operands[0]));
                return true;
            }
            else if (mnemonic == "jl") {
                if (operands.empty()) return false;
                ctx.assembler.jl(ctx.GetOrCreateLabel(operands[0]));
                return true;
            }
            else if (mnemonic == "jle") {
                if (operands.empty()) return false;
                ctx.assembler.jle(ctx.GetOrCreateLabel(operands[0]));
                return true;
            }
            else if (mnemonic == "jge") {
                if (operands.empty()) return false;
                ctx.assembler.jge(ctx.GetOrCreateLabel(operands[0]));
                return true;
            }

            // COMISS/UCOMISS
            else if (mnemonic == "comiss" || mnemonic == "ucomiss") {
                if (operands.size() != 2) return false;

                if (ParseXmmRegister(operands[0]).isValid()) {
                    Xmm xmm = ParseXmmRegister(operands[0]);

                    if (operands[1][0] == '[') {
                        if (mnemonic == "comiss")
                            ctx.assembler.comiss(xmm, ParseMemoryOperand(ctx, operands[1]));
                        else
                            ctx.assembler.ucomiss(xmm, ParseMemoryOperand(ctx, operands[1]));
                    }
                    else if (ParseXmmRegister(operands[1]).isValid()) {
                        if (mnemonic == "comiss")
                            ctx.assembler.comiss(xmm, ParseXmmRegister(operands[1]));
                        else
                            ctx.assembler.ucomiss(xmm, ParseXmmRegister(operands[1]));
                    }
                }
                return true;
            }

            // RET
            else if (mnemonic == "ret") {
                if (operands.empty()) {
                    ctx.assembler.ret();
                }
                else {
                    // ret with immediate (stack cleanup)
                    ctx.assembler.ret(ResolveImmediate(ctx, operands[0]));
                }
                return true;
            }

            // NOP
            else if (mnemonic == "nop") {
                if (operands.empty()) {
                    ctx.assembler.nop();
                }
                else {
                    // Multiple NOPs
                    int count = std::stoi(operands[0]);
                    for (int i = 0; i < count; ++i) {
                        ctx.assembler.nop();
                    }
                }
                return true;
            }

            // INT3
            else if (mnemonic == "int3") {
                ctx.assembler.int3();
                return true;
            }
        }
        catch (const std::exception& e) {
            std::cerr << "[ERROR] Assembly exception: " << e.what() << std::endl;
            return false;
        }

        std::cerr << "[WARNING] Unhandled mnemonic: " << mnemonic << std::endl;
        return false;
    }

    Mem AssemblyEngine::ParseMemoryOperand(AssemblyContext& ctx, const std::string& memExpr) {
        // Remove brackets
        std::string expr = memExpr;
        if (expr.front() == '[') expr = expr.substr(1);
        if (expr.back() == ']') expr.pop_back();

        // Trim whitespace
        expr.erase(0, expr.find_first_not_of(" \t"));
        expr.erase(expr.find_last_not_of(" \t") + 1);

        std::cout << "[DEBUG] Parsing memory operand: " << expr << std::endl;

        // Parse components: base + index*scale + displacement
        Gp base;
        Gp index;
        uint32_t scale = 0;
        int64_t displacement = 0;

        // Tokenize the expression
        std::vector<std::string> tokens;
        std::string currentToken;
        bool lastWasOperator = true;

        for (size_t i = 0; i < expr.length(); ++i) {
            char c = expr[i];

            if (c == '+' || c == '-' || c == '*') {
                if (!currentToken.empty()) {
                    tokens.push_back(currentToken);
                    currentToken.clear();
                }
                tokens.push_back(std::string(1, c));
                lastWasOperator = true;
            }
            else if (c == ' ' || c == '\t') {
                if (!currentToken.empty() && !lastWasOperator) {
                    tokens.push_back(currentToken);
                    currentToken.clear();
                }
            }
            else {
                currentToken += c;
                lastWasOperator = false;
            }
        }

        if (!currentToken.empty()) {
            tokens.push_back(currentToken);
        }

        // Process tokens
        bool expectingScale = false;
        bool negativeNext = false;

        for (size_t i = 0; i < tokens.size(); ++i) {
            const std::string& token = tokens[i];

            if (token == "+") {
                negativeNext = false;
            }
            else if (token == "-") {
                negativeNext = true;
            }
            else if (token == "*") {
                expectingScale = true;
            }
            else if (expectingScale) {
                scale = std::stoul(token);
                expectingScale = false;
            }
            else if (ParseGpRegister(token).isValid()) {
                Gp reg = ParseGpRegister(token);

                // Check if next token is * for scale
                if (i + 2 < tokens.size() && tokens[i + 1] == "*") {
                    index = reg;
                    i++; // Skip *
                    scale = std::stoul(tokens[i + 1]);
                    i++; // Skip scale
                }
                else if (!base.isValid()) {
                    base = reg;
                }
                else if (!index.isValid()) {
                    index = reg;
                    scale = 1; // Default scale
                }
            }
            else {
                // Must be a displacement or symbol
                int64_t value = 0;

                // First try to resolve as symbol
                bool resolved = false;
                if (symbolManager_) {
                    auto addr = symbolManager_->ResolveAddress(token);
                    if (addr) {
                        value = static_cast<int64_t>(*addr);
                        resolved = true;
                        std::cout << "[DEBUG]   Resolved symbol '" << token << "' to 0x"
                            << std::hex << *addr << std::dec << std::endl;
                    }
                }

                // If not a symbol, parse as number
                if (!resolved) {
                    try {
                        if (token.size() > 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X')) {
                            value = std::stoll(token, nullptr, 16);
                        }
                        else {
                            // Check if all hex digits (no 0x prefix)
                            bool isHex = std::all_of(token.begin(), token.end(),
                                [](char c) { return std::isxdigit(c); });

                            if (isHex && token.find_first_of("abcdefABCDEF") != std::string::npos) {
                                value = std::stoll(token, nullptr, 16);
                            }
                            else {
                                value = std::stoll(token);
                            }
                        }
                    }
                    catch (...) {
                        std::cerr << "[WARNING] Failed to parse displacement: " << token << std::endl;
                        value = 0;
                    }
                }

                displacement += (negativeNext ? -value : value);
                negativeNext = false;
            }
        }

        // Build memory operand
        if (base.isValid() && index.isValid() && scale > 0) {
            std::cout << "[DEBUG]   Memory: base=" << base.name() << " index=" << index.name()
                << " scale=" << scale << " disp=" << displacement << std::endl;
            return ptr(base, index, scale, static_cast<int32_t>(displacement));
        }
        else if (base.isValid() && index.isValid()) {
            std::cout << "[DEBUG]   Memory: base=" << base.name() << " index=" << index.name()
                << " disp=" << displacement << std::endl;
            return ptr(base, index, 0, static_cast<int32_t>(displacement));
        }
        else if (base.isValid()) {
            std::cout << "[DEBUG]   Memory: base=" << base.name() << " disp=" << displacement << std::endl;
            return ptr(base, static_cast<int32_t>(displacement));
        }
        else {
            // Absolute address
            std::cout << "[DEBUG]   Memory: absolute addr=0x" << std::hex << displacement << std::dec << std::endl;
            return ptr(static_cast<uint64_t>(displacement));
        }
    }

    Gp AssemblyEngine::ParseGpRegister(const std::string& regName) {
        std::string lower = regName;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        // 64-bit registers
        if (lower == "rax") return rax;
        if (lower == "rbx") return rbx;
        if (lower == "rcx") return rcx;
        if (lower == "rdx") return rdx;
        if (lower == "rsi") return rsi;
        if (lower == "rdi") return rdi;
        if (lower == "rbp") return rbp;
        if (lower == "rsp") return rsp;
        if (lower == "r8")  return r8;
        if (lower == "r9")  return r9;
        if (lower == "r10") return r10;
        if (lower == "r11") return r11;
        if (lower == "r12") return r12;
        if (lower == "r13") return r13;
        if (lower == "r14") return r14;
        if (lower == "r15") return r15;

        // 32-bit registers
        if (lower == "eax") return eax;
        if (lower == "ebx") return ebx;
        if (lower == "ecx") return ecx;
        if (lower == "edx") return edx;
        if (lower == "esi") return esi;
        if (lower == "edi") return edi;
        if (lower == "ebp") return ebp;
        if (lower == "esp") return esp;
        if (lower == "r8d")  return r8d;
        if (lower == "r9d")  return r9d;
        if (lower == "r10d") return r10d;
        if (lower == "r11d") return r11d;
        if (lower == "r12d") return r12d;
        if (lower == "r13d") return r13d;
        if (lower == "r14d") return r14d;
        if (lower == "r15d") return r15d;

        // 16-bit registers
        if (lower == "ax")  return ax;
        if (lower == "bx")  return bx;
        if (lower == "cx")  return cx;
        if (lower == "dx")  return dx;
        if (lower == "si")  return si;
        if (lower == "di")  return di;
        if (lower == "bp")  return bp;
        if (lower == "sp")  return sp;
        if (lower == "r8w")  return r8w;
        if (lower == "r9w")  return r9w;
        if (lower == "r10w") return r10w;
        if (lower == "r11w") return r11w;
        if (lower == "r12w") return r12w;
        if (lower == "r13w") return r13w;
        if (lower == "r14w") return r14w;
        if (lower == "r15w") return r15w;

        // 8-bit registers
        if (lower == "al")  return al;
        if (lower == "bl")  return bl;
        if (lower == "cl")  return cl;
        if (lower == "dl")  return dl;
        if (lower == "sil") return sil;
        if (lower == "dil") return dil;
        if (lower == "bpl") return bpl;
        if (lower == "spl") return spl;
        if (lower == "ah")  return ah;
        if (lower == "bh")  return bh;
        if (lower == "ch")  return ch;
        if (lower == "dh")  return dh;
        if (lower == "r8b")  return r8b;
        if (lower == "r9b")  return r9b;
        if (lower == "r10b") return r10b;
        if (lower == "r11b") return r11b;
        if (lower == "r12b") return r12b;
        if (lower == "r13b") return r13b;
        if (lower == "r14b") return r14b;
        if (lower == "r15b") return r15b;

        // Return invalid register
        return Gp();
    }

    Xmm AssemblyEngine::ParseXmmRegister(const std::string& regName) {
        std::string lower = regName;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        if (lower == "xmm0")  return xmm0;
        if (lower == "xmm1")  return xmm1;
        if (lower == "xmm2")  return xmm2;
        if (lower == "xmm3")  return xmm3;
        if (lower == "xmm4")  return xmm4;
        if (lower == "xmm5")  return xmm5;
        if (lower == "xmm6")  return xmm6;
        if (lower == "xmm7")  return xmm7;
        if (lower == "xmm8")  return xmm8;
        if (lower == "xmm9")  return xmm9;
        if (lower == "xmm10") return xmm10;
        if (lower == "xmm11") return xmm11;
        if (lower == "xmm12") return xmm12;
        if (lower == "xmm13") return xmm13;
        if (lower == "xmm14") return xmm14;
        if (lower == "xmm15") return xmm15;

        return Xmm();
    }

    Mm AssemblyEngine::ParseMmRegister(const std::string& regName) {
        std::string lower = regName;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        if (lower == "mm0") return mm0;
        if (lower == "mm1") return mm1;
        if (lower == "mm2") return mm2;
        if (lower == "mm3") return mm3;
        if (lower == "mm4") return mm4;
        if (lower == "mm5") return mm5;
        if (lower == "mm6") return mm6;
        if (lower == "mm7") return mm7;

        return Mm();
    }

    Imm AssemblyEngine::ResolveImmediate(AssemblyContext& ctx, const std::string& value) {
        std::cout << "[DEBUG] Resolving immediate: " << value << std::endl;

        // First try to resolve from captures
        if (captureStorage_ && captureStorage_->Exists(value)) {
            auto capture = captureStorage_->Get(value);
            if (capture) {
                uint64_t val = 0;
                switch (capture->size) {
                case 1: val = capture->AsUInt8(); break;
                case 2: val = capture->AsUInt16(); break;
                case 4: val = capture->AsUInt32(); break;
                case 8: val = capture->AsUInt64(); break;
                }
                std::cout << "[DEBUG]   Resolved capture '" << value << "' to 0x"
                    << std::hex << val << std::dec << std::endl;
                return imm(val);
            }
        }

        // Then try symbols
        if (symbolManager_) {
            auto addr = symbolManager_->ResolveAddress(value);
            if (addr) {
                std::cout << "[DEBUG]   Resolved symbol '" << value << "' to 0x"
                    << std::hex << *addr << std::dec << std::endl;
                return imm(*addr);
            }
        }

        // Check if it's a float cast
        if (value.find("(float)") == 0) {
            std::string floatStr = value.substr(7);
            float f = std::stof(floatStr);
            uint32_t bits = *reinterpret_cast<uint32_t*>(&f);
            std::cout << "[DEBUG]   Resolved float " << f << " to 0x"
                << std::hex << bits << std::dec << std::endl;
            return imm(bits);
        }

        // Parse as number
        uint64_t result = 0;
        try {
            if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
                result = std::stoull(value, nullptr, 16);
            }
            else if (value[0] == '$') {
                // CE style hex
                result = std::stoull(value.substr(1), nullptr, 16);
            }
            else if (std::all_of(value.begin(), value.end(), ::isxdigit)) {
                // Pure hex without prefix
                result = std::stoull(value, nullptr, 16);
            }
            else {
                // Decimal
                result = std::stoull(value);
            }
        }
        catch (...) {
            std::cerr << "[WARNING] Failed to parse immediate: " << value << std::endl;
            result = 0;
        }

        std::cout << "[DEBUG]   Parsed immediate as 0x" << std::hex << result << std::dec << std::endl;
        return imm(result);
    }

    std::string AssemblyEngine::PreprocessLine(const std::string& line, AssemblyContext& ctx) const {
        std::string result = line;

        // Replace capture references
        result = ReplaceCaptureReferences(result);

        // Replace symbol references
        result = ReplaceSymbolReferences(result, ctx);

        return result;
    }

    std::string AssemblyEngine::ReplaceCaptureReferences(const std::string& line) const {
        if (!captureStorage_) {
            return line;
        }

        std::string result = line;
        auto captureNames = captureStorage_->GetAllNames();

        std::cout << "[DEBUG] Replacing captures in: " << line << std::endl;

        // Sort by length descending to replace longer names first
        std::sort(captureNames.begin(), captureNames.end(),
            [](const std::string& a, const std::string& b) {
                return a.length() > b.length();
            });

        for (const auto& captureName : captureNames) {
            std::regex captureRegex(R"(\b)" + captureName + R"(\b)");

            auto capture = captureStorage_->Get(captureName);
            if (!capture) continue;

            std::string replacement;
            switch (capture->size) {
            case 1:
                replacement = "0x" + std::to_string(capture->AsUInt8());
                break;
            case 2:
                replacement = "0x" + std::to_string(capture->AsUInt16());
                break;
            case 4:
                replacement = "0x" + std::to_string(capture->AsUInt32());
                break;
            case 8:
                replacement = "0x" + std::to_string(capture->AsUInt64());
                break;
            }

            try {
                std::string before = result;
                result = std::regex_replace(result, captureRegex, replacement);
                if (before != result) {
                    std::cout << "[DEBUG]   Replaced '" << captureName << "' with " << replacement << std::endl;
                }
            }
            catch (const std::regex_error& e) {
                std::cerr << "[WARNING] Regex error replacing capture: " << e.what() << std::endl;
            }
        }

        if (result != line) {
            std::cout << "[DEBUG] After capture replacement: " << result << std::endl;
        }

        return result;
    }

    std::string AssemblyEngine::ReplaceSymbolReferences(const std::string& line, AssemblyContext& ctx) const {
        // For now, minimal implementation - symbols are resolved during immediate/memory parsing
        return line;
    }

    uint32_t AssemblyEngine::ParseScale(const std::string& scaleStr) {
        try {
            uint32_t scale = std::stoul(scaleStr);
            if (scale == 1 || scale == 2 || scale == 4 || scale == 8) {
                return scale;
            }
        }
        catch (...) {}

        return 1; // Default scale
    }

    bool AssemblyEngine::HandleDataDirective(AssemblyContext& ctx, const std::string& directive,
        const std::vector<std::string>& values) {
        for (const auto& value : values) {
            if (directive == "db") {
                // Define byte
                uint8_t byte = static_cast<uint8_t>(ResolveImmediate(ctx, value).value());
                ctx.assembler.db(byte);
            }
            else if (directive == "dw") {
                // Define word
                uint16_t word = static_cast<uint16_t>(ResolveImmediate(ctx, value).value());
                ctx.assembler.dw(word);
            }
            else if (directive == "dd") {
                // Define dword
                uint32_t dword = static_cast<uint32_t>(ResolveImmediate(ctx, value).value());
                ctx.assembler.dd(dword);
            }
            else if (directive == "dq") {
                // Define qword
                uint64_t qword = ResolveImmediate(ctx, value).value();
                ctx.assembler.dq(qword);
            }
        }
        return true;
    }

    bool AssemblyEngine::HandleSpecialInstruction(AssemblyContext& ctx, const std::string& mnemonic,
        const std::vector<std::string>& operands) {
        // Handle special CE script instructions
        if (mnemonic == "nop" && operands.size() == 1) {
            // Handle "nop X" syntax
            try {
                int count = std::stoi(operands[0]);
                for (int i = 0; i < count; ++i) {
                    ctx.assembler.nop();
                }
                return true;
            }
            catch (...) {
                // Not a number, treat as regular nop
            }
        }

        // Handle other special cases here

        return false;
    }

    std::optional<ByteVector> AssemblyEngine::AssembleInstruction(const std::string& instruction, AddressType address) {
        AssemblyContext ctx(runtime_.get(), address);

        std::string processed = PreprocessLine(instruction, ctx);

        if (!ParseAndAssembleInstruction(ctx, processed)) {
            return std::nullopt;
        }

        // Finalize and extract code
        Error err = ctx.code.flatten();
        if (err) return std::nullopt;

        CodeBuffer& buffer = ctx.code.sectionById(0)->buffer();
        if (buffer.size() == 0) return std::nullopt;

        return ByteVector(buffer.data(), buffer.data() + buffer.size());
    }

    ByteVector AssemblyEngine::GenerateNop(size_t count) {
        AssemblyContext ctx(runtime_.get());

        for (size_t i = 0; i < count; ++i) {
            ctx.assembler.nop();
        }

        ctx.code.flatten();
        CodeBuffer& buffer = ctx.code.sectionById(0)->buffer();

        return ByteVector(buffer.data(), buffer.data() + buffer.size());
    }

    ByteVector AssemblyEngine::GenerateJump(AddressType from, AddressType to) {
        AssemblyContext ctx(runtime_.get(), from);

        int64_t offset = static_cast<int64_t>(to) - static_cast<int64_t>(from) - 5;

        std::cout << "[DEBUG] Generating jump from 0x" << std::hex << from
            << " to 0x" << to << ", offset=0x" << offset << std::dec << std::endl;

        if (offset >= INT32_MIN && offset <= INT32_MAX) {
            // Near jump with 32-bit offset
            ctx.assembler.jmp(imm(to));
        }
        else {
            // Far jump using absolute address
            // Method 1: Using indirect jump through memory
            Label dataLabel = ctx.assembler.newLabel();
            ctx.assembler.jmp(ptr(dataLabel));
            ctx.assembler.align(AlignMode::kData, 8);
            ctx.assembler.bind(dataLabel);
            ctx.assembler.dq(to);
        }

        ctx.code.flatten();
        ctx.code.resolveUnresolvedLinks();

        CodeBuffer& buffer = ctx.code.sectionById(0)->buffer();
        ByteVector result(buffer.data(), buffer.data() + buffer.size());

        std::cout << "[DEBUG] Generated jump code (" << result.size() << " bytes): ";
        for (size_t i = 0; i < std::min<size_t>(result.size(), 16); ++i) {
            std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)result[i] << " ";
        }
        std::cout << std::dec << std::endl;

        return result;
    }

    ByteVector AssemblyEngine::GenerateCall(AddressType from, AddressType to) {
        AssemblyContext ctx(runtime_.get(), from);

        int64_t offset = static_cast<int64_t>(to) - static_cast<int64_t>(from) - 5;

        if (offset >= INT32_MIN && offset <= INT32_MAX) {
            // Near call
            ctx.assembler.call(imm(to));
        }
        else {
            // Far call
            ctx.assembler.mov(rax, imm(to));
            ctx.assembler.call(rax);
        }

        ctx.code.flatten();
        ctx.code.resolveUnresolvedLinks();

        CodeBuffer& buffer = ctx.code.sectionById(0)->buffer();
        return ByteVector(buffer.data(), buffer.data() + buffer.size());
    }

    ByteVector AssemblyEngine::GenerateDetour(AddressType from, AddressType to, size_t& trampolineSize) {
        // Generate a jump and ensure minimum size
        ByteVector jump = GenerateJump(from, to);

        trampolineSize = max(jump.size(), size_t(5));

        // Pad with NOPs if needed
        while (jump.size() < trampolineSize) {
            jump.push_back(0x90);
        }

        return jump;
    }

    std::optional<AssemblyEngine::HookInfo> AssemblyEngine::CreateHook(
        AddressType targetAddress,
        const std::string& hookCode) {

        HookInfo hook;
        hook.targetAddress = targetAddress;

        // Assemble hook code
        auto assembled = Assemble(hookCode);
        if (!assembled) {
            return std::nullopt;
        }

        hook.hookBytes = assembled->machineCode;

        // Additional hook setup would go here

        return hook;
    }

    ByteVector AssemblyEngine::GeneratePushAll() {
        AssemblyContext ctx(runtime_.get());

        // Push all general purpose registers
        ctx.assembler.push(rax);
        ctx.assembler.push(rbx);
        ctx.assembler.push(rcx);
        ctx.assembler.push(rdx);
        ctx.assembler.push(rsi);
        ctx.assembler.push(rdi);
        ctx.assembler.push(rbp);
        ctx.assembler.push(r8);
        ctx.assembler.push(r9);
        ctx.assembler.push(r10);
        ctx.assembler.push(r11);
        ctx.assembler.push(r12);
        ctx.assembler.push(r13);
        ctx.assembler.push(r14);
        ctx.assembler.push(r15);

        // Push flags
        ctx.assembler.pushf();

        ctx.code.flatten();
        CodeBuffer& buffer = ctx.code.sectionById(0)->buffer();

        return ByteVector(buffer.data(), buffer.data() + buffer.size());
    }

    ByteVector AssemblyEngine::GeneratePopAll() {
        AssemblyContext ctx(runtime_.get());

        // Pop in reverse order
        ctx.assembler.popf();
        ctx.assembler.pop(r15);
        ctx.assembler.pop(r14);
        ctx.assembler.pop(r13);
        ctx.assembler.pop(r12);
        ctx.assembler.pop(r11);
        ctx.assembler.pop(r10);
        ctx.assembler.pop(r9);
        ctx.assembler.pop(r8);
        ctx.assembler.pop(rbp);
        ctx.assembler.pop(rdi);
        ctx.assembler.pop(rsi);
        ctx.assembler.pop(rdx);
        ctx.assembler.pop(rcx);
        ctx.assembler.pop(rbx);
        ctx.assembler.pop(rax);

        ctx.code.flatten();
        CodeBuffer& buffer = ctx.code.sectionById(0)->buffer();

        return ByteVector(buffer.data(), buffer.data() + buffer.size());
    }

    ByteVector AssemblyEngine::GeneratePrologue(size_t stackSpace) {
        AssemblyContext ctx(runtime_.get());

        ctx.assembler.push(rbp);
        ctx.assembler.mov(rbp, rsp);

        if (stackSpace > 0) {
            ctx.assembler.sub(rsp, stackSpace);
        }

        ctx.code.flatten();
        CodeBuffer& buffer = ctx.code.sectionById(0)->buffer();

        return ByteVector(buffer.data(), buffer.data() + buffer.size());
    }

    ByteVector AssemblyEngine::GenerateEpilogue() {
        AssemblyContext ctx(runtime_.get());

        ctx.assembler.mov(rsp, rbp);
        ctx.assembler.pop(rbp);
        ctx.assembler.ret();

        ctx.code.flatten();
        CodeBuffer& buffer = ctx.code.sectionById(0)->buffer();

        return ByteVector(buffer.data(), buffer.data() + buffer.size());
    }

} // namespace AsmEngine