#include "code_gen.h"

#include "evaluator.h"
#include "parser.h"
#include "token.h"

#include <cstdarg>

#include <queue>
#include <set>


extern std::string inFileName;
extern std::string outFileName;


Parser* Generator::_parser = nullptr;
FILE* Generator::_outFile = nullptr;
//std::string Generator::_cons;
RODataList Generator::_rodatas;
std::vector<Declaration*> Generator::_staticDecls;
int Generator::_offset = 0;
int Generator::_retAddrOffset = 0;


static std::vector<const char*> regs {
    "rdi", "rsi", "rdx",
    "rcx", "r8", "r9"
};

static std::vector<const char*> xregs {
    "xmm0", "xmm1", "xmm2", "xmm3",
    "xmm4", "xmm5", "xmm6", "xmm7"
};

static ParamClass Classify(Type* paramType, int offset=0)
{
    if (paramType->IsInteger() || paramType->ToPointerType()
            || paramType->ToArrayType()) {
        return ParamClass::INTEGER;
    }
    
    if (paramType->ToArithmType()) {
        auto type = paramType->ToArithmType();
        if (type->Tag() == T_FLOAT || type->Tag() == T_DOUBLE)
            return ParamClass::SSE;
        if (type->Tag() == (T_LONG | T_DOUBLE)) {
            // TODO(wgtdkp):
            assert(false); 
            return ParamClass::X87;
        }
        
        // TODO(wgtdkp):
        assert(false);
        // It is complex
        if ((type->Tag() & T_LONG) && (type->Tag() & T_DOUBLE))
            return ParamClass::COMPLEX_X87;
        
    }

    // TODO(wgtdkp): Support agrregate type 
    assert(false);
    /*
    auto type = paramType->ToStructUnionType();
    assert(type);

    if (type->Width() > 4 * 8)
        return PC_MEMORY;

    std::vector<ParamClass> classes;
    int cnt = (type->Width() + 7) / 8;
    for (int i = 0; i < cnt; i++) {
        auto  types = FieldsIn8Bytes(type, i);
        assert(types.size() > 0);
        
        auto fieldClass = (types.size() == 1)
                ? PC_NO_CLASS: FieldClass(types, 0);
        classes.push_back(fieldClass);
        
    }

    bool sawX87 = false;
    for (int i = 0; i < classes.size(); i++) {
        if (classes[i] == PC_MEMORY)
            return PC_MEMORY;
        if (classes[i] == PC_X87_UP && sawX87)
            return PC_MEMORY;
        if (classes[i] == PC_X87)
            sawX87 = true;
    }
    */
    return ParamClass::NO_CLASS; // Make compiler happy
}


std::string ObjectLabel(Object* obj)
{
    static int tag = 0;
    assert(obj->IsStatic());
    if (obj->Linkage() == L_NONE) {
        return obj->Name() + "." + std::to_string(tag++);
    }
    return obj->Name();
}


std::string Generator::ConsLabel(Constant* cons)
{
    if (cons->Type()->IsInteger()) {
        return "$" + std::to_string(cons->IVal());
    } else if (cons->Type()->IsFloat()) {
        double valsd = cons->FVal();
        float  valss = valsd;
        // TODO(wgtdkp): Add rodata
        auto width = cons->Type()->Width();
        long val = width == 4 ? *(int*)&valss: *(long*)&valsd;
        const ROData& rodata = ROData(val, width);
        _rodatas.push_back(rodata);
        return "$" + rodata._label;
    } else { // Literal
        const ROData& rodata = ROData(*cons->SVal());
        _rodatas.push_back(rodata);
        return "$" + rodata._label;
    }
}

static const char* GetLoad(int width, bool flt=false)
{
    switch (width) {
    case 1: return "movzbq";
    case 2: return "movzwq";
    case 4: return !flt ? "movl": "movss";
    case 8: return !flt ? "movq": "movsd";
    default: assert(false); return nullptr;
    }
}


static std::string GetInst(const std::string& inst, int width, bool flt)
{
    if (flt)  {
        return inst + (width == 4 ? "ss": "sd");
    } else {
        switch (width) {
        case 1: return inst + "b";
        case 2: return inst + "w";
        case 4: return inst + "l";
        case 8: return inst + "q";
        default: assert(false);
        }
        return inst; // Make compiler happy
    } 
}


