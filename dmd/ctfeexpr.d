/**
 * CTFE for expressions involving pointers, slices, array concatenation etc.
 *
 * Copyright:   Copyright (C) 1999-2021 by The D Language Foundation, All Rights Reserved
 * Authors:     $(LINK2 http://www.digitalmars.com, Walter Bright)
 * License:     $(LINK2 http://www.boost.org/LICENSE_1_0.txt, Boost License 1.0)
 * Source:      $(LINK2 https://github.com/dlang/dmd/blob/master/src/dmd/ctfeexpr.d, _ctfeexpr.d)
 * Documentation:  https://dlang.org/phobos/dmd_ctfeexpr.html
 * Coverage:    https://codecov.io/gh/dlang/dmd/src/master/src/dmd/ctfeexpr.d
 */

module dmd.ctfeexpr;

import core.stdc.stdio;
import core.stdc.stdlib;
import core.stdc.string;
import dmd.arraytypes;
import dmd.complex;
import dmd.constfold;
import dmd.compiler;
import dmd.dclass;
import dmd.declaration;
import dmd.dinterpret;
import dmd.dstruct;
import dmd.dtemplate;
import dmd.errors;
import dmd.expression;
import dmd.func;
import dmd.globals;
import dmd.mtype;
import dmd.root.ctfloat;
import dmd.root.port;
import dmd.root.rmem;
import dmd.tokens;
import dmd.visitor;


/***********************************************************
 * A reference to a class, or an interface. We need this when we
 * point to a base class (we must record what the type is).
 */
extern (C++) final class ClassReferenceExp : Expression
{
    StructLiteralExp value;

    extern (D) this(const ref Loc loc, StructLiteralExp lit, Type type)
    {
        super(loc, TOK.classReference, __traits(classInstanceSize, ClassReferenceExp));
        assert(lit && lit.sd && lit.sd.isClassDeclaration());
        this.value = lit;
        this.type = type;
    }

    ClassDeclaration originalClass()
    {
        return value.sd.isClassDeclaration();
    }

    // Return index of the field, or -1 if not found
    private int getFieldIndex(Type fieldtype, uint fieldoffset)
    {
        ClassDeclaration cd = originalClass();
        uint fieldsSoFar = 0;
        for (size_t j = 0; j < value.elements.dim; j++)
        {
            while (j - fieldsSoFar >= cd.fields.dim)
            {
                fieldsSoFar += cd.fields.dim;
                cd = cd.baseClass;
            }
            VarDeclaration v2 = cd.fields[j - fieldsSoFar];
            if (fieldoffset == v2.offset && fieldtype.size() == v2.type.size())
            {
                return cast(int)(value.elements.dim - fieldsSoFar - cd.fields.dim + (j - fieldsSoFar));
            }
        }
        return -1;
    }

    // Return index of the field, or -1 if not found
    // Same as getFieldIndex, but checks for a direct match with the VarDeclaration
    int findFieldIndexByName(VarDeclaration v)
    {
        ClassDeclaration cd = originalClass();
        size_t fieldsSoFar = 0;
        for (size_t j = 0; j < value.elements.dim; j++)
        {
            while (j - fieldsSoFar >= cd.fields.dim)
            {
                fieldsSoFar += cd.fields.dim;
                cd = cd.baseClass;
            }
            VarDeclaration v2 = cd.fields[j - fieldsSoFar];
            if (v == v2)
            {
                return cast(int)(value.elements.dim - fieldsSoFar - cd.fields.dim + (j - fieldsSoFar));
            }
        }
        return -1;
    }

    override void accept(Visitor v)
    {
        v.visit(this);
    }
}

/*************************
 * Same as getFieldIndex, but checks for a direct match with the VarDeclaration
 * Returns:
 *    index of the field, or -1 if not found
 */
int findFieldIndexByName(const StructDeclaration sd, const VarDeclaration v) pure
{
    foreach (i, field; sd.fields)
    {
        if (field == v)
            return cast(int)i;
    }
    return -1;
}

/***********************************************************
 * Fake class which holds the thrown exception.
 * Used for implementing exception handling.
 */
extern (C++) final class ThrownExceptionExp : Expression
{
    ClassReferenceExp thrown;   // the thing being tossed

    extern (D) this(const ref Loc loc, ClassReferenceExp victim)
    {
        super(loc, TOK.thrownException, __traits(classInstanceSize, ThrownExceptionExp));
        this.thrown = victim;
        this.type = victim.type;
    }

    override const(char)* toChars() const
    {
        return "CTFE ThrownException";
    }

    // Generate an error message when this exception is not caught
    extern (D) void generateUncaughtError()
    {
        UnionExp ue = void;
        Expression e = resolveSlice((*thrown.value.elements)[0], &ue);
        StringExp se = e.toStringExp();
        thrown.error("uncaught CTFE exception `%s(%s)`", thrown.type.toChars(), se ? se.toChars() : e.toChars());
        /* Also give the line where the throw statement was. We won't have it
         * in the case where the ThrowStatement is generated internally
         * (eg, in ScopeStatement)
         */
        if (loc.isValid() && !loc.equals(thrown.loc))
            .errorSupplemental(loc, "thrown from here");
    }

    override void accept(Visitor v)
    {
        v.visit(this);
    }
}

/***********************************************************
 * This type is only used by the interpreter.
 */
extern (C++) final class CTFEExp : Expression
{
    extern (D) this(TOK tok)
    {
        super(Loc.initial, tok, __traits(classInstanceSize, CTFEExp));
        type = Type.tvoid;
    }

    override const(char)* toChars() const
    {
        switch (op)
        {
        case TOK.cantExpression:
            return "<cant>";
        case TOK.voidExpression:
            return "cast(void)0";
        case TOK.showCtfeContext:
            return "<error>";
        case TOK.break_:
            return "<break>";
        case TOK.continue_:
            return "<continue>";
        case TOK.goto_:
            return "<goto>";
        default:
            assert(0);
        }
    }

    extern (D) __gshared CTFEExp cantexp;
    extern (D) __gshared CTFEExp voidexp;
    extern (D) __gshared CTFEExp breakexp;
    extern (D) __gshared CTFEExp continueexp;
    extern (D) __gshared CTFEExp gotoexp;
    /* Used when additional information is needed regarding
     * a ctfe error.
     */
    extern (D) __gshared CTFEExp showcontext;

    extern (D) static bool isCantExp(const Expression e)
    {
        return e && e.op == TOK.cantExpression;
    }

    extern (D) static bool isGotoExp(const Expression e)
    {
        return e && e.op == TOK.goto_;
    }
}

// True if 'e' is CTFEExp::cantexp, or an exception
bool exceptionOrCantInterpret(const Expression e)
{
    return e && (e.op == TOK.cantExpression || e.op == TOK.thrownException || e.op == TOK.showCtfeContext);
}

/************** Aggregate literals (AA/string/array/struct) ******************/
// Given expr, which evaluates to an array/AA/string literal,
// return true if it needs to be copied
bool needToCopyLiteral(const Expression expr)
{
    Expression e = cast()expr;
    for (;;)
    {
        switch (e.op)
        {
        case TOK.arrayLiteral:
            return (cast(ArrayLiteralExp)e).ownedByCtfe == OwnedBy.code;
        case TOK.assocArrayLiteral:
            return (cast(AssocArrayLiteralExp)e).ownedByCtfe == OwnedBy.code;
        case TOK.structLiteral:
            return (cast(StructLiteralExp)e).ownedByCtfe == OwnedBy.code;
        case TOK.string_:
        case TOK.this_:
        case TOK.variable:
            return false;
        case TOK.assign:
            return false;
        case TOK.index:
        case TOK.dotVariable:
        case TOK.slice:
        case TOK.cast_:
            e = (cast(UnaExp)e).e1;
            continue;
        case TOK.concatenate:
            return needToCopyLiteral((cast(BinExp)e).e1) || needToCopyLiteral((cast(BinExp)e).e2);
        case TOK.concatenateAssign:
        case TOK.concatenateElemAssign:
        case TOK.concatenateDcharAssign:
            e = (cast(BinExp)e).e2;
            continue;
        default:
            return false;
        }
    }
}

private Expressions* copyLiteralArray(Expressions* oldelems, Expression basis = null)
{
    if (!oldelems)
        return oldelems;
    incArrayAllocs();
    auto newelems = new Expressions(oldelems.dim);
    foreach (i, el; *oldelems)
    {
        (*newelems)[i] = copyLiteral(el ? el : basis).copy();
    }
    return newelems;
}

