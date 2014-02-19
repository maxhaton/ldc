
// Compiler implementation of the D programming language
// Copyright (c) 1999-2013 by Digital Mars
// All Rights Reserved
// written by Walter Bright
// http://www.digitalmars.com
// License for redistribution is by either the Artistic License
// in artistic.txt, or the GNU General Public License in gnu.txt.
// See the included readme.txt for details.

#include        <stdio.h>
#include        <string.h>
#include        <time.h>

#include        "port.h"
#include        "target.h"

#include        "lexer.h"
#include        "expression.h"
#include        "mtype.h"
#include        "dsymbol.h"
#include        "declaration.h"
#include        "enum.h"
#include        "aggregate.h"
#include        "attrib.h"
#include        "module.h"
#include        "init.h"
#include        "template.h"

#include        "mem.h" // for tk/mem_malloc

#include        "cc.h"
#include        "el.h"
#include        "oper.h"
#include        "global.h"
#include        "code.h"
#include        "type.h"
#include        "dt.h"
#include        "irstate.h"
#include        "id.h"
#include        "type.h"
#include        "toir.h"
#include        "ctfe.h"
static char __file__[] = __FILE__;      /* for tassert.h                */
#include        "tassert.h"

typedef Array<elem *> Elems;

elem *addressElem(elem *e, Type *t, bool alwaysCopy = false);
elem *array_toPtr(Type *t, elem *e);
elem *appendDtors(IRState *irs, elem *er, size_t starti, size_t endi);
VarDeclarations *VarDeclarations_create();
type *Type_toCtype(Type *t);
elem *toElemDtor(Expression *e, IRState *irs);
unsigned totym(Type *tx);

#define el_setLoc(e,loc)        ((e)->Esrcpos.Sfilename = (char *)(loc).filename, \
                                 (e)->Esrcpos.Slinnum = (loc).linnum, \
                                 (e)->Esrcpos.Scharnum = (loc).charnum)

/* If variable var of type typ is a reference
 */
bool ISREF(Declaration *var, Type *tb)
{
    return (var->isParameter() && config.exe == EX_WIN64 && (var->type->size(Loc()) > REGSIZE || var->storage_class & STClazy))
            || var->isOut() || var->isRef();
}

/* If variable var of type typ is a reference due to Win64 calling conventions
 */
bool ISWIN64REF(Declaration *var)
{
    return (config.exe == EX_WIN64 && var->isParameter() &&
            (var->type->size(Loc()) > REGSIZE || var->storage_class & STClazy)) &&
            !(var->isOut() || var->isRef());
}

/******************************************
 * If argument to a function should use OPstrpar,
 * fix it so it does and return it.
 */
elem *useOPstrpar(elem *e)
{
    tym_t ty = tybasic(e->Ety);
    if (ty == TYstruct || ty == TYarray)
    {
        e = el_una(OPstrpar, TYstruct, e);
        e->ET = e->E1->ET;
        assert(e->ET);
    }
    return e;
}

/************************************
 * Call a function.
 */

elem *callfunc(Loc loc,
        IRState *irs,
        int directcall,         // 1: don't do virtual call
        Type *tret,             // return type
        elem *ec,               // evaluates to function address
        Type *ectype,           // original type of ec
        FuncDeclaration *fd,    // if !=NULL, this is the function being called
        Type *t,                // TypeDelegate or TypeFunction for this function
        elem *ehidden,          // if !=NULL, this is the 'hidden' argument
        Expressions *arguments)
{
    elem *ep;
    elem *e;
    elem *ethis = NULL;
    elem *eside = NULL;
    tym_t ty;
    tym_t tyret;
    RET retmethod;
    int reverse;
    TypeFunction *tf;
    int op;
    elem *eresult = ehidden;

#if 0
    printf("callfunc(directcall = %d, tret = '%s', ec = %p, fd = %p)\n",
        directcall, tret->toChars(), ec, fd);
    printf("ec: "); elem_print(ec);
    if (fd)
        printf("fd = '%s', vtblIndex = %d, isVirtual() = %d\n", fd->toChars(), fd->vtblIndex, fd->isVirtual());
    if (ehidden)
    {   printf("ehidden: "); elem_print(ehidden); }
#endif

    t = t->toBasetype();
    if (t->ty == Tdelegate)
    {
        // A delegate consists of:
        //      { Object *this; Function *funcptr; }
        assert(!fd);
        assert(t->nextOf()->ty == Tfunction);
        tf = (TypeFunction *)(t->nextOf());
        ethis = ec;
        ec = el_same(&ethis);
        ethis = el_una(I64 ? OP128_64 : OP64_32, TYnptr, ethis); // get this
        ec = array_toPtr(t, ec);                // get funcptr
        ec = el_una(OPind, totym(tf), ec);
    }
    else
    {
        assert(t->ty == Tfunction);
        tf = (TypeFunction *)(t);
    }
    retmethod = retStyle(tf);
    ty = ec->Ety;
    if (fd)
        ty = fd->toSymbol()->Stype->Tty;
    reverse = tyrevfunc(ty);
    ep = NULL;
    op = (ec->Eoper == OPvar) ? intrinsic_op(ec->EV.sp.Vsym->Sident) : -1;
    if (arguments)
    {
        for (size_t i = 0; i < arguments->dim; i++)
        {
        Lagain:
            Expression *arg = (*arguments)[i];
            assert(arg->op != TOKtuple);
            if (arg->op == TOKcomma)
            {
                CommaExp *ce = (CommaExp *)arg;
                eside = el_combine(eside, ce->e1->toElem(irs));
                (*arguments)[i] = ce->e2;
                goto Lagain;
            }
        }

        // j=1 if _arguments[] is first argument
        int j = (tf->linkage == LINKd && tf->varargs == 1);

        for (size_t i = 0; i < arguments->dim ; i++)
        {
            Expression *arg = (*arguments)[i];
            elem *ea;

            //printf("\targ[%d]: %s\n", i, arg->toChars());

            size_t nparams = Parameter::dim(tf->parameters);
            if (i - j < nparams && i >= j)
            {
                Parameter *p = Parameter::getNth(tf->parameters, i - j);

                if (p->storageClass & (STCout | STCref))
                {
                    // Convert argument to a pointer,
                    // use AddrExp::toElem()
                    Expression *ae = arg->addressOf();
                    ea = ae->toElem(irs);
                    goto L1;
                }
            }
            if (config.exe == EX_WIN64 && arg->type->size(arg->loc) > REGSIZE && op == -1)
            {
                /* Copy to a temporary, and make the argument a pointer
                 * to that temporary.
                 */
                ea = arg->toElem(irs);
                ea = addressElem(ea, arg->type, true);
                goto L1;
            }
            ea = arg->toElem(irs);
            if (config.exe == EX_WIN64 && tybasic(ea->Ety) == TYcfloat)
            {
                /* Treat a cfloat like it was a struct { float re,im; }
                 */
                ea->Ety = TYllong;
            }
        L1:
            ea = useOPstrpar(ea);
            if (reverse)
                ep = el_param(ep,ea);
            else
                ep = el_param(ea,ep);
        }
    }

    if (retmethod == RETstack)
    {
        if (!ehidden)
        {
            // Don't have one, so create one
            type *tc;

            Type *tret = tf->next;
            if (tret->toBasetype()->ty == Tstruct ||
                tret->toBasetype()->ty == Tsarray)
                tc = Type_toCtype(tret);
            else
                tc = type_fake(totym(tret));
            Symbol *stmp = symbol_genauto(tc);
            ehidden = el_ptr(stmp);
            eresult = ehidden;
        }
        if ((global.params.isLinux ||
             global.params.isOSX ||
             global.params.isFreeBSD ||
             global.params.isSolaris) && tf->linkage != LINKd)
            ;   // ehidden goes last on Linux/OSX C++
        else
        {
            if (ep)
            {
#if 0 // BUG: implement
                if (reverse && type_mangle(tfunc) == mTYman_cpp)
                    ep = el_param(ehidden,ep);
                else
#endif
                    ep = el_param(ep,ehidden);
            }
            else
                ep = ehidden;
            ehidden = NULL;
        }
    }

    if (fd && fd->isMember2())
    {
        assert(op == -1);       // members should not be intrinsics

        AggregateDeclaration *ad = fd->isThis();
        if (ad)
        {
            ethis = ec;
            if (ad->isStructDeclaration() && tybasic(ec->Ety) != TYnptr)
            {
                ethis = addressElem(ec, ectype);
            }
        }
        else
        {
            // Evaluate ec for side effects
            eside = el_combine(ec, eside);
        }
        Symbol *sfunc = fd->toSymbol();

        if (!fd->isVirtual() ||
            directcall ||               // BUG: fix
            fd->isFinalFunc()
           /* Future optimization: || (whole program analysis && not overridden)
            */
           )
        {
            // make static call
            ec = el_var(sfunc);
        }
        else
        {
            // make virtual call
            assert(ethis);
            elem *ev = el_same(&ethis);
            ev = el_una(OPind, TYnptr, ev);
            unsigned vindex = fd->vtblIndex;
            assert((int)vindex >= 0);

            // Build *(ev + vindex * 4)
if (I32) assert(tysize[TYnptr] == 4);
            ec = el_bin(OPadd,TYnptr,ev,el_long(TYsize_t, vindex * tysize[TYnptr]));
            ec = el_una(OPind,TYnptr,ec);
            ec = el_una(OPind,tybasic(sfunc->Stype->Tty),ec);
        }
    }
    else if (fd && fd->isNested())
    {
        assert(!ethis);
        ethis = getEthis(loc, irs, fd);
    }

    ep = el_param(ep, ethis);
    if (ehidden)
        ep = el_param(ep, ehidden);     // if ehidden goes last

    tyret = totym(tret);

    // Look for intrinsic functions
    if (ec->Eoper == OPvar && op != -1)
    {
        el_free(ec);
        if (OTbinary(op))
        {
            ep->Eoper = op;
            ep->Ety = tyret;
            e = ep;
#if TX86
            if (op == OPscale)
            {
                elem *et = e->E1;
                e->E1 = el_una(OPs32_d, TYdouble, e->E2);
                e->E1 = el_una(OPd_ld, TYldouble, e->E1);
                e->E2 = et;
            }
            else if (op == OPyl2x || op == OPyl2xp1)
            {
                elem *et = e->E1;
                e->E1 = e->E2;
                e->E2 = et;
            }
#endif
        }
        else if (op == OPvector)
        {
            e = ep;
            /* Recognize store operations as:
             *  ((op OPparam op1) OPparam op2)
             * Rewrite as:
             *  (op1 OPvecsto (op OPparam op2))
             * A separate operation is used for stores because it
             * has a side effect, and so takes a different path through
             * the optimizer.
             */
            if (e->Eoper == OPparam &&
                e->E1->Eoper == OPparam &&
                e->E1->E1->Eoper == OPconst &&
                isXMMstore(el_tolong(e->E1->E1)))
            {
                //printf("OPvecsto\n");
                elem *tmp = e->E2;
                e->E2 = e->E1;
                e->E1 = e->E2->E2;
                e->E2->E2 = tmp;
                e->Eoper = OPvecsto;
                e->Ety = tyret;
            }
            else
                e = el_una(op,tyret,ep);
        }
        else
            e = el_una(op,tyret,ep);
    }
    else
    {   /* Do not do "no side effect" calls if a hidden parameter is passed,
         * as the return value is stored through the hidden parameter, which
         * is a side effect.
         */
        //printf("1: fd = %p prity = %d, nothrow = %d, retmethod = %d, use-assert = %d\n",
        //       fd, (fd ? fd->isPure() : tf->purity), tf->isnothrow, retmethod, global.params.useAssert);
        //printf("\tfd = %s, tf = %s\n", fd->toChars(), tf->toChars());
        /* assert() has 'implicit side effect' so disable this optimization.
         */
        int ns = ((fd ? fd->isPure() : tf->purity) == PUREstrong &&
                  tf->isnothrow && (retmethod != RETstack) &&
                  !global.params.useAssert && global.params.optimize);
        if (ep)
            e = el_bin(ns ? OPcallns : OPcall, tyret, ec, ep);
        else
            e = el_una(ns ? OPucallns : OPucall, tyret, ec);

        if (tf->varargs)
            e->Eflags |= EFLAGS_variadic;
    }

    if (retmethod == RETstack)
    {
        if (global.params.isOSX && eresult)
            /* ABI quirk: hidden pointer is not returned in registers
             */
            e = el_combine(e, el_copytree(eresult));
        e->Ety = TYnptr;
        e = el_una(OPind, tyret, e);
    }

    if (tf->isref)
    {
        e->Ety = TYnptr;
        e = el_una(OPind, tyret, e);
    }

    if (tybasic(tyret) == TYstruct)
    {
        e->ET = Type_toCtype(tret);
    }
    e = el_combine(eside, e);
    return e;
}

/*******************************************
 * Take address of an elem.
 */

elem *addressElem(elem *e, Type *t, bool alwaysCopy)
{
    //printf("addressElem()\n");

    elem **pe;
    for (pe = &e; (*pe)->Eoper == OPcomma; pe = &(*pe)->E2)
        ;

    // For conditional operator, both branches need conversion.
    if ((*pe)->Eoper == OPcond)
    {
        elem *ec = (*pe)->E2;

        ec->E1 = addressElem(ec->E1, t, alwaysCopy);
        ec->E2 = addressElem(ec->E2, t, alwaysCopy);

        (*pe)->Ejty = (*pe)->Ety = ec->E1->Ety;
        (*pe)->ET = ec->E1->ET;

        e->Ety = TYnptr;
        return e;
    }

    if (alwaysCopy || ((*pe)->Eoper != OPvar && (*pe)->Eoper != OPind))
    {
        elem *e2 = *pe;
        type *tx;

        // Convert to ((tmp=e2),tmp)
        TY ty;
        if (t && ((ty = t->toBasetype()->ty) == Tstruct || ty == Tsarray))
            tx = Type_toCtype(t);
        else if (tybasic(e2->Ety) == TYstruct)
        {
            assert(t);                  // don't know of a case where this can be NULL
            tx = Type_toCtype(t);
        }
        else
            tx = type_fake(e2->Ety);
        Symbol *stmp = symbol_genauto(tx);
        elem *eeq = el_bin(OPeq,e2->Ety,el_var(stmp),e2);
        if (tybasic(e2->Ety) == TYstruct)
        {
            eeq->Eoper = OPstreq;
            eeq->ET = e2->ET;
        }
        else if (tybasic(e2->Ety) == TYarray)
        {
            eeq->Eoper = OPstreq;
            eeq->Ejty = eeq->Ety = TYstruct;
            eeq->ET = t ? Type_toCtype(t) : tx;
        }
        *pe = el_bin(OPcomma,e2->Ety,eeq,el_var(stmp));
    }
    e = el_una(OPaddr,TYnptr,e);
    return e;
}

/*****************************************
 * Convert array to a pointer to the data.
 */

elem *array_toPtr(Type *t, elem *e)
{
    //printf("array_toPtr()\n");
    //elem_print(e);
    t = t->toBasetype();
    switch (t->ty)
    {
        case Tpointer:
            break;

        case Tarray:
        case Tdelegate:
            if (e->Eoper == OPcomma)
            {
                e->Ety = TYnptr;
                e->E2 = array_toPtr(t, e->E2);
            }
            else if (e->Eoper == OPpair)
            {
                e->Eoper = OPcomma;
                e->Ety = TYnptr;
            }
            else
            {
#if 1
                e = el_una(OPmsw, TYnptr, e);
#else
                e = el_una(OPaddr, TYnptr, e);
                e = el_bin(OPadd, TYnptr, e, el_long(TYsize_t, 4));
                e = el_una(OPind, TYnptr, e);
#endif
            }
            break;

        case Tsarray:
            //e = el_una(OPaddr, TYnptr, e);
            e = addressElem(e, t);
            break;

        default:
            t->print();
            assert(0);
    }
    return e;
}

/*****************************************
 * Convert array to a dynamic array.
 */

elem *array_toDarray(Type *t, elem *e)
{
    unsigned dim;
    elem *ef = NULL;
    elem *ex;

    //printf("array_toDarray(t = %s)\n", t->toChars());
    //elem_print(e);
    t = t->toBasetype();
    switch (t->ty)
    {
        case Tarray:
            break;

        case Tsarray:
            e = addressElem(e, t);
            dim = ((TypeSArray *)t)->dim->toInteger();
            e = el_pair(TYdarray, el_long(TYsize_t, dim), e);
            break;

        default:
        L1:
            switch (e->Eoper)
            {
                case OPconst:
                {
                    size_t len = tysize[tybasic(e->Ety)];
                    elem *es = el_calloc();
                    es->Eoper = OPstring;

                    // freed in el_free
                    es->EV.ss.Vstring = (char *)mem_malloc(len);
                    memcpy(es->EV.ss.Vstring, &e->EV, len);

                    es->EV.ss.Vstrlen = len;
                    es->Ety = TYnptr;
                    e = es;
                    break;
                }

                case OPvar:
                    e = el_una(OPaddr, TYnptr, e);
                    break;

                case OPcomma:
                    ef = el_combine(ef, e->E1);
                    ex = e;
                    e = e->E2;
                    ex->E1 = NULL;
                    ex->E2 = NULL;
                    el_free(ex);
                    goto L1;

                case OPind:
                    ex = e;
                    e = e->E1;
                    ex->E1 = NULL;
                    ex->E2 = NULL;
                    el_free(ex);
                    break;

                default:
                {
                    // Copy expression to a variable and take the
                    // address of that variable.
                    Symbol *stmp;
                    tym_t ty = tybasic(e->Ety);

                    if (ty == TYstruct)
                    {   unsigned sz = type_size(e->ET);
                        if (sz <= 4)
                            ty = TYint;
                        else if (sz <= 8)
                            ty = TYllong;
                        else if (sz <= 16)
                            ty = TYcent;
                    }
                    e->Ety = ty;
                    stmp = symbol_genauto(type_fake(ty));
                    e = el_bin(OPeq, e->Ety, el_var(stmp), e);
                    e = el_bin(OPcomma, TYnptr, e, el_una(OPaddr, TYnptr, el_var(stmp)));
                    break;
                }
            }
            dim = 1;
            e = el_pair(TYdarray, el_long(TYsize_t, dim), e);
            break;
    }
    return el_combine(ef, e);
}

/************************************
 */

elem *sarray_toDarray(Loc loc, Type *tfrom, Type *tto, elem *e)
{
    //printf("sarray_toDarray()\n");
    //elem_print(e);

    dinteger_t dim = ((TypeSArray *)tfrom)->dim->toInteger();

    if (tto)
    {
        unsigned fsize = tfrom->nextOf()->size();
        unsigned tsize = tto->nextOf()->size();

        if ((dim * fsize) % tsize != 0)
        {
            error(loc, "cannot cast %s to %s since sizes don't line up", tfrom->toChars(), tto->toChars());
        }
        dim = (dim * fsize) / tsize;
    }
    elem *elen = el_long(TYsize_t, dim);
    e = addressElem(e, tfrom);
    e = el_pair(TYdarray, elen, e);
    return e;
}

/********************************************
 * Determine if t is an array of structs that need a postblit.
 */

StructDeclaration *needsPostblit(Type *t)
{
    t = t->baseElemOf();
    if (t->ty == Tstruct)
    {
        StructDeclaration *sd = ((TypeStruct *)t)->sym;
        if (sd->postblit)
            return sd;
    }
    return NULL;
}

/*******************************************
 * Set an array pointed to by eptr to evalue:
 *      eptr[0..edim] = evalue;
 * Input:
 *      eptr    where to write the data to
 *      evalue  value to write
 *      edim    number of times to write evalue to eptr[]
 *      tb      type of evalue
 */