static std::string GetInst(const std::string& inst, Type* type)
{
    assert(type->IsScalar());
    return GetInst(inst, type->Width(), type->IsFloat());
}


static const char* GetReg(int width)
{
    switch (width) {
    case 1: return "al";
    case 2: return "ax";
    case 4: return "r10d";
    case 8: return "r10";
    default: assert(false); return nullptr;
    }
}


static inline void GetOperands(const char*& src,
        const char*& des, int width, bool flt)
{
    src = flt ? "xmm9": (width == 8 ? "r10": "r10d");
    des = flt ? "xmm8": (width == 8 ? "r10": "r10d");
}


int Generator::Push(const char* reg)
{
    _offset -= 8;
    auto mov = reg[0] == 'x' ? "movsd": "movq";
    Emit("%s #%s, %d(#rbp)", mov, reg, _offset);
    return _offset;
}


int Generator::Pop(const char* reg)
{
    auto mov = reg[0] == 'x' ? "movsd": "movq";
    Emit("%s %d(#rbp), #%s", mov, _offset, reg);
    _offset += 8;
    return _offset;
}


void Generator::Restore(bool flt)
{
    const char* src = flt ? "xmm8": "r10";
    const char* des = flt ? "xmm9": "r10";
    const char* inst = flt ? "movsd": "movq";
    Emit("%s #%s, #%s", inst, src, des);
    Pop(src);
}


/*
 * Operaotr/Instruction mapping:
 * +  add
 * -  sub
 * *  mul
 * /  div
 * %  div
 * << sal
 * >> sar
 * |  or
 * &  and
 * ^  xor
 * =  mov
 * <  cmp, setl, movzbq
 * >  cmp, setg, movzbq
 * <= cmp, setle, movzbq
 * >= cmp, setle, movzbq
 * == cmp, sete, movzbq
 * != cmp, setne, movzbq
 * && GenAndOp
 * || GenOrOp
 * ]  GenSubScriptingOp
 * .  GenMemberRefOp
 */
void Generator::VisitBinaryOp(BinaryOp* binary)
{
    auto op = binary->_op;

    if (op == '=')
        return GenAssignOp(binary);
    if (op == Token::AND_OP)
        return GenAndOp(binary);
    if (op == Token::OR_OP)
        return GenOrOp(binary);
    if (op == '.')
        return GenMemberRefOp(binary);
    //if (op == ']')
    //    return GenSubScriptingOp(binary);
    if (binary->Type()->ToPointerType())
        return GenPointerArithm(binary);


    const char* inst;
    auto width = binary->Type()->Width();
    auto flt = binary->Type()->IsFloat();
    auto sign = !flt && !(binary->Type()->ToArithmType()->Tag() & T_UNSIGNED);

    Visit(binary->_lhs);
    Spill(flt);
    Visit(binary->_rhs);
    Restore(flt);

    switch (op) {
    case '+': inst = flt ? (width == 4 ? "addss": "addsd"): "add"; break;
    case '-': inst = flt ? (width == 4 ? "subss": "subsd"): "sub"; break;
    case '*': inst = flt ? (width == 4 ? "mulss": "mulsd"): "mul"; break; 
    case '/': case '%': return GenDivOp(flt, sign, width, op);
    case '<': 
        return GenCompOp(flt, width, (flt || !sign) ? "setb": "setl");
    case '>':
        return GenCompOp(flt, width, (flt || !sign) ? "seta": "setg");
    case Token::LE_OP:
        return GenCompOp(flt, width, (flt || !sign) ? "setbe": "setle");
    case Token::GE_OP:
        return GenCompOp(flt, width, (flt || !sign) ? "setae": "setge");
    case Token::EQ_OP:
        return GenCompOp(flt, width, "sete");
    case Token::NE_OP:
        return GenCompOp(flt, width, "setne");

    case '|': inst = "or"; break;
    case '&': inst = "and"; break;
    case '^': inst = "xor"; break;
    case Token::LEFT_OP: inst = "sal"; break;
    case Token::RIGHT_OP: inst = sign ? "sar": "shr"; break;
    }

    const char* src;
    const char* des;
    GetOperands(src, des, width, flt);
    Emit("%s #%s, #%s", inst, src, des);
}