// Make a copy of the ArrayLiteral, AALiteral, String, or StructLiteral.
// This value will be used for in-place modification.
UnionExp copyLiteral(Expression e)
{
    UnionExp ue = void;
    if (auto se = e.isStringExp()) // syntaxCopy doesn't make a copy for StringExp!
    {
        char* s = cast(char*)mem.xcalloc(se.len + 1, se.sz);
        const slice = se.peekData();
        memcpy(s, slice.ptr, slice.length);
        emplaceExp!(StringExp)(&ue, se.loc, s[0 .. se.len * se.sz], se.len, se.sz);
        StringExp se2 = cast(StringExp)ue.exp();
        se2.committed = se.committed;
        se2.postfix = se.postfix;
        se2.type = se.type;
        se2.ownedByCtfe = OwnedBy.ctfe;
        return ue;
    }
    if (auto ale = e.isArrayLiteralExp())
    {
        auto elements = copyLiteralArray(ale.elements, ale.basis);

        emplaceExp!(ArrayLiteralExp)(&ue, e.loc, e.type, elements);

        ArrayLiteralExp r = cast(ArrayLiteralExp)ue.exp();
        r.ownedByCtfe = OwnedBy.ctfe;
        return ue;
    }
    if (auto aae = e.isAssocArrayLiteralExp())
    {
        emplaceExp!(AssocArrayLiteralExp)(&ue, e.loc, copyLiteralArray(aae.keys), copyLiteralArray(aae.values));
        AssocArrayLiteralExp r = cast(AssocArrayLiteralExp)ue.exp();
        r.type = e.type;
        r.ownedByCtfe = OwnedBy.ctfe;
        return ue;
    }
    if (auto sle = e.isStructLiteralExp())
    {
        /* syntaxCopy doesn't work for struct literals, because of a nasty special
         * case: block assignment is permitted inside struct literals, eg,
         * an int[4] array can be initialized with a single int.
         */
        auto oldelems = sle.elements;
        auto newelems = new Expressions(oldelems.dim);
        foreach (i, ref el; *newelems)
        {
            // We need the struct definition to detect block assignment
            auto v = sle.sd.fields[i];
            auto m = (*oldelems)[i];

            // If it is a void assignment, use the default initializer
            if (!m)
                m = voidInitLiteral(v.type, v).copy();

            if (v.type.ty == Tarray || v.type.ty == Taarray)
            {
                // Don't have to copy array references
            }
            else
            {
                // Buzilla 15681: Copy the source element always.
                m = copyLiteral(m).copy();

                // Block assignment from inside struct literals
                if (v.type.ty != m.type.ty && v.type.ty == Tsarray)
                {
                    auto tsa = v.type.isTypeSArray();
                    auto len = cast(size_t)tsa.dim.toInteger();
                    UnionExp uex = void;
                    m = createBlockDuplicatedArrayLiteral(&uex, e.loc, v.type, m, len);
                    if (m == uex.exp())
                        m = uex.copy();
                }
            }
            el = m;
        }
        emplaceExp!(StructLiteralExp)(&ue, e.loc, sle.sd, newelems, sle.stype);
        auto r = ue.exp().isStructLiteralExp();
        r.type = e.type;
        r.ownedByCtfe = OwnedBy.ctfe;
        r.origin = sle.origin;
        return ue;
    }
    if (e.op == TOK.function_ || e.op == TOK.delegate_ || e.op == TOK.symbolOffset || e.op == TOK.null_ || e.op == TOK.variable || e.op == TOK.dotVariable || e.op == TOK.int64 || e.op == TOK.float64 || e.op == TOK.char_ || e.op == TOK.complex80 || e.op == TOK.void_ || e.op == TOK.vector || e.op == TOK.typeid_)
    {
        // Simple value types
        // Keep e1 for DelegateExp and DotVarExp
        emplaceExp!(UnionExp)(&ue, e);
        Expression r = ue.exp();
        r.type = e.type;
        return ue;
    }
    if (auto se = e.isSliceExp())
    {
        if (se.type.toBasetype().ty == Tsarray)
        {
            // same with resolveSlice()
            if (se.e1.op == TOK.null_)
            {
                emplaceExp!(NullExp)(&ue, se.loc, se.type);
                return ue;
            }
            ue = Slice(se.type, se.e1, se.lwr, se.upr);
            auto r = ue.exp().isArrayLiteralExp();
            r.elements = copyLiteralArray(r.elements);
            r.ownedByCtfe = OwnedBy.ctfe;
            return ue;
        }
        else
        {
            // Array slices only do a shallow copy
            emplaceExp!(SliceExp)(&ue, e.loc, se.e1, se.lwr, se.upr);
            Expression r = ue.exp();
            r.type = e.type;
            return ue;
        }
    }
    if (isPointer(e.type))
    {
        // For pointers, we only do a shallow copy.
        if (auto ae = e.isAddrExp())
            emplaceExp!(AddrExp)(&ue, e.loc, ae.e1);
        else if (auto ie = e.isIndexExp())
            emplaceExp!(IndexExp)(&ue, e.loc, ie.e1, ie.e2);
        else if (auto dve = e.isDotVarExp())
        {
            emplaceExp!(DotVarExp)(&ue, e.loc, dve.e1, dve.var, dve.hasOverloads);
        }
        else
            assert(0);

        Expression r = ue.exp();
        r.type = e.type;
        return ue;
    }
    if (auto cre = e.isClassReferenceExp())
    {
        emplaceExp!(ClassReferenceExp)(&ue, e.loc, cre.value, e.type);
        return ue;
    }
    if (e.op == TOK.error)
    {
        emplaceExp!(UnionExp)(&ue, e);
        return ue;
    }
    e.error("CTFE internal error: literal `%s`", e.toChars());
    assert(0);
}

/* Deal with type painting.
 * Type painting is a major nuisance: we can't just set
 * e.type = type, because that would change the original literal.
 * But, we can't simply copy the literal either, because that would change
 * the values of any pointers.
 */
Expression paintTypeOntoLiteral(Type type, Expression lit)
{
    if (lit.type.equals(type))
        return lit;
    return paintTypeOntoLiteralCopy(type, lit).copy();
}

Expression paintTypeOntoLiteral(UnionExp* pue, Type type, Expression lit)
{
    if (lit.type.equals(type))
        return lit;
    *pue = paintTypeOntoLiteralCopy(type, lit);
    return pue.exp();
}

private UnionExp paintTypeOntoLiteralCopy(Type type, Expression lit)
{
    UnionExp ue;
    if (lit.type.equals(type))
    {
        emplaceExp!(UnionExp)(&ue, lit);
        return ue;
    }
    // If it is a cast to inout, retain the original type of the referenced part.
    if (type.hasWild() && type.hasPointers())
    {
        emplaceExp!(UnionExp)(&ue, lit);
        ue.exp().type = type;
        return ue;
    }
    if (auto se = lit.isSliceExp())
    {
        emplaceExp!(SliceExp)(&ue, lit.loc, se.e1, se.lwr, se.upr);
    }
    else if (auto ie = lit.isIndexExp())
    {
        emplaceExp!(IndexExp)(&ue, lit.loc, ie.e1, ie.e2);
    }
    else if (lit.op == TOK.arrayLiteral)
    {
        emplaceExp!(SliceExp)(&ue, lit.loc, lit, ctfeEmplaceExp!IntegerExp(Loc.initial, 0, Type.tsize_t), ArrayLength(Type.tsize_t, lit).copy());
    }
    else if (lit.op == TOK.string_)
    {
        // For strings, we need to introduce another level of indirection
        emplaceExp!(SliceExp)(&ue, lit.loc, lit, ctfeEmplaceExp!IntegerExp(Loc.initial, 0, Type.tsize_t), ArrayLength(Type.tsize_t, lit).copy());
    }
    else if (auto aae = lit.isAssocArrayLiteralExp())
    {
        // TODO: we should be creating a reference to this AAExp, not
        // just a ref to the keys and values.
        OwnedBy wasOwned = aae.ownedByCtfe;
        emplaceExp!(AssocArrayLiteralExp)(&ue, lit.loc, aae.keys, aae.values);
        aae = cast(AssocArrayLiteralExp)ue.exp();
        aae.ownedByCtfe = wasOwned;
    }
    else
    {
        // Can't type paint from struct to struct*; this needs another
        // level of indirection
        if (lit.op == TOK.structLiteral && isPointer(type))
            lit.error("CTFE internal error: painting `%s`", type.toChars());
        ue = copyLiteral(lit);
    }
    ue.exp().type = type;
    return ue;
}

/*************************************
 * If e is a SliceExp, constant fold it.
 * Params:
 *      e = expression to resolve
 *      pue = if not null, store resulting expression here
 * Returns:
 *      resulting expression
 */
Expression resolveSlice(Expression e, UnionExp* pue = null)
{
    SliceExp se = e.isSliceExp();
    if (!se)
        return e;
    if (se.e1.op == TOK.null_)
        return se.e1;
    if (pue)
    {
        *pue = Slice(e.type, se.e1, se.lwr, se.upr);
        return pue.exp();
    }
    else
        return Slice(e.type, se.e1, se.lwr, se.upr).copy();
}

/* Determine the array length, without interpreting it.
 * e must be an array literal, or a slice
 * It's very wasteful to resolve the slice when we only
 * need the length.
 */
uinteger_t resolveArrayLength(const Expression e)
{
    switch (e.op)
    {
        case TOK.vector:
            return e.isVectorExp().dim;

        case TOK.null_:
            return 0;

        case TOK.slice:
        {
            auto se = cast(SliceExp)e;
            const ilo = se.lwr.toInteger();
            const iup = se.upr.toInteger();
            return iup - ilo;
        }

        case TOK.string_:
            return e.isStringExp().len;

        case TOK.arrayLiteral:
        {
            const ale = e.isArrayLiteralExp();
            return ale.elements ? ale.elements.dim : 0;
        }

        case TOK.assocArrayLiteral:
        {
            return e.isAssocArrayLiteralExp().keys.dim;
        }

        default:
            assert(0);
    }
}