elem *setArray(elem *eptr, elem *edim, Type *tb, elem *evalue, IRState *irs, int op)
{   int r;
    elem *e;
    unsigned sz = tb->size();

Lagain:
    switch (tb->ty)
    {
        case Tfloat80:
        case Timaginary80:
            r = RTLSYM_MEMSET80;
            break;
        case Tcomplex80:
            r = RTLSYM_MEMSET160;
            break;
        case Tcomplex64:
            r = RTLSYM_MEMSET128;
            break;
        case Tfloat32:
        case Timaginary32:
            if (I32)
                goto Ldefault;          // legacy binary compatibility
            r = RTLSYM_MEMSETFLOAT;
            break;
        case Tfloat64:
        case Timaginary64:
            if (I32)
                goto Ldefault;          // legacy binary compatibility
            r = RTLSYM_MEMSETDOUBLE;
            break;

        case Tstruct:
            if (I32)
                goto Ldefault;

        {   TypeStruct *tc = (TypeStruct *)tb;
            StructDeclaration *sd = tc->sym;
            if (sd->arg1type && !sd->arg2type)
            {
                tb = sd->arg1type;
                goto Lagain;
            }
            goto Ldefault;
        }

        default:
        Ldefault:
            switch (sz)
            {
                case 1:      r = RTLSYM_MEMSET8;    break;
                case 2:      r = RTLSYM_MEMSET16;   break;
                case 4:      r = RTLSYM_MEMSET32;   break;
                case 8:      r = RTLSYM_MEMSET64;   break;
                case 16:     r = I64 ? RTLSYM_MEMSET128ii : RTLSYM_MEMSET128; break;
                default:     r = RTLSYM_MEMSETN;    break;
            }

            /* Determine if we need to do postblit
             */
            if (op != TOKblit)
            {
                StructDeclaration *sd = needsPostblit(tb);
                if (sd)
                {   /* Need to do postblit.
                     *   void *_d_arraysetassign(void *p, void *value, int dim, TypeInfo ti);
                     */
                    r = (op == TOKconstruct) ? RTLSYM_ARRAYSETCTOR : RTLSYM_ARRAYSETASSIGN;
                    evalue = el_una(OPaddr, TYnptr, evalue);
                    Expression *ti = tb->getTypeInfo(NULL);
                    elem *eti = ti->toElem(irs);
                    e = el_params(eti, edim, evalue, eptr, NULL);
                    e = el_bin(OPcall,TYnptr,el_var(rtlsym[r]),e);
                    return e;
                }
            }

            if (I64 && tybasic(evalue->Ety) == TYstruct && r != RTLSYM_MEMSETN)
            {
                /* If this struct is in-memory only, i.e. cannot necessarily be passed as
                 * a gp register parameter.
                 * The trouble is that memset() is expecting the argument to be in a gp
                 * register, but the argument pusher may have other ideas on I64.
                 * MEMSETN is inefficient, though.
                 */
                if (tybasic(evalue->ET->Tty) == TYstruct &&
                    !evalue->ET->Ttag->Sstruct->Sarg1type &&
                    !evalue->ET->Ttag->Sstruct->Sarg2type)
                    r = RTLSYM_MEMSETN;
            }

            if (r == RTLSYM_MEMSETN)
            {
                // void *_memsetn(void *p, void *value, int dim, int sizelem)
                evalue = el_una(OPaddr, TYnptr, evalue);
                elem *esz = el_long(TYsize_t, sz);
                e = el_params(esz, edim, evalue, eptr, NULL);
                e = el_bin(OPcall,TYnptr,el_var(rtlsym[r]),e);
                return e;
            }
            break;
    }
    if (sz > 1 && sz <= 8 &&
        evalue->Eoper == OPconst && el_allbits(evalue, 0))
    {
        r = RTLSYM_MEMSET8;
        edim = el_bin(OPmul, TYsize_t, edim, el_long(TYsize_t, sz));
    }

    if (config.exe == EX_WIN64 && sz > REGSIZE)
    {
        evalue = addressElem(evalue, tb);
    }

    evalue = useOPstrpar(evalue);

    // Be careful about parameter side effect ordering
    if (r == RTLSYM_MEMSET8)
    {
        e = el_param(edim, evalue);
        e = el_bin(OPmemset,TYnptr,eptr,e);
    }
    else
    {
        e = el_params(edim, evalue, eptr, NULL);
        e = el_bin(OPcall,TYnptr,el_var(rtlsym[r]),e);
    }
    return e;
}

struct StringTab
{
    Module *m;          // module we're generating code for
    Symbol *si;
    void *string;
    size_t sz;
    size_t len;
};

#define STSIZE 16
StringTab stringTab[STSIZE];
size_t stidx;

static Symbol *assertexp_sfilename = NULL;
static const char *assertexp_name = NULL;
static Module *assertexp_mn = NULL;

void clearStringTab()
{
    //printf("clearStringTab()\n");
    memset(stringTab, 0, sizeof(stringTab));
    stidx = 0;

    assertexp_sfilename = NULL;
    assertexp_name = NULL;
    assertexp_mn = NULL;
}