void Generator::GenAndOp(BinaryOp* andOp)
{
    // TODO(wgtdkp):
}


void Generator::GenOrOp(BinaryOp* orOp)
{
    // TODO(wgtdkp):
}


void Generator::GenMemberRefOp(BinaryOp* ref)
{
    // As the lhs will always be struct/union 
    auto addr = LValGenerator().GenExpr(ref).Repr();

    if (!ref->Type()->IsScalar()) {
        Emit("lea %s, #r10", addr.c_str());
    } else {
        EmitLoad(addr, ref->Type());
    }
}


void Generator::GenAssignOp(BinaryOp* assign)
{
    // The base register of addr is %r11
    auto addr = LValGenerator().GenExpr(assign->_lhs);
    GenExpr(assign->_rhs);

    if (assign->Type()->IsScalar()) {
        EmitStore(addr.Repr(), assign->Type());
    } else {
        // struct/union type
        // The address of rhs is in %r10
        CopyStruct(addr, assign->Type()->Width());
    }
}


void Generator::CopyStruct(ObjectAddr desAddr, int len)
{
    int widths[] = {8, 4, 2, 1};
    int offset = 0;

    for (auto width: widths) {
        while (len >= width) {
            auto mov = GetInst("mov", width, false);
            Emit("%s %d(#r10), #r10", mov.c_str(), offset);
            Emit("%s #r10, %s", mov.c_str(), desAddr.Repr().c_str());
            desAddr._offset += width;
            offset += width;
            len -= width;
        }
    }
}


void Generator::GenCompOp(bool flt, int width, const char* set)
{
    auto cmp = flt ? (width == 8 ? "ucomisd": "ucomiss"): "cmp";
    
    const char* src;
    const char* des;
    GetOperands(src, des, width, flt);

    Emit("%s #%s, #%s", cmp, src, des);
    Emit("%s #al", set);
    Emit("movzbl #al, #r10");
}


void Generator::GenDivOp(bool flt, bool sign, int width, int op)
{
    if (flt) {
        auto inst = width == 4 ? "divss": "divsd";
        Emit("%s #xmm9, #xmm8", inst);
        return;
    } else if (!sign) {
        Emit("xor #rdx, #rdx");
        Emit("div #r10");
    } else {
        Emit("cqto");
        Emit("idiv #r10");            
    }
    if (op == '%')
        Emit("mov #rdx, #r10");
}

 
void Generator::GenPointerArithm(BinaryOp* binary)
{
    // For '+', we have swap _lhs and _rhs to ensure that 
    // the pointer is at lhs.
    Visit(binary->_lhs);
    Push("r10");
    Push("r10");
    Visit(binary->_rhs);
    
    auto type = binary->_lhs->Type()->ToPointerType()->Derived();
    if (type->Width() > 1)
        Emit("imul $%d, #r10", type->Width());
    Emit("mov #r10, #r10");
    Pop("r10");
    if (binary->_op == '+')
        Emit("add #r10, #r10");
    else
        Emit("sub #r10, #r10");
    Pop("r10");
}


void Generator::GenDerefOp(UnaryOp* deref)
{
    auto addr = LValGenerator().GenExpr(deref->_operand).Repr();
    Emit("lea %s, #r10", addr.c_str());
}


void Generator::VisitObject(Object* obj)
{
    auto addr = LValGenerator().GenExpr(obj).Repr();

    // TODO(wgtdkp): handle static object
    if (!obj->Type()->IsScalar()) {
        // Return the address of the object in r10
        Emit("lea %s, #r10", addr.c_str());
    } else {
        EmitLoad(addr, obj->Type());
    }
}