/******************************
 * Helper for NewExp
 * Create an array literal consisting of 'elem' duplicated 'dim' times.
 * Params:
 *      pue = where to store result
 *      loc = source location where the interpretation occurs
 *      type = target type of the result
 *      elem = the source of array element, it will be owned by the result
 *      dim = element number of the result
 * Returns:
 *      Constructed ArrayLiteralExp
 */
ArrayLiteralExp createBlockDuplicatedArrayLiteral(UnionExp* pue, const ref Loc loc, Type type, Expression elem, size_t dim)
{
    if (type.ty == Tsarray && type.nextOf().ty == Tsarray && elem.type.ty != Tsarray)
    {
        // If it is a multidimensional array literal, do it recursively
        auto tsa = type.nextOf().isTypeSArray();
        const len = cast(size_t)tsa.dim.toInteger();
        UnionExp ue = void;
        elem = createBlockDuplicatedArrayLiteral(&ue, loc, type.nextOf(), elem, len);
        if (elem == ue.exp())
            elem = ue.copy();
    }

    // Buzilla 15681
    const tb = elem.type.toBasetype();
    const mustCopy = tb.ty == Tstruct || tb.ty == Tsarray;

    auto elements = new Expressions(dim);
    foreach (i, ref el; *elements)
    {
        el = mustCopy && i ? copyLiteral(elem).copy() : elem;
    }
    emplaceExp!(ArrayLiteralExp)(pue, loc, type, elements);
    auto ale = pue.exp().isArrayLiteralExp();
    ale.ownedByCtfe = OwnedBy.ctfe;
    return ale;
}

/******************************
 * Helper for NewExp
 * Create a string literal consisting of 'value' duplicated 'dim' times.
 */
StringExp createBlockDuplicatedStringLiteral(UnionExp* pue, const ref Loc loc, Type type, dchar value, size_t dim, ubyte sz)
{
    auto s = cast(char*)mem.xcalloc(dim, sz);
    foreach (elemi; 0 .. dim)
    {
        switch (sz)
        {
        case 1:
            s[elemi] = cast(char)value;
            break;
        case 2:
            (cast(wchar*)s)[elemi] = cast(wchar)value;
            break;
        case 4:
            (cast(dchar*)s)[elemi] = value;
            break;
        default:
            assert(0);
        }
    }
    emplaceExp!(StringExp)(pue, loc, s[0 .. dim * sz], dim, sz);
    auto se = pue.exp().isStringExp();
    se.type = type;
    se.committed = true;
    se.ownedByCtfe = OwnedBy.ctfe;
    return se;
}

// Return true if t is an AA
bool isAssocArray(Type t)
{
    return t.toBasetype().isTypeAArray() !is null;
}

// Given a template AA type, extract the corresponding built-in AA type
TypeAArray toBuiltinAAType(Type t)
{
    return t.toBasetype().isTypeAArray();
}

/************** TypeInfo operations ************************************/
// Return true if type is TypeInfo_Class
bool isTypeInfo_Class(const Type type)
{
    auto tc = cast()type.isTypeClass();
    return tc && (Type.dtypeinfo == tc.sym || Type.dtypeinfo.isBaseOf(tc.sym, null));
}

/************** Pointer operations ************************************/
// Return true if t is a pointer (not a function pointer)
bool isPointer(Type t)
{
    Type tb = t.toBasetype();
    return tb.ty == Tpointer && tb.nextOf().ty != Tfunction;
}

// For CTFE only. Returns true if 'e' is true or a non-null pointer.
bool isTrueBool(Expression e)
{
    return e.isBool(true) || ((e.type.ty == Tpointer || e.type.ty == Tclass) && e.op != TOK.null_);
}

/* Is it safe to convert from srcPointee* to destPointee* ?
 * srcPointee is the genuine type (never void).
 * destPointee may be void.
 */
bool isSafePointerCast(Type srcPointee, Type destPointee)
{
    // It's safe to cast S** to D** if it's OK to cast S* to D*
    while (srcPointee.ty == Tpointer && destPointee.ty == Tpointer)
    {
        srcPointee = srcPointee.nextOf();
        destPointee = destPointee.nextOf();
    }
    // It's OK if both are the same (modulo const)
    if (srcPointee.constConv(destPointee))
        return true;
    // It's OK if function pointers differ only in safe/pure/nothrow
    if (srcPointee.ty == Tfunction && destPointee.ty == Tfunction)
        return srcPointee.covariant(destPointee) == 1;
    // it's OK to cast to void*
    if (destPointee.ty == Tvoid)
        return true;
    // It's OK to cast from V[K] to void*
    if (srcPointee.ty == Taarray && destPointee == Type.tvoidptr)
        return true;
    // It's OK if they are the same size (static array of) integers, eg:
    //     int*     --> uint*
    //     int[5][] --> uint[5][]
    if (srcPointee.ty == Tsarray && destPointee.ty == Tsarray)
    {
        if (srcPointee.size() != destPointee.size())
            return false;
        srcPointee = srcPointee.baseElemOf();
        destPointee = destPointee.baseElemOf();
    }
    return srcPointee.isintegral() && destPointee.isintegral() && srcPointee.size() == destPointee.size();
}

Expression getAggregateFromPointer(Expression e, dinteger_t* ofs)
{
    *ofs = 0;
    if (auto ae = e.isAddrExp())
        e = ae.e1;
    if (auto soe = e.isSymOffExp())
        *ofs = soe.offset;
    if (auto dve = e.isDotVarExp())
    {
        const ex = dve.e1;
        const v = dve.var.isVarDeclaration();
        assert(v);
        StructLiteralExp se = (ex.op == TOK.classReference)
            ? (cast(ClassReferenceExp)ex).value
            : cast(StructLiteralExp)ex;

        // We can't use getField, because it makes a copy
        const i = (ex.op == TOK.classReference)
            ? (cast(ClassReferenceExp)ex).getFieldIndex(e.type, v.offset)
            : se.getFieldIndex(e.type, v.offset);
        e = (*se.elements)[i];
    }
    if (auto ie = e.isIndexExp())
    {
        // Note that each AA element is part of its own memory block
        if ((ie.e1.type.ty == Tarray || ie.e1.type.ty == Tsarray || ie.e1.op == TOK.string_ || ie.e1.op == TOK.arrayLiteral) && ie.e2.op == TOK.int64)
        {
            *ofs = ie.e2.toInteger();
            return ie.e1;
        }
    }
    if (auto se = e.isSliceExp())
    {
        if (se && e.type.toBasetype().ty == Tsarray &&
           (se.e1.type.ty == Tarray || se.e1.type.ty == Tsarray || se.e1.op == TOK.string_ || se.e1.op == TOK.arrayLiteral) && se.lwr.op == TOK.int64)
        {
            *ofs = se.lwr.toInteger();
            return se.e1;
        }
    }

    // It can be a `null` disguised as a cast, e.g. `cast(void*)0`.
    if (auto ie = e.isIntegerExp())
        if (ie.type.ty == Tpointer && ie.getInteger() == 0)
            return new NullExp(ie.loc, e.type.nextOf());
    // Those casts are invalid, but let the rest of the code handle it,
    // as it could be something like `x !is null`, which doesn't need
    // to dereference the pointer, even if the pointer is `cast(void*)420`.

    return e;
}

/** Return true if agg1 and agg2 are pointers to the same memory block
 */
bool pointToSameMemoryBlock(Expression agg1, Expression agg2)
{
    if (agg1 == agg2)
        return true;
    // For integers cast to pointers, we regard them as non-comparable
    // unless they are identical. (This may be overly strict).
    if (agg1.op == TOK.int64 && agg2.op == TOK.int64 && agg1.toInteger() == agg2.toInteger())
    {
        return true;
    }
    // Note that type painting can occur with VarExp, so we
    // must compare the variables being pointed to.
    if (agg1.op == TOK.variable && agg2.op == TOK.variable && (cast(VarExp)agg1).var == (cast(VarExp)agg2).var)
    {
        return true;
    }
    if (agg1.op == TOK.symbolOffset && agg2.op == TOK.symbolOffset && (cast(SymOffExp)agg1).var == (cast(SymOffExp)agg2).var)
    {
        return true;
    }
    return false;
}

// return e1 - e2 as an integer, or error if not possible
UnionExp pointerDifference(const ref Loc loc, Type type, Expression e1, Expression e2)
{
    UnionExp ue = void;
    dinteger_t ofs1, ofs2;
    Expression agg1 = getAggregateFromPointer(e1, &ofs1);
    Expression agg2 = getAggregateFromPointer(e2, &ofs2);
    if (agg1 == agg2)
    {
        Type pointee = (cast(TypePointer)agg1.type).next;
        const sz = pointee.size();
        emplaceExp!(IntegerExp)(&ue, loc, (ofs1 - ofs2) * sz, type);
    }
    else if (agg1.op == TOK.string_ && agg2.op == TOK.string_ &&
             (cast(StringExp)agg1).peekString().ptr == (cast(StringExp)agg2).peekString().ptr)
    {
        Type pointee = (cast(TypePointer)agg1.type).next;
        const sz = pointee.size();
        emplaceExp!(IntegerExp)(&ue, loc, (ofs1 - ofs2) * sz, type);
    }
    else if (agg1.op == TOK.symbolOffset && agg2.op == TOK.symbolOffset &&
             (cast(SymOffExp)agg1).var == (cast(SymOffExp)agg2).var)
    {
        emplaceExp!(IntegerExp)(&ue, loc, ofs1 - ofs2, type);
    }
    else
    {
        error(loc, "`%s - %s` cannot be interpreted at compile time: cannot subtract pointers to two different memory blocks", e1.toChars(), e2.toChars());
        emplaceExp!(CTFEExp)(&ue, TOK.cantExpression);
    }
    return ue;
}

