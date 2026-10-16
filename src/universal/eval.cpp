#include <universal/bo1_ptrcast.h>
#include "eval.h"
#include "assertive.h"
#include <cstdlib>
#include <string.h>
#include "q_shared.h"

#include <cmath>

int s_consumedOperandCount[26] =
{
  1,
  -1,
  2,
  0,
  1,
  1,
  0,
  0,
  1,
  1,
  1,
  1,
  1,
  0,
  1,
  1,
  1,
  0,
  1,
  1,
  1,
  1,
  1,
  1,
  1,
  1
};

char s_precedence[26] =
{
  'c',
  '\0',
  '\x03',
  '\x02',
  '\v',
  '\v',
  '\r',
  '\r',
  '\f',
  '\f',
  '\f',
  '\n',
  '\n',
  '\r',
  '\a',
  '\x05',
  '\x06',
  '\r',
  '\x02',
  '\x01',
  '\b',
  '\b',
  '\t',
  '\t',
  '\t',
  '\t'
};

bool s_rightToLeft[26] =
{
  false,
  false,
  true,
  true,
  false,
  false,
  true,
  true,
  false,
  false,
  false,
  false,
  false,
  true,
  false,
  false,
  false,
  true,
  false,
  false,
  false,
  false,
  false,
  false,
  false,
  false
};

bool __cdecl Eval_AnyMissingOperands(const Eval *eval)
{
    int opIndex; // [esp+0h] [ebp-8h]
    int requiredOperandCount; // [esp+4h] [ebp-4h]

    requiredOperandCount = 1;
    for ( opIndex = 0; opIndex < eval->opStackPos; ++opIndex )
        requiredOperandCount += s_consumedOperandCount[eval->opStack[opIndex]];
    return requiredOperandCount != eval->valStackPos;
}

// The exe throws the error text as a C++ exception (const char *) and the menu expression parsers catch it; the
// decompiled bodies had lost that, along with every value-stack access (they did byte offsets on an Eval *).
[[noreturn]] static void Eval_Throw(const char *message)
{
    throw EvalException{ message };
}

static const char *const EVAL_MISSING_OPERAND = "missing operand (for example, 'a + ' or ' / b')";

char    Eval_PushOperator(Eval *eval, EvalOperatorType op)
{
    bool v4; // [esp+0h] [ebp-10h]
    bool leftToRight; // [esp+Dh] [ebp-3h]
    char precedence; // [esp+Eh] [ebp-2h]
    bool higherPrecedence; // [esp+Fh] [ebp-1h]

    if ( (unsigned int)op >= EVAL_OP_COUNT || s_precedence[op] < 0 )
        return 0;
    if ( op == EVAL_OP_RPAREN && !eval->parenCount )
        return 0;
    if ( op == EVAL_OP_LPAREN )
    {
        if ( eval->valStackPos && !eval->pushedOp )
            return 0;
        ++eval->parenCount;
    }
    if ( op == EVAL_OP_PLUS )
    {
        if ( Eval_IsUnaryOp(eval) )
            op = EVAL_OP_UNARY_PLUS;
    }
    else if ( op == EVAL_OP_MINUS && Eval_IsUnaryOp(eval) )
    {
        op = EVAL_OP_UNARY_MINUS;
    }
    precedence = s_precedence[op];
    while ( eval->opStackPos > 0 )
    {
        higherPrecedence = s_precedence[eval->opStack[eval->opStackPos - 1]] > precedence;
        v4 = s_precedence[eval->opStack[eval->opStackPos - 1]] == precedence
            && !s_rightToLeft[eval->opStack[eval->opStackPos - 1]];
        leftToRight = v4;
        if ( !higherPrecedence && !leftToRight )
            break;
        if ( eval->opStack[eval->opStackPos - 1] == EVAL_OP_LPAREN )
        {
            if ( op == EVAL_OP_RPAREN )
            {
                --eval->parenCount;
                --eval->opStackPos;
                eval->pushedOp = 0;
                return 1;
            }
            break;
        }
        if ( !Eval_EvaluationStep(eval) )
            return 0;
    }
    if ( op != EVAL_OP_COLON || eval->opStackPos && eval->opStack[eval->opStackPos - 1] == EVAL_OP_QUESTION )
    {
        if ( eval->opStackPos == 1024 )
            Eval_Throw("evaluation stack overflow - expression is too complex");
        eval->opStack[eval->opStackPos++] = op;
        eval->pushedOp = 1;
        return 1;
    }
    if ( eval->parenCount )
        Eval_Throw("found ':' without preceding '?' in expression of type 'a ? b : c'");
    return 0;
}

