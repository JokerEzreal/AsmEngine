#include "AssemblyEngine.h"
#include <regex>
#include <sstream>
#include <iostream>

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

        while (std::getline(stream, line)) {
            // Trim whitespace
            line.erase(0, line.find_first_not_of(" \t"));
            line.erase(line.find_last_not_of(" \t") + 1);

            if (line.empty() || line[0] == ';') {
                continue;
            }

            // Check for label definition
            if (line.back() == ':') {
                std::string labelName = line.substr(0, line.length() - 1);
                ctx.BindLabel(labelName);

                // Register in symbol manager if available
                if (symbolManager_) {
                    symbolManager_->RegisterLabel(labelName,
                        address + ctx.assembler.offset());
                }
                continue;
            }

            // Preprocess and assemble instruction
            std::string processed = PreprocessLine(line, ctx);
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

        return result;
    }

    bool AssemblyEngine::ParseAndAssembleInstruction(AssemblyContext& ctx, const std::string& instruction) {
        std::istringstream iss(instruction);
        std::string mnemonic;
        iss >> mnemonic;

        // Convert to lowercase for comparison
        std::transform(mnemonic.begin(), mnemonic.end(), mnemonic.begin(), ::tolower);

        try {
            // MOV instruction
            if (mnemonic == "mov") {
                std::string op1, op2;
                if (!(iss >> op1)) return false;

                // Skip comma
                if (iss.peek() == ',') iss.get();

                if (!(iss >> op2)) return false;

                // Parse operands
                if (op1[0] == '[') {
                    // Memory destination
                    Mem mem = ParseMemoryOperand(ctx, op1);

                    if (op2[0] == '[') {
                        // mov [mem], [mem] - not directly supported
                        return false;
                    }
                    else if (ParseGpRegister(op2).isValid()) {
                        ctx.assembler.mov(mem, ParseGpRegister(op2));
                    }
                    else {
                        // Immediate to memory
                        Imm imm = ResolveImmediate(ctx, op2);
                        ctx.assembler.mov(mem, imm);
                    }
                }
                else if (ParseGpRegister(op1).isValid()) {
                    // Register destination
                    Gp reg1 = ParseGpRegister(op1);

                    if (op2[0] == '[') {
                        // mov reg, [mem]
                        Mem mem = ParseMemoryOperand(ctx, op2);
                        ctx.assembler.mov(reg1, mem);
                    }
                    else if (ParseGpRegister(op2).isValid()) {
                        // mov reg, reg
                        ctx.assembler.mov(reg1, ParseGpRegister(op2));
                    }
                    else {
                        // mov reg, imm
                        Imm imm = ResolveImmediate(ctx, op2);
                        ctx.assembler.mov(reg1, imm);
                    }
                }
                else {
                    return false;
                }
                return true;
            }

            // JMP instruction
            else if (mnemonic == "jmp") {
                std::string target;
                iss >> target;

                if (ParseGpRegister(target).isValid()) {
                    // jmp reg
                    ctx.assembler.jmp(ParseGpRegister(target));
                }
                else if (target[0] == '[') {
                    // jmp [mem]
                    ctx.assembler.jmp(ParseMemoryOperand(ctx, target));
                }
                else {
                    // jmp label/address
                    Label label = ctx.GetOrCreateLabel(target);
                    ctx.assembler.jmp(label);
                }
                return true;
            }

            // CALL instruction
            else if (mnemonic == "call") {
                std::string target;
                iss >> target;

                if (ParseGpRegister(target).isValid()) {
                    ctx.assembler.call(ParseGpRegister(target));
                }
                else if (target[0] == '[') {
                    ctx.assembler.call(ParseMemoryOperand(ctx, target));
                }
                else {
                    Label label = ctx.GetOrCreateLabel(target);
                    ctx.assembler.call(label);
                }
                return true;
            }

            // PUSH/POP
            else if (mnemonic == "push") {
                std::string op;
                iss >> op;

                if (ParseGpRegister(op).isValid()) {
                    ctx.assembler.push(ParseGpRegister(op));
                }
                else if (op[0] == '[') {
                    ctx.assembler.push(ParseMemoryOperand(ctx, op));
                }
                else {
                    ctx.assembler.push(ResolveImmediate(ctx, op));
                }
                return true;
            }
            else if (mnemonic == "pop") {
                std::string op;
                iss >> op;

                if (ParseGpRegister(op).isValid()) {
                    ctx.assembler.pop(ParseGpRegister(op));
                }
                else if (op[0] == '[') {
                    ctx.assembler.pop(ParseMemoryOperand(ctx, op));
                }
                return true;
            }

            // ADD/SUB
            else if (mnemonic == "add" || mnemonic == "sub") {
                std::string op1, op2;
                iss >> op1;
                if (iss.peek() == ',') iss.get();
                iss >> op2;

                if (ParseGpRegister(op1).isValid()) {
                    Gp reg = ParseGpRegister(op1);

                    if (ParseGpRegister(op2).isValid()) {
                        if (mnemonic == "add")
                            ctx.assembler.add(reg, ParseGpRegister(op2));
                        else
                            ctx.assembler.sub(reg, ParseGpRegister(op2));
                    }
                    else if (op2[0] == '[') {
                        if (mnemonic == "add")
                            ctx.assembler.add(reg, ParseMemoryOperand(ctx, op2));
                        else
                            ctx.assembler.sub(reg, ParseMemoryOperand(ctx, op2));
                    }
                    else {
                        if (mnemonic == "add")
                            ctx.assembler.add(reg, ResolveImmediate(ctx, op2));
                        else
                            ctx.assembler.sub(reg, ResolveImmediate(ctx, op2));
                    }
                }
                else if (op1[0] == '[') {
                    Mem mem = ParseMemoryOperand(ctx, op1);

                    if (ParseGpRegister(op2).isValid()) {
                        if (mnemonic == "add")
                            ctx.assembler.add(mem, ParseGpRegister(op2));
                        else
                            ctx.assembler.sub(mem, ParseGpRegister(op2));
                    }
                    else {
                        if (mnemonic == "add")
                            ctx.assembler.add(mem, ResolveImmediate(ctx, op2));
                        else
                            ctx.assembler.sub(mem, ResolveImmediate(ctx, op2));
                    }
                }
                return true;
            }

            // LEA
            else if (mnemonic == "lea") {
                std::string op1, op2;
                iss >> op1;
                if (iss.peek() == ',') iss.get();
                iss >> op2;

                if (ParseGpRegister(op1).isValid() && op2[0] == '[') {
                    ctx.assembler.lea(ParseGpRegister(op1), ParseMemoryOperand(ctx, op2));
                    return true;
                }
            }

            // TEST/CMP
            else if (mnemonic == "test" || mnemonic == "cmp") {
                std::string op1, op2;
                iss >> op1;
                if (iss.peek() == ',') iss.get();
                iss >> op2;

                if (ParseGpRegister(op1).isValid()) {
                    Gp reg = ParseGpRegister(op1);

                    if (ParseGpRegister(op2).isValid()) {
                        if (mnemonic == "test")
                            ctx.assembler.test(reg, ParseGpRegister(op2));
                        else
                            ctx.assembler.cmp(reg, ParseGpRegister(op2));
                    }
                    else if (op2[0] == '[') {
                        if (mnemonic == "test")
                            ctx.assembler.test(reg, ParseMemoryOperand(ctx, op2));
                        else
                            ctx.assembler.cmp(reg, ParseMemoryOperand(ctx, op2));
                    }
                    else {
                        if (mnemonic == "test")
                            ctx.assembler.test(reg, ResolveImmediate(ctx, op2));
                        else
                            ctx.assembler.cmp(reg, ResolveImmediate(ctx, op2));
                    }
                }
                return true;
            }

            // Conditional jumps
            else if (mnemonic == "je" || mnemonic == "jz") {
                std::string target;
                iss >> target;
                ctx.assembler.je(ctx.GetOrCreateLabel(target));
                return true;
            }
            else if (mnemonic == "jne" || mnemonic == "jnz") {
                std::string target;
                iss >> target;
                ctx.assembler.jne(ctx.GetOrCreateLabel(target));
                return true;
            }
            else if (mnemonic == "jg") {
                std::string target;
                iss >> target;
                ctx.assembler.jg(ctx.GetOrCreateLabel(target));
                return true;
            }
            else if (mnemonic == "jl") {
                std::string target;
                iss >> target;
                ctx.assembler.jl(ctx.GetOrCreateLabel(target));
                return true;
            }

            // RET
            else if (mnemonic == "ret") {
                ctx.assembler.ret();
                return true;
            }

            // NOP
            else if (mnemonic == "nop") {
                ctx.assembler.nop();
                return true;
            }

            // SSE instructions
            else if (mnemonic == "movss") {
                std::string op1, op2;
                iss >> op1;
                if (iss.peek() == ',') iss.get();
                iss >> op2;

                if (ParseXmmRegister(op1).isValid()) {
                    Xmm xmm = ParseXmmRegister(op1);

                    if (op2[0] == '[') {
                        ctx.assembler.movss(xmm, ParseMemoryOperand(ctx, op2));
                    }
                    else if (ParseXmmRegister(op2).isValid()) {
                        ctx.assembler.movss(xmm, ParseXmmRegister(op2));
                    }
                }
                else if (op1[0] == '[' && ParseXmmRegister(op2).isValid()) {
                    ctx.assembler.movss(ParseMemoryOperand(ctx, op1), ParseXmmRegister(op2));
                }
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

        // Parse components: base + index*scale + displacement
        Gp base;
        Gp index;
        uint32_t scale = 0;
        int32_t displacement = 0;

        // Simple parsing - this could be enhanced
        std::istringstream iss(expr);
        std::string token;
        bool expectingOperator = false;
        bool lastWasPlus = true;

        while (iss >> token) {
            if (token == "+" || token == "-") {
                lastWasPlus = (token == "+");
                expectingOperator = false;
            }
            else if (token == "*") {
                // Next token should be scale
                if (iss >> token) {
                    scale = std::stoul(token);
                }
            }
            else if (ParseGpRegister(token).isValid()) {
                Gp reg = ParseGpRegister(token);

                if (!base.isValid()) {
                    base = reg;
                }
                else if (!index.isValid()) {
                    index = reg;
                }
            }
            else {
                // Must be a displacement or immediate
                int32_t value = 0;

                // Resolve symbol or parse number
                if (symbolManager_) {
                    auto addr = symbolManager_->ResolveAddress(token);
                    if (addr) {
                        value = static_cast<int32_t>(*addr);
                    }
                    else {
                        value = static_cast<int32_t>(ResolveImmediate(ctx, token).value());
                    }
                }
                else {
                    value = static_cast<int32_t>(ResolveImmediate(ctx, token).value());
                }

                displacement += (lastWasPlus ? value : -value);
            }
        }

        // Build memory operand
        if (base.isValid() && index.isValid() && scale > 0) {
            return ptr(base, index, scale, displacement);
        }
        else if (base.isValid() && index.isValid()) {
            return ptr(base, index, 0, displacement);
        }
        else if (base.isValid()) {
            return ptr(base, displacement);
        }
        else {
            // Absolute address
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

    Imm AssemblyEngine::ResolveImmediate(AssemblyContext& ctx, const std::string& value) {
        // First try to resolve from captures
        if (captureStorage_ && captureStorage_->Exists(value)) {
            auto capture = captureStorage_->Get(value);
            if (capture) {
                switch (capture->size) {
                case 1: return imm(capture->AsUInt8());
                case 2: return imm(capture->AsUInt16());
                case 4: return imm(capture->AsUInt32());
                case 8: return imm(capture->AsUInt64());
                }
            }
        }

        // Then try symbols
        if (symbolManager_) {
            auto addr = symbolManager_->ResolveAddress(value);
            if (addr) {
                return imm(*addr);
            }
        }

        // Check if it's a float cast
        if (value.find("(float)") == 0) {
            std::string floatStr = value.substr(7);
            float f = std::stof(floatStr);
            uint32_t bits = *reinterpret_cast<uint32_t*>(&f);
            return imm(bits);
        }

        // Parse as number
        return ctx.ResolveImmediate(value);
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

            result = std::regex_replace(result, captureRegex, replacement);
        }

        return result;
    }

    std::string AssemblyEngine::ReplaceSymbolReferences(const std::string& line, AssemblyContext& ctx) const {
        // For now, minimal implementation - symbols are resolved during immediate parsing
        return line;
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

        if (offset >= INT32_MIN && offset <= INT32_MAX) {
            // Near jump with 32-bit offset
            ctx.assembler.jmp(imm(to));
        }
        else {
            // Far jump using absolute address
            Label target = ctx.assembler.newLabel();
            ctx.assembler.jmp(ptr(target));
            ctx.assembler.align(AlignMode::kData, 8);
            ctx.assembler.bind(target);
            ctx.assembler.embedUInt64(to);
        }

        ctx.code.flatten();
        ctx.code.resolveUnresolvedLinks();

        CodeBuffer& buffer = ctx.code.sectionById(0)->buffer();
        return ByteVector(buffer.data(), buffer.data() + buffer.size());
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

} // namespace AsmEngine