// Return eptr op e2, where eptr is a pointer, e2 is an integer,
// and op is TOK.add or TOK.min
UnionExp pointerArithmetic(const ref Loc loc, TOK op, Type type, Expression eptr, Expression e2)
{
    UnionExp ue;
    if (eptr.type.nextOf().ty == Tvoid)
    {
        error(loc, "cannot perform arithmetic on `void*` pointers at compile time");
    Lcant:
        emplaceExp!(CTFEExp)(&ue, TOK.cantExpression);
        return ue;
    }
    if (eptr.op == TOK.address)
        eptr = (cast(AddrExp)eptr).e1;
    dinteger_t ofs1;
    Expression agg1 = getAggregateFromPointer(eptr, &ofs1);
    if (agg1.op == TOK.symbolOffset)
    {
        if ((cast(SymOffExp)agg1).var.type.ty != Tsarray)
        {
            error(loc, "cannot perform pointer arithmetic on arrays of unknown length at compile time");
            goto Lcant;
        }
    }
    else if (agg1.op != TOK.string_ && agg1.op != TOK.arrayLiteral)
    {
        error(loc, "cannot perform pointer arithmetic on non-arrays at compile time");
        goto Lcant;
    }
    dinteger_t ofs2 = e2.toInteger();
    Type pointee = (cast(TypeNext)agg1.type.toBasetype()).next;
    dinteger_t sz = pointee.size();
    sinteger_t indx;
    dinteger_t len;
    if (agg1.op == TOK.symbolOffset)
    {
        indx = ofs1 / sz;
        len = (cast(TypeSArray)(cast(SymOffExp)agg1).var.type).dim.toInteger();
    }
    else
    {
        Expression dollar = ArrayLength(Type.tsize_t, agg1).copy();
        assert(!CTFEExp.isCantExp(dollar));
        indx = ofs1;
        len = dollar.toInteger();
    }
    if (op == TOK.add || op == TOK.addAssign || op == TOK.plusPlus)
        indx += ofs2 / sz;
    else if (op == TOK.min || op == TOK.minAssign || op == TOK.minusMinus)
        indx -= ofs2 / sz;
    else
    {
        error(loc, "CTFE internal error: bad pointer operation");
        goto Lcant;
    }
    if (indx < 0 || len < indx)
    {
        error(loc, "cannot assign pointer to index %lld inside memory block `[0..%lld]`", indx, len);
        goto Lcant;
    }
    if (agg1.op == TOK.symbolOffset)
    {
        emplaceExp!(SymOffExp)(&ue, loc, (cast(SymOffExp)agg1).var, indx * sz);
        SymOffExp se = cast(SymOffExp)ue.exp();
        se.type = type;
        return ue;
    }
    if (agg1.op != TOK.arrayLiteral && agg1.op != TOK.string_)
    {
        error(loc, "CTFE internal error: pointer arithmetic `%s`", agg1.toChars());
        goto Lcant;
    }
    if (eptr.type.toBasetype().ty == Tsarray)
    {
        dinteger_t dim = (cast(TypeSArray)eptr.type.toBasetype()).dim.toInteger();
        // Create a CTFE pointer &agg1[indx .. indx+dim]
        auto se = ctfeEmplaceExp!SliceExp(loc, agg1,
                ctfeEmplaceExp!IntegerExp(loc, indx, Type.tsize_t),
                ctfeEmplaceExp!IntegerExp(loc, indx + dim, Type.tsize_t));
        se.type = type.toBasetype().nextOf();
        emplaceExp!(AddrExp)(&ue, loc, se);
        ue.exp().type = type;
        return ue;
    }
    // Create a CTFE pointer &agg1[indx]
    auto ofs = ctfeEmplaceExp!IntegerExp(loc, indx, Type.tsize_t);
    Expression ie = ctfeEmplaceExp!IndexExp(loc, agg1, ofs);
    ie.type = type.toBasetype().nextOf(); // https://issues.dlang.org/show_bug.cgi?id=13992
    emplaceExp!(AddrExp)(&ue, loc, ie);
    ue.exp().type = type;
    return ue;
}

// Return 1 if true, 0 if false
// -1 if comparison is illegal because they point to non-comparable memory blocks
int comparePointers(TOK op, Expression agg1, dinteger_t ofs1, Expression agg2, dinteger_t ofs2)
{
    if (pointToSameMemoryBlock(agg1, agg2))
    {
        int n;
        switch (op)
        {
        case TOK.lessThan:
            n = (ofs1 < ofs2);
            break;
        case TOK.lessOrEqual:
            n = (ofs1 <= ofs2);
            break;
        case TOK.greaterThan:
            n = (ofs1 > ofs2);
            break;
        case TOK.greaterOrEqual:
            n = (ofs1 >= ofs2);
            break;
        case TOK.identity:
        case TOK.equal:
            n = (ofs1 == ofs2);
            break;
        case TOK.notIdentity:
        case TOK.notEqual:
            n = (ofs1 != ofs2);
            break;
        default:
            assert(0);
        }
        return n;
    }
    const null1 = (agg1.op == TOK.null_);
    const null2 = (agg2.op == TOK.null_);
    int cmp;
    if (null1 || null2)
    {
        switch (op)
        {
        case TOK.lessThan:
            cmp = null1 && !null2;
            break;
        case TOK.greaterThan:
            cmp = !null1 && null2;
            break;
        case TOK.lessOrEqual:
            cmp = null1;
            break;
        case TOK.greaterOrEqual:
            cmp = null2;
            break;
        case TOK.identity:
        case TOK.equal:
        case TOK.notIdentity: // 'cmp' gets inverted below
        case TOK.notEqual:
            cmp = (null1 == null2);
            break;
        default:
            assert(0);
        }
    }
    else
    {
        switch (op)
        {
        case TOK.identity:
        case TOK.equal:
        case TOK.notIdentity: // 'cmp' gets inverted below
        case TOK.notEqual:
            cmp = 0;
            break;
        default:
            return -1; // memory blocks are different
        }
    }
    if (op == TOK.notIdentity || op == TOK.notEqual)
        cmp ^= 1;
    return cmp;
}

// True if conversion from type 'from' to 'to' involves a reinterpret_cast
// floating point -> integer or integer -> floating point
bool isFloatIntPaint(Type to, Type from)
{
    return from.size() == to.size() && (from.isintegral() && to.isfloating() || from.isfloating() && to.isintegral());
}

// Reinterpret float/int value 'fromVal' as a float/integer of type 'to'.
Expression paintFloatInt(UnionExp* pue, Expression fromVal, Type to)
{
    if (exceptionOrCantInterpret(fromVal))
        return fromVal;
    assert(to.size() == 4 || to.size() == 8);
    return Compiler.paintAsType(pue, fromVal, to);
}

/******** Constant folding, with support for CTFE ***************************/
/// Return true if non-pointer expression e can be compared
/// with >,is, ==, etc, using ctfeCmp, ctfeEqual, ctfeIdentity
bool isCtfeComparable(Expression e)
{
    if (e.op == TOK.slice)
        e = (cast(SliceExp)e).e1;
    if (e.isConst() != 1)
    {
        if (e.op == TOK.null_ || e.op == TOK.string_ || e.op == TOK.function_ || e.op == TOK.delegate_ || e.op == TOK.arrayLiteral || e.op == TOK.structLiteral || e.op == TOK.assocArrayLiteral || e.op == TOK.classReference)
        {
            return true;
        }
        // https://issues.dlang.org/show_bug.cgi?id=14123
        // TypeInfo object is comparable in CTFE
        if (e.op == TOK.typeid_)
            return true;
        return false;
    }
    return true;
}

/// Map TOK comparison ops
private bool numCmp(N)(TOK op, N n1, N n2)
{
    switch (op)
    {
    case TOK.lessThan:
        return n1 < n2;
    case TOK.lessOrEqual:
        return n1 <= n2;
    case TOK.greaterThan:
        return n1 > n2;
    case TOK.greaterOrEqual:
        return n1 >= n2;

    default:
        assert(0);
    }
}

/// Returns cmp OP 0; where OP is ==, !=, <, >=, etc. Result is 0 or 1
bool specificCmp(TOK op, int rawCmp)
{
    return numCmp!int(op, rawCmp, 0);
}

/// Returns e1 OP e2; where OP is ==, !=, <, >=, etc. Result is 0 or 1
bool intUnsignedCmp(TOK op, dinteger_t n1, dinteger_t n2)
{
    return numCmp!dinteger_t(op, n1, n2);
}

/// Returns e1 OP e2; where OP is ==, !=, <, >=, etc. Result is 0 or 1
bool intSignedCmp(TOK op, sinteger_t n1, sinteger_t n2)
{
    return numCmp!sinteger_t(op, n1, n2);
}