void Generator::GenCastOp(UnaryOp* cast)
{
    auto desType = cast->Type();
    auto srcType = cast->_operand->Type();

    if (srcType->IsFloat() && desType->IsFloat()) {
        if (srcType->Width() == desType->Width())
            return;
        const char* inst = srcType->Width() == 4 ? "movss2sd": "movsd2ss";
        Emit("%s #xmm8, #xmm8", inst);
    } else if (srcType->IsFloat()) {
        const char* inst = srcType->Width() == 4 ? "cvttss2si": "cvttsd2si";
        Emit("%s #xmm8, #r10", inst);
    } else if (desType->IsFloat()) {
        const char* inst = desType->Width() == 4 ? "cvtsi2ss": "cvtsi2sd";
        Emit("%s #r10, #xmm8", inst);
    } else if (srcType->ToPointerType()) {
        // Do nothing
    } else {
        int width = srcType->Width();
        auto sign = !(srcType->ToArithmType()->Tag() & T_UNSIGNED);
        const char* inst;
        switch (width) {
        case 1: inst = sign ? "movsbq": "movzbq"; break;
        case 2: inst = sign ? "movswq": "movzwq"; break;
        case 4: inst = "movl"; break;
        case 8: inst = "movq"; break;
        }
        if (inst[4] == 0)
            return;
        Emit("%s #%s, #r10", inst, GetReg(width));
    }
}


void Generator::VisitUnaryOp(UnaryOp* unary)
{
    switch  (unary->_op) {
    case Token::PREFIX_INC:
        return GenPrefixIncDec(unary->_operand, "add");
    case Token::PREFIX_DEC:
        return GenPrefixIncDec(unary->_operand, "sub");
    case Token::POSTFIX_INC:
        return GenPostfixIncDec(unary->_operand, "add");
    case Token::POSTFIX_DEC:
        return GenPostfixIncDec(unary->_operand, "sub");
    case Token::ADDR: {
        auto addr = LValGenerator().GenExpr(unary->_operand).Repr();
        Emit("leaq %s, #r10", addr.c_str());
    } return;
    case Token::DEREF:
        if (!unary->Type()->ToStructUnionType())
            EmitLoad("(#r10)", unary->Type());
        return;
    case Token::PLUS:
        return;
    case Token::MINUS:
        // TODO(wgtdkp): pxor %xmm9, %xmm9
        return;
    case '~': return;
    case '!': return;
    case Token::CAST:
        Visit(unary->_operand);
        GenCastOp(unary);
        return;
    default: assert(false);
    }
}


void Generator::GenPrefixIncDec(Expr* operand, const std::string& inst)
{
    // Need a special base register to reduce register conflict
    auto addr = LValGenerator().GenExpr(operand).Repr();
    auto des = EmitLoad(addr, operand->Type());

    Constant* cons;
    auto pointerType = operand->Type()->ToPointerType();
     if (pointerType) {
        long width = pointerType->Derived()->Width();
        cons = Constant::New(operand->Tok(), T_LONG, width);
    } else if (operand->Type()->IsInteger()) {
        cons = Constant::New(operand->Tok(), T_LONG, 1L);
    } else {
        if (operand->Type()->Width() == 4)
            cons = Constant::New(operand->Tok(), T_FLOAT, 1.0);
        else
            cons = Constant::New(operand->Tok(), T_DOUBLE, 1.0);
    }
    auto consLabel = ConsLabel(cons);

    auto addSub = GetInst(inst, operand->Type());    
    Emit("%s %s, #%s", addSub.c_str(), consLabel.c_str(), des.c_str());
    
    EmitStore(addr, operand->Type());
}


void Generator::GenPostfixIncDec(Expr* operand, const std::string& inst)
{
    // Need a special base register to reduce register conflict
    auto addr = LValGenerator().GenExpr(operand).Repr();
    auto des = EmitLoad(addr, operand->Type());
    auto saved = Save(des);

    Constant* cons;
    auto pointerType = operand->Type()->ToPointerType();
     if (pointerType) {
        long width = pointerType->Derived()->Width();
        cons = Constant::New(operand->Tok(), T_LONG, width);
    } else if (operand->Type()->IsInteger()) {
        cons = Constant::New(operand->Tok(), T_LONG, 1L);
    } else {
        if (operand->Type()->Width() == 4)
            cons = Constant::New(operand->Tok(), T_FLOAT, 1.0);
        else
            cons = Constant::New(operand->Tok(), T_DOUBLE, 1.0);
    }
    auto consLabel = ConsLabel(cons);

    auto addSub = GetInst(inst, operand->Type());    
    Emit("%s %s, #%s", addSub.c_str(), consLabel.c_str(), des.c_str());

    EmitStore(addr, operand->Type());
    Exchange(des, saved);
}


