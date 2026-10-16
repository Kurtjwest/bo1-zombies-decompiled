#pragma once
#include <universal/ptr32.h>

enum parseSkip_t : __int32
{                                       // XREF: indent_s/r
    SKIP_NO        = 0x0,
    SKIP_YES       = 0x1,
    SKIP_ALL_ELIFS = 0x2,
};

struct punctuation_s // sizeof=0xC
{                                       // XREF: punctuation_t/r
    Ptr32<char> p;
    int n;
    Ptr32<punctuation_s> next;
};

struct __declspec(align(8)) token_s // sizeof=0x430
{                                       // XREF: script_s/r source_s/r ...
    char string[1024];                  // XREF: PC_Directive_include(source_s *)+1EF/r
                                        // PC_Directive_include(source_s *)+29E/r ...
    int type;                           // XREF: PC_Directive_include(source_s *):loc_46B2B8/r
                                        // PC_Directive_include(source_s *):loc_46B425/r ...
    int subtype;                        // XREF: UnreadSignToken(source_s *)+5B/w
                                        // PC_Directive_eval+8A/w ...
    unsigned int intvalue;              // XREF: PC_DollarDirective_evalint(source_s *)+94/w
                                        // PC_DollarDirective_evalfloat(source_s *)+E3/w ...
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    long double floatvalue;             // XREF: PC_DollarDirective_evalint(source_s *)+9C/w
                                        // PC_DollarDirective_evalfloat(source_s *)+EB/w ...
    Ptr32<char> whitespace_p;                 // XREF: UnreadSignToken(source_s *)+2E/w
                                        // PC_Directive_eval+4D/w ...
    Ptr32<char> endwhitespace_p;              // XREF: UnreadSignToken(source_s *)+3D/w
                                        // PC_Directive_eval+5C/w ...
    int line;                           // XREF: UnreadSignToken(source_s *)+1F/w
                                        // PC_Directive_eval+3E/w ...
    int linescrossed;                   // XREF: PC_Directive_include(source_s *):loc_46B29A/r
                                        // PC_Directive_include(source_s *)+27D/r ...
    Ptr32<token_s> next;
    // padding byte
    // padding byte
    // padding byte
    // padding byte
};

struct __declspec(align(8)) script_s // sizeof=0x4B0
{                                       // XREF: script_t/r
    char filename[64];
    Ptr32<char> buffer;
    Ptr32<char> script_p;
    Ptr32<char> end_p;
    Ptr32<char> lastscript_p;
    Ptr32<char> whitespace_p;
    Ptr32<char> endwhitespace_p;
    int length;
    int line;
    int lastline;
    int tokenavailable;
    int flags;
    Ptr32<punctuation_s> punctuations;
    Ptr32<Ptr32<punctuation_s>> punctuationtable;
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    token_s token;
    Ptr32<script_s> next;
    // padding byte
    // padding byte
    // padding byte
    // padding byte
};

struct define_s // sizeof=0x20
{                                       // XREF: define_t/r
    Ptr32<char> name;
    int flags;
    int builtin;
    int numparms;
    Ptr32<token_s> parms;
    Ptr32<token_s> tokens;
    Ptr32<define_s> next;
    Ptr32<define_s> hashnext;
};

struct indent_s // sizeof=0x10
{                                       // XREF: indent_t/r
    int type;
    parseSkip_t skip;
    Ptr32<script_s> script;
    Ptr32<indent_s> next;
};

struct source_s // sizeof=0x4D0
{                                       // XREF: source_t/r
                                        // ?PC_DefineFromString@@YAPAUdefine_s@@PBD@Z/r
    char filename[64];
    char includepath[64];
    Ptr32<punctuation_s> punctuations;
    Ptr32<script_s> scriptstack;              // XREF: PC_DefineFromString(char const *)+A5/w
    Ptr32<token_s> tokens;                    // XREF: PC_DefineFromString(char const *)+D0/r
                                        // PC_DefineFromString(char const *):loc_46C36B/r ...
    Ptr32<define_s> defines;                  // XREF: PC_DefineFromString(char const *):loc_46C404/r
    Ptr32<Ptr32<define_s>> definehash;              // XREF: PC_DefineFromString(char const *)+B8/w
                                        // PC_DefineFromString(char const *)+12F/r ...
    Ptr32<indent_s> indentstack;
    int skip;
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    token_s token;
};

struct __declspec(align(8)) value_s // sizeof=0x20
{                                       // XREF: value_t/r
    int intvalue;
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    long double floatvalue;
    int parentheses;
    Ptr32<value_s> prev;
    Ptr32<value_s> next;
    // padding byte
    // padding byte
    // padding byte
    // padding byte
};

struct operator_s // sizeof=0x14
{                                       // XREF: operator_t/r
    int op;
    int priority;
    int parentheses;
    Ptr32<operator_s> prev;
    Ptr32<operator_s> next;
};

void __cdecl PS_CreatePunctuationTable(script_s *script, punctuation_s *punctuations);
void ScriptError(script_s *script, const char *str, ...);
void ScriptWarning(script_s *script, const char *str, ...);
int __cdecl PS_ReadWhiteSpace(script_s *script);
int __cdecl PS_ReadEscapeCharacter(script_s *script, char *ch);
int __cdecl PS_ReadString(script_s *script, token_s *token, int quote);
int __cdecl PS_ReadName(script_s *script, token_s *token);
void __cdecl NumberValue(char *string, __int16 subtype, unsigned int *intvalue, long double *floatvalue);
int __cdecl PS_ReadNumber(script_s *script, token_s *token);
int __cdecl PS_ReadPunctuation(script_s *script, token_s *token);
int __cdecl PS_ReadPrimitive(script_s *script, token_s *token);
int __cdecl PS_ReadToken(script_s *script, token_s *token);
void __cdecl StripDoubleQuotes(char *string);
bool __cdecl EndOfScript(script_s *script);
script_s *__cdecl LoadScriptFile(const char *filename);
void __cdecl SetScriptPunctuations(script_s *script);
script_s *__cdecl LoadScriptMemory(char *ptr, unsigned int length, const char *name);
void __cdecl FreeScript(script_s *script);