/// Returns e1 OP e2; where OP is ==, !=, <, >=, etc. Result is 0 or 1
bool realCmp(TOK op, real_t r1, real_t r2)
{
    // Don't rely on compiler, handle NAN arguments separately
    if (CTFloat.isNaN(r1) || CTFloat.isNaN(r2)) // if unordered
    {
        switch (op)
        {
        case TOK.lessThan:
        case TOK.lessOrEqual:
        case TOK.greaterThan:
        case TOK.greaterOrEqual:
            return false;

        default:
            assert(0);
        }
    }
    else
    {
        return numCmp!real_t(op, r1, r2);
    }
}

/* Conceptually the same as memcmp(e1, e2).
 * e1 and e2 may be strings, arrayliterals, or slices.
 * For string types, return <0 if e1 < e2, 0 if e1==e2, >0 if e1 > e2.
 * For all other types, return 0 if e1 == e2, !=0 if e1 != e2.
 * Returns:
 *      -1,0,1
 */
private int ctfeCmpArrays(const ref Loc loc, Expression e1, Expression e2, uinteger_t len)
{
    // Resolve slices, if necessary
    uinteger_t lo1 = 0;
    uinteger_t lo2 = 0;

    Expression x1 = e1;
    if (auto sle1 = x1.isSliceExp())
    {
        lo1 = sle1.lwr.toInteger();
        x1 = sle1.e1;
    }
    auto se1 = x1.isStringExp();
    auto ae1 = x1.isArrayLiteralExp();

    Expression x2 = e2;
    if (auto sle2 = x2.isSliceExp())
    {
        lo2 = sle2.lwr.toInteger();
        x2 = sle2.e1;
    }
    auto se2 = x2.isStringExp();
    auto ae2 = x2.isArrayLiteralExp();

    // Now both must be either TOK.arrayLiteral or TOK.string_
    if (se1 && se2)
        return sliceCmpStringWithString(se1, se2, cast(size_t)lo1, cast(size_t)lo2, cast(size_t)len);
    if (se1 && ae2)
        return sliceCmpStringWithArray(se1, ae2, cast(size_t)lo1, cast(size_t)lo2, cast(size_t)len);
    if (se2 && ae1)
        return -sliceCmpStringWithArray(se2, ae1, cast(size_t)lo2, cast(size_t)lo1, cast(size_t)len);
    assert(ae1 && ae2);
    // Comparing two array literals. This case is potentially recursive.
    // If they aren't strings, we just need an equality check rather than
    // a full cmp.
    const bool needCmp = ae1.type.nextOf().isintegral();
    foreach (size_t i; 0 .. cast(size_t)len)
    {
        Expression ee1 = (*ae1.elements)[cast(size_t)(lo1 + i)];
        Expression ee2 = (*ae2.elements)[cast(size_t)(lo2 + i)];
        if (needCmp)
        {
            const sinteger_t c = ee1.toInteger() - ee2.toInteger();
            if (c > 0)
                return 1;
            if (c < 0)
                return -1;
        }
        else
        {
            if (ctfeRawCmp(loc, ee1, ee2))
                return 1;
        }
    }
    return 0;
}

/* Given a delegate expression e, return .funcptr.
 * If e is NullExp, return NULL.
 */
private FuncDeclaration funcptrOf(Expression e)
{
    assert(e.type.ty == Tdelegate);
    if (auto de = e.isDelegateExp())
        return de.func;
    if (auto fe = e.isFuncExp())
        return fe.fd;
    assert(e.op == TOK.null_);
    return null;
}

private bool isArray(const Expression e)
{
    return e.op == TOK.arrayLiteral || e.op == TOK.string_ || e.op == TOK.slice || e.op == TOK.null_;
}

/*****
 * Params:
 *      loc = source file location
 *      e1 = left operand
 *      e2 = right operand
 *      identity = true for `is` identity comparisons
 * Returns:
 * For strings, return <0 if e1 < e2, 0 if e1==e2, >0 if e1 > e2.
 * For all other types, return 0 if e1 == e2, !=0 if e1 != e2.
 */
private int ctfeRawCmp(const ref Loc loc, Expression e1, Expression e2, bool identity = false)
{
    if (e1.op == TOK.classReference || e2.op == TOK.classReference)
    {
        if (e1.op == TOK.classReference && e2.op == TOK.classReference &&
            (cast(ClassReferenceExp)e1).value == (cast(ClassReferenceExp)e2).value)
            return 0;
        return 1;
    }
    if (e1.op == TOK.typeid_ && e2.op == TOK.typeid_)
    {
        // printf("e1: %s\n", e1.toChars());
        // printf("e2: %s\n", e2.toChars());
        Type t1 = isType((cast(TypeidExp)e1).obj);
        Type t2 = isType((cast(TypeidExp)e2).obj);
        assert(t1);
        assert(t2);
        return t1 != t2;
    }
    // null == null, regardless of type
    if (e1.op == TOK.null_ && e2.op == TOK.null_)
        return 0;
    if (e1.type.ty == Tpointer && e2.type.ty == Tpointer)
    {
        // Can only be an equality test.
        dinteger_t ofs1, ofs2;
        Expression agg1 = getAggregateFromPointer(e1, &ofs1);
        Expression agg2 = getAggregateFromPointer(e2, &ofs2);
        if ((agg1 == agg2) || (agg1.op == TOK.variable && agg2.op == TOK.variable && (cast(VarExp)agg1).var == (cast(VarExp)agg2).var))
        {
            if (ofs1 == ofs2)
                return 0;
        }
        return 1;
    }
    if (e1.type.ty == Tdelegate && e2.type.ty == Tdelegate)
    {
        // If .funcptr isn't the same, they are not equal
        if (funcptrOf(e1) != funcptrOf(e2))
            return 1;
        // If both are delegate literals, assume they have the
        // same closure pointer. TODO: We don't support closures yet!
        if (e1.op == TOK.function_ && e2.op == TOK.function_)
            return 0;
        assert(e1.op == TOK.delegate_ && e2.op == TOK.delegate_);
        // Same .funcptr. Do they have the same .ptr?
        Expression ptr1 = (cast(DelegateExp)e1).e1;
        Expression ptr2 = (cast(DelegateExp)e2).e1;
        dinteger_t ofs1, ofs2;
        Expression agg1 = getAggregateFromPointer(ptr1, &ofs1);
        Expression agg2 = getAggregateFromPointer(ptr2, &ofs2);
        // If they are TOK.variable, it means they are FuncDeclarations
        if ((agg1 == agg2 && ofs1 == ofs2) || (agg1.op == TOK.variable && agg2.op == TOK.variable && (cast(VarExp)agg1).var == (cast(VarExp)agg2).var))
        {
            return 0;
        }
        return 1;
    }
    if (isArray(e1) && isArray(e2))
    {
        const uinteger_t len1 = resolveArrayLength(e1);
        const uinteger_t len2 = resolveArrayLength(e2);
        // workaround for dmc optimizer bug calculating wrong len for
        // uinteger_t len = (len1 < len2 ? len1 : len2);
        // if (len == 0) ...
        if (len1 > 0 && len2 > 0)
        {
            const uinteger_t len = (len1 < len2 ? len1 : len2);
            const int res = ctfeCmpArrays(loc, e1, e2, len);
            if (res != 0)
                return res;
        }
        return cast(int)(len1 - len2);
    }
    if (e1.type.isintegral())
    {
        return e1.toInteger() != e2.toInteger();
    }
    if (e1.type.isreal() || e1.type.isimaginary())
    {
        real_t r1 = e1.type.isreal() ? e1.toReal() : e1.toImaginary();
        real_t r2 = e1.type.isreal() ? e2.toReal() : e2.toImaginary();
        if (identity)
            return !RealIdentical(r1, r2);
        if (CTFloat.isNaN(r1) || CTFloat.isNaN(r2)) // if unordered
        {
            return 1;   // they are not equal
        }
        else
        {
            return (r1 != r2);
        }
    }
    else if (e1.type.iscomplex())
    {
        auto c1 = e1.toComplex();
        auto c2 = e2.toComplex();
        if (identity)
        {
            return !RealIdentical(c1.re, c2.re) && !RealIdentical(c1.im, c2.im);
        }
        return c1 != c2;
    }
    if (e1.op == TOK.structLiteral && e2.op == TOK.structLiteral)
    {
        StructLiteralExp es1 = cast(StructLiteralExp)e1;
        StructLiteralExp es2 = cast(StructLiteralExp)e2;
        // For structs, we only need to return 0 or 1 (< and > aren't legal).
        if (es1.sd != es2.sd)
            return 1;
        else if ((!es1.elements || !es1.elements.dim) && (!es2.elements || !es2.elements.dim))
            return 0; // both arrays are empty
        else if (!es1.elements || !es2.elements)
            return 1;
        else if (es1.elements.dim != es2.elements.dim)
            return 1;
        else
        {
            foreach (size_t i; 0 .. es1.elements.dim)
            {
                Expression ee1 = (*es1.elements)[i];
                Expression ee2 = (*es2.elements)[i];

                // https://issues.dlang.org/show_bug.cgi?id=16284
                if (ee1.op == TOK.void_ && ee2.op == TOK.void_) // if both are VoidInitExp
                    continue;

                if (ee1 == ee2)
                    continue;
                if (!ee1 || !ee2)
                    return 1;
                const int cmp = ctfeRawCmp(loc, ee1, ee2, identity);
                if (cmp)
                    return 1;
            }
            return 0; // All elements are equal
        }
    }
    if (e1.op == TOK.assocArrayLiteral && e2.op == TOK.assocArrayLiteral)
    {
        AssocArrayLiteralExp es1 = cast(AssocArrayLiteralExp)e1;
        AssocArrayLiteralExp es2 = cast(AssocArrayLiteralExp)e2;
        size_t dim = es1.keys.dim;
        if (es2.keys.dim != dim)
            return 1;
        bool* used = cast(bool*)mem.xmalloc(bool.sizeof * dim);
        memset(used, 0, bool.sizeof * dim);
        foreach (size_t i; 0 .. dim)
        {
            Expression k1 = (*es1.keys)[i];
            Expression v1 = (*es1.values)[i];
            Expression v2 = null;
            foreach (size_t j; 0 .. dim)
            {
                if (used[j])
                    continue;
                Expression k2 = (*es2.keys)[j];
                if (ctfeRawCmp(loc, k1, k2, identity))
                    continue;
                used[j] = true;
                v2 = (*es2.values)[j];
                break;
            }
            if (!v2 || ctfeRawCmp(loc, v1, v2, identity))
            {
                mem.xfree(used);
                return 1;
            }
        }
        mem.xfree(used);
        return 0;
    }
    error(loc, "CTFE internal error: bad compare of `%s` and `%s`", e1.toChars(), e2.toChars());
    assert(0);
}