static void Eval_FreeString(EvalValue *value)
{
    free(value->u.s.get());
    value->u.s = nullptr;
}

bool    Eval_EvaluationStep(Eval *eval)
{
    EvalOperatorType op;
    EvalValue *a;
    EvalValue *b;
    int n;

    if ( !eval->opStackPos )
        return 0;
    op = eval->opStack[--eval->opStackPos];
    if ( op == EVAL_OP_LPAREN )
        return 1;
    if ( op == EVAL_OP_QUESTION )
        Eval_Throw("found '?' with no following ':' in expression of type 'a ? b : c'");
    n = eval->valStackPos;
    if ( !n )
        Eval_Throw(EVAL_MISSING_OPERAND);
    // a: second operand from the top, b: top of the value stack (the only operand of the unary operators)
    a = n >= 2 ? &eval->valStack[n - 2] : nullptr;
    b = &eval->valStack[n - 1];
    switch ( op )
    {
    case EVAL_OP_COLON:
    {
        EvalValue *cond;
        int chosen;
        if ( n < 3 )
            Eval_Throw(EVAL_MISSING_OPERAND);
        cond = &eval->valStack[n - 3];
        if ( cond->type == EVAL_VALUE_INT )
            chosen = cond->u.i != 0 ? n - 2 : n - 1;
        else if ( cond->type == EVAL_VALUE_DOUBLE )
            chosen = cond->u.d != 0.0 ? n - 2 : n - 1;
        else
            Eval_Throw("can only switch on numbers");
        if ( a->type == EVAL_VALUE_STRING && b->type == EVAL_VALUE_STRING )
            Eval_FreeString(&eval->valStack[(n - 2) + (n - 1) - chosen]);
        else
            Eval_PrepareBinaryOpSameTypes(eval);
        *cond = eval->valStack[chosen];
        eval->valStackPos = n - 2;
        // the '?' this ':' belongs to
        if ( eval->opStackPos > 0 )
            --eval->opStackPos;
        return 1;
    }
    case EVAL_OP_PLUS:
        if ( a && a->type == EVAL_VALUE_STRING && b->type == EVAL_VALUE_STRING )
        {
            const char *left = a->u.s.get();
            const char *right = b->u.s.get();
            size_t leftLen = strlen(left);
            size_t rightLen = strlen(right);
            char *s = (char *)malloc(leftLen + rightLen + 1);
            memcpy(s, left, leftLen);
            memcpy(s + leftLen, right, rightLen + 1);
            Eval_FreeString(a);
            Eval_FreeString(b);
            a->u.s = s;
        }
        else
        {
            Eval_PrepareBinaryOpSameTypes(eval);
            if ( a->type == EVAL_VALUE_INT )
                a->u.i += b->u.i;
            else
                a->u.d = a->u.d + b->u.d;
        }
        --eval->valStackPos;
        return 1;
    case EVAL_OP_MINUS:
        Eval_PrepareBinaryOpSameTypes(eval);
        if ( a->type == EVAL_VALUE_INT )
            a->u.i -= b->u.i;
        else
            a->u.d = a->u.d - b->u.d;
        --eval->valStackPos;
        return 1;
    case EVAL_OP_UNARY_PLUS:
        return 1;
    case EVAL_OP_UNARY_MINUS:
        if ( b->type == EVAL_VALUE_INT )
            b->u.i = -b->u.i;
        else if ( b->type == EVAL_VALUE_STRING )
            Eval_Throw("cannot negate strings");
        else
            b->u.d = -b->u.d;
        return 1;
    case EVAL_OP_MULTIPLY:
        Eval_PrepareBinaryOpSameTypes(eval);
        if ( a->type == EVAL_VALUE_INT )
            a->u.i *= b->u.i;
        else
            a->u.d = a->u.d * b->u.d;
        --eval->valStackPos;
        return 1;
    case EVAL_OP_DIVIDE:
        Eval_PrepareBinaryOpSameTypes(eval);
        if ( a->type == EVAL_VALUE_INT )
        {
            if ( !b->u.i )
                Eval_Throw("divide by zero");
            a->u.i /= b->u.i;
        }
        else
        {
            if ( b->u.d == 0.0 )
                Eval_Throw("divide by zero");
            a->u.d = a->u.d / b->u.d;
        }
        --eval->valStackPos;
        return 1;
    case EVAL_OP_MODULUS:
        Eval_PrepareBinaryOpSameTypes(eval);
        if ( a->type == EVAL_VALUE_INT )
        {
            if ( !b->u.i )
                Eval_Throw("divide by zero");
            a->u.i %= b->u.i;
        }
        else
        {
            double quotientFloor;
            if ( b->u.d == 0.0 )
                Eval_Throw("divide by zero");
            quotientFloor = floor((double)a->u.d / (double)b->u.d);
            a->u.d = a->u.d - b->u.d * quotientFloor;
        }
        --eval->valStackPos;
        return 1;
    case EVAL_OP_LSHIFT:
        Eval_PrepareBinaryOpSameTypes(eval);
        if ( a->type == EVAL_VALUE_INT )
            a->u.i <<= b->u.i;
        else
            a->u.d = a->u.d * pow(2.0, (double)b->u.d);
        --eval->valStackPos;
        return 1;
    case EVAL_OP_RSHIFT:
        Eval_PrepareBinaryOpSameTypes(eval);
        if ( a->type == EVAL_VALUE_INT )
            a->u.i >>= b->u.i;
        else
            a->u.d = a->u.d * pow(2.0, -(double)b->u.d);
        --eval->valStackPos;
        return 1;
    case EVAL_OP_BITWISE_NOT:
        if ( b->type == EVAL_VALUE_STRING )
            Eval_Throw("cannot bitwise invert strings");
        if ( b->type == EVAL_VALUE_DOUBLE )
        {
            b->u.i = (int)b->u.d;
            b->type = EVAL_VALUE_INT;
        }
        b->u.i = ~b->u.i;
        return 1;
    case EVAL_OP_BITWISE_AND:
        Eval_PrepareBinaryOpIntegers(eval);
        a->u.i &= b->u.i;
        --eval->valStackPos;
        return 1;
    case EVAL_OP_BITWISE_OR:
        Eval_PrepareBinaryOpIntegers(eval);
        a->u.i |= b->u.i;
        --eval->valStackPos;
        return 1;
    case EVAL_OP_BITWISE_XOR:
        Eval_PrepareBinaryOpIntegers(eval);
        a->u.i ^= b->u.i;
        --eval->valStackPos;
        return 1;
    case EVAL_OP_LOGICAL_NOT:
        if ( b->type == EVAL_VALUE_INT )
            b->u.i = b->u.i == 0;
        else if ( b->type == EVAL_VALUE_STRING )
            Eval_Throw("cannot logical invert strings");
        else
            b->u.i = b->u.d == 0.0;
        b->type = EVAL_VALUE_INT;
        return 1;
    case EVAL_OP_LOGICAL_AND:
        Eval_PrepareBinaryOpBoolean(eval);
        a->u.i &= b->u.i;
        --eval->valStackPos;
        return 1;
    case EVAL_OP_LOGICAL_OR:
        Eval_PrepareBinaryOpBoolean(eval);
        a->u.i |= b->u.i;
        --eval->valStackPos;
        return 1;
    case EVAL_OP_EQUALS:
    case EVAL_OP_NOT_EQUAL:
    {
        bool same;
        if ( a && a->type == EVAL_VALUE_STRING && b->type == EVAL_VALUE_STRING )
        {
            same = I_stricmp(a->u.s.get(), b->u.s.get()) == 0;
            Eval_FreeString(a);
            Eval_FreeString(b);
        }
        else
        {
            Eval_PrepareBinaryOpSameTypes(eval);
            if ( a->type == EVAL_VALUE_INT )
                same = a->u.i == b->u.i;
            else
                same = a->u.d == b->u.d;
        }
        a->type = EVAL_VALUE_INT;
        a->u.i = op == EVAL_OP_EQUALS ? same : !same;
        --eval->valStackPos;
        return 1;
    }
    case EVAL_OP_LESS:
    case EVAL_OP_LESS_EQUAL:
    case EVAL_OP_GREATER:
    case EVAL_OP_GREATER_EQUAL:
    {
        int cmp;
        Eval_PrepareBinaryOpSameTypes(eval);
        if ( a->type == EVAL_VALUE_INT )
            cmp = a->u.i < b->u.i ? -1 : a->u.i > b->u.i;
        else
            cmp = a->u.d < b->u.d ? -1 : a->u.d > b->u.d ? 1 : a->u.d == b->u.d ? 0 : 2; // 2: unordered (NaN)
        switch ( op )
        {
        case EVAL_OP_LESS:
            a->u.i = cmp == -1;
            break;
        case EVAL_OP_LESS_EQUAL:
            a->u.i = cmp == -1 || cmp == 0;
            break;
        case EVAL_OP_GREATER:
            a->u.i = cmp == 1;
            break;
        default:
            a->u.i = cmp == 1 || cmp == 0;
            break;
        }
        a->type = EVAL_VALUE_INT;
        --eval->valStackPos;
        return 1;
    }
    default:
        if ( !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\universal\\eval.cpp", 488, 0, "unknown operator type") )
            __debugbreak();
        return 0;
    }
}

static void Eval_CheckBinaryOperands(const Eval *eval)
{
    if ( eval->valStackPos < 2 )
        Eval_Throw(EVAL_MISSING_OPERAND);
    if ( eval->valStack[eval->valStackPos - 2].type == EVAL_VALUE_STRING
        || eval->valStack[eval->valStackPos - 1].type == EVAL_VALUE_STRING )
    {
        Eval_Throw("operation not valid on strings");
    }
}

void __cdecl Eval_PrepareBinaryOpSameTypes(Eval *eval)
{
    EvalValue *a;
    EvalValue *b;

    Eval_CheckBinaryOperands(eval);
    a = &eval->valStack[eval->valStackPos - 2];
    b = &eval->valStack[eval->valStackPos - 1];
    if ( a->type != b->type )
    {
        // mixed int / double: the int side becomes a double
        EvalValue *intSide = a->type == EVAL_VALUE_INT ? a : b;
        intSide->u.d = (double)intSide->u.i;
        intSide->type = EVAL_VALUE_DOUBLE;
    }
}

void __cdecl Eval_PrepareBinaryOpIntegers(Eval *eval)
{
    int i;

    Eval_CheckBinaryOperands(eval);
    for ( i = eval->valStackPos - 2; i < eval->valStackPos; ++i )
    {
        if ( eval->valStack[i].type == EVAL_VALUE_DOUBLE )
        {
            eval->valStack[i].u.i = (int)eval->valStack[i].u.d;
            eval->valStack[i].type = EVAL_VALUE_INT;
        }
    }
}

void __cdecl Eval_PrepareBinaryOpBoolean(Eval *eval)
{
    int i;

    Eval_CheckBinaryOperands(eval);
    for ( i = eval->valStackPos - 2; i < eval->valStackPos; ++i )
    {
        if ( eval->valStack[i].type == EVAL_VALUE_DOUBLE )
            eval->valStack[i].u.i = eval->valStack[i].u.d != 0.0;
        else
            eval->valStack[i].u.i = eval->valStack[i].u.i != 0;
        eval->valStack[i].type = EVAL_VALUE_INT;
    }
}

bool __cdecl Eval_IsUnaryOp(const Eval *eval)
{
    return !eval->valStackPos || eval->pushedOp;
}

char __cdecl Eval_PushInteger(Eval *eval, int value)
{
    if ( !Eval_CanPushValue(eval) )
        return 0;
    eval->valStack[eval->valStackPos].type = EVAL_VALUE_INT;
    eval->valStack[eval->valStackPos++].u.i = value;
    eval->pushedOp = 0;
    return 1;
}

bool __cdecl Eval_CanPushValue(const Eval *eval)
{
    if ( !eval->valStackPos )
        return 1;
    if ( eval->valStackPos == 1024 )
        Eval_Throw("evaluation stack overflow - expression is too complex");
    return eval->pushedOp;
}

char __cdecl Eval_PushNumber(Eval *eval, long double value)
{
    if ( !Eval_CanPushValue(eval) )
        return 0;
    eval->valStack[eval->valStackPos].type = EVAL_VALUE_DOUBLE;
    eval->valStack[eval->valStackPos++].u.d = value;
    eval->pushedOp = 0;
    return 1;
}

EvalValue * Eval_Solve(EvalValue *result, Eval *eval)
{
    if ( eval->parenCount )
        Eval_Throw("missing ')'");
    while ( Eval_EvaluationStep(eval) )
        ;
    if ( eval->opStackPos
        && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\universal\\eval.cpp",
                    649,
                    0,
                    "%s",
                    "eval->opStackPos == 0") )
    {
        __debugbreak();
    }
    if ( eval->valStackPos > 1 )
        Eval_Throw("extra operand (for example, 'a b +')");
    if ( !eval->valStackPos )
        Eval_Throw(EVAL_MISSING_OPERAND);
    *result = eval->valStack[0];
    return result;
}

