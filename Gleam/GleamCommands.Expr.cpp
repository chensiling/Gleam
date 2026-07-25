// Address expression evaluation. Shared by every address-taking command
// through parseAddress. Supported grammar (no spaces needed):
//
//   expr  := unary (('+'|'-') unary)*
//   unary := '-' unary | '[' expr ']' | '(' expr ')' | atom
//   atom  := hex literal | register | module | module!symbol
//
// '[expr]' dereferences a pointer (8 bytes) in the debuggee. A bare module
// name evaluates to its base address, so "kernel32+1234" is module+RVA.
// Arithmetic wraps (unsigned), matching debugger convention.

#include "GleamDebugger.h"

#include <cstdio>
#include <cstring>

using namespace GleeBug;

// Read 8 bytes for the '[expr]' dereference.
bool GleamDebugger::exprReadPointer(uint64_t addr, uint64_t & out)
{
    if(!mProcess)
        return false;
    return mProcess->MemReadSafe(addr, &out, sizeof(out));
}

bool GleamDebugger::exprParseAtom(const std::string & s, size_t & pos, uint64_t & out, std::string & err)
{
    size_t start = pos;
    while(pos < s.size() && strchr("+-()[] \t", s[pos]) == nullptr)
        pos++;
    const std::string tok = s.substr(start, pos - start);
    if(tok.empty())
    {
        err = "expected a value";
        return false;
    }

    // module!symbol (dbghelp)
    if(tok.find('!') != std::string::npos)
    {
        if(resolveModuleSymbol(tok, out))
            return true;
        err = "unknown symbol '" + tok + "'";
        return false;
    }

    // hex literal (strtoull rejects names containing non-hex letters)
    if(parseHex(tok, out))
        return true;

    // register of the current thread
    RegId reg;
    if(registerByName(tok, reg))
    {
        Thread* thread = currentThread();
        if(!thread)
        {
            err = "no current thread";
            return false;
        }
        Registers r(thread->hThread);
        out = r.Get(reg);
        return true;
    }

    // module base
    if(moduleBaseByName(tok, out))
        return true;

    err = "unknown name '" + tok + "'";
    return false;
}

bool GleamDebugger::exprParseUnary(const std::string & s, size_t & pos, uint64_t & out, std::string & err)
{
    // skip whitespace
    while(pos < s.size() && (s[pos] == ' ' || s[pos] == '\t'))
        pos++;

    if(pos < s.size() && s[pos] == '-')
    {
        pos++;
        if(!exprParseUnary(s, pos, out, err))
            return false;
        out = 0 - out;
        return true;
    }
    if(pos < s.size() && s[pos] == '[')
    {
        pos++;
        uint64_t addr = 0;
        if(!exprParseSum(s, pos, addr, err))
            return false;
        while(pos < s.size() && (s[pos] == ' ' || s[pos] == '\t'))
            pos++;
        if(pos >= s.size() || s[pos] != ']')
        {
            err = "missing ']'";
            return false;
        }
        pos++;
        if(!exprReadPointer(addr, out))
        {
            char buf[96];
            sprintf_s(buf, "cannot read memory at 0x%llX", (unsigned long long)addr);
            err = buf;
            return false;
        }
        return true;
    }
    if(pos < s.size() && s[pos] == '(')
    {
        pos++;
        if(!exprParseSum(s, pos, out, err))
            return false;
        while(pos < s.size() && (s[pos] == ' ' || s[pos] == '\t'))
            pos++;
        if(pos >= s.size() || s[pos] != ')')
        {
            err = "missing ')'";
            return false;
        }
        pos++;
        return true;
    }
    return exprParseAtom(s, pos, out, err);
}

bool GleamDebugger::exprParseSum(const std::string & s, size_t & pos, uint64_t & out, std::string & err)
{
    if(!exprParseUnary(s, pos, out, err))
        return false;
    for(;;)
    {
        size_t save = pos;
        while(pos < s.size() && (s[pos] == ' ' || s[pos] == '\t'))
            pos++;
        if(pos >= s.size() || (s[pos] != '+' && s[pos] != '-'))
        {
            pos = save;
            return true;
        }
        const char op = s[pos++];
        uint64_t rhs = 0;
        if(!exprParseUnary(s, pos, rhs, err))
            return false;
        if(op == '+')
            out += rhs;
        else
            out -= rhs;
    }
}

bool GleamDebugger::evalExpression(const std::string & s, uint64_t & out, std::string & err)
{
    if(s.empty())
    {
        err = "empty expression";
        return false;
    }
    size_t pos = 0;
    if(!exprParseSum(s, pos, out, err))
        return false;
    while(pos < s.size() && (s[pos] == ' ' || s[pos] == '\t'))
        pos++;
    if(pos != s.size())
    {
        err = "unexpected trailing characters";
        return false;
    }
    return true;
}