/// Evaluate ==, !=.  Resolves slices before comparing. Returns 0 or 1
bool ctfeEqual(const ref Loc loc, TOK op, Expression e1, Expression e2)
{
    return !ctfeRawCmp(loc, e1, e2) ^ (op == TOK.notEqual);
}

/// Evaluate is, !is.  Resolves slices before comparing. Returns 0 or 1
bool ctfeIdentity(const ref Loc loc, TOK op, Expression e1, Expression e2)
{
    //printf("ctfeIdentity %s %s\n", e1.toChars(), e2.toChars());
    //printf("ctfeIdentity op = '%s', e1 = %s %s, e2 = %s %s\n", Token::toChars(op),
    //    Token::toChars(e1.op), e1.toChars(), Token::toChars(e2.op), e1.toChars());
    bool cmp;
    if (e1.op == TOK.null_)
    {
        cmp = (e2.op == TOK.null_);
    }
    else if (e2.op == TOK.null_)
    {
        cmp = false;
    }
    else if (e1.op == TOK.symbolOffset && e2.op == TOK.symbolOffset)
    {
        SymOffExp es1 = cast(SymOffExp)e1;
        SymOffExp es2 = cast(SymOffExp)e2;
        cmp = (es1.var == es2.var && es1.offset == es2.offset);
    }
    else if (e1.type.isreal())
        cmp = RealIdentical(e1.toReal(), e2.toReal());
    else if (e1.type.isimaginary())
        cmp = RealIdentical(e1.toImaginary(), e2.toImaginary());
    else if (e1.type.iscomplex())
    {
        complex_t v1 = e1.toComplex();
        complex_t v2 = e2.toComplex();
        cmp = RealIdentical(creall(v1), creall(v2)) && RealIdentical(cimagl(v1), cimagl(v1));
    }
    else
    {
        cmp = !ctfeRawCmp(loc, e1, e2, true);
    }
    if (op == TOK.notIdentity || op == TOK.notEqual)
        cmp ^= true;
    return cmp;
}

/// Evaluate >,<=, etc. Resolves slices before comparing. Returns 0 or 1
bool ctfeCmp(const ref Loc loc, TOK op, Expression e1, Expression e2)
{
    Type t1 = e1.type.toBasetype();
    Type t2 = e2.type.toBasetype();

    if (t1.isString() && t2.isString())
        return specificCmp(op, ctfeRawCmp(loc, e1, e2));
    else if (t1.isreal())
        return realCmp(op, e1.toReal(), e2.toReal());
    else if (t1.isimaginary())
        return realCmp(op, e1.toImaginary(), e2.toImaginary());
    else if (t1.isunsigned() || t2.isunsigned())
        return intUnsignedCmp(op, e1.toInteger(), e2.toInteger());
    else
        return intSignedCmp(op, e1.toInteger(), e2.toInteger());
}

UnionExp ctfeCat(const ref Loc loc, Type type, Expression e1, Expression e2)
{
    Type t1 = e1.type.toBasetype();
    Type t2 = e2.type.toBasetype();
    UnionExp ue;
    if (e2.op == TOK.string_ && e1.op == TOK.arrayLiteral && t1.nextOf().isintegral())
    {
        // [chars] ~ string => string (only valid for CTFE)
        StringExp es1 = cast(StringExp)e2;
        ArrayLiteralExp es2 = cast(ArrayLiteralExp)e1;
        const len = es1.len + es2.elements.dim;
        const sz = es1.sz;
        void* s = mem.xmalloc((len + 1) * sz);
        const data1 = es1.peekData();
        memcpy(cast(char*)s + sz * es2.elements.dim, data1.ptr, data1.length);
        foreach (size_t i; 0 .. es2.elements.dim)
        {
            Expression es2e = (*es2.elements)[i];
            if (es2e.op != TOK.int64)
            {
                emplaceExp!(CTFEExp)(&ue, TOK.cantExpression);
                return ue;
            }
            dinteger_t v = es2e.toInteger();
            Port.valcpy(cast(char*)s + i * sz, v, sz);
        }
        // Add terminating 0
        memset(cast(char*)s + len * sz, 0, sz);
        emplaceExp!(StringExp)(&ue, loc, s[0 .. len * sz], len, sz);
        StringExp es = cast(StringExp)ue.exp();
        es.committed = 0;
        es.type = type;
        return ue;
    }
    if (e1.op == TOK.string_ && e2.op == TOK.arrayLiteral && t2.nextOf().isintegral())
    {
        // string ~ [chars] => string (only valid for CTFE)
        // Concatenate the strings
        StringExp es1 = cast(StringExp)e1;
        ArrayLiteralExp es2 = cast(ArrayLiteralExp)e2;
        const len = es1.len + es2.elements.dim;
        const sz = es1.sz;
        void* s = mem.xmalloc((len + 1) * sz);
        auto slice = es1.peekData();
        memcpy(s, slice.ptr, slice.length);
        foreach (size_t i; 0 .. es2.elements.dim)
        {
            Expression es2e = (*es2.elements)[i];
            if (es2e.op != TOK.int64)
            {
                emplaceExp!(CTFEExp)(&ue, TOK.cantExpression);
                return ue;
            }
            const v = es2e.toInteger();
            Port.valcpy(cast(char*)s + (es1.len + i) * sz, v, sz);
        }
        // Add terminating 0
        memset(cast(char*)s + len * sz, 0, sz);
        emplaceExp!(StringExp)(&ue, loc, s[0 .. len * sz], len, sz);
        StringExp es = cast(StringExp)ue.exp();
        es.sz = sz;
        es.committed = 0; //es1.committed;
        es.type = type;
        return ue;
    }
    if (e1.op == TOK.arrayLiteral && e2.op == TOK.arrayLiteral && t1.nextOf().equals(t2.nextOf()))
    {
        //  [ e1 ] ~ [ e2 ] ---> [ e1, e2 ]
        ArrayLiteralExp es1 = cast(ArrayLiteralExp)e1;
        ArrayLiteralExp es2 = cast(ArrayLiteralExp)e2;
        emplaceExp!(ArrayLiteralExp)(&ue, es1.loc, type, copyLiteralArray(es1.elements));
        es1 = cast(ArrayLiteralExp)ue.exp();
        es1.elements.insert(es1.elements.dim, copyLiteralArray(es2.elements));
        return ue;
    }
    if (e1.op == TOK.arrayLiteral && e2.op == TOK.null_ && t1.nextOf().equals(t2.nextOf()))
    {
        //  [ e1 ] ~ null ----> [ e1 ].dup
        ue = paintTypeOntoLiteralCopy(type, copyLiteral(e1).copy());
        return ue;
    }
    if (e1.op == TOK.null_ && e2.op == TOK.arrayLiteral && t1.nextOf().equals(t2.nextOf()))
    {
        //  null ~ [ e2 ] ----> [ e2 ].dup
        ue = paintTypeOntoLiteralCopy(type, copyLiteral(e2).copy());
        return ue;
    }
    ue = Cat(type, e1, e2);
    return ue;
}

/*  Given an AA literal 'ae', and a key 'e2':
 *  Return ae[e2] if present, or NULL if not found.
 */
Expression findKeyInAA(const ref Loc loc, AssocArrayLiteralExp ae, Expression e2)
{
    /* Search the keys backwards, in case there are duplicate keys
     */
    for (size_t i = ae.keys.dim; i;)
    {
        --i;
        Expression ekey = (*ae.keys)[i];
        const int eq = ctfeEqual(loc, TOK.equal, ekey, e2);
        if (eq)
        {
            return (*ae.values)[i];
        }
    }
    return null;
}

/* Same as for constfold.Index, except that it only works for static arrays,
 * dynamic arrays, and strings. We know that e1 is an
 * interpreted CTFE expression, so it cannot have side-effects.
 */
