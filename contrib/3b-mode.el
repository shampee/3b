;;; 3b-mode.el --- Major mode for the 3b language -*- lexical-binding: t; -*-

;; Author: shampee
;; Version: 0.3

(require 'lisp-mode)

(defgroup 3b nil
  "Major mode for the 3b language."
  :group 'languages)

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;; Syntax

(defvar 3b-mode-syntax-table
  (let ((st (copy-syntax-table lisp-mode-syntax-table)))

    ;; identifiers
    (modify-syntax-entry ?_ "w" st)

    ;; maps
    (modify-syntax-entry ?{ "(}" st)
    (modify-syntax-entry ?} "){" st)

    ;; vectors -- lisp-mode-syntax-table treats [ ] as plain symbol
    ;; constituents (traditional Common Lisp has no vector-literal
    ;; syntax), which means any token touching a bracket with no
    ;; intervening space (e.g. "[f32 2]", "w Vec4]") gets fused into
    ;; one giant symbol and \_< / \_> can never find a boundary inside
    ;; it. That silently breaks font-lock *and* syntax-ppss-based
    ;; indentation/paredit for every vector form. Give them real
    ;; paired-delimiter syntax, same as { } above.
    (modify-syntax-entry ?\[ "(]" st)
    (modify-syntax-entry ?\] ")[" st)

    st))

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;; Faces

(defface 3b-keyword-face
  '((t :inherit font-lock-constant-face))
  "Face used for map keywords.")

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;; Language

(defconst 3b-keywords
  '("fn"
    "struct"
    "union"
    "enum"
    "flags"
    "handle"

    "const"

    "package"
    "import"
    "private"
    "extern"
    "packed"
    "align"

    "val"
    "var"
    "let"

    "type-name"
    "member-type"
    "member-offset"

    "and"
    "or"
    "not"
    "bit-and"
    "bit-or"
    "bit-not"
    "bit-xor"
    "bit-shl"
    "bit-shr"

    "if"
    "else"
    "when"
    "do"
    "void"
    "match"
    "while"
    "for"

    "->"
    "->>"
    "some->"

    "zero"

    "true"
    "false"
    "nil"

    "break"
    "continue"
    "return"))

(defconst 3b-builtins
  '("addr"
    "deref"
    "cast"
    "reinterpret"
    "get"
    "get-in"
    "set"
    "."
    "&"
    "nth"
    "nth-checked"

    "min"
    "max"
    "clamp"
    "clamp-top"
    "clamp-bot"

    "string-to-i32"
    "string-to-i64"
    "string-to-u32"
    "string-to-u64"
    "string-to-f32"
    "string-to-f64"

    "string-match"
    "string-prefix"
    "string-postfix"
    "string-skip"
    "string-chop"
    "string-substr"
    "string-find"
    "string-find-reverse"
    "string-starts-with"
    "string-ends-with"
    "string-cat"
    "string-copy"

    "cstring-copy"
    "cstring"

    "str"

    "print"
    "println"

    "push"
    "push0"
    "push-zero"
    "dyn-push"
    "dyn-count"
    "commit"

    "len" ; should work on all builtin collections

    "vector-push" ; thin safety wrapper around dyn-push
    "vector-clear"
    "vector-swap-remove"
    "vector-remove-at"
    "vector-contains?"
    "vector-index-of"

    "map-set"
    "map-get"
    "map-remove"
    "map-contains?"

    "set-add"
    "set-remove"
    "set-contains?"

    "handle-pool-init"
    "handle-alloc"
    "handle-deref"
    "handle-free"
    "handle-valid?"

    "create"
    "destroy"
    "reset"
    "release"
    "mark"
    "pop"
    "scratch"

    "lane-fn"
    "parallel"
    "parallel-for"
    "lane-index"
    "lane-count"
    "lane-sync"
    "lane-arena"

    "sin"
    "cos"
    "tan"
    "asin"
    "asin-checked"
    "acos"
    "acos-checked"
    "atan"
    "sinh"
    "cosh"
    "tanh"
    "sqrt"
    "sqrt-checked"
    "cbrt"
    "ceil"
    "floor"
    "round"
    "atan2"
    "pow"
    "pow-checked"
    "mod"

    "sizeof"
    "alignof"

    "mem-set"
    "mem-copy"
    "mem-zero"
    "mem-compare"

    "+"
    "-"
    "*"
    "/"
    "%"

    "="
    "!="
    "<"
    "<="
    ">"
    ">="

    "++"
    "--"
    "-="
    "+="
    "*="
    "/="))