bool __cdecl Eval_OperatorForToken(const char *text, EvalOperatorType *op)
{
    bool result; // al

    if ( !text && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\universal\\eval.cpp", 674, 0, "%s", "text") )
        __debugbreak();
    if ( !op && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\universal\\eval.cpp", 675, 0, "%s", "op") )
        __debugbreak();
    switch ( *text )
    {
        case '!':
            if ( text[1] == '=' )
                *op = EVAL_OP_NOT_EQUAL;
            else
                *op = EVAL_OP_LOGICAL_NOT;
            result = 1;
            break;
        case '%':
            *op = EVAL_OP_MODULUS;
            result = 1;
            break;
        case '&':
            if ( text[1] == '&' )
                *op = EVAL_OP_LOGICAL_AND;
            else
                *op = EVAL_OP_BITWISE_AND;
            result = 1;
            break;
        case '(':
            *op = EVAL_OP_LPAREN;
            result = 1;
            break;
        case ')':
            *op = EVAL_OP_RPAREN;
            result = 1;
            break;
        case '*':
            *op = EVAL_OP_MULTIPLY;
            result = 1;
            break;
        case '+':
            *op = EVAL_OP_PLUS;
            result = 1;
            break;
        case '-':
            *op = EVAL_OP_MINUS;
            result = 1;
            break;
        case '/':
            *op = EVAL_OP_DIVIDE;
            result = 1;
            break;
        case ':':
            *op = EVAL_OP_COLON;
            result = 1;
            break;
        case '<':
            if ( text[1] == '<' )
            {
                *op = EVAL_OP_LSHIFT;
                result = 1;
            }
            else
            {
                if ( text[1] == '=' )
                    *op = EVAL_OP_LESS_EQUAL;
                else
                    *op = EVAL_OP_LESS;
                result = 1;
            }
            break;
        case '=':
            if ( text[1] != '=' )
                goto LABEL_46;
            *op = EVAL_OP_EQUALS;
            result = 1;
            break;
        case '>':
            if ( text[1] == '>' )
            {
                *op = EVAL_OP_RSHIFT;
                result = 1;
            }
            else
            {
                if ( text[1] == 61 )
                    *op = EVAL_OP_GREATER_EQUAL;
                else
                    *op = EVAL_OP_GREATER;
                result = 1;
            }
            break;
        case '?':
            *op = EVAL_OP_QUESTION;
            result = 1;
            break;
        case '^':
            *op = EVAL_OP_BITWISE_XOR;
            result = 1;
            break;
        case '|':
            if ( text[1] == '|' )
                *op = EVAL_OP_LOGICAL_OR;
            else
                *op = EVAL_OP_BITWISE_OR;
            result = 1;
            break;
        case '~':
            *op = EVAL_OP_BITWISE_NOT;
            result = 1;
            break;
        default:
LABEL_46:
            result = 0;
            break;
    }
    return result;
}

