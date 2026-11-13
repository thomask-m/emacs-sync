;;; smoke.el --- exercise every package in a real (non-batch) Emacs  -*- lexical-binding: t; -*-

;; Loaded into a running Emacs daemon by tests/smoke.sh, after it started up
;; normally with the synced config. Each check actually *uses* a package rather
;; than just `require'-ing it. Results are written to $EMACS_SYNC_SMOKE_OUT.

(require 'cl-lib)

(defvar smoke-results nil)

(defmacro smoke (name &rest body)
  "Run BODY; record pass if it returns non-nil without signalling."
  (declare (indent 1))
  `(push (cons ,name
               (condition-case err
                   (if (progn ,@body) "ok" "FAIL (returned nil)")
                 (error (format "FAIL %s" (error-message-string err)))))
         smoke-results))

(defun smoke-tmp (name content)
  (let ((f (expand-file-name name (make-temp-file "smoke" t))))
    (with-temp-file f (insert content))
    f))

(defun smoke-wait-for (pred &optional secs)
  (let ((deadline (+ (float-time) (or secs 10))))
    (while (and (not (funcall pred)) (< (float-time) deadline))
      (accept-process-output nil 0.1))
    (funcall pred)))

;;; Startup

(smoke "init loaded without errors" (not init-file-had-error))
(smoke "custom file loaded" (eq display-line-numbers-type 'relative))
(smoke "package archives include melpa" (assoc "melpa" package-archives))

;;; Packages

(smoke "rust-mode: .rs opens in rust-mode"
  (with-current-buffer (find-file-noselect (smoke-tmp "a.rs" "fn main() {}\n"))
    (prog1 (eq major-mode 'rust-mode) (kill-buffer))))

(smoke "go-mode: .go opens in go-mode"
  (with-current-buffer (find-file-noselect (smoke-tmp "a.go" "package main\n"))
    (prog1 (eq major-mode 'go-mode) (kill-buffer))))

(smoke "expand-region: C-= expands region"
  (and (eq (key-binding (kbd "C-=")) 'er/expand-region)
       (with-temp-buffer
         (emacs-lisp-mode)
         (insert "(foo (bar baz) qux)")
         (goto-char 8)
         (transient-mark-mode 1)
         (er/expand-region 1)
         (and (region-active-p)
              (equal (buffer-substring (region-beginning) (region-end)) "bar")))))

(smoke "multiple-cursors: mark-next-like-this adds a cursor"
  (and (eq (key-binding (kbd "C->")) 'mc/mark-next-like-this)
       (with-temp-buffer
         (switch-to-buffer (current-buffer))
         (insert "foo bar foo bar foo")
         (goto-char 1)
         (set-mark 1) (goto-char 4) (activate-mark)
         (mc/mark-next-like-this 1)
         (prog1 (= (mc/num-cursors) 2) (mc/disable-multiple-cursors-mode)))))

(smoke "avy: C-: bound to avy-goto-char"
  (and (eq (key-binding (kbd "C-:")) 'avy-goto-char)
       (progn (require 'avy) (fboundp 'avy-goto-char))))

(smoke "ace-window: M-o bound, aw-keys set"
  (and (eq (key-binding (kbd "M-o")) 'ace-window)
       (equal aw-keys '(?a ?s ?d ?f ?g ?h ?j ?k ?l))))

(smoke "reformatter: clang-format via `cf' reformats C++"
  (and (fboundp 'sillyfmt-format-buffer)
       (eq (key-binding (kbd "C-c C-f")) 'cf)
       (if (not (executable-find "clang-format"))
           t ; tool not installed on this host; definition is what we can check
         (with-temp-buffer
           (c++-mode)
           (insert "int main(){return 0;}\n")
           (cf)
           (string-match-p "int main() { return 0; }" (buffer-string))))))

(smoke "magit: status works on a git repo"
  (let* ((dir (file-name-as-directory (make-temp-file "smoke-git" t)))
         (default-directory dir))
    (call-process "git" nil nil nil "init" "-q")
    (with-temp-file (expand-file-name "x.txt" dir) (insert "x\n"))
    (magit-status-setup-buffer dir)
    (let ((buf (magit-get-mode-buffer 'magit-status-mode)))
      (prog1 (and buf (with-current-buffer buf
                        (string-match-p "Untracked files" (buffer-string))))
        (when buf (kill-buffer buf))))))

(require 'vterm)          ; so the `let' below binds the special var
(require 'exec-path-from-shell)

(smoke "vterm: terminal runs a shell command"
  (let* ((vterm-shell "/bin/sh")
         (buf (vterm "smoke-vterm")))
    (prog1 (and (process-live-p (get-buffer-process buf))
                (progn
                  (with-current-buffer buf (vterm-send-string "echo SMOKE_$((40+2))\n"))
                  (smoke-wait-for
                   (lambda () (with-current-buffer buf
                                (string-match-p "SMOKE_42" (buffer-string)))))))
      (let ((kill-buffer-query-functions nil)) (kill-buffer buf)))))

(smoke "undo-tree: undo/redo through the tree"
  (with-temp-buffer
    (switch-to-buffer (current-buffer))
    (buffer-enable-undo)
    (undo-tree-mode 1)
    (insert "a") (undo-boundary)
    (insert "b") (undo-boundary)
    (undo-tree-undo)
    (prog1 (equal (buffer-string) "a") (undo-tree-mode -1))))

(smoke "prism: prism-mode enables"
  (with-temp-buffer
    (emacs-lisp-mode)
    (insert "(a (b (c)))")
    (prism-mode 1)
    (prog1 prism-mode (prism-mode -1))))

(smoke "rainbow-delimiters: mode enables"
  (with-temp-buffer
    (emacs-lisp-mode)
    (rainbow-delimiters-mode 1)
    rainbow-delimiters-mode))

(smoke "ultra-scroll: global mode toggles"
  (prog1 (progn (ultra-scroll-mode 1) ultra-scroll-mode)
    (ultra-scroll-mode -1)))

(smoke "exec-path-from-shell: reads PATH from login shell"
  (let ((exec-path-from-shell-shell-name "/bin/sh")
        (exec-path-from-shell-arguments '("-l")))
    (let ((path (exec-path-from-shell-getenv "PATH")))
      (and (stringp path) (string-match-p "/" path)))))

;;; Report

(with-temp-file (getenv "EMACS_SYNC_SMOKE_OUT")
  (dolist (r (reverse smoke-results))
    (insert (format "%-55s %s\n" (car r) (cdr r)))))