void Generator::Exchange(const std::string& lhs, const std::string& rhs)
{
    if (lhs == "xmm8" || rhs == "xmm8") {
        Emit("movsd #%s, #xmm2", lhs.c_str());
        Emit("movsd #%s, #%s", rhs.c_str(), lhs.c_str());
        Emit("movsd #xmm2, #%s", rhs.c_str());
    } else {
        Emit("xchgq #%s, #%s", lhs.c_str(), rhs.c_str());
    }

}


void Generator::VisitConditionalOp(ConditionalOp* condOp)
{

}



void Generator::VisitEnumerator(Enumerator* enumer)
{

}


void Generator::VisitIdentifier(Identifier* ident)
{

}


void Generator::VisitConstant(Constant* cons)
{
    auto label = ConsLabel(cons);
    EmitLoad(label, cons->Type());
}



void Generator::VisitTempVar(TempVar* tempVar)
{

}


void Generator::VisitDeclaration(Declaration* decl)
{
    auto obj = decl->_obj;

    if (!obj->IsStatic()) {
        // The object has no linkage and has 
        //     no static storage(the object is on stack).
        // If it has no initialization, then it's value is random
        //     initialized.
        if (!obj->HasInit())
            return;

        for (auto init: decl->Inits()) {
            ObjectAddr addr = {"", "rbp", obj->Offset() + init._offset};
            GenExpr(init._expr);
            EmitStore(addr.Repr(), init._type);
        }
        return;
    }

    if (obj->Linkage() == L_NONE)
        _staticDecls.push_back(decl);
    else
        GenStaticDecl(decl);
}


void Generator::GenStaticDecl(Declaration* decl)
{
    auto obj = decl->_obj;
    assert(obj->IsStatic());

    auto label = ObjectLabel(obj);
    auto width = obj->Type()->Width();
    auto align = obj->Type()->Align();

    // omit the external without initilizer
    if ((obj->Storage() & S_EXTERN) && !obj->HasInit())
        return;
    
    Emit(".data");
    auto glb = obj->Linkage() == L_EXTERNAL ? ".globl": ".local";
    Emit("%s %s", glb, label.c_str());

    if (!obj->HasInit()) {    
        Emit(".comm %s, %d, %d", label.c_str(), width, align);
        return;
    }

    Emit(".align %d", align);
    Emit(".type %s, @object", label.c_str());
    Emit(".size %s, %d", label.c_str(), width);
    EmitLabel(label.c_str());
    
    // TODO(wgtdkp): Add .zero
    int offset = 0;
    for (auto init: decl->Inits()) {
        auto staticInit = GetStaticInit(init);
        if (staticInit._offset > offset) {
            Emit(".zero %d", staticInit._offset - offset);
            offset += staticInit._width;
        }

        switch (staticInit._width) {
        case 1:
            Emit(".byte %d", static_cast<char>(staticInit._val));
            break;
        case 2:
            Emit(".value %d", static_cast<short>(staticInit._val));
            break;
        case 4:
            Emit(".long %d", static_cast<int>(staticInit._val));
            break;
        case 8: 
            if (staticInit._label.size() == 0) {
                Emit(".quad %ld", staticInit._val);
            } else if (staticInit._val != 0) {
                Emit(".quad %s+%ld", staticInit._label.c_str(), staticInit._val);
            } else {
                Emit(".quad %s", staticInit._label.c_str());
            }
            break;
        default: assert(false);
        }
    }
}


void Generator::VisitEmptyStmt(EmptyStmt* emptyStmt)
{

}


void Generator::VisitIfStmt(IfStmt* ifStmt)
{

}


void Generator::VisitJumpStmt(JumpStmt* jumpStmt)
{
    Emit("jmp %s", jumpStmt->_label->Label().c_str());
}