(defconst 3b-primitive-types
  '("void"
    "any"

    "arena"

    "stream"

    "bool"
    "char"
    "string"

    "i8"
    "i16"
    "i32"
    "i64"

    "u8"
    "u16"
    "u32"
    "u64"

    "f32"
    "f64"))

;; Reusable rx fragments. An "identifier" character is anything the
;; syntax table calls word- or symbol-constituent (covers letters,
;; digits, - _ . etc. the way \sw \s_ did in the old hand-rolled
;; regexps), so this stays correct even if the syntax table changes.
(rx-define 3b-ident-char (or (syntax word) (syntax symbol)))
(rx-define 3b-ident (+ 3b-ident-char))
(rx-define 3b-int-suffix (or "i8" "i16" "i32" "i64" "u8" "u16" "u32" "u64"))
(rx-define 3b-type-name (seq (any "A-Z") (* (any "A-Za-z0-9_"))))

(defconst 3b--primitive-type-regexp
  (rx symbol-start
      (regexp (regexp-opt 3b-primitive-types))
      (? (+ "*"))
      (? "^")
      symbol-end))

(defconst 3b--user-type-regexp
  (rx symbol-start
      (group 3b-type-name)
      (? (+ "*"))
      (? "^")
      symbol-end))

;; Numbers: int/float/hex, each with an optional sign and an optional
;; type suffix, e.g. -1, 3.14, 30.5f32, 0x00000000u32. All three
;; shapes and both optional pieces are needed -- real code in this
;; codebase uses every combination (negative ints in gl.3b, suffixed
;; hex in sdl.3b, plain floats in math.3b).
(defconst 3b--number-regexp
  (rx symbol-start
      (? "-")
      (or (seq "0" (any "xX") (+ hex-digit) (? 3b-int-suffix))
          (seq (+ digit) "." (+ digit) (? (or "f32" "f64")))
          (seq (+ digit) (? 3b-int-suffix)))
      symbol-end))

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;; Font Lock