Expression ctfeIndex(UnionExp* pue, const ref Loc loc, Type type, Expression e1, uinteger_t indx)
{
    //printf("ctfeIndex(e1 = %s)\n", e1.toChars());
    assert(e1.type);
    if (auto es1 = e1.isStringExp())
    {
        if (indx >= es1.len)
        {
            error(loc, "string index %llu is out of bounds `[0 .. %zu]`", indx, es1.len);
            return CTFEExp.cantexp;
        }
        emplaceExp!IntegerExp(pue, loc, es1.charAt(indx), type);
        return pue.exp();
    }

    if (auto ale = e1.isArrayLiteralExp())
    {
        if (indx >= ale.elements.dim)
        {
            error(loc, "array index %llu is out of bounds `%s[0 .. %zu]`", indx, e1.toChars(), ale.elements.dim);
            return CTFEExp.cantexp;
        }
        Expression e = (*ale.elements)[cast(size_t)indx];
        return paintTypeOntoLiteral(pue, type, e);
    }

    assert(0);
}

Expression ctfeCast(UnionExp* pue, const ref Loc loc, Type type, Type to, Expression e)
{
    Expression paint()
    {
        return paintTypeOntoLiteral(pue, to, e);
    }

    if (e.op == TOK.null_)
        return paint();

    if (e.op == TOK.classReference)
    {
        // Disallow reinterpreting class casts. Do this by ensuring that
        // the original class can implicitly convert to the target class
        ClassDeclaration originalClass = (cast(ClassReferenceExp)e).originalClass();
        if (originalClass.type.implicitConvTo(to.mutableOf()))
            return paint();
        else
        {
            emplaceExp!(NullExp)(pue, loc, to);
            return pue.exp();
        }
    }

    // Allow TypeInfo type painting
    if (isTypeInfo_Class(e.type) && e.type.implicitConvTo(to))
        return paint();

    // Allow casting away const for struct literals
    if (e.op == TOK.structLiteral && e.type.toBasetype().castMod(0) == to.toBasetype().castMod(0))
        return paint();

    Expression r;
    if (e.type.equals(type) && type.equals(to))
    {
        // necessary not to change e's address for pointer comparisons
        r = e;
    }
    else if (to.toBasetype().ty == Tarray &&
             type.toBasetype().ty == Tarray &&
             to.toBasetype().nextOf().size() == type.toBasetype().nextOf().size())
    {
        // https://issues.dlang.org/show_bug.cgi?id=12495
        // Array reinterpret casts: eg. string to immutable(ubyte)[]
        return paint();
    }
    else
    {
        *pue = Cast(loc, type, to, e);
        r = pue.exp();
    }

    if (CTFEExp.isCantExp(r))
        error(loc, "cannot cast `%s` to `%s` at compile time", e.toChars(), to.toChars());

    if (auto ae = e.isArrayLiteralExp())
        ae.ownedByCtfe = OwnedBy.ctfe;

    if (auto se = e.isStringExp())
        se.ownedByCtfe = OwnedBy.ctfe;

    return r;
}

/******** Assignment helper functions ***************************/
/* Set dest = src, where both dest and src are container value literals
 * (ie, struct literals, or static arrays (can be an array literal or a string))
 * Assignment is recursively in-place.
 * Purpose: any reference to a member of 'dest' will remain valid after the
 * assignment.
 */
void assignInPlace(Expression dest, Expression src)
{
    if (!(dest.op == TOK.structLiteral || dest.op == TOK.arrayLiteral || dest.op == TOK.string_))
    {
        printf("invalid op %d %d\n", src.op, dest.op);
        assert(0);
    }
    Expressions* oldelems;
    Expressions* newelems;
    if (dest.op == TOK.structLiteral)
    {
        assert(dest.op == src.op);
        oldelems = (cast(StructLiteralExp)dest).elements;
        newelems = (cast(StructLiteralExp)src).elements;
        auto sd = (cast(StructLiteralExp)dest).sd;
        const nfields = sd.nonHiddenFields();
        const nvthis = sd.fields.dim - nfields;
        if (nvthis && oldelems.dim >= nfields && oldelems.dim < newelems.dim)
            foreach (_; 0 .. newelems.dim - oldelems.dim)
                oldelems.push(null);
    }
    else if (dest.op == TOK.arrayLiteral && src.op == TOK.arrayLiteral)
    {
        oldelems = (cast(ArrayLiteralExp)dest).elements;
        newelems = (cast(ArrayLiteralExp)src).elements;
    }
    else if (dest.op == TOK.string_ && src.op == TOK.string_)
    {
        sliceAssignStringFromString(cast(StringExp)dest, cast(StringExp)src, 0);
        return;
    }
    else if (dest.op == TOK.arrayLiteral && src.op == TOK.string_)
    {
        sliceAssignArrayLiteralFromString(cast(ArrayLiteralExp)dest, cast(StringExp)src, 0);
        return;
    }
    else if (src.op == TOK.arrayLiteral && dest.op == TOK.string_)
    {
        sliceAssignStringFromArrayLiteral(cast(StringExp)dest, cast(ArrayLiteralExp)src, 0);
        return;
    }
    else
    {
        printf("invalid op %d %d\n", src.op, dest.op);
        assert(0);
    }
    assert(oldelems.dim == newelems.dim);
    foreach (size_t i; 0 .. oldelems.dim)
    {
        Expression e = (*newelems)[i];
        Expression o = (*oldelems)[i];
        if (e.op == TOK.structLiteral)
        {
            assert(o.op == e.op);
            assignInPlace(o, e);
        }
        else if (e.type.ty == Tsarray && e.op != TOK.void_ && o.type.ty == Tsarray)
        {
            assignInPlace(o, e);
        }
        else
        {
            (*oldelems)[i] = (*newelems)[i];
        }
    }
}

// Given an AA literal aae,  set aae[index] = newval and return newval.
Expression assignAssocArrayElement(const ref Loc loc, AssocArrayLiteralExp aae, Expression index, Expression newval)
{
    /* Create new associative array literal reflecting updated key/value
     */
    Expressions* keysx = aae.keys;
    Expressions* valuesx = aae.values;
    int updated = 0;
    for (size_t j = valuesx.dim; j;)
    {
        j--;
        Expression ekey = (*aae.keys)[j];
        int eq = ctfeEqual(loc, TOK.equal, ekey, index);
        if (eq)
        {
            (*valuesx)[j] = newval;
            updated = 1;
        }
    }
    if (!updated)
    {
        // Append index/newval to keysx[]/valuesx[]
        valuesx.push(newval);
        keysx.push(index);
    }
    return newval;
}

/// Given array literal oldval of type ArrayLiteralExp or StringExp, of length
/// oldlen, change its length to newlen. If the newlen is longer than oldlen,
/// all new elements will be set to the default initializer for the element type.
UnionExp changeArrayLiteralLength(const ref Loc loc, TypeArray arrayType, Expression oldval, size_t oldlen, size_t newlen)
{
    UnionExp ue;
    Type elemType = arrayType.next;
    assert(elemType);
    Expression defaultElem = elemType.defaultInitLiteral(loc);
    auto elements = new Expressions(newlen);
    // Resolve slices
    size_t indxlo = 0;
    if (oldval.op == TOK.slice)
    {
        indxlo = cast(size_t)(cast(SliceExp)oldval).lwr.toInteger();
        oldval = (cast(SliceExp)oldval).e1;
    }
    size_t copylen = oldlen < newlen ? oldlen : newlen;
    if (oldval.op == TOK.string_)
    {
        StringExp oldse = cast(StringExp)oldval;
        void* s = mem.xcalloc(newlen + 1, oldse.sz);
        const data = oldse.peekData();
        memcpy(s, data.ptr, copylen * oldse.sz);
        const defaultValue = cast(uint)defaultElem.toInteger();
        foreach (size_t elemi; copylen .. newlen)
        {
            switch (oldse.sz)
            {
            case 1:
                (cast(char*)s)[cast(size_t)(indxlo + elemi)] = cast(char)defaultValue;
                break;
            case 2:
                (cast(wchar*)s)[cast(size_t)(indxlo + elemi)] = cast(wchar)defaultValue;
                break;
            case 4:
                (cast(dchar*)s)[cast(size_t)(indxlo + elemi)] = cast(dchar)defaultValue;
                break;
            default:
                assert(0);
            }
        }
        emplaceExp!(StringExp)(&ue, loc, s[0 .. newlen * oldse.sz], newlen, oldse.sz);
        StringExp se = cast(StringExp)ue.exp();
        se.type = arrayType;
        se.sz = oldse.sz;
        se.committed = oldse.committed;
        se.ownedByCtfe = OwnedBy.ctfe;
    }
    else
    {
        if (oldlen != 0)
        {
            assert(oldval.op == TOK.arrayLiteral);
            ArrayLiteralExp ae = cast(ArrayLiteralExp)oldval;
            foreach (size_t i; 0 .. copylen)
                (*elements)[i] = (*ae.elements)[indxlo + i];
        }
        if (elemType.ty == Tstruct || elemType.ty == Tsarray)
        {
            /* If it is an aggregate literal representing a value type,
             * we need to create a unique copy for each element
             */
            foreach (size_t i; copylen .. newlen)
                (*elements)[i] = copyLiteral(defaultElem).copy();
        }
        else
        {
            foreach (size_t i; copylen .. newlen)
                (*elements)[i] = defaultElem;
        }
        emplaceExp!(ArrayLiteralExp)(&ue, loc, arrayType, elements);
        ArrayLiteralExp aae = cast(ArrayLiteralExp)ue.exp();
        aae.ownedByCtfe = OwnedBy.ctfe;
    }
    return ue;
}

