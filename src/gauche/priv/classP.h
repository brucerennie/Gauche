/*
 * classP.h - Gauche object system private header
 *
 *   Copyright (c) 2000-2025  Shiro Kawai  <shiro@acm.org>
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *
 *   3. Neither the name of the authors nor the names of its contributors
 *      may be used to endorse or promote products derived from this
 *      software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 *   TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 *   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 *   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 *   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef GAUCHE_PRIV_CLASSP_H
#define GAUCHE_PRIV_CLASSP_H

/* Specialized constructor for records */
SCM_EXTERN ScmObj Scm__AllocateAndInitializeInstance(ScmClass *klass,
                                                     ScmObj *inits,
                                                     int numInits,
                                                     u_long flags);

/* Method dispatcher developer API */
SCM_EXTERN ScmObj Scm__GenericBuildDispatcher(ScmGeneric *gf, int axis);
SCM_EXTERN void   Scm__GenericInvalidateDispatcher(ScmGeneric *gf);
SCM_EXTERN ScmObj Scm__GenericDispatcherInfo(ScmGeneric *gf);
SCM_EXTERN void   Scm__GenericDispatcherDump(ScmGeneric *gf, ScmPort *port);


/* A proxy type is a class to hold a reference to another class.
   It is used to keep reference to a type in another compound type
   structure.  We need an indirection because a class may be redefined.
*/
struct ScmProxyTypeRec {
    SCM_HEADER;
    ScmIdentifier *id;          /* Original Id (need to serialize in
                                   precomp output. */
    ScmGloc *ref;               /* GLOC that holds the actual class.
                                   It can be NULL, if it is computed
                                   from ID lazily. */
};

#endif /*GAUCHE_PRIV_CLASSP_H*/