elem *toElem(Expression *e, IRState *irs)
{
    class ToElemVisitor : public Visitor
    {
    public:
        IRState *irs;
        elem *result;

        ToElemVisitor(IRState *irs)
            : irs(irs)
        {
            result = NULL;
        }

        /***************************************
         */

        void visit(Expression *e)
        {
            printf("[%s] %s ", e->loc.toChars(), Token::toChars(e->op));
            e->print();
            assert(0);
            result = NULL;
        }

        /************************************
         */
        void visit(SymbolExp *se)
        {
            elem *e;
            tym_t tym;
            Type *tb = (se->op == TOKsymoff) ? se->var->type->toBasetype() : se->type->toBasetype();
            int offset = (se->op == TOKsymoff) ? ((SymOffExp*)se)->offset : 0;
            VarDeclaration *v = se->var->isVarDeclaration();

            //printf("SymbolExp::toElem('%s') %p, %s\n", toChars(), se, se->type->toChars());
            //printf("\tparent = '%s'\n", se->var->parent ? se->var->parent->toChars() : "null");
            if (se->op == TOKvar && se->var->needThis())
            {
                se->error("need 'this' to access member %s", se->toChars());
                result = el_long(TYsize_t, 0);
                return;
            }

            /* The magic variable __ctfe is always false at runtime
             */
            if (se->op == TOKvar && v && v->ident == Id::ctfe)
            {
                result = el_long(totym(se->type), 0);
                return;
            }

            Symbol *s = se->var->toSymbol();
            FuncDeclaration *fd = NULL;
            if (se->var->toParent2())
                fd = se->var->toParent2()->isFuncDeclaration();

            int nrvo = 0;
            if (fd && fd->nrvo_can && fd->nrvo_var == se->var)
            {
                s = fd->shidden;
                nrvo = 1;
            }

            if (s->Sclass == SCauto || s->Sclass == SCparameter || s->Sclass == SCshadowreg)
            {
                if (fd && fd != irs->getFunc())
                {
                    // 'var' is a variable in an enclosing function.
                    elem *ethis = getEthis(se->loc, irs, fd);
                    ethis = el_una(OPaddr, TYnptr, ethis);

                    int soffset;
                    if (v && v->offset)
                        soffset = v->offset;
                    else
                    {
                        soffset = s->Soffset;
                        /* If fd is a non-static member function of a class or struct,
                         * then ethis isn't the frame pointer.
                         * ethis is the 'this' pointer to the class/struct instance.
                         * We must offset it.
                         */
                        if (fd->vthis)
                        {
                            symbol *vs = fd->vthis->toSymbol();
                            //printf("vs = %s, offset = %x, %p\n", vs->Sident, (int)vs->Soffset, vs);
                            soffset -= vs->Soffset;
                        }
                        //printf("\tSoffset = x%x, sthis->Soffset = x%x\n", s->Soffset, irs->sthis->Soffset);
                    }

                    if (!nrvo)
                        soffset += offset;

                    e = el_bin(OPadd, TYnptr, ethis, el_long(TYnptr, soffset));
                    if (se->op == TOKvar)
                        e = el_una(OPind, TYnptr, e);
                    if (ISREF(se->var, tb) && !(ISWIN64REF(se->var) && v && v->offset))
                        e = el_una(OPind, s->ty(), e);
                    else if (se->op == TOKsymoff && nrvo)
                    {
                        e = el_una(OPind, TYnptr, e);
                        e = el_bin(OPadd, e->Ety, e, el_long(TYsize_t, offset));
                    }
                    goto L1;
                }
            }

            /* If var is a member of a closure
             */
            if (v && v->offset)
            {
                assert(irs->sclosure);
                e = el_var(irs->sclosure);
                e = el_bin(OPadd, TYnptr, e, el_long(TYsize_t, v->offset));
                if (se->op == TOKvar)
                {
                    e = el_una(OPind, totym(se->type), e);
                    if (tybasic(e->Ety) == TYstruct)
                        e->ET = Type_toCtype(se->type);
                    el_setLoc(e, se->loc);
                }
                if (ISREF(se->var, tb) && !ISWIN64REF(se->var))
                {
                    e->Ety = TYnptr;
                    e = el_una(OPind, s->ty(), e);
                }
                else if (se->op == TOKsymoff && nrvo)
                {
                    e = el_una(OPind, TYnptr, e);
                    e = el_bin(OPadd, e->Ety, e, el_long(TYsize_t, offset));
                }
                else if (se->op == TOKsymoff)
                {
                    e = el_bin(OPadd, e->Ety, e, el_long(TYsize_t, offset));
                }
                goto L1;
            }

            if (s->Sclass == SCauto && s->Ssymnum == -1)
            {
                //printf("\tadding symbol %s\n", s->Sident);
                symbol_add(s);
            }

            if (se->var->isImportedSymbol())
            {
                assert(se->op == TOKvar);
                e = el_var(se->var->toImport());
                e = el_una(OPind,s->ty(),e);
            }
            else if (ISREF(se->var, tb))
            {
                // Static arrays are really passed as pointers to the array
                // Out parameters are really references
                e = el_var(s);
                e->Ety = TYnptr;
                if (se->op == TOKvar)
                    e = el_una(OPind, s->ty(), e);
                else if (offset)
                    e = el_bin(OPadd, TYnptr, e, el_long(TYsize_t, offset));
            }
            else if (se->op == TOKvar)
                e = el_var(s);
            else
            {
                e = nrvo ? el_var(s) : el_ptr(s);
                e = el_bin(OPadd, e->Ety, e, el_long(TYsize_t, offset));
            }
        L1:
            if (se->op == TOKvar)
            {
                if (nrvo)
                {
                    e->Ety = TYnptr;
                    e = el_una(OPind, 0, e);
                }
                if (tb->ty == Tfunction)
                {
                    tym = s->Stype->Tty;
                }
                else
                    tym = totym(se->type);
                e->Ejty = e->Ety = tym;
                if (tybasic(tym) == TYstruct)
                {
                    e->ET = Type_toCtype(se->type);
                }
                else if (tybasic(tym) == TYarray)
                {
                    e->Ejty = e->Ety = TYstruct;
                    e->ET = Type_toCtype(se->type);
                }
                else if (tysimd(tym))
                {
                    e->ET = Type_toCtype(se->type);
                }
            }
            el_setLoc(e,se->loc);
            result = e;
        }

        /**************************************
         */

        void visit(FuncExp *fe)
        {
            //printf("FuncExp::toElem() %s\n", fe->toChars());
            if (fe->fd->tok == TOKreserved && fe->type->ty == Tpointer)
            {
                // change to non-nested
                fe->fd->tok = TOKfunction;
                fe->fd->vthis = NULL;
            }
            Symbol *s = fe->fd->toSymbol();
            elem *e = el_ptr(s);
            if (fe->fd->isNested())
            {
                elem *ethis = getEthis(fe->loc, irs, fe->fd);
                e = el_pair(TYdelegate, ethis, e);
            }

            irs->deferToObj->push(fe->fd);
            el_setLoc(e,fe->loc);
            result = e;
        }

        void visit(DeclarationExp *de)
        {
            //printf("DeclarationExp::toElem() %s\n", detoChars());
            result = Dsymbol_toElem(de->declaration);
        }

        /***************************************
         */

        void visit(ThisExp *te)
        {
            //printf("ThisExp::toElem()\n");
            assert(irs->sthis);

            elem *ethis;
            if (te->var)
            {
                assert(te->var->parent);
                FuncDeclaration *fd = te->var->toParent2()->isFuncDeclaration();
                assert(fd);
                ethis = getEthis(te->loc, irs, fd);
            }
            else
                ethis = el_var(irs->sthis);

            if (te->type->ty == Tstruct)
            {
                ethis = el_una(OPind, TYstruct, ethis);
                ethis->ET = Type_toCtype(te->type);
            }
            el_setLoc(ethis,te->loc);
            result = ethis;
        }

        /***************************************
         */

        void visit(IntegerExp *ie)
        {
            elem *e = el_long(totym(ie->type), ie->value);
            el_setLoc(e,ie->loc);
            result = e;
        }

        /***************************************
         */

        void visit(RealExp *re)
        {
            //printf("RealExp::toElem(%p) %s\n", re, re->toChars());
            union eve c;
            memset(&c, 0, sizeof(c));
            tym_t ty = totym(re->type->toBasetype());
            switch (tybasic(ty))
            {
                case TYfloat:
                case TYifloat:
                    /* This assignment involves a conversion, which
                     * unfortunately also converts SNAN to QNAN.
                     */
                    c.Vfloat = re->value;
                    if (Port::isSignallingNan(re->value))
                    {
                        // Put SNAN back
                        c.Vuns &= 0xFFBFFFFFL;
                    }
                    break;

                case TYdouble:
                case TYidouble:
                    /* This assignment involves a conversion, which
                     * unfortunately also converts SNAN to QNAN.
                     */
                    c.Vdouble = re->value;
                    if (Port::isSignallingNan(re->value))
                    {
                        // Put SNAN back
                        c.Vullong &= 0xFFF7FFFFFFFFFFFFULL;
                    }
                    break;

                case TYldouble:
                case TYildouble:
                    c.Vldouble = re->value;
                    break;

                default:
                    re->print();
                    re->type->print();
                    re->type->toBasetype()->print();
                    printf("ty = %d, tym = %x\n", re->type->ty, ty);
                    assert(0);
            }
            result = el_const(ty, &c);
        }

        /***************************************
         */

        void visit(ComplexExp *ce)
        {

            //printf("ComplexExp::toElem(%p) %s\n", ce, ce->toChars());

            union eve c;
            memset(&c, 0, sizeof(c));
            real_t re = creall(ce->value);
            real_t im = cimagl(ce->value);

            tym_t ty = totym(ce->type);
            switch (tybasic(ty))
            {
                case TYcfloat:
                    c.Vcfloat.re = (float) re;
                    if (Port::isSignallingNan(re))
                    {
                        union { float f; unsigned i; } u;
                        u.f = c.Vcfloat.re;
                        u.i &= 0xFFBFFFFFL;
                        c.Vcfloat.re = u.f;
                    }
                    c.Vcfloat.im = (float) im;
                    if (Port::isSignallingNan(im))
                    {
                        union { float f; unsigned i; } u;
                        u.f = c.Vcfloat.im;
                        u.i &= 0xFFBFFFFFL;
                        c.Vcfloat.im = u.f;
                    }
                    break;

                case TYcdouble:
                    c.Vcdouble.re = (double) re;
                    if (Port::isSignallingNan(re))
                    {
                        union { double d; unsigned long long i; } u;
                        u.d = c.Vcdouble.re;
                        u.i &= 0xFFF7FFFFFFFFFFFFULL;
                        c.Vcdouble.re = u.d;
                    }
                    c.Vcdouble.im = (double) im;
                    if (Port::isSignallingNan(re))
                    {
                        union { double d; unsigned long long i; } u;
                        u.d = c.Vcdouble.im;
                        u.i &= 0xFFF7FFFFFFFFFFFFULL;
                        c.Vcdouble.im = u.d;
                    }
                    break;

                case TYcldouble:
                    c.Vcldouble.re = re;
                    c.Vcldouble.im = im;
                    break;

                default:
                    assert(0);
            }
            result = el_const(ty, &c);
        }

        /***************************************
         */

        void visit(NullExp *ne)
        {
            result = el_long(totym(ne->type), 0);
        }

        /***************************************
         */

        void visit(StringExp *se)
        {
        #if 0
            printf("StringExp::toElem() %s, type = %s\n", se->toChars(), se->type->toChars());
        #endif

            elem *e;
            Type *tb = se->type->toBasetype();
            if (tb->ty == Tarray)
            {
                Symbol *si;
                dt_t *dt;
                StringTab *st;

        #if 0
                printf("irs->m = %p\n", irs->m);
                printf(" m   = %s\n", irs->m->toChars());
                printf(" len = %d\n", se->len);
                printf(" sz  = %d\n", se->sz);
        #endif
                for (size_t i = 0; i < STSIZE; i++)
                {
                    st = &stringTab[(stidx + i) % STSIZE];
                    //if (!st->m) continue;
                    //printf(" st.m   = %s\n", st->m->toChars());
                    //printf(" st.len = %d\n", st->len);
                    //printf(" st.sz  = %d\n", st->sz);
                    if (st->m == irs->m &&
                        st->si &&
                        st->len == se->len &&
                        st->sz == se->sz &&
                        memcmp(st->string, se->string, se->sz * se->len) == 0)
                    {
                        //printf("use cached value\n");
                        si = st->si;    // use cached value
                        goto L1;
                    }
                }

                stidx = (stidx + 1) % STSIZE;
                st = &stringTab[stidx];

                dt = NULL;
                se->toDt(&dt);

                si = symbol_generate(SCstatic,type_fake(TYdarray));
                si->Sdt = dt;
                si->Sfl = FLdata;
                out_readonly(si);
                outdata(si);

                st->m = irs->m;
                st->si = si;
                st->string = se->string;
                st->len = se->len;
                st->sz = se->sz;
            L1:
                e = el_var(si);
            }
            else if (tb->ty == Tsarray)
            {
                dt_t *dt = NULL;

                se->toDt(&dt);
                dtnzeros(&dt, se->sz);              // leave terminating 0

                ::type *t = type_static_array(se->sz * se->len, tschar);
                Symbol *si = symbol_generate(SCstatic, t);
                si->Sdt = dt;
                si->Sfl = FLdata;
                out_readonly(si);
                outdata(si);

                e = el_var(si);

                e->Ejty = e->Ety = TYstruct;
                e->ET = t;
                t->Tcount++;
            }
            else if (tb->ty == Tpointer)
            {
                e = el_calloc();
                e->Eoper = OPstring;
                // freed in el_free
                e->EV.ss.Vstring = (char *)mem_malloc((se->len + 1) * se->sz);
                memcpy(e->EV.ss.Vstring, se->string, (se->len + 1) * se->sz);
                e->EV.ss.Vstrlen = (se->len + 1) * se->sz;
                e->Ety = TYnptr;
            }
            else
            {
                printf("type is %s\n", se->type->toChars());
                assert(0);
            }
            el_setLoc(e,se->loc);
            result = e;
        }

        void visit(NewExp *ne)
        {
            //printf("NewExp::toElem() %s\n", ne->toChars());
            Type *t = ne->type->toBasetype();
            //printf("\ttype = %s\n", t->toChars());
            //if (ne->member)
                //printf("\tmember = %s\n", ne->member->toChars());
            elem *e;
            Type *ectype;
            if (t->ty == Tclass)
            {
                t = ne->newtype->toBasetype();
                assert(t->ty == Tclass);
                TypeClass *tclass = (TypeClass *)t;
                ClassDeclaration *cd = tclass->sym;

                /* Things to do:
                 * 1) ex: call allocator
                 * 2) ey: set vthis for nested classes
                 * 3) ez: call constructor
                 */

                elem *ex = NULL;
                elem *ey = NULL;
                elem *ez = NULL;

                if (ne->allocator || ne->onstack)
                {
                    if (ne->onstack)
                    {
                        /* Create an instance of the class on the stack,
                         * and call it stmp.
                         * Set ex to be the &stmp.
                         */
                        ::type *tc = type_struct_class(tclass->sym->toChars(),
                                tclass->sym->alignsize, tclass->sym->structsize,
                                NULL, NULL,
                                false, false, true);
                        tc->Tcount--;
                        Symbol *stmp = symbol_genauto(tc);
                        ex = el_ptr(stmp);
                    }
                    else
                    {
                        ex = el_var(ne->allocator->toSymbol());
                        ex = callfunc(ne->loc, irs, 1, ne->type, ex, ne->allocator->type,
                                ne->allocator, ne->allocator->type, NULL, ne->newargs);
                    }

                    Symbol *si = tclass->sym->toInitializer();
                    elem *ei = el_var(si);

                    if (cd->isNested())
                    {
                        ey = el_same(&ex);
                        ez = el_copytree(ey);
                    }
                    else if (ne->member)
                        ez = el_same(&ex);

                    ex = el_una(OPind, TYstruct, ex);
                    ex = el_bin(OPstreq, TYnptr, ex, ei);
                    ex->ET = Type_toCtype(tclass)->Tnext;
                    ex = el_una(OPaddr, TYnptr, ex);
                    ectype = tclass;
                }
                else
                {
                    Symbol *csym = cd->toSymbol();
                    ex = el_bin(OPcall,TYnptr,el_var(rtlsym[RTLSYM_NEWCLASS]),el_ptr(csym));
                    ectype = NULL;

                    if (cd->isNested())
                    {
                        ey = el_same(&ex);
                        ez = el_copytree(ey);
                    }
                    else if (ne->member)
                        ez = el_same(&ex);
                    //elem_print(ex);
                    //elem_print(ey);
                    //elem_print(ez);
                }

                if (ne->thisexp)
                {
                    ClassDeclaration *cdthis = ne->thisexp->type->isClassHandle();
                    assert(cdthis);
                    //printf("cd = %s\n", cd->toChars());
                    //printf("cdthis = %s\n", cdthis->toChars());
                    assert(cd->isNested());
                    int offset = 0;
                    Dsymbol *cdp = cd->toParent2();     // class we're nested in

                    //printf("member = %p\n", member);
                    //printf("cdp = %s\n", cdp->toChars());
                    //printf("cdthis = %s\n", cdthis->toChars());
                    if (cdp != cdthis)
                    {
                        int i = cdp->isClassDeclaration()->isBaseOf(cdthis, &offset);
                        assert(i);
                    }
                    elem *ethis = ne->thisexp->toElem(irs);
                    if (offset)
                        ethis = el_bin(OPadd, TYnptr, ethis, el_long(TYsize_t, offset));

                    if (!cd->vthis)
                    {
                        ne->error("forward reference to %s", cd->toChars());
                    }
                    else
                    {
                        ey = el_bin(OPadd, TYnptr, ey, el_long(TYsize_t, cd->vthis->offset));
                        ey = el_una(OPind, TYnptr, ey);
                        ey = el_bin(OPeq, TYnptr, ey, ethis);
                    }
                    //printf("ex: "); elem_print(ex);
                    //printf("ey: "); elem_print(ey);
                    //printf("ez: "); elem_print(ez);
                }
                else if (cd->isNested())
                {
                    /* Initialize cd->vthis:
                     *  *(ey + cd.vthis.offset) = this;
                     */
                    ey = setEthis(ne->loc, irs, ey, cd);
                }

                if (ne->member)
                {
                    // Call constructor
                    ez = callfunc(ne->loc, irs, 1, ne->type, ez, ectype, ne->member, ne->member->type, NULL, ne->arguments);
                }

                e = el_combine(ex, ey);
                e = el_combine(e, ez);
            }
            else if (t->ty == Tpointer && t->nextOf()->toBasetype()->ty == Tstruct)
            {
                t = ne->newtype->toBasetype();
                assert(t->ty == Tstruct);
                TypeStruct *tclass = (TypeStruct *)t;
                StructDeclaration *cd = tclass->sym;

                /* Things to do:
                 * 1) ex: call allocator
                 * 2) ey: set vthis for nested classes
                 * 3) ez: call constructor
                 */

                elem *ex = NULL;
                elem *ey = NULL;
                elem *ez = NULL;

                if (ne->allocator)
                {

                    ex = el_var(ne->allocator->toSymbol());
                    ex = callfunc(ne->loc, irs, 1, ne->type, ex, ne->allocator->type,
                                ne->allocator, ne->allocator->type, NULL, ne->newargs);

                    Symbol *si = tclass->sym->toInitializer();
                    elem *ei = el_var(si);

                    if (cd->isNested())
                    {
                        ey = el_same(&ex);
                        ez = el_copytree(ey);
                    }
                    else if (ne->member)
                        ez = el_same(&ex);

                    if (!ne->member)
                    {
                        /* Statically intialize with default initializer
                         */
                        ex = el_una(OPind, TYstruct, ex);
                        ex = el_bin(OPstreq, TYnptr, ex, ei);
                        ex->ET = Type_toCtype(tclass);
                        ex = el_una(OPaddr, TYnptr, ex);
                    }
                    ectype = tclass;
                }
                else
                {
                    d_uns64 elemsize = cd->size(ne->loc);

                    // call _d_newitemT(ti)
                    e = ne->type->getTypeInfo(NULL)->toElem(irs);

                    int rtl = t->isZeroInit() ? RTLSYM_NEWITEMT : RTLSYM_NEWITEMIT;
                    ex = el_bin(OPcall,TYnptr,el_var(rtlsym[rtl]),e);

                    ectype = NULL;

                    if (cd->isNested())
                    {
                        ey = el_same(&ex);
                        ez = el_copytree(ey);
                    }
                    else if (ne->member)
                        ez = el_same(&ex);
                    //elem_print(ex);
                    //elem_print(ey);
                    //elem_print(ez);
                }

                if (cd->isNested())
                {
                    /* Initialize cd->vthis:
                     *  *(ey + cd.vthis.offset) = this;
                     */
                    ey = setEthis(ne->loc, irs, ey, cd);
                }

                if (ne->member)
                {
                    // Call constructor
                    ez = callfunc(ne->loc, irs, 1, ne->type, ez, ectype, ne->member, ne->member->type, NULL, ne->arguments);
                    /* Structs return a ref, which gets automatically dereferenced.
                     * But we want a pointer to the instance.
                     */
                    ez = el_una(OPaddr, TYnptr, ez);
                }

                e = el_combine(ex, ey);
                e = el_combine(e, ez);
            }
            else if (t->ty == Tarray)
            {
                TypeDArray *tda = (TypeDArray *)t;

                assert(ne->arguments && ne->arguments->dim >= 1);
                if (ne->arguments->dim == 1)
                {
                    // Single dimension array allocations
                    Expression *arg = (*ne->arguments)[0]; // gives array length
                    e = arg->toElem(irs);
                    d_uns64 elemsize = tda->next->size();

                    // call _d_newT(ti, arg)
                    e = el_param(e, ne->type->getTypeInfo(NULL)->toElem(irs));
                    int rtl = tda->next->isZeroInit() ? RTLSYM_NEWARRAYT : RTLSYM_NEWARRAYIT;
                    e = el_bin(OPcall,TYdarray,el_var(rtlsym[rtl]),e);
                }
                else
                {
                    // Multidimensional array allocations
                    e = el_long(TYsize_t, ne->arguments->dim);
                    for (size_t i = 0; i < ne->arguments->dim; i++)
                    {
                        Expression *arg = (*ne->arguments)[i];     // gives array length
                        e = el_param(arg->toElem(irs), e);
                        assert(t->ty == Tarray);
                        t = t->nextOf();
                        assert(t);
                    }

                    e = el_param(e, ne->type->getTypeInfo(NULL)->toElem(irs));

                    int rtl = t->isZeroInit() ? RTLSYM_NEWARRAYMT : RTLSYM_NEWARRAYMIT;
                    e = el_bin(OPcall,TYdarray,el_var(rtlsym[rtl]),e);
                    e->Eflags |= EFLAGS_variadic;
                }
            }
            else if (t->ty == Tpointer)
            {
                TypePointer *tp = (TypePointer *)t;
                d_uns64 elemsize = tp->next->size();
                Expression *di = tp->next->defaultInit();
                d_uns64 disize = di->type->size();

                // call _d_newitemT(ti)
                e = ne->type->getTypeInfo(NULL)->toElem(irs);

                int rtl = tp->next->isZeroInit() ? RTLSYM_NEWITEMT : RTLSYM_NEWITEMIT;
                e = el_bin(OPcall,TYnptr,el_var(rtlsym[rtl]),e);
            }
            else
            {
                ne->error("ICE: cannot new type %s\n", t->toChars());
                assert(0);
            }

            el_setLoc(e,ne->loc);
            result = e;
        }

        //////////////////////////// Unary ///////////////////////////////

        /***************************************
         */

        void visit(NegExp *ne)
        {
            elem *e = ne->e1->toElem(irs);
            Type *tb1 = ne->e1->type->toBasetype();

            assert(tb1->ty != Tarray && tb1->ty != Tsarray);

            switch (tb1->ty)
            {
                case Tvector:
                {
                    // rewrite (-e) as (0-e)
                    elem *ez = el_calloc();
                    ez->Eoper = OPconst;
                    ez->Ety = e->Ety;
                    ez->EV.Vcent.lsw = 0;
                    ez->EV.Vcent.msw = 0;
                    e = el_bin(OPmin, totym(ne->type), ez, e);
                    break;
                }

                default:
                    e = el_una(OPneg, totym(ne->type), e);
                    break;
            }

            el_setLoc(e,ne->loc);
            result = e;
        }

        /***************************************
         */

        void visit(ComExp *ce)
        {
            elem *e1 = ce->e1->toElem(irs);
            Type *tb1 = ce->e1->type->toBasetype();
            tym_t ty = totym(ce->type);

            assert(tb1->ty != Tarray && tb1->ty != Tsarray);

            elem *e;
            switch (tb1->ty)
            {
                case Tbool:
                    e = el_bin(OPxor, ty, e1, el_long(ty, 1));
                    break;

                case Tvector:
                {
                    // rewrite (~e) as (e^~0)
                    elem *ec = el_calloc();
                    ec->Eoper = OPconst;
                    ec->Ety = e1->Ety;
                    ec->EV.Vcent.lsw = ~0LL;
                    ec->EV.Vcent.msw = ~0LL;
                    e = el_bin(OPxor, ty, e1, ec);
                    break;
                }

                default:
                    e = el_una(OPcom,ty,e1);
                    break;
            }

            el_setLoc(e,ce->loc);
            result = e;
        }

        /***************************************
         */

        void visit(NotExp *ne)
        {
            elem *e = el_una(OPnot, totym(ne->type), ne->e1->toElem(irs));
            el_setLoc(e,ne->loc);
            result = e;
        }


        /***************************************
         */

        void visit(HaltExp *he)
        {
            elem *e = el_calloc();
            e->Ety = TYvoid;
            e->Eoper = OPhalt;
            el_setLoc(e,he->loc);
            result = e;
        }

        /********************************************
         */

        void visit(AssertExp *ae)
        {

            //printf("AssertExp::toElem() %s\n", toChars());
            elem *e;
            if (global.params.useAssert)
            {
                e = ae->e1->toElem(irs);
                symbol *ts = NULL;
                elem *einv = NULL;
                Type *t1 = ae->e1->type->toBasetype();

                FuncDeclaration *inv;

                // If e1 is a class object, call the class invariant on it
                if (global.params.useInvariants && t1->ty == Tclass &&
                    !((TypeClass *)t1)->sym->isInterfaceDeclaration() &&
                    !((TypeClass *)t1)->sym->isCPPclass())
                {
                    ts = symbol_genauto(Type_toCtype(t1));
                    int rtl;
                    if (global.params.isLinux || global.params.isFreeBSD || global.params.isSolaris ||
                        I64 && global.params.isWindows)
                        rtl = RTLSYM__DINVARIANT;
                    else
                        rtl = RTLSYM_DINVARIANT;
                    einv = el_bin(OPcall, TYvoid, el_var(rtlsym[rtl]), el_var(ts));
                }
                else if (global.params.useInvariants &&
                    t1->ty == Tpointer &&
                    t1->nextOf()->ty == Tstruct &&
                    (inv = ((TypeStruct *)t1->nextOf())->sym->inv) != NULL)
                {
                    // If e1 is a struct object, call the struct invariant on it
                    ts = symbol_genauto(Type_toCtype(t1));
                    einv = callfunc(ae->loc, irs, 1, inv->type->nextOf(), el_var(ts), ae->e1->type, inv, inv->type, NULL, NULL);
                }

                // Construct: (e1 || ModuleAssert(line))
                Module *m = irs->blx->module;
                char *mname = m->srcfile->toChars();

                //printf("filename = '%s'\n", ae->loc.filename);
                //printf("module = '%s'\n", m->srcfile->toChars());

                /* Determine if we are in a unittest
                 */
                FuncDeclaration *fd = irs->getFunc();
                UnitTestDeclaration *ud = fd ? fd->isUnitTestDeclaration() : NULL;

                /* If the source file name has changed, probably due
                 * to a #line directive.
                 */
                elem *ea;
                if (ae->loc.filename && (ae->msg || strcmp(ae->loc.filename, mname) != 0))
                {
                    /* Cache values.
                     */
                    //static Symbol *assertexp_sfilename = NULL;
                    //static char *assertexp_name = NULL;
                    //static Module *assertexp_mn = NULL;

                    if (!assertexp_sfilename || strcmp(ae->loc.filename, assertexp_name) != 0 || assertexp_mn != m)
                    {

                        const char *id = ae->loc.filename;
                        int len = strlen(id);
                        dt_t *dt = NULL;
                        dtsize_t(&dt, len);
                        dtabytes(&dt,TYnptr, 0, len + 1, id);

                        assertexp_sfilename = symbol_generate(SCstatic,type_fake(TYdarray));
                        assertexp_sfilename->Sdt = dt;
                        assertexp_sfilename->Sfl = FLdata;
                        out_readonly(assertexp_sfilename);
                        outdata(assertexp_sfilename);

                        assertexp_mn = m;
                        assertexp_name = id;
                    }

                    elem *efilename = (config.exe == EX_WIN64) ? el_ptr(assertexp_sfilename)
                                                               : el_var(assertexp_sfilename);

                    if (ae->msg)
                    {
                        /* Bugzilla 8360: If the condition is evalated to true,
                         * msg is not evaluated at all. so should use
                         * toElemDtor(msg, irs) instead of msg->toElem(irs).
                         */
                        elem *emsg = toElemDtor(ae->msg, irs);
                        emsg = array_toDarray(ae->msg->type, emsg);
                        if (config.exe == EX_WIN64)
                            emsg = addressElem(emsg, Type::tvoid->arrayOf(), false);

                        ea = el_var(rtlsym[ud ? RTLSYM_DUNITTEST_MSG : RTLSYM_DASSERT_MSG]);
                        ea = el_bin(OPcall, TYvoid, ea, el_params(el_long(TYint, ae->loc.linnum), efilename, emsg, NULL));
                    }
                    else
                    {
                        ea = el_var(rtlsym[ud ? RTLSYM_DUNITTEST : RTLSYM_DASSERT]);
                        ea = el_bin(OPcall, TYvoid, ea, el_param(el_long(TYint, ae->loc.linnum), efilename));
                    }
                }
                else
                {
                    Symbol *sassert = ud ? m->toModuleUnittest() : m->toModuleAssert();
                    ea = el_bin(OPcall,TYvoid,el_var(sassert),
                        el_long(TYint, ae->loc.linnum));
                }
                if (einv)
                {
                    // tmp = e, e || assert, e->inv
                    elem *eassign = el_bin(OPeq, e->Ety, el_var(ts), e);
                    e = el_combine(eassign, el_bin(OPoror, TYvoid, el_var(ts), ea));
                    e = el_combine(e, einv);
                }
                else
                    e = el_bin(OPoror,TYvoid,e,ea);
            }
            else
            {
                // BUG: should replace assert(0); with a HLT instruction
                e = el_long(TYint, 0);
            }
            el_setLoc(e,ae->loc);
            result = e;
        }

        void visit(PostExp *pe)
        {
            //printf("PostExp::toElem() '%s'\n", pe->toChars());
            elem *e = pe->e1->toElem(irs);
            elem *einc = pe->e2->toElem(irs);
            e = el_bin((pe->op == TOKplusplus) ? OPpostinc : OPpostdec,
                        e->Ety,e,einc);
            el_setLoc(e,pe->loc);
            result = e;
        }

        //////////////////////////// Binary ///////////////////////////////

        /********************************************
         */

        elem *toElemBin(BinExp *be, int op)
        {
            //printf("toElemBin() '%s'\n", be->toChars());

            Type *tb1 = be->e1->type->toBasetype();
            Type *tb2 = be->e2->type->toBasetype();

            assert(!((tb1->ty == Tarray || tb1->ty == Tsarray ||
                      tb2->ty == Tarray || tb2->ty == Tsarray) &&
                     tb2->ty != Tvoid &&
                     op != OPeq && op != OPandand && op != OPoror));

            tym_t tym = totym(be->type);

            elem *el = be->e1->toElem(irs);
            elem *er = be->e2->toElem(irs);
            elem *e = el_bin(op,tym,el,er);

            el_setLoc(e,be->loc);
            return e;
        }

        /***************************************
         */

        void visit(AddExp *e)
        {
            result = toElemBin(e, OPadd);
        }

        /***************************************
         */

        void visit(MinExp *e)
        {
            result = toElemBin(e, OPmin);
        }

        /*****************************************
         * Evaluate elem and convert to dynamic array suitable for a function argument.
         */

        elem *eval_Darray(Expression *e)
        {
            elem *ex = e->toElem(irs);
            ex = array_toDarray(e->type, ex);
            if (config.exe == EX_WIN64)
            {
                ex = addressElem(ex, Type::tvoid->arrayOf(), false);
            }
            return ex;
        }

        /***************************************
         */

        void visit(CatExp *ce)
        {
        #if 0
            printf("CatExp::toElem()\n");
            ce->print();
        #endif

            Type *tb1 = ce->e1->type->toBasetype();
            Type *tb2 = ce->e2->type->toBasetype();

            Type *ta = (tb1->ty == Tarray || tb1->ty == Tsarray) ? tb1 : tb2;
            Type *tn = ta->nextOf();

            elem *e;
            if (ce->e1->op == TOKcat)
            {
                CatExp *ex = ce;
                int n = 2;

                elem *ep = eval_Darray(ex->e2);
                do
                {
                    n++;
                    ex = (CatExp *)ex->e1;
                    ep = el_param(ep, eval_Darray(ex->e2));
                } while (ex->e1->op == TOKcat);
                ep = el_param(ep, eval_Darray(ex->e1));
                ep = el_params(ep,
                               el_long(TYsize_t, n),
                               ta->getTypeInfo(NULL)->toElem(irs),
                               NULL);
                e = el_bin(OPcall, TYdarray, el_var(rtlsym[RTLSYM_ARRAYCATNT]), ep);
                e->Eflags |= EFLAGS_variadic;
            }
            else
            {
                elem *e1 = eval_Darray(ce->e1);
                elem *e2 = eval_Darray(ce->e2);
                elem *ep = el_params(e2, e1, ta->getTypeInfo(NULL)->toElem(irs), NULL);
                e = el_bin(OPcall, TYdarray, el_var(rtlsym[RTLSYM_ARRAYCATT]), ep);
            }
            el_setLoc(e,ce->loc);
            result = e;
        }

        /***************************************
         */

        void visit(MulExp *e)
        {
            result = toElemBin(e, OPmul);
        }

        /************************************
         */

        void visit(DivExp *e)
        {
            result = toElemBin(e, OPdiv);
        }

        /***************************************
         */

        void visit(ModExp *e)
        {
            result = toElemBin(e, OPmod);
        }

        /***************************************
         */

        void visit(CmpExp *ce)
        {
            OPER eop;
            Type *t1 = ce->e1->type->toBasetype();
            Type *t2 = ce->e2->type->toBasetype();

            switch (ce->op)
            {
                case TOKlt:     eop = OPlt;     break;
                case TOKgt:     eop = OPgt;     break;
                case TOKle:     eop = OPle;     break;
                case TOKge:     eop = OPge;     break;
                case TOKequal:  eop = OPeqeq;   break;
                case TOKnotequal: eop = OPne;   break;

                // NCEG floating point compares
                case TOKunord:  eop = OPunord;  break;
                case TOKlg:     eop = OPlg;     break;
                case TOKleg:    eop = OPleg;    break;
                case TOKule:    eop = OPule;    break;
                case TOKul:     eop = OPul;     break;
                case TOKuge:    eop = OPuge;    break;
                case TOKug:     eop = OPug;     break;
                case TOKue:     eop = OPue;     break;
                default:
                    ce->print();
                    assert(0);
            }
            if (!t1->isfloating())
            {
                // Convert from floating point compare to equivalent
                // integral compare
                eop = (OPER)rel_integral(eop);
            }
            elem *e;
            if ((int)eop > 1 && t1->ty == Tclass && t2->ty == Tclass)
            {
                // Should have already been lowered
                assert(0);
            }
            else if ((int)eop > 1 &&
                     (t1->ty == Tarray || t1->ty == Tsarray) &&
                     (t2->ty == Tarray || t2->ty == Tsarray))
            {
                Type *telement = t1->nextOf()->toBasetype();

                elem *ea1 = eval_Darray(ce->e1);
                elem *ea2 = eval_Darray(ce->e2);

                elem *ep = el_params(telement->arrayOf()->getInternalTypeInfo(NULL)->toElem(irs),
                        ea2, ea1, NULL);
                int rtlfunc = RTLSYM_ARRAYCMP2;
                e = el_bin(OPcall, TYint, el_var(rtlsym[rtlfunc]), ep);
                e = el_bin(eop, TYint, e, el_long(TYint, 0));
                el_setLoc(e,ce->loc);
            }
            else
            {
                if ((int)eop <= 1)
                {
                    /* The result is determinate, create:
                     *   (e1 , e2) , eop
                     */
                    e = toElemBin(ce,OPcomma);
                    e = el_bin(OPcomma,e->Ety,e,el_long(e->Ety,(int)eop));
                }
                else
                    e = toElemBin(ce,eop);
            }
            result = e;
        }

        void visit(EqualExp *ee)
        {
            //printf("EqualExp::toElem() %s\n", ee->toChars());

            Type *t1 = ee->e1->type->toBasetype();
            Type *t2 = ee->e2->type->toBasetype();

            OPER eop;
            switch (ee->op)
            {
                case TOKequal:          eop = OPeqeq;   break;
                case TOKnotequal:       eop = OPne;     break;
                default:
                    ee->print();
                    assert(0);
            }

            //printf("EqualExp::toElem()\n");
            elem *e;
            if (t1->ty == Tstruct)
            {
                // Do bit compare of struct's
                elem *es1 = ee->e1->toElem(irs);
                elem *es2 = ee->e2->toElem(irs);
                es1 = addressElem(es1, t1);
                es2 = addressElem(es2, t2);
                e = el_param(es1, es2);
                elem *ecount = el_long(TYsize_t, t1->size());
                e = el_bin(OPmemcmp, TYint, e, ecount);
                e = el_bin(eop, TYint, e, el_long(TYint, 0));
                el_setLoc(e, ee->loc);
            }
            else if ((t1->ty == Tarray || t1->ty == Tsarray) &&
                     (t2->ty == Tarray || t2->ty == Tsarray))
            {
                Type *telement  = t1->nextOf()->toBasetype();
                Type *telement2 = t2->nextOf()->toBasetype();

                if ((telement->isintegral() || telement->ty == Tvoid) && telement->ty == telement2->ty)
                {
                    // Optimize comparisons of arrays of basic types
                    // For arrays of integers/characters, and void[],
                    // replace druntime call with:
                    // For a==b: a.length==b.length && memcmp(a.ptr, b.ptr, size)==0
                    // For a!=b: a.length!=b.length || memcmp(a.ptr, b.ptr, size)!=0
                    // size is a.length*sizeof(a[0]) for dynamic arrays, or sizeof(a) for static arrays.

                    elem *earr1 = ee->e1->toElem(irs);
                    elem *earr2 = ee->e2->toElem(irs);
                    elem *eptr1, *eptr2; // Pointer to data, to pass to memcmp
                    elem *elen1, *elen2; // Length, for comparison
                    elem *esiz1, *esiz2; // Data size, to pass to memcmp
                    d_uns64 sz = telement->size(); // Size of one element

                    if (t1->ty == Tarray)
                    {
                        elen1 = el_una(I64 ? OP128_64 : OP64_32, TYsize_t, el_same(&earr1));
                        esiz1 = el_bin(OPmul, TYsize_t, el_same(&elen1), el_long(TYsize_t, sz));
                        eptr1 = array_toPtr(t1, el_same(&earr1));
                    }
                    else
                    {
                        elen1 = el_long(TYsize_t, ((TypeSArray *)t1)->dim->toInteger());
                        esiz1 = el_long(TYsize_t, t1->size());
                        earr1 = addressElem(earr1, t1);
                        eptr1 = el_same(&earr1);
                    }

                    if (t2->ty == Tarray)
                    {
                        elen2 = el_una(I64 ? OP128_64 : OP64_32, TYsize_t, el_same(&earr2));
                        esiz2 = el_bin(OPmul, TYsize_t, el_same(&elen2), el_long(TYsize_t, sz));
                        eptr2 = array_toPtr(t2, el_same(&earr2));
                    }
                    else
                    {
                        elen2 = el_long(TYsize_t, ((TypeSArray *)t2)->dim->toInteger());
                        esiz2 = el_long(TYsize_t, t2->size());
                        earr2 = addressElem(earr2, t2);
                        eptr2 = el_same(&earr2);
                    }

                    elem *esize = t2->ty == Tsarray ? esiz2 : esiz1;

                    e = el_param(eptr1, eptr2);
                    e = el_bin(OPmemcmp, TYint, e, esize);
                    e = el_bin(eop, TYint, e, el_long(TYint, 0));

                    if (t1->ty == Tsarray && t2->ty == Tsarray)
                        assert(t1->size() == t2->size());
                    else
                    {
                        elem *elencmp = el_bin(eop, TYint, elen1, elen2);
                        e = el_bin(ee->op == TOKequal ? OPandand : OPoror, TYint, elencmp, e);
                    }

                    // Ensure left-to-right order of evaluation
                    e = el_combine(earr2, e);
                    e = el_combine(earr1, e);
                    el_setLoc(e, ee->loc);
                    result = e;
                    return;
                }

                elem *ea1 = eval_Darray(ee->e1);
                elem *ea2 = eval_Darray(ee->e2);

                elem *ep = el_params(telement->arrayOf()->getInternalTypeInfo(NULL)->toElem(irs),
                        ea2, ea1, NULL);
                int rtlfunc = RTLSYM_ARRAYEQ2;
                e = el_bin(OPcall, TYint, el_var(rtlsym[rtlfunc]), ep);
                if (ee->op == TOKnotequal)
                    e = el_bin(OPxor, TYint, e, el_long(TYint, 1));
                el_setLoc(e,ee->loc);
            }
            else if (t1->ty == Taarray && t2->ty == Taarray)
            {
                TypeAArray *taa = (TypeAArray *)t1;
                Symbol *s = taa->aaGetSymbol("Equal", 0);
                elem *ti = taa->getTypeInfo(NULL)->toElem(irs);
                elem *ea1 = ee->e1->toElem(irs);
                elem *ea2 = ee->e2->toElem(irs);
                // aaEqual(ti, e1, e2)
                elem *ep = el_params(ea2, ea1, ti, NULL);
                e = el_bin(OPcall, TYnptr, el_var(s), ep);
                if (ee->op == TOKnotequal)
                    e = el_bin(OPxor, TYint, e, el_long(TYint, 1));
                el_setLoc(e, ee->loc);
                result = e;
                return;
            }
            else
                e = toElemBin(ee, eop);
            result = e;
        }

        void visit(IdentityExp *ie)
        {
            Type *t1 = ie->e1->type->toBasetype();
            Type *t2 = ie->e2->type->toBasetype();

            OPER eop;
            switch (ie->op)
            {
                case TOKidentity:       eop = OPeqeq;   break;
                case TOKnotidentity:    eop = OPne;     break;
                default:
                    ie->print();
                    assert(0);
            }

            //printf("IdentityExp::toElem() %s\n", toChars());

            elem *e;
            if (t1->ty == Tstruct || t1->isfloating())
            {
                // Do bit compare of struct's
                elem *es1 = ie->e1->toElem(irs);
                es1 = addressElem(es1, ie->e1->type);
                elem *es2 = ie->e2->toElem(irs);
                es2 = addressElem(es2, ie->e2->type);
                e = el_param(es1, es2);
                elem *ecount = el_long(TYsize_t, t1->size());
                e = el_bin(OPmemcmp, TYint, e, ecount);
                e = el_bin(eop, TYint, e, el_long(TYint, 0));
                el_setLoc(e, ie->loc);
            }
            else if ((t1->ty == Tarray || t1->ty == Tsarray) &&
                     (t2->ty == Tarray || t2->ty == Tsarray))
            {

                elem *ea1 = ie->e1->toElem(irs);
                ea1 = array_toDarray(t1, ea1);
                elem *ea2 = ie->e2->toElem(irs);
                ea2 = array_toDarray(t2, ea2);

                e = el_bin(eop, totym(ie->type), ea1, ea2);
                el_setLoc(e, ie->loc);
            }
            else
                e = toElemBin(ie, eop);

            result = e;
        }

        /***************************************
         */

        void visit(InExp *ie)
        {
            elem *key = ie->e1->toElem(irs);
            elem *aa = ie->e2->toElem(irs);
            TypeAArray *taa = (TypeAArray *)ie->e2->type->toBasetype();

            // aaInX(aa, keyti, key);
            key = addressElem(key, ie->e1->type);
            Symbol *s = taa->aaGetSymbol("InX", 0);
            elem *keyti = taa->index->getInternalTypeInfo(NULL)->toElem(irs);
            elem *ep = el_params(key, keyti, aa, NULL);
            elem *e = el_bin(OPcall, totym(ie->type), el_var(s), ep);

            el_setLoc(e, ie->loc);
            result = e;
        }

        /***************************************
         */

        void visit(RemoveExp *re)
        {
            Type *tb = re->e1->type->toBasetype();
            assert(tb->ty == Taarray);
            TypeAArray *taa = (TypeAArray *)tb;
            elem *ea = re->e1->toElem(irs);
            elem *ekey = re->e2->toElem(irs);

            ekey = addressElem(ekey, re->e1->type);
            Symbol *s = taa->aaGetSymbol("DelX", 0);
            elem *keyti = taa->index->getInternalTypeInfo(NULL)->toElem(irs);
            elem *ep = el_params(ekey, keyti, ea, NULL);
            elem *e = el_bin(OPcall, TYnptr, el_var(s), ep);

            el_setLoc(e, re->loc);
            result = e;
        }

        /***************************************
         */

        void visit(AssignExp *ae)
        {
            //printf("AssignExp::toElem('%s')\n", ae->toChars());
            Type *t1b = ae->e1->type->toBasetype();

            elem *e;

            // Look for array.length = n
            if (ae->e1->op == TOKarraylength)
            {
                // Generate:
                //      _d_arraysetlength(e2, sizeelem, &ale->e1);

                ArrayLengthExp *ale = (ArrayLengthExp *)ae->e1;

                elem *p1 = ae->e2->toElem(irs);
                elem *p3 = ale->e1->toElem(irs);
                p3 = addressElem(p3, NULL);
                Type *t1 = ale->e1->type->toBasetype();

                // call _d_arraysetlengthT(ti, e2, &ale->e1);
                elem *p2 = t1->getTypeInfo(NULL)->toElem(irs);
                elem *ep = el_params(p3, p1, p2, NULL); // c function
                int r = t1->nextOf()->isZeroInit() ? RTLSYM_ARRAYSETLENGTHT : RTLSYM_ARRAYSETLENGTHIT;

                e = el_bin(OPcall, totym(ae->type), el_var(rtlsym[r]), ep);
                goto Lret;
            }

            // Look for array[]=n
            if (ae->e1->op == TOKslice)
            {
                SliceExp *are = (SliceExp *)ae->e1;
                Type *t1 = t1b;
                Type *t2 = ae->e2->type->toBasetype();
                Type *ta = are->e1->type->toBasetype();

                /* Optimize static array assignment with array literal.
                 * Rewrite:
                 *      sa[] = [a, b, ...];
                 * as:
                 *      sa[0] = a, sa[1] = b, ...;
                 *
                 * If the same values are contiguous, that will be rewritten
                 * to block assignment.
                 * Rewrite:
                 *      sa[] = [x, a, a, b, ...];
                 * as:
                 *      sa[0] = x, sa[1..2] = a, sa[3] = b, ...;
                 */
                if (are->lwr == NULL && ta->ty == Tsarray &&
                    ae->e2->op == TOKarrayliteral &&
                    ae->op == TOKconstruct &&   // Bugzilla 11238: avoid aliasing issue
                    t2->nextOf()->mutableOf()->implicitConvTo(ta->nextOf()))
                {
                    ArrayLiteralExp *ale = (ArrayLiteralExp *)ae->e2;
                    TypeSArray *tsa = (TypeSArray *)ta;

                    size_t dim = ale->elements->dim;
                    if (dim == 0)
                    {
                        goto Lx;
                    #if 0
                        /* This code doesn't work with -O switch. Because backend optimizer
                         * will eliminate this useless initializing code for zero-length
                         * static array completely, then the variable kept in "unset".
                         * Instead to fallback to the __d_arrayliteral call.
                         */
                        symbol *stmp = symbol_genauto(TYnptr);
                        e = are->e1->toElem(irs);
                        e = addressElem(e, tsa);
                        e = el_bin(OPeq, TYnptr, el_var(stmp), e);

                        elem *e1 = el_var(stmp);
                        e1 = el_una(OPind, totym(tsa), e1);
                        e1->ET = Type_toCtype(tsa);
                        elem *e2 = el_long(totym(tsa), 0);

                        elem *ex = el_bin(OPstreq, TYstruct, e1, e2);
                        ex->ET = Type_toCtype(tsa);
                        e = el_combine(e, ex);
                        goto Lret;
                    #endif
                    }

                    Type *tn = tsa->nextOf()->toBasetype();
                    bool postblit = needsPostblit(tn) != NULL;
                    tym_t ty = totym(tn);

                    symbol *stmp = symbol_genauto(TYnptr);
                    e = are->e1->toElem(irs);
                    e = addressElem(e, tsa);
                    e = el_bin(OPeq, TYnptr, el_var(stmp), e);

                    size_t esz = tn->size();
                    for (size_t i = 0; i < dim; )
                    {
                        Expression *en = (*ale->elements)[i];
                        size_t j = i + 1;
                        if (!postblit)
                        {
                            // If the elements are same literal and elaborate copy
                            // is not necessary, do memcpy.
                            while (j < dim && en->equals((*ale->elements)[j])) { j++; }
                        }

                        elem *e1 = el_var(stmp);
                        if (i > 0)
                            e1 = el_bin(OPadd, TYnptr, e1, el_long(TYsize_t, i * esz));
                        elem *ex;
                        if (j == i + 1)
                        {
                            e1 = el_una(OPind, ty, e1);
                            if (tybasic(ty) == TYstruct)
                                e1->ET = Type_toCtype(tn);
                            ex = el_bin(OPeq, e1->Ety, e1, en->toElem(irs));
                            if (tybasic(ty) == TYstruct)
                            {
                                ex->Eoper = OPstreq;
                                ex->ET = Type_toCtype(tn);
                            }
                        }
                        else
                        {
                            assert(j - i >= 2);
                            elem *edim = el_long(TYsize_t, j - i);
                            ex = setArray(e1, edim, tn, en->toElem(irs), irs, ae->op);
                        }
                        e = el_combine(e, ex);
                        i = j;
                    }
                    goto Lret;
                }
            Lx:

                if (ae->op == TOKconstruct && !ae->ismemset)
                {
                    Expression *e1x = are->e1;
                    Expression *e2x = ae->e2;
                    if (e2x->op == TOKcast)
                    {
                        Expression *e2y = ((CastExp *)e2x)->e1;
                        if (Type *t2n = e2y->type->toBasetype()->nextOf())
                        {
                            Type *t1n = e1x->type->toBasetype()->nextOf();
                            assert(t1n);
                            Type *t1 = t1n->arrayOf()->immutableOf();
                            Type *t2 = t2n->arrayOf()->immutableOf();
                            if (t1->equals(t2))
                                e2x = e2y;
                        }
                    }
                    if (e2x->op == TOKcall)
                    {
                        CallExp *ce = (CallExp *)e2x;

                        TypeFunction *tf = (TypeFunction *)ce->e1->type->toBasetype();
                        if (tf->ty == Tfunction && retStyle(tf) == RETstack)
                        {
                            elem *ehidden = e1x->toElem(irs);
                            ehidden = el_una(OPaddr, TYnptr, ehidden);
                            assert(!irs->ehidden);
                            irs->ehidden = ehidden;
                            e = ce->toElem(irs);
                            goto Lret;
                        }
                    }
                }

                // which we do if the 'next' types match
                if (ae->ismemset)
                {
                    // Do a memset for array[]=v
                    //printf("Lpair %s\n", ae->toChars());
                    Type *tb = ta->nextOf()->toBasetype();
                    unsigned sz = tb->size();
                    tym_t tym = totym(ae->type);

                    elem *n1 = are->e1->toElem(irs);
                    elem *elwr = are->lwr ? are->lwr->toElem(irs) : NULL;
                    elem *eupr = are->upr ? are->upr->toElem(irs) : NULL;

                    elem *n1x = n1;

                    elem *enbytes;
                    elem *einit;
                    // Look for array[]=n
                    if (ta->ty == Tsarray)
                    {
                        TypeSArray *ts = (TypeSArray *)ta;
                        n1 = array_toPtr(ta, n1);
                        enbytes = ts->dim->toElem(irs);
                        n1x = n1;
                        n1 = el_same(&n1x);
                        einit = resolveLengthVar(are->lengthVar, &n1, ta);
                    }
                    else if (ta->ty == Tarray)
                    {
                        n1 = el_same(&n1x);
                        einit = resolveLengthVar(are->lengthVar, &n1, ta);
                        enbytes = el_copytree(n1);
                        n1 = array_toPtr(ta, n1);
                        enbytes = el_una(I64 ? OP128_64 : OP64_32, TYsize_t, enbytes);
                    }
                    else if (ta->ty == Tpointer)
                    {
                        n1 = el_same(&n1x);
                        enbytes = el_long(TYsize_t, -1);   // largest possible index
                        einit = NULL;
                    }

                    // Enforce order of evaluation of n1[elwr..eupr] as n1,elwr,eupr
                    elem *elwrx = elwr;
                    if (elwr) elwr = el_same(&elwrx);
                    elem *euprx = eupr;
                    if (eupr) eupr = el_same(&euprx);

        #if 0
                    printf("sz = %d\n", sz);
                    printf("n1x\n");        elem_print(n1x);
                    printf("einit\n");      elem_print(einit);
                    printf("elwrx\n");      elem_print(elwrx);
                    printf("euprx\n");      elem_print(euprx);
                    printf("n1\n");         elem_print(n1);
                    printf("elwr\n");       elem_print(elwr);
                    printf("eupr\n");       elem_print(eupr);
                    printf("enbytes\n");    elem_print(enbytes);
        #endif
                    einit = el_combine(n1x, einit);
                    einit = el_combine(einit, elwrx);
                    einit = el_combine(einit, euprx);

                    elem *evalue = ae->e2->toElem(irs);

        #if 0
                    printf("n1\n");         elem_print(n1);
                    printf("enbytes\n");    elem_print(enbytes);
        #endif

                    if (irs->arrayBoundsCheck() && eupr && ta->ty != Tpointer)
                    {
                        assert(elwr);
                        elem *enbytesx = enbytes;
                        enbytes = el_same(&enbytesx);
                        elem *c1 = el_bin(OPle, TYint, el_copytree(eupr), enbytesx);
                        elem *c2 = el_bin(OPle, TYint, el_copytree(elwr), el_copytree(eupr));
                        c1 = el_bin(OPandand, TYint, c1, c2);

                        // Construct: (c1 || ModuleArray(line))
                        Symbol *sassert = irs->blx->module->toModuleArray();
                        elem *ea = el_bin(OPcall,TYvoid,el_var(sassert), el_long(TYint, ae->loc.linnum));
                        elem *eb = el_bin(OPoror,TYvoid,c1,ea);
                        einit = el_combine(einit, eb);
                    }

                    elem *elength;
                    if (elwr)
                    {
                        el_free(enbytes);
                        elem *elwr2 = el_copytree(elwr);
                        elwr2 = el_bin(OPmul, TYsize_t, elwr2, el_long(TYsize_t, sz));
                        n1 = el_bin(OPadd, TYnptr, n1, elwr2);
                        enbytes = el_bin(OPmin, TYsize_t, eupr, elwr);
                        elength = el_copytree(enbytes);
                    }
                    else
                        elength = el_copytree(enbytes);
                    e = setArray(n1, enbytes, tb, evalue, irs, ae->op);
                    e = el_pair(TYdarray, elength, e);
                    e = el_combine(einit, e);
                    //elem_print(e);
                    goto Lret;
                }
                else
                {
                    /* It's array1[]=array2[]
                     * which is a memcpy
                     */
                    elem *eto = ae->e1->toElem(irs);
                    elem *efrom = ae->e2->toElem(irs);

                    unsigned size = t1->nextOf()->size();
                    elem *esize = el_long(TYsize_t, size);

                    /* Determine if we need to do postblit
                     */
                    int postblit = 0;
                    if (needsPostblit(t1->nextOf()) &&
                        (ae->e2->op == TOKslice && ((UnaExp *)ae->e2)->e1->isLvalue() ||
                         ae->e2->op == TOKcast  && ((UnaExp *)ae->e2)->e1->isLvalue() ||
                         ae->e2->op != TOKslice && ae->e2->isLvalue()))
                    {
                        postblit = 1;
                    }

                    assert(ae->e2->type->ty != Tpointer);

                    if (!postblit && !irs->arrayBoundsCheck())
                    {
                        elem *ex = el_same(&eto);

                        // Determine if elen is a constant
                        elem *elen;
                        if (eto->Eoper == OPpair &&
                            eto->E1->Eoper == OPconst)
                        {
                            elen = el_copytree(eto->E1);
                        }
                        else
                        {
                            // It's not a constant, so pull it from the dynamic array
                            elen = el_una(I64 ? OP128_64 : OP64_32, TYsize_t, el_copytree(ex));
                        }

                        esize = el_bin(OPmul, TYsize_t, elen, esize);
                        elem *epto = array_toPtr(ae->e1->type, ex);
                        elem *epfr = array_toPtr(ae->e2->type, efrom);
                        e = el_params(esize, epfr, epto, NULL);
                        e = el_bin(OPcall,TYnptr,el_var(rtlsym[RTLSYM_MEMCPY]),e);
                        e = el_pair(eto->Ety, el_copytree(elen), e);
                        e = el_combine(eto, e);
                    }
                    else if (postblit && ae->op != TOKblit)
                    {
                        /* Generate:
                         *      _d_arrayassign(ti, efrom, eto)
                         * or:
                         *      _d_arrayctor(ti, efrom, eto)
                         */
                        el_free(esize);
                        Expression *ti = t1->nextOf()->toBasetype()->getTypeInfo(NULL);
                        if (config.exe == EX_WIN64)
                        {
                            eto   = addressElem(eto,   Type::tvoid->arrayOf());
                            efrom = addressElem(efrom, Type::tvoid->arrayOf());
                        }
                        elem *ep = el_params(eto, efrom, ti->toElem(irs), NULL);
                        int rtl = (ae->op == TOKconstruct) ? RTLSYM_ARRAYCTOR : RTLSYM_ARRAYASSIGN;
                        e = el_bin(OPcall, totym(ae->type), el_var(rtlsym[rtl]), ep);
                    }
                    else
                    {
                        // Generate:
                        //      _d_arraycopy(eto, efrom, esize)

                        if (config.exe == EX_WIN64)
                        {
                            eto   = addressElem(eto,   Type::tvoid->arrayOf());
                            efrom = addressElem(efrom, Type::tvoid->arrayOf());
                        }
                        elem *ep = el_params(eto, efrom, esize, NULL);
                        e = el_bin(OPcall, totym(ae->type), el_var(rtlsym[RTLSYM_ARRAYCOPY]), ep);
                    }
                    el_setLoc(e, ae->loc);
                    result = e;
                    return;
                }
            }

            /* Look for reference initializations
             */
            if (ae->op == TOKconstruct && ae->e1->op == TOKvar)
            {
                VarExp *ve = (VarExp *)ae->e1;
                Declaration *s = ve->var;
                if (s->storage_class & (STCout | STCref))
                {
                    e = ae->e2->toElem(irs);
                    e = addressElem(e, ae->e2->type);
                    elem *es = ae->e1->toElem(irs);
                    if (es->Eoper == OPind)
                        es = es->E1;
                    else
                        es = el_una(OPaddr, TYnptr, es);
                    es->Ety = TYnptr;
                    e = el_bin(OPeq, TYnptr, es, e);
                    // BUG: type is struct, and e2 is TOKint64
                    goto Lret;
                }
            }

            /* This will work if we can distinguish an assignment from
             * an initialization of the lvalue. It'll work if the latter.
             * If the former, because of aliasing of the return value with
             * function arguments, it'll fail.
             */
            if (ae->op == TOKconstruct && ae->e2->op == TOKcall)
            {
                CallExp *ce = (CallExp *)ae->e2;
                TypeFunction *tf = (TypeFunction *)ce->e1->type->toBasetype();
                if (tf->ty == Tfunction && retStyle(tf) == RETstack)
                {
                    elem *ehidden = ae->e1->toElem(irs);
                    ehidden = el_una(OPaddr, TYnptr, ehidden);
                    assert(!irs->ehidden);
                    irs->ehidden = ehidden;
                    e = ae->e2->toElem(irs);
                    goto Lret;
                }
            }

            //if (op == TOKconstruct) printf("construct\n");
            if (t1b->ty == Tstruct || t1b->ty == Tsarray)
            {
                elem *eleft = ae->e1->toElem(irs);
                if (ae->e2->op == TOKint64)
                {
                    /* Implement:
                     *  (struct = 0)
                     * with:
                     *  memset(&struct, 0, struct.sizeof)
                     */
                    elem *ey = NULL;
                    unsigned sz = ae->e1->type->size();
                    StructDeclaration *sd = ((TypeStruct *)t1b)->sym;
                    if (sd->isNested() && ae->op == TOKconstruct)
                    {
                        ey = el_una(OPaddr, TYnptr, eleft);
                        eleft = el_same(&ey);
                        ey = setEthis(ae->loc, irs, ey, sd);
                        sz = sd->vthis->offset;
                    }

                    elem *el = eleft;
                    elem *enbytes = el_long(TYsize_t, sz);
                    elem *evalue = el_long(TYsize_t, 0);

                    if (!(sd->isNested() && ae->op == TOKconstruct))
                        el = el_una(OPaddr, TYnptr, el);
                    e = el_param(enbytes, evalue);
                    e = el_bin(OPmemset,TYnptr,el,e);
                    e = el_combine(ey, e);
                    el_setLoc(e, ae->loc);
                    //e = el_una(OPind, TYstruct, e);
                }
                else
                {
                    //printf("toElemBin() '%s'\n", ae->toChars());

                    tym_t tym = totym(ae->type);

                    elem *e1 = eleft;
                    elem *ex = e1;
                    if (e1->Eoper == OPind)
                        ex = e1->E1;
                    if (ae->e2->op == TOKstructliteral &&
                        ex->Eoper == OPvar && ex->EV.sp.Voffset == 0)
                    {
                        StructLiteralExp *se = (StructLiteralExp *)ae->e2;

                        Symbol *symSave = se->sym;
                        size_t soffsetSave = se->soffset;
                        int fillHolesSave = se->fillHoles;

                        se->sym = ex->EV.sp.Vsym;
                        se->soffset = 0;
                        se->fillHoles = (ae->op == TOKconstruct || ae->op == TOKblit) ? 1 : 0;

                        el_free(e1);
                        e = ae->e2->toElem(irs);

                        se->sym = symSave;
                        se->soffset = soffsetSave;
                        se->fillHoles = fillHolesSave;
                    }
                    else
                    {
                        elem *e2 = ae->e2->toElem(irs);
                        e = el_bin(OPstreq,tym,e1,e2);
                        e->ET = Type_toCtype(ae->e1->type);
                        if (type_size(e->ET) == 0)
                            e->Eoper = OPcomma;
                    }
                    goto Lret;
                }
            }
            else
                e = toElemBin(ae,OPeq);
            result = e;
            return;

        Lret:
            el_setLoc(e, ae->loc);
            result = e;
        }

        /***************************************
         */

        void visit(AddAssignExp *e)
        {
            //printf("AddAssignExp::toElem() %s\n", e->toChars());
            result = toElemBin(e, OPaddass);
        }


        /***************************************
         */

        void visit(MinAssignExp *e)
        {
            result = toElemBin(e, OPminass);
        }

        /***************************************
         */

        void visit(CatAssignExp *ce)
        {
            //printf("CatAssignExp::toElem('%s')\n", ce->toChars());
            elem *e;
            Type *tb1 = ce->e1->type->toBasetype();
            Type *tb2 = ce->e2->type->toBasetype();

            if (tb1->ty == Tarray && tb2->ty == Tdchar &&
                (tb1->nextOf()->ty == Tchar || tb1->nextOf()->ty == Twchar))
            {
                // Append dchar to char[] or wchar[]
                elem *e1 = ce->e1->toElem(irs);
                e1 = el_una(OPaddr, TYnptr, e1);

                elem *e2 = ce->e2->toElem(irs);

                elem *ep = el_params(e2, e1, NULL);
                int rtl = (tb1->nextOf()->ty == Tchar)
                        ? RTLSYM_ARRAYAPPENDCD
                        : RTLSYM_ARRAYAPPENDWD;
                e = el_bin(OPcall, TYdarray, el_var(rtlsym[rtl]), ep);
                el_setLoc(e, ce->loc);
            }
            else if (tb1->ty == Tarray || tb2->ty == Tsarray)
            {
                elem *e1 = ce->e1->toElem(irs);
                elem *e2 = ce->e2->toElem(irs);

                Type *tb1n = tb1->nextOf()->toBasetype();
                if ((tb2->ty == Tarray || tb2->ty == Tsarray) &&
                    tb1n->equals(tb2->nextOf()->toBasetype()))
                {
                    // Append array
                    e1 = el_una(OPaddr, TYnptr, e1);
                    if (config.exe == EX_WIN64)
                        e2 = addressElem(e2, tb2);
                    else
                        e2 = useOPstrpar(e2);
                    elem *ep = el_params(e2, e1, ce->e1->type->getTypeInfo(NULL)->toElem(irs), NULL);
                    e = el_bin(OPcall, TYdarray, el_var(rtlsym[RTLSYM_ARRAYAPPENDT]), ep);
                }
                else if (tb1n->equals(tb2))
                {
                    // Append element

                    elem *e2x = NULL;

                    if (e2->Eoper != OPvar && e2->Eoper != OPconst)
                    {
                        // Evaluate e2 and assign result to temporary s2.
                        // Do this because of:
                        //    a ~= a[$-1]
                        // because $ changes its value
                        symbol *s2 = symbol_genauto(Type_toCtype(tb2));
                        e2x = el_bin(OPeq, e2->Ety, el_var(s2), e2);
                        if (tybasic(e2->Ety) == TYstruct)
                        {
                            e2x->Eoper = OPstreq;
                            e2x->ET = Type_toCtype(tb1n);
                        }
                        else if (tybasic(e2->Ety) == TYarray)
                        {
                            e2x->Eoper = OPstreq;
                            e2x->Ejty = e2x->Ety = TYstruct;
                            e2x->ET = Type_toCtype(tb1n);
                        }
                        e2 = el_var(s2);
                    }

                    // Extend array with _d_arrayappendcTX(TypeInfo ti, e1, 1)
                    e1 = el_una(OPaddr, TYnptr, e1);
                    elem *ep = el_param(e1, ce->e1->type->getTypeInfo(NULL)->toElem(irs));
                    ep = el_param(el_long(TYsize_t, 1), ep);
                    e = el_bin(OPcall, TYdarray, el_var(rtlsym[RTLSYM_ARRAYAPPENDCTX]), ep);
                    symbol *stmp = symbol_genauto(Type_toCtype(tb1));
                    e = el_bin(OPeq, TYdarray, el_var(stmp), e);

                    // Assign e2 to last element in stmp[]
                    // *(stmp.ptr + (stmp.length - 1) * szelem) = e2

                    elem *eptr = array_toPtr(tb1, el_var(stmp));
                    elem *elength = el_una(I64 ? OP128_64 : OP64_32, TYsize_t, el_var(stmp));
                    elength = el_bin(OPmin, TYsize_t, elength, el_long(TYsize_t, 1));
                    elength = el_bin(OPmul, TYsize_t, elength, el_long(TYsize_t, ce->e2->type->size()));
                    eptr = el_bin(OPadd, TYnptr, eptr, elength);
                    elem *ederef = el_una(OPind, e2->Ety, eptr);
                    elem *eeq = el_bin(OPeq, e2->Ety, ederef, e2);

                    if (tybasic(e2->Ety) == TYstruct)
                    {
                        eeq->Eoper = OPstreq;
                        eeq->ET = Type_toCtype(tb1n);
                    }
                    else if (tybasic(e2->Ety) == TYarray)
                    {
                        eeq->Eoper = OPstreq;
                        eeq->Ejty = eeq->Ety = TYstruct;
                        eeq->ET = Type_toCtype(tb1n);
                    }

                    e = el_combine(e2x, e);
                    e = el_combine(e, eeq);
                    e = el_combine(e, el_var(stmp));
                }
                else
                {
                    ce->error("ICE: cannot append '%s' to '%s'", tb2->toChars(), tb1->toChars());
                    assert(0);
                }

                el_setLoc(e, ce->loc);
            }
            else
                assert(0);
            result = e;
        }

        /***************************************
         */

        void visit(DivAssignExp *e)
        {
            result = toElemBin(e, OPdivass);
        }

        /***************************************
         */

        void visit(ModAssignExp *e)
        {
            result = toElemBin(e, OPmodass);
        }

        /***************************************
         */

        void visit(MulAssignExp *e)
        {
            result = toElemBin(e, OPmulass);
        }

        /***************************************
         */

        void visit(ShlAssignExp *e)
        {
            result = toElemBin(e, OPshlass);
        }

        /***************************************
         */

        void visit(ShrAssignExp *e)
        {
            //printf("ShrAssignExp::toElem() %s, %s\n", e->e1->type->toChars(), e->e1->toChars());
            Type *t1 = e->e1->type;
            if (e->e1->op == TOKcast)
            {
                /* Use the type before it was integrally promoted to int
                 */
                CastExp *ce = (CastExp *)e->e1;
                t1 = ce->e1->type;
            }
            result = toElemBin(e, t1->isunsigned() ? OPshrass : OPashrass);
        }

        /***************************************
         */

        void visit(UshrAssignExp *e)
        {
            result = toElemBin(e, OPshrass);
        }

        /***************************************
         */

        void visit(AndAssignExp *e)
        {
            result = toElemBin(e, OPandass);
        }

        /***************************************
         */

        void visit(OrAssignExp *e)
        {
            result = toElemBin(e, OPorass);
        }

        /***************************************
         */

        void visit(XorAssignExp *e)
        {
            result = toElemBin(e, OPxorass);
        }

        /***************************************
         */

        void visit(PowAssignExp *e)
        {
            Type *tb1 = e->e1->type->toBasetype();
            assert(tb1->ty != Tarray && tb1->ty != Tsarray);

            e->error("^^ operator is not supported");
            result = el_long(totym(e->type), 0);  // error recovery
        }

        /***************************************
         */

        void visit(AndAndExp *aae)
        {
            tym_t tym = totym(aae->type);

            elem *el = aae->e1->toElem(irs);
            elem *er = toElemDtor(aae->e2, irs);
            elem *e = el_bin(OPandand,tym,el,er);

            el_setLoc(e, aae->loc);

            if (global.params.cov && aae->e2->loc.linnum)
                e->E2 = el_combine(incUsageElem(irs, aae->e2->loc), e->E2);
            result = e;
        }

        /***************************************
         */

        void visit(OrOrExp *ooe)
        {
            tym_t tym = totym(ooe->type);

            elem *el = ooe->e1->toElem(irs);
            elem *er = toElemDtor(ooe->e2, irs);
            elem *e = el_bin(OPoror,tym,el,er);

            el_setLoc(e, ooe->loc);

            if (global.params.cov && ooe->e2->loc.linnum)
                e->E2 = el_combine(incUsageElem(irs, ooe->e2->loc), e->E2);
            result = e;
        }

        /***************************************
         */

        void visit(XorExp *e)
        {
            result = toElemBin(e, OPxor);
        }

        /***************************************
         */

        void visit(PowExp *e)
        {
            Type *tb1 = e->e1->type->toBasetype();
            assert(tb1->ty != Tarray && tb1->ty != Tsarray);

            e->error("^^ operator is not supported");
            result = el_long(totym(e->type), 0);  // error recovery
        }

        /***************************************
         */

        void visit(AndExp *e)
        {
            result = toElemBin(e, OPand);
        }

        /***************************************
         */

        void visit(OrExp *e)
        {
            result = toElemBin(e, OPor);
        }

        /***************************************
         */

        void visit(ShlExp *e)
        {
            result = toElemBin(e, OPshl);
        }

        /***************************************
         */

        void visit(ShrExp *e)
        {
            result = toElemBin(e, e->e1->type->isunsigned() ? OPshr : OPashr);
        }

        /***************************************
         */

        void visit(UshrExp *se)
        {
            elem *eleft  = se->e1->toElem(irs);
            eleft->Ety = touns(eleft->Ety);
            elem *eright = se->e2->toElem(irs);
            elem *e = el_bin(OPshr, totym(se->type), eleft, eright);
            el_setLoc(e, se->loc);
            result = e;
        }

        /****************************************
         */

        void visit(CommaExp *ce)
        {
            assert(ce->e1 && ce->e2);
            elem *eleft  = ce->e1->toElem(irs);
            elem *eright = ce->e2->toElem(irs);
            elem *e = el_combine(eleft, eright);
            if (e)
                el_setLoc(e, ce->loc);
            result = e;
        }

        /***************************************
         */

        void visit(CondExp *ce)
        {
            elem *ec = ce->econd->toElem(irs);

            elem *eleft = toElemDtor(ce->e1, irs);
            tym_t ty = eleft->Ety;
            if (global.params.cov && ce->e1->loc.linnum)
                eleft = el_combine(incUsageElem(irs, ce->e1->loc), eleft);

            elem *eright = toElemDtor(ce->e2, irs);
            if (global.params.cov && ce->e2->loc.linnum)
                eright = el_combine(incUsageElem(irs, ce->e2->loc), eright);

            elem *e = el_bin(OPcond, ty, ec, el_bin(OPcolon, ty, eleft, eright));
            if (tybasic(ty) == TYstruct)
                e->ET = Type_toCtype(ce->e1->type);
            el_setLoc(e, ce->loc);
            result = e;
        }

        /***************************************
         */

        void visit(TypeExp *e)
        {
        #if 0
            printf("TypeExp::toElem()\n");
        #endif
            e->error("type %s is not an expression", e->toChars());
            result = el_long(TYint, 0);
        }

        void visit(ScopeExp *e)
        {
            e->error("%s is not an expression", e->sds->toChars());
            result = el_long(TYint, 0);
        }

        void visit(DotVarExp *dve)
        {
            // *(&e + offset)

            //printf("DotVarExp::toElem('%s')\n", dve->toChars());

            VarDeclaration *v = dve->var->isVarDeclaration();
            if (!v)
            {
                dve->error("%s is not a field, but a %s", dve->var->toChars(), dve->var->kind());
            }

            elem *e = dve->e1->toElem(irs);
            Type *tb1 = dve->e1->type->toBasetype();
            if (tb1->ty != Tclass && tb1->ty != Tpointer)
                e = addressElem(e, tb1);
            e = el_bin(OPadd, TYnptr, e, el_long(TYsize_t, v ? v->offset : 0));
            e = el_una(OPind, totym(dve->type), e);
            if (tybasic(e->Ety) == TYstruct)
            {
                e->ET = Type_toCtype(dve->type);
            }
            el_setLoc(e,dve->loc);
            result = e;
        }

        void visit(DelegateExp *de)
        {
            int directcall = 0;
            //printf("DelegateExp::toElem() '%s'\n", de->toChars());

            if (de->func->semanticRun == PASSsemantic3done)
            {
                // Bug 7745 - only include the function if it belongs to this module
                // ie, it is a member of this module, or is a template instance
                // (the template declaration could come from any module).
                Dsymbol *owner = de->func->toParent();
                while (!owner->isTemplateInstance() && owner->toParent())
                    owner = owner->toParent();
                if (owner->isTemplateInstance() || owner == irs->m )
                {
                    irs->deferToObj->push(de->func);
                }
            }

            elem *ethis;
            Symbol *sfunc = de->func->toSymbol();
            elem *ep;
            if (de->func->isNested())
            {
                ep = el_ptr(sfunc);
                if (de->e1->op == TOKnull)
                    ethis = de->e1->toElem(irs);
                else
                    ethis = getEthis(de->loc, irs, de->func);
            }
            else
            {
                ethis = de->e1->toElem(irs);
                if (de->e1->type->ty != Tclass && de->e1->type->ty != Tpointer)
                    ethis = addressElem(ethis, de->e1->type);

                if (de->e1->op == TOKsuper || de->e1->op == TOKdottype)
                    directcall = 1;

                if (!de->func->isThis())
                    de->error("delegates are only for non-static functions");

                if (!de->func->isVirtual() ||
                    directcall ||
                    de->func->isFinalFunc())
                {
                    ep = el_ptr(sfunc);
                }
                else
                {
                    // Get pointer to function out of virtual table

                    assert(ethis);
                    ep = el_same(&ethis);
                    ep = el_una(OPind, TYnptr, ep);
                    unsigned vindex = de->func->vtblIndex;

                    assert((int)vindex >= 0);

                    // Build *(ep + vindex * 4)
                    ep = el_bin(OPadd,TYnptr,ep,el_long(TYsize_t, vindex * Target::ptrsize));
                    ep = el_una(OPind,TYnptr,ep);
                }

                //if (func->tintro)
                //    func->error(loc, "cannot form delegate due to covariant return type");
            }
            elem *e;
            if (ethis->Eoper == OPcomma)
            {
                ethis->E2 = el_pair(TYdelegate, ethis->E2, ep);
                ethis->Ety = TYdelegate;
                e = ethis;
            }
            else
                e = el_pair(TYdelegate, ethis, ep);
            el_setLoc(e, de->loc);
            result = e;
        }

        void visit(DotTypeExp *dte)
        {
            // Just a pass-thru to e1
            //printf("DotTypeExp::toElem() %s\n", dte->toChars());
            elem *e = dte->e1->toElem(irs);
            el_setLoc(e, dte->loc);
            result = e;
        }

        void visit(CallExp *ce)
        {
            //printf("CallExp::toElem('%s')\n", ce->toChars());
            assert(ce->e1->type);
            Type *t1 = ce->e1->type->toBasetype();
            Type *ectype = t1;
            elem *eeq = NULL;

            elem *ehidden = irs->ehidden;
            irs->ehidden = NULL;

            int directcall = 0;
            elem *ec;
            FuncDeclaration *fd = NULL;
            if (ce->e1->op == TOKdotvar && t1->ty != Tdelegate)
            {
                DotVarExp *dve = (DotVarExp *)ce->e1;

                fd = dve->var->isFuncDeclaration();
                Expression *ex = dve->e1;
                while (1)
                {
                    switch (ex->op)
                    {
                        case TOKsuper:          // super.member() calls directly
                        case TOKdottype:        // type.member() calls directly
                            directcall = 1;
                            break;

                        case TOKcast:
                            ex = ((CastExp *)ex)->e1;
                            continue;

                        default:
                            //ex->dump(0);
                            break;
                    }
                    break;
                }
                if (dve->e1->op == TOKstructliteral)
                {
                    StructLiteralExp *sle = (StructLiteralExp *)dve->e1;
                    sle->sinit = NULL;          // don't modify initializer
                }

                ec = dve->e1->toElem(irs);
                ectype = dve->e1->type->toBasetype();

                if (ce->arguments && ce->arguments->dim && ec->Eoper != OPvar)
                {
                    if (ec->Eoper == OPind && el_sideeffect(ec->E1))
                    {
                        /* Rewrite (*exp)(arguments) as:
                         * tmp = exp, (*tmp)(arguments)
                         */
                        elem *ec1 = ec->E1;
                        Symbol *stmp = symbol_genauto(type_fake(ec1->Ety));
                        eeq = el_bin(OPeq, ec->Ety, el_var(stmp), ec1);
                        ec->E1 = el_var(stmp);
                    }
                    else if (tybasic(ec->Ety) != TYnptr)
                    {
                        /* Rewrite (exp)(arguments) as:
                         * tmp=&exp, (*tmp)(arguments)
                         */
                        ec = addressElem(ec, ectype);

                        Symbol *stmp = symbol_genauto(type_fake(ec->Ety));
                        eeq = el_bin(OPeq, ec->Ety, el_var(stmp), ec);
                        ec = el_una(OPind, totym(ectype), el_var(stmp));
                    }
                }
            }
            else if (ce->e1->op == TOKvar)
            {
                fd = ((VarExp *)ce->e1)->var->isFuncDeclaration();
        #if 0 // This optimization is not valid if alloca can be called
              // multiple times within the same function, eg in a loop
              // see issue 3822
                if (fd && fd->ident == Id::__alloca &&
                    !fd->fbody && fd->linkage == LINKc &&
                    arguments && arguments->dim == 1)
                {   Expression *arg = (*arguments)[0];
                    arg = arg->optimize(WANTvalue);
                    if (arg->isConst() && arg->type->isintegral())
                    {   dinteger_t sz = arg->toInteger();
                        if (sz > 0 && sz < 0x40000)
                        {
                            // It's an alloca(sz) of a fixed amount.
                            // Replace with an array allocated on the stack
                            // of the same size: char[sz] tmp;

                            assert(!ehidden);
                            ::type *t = type_static_array(sz, tschar);  // BUG: fix extra Tcount++
                            Symbol *stmp = symbol_genauto(t);
                            ec = el_ptr(stmp);
                            el_setLoc(ec,loc);
                            return ec;
                        }
                    }
                }
        #endif

                ec = ce->e1->toElem(irs);
            }
            else
            {
                ec = ce->e1->toElem(irs);
                if (ce->arguments && ce->arguments->dim)
                {
                    /* The idea is to enforce expressions being evaluated left to right,
                     * even though call trees are evaluated parameters first.
                     * We just do a quick hack to catch the more obvious cases, though
                     * we need to solve this generally.
                     */
                    if (ec->Eoper == OPind && el_sideeffect(ec->E1))
                    {
                        /* Rewrite (*exp)(arguments) as:
                         * tmp=exp, (*tmp)(arguments)
                         */
                        elem *ec1 = ec->E1;
                        Symbol *stmp = symbol_genauto(type_fake(ec1->Ety));
                        eeq = el_bin(OPeq, ec->Ety, el_var(stmp), ec1);
                        ec->E1 = el_var(stmp);
                    }
                    else if (tybasic(ec->Ety) == TYdelegate && el_sideeffect(ec))
                    {
                        /* Rewrite (exp)(arguments) as:
                         * tmp=exp, (tmp)(arguments)
                         */
                        Symbol *stmp = symbol_genauto(type_fake(ec->Ety));
                        eeq = el_bin(OPeq, ec->Ety, el_var(stmp), ec);
                        ec = el_var(stmp);
                    }
                }
            }
            ec = callfunc(ce->loc, irs, directcall, ce->type, ec, ectype, fd, t1, ehidden, ce->arguments);
            el_setLoc(ec, ce->loc);
            if (eeq)
                ec = el_combine(eeq, ec);
            result = ec;
        }

        void visit(AddrExp *ae)
        {
            //printf("AddrExp::toElem('%s')\n", ae->toChars());
            if (ae->e1->op == TOKstructliteral)
            {
                StructLiteralExp *sl = (StructLiteralExp*)ae->e1;
                //printf("AddrExp::toElem('%s') %d\n", ae->toChars(), ae);
                //printf("StructLiteralExp(%p); origin:%p\n", sl, sl->origin);
                //printf("sl->toSymbol() (%p)\n", sl->toSymbol());
                elem *e = el_ptr(sl->origin->toSymbol());
                e->ET = Type_toCtype(ae->type);
                el_setLoc(e, ae->loc);
                result = e;
                return;
            }
            else
            {
                elem *e = ae->e1->toElem(irs);
                e = addressElem(e, ae->e1->type);
                e->Ety = totym(ae->type);
                el_setLoc(e, ae->loc);
                result = e;
                return;
            }
        }

        void visit(PtrExp *pe)
        {
            //printf("PtrExp::toElem() %s\n", pe->toChars());
            elem *e = pe->e1->toElem(irs);
            e = el_una(OPind,totym(pe->type),e);
            if (tybasic(e->Ety) == TYstruct)
            {
                e->ET = Type_toCtype(pe->type);
            }
            el_setLoc(e, pe->loc);
            result = e;
        }

        void visit(BoolExp *e)
        {
            elem *e1 = e->e1->toElem(irs);
            result = el_una(OPbool, totym(e->type), e1);
        }

        void visit(DeleteExp *de)
        {
            Type *tb;

            //printf("DeleteExp::toElem()\n");
            if (de->e1->op == TOKindex)
            {
                IndexExp *ae = (IndexExp *)de->e1;
                tb = ae->e1->type->toBasetype();
                assert(tb->ty != Taarray);
            }
            //e1->type->print();
            elem *e = de->e1->toElem(irs);
            tb = de->e1->type->toBasetype();
            int rtl;
            switch (tb->ty)
            {
                case Tarray:
                {
                    e = addressElem(e, de->e1->type);
                    rtl = RTLSYM_DELARRAYT;

                    /* See if we need to run destructors on the array contents
                     */
                    elem *et = NULL;
                    Type *tv = tb->nextOf()->baseElemOf();
                    if (tv->ty == Tstruct)
                    {
                        TypeStruct *ts = (TypeStruct *)tv;
                        StructDeclaration *sd = ts->sym;
                        if (sd->dtor)
                            et = tb->nextOf()->getTypeInfo(NULL)->toElem(irs);
                    }
                    if (!et)                            // if no destructors needed
                        et = el_long(TYnptr, 0);        // pass null for TypeInfo
                    e = el_params(et, e, NULL);
                    // call _d_delarray_t(e, et);
                    e = el_bin(OPcall, TYvoid, el_var(rtlsym[rtl]), e);
                    goto Lret;
                }
                case Tclass:
                    if (de->e1->op == TOKvar)
                    {
                        VarExp *ve = (VarExp *)de->e1;
                        if (ve->var->isVarDeclaration() &&
                            ve->var->isVarDeclaration()->onstack)
                        {
                            rtl = RTLSYM_CALLFINALIZER;
                            if (tb->isClassHandle()->isInterfaceDeclaration())
                                rtl = RTLSYM_CALLINTERFACEFINALIZER;
                            break;
                        }
                    }
                    e = addressElem(e, de->e1->type);
                    rtl = RTLSYM_DELCLASS;
                    if (tb->isClassHandle()->isInterfaceDeclaration())
                        rtl = RTLSYM_DELINTERFACE;
                    break;

                case Tpointer:
                    e = addressElem(e, de->e1->type);
                    rtl = RTLSYM_DELMEMORY;
                    break;

                default:
                    assert(0);
                    break;
            }
            e = el_bin(OPcall, TYvoid, el_var(rtlsym[rtl]), e);

        Lret:
            el_setLoc(e, de->loc);
            result = e;
        }

        void visit(VectorExp *ve)
        {
        #if 0
            printf("VectorExp::toElem()\n");
            ve->print();
            printf("\tfrom: %s\n", ve->e1->type->toChars());
            printf("\tto  : %s\n", ve->to->toChars());
        #endif

            elem *e = el_calloc();
            e->Eoper = OPconst;
            e->Ety = totym(ve->type);

            for (size_t i = 0; i < ve->dim; i++)
            {
                Expression *elem;
                if (ve->e1->op == TOKarrayliteral)
                {
                    ArrayLiteralExp *ea = (ArrayLiteralExp *)ve->e1;
                    elem = (*ea->elements)[i];
                }
                else
                    elem = ve->e1;
                switch (elem->type->toBasetype()->ty)
                {
                    case Tfloat32:
                        ((targ_float *)&e->EV.Vcent)[i] = elem->toReal();
                        break;

                    case Tfloat64:
                        ((targ_double *)&e->EV.Vcent)[i] = elem->toReal();
                        break;

                    case Tint64:
                    case Tuns64:
                        ((targ_ullong *)&e->EV.Vcent)[i] = elem->toInteger();
                        break;

                    case Tint32:
                    case Tuns32:
                        ((targ_ulong *)&e->EV.Vcent)[i] = elem->toInteger();
                        break;

                    case Tint16:
                    case Tuns16:
                        ((targ_ushort *)&e->EV.Vcent)[i] = elem->toInteger();
                        break;

                    case Tint8:
                    case Tuns8:
                        ((targ_uchar *)&e->EV.Vcent)[i] = elem->toInteger();
                        break;

                    default:
                        assert(0);
                }
            }
            el_setLoc(e, ve->loc);
            result = e;
        }

        void visit(CastExp *ce)
        {
            tym_t ftym;
            tym_t ttym;
            OPER eop;

        #if 0
            printf("CastExp::toElem()\n");
            ce->print();
            printf("\tfrom: %s\n", ce->e1->type->toChars());
            printf("\tto  : %s\n", ce->to->toChars());
        #endif

            elem *e = ce->e1->toElem(irs);
            Type *tfrom = ce->e1->type->toBasetype();
            Type *t = ce->to->toBasetype();         // skip over typedef's

            TY fty;
            TY tty;
            if (t->equals(tfrom))
                goto Lret;

            fty = tfrom->ty;
            //printf("fty = %d\n", fty);
            tty = t->ty;

            if (tty == Tpointer && fty == Tarray)
            {
                if (e->Eoper == OPvar)
                {
                    // e1 -> *(&e1 + 4)
                    e = el_una(OPaddr, TYnptr, e);
                    e = el_bin(OPadd, TYnptr, e, el_long(TYsize_t, tysize[TYnptr]));
                    e = el_una(OPind,totym(t),e);
                }
                else
                {
                    // e1 -> (unsigned)(e1 >> 32)
                    if (I64)
                    {
                        e = el_bin(OPshr, TYucent, e, el_long(TYint, 64));
                        e = el_una(OP128_64, totym(t), e);
                    }
                    else
                    {
                        e = el_bin(OPshr, TYullong, e, el_long(TYint, 32));
                        e = el_una(OP64_32, totym(t), e);
                    }
                }
                goto Lret;
            }

            if (tty == Tpointer && fty == Tsarray)
            {
                // e1 -> &e1
                e = el_una(OPaddr, TYnptr, e);
                goto Lret;
            }

            // Convert from static array to dynamic array
            if (tty == Tarray && fty == Tsarray)
            {
                e = sarray_toDarray(ce->loc, tfrom, t, e);
                goto Lret;
            }

            // Convert from dynamic array to dynamic array
            if (tty == Tarray && fty == Tarray)
            {
                unsigned fsize = tfrom->nextOf()->size();
                unsigned tsize = t->nextOf()->size();

                if (fsize != tsize)
                {   // Array element sizes do not match, so we must adjust the dimensions
                    if (config.exe == EX_WIN64)
                        e = addressElem(e, t, true);
                    elem *ep = el_params(e, el_long(TYsize_t, fsize), el_long(TYsize_t, tsize), NULL);
                    e = el_bin(OPcall, totym(ce->type), el_var(rtlsym[RTLSYM_ARRAYCAST]), ep);
                }
                goto Lret;
            }

            // Casting from base class to derived class requires a runtime check
            if (fty == Tclass && tty == Tclass)
            {
                // Casting from derived class to base class is a no-op
                int offset;
                int rtl = RTLSYM_DYNAMIC_CAST;

                ClassDeclaration *cdfrom = tfrom->isClassHandle();
                ClassDeclaration *cdto   = t->isClassHandle();
                if (cdfrom->cpp)
                {
                    if (cdto->cpp)
                    {
                        /* Casting from a C++ interface to a C++ interface
                         * is always a 'paint' operation
                         */
                        goto Lret;                  // no-op
                    }

                    /* Casting from a C++ interface to a class
                     * always results in null because there is no runtime
                     * information available to do it.
                     *
                     * Casting from a C++ interface to a non-C++ interface
                     * always results in null because there's no way one
                     * can be derived from the other.
                     */
                    e = el_bin(OPcomma, TYnptr, e, el_long(TYnptr, 0));
                    goto Lret;
                }
                if (cdfrom->isInterfaceDeclaration())
                {
                    rtl = RTLSYM_INTERFACE_CAST;
                }
                if (cdto->isBaseOf(cdfrom, &offset) && offset != OFFSET_RUNTIME)
                {
                    /* The offset from cdfrom=>cdto is known at compile time.
                     */

                    //printf("offset = %d\n", offset);
                    if (offset)
                    {
                        /* Rewrite cast as (e ? e + offset : null)
                         */
                        if (ce->e1->op == TOKthis)
                        {
                            // Assume 'this' is never null, so skip null check
                            e = el_bin(OPadd, TYnptr, e, el_long(TYsize_t, offset));
                        }
                        else
                        {
                            elem *etmp = el_same(&e);
                            elem *ex = el_bin(OPadd, TYnptr, etmp, el_long(TYsize_t, offset));
                            ex = el_bin(OPcolon, TYnptr, ex, el_long(TYnptr, 0));
                            e = el_bin(OPcond, TYnptr, e, ex);
                        }
                    }
                    goto Lret;                  // no-op
                }

                /* The offset from cdfrom=>cdto can only be determined at runtime.
                 */
                elem *ep = el_param(el_ptr(cdto->toSymbol()), e);
                e = el_bin(OPcall, TYnptr, el_var(rtlsym[rtl]), ep);
                goto Lret;
            }

            if (fty == Tvector && tty == Tsarray)
            {
                if (tfrom->size() == t->size())
                    goto Lret;
            }

            ftym = tybasic(e->Ety);
            ttym = tybasic(totym(t));
            if (ftym == ttym)
                goto Lret;

            /* Reduce combinatorial explosion by rewriting the 'to' and 'from' types to a
             * generic equivalent (as far as casting goes)
             */
            switch (tty)
            {
                case Tpointer:
                    if (fty == Tdelegate)
                        goto Lpaint;
                    tty = I64 ? Tuns64 : Tuns32;
                    break;

                case Tchar:     tty = Tuns8;    break;
                case Twchar:    tty = Tuns16;   break;
                case Tdchar:    tty = Tuns32;   break;
                case Tvoid:     goto Lpaint;

                case Tbool:
                {
                    // Construct e?true:false
                    e = el_una(OPbool, ttym, e);
                    goto Lret;
                }
            }

            switch (fty)
            {
                case Tpointer:  fty = I64 ? Tuns64 : Tuns32;  break;
                case Tchar:     fty = Tuns8;    break;
                case Twchar:    fty = Tuns16;   break;
                case Tdchar:    fty = Tuns32;   break;
            }

            #define X(fty, tty) ((fty) * TMAX + (tty))
        Lagain:
            switch (X(fty,tty))
            {
                /* ============================= */

                case X(Tbool,Tint8):
                case X(Tbool,Tuns8):
                                        goto Lpaint;
                case X(Tbool,Tint16):
                case X(Tbool,Tuns16):
                case X(Tbool,Tint32):
                case X(Tbool,Tuns32):   eop = OPu8_16;  goto Leop;
                case X(Tbool,Tint64):
                case X(Tbool,Tuns64):
                case X(Tbool,Tfloat32):
                case X(Tbool,Tfloat64):
                case X(Tbool,Tfloat80):
                case X(Tbool,Tcomplex32):
                case X(Tbool,Tcomplex64):
                case X(Tbool,Tcomplex80):
                                        e = el_una(OPu8_16, TYuint, e);
                                        fty = Tuns32;
                                        goto Lagain;
                case X(Tbool,Timaginary32):
                case X(Tbool,Timaginary64):
                case X(Tbool,Timaginary80): goto Lzero;

                /* ============================= */

                case X(Tint8,Tuns8):    goto Lpaint;
                case X(Tint8,Tint16):
                case X(Tint8,Tuns16):
                case X(Tint8,Tint32):
                case X(Tint8,Tuns32):   eop = OPs8_16;  goto Leop;
                case X(Tint8,Tint64):
                case X(Tint8,Tuns64):
                case X(Tint8,Tfloat32):
                case X(Tint8,Tfloat64):
                case X(Tint8,Tfloat80):
                case X(Tint8,Tcomplex32):
                case X(Tint8,Tcomplex64):
                case X(Tint8,Tcomplex80):
                                        e = el_una(OPs8_16, TYint, e);
                                        fty = Tint32;
                                        goto Lagain;
                case X(Tint8,Timaginary32):
                case X(Tint8,Timaginary64):
                case X(Tint8,Timaginary80): goto Lzero;

                /* ============================= */

                case X(Tuns8,Tint8):    goto Lpaint;
                case X(Tuns8,Tint16):
                case X(Tuns8,Tuns16):
                case X(Tuns8,Tint32):
                case X(Tuns8,Tuns32):   eop = OPu8_16;  goto Leop;
                case X(Tuns8,Tint64):
                case X(Tuns8,Tuns64):
                case X(Tuns8,Tfloat32):
                case X(Tuns8,Tfloat64):
                case X(Tuns8,Tfloat80):
                case X(Tuns8,Tcomplex32):
                case X(Tuns8,Tcomplex64):
                case X(Tuns8,Tcomplex80):
                                        e = el_una(OPu8_16, TYuint, e);
                                        fty = Tuns32;
                                        goto Lagain;
                case X(Tuns8,Timaginary32):
                case X(Tuns8,Timaginary64):
                case X(Tuns8,Timaginary80): goto Lzero;

                /* ============================= */

                case X(Tint16,Tint8):
                case X(Tint16,Tuns8):   eop = OP16_8;   goto Leop;
                case X(Tint16,Tuns16):  goto Lpaint;
                case X(Tint16,Tint32):
                case X(Tint16,Tuns32):  eop = OPs16_32; goto Leop;
                case X(Tint16,Tint64):
                case X(Tint16,Tuns64):  e = el_una(OPs16_32, TYint, e);
                                        fty = Tint32;
                                        goto Lagain;
                case X(Tint16,Tfloat32):
                case X(Tint16,Tfloat64):
                case X(Tint16,Tfloat80):
                case X(Tint16,Tcomplex32):
                case X(Tint16,Tcomplex64):
                case X(Tint16,Tcomplex80):
                                        e = el_una(OPs16_d, TYdouble, e);
                                        fty = Tfloat64;
                                        goto Lagain;
                case X(Tint16,Timaginary32):
                case X(Tint16,Timaginary64):
                case X(Tint16,Timaginary80): goto Lzero;

                /* ============================= */

                case X(Tuns16,Tint8):
                case X(Tuns16,Tuns8):   eop = OP16_8;   goto Leop;
                case X(Tuns16,Tint16):  goto Lpaint;
                case X(Tuns16,Tint32):
                case X(Tuns16,Tuns32):  eop = OPu16_32; goto Leop;
                case X(Tuns16,Tint64):
                case X(Tuns16,Tuns64):
                case X(Tuns16,Tfloat64):
                case X(Tuns16,Tfloat32):
                case X(Tuns16,Tfloat80):
                case X(Tuns16,Tcomplex32):
                case X(Tuns16,Tcomplex64):
                case X(Tuns16,Tcomplex80):
                                        e = el_una(OPu16_32, TYuint, e);
                                        fty = Tuns32;
                                        goto Lagain;
                case X(Tuns16,Timaginary32):
                case X(Tuns16,Timaginary64):
                case X(Tuns16,Timaginary80): goto Lzero;

                /* ============================= */

                case X(Tint32,Tint8):
                case X(Tint32,Tuns8):   e = el_una(OP32_16, TYshort, e);
                                        fty = Tint16;
                                        goto Lagain;
                case X(Tint32,Tint16):
                case X(Tint32,Tuns16):  eop = OP32_16;  goto Leop;
                case X(Tint32,Tuns32):  goto Lpaint;
                case X(Tint32,Tint64):
                case X(Tint32,Tuns64):  eop = OPs32_64; goto Leop;
                case X(Tint32,Tfloat32):
                case X(Tint32,Tfloat64):
                case X(Tint32,Tfloat80):
                case X(Tint32,Tcomplex32):
                case X(Tint32,Tcomplex64):
                case X(Tint32,Tcomplex80):
                                        e = el_una(OPs32_d, TYdouble, e);
                                        fty = Tfloat64;
                                        goto Lagain;
                case X(Tint32,Timaginary32):
                case X(Tint32,Timaginary64):
                case X(Tint32,Timaginary80): goto Lzero;

                /* ============================= */

                case X(Tuns32,Tint8):
                case X(Tuns32,Tuns8):   e = el_una(OP32_16, TYshort, e);
                                        fty = Tuns16;
                                        goto Lagain;
                case X(Tuns32,Tint16):
                case X(Tuns32,Tuns16):  eop = OP32_16;  goto Leop;
                case X(Tuns32,Tint32):  goto Lpaint;
                case X(Tuns32,Tint64):
                case X(Tuns32,Tuns64):  eop = OPu32_64; goto Leop;
                case X(Tuns32,Tfloat32):
                case X(Tuns32,Tfloat64):
                case X(Tuns32,Tfloat80):
                case X(Tuns32,Tcomplex32):
                case X(Tuns32,Tcomplex64):
                case X(Tuns32,Tcomplex80):
                                        e = el_una(OPu32_d, TYdouble, e);
                                        fty = Tfloat64;
                                        goto Lagain;
                case X(Tuns32,Timaginary32):
                case X(Tuns32,Timaginary64):
                case X(Tuns32,Timaginary80): goto Lzero;

                /* ============================= */

                case X(Tint64,Tint8):
                case X(Tint64,Tuns8):
                case X(Tint64,Tint16):
                case X(Tint64,Tuns16):  e = el_una(OP64_32, TYint, e);
                                        fty = Tint32;
                                        goto Lagain;
                case X(Tint64,Tint32):
                case X(Tint64,Tuns32):  eop = OP64_32; goto Leop;
                case X(Tint64,Tuns64):  goto Lpaint;
                case X(Tint64,Tfloat32):
                case X(Tint64,Tfloat64):
                case X(Tint64,Tfloat80):
                case X(Tint64,Tcomplex32):
                case X(Tint64,Tcomplex64):
                case X(Tint64,Tcomplex80):
                                        e = el_una(OPs64_d, TYdouble, e);
                                        fty = Tfloat64;
                                        goto Lagain;
                case X(Tint64,Timaginary32):
                case X(Tint64,Timaginary64):
                case X(Tint64,Timaginary80): goto Lzero;

                /* ============================= */

                case X(Tuns64,Tint8):
                case X(Tuns64,Tuns8):
                case X(Tuns64,Tint16):
                case X(Tuns64,Tuns16):  e = el_una(OP64_32, TYint, e);
                                        fty = Tint32;
                                        goto Lagain;
                case X(Tuns64,Tint32):
                case X(Tuns64,Tuns32):  eop = OP64_32;  goto Leop;
                case X(Tuns64,Tint64):  goto Lpaint;
                case X(Tuns64,Tfloat32):
                case X(Tuns64,Tfloat64):
                case X(Tuns64,Tfloat80):
                case X(Tuns64,Tcomplex32):
                case X(Tuns64,Tcomplex64):
                case X(Tuns64,Tcomplex80):
                                         e = el_una(OPu64_d, TYdouble, e);
                                         fty = Tfloat64;
                                         goto Lagain;
                case X(Tuns64,Timaginary32):
                case X(Tuns64,Timaginary64):
                case X(Tuns64,Timaginary80): goto Lzero;

                /* ============================= */

                case X(Tfloat32,Tint8):
                case X(Tfloat32,Tuns8):
                case X(Tfloat32,Tint16):
                case X(Tfloat32,Tuns16):
                case X(Tfloat32,Tint32):
                case X(Tfloat32,Tuns32):
                case X(Tfloat32,Tint64):
                case X(Tfloat32,Tuns64):
                case X(Tfloat32,Tfloat80): e = el_una(OPf_d, TYdouble, e);
                                           fty = Tfloat64;
                                           goto Lagain;
                case X(Tfloat32,Tfloat64): eop = OPf_d; goto Leop;
                case X(Tfloat32,Timaginary32):
                case X(Tfloat32,Timaginary64):
                case X(Tfloat32,Timaginary80): goto Lzero;
                case X(Tfloat32,Tcomplex32):
                case X(Tfloat32,Tcomplex64):
                case X(Tfloat32,Tcomplex80):
                    e = el_bin(OPadd,TYcfloat,el_long(TYifloat,0),e);
                    fty = Tcomplex32;
                    goto Lagain;

                /* ============================= */

                case X(Tfloat64,Tint8):
                case X(Tfloat64,Tuns8):    e = el_una(OPd_s16, TYshort, e);
                                           fty = Tint16;
                                           goto Lagain;
                case X(Tfloat64,Tint16):   eop = OPd_s16; goto Leop;
                case X(Tfloat64,Tuns16):   eop = OPd_u16; goto Leop;
                case X(Tfloat64,Tint32):   eop = OPd_s32; goto Leop;
                case X(Tfloat64,Tuns32):   eop = OPd_u32; goto Leop;
                case X(Tfloat64,Tint64):   eop = OPd_s64; goto Leop;
                case X(Tfloat64,Tuns64):   eop = OPd_u64; goto Leop;
                case X(Tfloat64,Tfloat32): eop = OPd_f;   goto Leop;
                case X(Tfloat64,Tfloat80): eop = OPd_ld;  goto Leop;
                case X(Tfloat64,Timaginary32):
                case X(Tfloat64,Timaginary64):
                case X(Tfloat64,Timaginary80):  goto Lzero;
                case X(Tfloat64,Tcomplex32):
                case X(Tfloat64,Tcomplex64):
                case X(Tfloat64,Tcomplex80):
                    e = el_bin(OPadd,TYcfloat,el_long(TYidouble,0),e);
                    fty = Tcomplex64;
                    goto Lagain;

                /* ============================= */

                case X(Tfloat80,Tint8):
                case X(Tfloat80,Tuns8):
                case X(Tfloat80,Tint16):
                case X(Tfloat80,Tuns16):
                case X(Tfloat80,Tint32):
                case X(Tfloat80,Tuns32):
                case X(Tfloat80,Tint64):
                case X(Tfloat80,Tfloat32): e = el_una(OPld_d, TYdouble, e);
                                           fty = Tfloat64;
                                           goto Lagain;
                case X(Tfloat80,Tuns64):
                                           eop = OPld_u64; goto Leop;
                case X(Tfloat80,Tfloat64): eop = OPld_d; goto Leop;
                case X(Tfloat80,Timaginary32):
                case X(Tfloat80,Timaginary64):
                case X(Tfloat80,Timaginary80): goto Lzero;
                case X(Tfloat80,Tcomplex32):
                case X(Tfloat80,Tcomplex64):
                case X(Tfloat80,Tcomplex80):
                    e = el_bin(OPadd,TYcldouble,e,el_long(TYildouble,0));
                    fty = Tcomplex80;
                    goto Lagain;

                /* ============================= */

                case X(Timaginary32,Tint8):
                case X(Timaginary32,Tuns8):
                case X(Timaginary32,Tint16):
                case X(Timaginary32,Tuns16):
                case X(Timaginary32,Tint32):
                case X(Timaginary32,Tuns32):
                case X(Timaginary32,Tint64):
                case X(Timaginary32,Tuns64):
                case X(Timaginary32,Tfloat32):
                case X(Timaginary32,Tfloat64):
                case X(Timaginary32,Tfloat80):  goto Lzero;
                case X(Timaginary32,Timaginary64): eop = OPf_d; goto Leop;
                case X(Timaginary32,Timaginary80):
                                           e = el_una(OPf_d, TYidouble, e);
                                           fty = Timaginary64;
                                           goto Lagain;
                case X(Timaginary32,Tcomplex32):
                case X(Timaginary32,Tcomplex64):
                case X(Timaginary32,Tcomplex80):
                    e = el_bin(OPadd,TYcfloat,el_long(TYfloat,0),e);
                    fty = Tcomplex32;
                    goto Lagain;

                /* ============================= */

                case X(Timaginary64,Tint8):
                case X(Timaginary64,Tuns8):
                case X(Timaginary64,Tint16):
                case X(Timaginary64,Tuns16):
                case X(Timaginary64,Tint32):
                case X(Timaginary64,Tuns32):
                case X(Timaginary64,Tint64):
                case X(Timaginary64,Tuns64):
                case X(Timaginary64,Tfloat32):
                case X(Timaginary64,Tfloat64):
                case X(Timaginary64,Tfloat80):  goto Lzero;
                case X(Timaginary64,Timaginary32): eop = OPd_f;   goto Leop;
                case X(Timaginary64,Timaginary80): eop = OPd_ld;  goto Leop;
                case X(Timaginary64,Tcomplex32):
                case X(Timaginary64,Tcomplex64):
                case X(Timaginary64,Tcomplex80):
                    e = el_bin(OPadd,TYcdouble,el_long(TYdouble,0),e);
                    fty = Tcomplex64;
                    goto Lagain;

                /* ============================= */

                case X(Timaginary80,Tint8):
                case X(Timaginary80,Tuns8):
                case X(Timaginary80,Tint16):
                case X(Timaginary80,Tuns16):
                case X(Timaginary80,Tint32):
                case X(Timaginary80,Tuns32):
                case X(Timaginary80,Tint64):
                case X(Timaginary80,Tuns64):
                case X(Timaginary80,Tfloat32):
                case X(Timaginary80,Tfloat64):
                case X(Timaginary80,Tfloat80):  goto Lzero;
                case X(Timaginary80,Timaginary32): e = el_una(OPld_d, TYidouble, e);
                                           fty = Timaginary64;
                                           goto Lagain;
                case X(Timaginary80,Timaginary64): eop = OPld_d; goto Leop;
                case X(Timaginary80,Tcomplex32):
                case X(Timaginary80,Tcomplex64):
                case X(Timaginary80,Tcomplex80):
                    e = el_bin(OPadd,TYcldouble,el_long(TYldouble,0),e);
                    fty = Tcomplex80;
                    goto Lagain;

                /* ============================= */

                case X(Tcomplex32,Tint8):
                case X(Tcomplex32,Tuns8):
                case X(Tcomplex32,Tint16):
                case X(Tcomplex32,Tuns16):
                case X(Tcomplex32,Tint32):
                case X(Tcomplex32,Tuns32):
                case X(Tcomplex32,Tint64):
                case X(Tcomplex32,Tuns64):
                case X(Tcomplex32,Tfloat32):
                case X(Tcomplex32,Tfloat64):
                case X(Tcomplex32,Tfloat80):
                        e = el_una(OPc_r, TYfloat, e);
                        fty = Tfloat32;
                        goto Lagain;
                case X(Tcomplex32,Timaginary32):
                case X(Tcomplex32,Timaginary64):
                case X(Tcomplex32,Timaginary80):
                        e = el_una(OPc_i, TYifloat, e);
                        fty = Timaginary32;
                        goto Lagain;
                case X(Tcomplex32,Tcomplex64):
                case X(Tcomplex32,Tcomplex80):
                        e = el_una(OPf_d, TYcdouble, e);
                        fty = Tcomplex64;
                        goto Lagain;

                /* ============================= */

                case X(Tcomplex64,Tint8):
                case X(Tcomplex64,Tuns8):
                case X(Tcomplex64,Tint16):
                case X(Tcomplex64,Tuns16):
                case X(Tcomplex64,Tint32):
                case X(Tcomplex64,Tuns32):
                case X(Tcomplex64,Tint64):
                case X(Tcomplex64,Tuns64):
                case X(Tcomplex64,Tfloat32):
                case X(Tcomplex64,Tfloat64):
                case X(Tcomplex64,Tfloat80):
                        e = el_una(OPc_r, TYdouble, e);
                        fty = Tfloat64;
                        goto Lagain;
                case X(Tcomplex64,Timaginary32):
                case X(Tcomplex64,Timaginary64):
                case X(Tcomplex64,Timaginary80):
                        e = el_una(OPc_i, TYidouble, e);
                        fty = Timaginary64;
                        goto Lagain;
                case X(Tcomplex64,Tcomplex32):   eop = OPd_f;   goto Leop;
                case X(Tcomplex64,Tcomplex80):   eop = OPd_ld;  goto Leop;

                /* ============================= */

                case X(Tcomplex80,Tint8):
                case X(Tcomplex80,Tuns8):
                case X(Tcomplex80,Tint16):
                case X(Tcomplex80,Tuns16):
                case X(Tcomplex80,Tint32):
                case X(Tcomplex80,Tuns32):
                case X(Tcomplex80,Tint64):
                case X(Tcomplex80,Tuns64):
                case X(Tcomplex80,Tfloat32):
                case X(Tcomplex80,Tfloat64):
                case X(Tcomplex80,Tfloat80):
                        e = el_una(OPc_r, TYldouble, e);
                        fty = Tfloat80;
                        goto Lagain;
                case X(Tcomplex80,Timaginary32):
                case X(Tcomplex80,Timaginary64):
                case X(Tcomplex80,Timaginary80):
                        e = el_una(OPc_i, TYildouble, e);
                        fty = Timaginary80;
                        goto Lagain;
                case X(Tcomplex80,Tcomplex32):
                case X(Tcomplex80,Tcomplex64):
                        e = el_una(OPld_d, TYcdouble, e);
                        fty = Tcomplex64;
                        goto Lagain;

                case X(Tnull, Tarray):
                case X(Tnull, Taarray):
                case X(Tnull, Tdelegate):
                    goto Lzero;

                /* ============================= */

                default:
                    if (fty == tty)
                        goto Lpaint;
                    //dump(0);
                    //printf("fty = %d, tty = %d, %d\n", fty, tty, t->ty);
                    // This error should really be pushed to the front end
                    ce->error("e2ir: cannot cast %s of type %s to type %s", ce->e1->toChars(), ce->e1->type->toChars(), t->toChars());
                    e = el_long(TYint, 0);
                    result = e;
                    return;

                Lzero:
                    e = el_bin(OPcomma, ttym, e, el_long(ttym, 0));
                    break;

                Lpaint:
                    e->Ety = ttym;
                    break;

                Leop:
                    e = el_una(eop, ttym, e);
                    break;
            }
        Lret:
            // Adjust for any type paints
            t = ce->type->toBasetype();
            e->Ety = totym(t);

            el_setLoc(e, ce->loc);
            result = e;
        }

        void visit(ArrayLengthExp *ale)
        {
            elem *e = ale->e1->toElem(irs);
            e = el_una(I64 ? OP128_64 : OP64_32, totym(ale->type), e);
            el_setLoc(e, ale->loc);
            result = e;
        }

        void visit(SliceExp *se)
        {
            //printf("SliceExp::toElem()\n");
            Type *tb = se->type->toBasetype();
            assert(tb->ty == Tarray || tb->ty == Tsarray && se->lwr);
            Type *t1 = se->e1->type->toBasetype();
            elem *e = se->e1->toElem(irs);
            if (se->lwr)
            {
                elem *einit = resolveLengthVar(se->lengthVar, &e, t1);

                unsigned sz = t1->nextOf()->size();

                elem *elwr = se->lwr->toElem(irs);
                elem *eupr = se->upr->toElem(irs);

                elem *elwr2 = el_same(&elwr);

                // Create an array reference where:
                // length is (upr - lwr)
                // pointer is (ptr + lwr*sz)
                // Combine as (length pair ptr)

                if (irs->arrayBoundsCheck())
                {
                    // Checks (unsigned compares):
                    //  upr <= array.length
                    //  lwr <= upr

                    elem *c1;
                    elem *c2;
                    elem *eupr2;
                    elem *elength;

                    if (t1->ty == Tpointer)
                    {
                        // Just do lwr <= upr check

                        eupr2 = el_same(&eupr);
                        eupr2->Ety = TYsize_t;                    // make sure unsigned comparison
                        c1 = el_bin(OPle, TYint, elwr2, eupr2);
                        c1 = el_combine(eupr, c1);
                        goto L2;
                    }
                    else if (t1->ty == Tsarray)
                    {
                        TypeSArray *tsa = (TypeSArray *)t1;
                        dinteger_t length = tsa->dim->toInteger();

                        elength = el_long(TYsize_t, length);
                        goto L1;
                    }
                    else if (t1->ty == Tarray)
                    {
                        if (se->lengthVar && !(se->lengthVar->storage_class & STCconst))
                            elength = el_var(se->lengthVar->toSymbol());
                        else
                        {
                            elength = e;
                            e = el_same(&elength);
                            elength = el_una(I64 ? OP128_64 : OP64_32, TYsize_t, elength);
                        }
                    L1:
                        eupr2 = el_same(&eupr);
                        c1 = el_bin(OPle, TYint, eupr, elength);
                        eupr2->Ety = TYsize_t;                    // make sure unsigned comparison
                        c2 = el_bin(OPle, TYint, elwr2, eupr2);
                        c1 = el_bin(OPandand, TYint, c1, c2);   // (c1 && c2)

                    L2:
                        // Construct: (c1 || ModuleArray(line))
                        Symbol *sassert = irs->blx->module->toModuleArray();
                        elem *ea = el_bin(OPcall,TYvoid,el_var(sassert), el_long(TYint, se->loc.linnum));
                        elem *eb = el_bin(OPoror,TYvoid,c1,ea);
                        elwr = el_combine(elwr, eb);

                        elwr2 = el_copytree(elwr2);
                        eupr = el_copytree(eupr2);
                    }
                }

                elem *eptr = array_toPtr(se->e1->type, e);

                elem *elength = el_bin(OPmin, TYsize_t, eupr, elwr2);
                eptr = el_bin(OPadd, TYnptr, eptr, el_bin(OPmul, TYsize_t, el_copytree(elwr2), el_long(TYsize_t, sz)));

                if (tb->ty == Tarray)
                    e = el_pair(TYdarray, elength, eptr);
                else
                {
                    assert(tb->ty == Tsarray);
                    e = el_una(OPind, totym(se->type), eptr);
                    if (tybasic(e->Ety) == TYstruct)
                        e->ET = Type_toCtype(se->type);
                }
                e = el_combine(elwr, e);
                e = el_combine(einit, e);
            }
            else if (t1->ty == Tsarray)
            {
                e = sarray_toDarray(se->loc, t1, NULL, e);
            }
            el_setLoc(e, se->loc);
            result = e;
        }

        void visit(IndexExp *ie)
        {
            elem *e;
            elem *n1 = ie->e1->toElem(irs);
            elem *eb = NULL;

            //printf("IndexExp::toElem() %s\n", ie->toChars());
            Type *t1 = ie->e1->type->toBasetype();
            if (t1->ty == Taarray)
            {
                // set to:
                //      *aaGetX(aa, keyti, valuesize, &key);

                TypeAArray *taa = (TypeAArray *)t1;
                unsigned vsize = taa->next->size();

                // n2 becomes the index, also known as the key
                elem *n2 = ie->e2->toElem(irs);

                /* Turn n2 into a pointer to the index.  If it's an lvalue,
                 * take the address of it. If not, copy it to a temp and
                 * take the address of that.
                 */
                n2 = addressElem(n2, taa->index);

                elem *valuesize = el_long(TYsize_t, vsize);
                //printf("valuesize: "); elem_print(valuesize);
                Symbol *s;
                if (ie->modifiable)
                {
                    n1 = el_una(OPaddr, TYnptr, n1);
                    s = taa->aaGetSymbol("GetX", 1);
                }
                else
                {
                    s = taa->aaGetSymbol("GetRvalueX", 1);
                }
                //printf("taa->index = %s\n", taa->index->toChars());
                elem* keyti = taa->index->getInternalTypeInfo(NULL)->toElem(irs);
                //keyti = taa->index->getTypeInfo(NULL)->toElem(irs);
                //printf("keyti:\n");
                //elem_print(keyti);
                elem* ep = el_params(n2, valuesize, keyti, n1, NULL);
                e = el_bin(OPcall, TYnptr, el_var(s), ep);
                if (irs->arrayBoundsCheck())
                {
                    elem *n = el_same(&e);

                    // Construct: ((e || ModuleAssert(line)),n)
                    Symbol *sassert = irs->blx->module->toModuleArray();
                    elem *ea = el_bin(OPcall,TYvoid,el_var(sassert),
                        el_long(TYint, ie->loc.linnum));
                    e = el_bin(OPoror,TYvoid,e,ea);
                    e = el_bin(OPcomma, TYnptr, e, n);
                }
                e = el_una(OPind, totym(ie->type), e);
                if (tybasic(e->Ety) == TYstruct)
                    e->ET = Type_toCtype(ie->type);
            }
            else
            {
                elem *einit = resolveLengthVar(ie->lengthVar, &n1, t1);
                elem *n2 = ie->e2->toElem(irs);

                if (irs->arrayBoundsCheck() && !ie->skipboundscheck)
                {
                    elem *elength;

                    if (t1->ty == Tsarray)
                    {
                        TypeSArray *tsa = (TypeSArray *)t1;
                        dinteger_t length = tsa->dim->toInteger();

                        elength = el_long(TYsize_t, length);
                        goto L1;
                    }
                    else if (t1->ty == Tarray)
                    {
                        elength = n1;
                        n1 = el_same(&elength);
                        elength = el_una(I64 ? OP128_64 : OP64_32, TYsize_t, elength);
                    L1:
                        elem *n2x = n2;
                        n2 = el_same(&n2x);
                        n2x = el_bin(OPlt, TYint, n2x, elength);

                        // Construct: (n2x || ModuleAssert(line))
                        Symbol *sassert = irs->blx->module->toModuleArray();
                        elem *ea = el_bin(OPcall,TYvoid,el_var(sassert),
                            el_long(TYint, ie->loc.linnum));
                        eb = el_bin(OPoror,TYvoid,n2x,ea);
                    }
                }

                n1 = array_toPtr(t1, n1);

                {
                    elem *escale = el_long(TYsize_t, t1->nextOf()->size());
                    n2 = el_bin(OPmul, TYsize_t, n2, escale);
                    e = el_bin(OPadd, TYnptr, n1, n2);
                    e = el_una(OPind, totym(ie->type), e);
                    if (tybasic(e->Ety) == TYstruct || tybasic(e->Ety) == TYarray)
                    {
                        e->Ety = TYstruct;
                        e->ET = Type_toCtype(ie->type);
                    }
                }

                eb = el_combine(einit, eb);
                e = el_combine(eb, e);
            }
            el_setLoc(e, ie->loc);
            result = e;
        }


        void visit(TupleExp *te)
        {
            //printf("TupleExp::toElem() %s\n", te->toChars());
            elem *e = NULL;
            if (te->e0)
                e = te->e0->toElem(irs);
            for (size_t i = 0; i < te->exps->dim; i++)
            {
                Expression *el = (*te->exps)[i];
                elem *ep = el->toElem(irs);
                e = el_combine(e, ep);
            }
            result = e;
        }

        static elem *tree_insert(Elems *args, size_t low, size_t high)
        {
            assert(low < high);
            if (low + 1 == high)
                return (*args)[low];
            int mid = (low + high) >> 1;
            return el_param(tree_insert(args, low, mid),
                            tree_insert(args, mid, high));
        }

        void visit(ArrayLiteralExp *ale)
        {
            elem *e;
            size_t dim;

            //printf("ArrayLiteralExp::toElem() %s, type = %s\n", ale->toChars(), ale->type->toChars());
            Type *tb = ale->type->toBasetype();
            if (tb->ty == Tsarray && tb->nextOf()->toBasetype()->ty == Tvoid)
            {
                // Convert void[n] to ubyte[n]
                tb = Type::tuns8->sarrayOf(((TypeSArray *)tb)->dim->toUInteger());
            }
            if (tb->ty == Tsarray && ale->elements && ale->elements->dim)
            {
                Symbol *sdata;
                e = ExpressionsToStaticArray(ale->loc, ale->elements, &sdata);
                e = el_combine(e, el_ptr(sdata));
            }
            else if (ale->elements)
            {
                /* Instead of passing the initializers on the stack, allocate the
                 * array and assign the members inline.
                 * Avoids the whole variadic arg mess.
                 */
                dim = ale->elements->dim;
                Elems args;
                args.setDim(dim);           // +1 for number of args parameter
                e = el_long(TYsize_t, dim);
                e = el_param(e, ale->type->getTypeInfo(NULL)->toElem(irs));
                // call _d_arrayliteralTX(ti, dim)
                e = el_bin(OPcall,TYnptr,el_var(rtlsym[RTLSYM_ARRAYLITERALTX]),e);
                Symbol *stmp = symbol_genauto(Type_toCtype(Type::tvoid->pointerTo()));
                e = el_bin(OPeq,TYnptr,el_var(stmp),e);

                targ_size_t sz = tb->nextOf()->size();      // element size
                ::type *te = Type_toCtype(tb->nextOf());       // element type
                for (size_t i = 0; i < dim; i++)
                {
                    Expression *el = (*ale->elements)[i];
                    /* Generate: *(stmp + i * sz) = element[i]
                     */
                    elem *ep = el->toElem(irs);
                    elem *ev = el_var(stmp);
                    ev = el_bin(OPadd, TYnptr, ev, el_long(TYsize_t, i * sz));
                    ev = el_una(OPind, te->Tty, ev);
                    elem *eeq = el_bin(OPeq,te->Tty,ev,ep);

                    if (tybasic(te->Tty) == TYstruct)
                    {
                        eeq->Eoper = OPstreq;
                        eeq->ET = te;
                    }
                    else if (tybasic(te->Tty) == TYarray)
                    {
                        eeq->Eoper = OPstreq;
                        eeq->Ejty = eeq->Ety = TYstruct;
                        eeq->ET = te;
                    }
                    args[i] = eeq;
                }
                e = el_combine(e, el_combines((void **)args.tdata(), dim));
                e = el_combine(e, el_var(stmp));
            }
            else
            {
                dim = 0;
                e = el_long(TYsize_t, 0);
            }
            if (tb->ty == Tarray)
            {
                e = el_pair(TYdarray, el_long(TYsize_t, dim), e);
            }
            else if (tb->ty == Tpointer)
            {
            }
            else
            {
                e = el_una(OPind,TYstruct,e);
                e->ET = Type_toCtype(ale->type);
            }

            el_setLoc(e, ale->loc);
            result = e;
        }

        /**************************************
         * Mirrors logic in Dsymbol_canThrow().
         */

        elem *Dsymbol_toElem(Dsymbol *s)
        {
            elem *e = NULL;

            //printf("Dsymbol_toElem() %s\n", s->toChars());
            if (AttribDeclaration *ad = s->isAttribDeclaration())
            {
                Dsymbols *decl = ad->include(NULL, NULL);
                if (decl && decl->dim)
                {
                    for (size_t i = 0; i < decl->dim; i++)
                    {
                        s = (*decl)[i];
                        e = el_combine(e, Dsymbol_toElem(s));
                    }
                }
            }
            else if (VarDeclaration *vd = s->isVarDeclaration())
            {
                s = s->toAlias();
                if (s != vd)
                    return Dsymbol_toElem(s);
                if (vd->storage_class & STCmanifest)
                    return NULL;
                else if (vd->isStatic() || vd->storage_class & (STCextern | STCtls | STCgshared))
                    vd->toObjFile(0);
                else
                {
                    Symbol *sp = s->toSymbol();
                    symbol_add(sp);
                    //printf("\tadding symbol '%s'\n", sp->Sident);
                    if (vd->init)
                    {
                        ExpInitializer *ie;

                        ie = vd->init->isExpInitializer();
                        if (ie)
                            e = ie->exp->toElem(irs);
                    }

                    /* Mark the point of construction of a variable that needs to be destructed.
                     */
                    if (vd->edtor && !vd->noscope)
                    {
                        e = el_dctor(e, vd);

                        // Put vd on list of things needing destruction
                        if (!irs->varsInScope)
                            irs->varsInScope = VarDeclarations_create();
                        irs->varsInScope->push(vd);
                    }
                }
            }
            else if (ClassDeclaration *cd = s->isClassDeclaration())
            {
                irs->deferToObj->push(s);
            }
            else if (StructDeclaration *sd = s->isStructDeclaration())
            {
                irs->deferToObj->push(sd);
            }
            else if (FuncDeclaration *fd = s->isFuncDeclaration())
            {
                //printf("function %s\n", fd->toChars());
                irs->deferToObj->push(fd);
            }
            else if (TemplateMixin *tm = s->isTemplateMixin())
            {
                //printf("%s\n", tm->toChars());
                if (tm->members)
                {
                    for (size_t i = 0; i < tm->members->dim; i++)
                    {
                        Dsymbol *sm = (*tm->members)[i];
                        e = el_combine(e, Dsymbol_toElem(sm));
                    }
                }
            }
            else if (TupleDeclaration *td = s->isTupleDeclaration())
            {
                for (size_t i = 0; i < td->objects->dim; i++)
                {   RootObject *o = (*td->objects)[i];
                    if (o->dyncast() == DYNCAST_EXPRESSION)
                    {   Expression *eo = (Expression *)o;
                        if (eo->op == TOKdsymbol)
                        {   DsymbolExp *se = (DsymbolExp *)eo;
                            e = el_combine(e, Dsymbol_toElem(se->s));
                        }
                    }
                }
            }
            else if (TypedefDeclaration *tyd = s->isTypedefDeclaration())
            {
                irs->deferToObj->push(tyd);
            }
            else if (EnumDeclaration *ed = s->isEnumDeclaration())
            {
                irs->deferToObj->push(ed);
            }
            return e;
        }

        /*************************************************
         * Allocate a static array, and initialize its members with
         * exps[].
         * Return the initialization expression, and the symbol for the static array in *psym.
         */
        elem *ExpressionsToStaticArray(Loc loc, Expressions *exps, symbol **psym)
        {
            // Create a static array of type telem[dim]
            size_t dim = exps->dim;

            Elems elems;
            elems.setDim(dim);

            Type *telem;
            Type *tsarray;
            symbol *stmp;
            targ_size_t szelem;
            ::type *te;      // stmp[] element type

            for (size_t i = 0; i < dim; i++)
            {
                Expression *el = (*exps)[i];
                if (i == 0)
                {
                    telem = el->type;
                    szelem = telem->size();
                    te = Type_toCtype(telem);

                    tsarray = telem->sarrayOf(dim);
                    stmp = symbol_genauto(Type_toCtype(tsarray));
                    *psym = stmp;
                }

                /* Generate: *(&stmp + i * szelem) = element[i]
                 */
                elem *ep = el->toElem(irs);
                elem *ev = el_ptr(stmp);
                ev = el_bin(OPadd, TYnptr, ev, el_long(TYsize_t, i * szelem));
                ev = el_una(OPind, te->Tty, ev);
                elem *eeq = el_bin(OPeq,te->Tty,ev,ep);

                if (tybasic(te->Tty) == TYstruct)
                {
                    eeq->Eoper = OPstreq;
                    eeq->ET = te;
                }
                else if (tybasic(te->Tty) == TYarray)
                {
                    eeq->Eoper = OPstreq;
                    eeq->Ejty = eeq->Ety = TYstruct;
                    eeq->ET = te;
                }
                elems[i] = eeq;
            }

            return el_combines((void **)elems.tdata(), dim);
        }

        void visit(AssocArrayLiteralExp *aale)
        {
            //printf("AssocArrayLiteralExp::toElem() %s\n", aale->toChars());

            Type *t = aale->type->toBasetype()->mutableOf();

            size_t dim = aale->keys->dim;
            if (dim)
            {
                // call _d_assocarrayliteralTX(TypeInfo_AssociativeArray ti, void[] keys, void[] values)
                // Prefer this to avoid the varargs fiasco in 64 bit code

                assert(t->ty == Taarray);
                Type *ta = t;

                symbol *skeys = NULL;
                elem *ekeys = ExpressionsToStaticArray(aale->loc, aale->keys, &skeys);

                symbol *svalues = NULL;
                elem *evalues = ExpressionsToStaticArray(aale->loc, aale->values, &svalues);

                elem *ev = el_pair(TYdarray, el_long(TYsize_t, dim), el_ptr(svalues));
                elem *ek = el_pair(TYdarray, el_long(TYsize_t, dim), el_ptr(skeys  ));
                if (config.exe == EX_WIN64)
                {
                    ev = addressElem(ev, Type::tvoid->arrayOf());
                    ek = addressElem(ek, Type::tvoid->arrayOf());
                }
                elem *e = el_params(ev, ek,
                                    ta->getTypeInfo(NULL)->toElem(irs),
                                    NULL);

                // call _d_assocarrayliteralTX(ti, keys, values)
                e = el_bin(OPcall,TYnptr,el_var(rtlsym[RTLSYM_ASSOCARRAYLITERALTX]),e);
                if (t != ta)
                    e = addressElem(e, ta);
                el_setLoc(e, aale->loc);

                e = el_combine(evalues, e);
                e = el_combine(ekeys, e);
                result = e;
                return;
            }
            else
            {
                elem *e = el_long(TYnptr, 0);      // empty associative array is the null pointer
                if (t->ty != Taarray)
                    e = addressElem(e, Type::tvoidptr);
                result = e;
                return;
            }
        }

        /*******************************************
         * Generate elem to zero fill contents of Symbol stmp
         * from *poffset..offset2.
         * May store anywhere from 0..maxoff, as this function
         * tries to use aligned int stores whereever possible.
         * Update *poffset to end of initialized hole; *poffset will be >= offset2.
         */

        static elem *fillHole(Symbol *stmp, size_t *poffset, size_t offset2, size_t maxoff)
        {
            elem *e = NULL;
            int basealign = 1;

            while (*poffset < offset2)
            {
                elem *e1;
                if (tybasic(stmp->Stype->Tty) == TYnptr)
                    e1 = el_var(stmp);
                else
                    e1 = el_ptr(stmp);
                if (basealign)
                    *poffset &= ~3;
                basealign = 1;
                size_t sz = maxoff - *poffset;
                tym_t ty;
                switch (sz)
                {
                    case 1: ty = TYchar;        break;
                    case 2: ty = TYshort;       break;
                    case 3:
                        ty = TYshort;
                        basealign = 0;
                        break;
                    default:
                        ty = TYlong;
                        break;
                }
                e1 = el_bin(OPadd, TYnptr, e1, el_long(TYsize_t, *poffset));
                e1 = el_una(OPind, ty, e1);
                e1 = el_bin(OPeq, ty, e1, el_long(ty, 0));
                e = el_combine(e, e1);
                *poffset += tysize[ty];
            }
            return e;
        }

        void visit(StructLiteralExp *sle)
        {
            //printf("StructLiteralExp::toElem() %s\n", sle->toChars());

            if (sle->sinit)
            {
                elem *e = el_var(sle->sinit);
                e->ET = Type_toCtype(sle->sd->type);
                el_setLoc(e, sle->loc);

                if (sle->sym)
                {
                    elem *ev = el_var(sle->sym);
                    if (tybasic(ev->Ety) == TYnptr)
                        ev = el_una(OPind, e->Ety, ev);
                    ev->ET = e->ET;
                    e = el_bin(OPstreq,e->Ety,ev,e);
                    e->ET = ev->ET;

                    //ev = el_var(sym);
                    //ev->ET = e->ET;
                    //e = el_combine(e, ev);
                    el_setLoc(e, sle->loc);
                }
                result = e;
                return;
            }

            // struct symbol to initialize with the literal
            Symbol *stmp = sle->sym ? sle->sym : symbol_genauto(Type_toCtype(sle->sd->type));

            elem *e = NULL;

            if (sle->fillHoles)
            {
                /* Initialize all alignment 'holes' to zero.
                 * Do before initializing fields, as the hole filling process
                 * can spill over into the fields.
                 */
                size_t offset = 0;
                for (size_t i = 0; i < sle->sd->fields.dim; i++)
                {
                    VarDeclaration *v = sle->sd->fields[i];

                    e = el_combine(e, fillHole(stmp, &offset, v->offset, sle->sd->structsize));
                    size_t vend = v->offset + v->type->size();
                    if (offset < vend)
                        offset = vend;
                }
                e = el_combine(e, fillHole(stmp, &offset, sle->sd->structsize, sle->sd->structsize));
            }

            size_t dim = sle->elements ? sle->elements->dim : 0;
            assert(dim <= sle->sd->fields.dim);
            // CTFE may fill the hidden pointer by NullExp.
            {
                for (size_t i = 0; i < dim; i++)
                {
                    Expression *el = (*sle->elements)[i];
                    if (!el)
                        continue;

                    VarDeclaration *v = sle->sd->fields[i];
                    assert(!v->isThisDeclaration() || el->op == TOKnull);

                    elem *e1;
                    if (tybasic(stmp->Stype->Tty) == TYnptr)
                    {
                        e1 = el_var(stmp);
                        e1->EV.sp.Voffset = sle->soffset;
                    }
                    else
                    {
                        e1 = el_ptr(stmp);
                        if (sle->soffset)
                            e1 = el_bin(OPadd, TYnptr, e1, el_long(TYsize_t, sle->soffset));
                    }
                    e1 = el_bin(OPadd, TYnptr, e1, el_long(TYsize_t, v->offset));
                    elem *ec = e1;                      // pointer to destination

                    elem *ep = el->toElem(irs);

                    Type *t1b = v->type->toBasetype();
                    Type *t2b = el->type->toBasetype();
                    if (t1b->ty == Tsarray)
                    {
                        if (t2b->implicitConvTo(t1b))
                        {
                            elem *esize = el_long(TYsize_t, t1b->size());
                            ep = array_toPtr(el->type, ep);
                            e1 = el_bin(OPmemcpy, TYnptr, e1, el_param(ep, esize));
                        }
                        else
                        {
                            elem *edim = el_long(TYsize_t, t1b->size() / t2b->size());
                            e1 = setArray(e1, edim, t2b, ep, irs, TOKconstruct);
                        }
                    }
                    else
                    {
                        tym_t ty = totym(v->type);
                        e1 = el_una(OPind, ty, e1);
                        if (tybasic(ty) == TYstruct)
                            e1->ET = Type_toCtype(v->type);
                        e1 = el_bin(OPeq, ty, e1, ep);
                        if (tybasic(ty) == TYstruct)
                        {
                            e1->Eoper = OPstreq;
                            e1->ET = Type_toCtype(v->type);
                        }
                    }
                    e = el_combine(e, e1);
                }
            }

            if (sle->sd->isNested() && dim != sle->sd->fields.dim)
            {
                // Initialize the hidden 'this' pointer
                assert(sle->sd->fields.dim);
                ThisDeclaration *v = sle->sd->fields[sle->sd->fields.dim - 1]->isThisDeclaration();

                elem *e1;
                if (tybasic(stmp->Stype->Tty) == TYnptr)
                {
                    e1 = el_var(stmp);
                    e1->EV.sp.Voffset = sle->soffset;
                }
                else
                {
                    e1 = el_ptr(stmp);
                    if (sle->soffset)
                        e1 = el_bin(OPadd, TYnptr, e1, el_long(TYsize_t, sle->soffset));
                }
                e1 = setEthis(sle->loc, irs, e1, sle->sd);

                e = el_combine(e, e1);
            }

            elem *ev = el_var(stmp);
            ev->ET = Type_toCtype(sle->sd->type);
            e = el_combine(e, ev);
            el_setLoc(e, sle->loc);
            result = e;
        }

        /*****************************************************/
        /*                   CTFE stuff                      */
        /*****************************************************/

        void visit(ClassReferenceExp *e)
        {
        #if 0
            printf("ClassReferenceExp::toElem() %p, value=%p, %s\n", e, e->value, e->toChars());
        #endif
            result = el_ptr(e->toSymbol());
        }

    };

    ToElemVisitor v(irs);
    e->accept(&v);
    return v.result;
}

