/// @file GleamCommands.Expr.cpp
/// @brief Address expression evaluator shared by every address-taking command
///        via parseAddress().
///
/// @section grammar Supported grammar (spaces not required)
/// @code
///   expr  := unary (('+'|'-') unary)*
///   unary := '-' unary | seg ':' '[' expr ']' | '[' expr ']' | '(' expr ')' | atom
///   seg   := 'gs' | 'fs' | 'ds' | 'es' | 'ss' | 'cs'
///   atom  := hex literal | register | module | module!symbol
/// @endcode
///
/// @note
///   - <tt>[expr]</tt> dereferences a pointer (8 bytes) from the debuggee.
///   - <tt>seg:[expr]</tt> adds the segment base first. In the x64 flat model
///     only GS has a non-zero base (the TEB), so <tt>gs:[60]</tt> is the PEB
///     pointer; the other prefixes are accepted and document intent.
///   - A bare module name evaluates to its load base, so
///     <tt>kernel32+1234</tt> means module_base + RVA.
///   - All arithmetic is unsigned and wraps, matching debugger convention.

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
        SymbolResult sr = resolveModuleSymbol(tok, out);
        if(sr == SymbolResult::Found)
            return true;
        if(sr == SymbolResult::Ambiguous)
        {
            // resolveModuleSymbol already printed the detailed error with both
            // addresses; don't duplicate it with a second "error: ..." line.
            // Return a marker error so the caller knows resolution failed.
            err = "ambiguous symbol '" + tok + "'";
        }
        else
            err = "unknown symbol '" + tok + "'";
        return false;
    }

    // hex literal (strtoull rejects names containing non-hex letters)
    if(parseHex(tok, out))
        return true;

    // It looked like a hex literal but did not parse: out of range.
    {
        const char* p = tok.c_str();
        if(p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
            p += 2;
        bool allHex = *p != '\0';
        for(const char* q = p; *q && allHex; q++)
            allHex = isxdigit((unsigned char)*q) != 0;
        if(allHex && (isdigit((unsigned char)*p) || tok.size() > 2))
        {
            err = "literal out of range: '" + tok + "'";
            return false;
        }
    }

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

// Parse "[expr]" and return the *address* (no dereference). Shared by the plain
// '[' case and the segment-prefixed one, which only adds a base.
// On entry s[pos] must be '['.
bool GleamDebugger::exprParseBracket(const std::string & s, size_t & pos, uint64_t & out, std::string & err)
{
    pos++; // '['
    if(!exprParseSum(s, pos, out, err))
        return false;
    while(pos < s.size() && (s[pos] == ' ' || s[pos] == '\t'))
        pos++;
    if(pos >= s.size() || s[pos] != ']')
    {
        err = "missing ']'";
        return false;
    }
    pos++;
    return true;
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
    // Segment-prefixed memory operand: "gs:[expr]", "ds:[expr]", ...
    // Only the segment base differs from a plain "[expr]"; in the x64 flat model
    // that base is 0 for every selector except GS (the TEB). Recognized before
    // the bare '[' case because the prefix is part of the same operand.
    if(pos + 3 < s.size() && s[pos + 2] == ':' && s[pos + 3] == '[' &&
       isalpha((unsigned char)s[pos]) && isalpha((unsigned char)s[pos + 1]))
    {
        const char seg[3] = { (char)tolower((unsigned char)s[pos]),
                              (char)tolower((unsigned char)s[pos + 1]), '\0' };
        uint64_t base = 0;
        bool known = true;
        if(strcmp(seg, "gs") == 0)
        {
            if(!segmentBase(true, base))
            {
                err = "no current thread";
                return false;
            }
            if(!base)
            {
                err = "gs base unknown (no TEB recorded for this thread)";
                return false;
            }
        }
        else if(strcmp(seg, "fs") == 0 || strcmp(seg, "ds") == 0 || strcmp(seg, "es") == 0 ||
                strcmp(seg, "ss") == 0 || strcmp(seg, "cs") == 0)
            base = 0; // flat model: base 0, the prefix only documents intent
        else
            known = false;

        if(known)
        {
            pos += 3; // consume "xx:", leaving '[' for the shared path below
            uint64_t off = 0;
            if(!exprParseBracket(s, pos, off, err))
                return false;
            if(!exprReadPointer(base + off, out))
            {
                char buf[96];
                sprintf_s(buf, "cannot read memory at 0x%llX", (unsigned long long)(base + off));
                err = buf;
                return false;
            }
            return true;
        }
    }
    if(pos < s.size() && s[pos] == '[')
    {
        uint64_t addr = 0;
        if(!exprParseBracket(s, pos, addr, err))
            return false;
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