void Generator::VisitLabelStmt(LabelStmt* labelStmt)
{
    EmitLabel(labelStmt->Label());
}


void Generator::VisitReturnStmt(ReturnStmt* returnStmt)
{
    auto expr = returnStmt->_expr;
    Visit(expr);
    if (expr->Type()->ToStructUnionType()) {
        // %r10 now has the address of the struct/union
        
        ObjectAddr addr = {"", "rbp", _retAddrOffset};
        Emit("movq %s, #r11", addr.Repr().c_str());
        addr = {"", "r11", 0};
        CopyStruct(addr, expr->Type()->Width());
        Emit("movq #r11, #r10");
    }

    Emit("leave");
    Emit("ret");
}


class Comp
{
public:
    bool operator()(Object* lhs, Object* rhs) {
        return lhs->Type()->Align() < rhs->Type()->Align();
    }
};


int Generator::AllocObjects(int baseOffset, Scope* scope,
        const FuncDef::ParamList& params)
{
    int offset = baseOffset;

    auto paramSet = std::set<Object*>(params.begin(), params.end());
    std::priority_queue<Object*, std::vector<Object*>, Comp> heap;
    for (auto iter = scope->begin(); iter != scope->end(); iter++) {
        auto obj = iter->second->ToObject();
        if (!obj || obj->IsStatic())
            continue;
        if (paramSet.find(obj) != paramSet.end())
            continue;
        heap.push(obj);
    }

    while (!heap.empty()) {
        auto obj = heap.top();
        heap.pop();

        offset -= obj->Type()->Width();
        offset = Type::MakeAlign(offset, obj->Type()->Align());
        obj->SetOffset(offset);
    }

    return offset;
}


void Generator::VisitCompoundStmt(CompoundStmt* compStmt)
{
    if (compStmt->_scope) {
        //compStmt
        _offset = AllocObjects(_offset, compStmt->_scope);
    }

    for (auto stmt: compStmt->_stmts) {
        Visit(stmt);
    }
}


void Generator::VisitFuncCall(FuncCall* funcCall)
{
    auto base = _offset;
    // Alloc memory for return value if it is struct/union
    auto funcType = funcCall->FuncType();
    auto retType = funcCall->Type()->ToStructUnionType();
    if (retType) {
        auto offset = _offset;
        offset -= funcCall->Type()->Width();
        offset = Type::MakeAlign(offset, funcCall->Type()->Align());
        Emit("leaq %d(#rbp), #rdi", offset);
        
        //Emit("subq $%d, #rsp", _offset - offset);
        _offset = offset;
    }

    std::vector<Type*> types;
    for (auto arg: funcCall->_args)
        types.push_back(arg->Type());
    
    auto locations = GetParamLocation(types, retType);
    //auto beforePass = _offset;
    for (int i = locations.size() - 1; i >=0; i--) {
        if (locations[i][0] == 'm') {
            Visit(funcCall->_args[i]);
            if (types[i]->IsFloat())
                Push("xmm8");
            else
                Push("r10");
        }
    }

    int fltCnt = 0;
    for (int i = locations.size() - 1; i >= 0; i--) {
        if (locations[i][0] == 'm')
            continue;
        Visit(funcCall->_args[i]);
        
        if (locations[i][0] == 'x') {
            ++fltCnt;
            auto inst = GetInst("mov", types[i]);
            Emit("%s #xmm8, #%s", inst.c_str(), locations[i]);
        } else {
            Emit("movq #r10, #%s", locations[i]);
        }
    }

    // If variadic, set %al to floating param number
    if (funcType->Variadic()) {
        Emit("movq $%d, %rax", fltCnt);
    }
    Emit("leaq %d(#rbp), #rsp", _offset);
    Emit("call %s", funcCall->Name().c_str());
    //Emit("leaq %d(#rbp), #rsp", beforePass);

    _offset = base;    
}