/*************************** CTFE Sanity Checks ***************************/

bool isCtfeValueValid(Expression newval)
{
    Type tb = newval.type.toBasetype();
    switch (newval.op)
    {
        case TOK.int64:
        case TOK.float64:
        case TOK.char_:
        case TOK.complex80:
            return tb.isscalar();

        case TOK.null_:
            return tb.ty == Tnull    ||
                   tb.ty == Tpointer ||
                   tb.ty == Tarray   ||
                   tb.ty == Taarray  ||
                   tb.ty == Tclass   ||
                   tb.ty == Tdelegate;

        case TOK.string_:
            return true; // CTFE would directly use the StringExp in AST.

        case TOK.arrayLiteral:
            return true; //((ArrayLiteralExp *)newval)->ownedByCtfe;

        case TOK.assocArrayLiteral:
            return true; //((AssocArrayLiteralExp *)newval)->ownedByCtfe;

        case TOK.structLiteral:
            return true; //((StructLiteralExp *)newval)->ownedByCtfe;

        case TOK.classReference:
            return true;

        case TOK.type:
            return true;

        case TOK.vector:
            return true; // vector literal

        case TOK.function_:
            return true; // function literal or delegate literal

        case TOK.delegate_:
        {
            // &struct.func or &clasinst.func
            // &nestedfunc
            Expression ethis = (cast(DelegateExp)newval).e1;
            return (ethis.op == TOK.structLiteral || ethis.op == TOK.classReference || ethis.op == TOK.variable && (cast(VarExp)ethis).var == (cast(DelegateExp)newval).func);
        }

        case TOK.symbolOffset:
        {
            // function pointer, or pointer to static variable
            Declaration d = (cast(SymOffExp)newval).var;
            return d.isFuncDeclaration() || d.isDataseg();
        }

        case TOK.typeid_:
        {
            // always valid
            return true;
        }

        case TOK.address:
        {
            // e1 should be a CTFE reference
            Expression e1 = (cast(AddrExp)newval).e1;
            return tb.ty == Tpointer &&
            (
                (e1.op == TOK.structLiteral || e1.op == TOK.arrayLiteral) && isCtfeValueValid(e1) ||
                 e1.op == TOK.variable ||
                 e1.op == TOK.dotVariable && isCtfeReferenceValid(e1) ||
                 e1.op == TOK.index && isCtfeReferenceValid(e1) ||
                 e1.op == TOK.slice && e1.type.toBasetype().ty == Tsarray
            );
        }

        case TOK.slice:
        {
            // e1 should be an array aggregate
            const SliceExp se = cast(SliceExp)newval;
            assert(se.lwr && se.lwr.op == TOK.int64);
            assert(se.upr && se.upr.op == TOK.int64);
            return (tb.ty == Tarray || tb.ty == Tsarray) && (se.e1.op == TOK.string_ || se.e1.op == TOK.arrayLiteral);
        }

        case TOK.void_:
            return true; // uninitialized value

        default:
            newval.error("CTFE internal error: illegal CTFE value `%s`", newval.toChars());
            return false;
    }
}

bool isCtfeReferenceValid(Expression newval)
{
    switch (newval.op)
    {
        case TOK.this_:
            return true;

        case TOK.variable:
        {
            const VarDeclaration v = (cast(VarExp)newval).var.isVarDeclaration();
            assert(v);
            // Must not be a reference to a reference
            return true;
        }

        case TOK.index:
        {
            const Expression eagg = (cast(IndexExp)newval).e1;
            return eagg.op == TOK.string_ || eagg.op == TOK.arrayLiteral || eagg.op == TOK.assocArrayLiteral;
        }

        case TOK.dotVariable:
        {
            Expression eagg = (cast(DotVarExp)newval).e1;
            return (eagg.op == TOK.structLiteral || eagg.op == TOK.classReference) && isCtfeValueValid(eagg);
        }

        default:
            // Internally a ref variable may directly point a stack memory.
            // e.g. ref int v = 1;
            return isCtfeValueValid(newval);
    }
}

// Used for debugging only
void showCtfeExpr(Expression e, int level = 0)
{
    for (int i = level; i > 0; --i)
        printf(" ");
    Expressions* elements = null;
    // We need the struct definition to detect block assignment
    StructDeclaration sd = null;
    ClassDeclaration cd = null;
    if (e.op == TOK.structLiteral)
    {
        elements = (cast(StructLiteralExp)e).elements;
        sd = (cast(StructLiteralExp)e).sd;
        printf("STRUCT type = %s %p:\n", e.type.toChars(), e);
    }
    else if (e.op == TOK.classReference)
    {
        elements = (cast(ClassReferenceExp)e).value.elements;
        cd = (cast(ClassReferenceExp)e).originalClass();
        printf("CLASS type = %s %p:\n", e.type.toChars(), (cast(ClassReferenceExp)e).value);
    }
    else if (e.op == TOK.arrayLiteral)
    {
        elements = (cast(ArrayLiteralExp)e).elements;
        printf("ARRAY LITERAL type=%s %p:\n", e.type.toChars(), e);
    }
    else if (e.op == TOK.assocArrayLiteral)
    {
        printf("AA LITERAL type=%s %p:\n", e.type.toChars(), e);
    }
    else if (e.op == TOK.string_)
    {
        printf("STRING %s %p\n", e.toChars(), e.isStringExp.peekString.ptr);
    }
    else if (e.op == TOK.slice)
    {
        printf("SLICE %p: %s\n", e, e.toChars());
        showCtfeExpr((cast(SliceExp)e).e1, level + 1);
    }
    else if (e.op == TOK.variable)
    {
        printf("VAR %p %s\n", e, e.toChars());
        VarDeclaration v = (cast(VarExp)e).var.isVarDeclaration();
        if (v && getValue(v))
            showCtfeExpr(getValue(v), level + 1);
    }
    else if (e.op == TOK.address)
    {
        // This is potentially recursive. We mustn't try to print the thing we're pointing to.
        printf("POINTER %p to %p: %s\n", e, (cast(AddrExp)e).e1, e.toChars());
    }
    else
        printf("VALUE %p: %s\n", e, e.toChars());
    if (elements)
    {
        size_t fieldsSoFar = 0;
        for (size_t i = 0; i < elements.dim; i++)
        {
            Expression z = null;
            VarDeclaration v = null;
            if (i > 15)
            {
                printf("...(total %d elements)\n", cast(int)elements.dim);
                return;
            }
            if (sd)
            {
                v = sd.fields[i];
                z = (*elements)[i];
            }
            else if (cd)
            {
                while (i - fieldsSoFar >= cd.fields.dim)
                {
                    fieldsSoFar += cd.fields.dim;
                    cd = cd.baseClass;
                    for (int j = level; j > 0; --j)
                        printf(" ");
                    printf(" BASE CLASS: %s\n", cd.toChars());
                }
                v = cd.fields[i - fieldsSoFar];
                assert((elements.dim + i) >= (fieldsSoFar + cd.fields.dim));
                size_t indx = (elements.dim - fieldsSoFar) - cd.fields.dim + i;
                assert(indx < elements.dim);
                z = (*elements)[indx];
            }
            if (!z)
            {
                for (int j = level; j > 0; --j)
                    printf(" ");
                printf(" void\n");
                continue;
            }
            if (v)
            {
                // If it is a void assignment, use the default initializer
                if ((v.type.ty != z.type.ty) && v.type.ty == Tsarray)
                {
                    for (int j = level; --j;)
                        printf(" ");
                    printf(" field: block initialized static array\n");
                    continue;
                }
            }
            showCtfeExpr(z, level + 1);
        }
    }
}

/*************************** Void initialization ***************************/
UnionExp voidInitLiteral(Type t, VarDeclaration var)
{
    UnionExp ue;
    if (t.ty == Tsarray)
    {
        TypeSArray tsa = cast(TypeSArray)t;
        Expression elem = voidInitLiteral(tsa.next, var).copy();
        // For aggregate value types (structs, static arrays) we must
        // create an a separate copy for each element.
        const mustCopy = (elem.op == TOK.arrayLiteral || elem.op == TOK.structLiteral);
        const d = cast(size_t)tsa.dim.toInteger();
        auto elements = new Expressions(d);
        foreach (i; 0 .. d)
        {
            if (mustCopy && i > 0)
                elem = copyLiteral(elem).copy();
            (*elements)[i] = elem;
        }
        emplaceExp!(ArrayLiteralExp)(&ue, var.loc, tsa, elements);
        ArrayLiteralExp ae = cast(ArrayLiteralExp)ue.exp();
        ae.ownedByCtfe = OwnedBy.ctfe;
    }
    else if (t.ty == Tstruct)
    {
        TypeStruct ts = cast(TypeStruct)t;
        auto exps = new Expressions(ts.sym.fields.dim);
        foreach (size_t i;  0 .. ts.sym.fields.dim)
        {
            (*exps)[i] = voidInitLiteral(ts.sym.fields[i].type, ts.sym.fields[i]).copy();
        }
        emplaceExp!(StructLiteralExp)(&ue, var.loc, ts.sym, exps);
        StructLiteralExp se = cast(StructLiteralExp)ue.exp();
        se.type = ts;
        se.ownedByCtfe = OwnedBy.ctfe;
    }
    else
        emplaceExp!(VoidInitExp)(&ue, var);
    return ue;
}