/********************************************
 * Add destructors
 */

elem *appendDtors(IRState *irs, elem *er, size_t starti, size_t endi)
{
    //printf("appendDtors(%d .. %d)\n", starti, endi);

    /* Code gen can be improved by determining if no exceptions can be thrown
     * between the OPdctor and OPddtor, and eliminating the OPdctor and OPddtor.
     */

    /* Build edtors, an expression that calls destructors on all the variables
     * going out of the scope starti..endi
     */
    elem *edtors = NULL;
    for (size_t i = starti; i != endi; ++i)
    {
        VarDeclaration *vd = (*irs->varsInScope)[i];
        if (vd)
        {
            //printf("appending dtor\n");
            (*irs->varsInScope)[i] = NULL;
            elem *ed = vd->edtor->toElem(irs);
            ed = el_ddtor(ed, vd);
            edtors = el_combine(ed, edtors);    // execute in reverse order
        }
    }

    if (edtors)
    {
#if TARGET_WINDOS
        if (!global.params.is64bit)
        {
            Blockx *blx = irs->blx;
            nteh_declarvars(blx);
        }
#endif
        /* Append edtors to er, while preserving the value of er
         */
        if (tybasic(er->Ety) == TYvoid)
        {
            /* No value to preserve, so simply append
             */
            er = el_combine(er, edtors);
        }
        else
        {
            elem **pe;
            for (pe = &er; (*pe)->Eoper == OPcomma; pe = &(*pe)->E2)
                ;
            elem *erx = *pe;

            if (erx->Eoper == OPconst || erx->Eoper == OPrelconst)
            {
                *pe = el_combine(edtors, erx);
            }
            else if (tybasic(erx->Ety) == TYstruct || tybasic(erx->Ety) == TYarray)
            {
                /* Expensive to copy, to take a pointer to it instead
                 */
                elem *ep = el_una(OPaddr, TYnptr, erx);
                elem *e = el_same(&ep);
                ep = el_combine(ep, edtors);
                ep = el_combine(ep, e);
                e = el_una(OPind, erx->Ety, ep);
                e->ET = erx->ET;
                *pe = e;
            }
            else
            {
                elem *e = el_same(&erx);
                erx = el_combine(erx, edtors);
                *pe = el_combine(erx, e);
            }
        }
    }
    return er;
}

/*******************************************
 * Evaluate Expression, then call destructors on any temporaries in it.
 */

elem *toElemDtor(Expression *e, IRState *irs)
{
    //printf("Expression::toElemDtor() %s\n", toChars());
    size_t starti = irs->varsInScope ? irs->varsInScope->dim : 0;
    elem *er = e->toElem(irs);
    size_t endi = irs->varsInScope ? irs->varsInScope->dim : 0;

    // Add destructors
    er = appendDtors(irs, er, starti, endi);
    return er;
}