std::vector<const char*> Generator::GetParamLocation(
        FuncType::TypeList& types, bool retStruct)
{
    std::vector<const char*> locations;

    size_t argOnStackOffset = 8;
    size_t regCnt = retStruct, xregCnt = 0;
    for (auto type: types) {
        auto cls = Classify(type);

        const char* reg = nullptr;
        if (cls == ParamClass::INTEGER) {
            if (regCnt < regs.size())
                reg = regs[regCnt++];
        } else if (cls == ParamClass::SSE) {
            if (xregCnt < xregs.size())
                reg = xregs[xregCnt++];
        }
        locations.push_back(reg ? reg: "mem");
    }
    return locations;
}


void Generator::VisitFuncDef(FuncDef* funcDef)
{
    auto name = funcDef->Name();

    Emit(".text");
    if (funcDef->Linkage() == L_INTERNAL)
        Emit(".local %s", name.c_str());
    else
        Emit(".globl %s", name.c_str());
    Emit(".type %s, @function", name.c_str());

    EmitLabel(name);
    Emit("pushq #rbp");
    Emit("movq #rsp, #rbp");


    FuncDef::ParamList& params = funcDef->Params();

    _offset = 0;
    int offset = _offset;

    // Arrange space to store params passed by registers
    auto retType = funcDef->Type()->Derived()->ToStructUnionType();
    auto locations = GetParamLocation(funcDef->Type()->ParamTypes(), retType);

    if (funcDef->Type()->Variadic()) {
        offset = GenSaveArea(); // 'offset' is now the begin of save area
        int regOffset = retType ? offset + 8: offset;
        int xregOffset = offset + 8 * 8;
        int byMemOffset = 16;
        for (size_t i = 0; i < locations.size(); i++) {
            if (locations[i][0] == 'm') {
                params[i]->SetOffset(byMemOffset);
                byMemOffset += 8;
            } else if (locations[i][0] == 'x') {
                params[i]->SetOffset(xregOffset);
                xregOffset += 16;
            } else {
                params[i]->SetOffset(regOffset);
                regOffset += 8;
            }
        }
    } else {
        int byMemOffset = 16;
        for (size_t i = 0; i < locations.size(); i++) {
            if (locations[i][0] == 'm') {
                params[i]->SetOffset(byMemOffset);
                byMemOffset += 8;
                continue;
            }
            params[i]->SetOffset(Push(locations[i]));
        }
    }

    offset = AllocObjects(offset, funcDef->Body()->Scope(), params);
    
    //Emit("subq $%d, #rsp", _offset - offset);
    _offset = offset;

    for (auto stmt: funcDef->_body->_stmts) {
        Visit(stmt);
    }
}


int Generator::GenSaveArea(void)
{
    static const int begin = -176;
    int offset = begin;
    for (auto reg: regs) {
        Emit("movq #%s, %d(rbp)", reg, offset);
        offset += 8;
    }
    Emit("testb #al, #al");
    auto label = LabelStmt::New();
    Emit("je %s", label->Label().c_str());
    for (auto xreg: xregs) {
        Emit("movaps #%s, %d(rbp)", xreg, offset);
        offset += 16;
    }
    assert(offset == 0);
    EmitLabel(label->Label());

    return begin;
}


void Generator::VisitTranslationUnit(TranslationUnit* unit)
{
    for (auto extDecl: unit->ExtDecls()) {
        Visit(extDecl);

        if (_rodatas.size())
            Emit(".section .rodata");
        for (auto rodata: _rodatas) {
            if (rodata._align == 1) {// Literal
                EmitLabel(rodata._label);
                Emit(".string \"%s\"", rodata._sval.c_str());
            } else if (rodata._align == 4) {
                Emit(".align 4");
                EmitLabel(rodata._label);
                Emit(".long %d", static_cast<int>(rodata._ival));
            } else {
                Emit(".align 8");
                EmitLabel(rodata._label);
                Emit(".quad %ld", rodata._ival);
            }
        }
        _rodatas.clear();

        for (auto staticDecl: _staticDecls) {
            GenStaticDecl(staticDecl);
        }
        _staticDecls.clear();
    }
}