(defconst 3b-font-lock-keywords
  `(
    ;; numbers: -1  42  3.14  30.5f32  20u8  0x00000000u32
    (,3b--number-regexp
     . font-lock-constant-face)

    ;; keywords
    (,(regexp-opt 3b-keywords 'symbols)
     . font-lock-keyword-face)

    ;; builtins
    (,(regexp-opt 3b-builtins 'symbols)
     . font-lock-keyword-face)

    ;; function definitions
    (,(rx "(fn" (+ space) (group 3b-ident))
     1 font-lock-function-name-face)

    ;; type declarations -- must run before the generic user-type
    ;; rule below so "(struct Foo ..." fontifies Foo as a type even
    ;; though the generic rule would have matched it too
    (,(rx "(" (or "struct" "union" "enum" "flags") (+ space)
          (group 3b-type-name))
     1 font-lock-type-face)

    ;; val bindings -- must run before the generic user-type rule so
    ;; capitalized constants like "(val SDLK_UNKNOWN ...)" get
    ;; constant-face instead of being swallowed as a type name
    (,(rx "(val" (+ space) (group 3b-ident))
     1 font-lock-constant-face)

    ;; var bindings, same reasoning as val above
    (,(rx "(var" (+ space) (group 3b-ident))
     1 font-lock-variable-name-face)

    ;; primitive types + pointers
    (,3b--primitive-type-regexp
     . font-lock-type-face)

    ;; user-defined types + pointers (generic catch-all: anything
    ;; capitalized not already claimed by a rule above)
    (,3b--user-type-regexp
     . font-lock-type-face)

    ;; Enum / Flag references
    (,(rx symbol-start (group 3b-type-name) "/" (group (+ (any "A-Za-z0-9_"))) symbol-end)
     (1 font-lock-type-face)
     (2 font-lock-constant-face))

    ;; Map/vector delimiters
    (,(rx (any "[]{}"))
     . font-lock-builtin-face)

    ;; map keywords
    (,(rx symbol-start ":" 3b-ident symbol-end)
     . '3b-keyword-face)

    ;; TODO
    (,(rx word-start (group (or "TODO" "FIXME" "NOTE")) ":")
     1 font-lock-warning-face t)))

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;; Indentation helpers

(defun 3b--container-position ()
  (nth 1 (syntax-ppss)))

(defun 3b--container-char ()
  (save-excursion
    (goto-char (3b--container-position))
    (char-after)))

(defun 3b--inside-form-p ()
  (3b--container-position))

(defun 3b--inside-vector-p ()
  (eq (3b--container-char) ?\[))

(defun 3b--inside-map-p ()
  (eq (3b--container-char) ?{))

(defun 3b--aligned-indent ()
  (save-excursion
    (goto-char (3b--container-position))
    (forward-char)
    (skip-chars-forward " \t\n")
    (current-column)))

(defun 3b--list-indent ()
  (save-excursion
    (goto-char (3b--container-position))
    (+ (current-column) 2)))

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;; Indentation

(defun 3b-indent-line ()
  (interactive)

  ;; `indent-line-to' always leaves point at the end of the line's
  ;; indentation, discarding point's position within the code. That's
  ;; correct when point started out in the indentation (e.g. user hit
  ;; TAB there), but wrong when point was past it -- callers like
  ;; `lispy--indent-for-tab' reindent the line as a side effect before
  ;; inserting text at point, and losing point mid-line sends the
  ;; insertion to the wrong place. Only let point jump when it began
  ;; in the indentation; otherwise keep it pinned to its original spot.
  (let ((restore-point (> (current-column) (current-indentation)))
        (indent
         (save-excursion
           (back-to-indentation)

           (cond

            ;; top level
            ((null (3b--inside-form-p))
             0)

            ;; vectors
            ((3b--inside-vector-p)
             (3b--aligned-indent))

            ;; maps
            ((3b--inside-map-p)
             (3b--aligned-indent))

            ;; lists
            (t
             (3b--list-indent))))))

    (if restore-point
        (save-excursion (indent-line-to indent))
      (indent-line-to indent))))

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;; Mode

;;;###autoload
(define-derived-mode 3b-mode prog-mode "3b"
  "Major mode for the 3b language."

  :syntax-table 3b-mode-syntax-table

  (setq-local font-lock-defaults
              '(3b-font-lock-keywords))

  (setq-local indent-line-function #'3b-indent-line)

  (setq-local comment-start ";; ")
  (setq-local comment-end "")

  (setq-local indent-tabs-mode nil)
  (setq-local tab-width 2)

  (electric-pair-local-mode 1)

  (when (fboundp 'show-paren-local-mode)
    (show-paren-local-mode 1))

  (when (fboundp 'rainbow-delimiters-mode)
    (rainbow-delimiters-mode 1))

  (when (fboundp 'paredit-mode)
    (paredit-mode 1))

  (when (fboundp 'lispy-mode)
    (lispy-mode 1)))

;; lispy's default key theme binds "[" and "]" to lispy-backward /
;; lispy-forward (unconditional list navigation, not gated on being
;; at a "special" position), which steals them from self-insert.
;; 3b uses square brackets constantly for vector types (e.g. "[f32
;; 2]"), so in evil insert state they need to type literally. Binding
;; on 3b-mode-map's evil insert state overrides lispy-mode-map (an
;; ordinary minor-mode keymap) regardless of minor-mode activation
;; order.
;;
;; "[" goes to `lispy-brackets' rather than plain self-insert so it
;; gets the same DWIM leading-space-if-needed treatment lispy already
;; gives "(" via `lispy-parens' (both are just `lispy-pair' instances).
(with-eval-after-load 'evil
  (evil-define-key 'insert 3b-mode-map (kbd "[") #'lispy-brackets)
  (evil-define-key 'insert 3b-mode-map (kbd "]") #'self-insert-command))

(defun my-3b-lispy-goto-symbol (&rest _)
  "Delegate lispy's goto-symbol to lsp-mode for 3b-mode."
  (lsp-find-definition))

(with-eval-after-load 'lispy
  (add-to-list 'lispy-goto-symbol-alist '(3b-mode my-3b-lispy-goto-symbol)))

(with-eval-after-load 'lsp-mode
  (setq-default lsp-auto-guess-root t)

  (lsp-register-client
   (make-lsp-client :new-connection (lsp-stdio-connection "~/.local/bin/3b-lsp")
                    :major-modes '(3b-mode)
                    :server-id '3b-lsp
                    :multi-root t))
  (add-to-list 'lsp-language-id-configuration '(3b-mode . "3b")))

;;;###autoload
(add-to-list 'auto-mode-alist
             '("\\.3b\\'" . 3b-mode))
(add-to-list 'auto-mode-alist
             '("\\.3bs\\'" . 3b-mode))

(provide '3b-mode)

;;; 3b-mode.el ends here
