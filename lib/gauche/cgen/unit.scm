;;;
;;; gauche.cgen.unit - cgen-unit
;;;
;;;   Copyright (c) 2004-2025  Shiro Kawai  <shiro@acm.org>
;;;
;;;   Redistribution and use in source and binary forms, with or without
;;;   modification, are permitted provided that the following conditions
;;;   are met:
;;;
;;;   1. Redistributions of source code must retain the above copyright
;;;      notice, this list of conditions and the following disclaimer.
;;;
;;;   2. Redistributions in binary form must reproduce the above copyright
;;;      notice, this list of conditions and the following disclaimer in the
;;;      documentation and/or other materials provided with the distribution.
;;;
;;;   3. Neither the name of the authors nor the names of its contributors
;;;      may be used to endorse or promote products derived from this
;;;      software without specific prior written permission.
;;;
;;;   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
;;;   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
;;;   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
;;;   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
;;;   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
;;;   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
;;;   TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
;;;   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
;;;   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
;;;   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
;;;   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
;;;

(define-module gauche.cgen.unit
  (use srfi.13)
  (use srfi.42)
  (use util.match)
  (use file.util)
  (use gauche.sequence)
  (export <cgen-unit> cgen-current-unit
          cgen-unit-c-file cgen-unit-init-name cgen-unit-h-file
          cgen-unit-toplevel-nodes cgen-add!
          cgen-emit-h cgen-emit-c

          <cgen-node> cgen-with-cpp-condition cgen-cpp-conditions cgen-emit
          cgen-emit-xtrn cgen-emit-decl cgen-emit-body cgen-emit-init
          cgen-extern cgen-decl cgen-body cgen-init
          cgen-include cgen-define cpp-condition->string

          cgen-safe-name cgen-safe-name-friendly
          cgen-safe-string cgen-safe-comment cgen-safe-comment-sexp

          ;; semi-private routines
          cgen-emit-static-data)
  )
(select-module gauche.cgen.unit)

;;=============================================================
;; Unit
;;

;; A 'cgen-unit' is the unit of C source.  It generates one .c file,
;; and optionally one .h file.
;; During the processing, a "current unit" is kept in a parameter
;; cgen-current-unit, and most cgen APIs implicitly work to it.