void Generator::Gen(void)
{
    Emit(".file \"%s\"", inFileName.c_str());

/*
    for (auto obj: _parser->StaticObjects()) {
        auto label = ObjectLabel(obj);
        auto width = obj->Type()->Width();
        auto align = obj->Type()->Align();

        // omit the external without initilizer
        if ((obj->Storage() & S_EXTERN) && !obj->Init())
            continue;

        auto glb = obj->Linkage() == L_EXTERNAL ? ".globl": ".local";
        Emit("%s %s", glb, label.c_str());

        if (!obj->Init()) {    
            Emit(".comm %s, %d, %d", label.c_str(), width, align);
        } else {
            Emit(".align %d", align);
            Emit(".type %s, @object", label.c_str());
            Emit(".size %s, %d", label.c_str(), width);
            EmitLabel(label.c_str());
            VisitDeclaration(obj->Init());
        }

    }
*/

    VisitTranslationUnit(_parser->Unit());
}


std::string Generator::EmitLoad(const std::string& addr, Type* type)
{
    assert(type->IsScalar());

    auto width = type->Width();
    auto flt = type->IsFloat();
    auto load = GetLoad(width, flt);
    const char* src;
    const char* des;
    GetOperands(src, des, width, flt);
    Emit("%s %s, #%s", load, addr.c_str(), des);
    return des;
}


void Generator::EmitStore(const std::string& addr, Type* type)
{
    auto store = GetInst("mov", type);
    const char* src;
    const char* des;
    GetOperands(src, des, type->Width(), type->IsFloat());
    Emit("%s #%s, %s", store.c_str(), des, addr.c_str());
}


void Generator::Emit(const char* format, ...)
{
    fprintf(_outFile, "\t");

    std::string str(format);
    auto pos = str.find(' ');
    if (pos != std::string::npos) {
        str[pos] = '\t';
        while ((pos = str.find('#', pos)) != std::string::npos)
            str.replace(pos, 1, "%%");
    }
    
    va_list args;
    va_start(args, format);
    vfprintf(_outFile, str.c_str(), args);
    va_end(args);
    fprintf(_outFile, "\n");
}


void Generator::EmitLabel(const std::string& label)
{
    fprintf(_outFile, "%s:\n", label.c_str());
}



void LValGenerator::VisitBinaryOp(BinaryOp* binary)
{
    assert(binary->_op == '.');

    auto addr = LValGenerator().GenExpr(binary->_lhs);
    auto name = binary->_rhs->Tok()->Str();
    auto structType = binary->_lhs->Type()->ToStructUnionType();
    auto member = structType->GetMember(name);
    addr._offset += member->Offset();

    _addr = addr;
}


void LValGenerator::VisitUnaryOp(UnaryOp* unary)
{
    assert(unary->_op == Token::DEREF);
    Generator().GenExpr(unary->_operand);
    Emit("movq #r10, #r11");

    _addr = {"", "r11", 0};
}


void LValGenerator::VisitObject(Object* obj)
{
    if (obj->IsStatic()) {
        _addr = {ObjectLabel(obj), "rip", 0};
    } else {
        _addr = {"", "rbp", obj->Offset()};
    }
}


// The identifier must be function
void LValGenerator::VisitIdentifier(Identifier* ident)
{
    assert(!ident->ToTypeName());

    // Function address
    _addr = {ident->Name(), "rip", 0};
}



std::string ObjectAddr::Repr(void) const
{
    auto ret = "(%" + _base + ")";
    if (_label.size() == 0) {
        if (_offset == 0)
            return ret;
        else
            return std::to_string(_offset) + ret;
    } else {
        if (_offset == 0)
            return _label + ret;
        else
            return _label + "+" + std::to_string(_offset) + ret;
    }
}




StaticInitializer Generator::GetStaticInit(const Initializer& init)
{
    // Delay until code gen
    auto width = init._type->Width();
    if (init._type->IsInteger()) {
        auto val = Evaluator<long>().Eval(init._expr);
        return {init._offset, width, val, ""};
    } else if (init._type->IsFloat()) {
        auto val = Evaluator<double>().Eval(init._expr);
        auto lval = *reinterpret_cast<long*>(&val);
        return {init._offset, width, lval, ""};
    } else if (init._type->ToPointerType()) {
        auto addr = Evaluator<Addr>().Eval(init._expr);
        return {init._offset, width, addr._offset, addr._label};
    } else {
        assert(false);
        return StaticInitializer(); //Make compiler happy
    }
}