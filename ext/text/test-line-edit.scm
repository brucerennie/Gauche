;;-------------------------------------------------------------------
(use gauche.test)
(test-start "line-edit")
(test-section "line-edit")
(use text.line-edit)
(test-module 'text.line-edit)

;; Test internal functions
(let ()
  (define (t s pos expect)
    (test* #"buffer-find-matching-paren: ~s ~pos" expect
           ((with-module text.line-edit buffer-find-matching-paren)
            ((with-module text.gap-buffer string->gap-buffer) s)
            0 pos)))

  (t "(abc)" 4 0)
  (t "aa(abc)zz" 6 2)
  (t "(a(abc)z)" 6 2)
  (t "(a)(abc)z" 7 3)
  (t "(a\\(b)z" 5 0)
  (t "(a\\(\\))z" 6 0)
  (t "(a\\)\\()z" 6 0)
  (t "(a(abc)(def)z)" 11 7)
  (t "(a(abc)(def)z)" 13 0)
  (t "aaaabc)zz" 6 #f)
  (t "(a{b}c)" 4 2)
  (t "(a{b}c)" 6 0)
  (t "(a{b(}c)" 5 #f)
  (t "(a{b(}c)" 7 4)
  (t "(a{b)}c)" 4 #f)
  (t "(a{b)}c)" 5 2)
  (t "(a\"(\"b)" 6 0)
  (t "(a\")\"b)" 6 0)
  (t "(a\"\\\"(\"b)" 8 0)
  (t "(a#/bc([d\\//)]" 12 0)
  (t "(a#/bc([d\\//)]" 13 #f)
  (t "(a#(b)c)" 5 3)
  (t "(a#(b)c)" 7 0)
  (t "(a#[b[:space:]]c)" 14 3)
  (t "(a#[b[:space:]]c)" 13 5)
  (t "(a#[b[:space:]]c)" 16 0)
  (t "(a#{b}c)" 5 3)
  (t "(a#{b)c}" 5 #f)
  (t "(a#{b)c}" 7 3)
  (t "(a\\#(b)c)" 6 4)
  (t "(a#\\(b)c)" 6 0)
  )

(test-end)