(define-class <cgen-unit> ()
  (;; public
   (name     :init-keyword :name   :init-value "cgen")
   (c-file   :init-keyword :c-file :init-value #f)
   (h-file   :init-keyword :h-file :init-value #f)
   (language :init-keyword :language :init-value 'c) ; c or c++
   (preamble :init-keyword :preamble
             :init-value '("/* Generated by gauche.cgen */"))
   (init-prologue :init-keyword :init-prologue :init-value #f)
   (init-epilogue :init-keyword :init-epilogue :init-value #f)
   ;; private
   (toplevels :init-value '())   ;; toplevel nodes to be realized
   (transients :init-value '())  ;; transient variables
   (literals  :init-form #f)     ;; literals. see gauche.cgen.literal
   (static-data-list :init-value '()) ;; static C data, see below
   (debug-info-bin :init-value #f)   ;; see gauche.vm.debug-info
   ))

;; A dummy instance to show a better error message  when cgen routine is
;; used without setting a proper unit
(define-class <cgen-dummy-unit> () ())

(define-method slot-missing ((c <class>) (obj <cgen-dummy-unit>) slot . _)
  (error "You need to bind an instance of <cgen-unit> to use \
          cgen code generation procedures."))

;; API
(define cgen-current-unit (make-parameter (make <cgen-dummy-unit>)))

;; API
(define-method cgen-unit-c-file ((unit <cgen-unit>))
  (or (~ unit'c-file)
      (ecase (~ unit'language)
        [(c)   #"~(~ unit 'name).c"]
        [(c++) #"~(~ unit 'name).cpp"])))

;; API
(define-method cgen-unit-init-name ((unit <cgen-unit>))
  (format "Scm__Init_~a"
          (or (~ unit'init-name) (cgen-safe-name (~ unit'name)))))

;; API
(define-method cgen-unit-h-file ((unit <cgen-unit>))
  (or (~ unit'h-file)
      #"~(~ unit 'name).h"))

;; API
(define-method cgen-unit-toplevel-nodes ((unit <cgen-unit>))
  (~ unit'toplevels))

;; API
(define (cgen-add! node)
  (and-let* ((unit (cgen-current-unit)))
    (slot-push! unit 'toplevels node))
  node)

(define-method cgen-emit-part ((unit <cgen-unit>) part)
  (let1 context (make-hash-table)
    (define (walker node)
      (unless (hash-table-get context node #f)
        (hash-table-put! context node #t)
        (cgen-node-traverse node walker)
        (cgen-emit node part)))
    (for-each walker (reverse (~ unit'toplevels)))))

;; API
(define-method cgen-emit-h ((unit <cgen-unit>))
  (cgen-with-output-file (cgen-unit-h-file unit) sys-rename
    (^[]
      (cond [(~ unit'preamble) => emit-raw])
      (cgen-emit-part unit 'extern))))

;; API
(define-method cgen-emit-c ((unit <cgen-unit>))
  (cgen-with-output-file (cgen-unit-c-file unit) sys-rename
    (^[]
      (cond [(~ unit'preamble) => emit-raw])
      (cgen-emit-part unit 'decl)
      (cgen-emit-static-data unit)
      (cgen-emit-part unit 'body)
      (cond [(~ unit'init-prologue) => emit-raw]
            [else
             (print "void Scm__Init_"(cgen-safe-name (~ unit'name))"(void)")
             (print "{")])
      (cgen-emit-part unit 'init)
      (cond [(~ unit'init-epilogue) => emit-raw]
            [else (print "}")])
      )))

;; NB: temporary solution for inter-module dependency.
;; The real procedure is defined in gauche.cgen.literal.
(define-generic cgen-emit-static-data)

;;=============================================================
;; Base node class
;;
(define-class <cgen-node> ()
  ([extern?        :init-keyword :extern? :init-value #f]
   [cpp-conditions :init-keyword :cpp-condition
                   :init-form (cgen-cpp-conditions)]
   ))

;; list of conditions.  internally used.
(define %cgen-cpp-conditions (make-parameter '()))

;; API
(define (cgen-cpp-conditions) (%cgen-cpp-conditions))

;; API
(define-syntax cgen-with-cpp-condition
  (syntax-rules ()
    [(_ condition . body)
     (let1 new-conditions (cons (cpp-condition->string condition)
                                (%cgen-cpp-conditions))
       (parameterize ([%cgen-cpp-conditions new-conditions])
         . body))]))

;; render cpp condition
(define (cpp-condition->string condition)
  (define (rec c)
    (match c
      [(and (or (? string?) (? symbol?) (? integer?)) c) (x->string c)]
      [('defined c) #"defined(~(rec c))"]
      [('not c)     #"!(~(rec c))"]
      [('and c ...) (n-ary "&&" c)]
      [('or c ...)  (n-ary "||" c)]
      [((and (or '+ '*) op) c ...) (n-ary op c)]
      [((and (or '- '/) op) c0 c1 ...)
       (if (null? c1) #"~|op|(~(rec c0))" (n-ary op (cons c0 c1)))]
      [((and (or '> '>= '== '< '<= '!= 'logand 'logior 'lognot '>> '<<) op) c0 c1)
       (binary op c0 c1)]
      [_ (error "Invalid C preprocessor condition expression:" condition)]))
  (define (n-ary op cs)
    (string-concatenate (intersperse (x->string op) (map (^c #"(~(rec c))") cs))))
  (define (binary op c0 c1)
    #"(~(rec c0))~|op|(~(rec c1))")
  (rec condition))

;; fallback methods
(define-method cgen-decl-common ((node <cgen-node>)) #f)

(define-method cgen-emit-xtrn ((node <cgen-node>)) #f)
(define-method cgen-emit-decl ((node <cgen-node>)) #f)
(define-method cgen-emit-body ((node <cgen-node>)) #f)
(define-method cgen-emit-init ((node <cgen-node>)) #f)

;; Should apply WALKER to the <cgen-node> instances the NODE
;; routine should visit.  The default method scans all slots
;; and picks up any <cgen-node>.  It fails to recognize, for example,
;; the node keeps a list of <cgen-nodes>; in which case the subclass
;; must provide a proper method.
(define-method cgen-node-traverse ((node <cgen-node>) walker)
  (do-ec (: slot (map slot-definition-name (class-slots (class-of node))))
         (if (slot-bound? node slot))
         (and-let* ([var (slot-ref node slot)]
                    [ (is-a? var <cgen-node>) ])
           (walker var))))

(define-method cgen-emit ((node <cgen-node>) part)
  ;; A kludge for emitting cpp-condition only when necessary.
  (define (method-overridden? gf)
    (and-let* ([meths (compute-applicable-methods gf (list node))]
               [ (not (null? meths)) ])
      (match (~ (car meths)'specializers)
        [((? (cut eq? <> <cgen-node>))) #f]
        [_ #t])))
  (define (with-cpp-condition gf)
    (cond [(~ node'cpp-conditions)
           => (^[cppc] (cond [(method-overridden? gf)
                              (for-each (cut print "#if " <>) (reverse cppc))
                              (gf node)
                              (for-each (cut print "#endif /* "<>" */")
                                        cppc)]
                             [else (gf node)]))]
          [else (gf node)]))
  (case part
    [(extern) (with-cpp-condition cgen-emit-xtrn)]
    [(decl)   (with-cpp-condition cgen-emit-decl)]
    [(body)   (with-cpp-condition cgen-emit-body)]
    [(init)   (with-cpp-condition cgen-emit-init)]))

;;=============================================================
;; Raw nodes - can be used to insert a raw piece of code
;;

(define-class <cgen-raw-xtrn> (<cgen-node>)
  ((code  :init-keyword :code :init-value "")))
(define-method cgen-emit-xtrn ((node <cgen-raw-xtrn>))
  (emit-raw (~ node'code)))

(define-class <cgen-raw-decl> (<cgen-node>)
  ((code  :init-keyword :code :init-value "")))
(define-method cgen-emit-decl ((node <cgen-raw-decl>))
  (emit-raw (~ node'code)))

(define-class <cgen-raw-body> (<cgen-node>)
  ((code  :init-keyword :code :init-value "")))
(define-method cgen-emit-body ((node <cgen-raw-body>))
  (emit-raw (~ node'code)))

(define-class <cgen-raw-init> (<cgen-node>)
  ((code  :init-keyword :code :init-value "")))
(define-method cgen-emit-init ((node <cgen-raw-init>))
  (emit-raw (~ node'code)))


(define (cgen-extern . code)
  (cgen-add! (make <cgen-raw-xtrn> :code code)))

(define (cgen-decl . code)
  (cgen-add! (make <cgen-raw-decl> :code code)))

(define (cgen-body . code)
  (cgen-add! (make <cgen-raw-body> :code code)))

(define (cgen-init . code)
  (cgen-add! (make <cgen-raw-init> :code code)))

;;=============================================================
;; cpp
;;

;; #include ---------------------------------------------------
(define-class <cgen-include> (<cgen-node>)
  ((path        :init-keyword :path)))

(define (include-common node)
  (print "#include "
         (if (string-prefix? "<" (~ node'path))
           (~ node'path)
           #"\"~(~ node'path)\"")))

(define-method cgen-emit-xtrn ((node <cgen-include>))
  (include-common node))
(define-method cgen-emit-decl ((node <cgen-include>))
  (include-common node))

(define (cgen-include path)
  (cgen-add! (make <cgen-include> :path path)))

;; #define -----------------------------------------------------
(define-class <cgen-cpp-define> (<cgen-node>)
  ((name   :init-keyword :name)
   (value  :init-keyword :value)
   ))

(define (cpp-define-common node)
  (print "#define "(~ node'name)" "(~ node'value)))

(define-method cgen-emit-xtrn ((node <cgen-cpp-define>))
  (cpp-define-common node))
(define-method cgen-emit-decl ((node <cgen-cpp-define>))
  (cpp-define-common node))

(define (cgen-define name :optional (value ""))
  (cgen-add!
   (make <cgen-cpp-define> :name name :value value)))

;;=============================================================
;; Utilities
;;

;; Call thunk while binding current-output-port to a temp file,
;; then calls (finisher tmpfile file).
(define (cgen-with-output-file file finisher thunk)
  (call-with-temporary-file
   (^[port tmpfile]
     (with-output-to-port port thunk)
     (close-output-port port)
     (finisher tmpfile file))
   :directory "."))

(define (emit-raw code)
  (if (list? code)
    (for-each print code)
    (print code)))

;; Creates a C-safe name from Scheme string str
(define (cgen-safe-name str)
  (with-string-io str
    (^[] (let loop ((b (read-byte)))
           (cond [(eof-object? b)]
                 [(or (<= 48 b 58)
                      (<= 65 b 90)
                      (<= 97 b 122))
                  (write-byte b) (loop (read-byte))]
                 [else
                  (format #t "_~2,'0x" b) (loop (read-byte))])))))

;; Like cgen-safe-name, but using more 'friendly' transliteration.
;; Used in genstub, since the transliterated name may be referred from
;; literal C code.
;; Not for general use, since this mapping is N to 1 and there's a
;; chance of name conflict.
(define (cgen-safe-name-friendly str)
  (with-string-io str
    (^[] (let loop ([c (read-char)])
           (unless (eof-object? c)
             (case c
               [(#\-) (let1 d (read-char)
                        (cond [(eqv? d #\>) (display "_TO") (loop (read-char))]
                              [else         (display #\_) (loop d)]))]
               [(#\?) (display #\P) (loop (read-char))]
               [(#\!) (display #\X) (loop (read-char))]
               [(#\<) (display "_LT") (loop (read-char))]
               [(#\>) (display "_GT") (loop (read-char))]
               [(#\* #\> #\@ #\$ #\% #\^ #\& #\* #\+ #\= #\. #\/ #\~ #\:)
                (display #\_)
                (display (number->string (char->integer c) 16))
                (loop (read-char))]
               [else (display c) (loop (read-char))]
               ))))))

(define (cgen-safe-string value)
  (with-string-io value
    (lambda ()
      (display "\"")
      (generator-for-each
       (^b (if (or (= #x20 b) (= #x21 b) ; #x22 = #\"
                   (<= #x23 b #x3e)      ; #x3f = #\?  - avoid trigraph trap
                   (<= #x40 b #x5b)      ; #x5c = #\\
                   (<= #x5d b #x7e))
             (write-byte b)
             (format #t "\\~3,'0o" b))) read-byte)
      (display "\""))))

;; Escape  '*/' so that str can be inserted safely within a comment.
(define (cgen-safe-comment str)
  (regexp-replace-all* (x->string str) #/\/\*/ "/ *" #/\*\// "* /"))

;; Useful to include sexp in C comment.  Strip #<identifier ...> away
;; for the readability.
(define (cgen-safe-comment-sexp obj)
  (cgen-safe-comment (write-to-string (unwrap-syntax obj))))
