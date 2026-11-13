;;; bootstrap.el --- emacs-sync package installer and verifier  -*- lexical-binding: t; -*-

;; Run by the emacs-sync binary as:
;;   emacs --batch -Q -l bootstrap.el
;; with these environment variables:
;;   EMACS_SYNC_MODE    "install" (install missing packages) or "verify"
;;   EMACS_SYNC_INIT    path to the init file (~/.emacs)
;;   EMACS_SYNC_CUSTOM  path to the custom file (~/.emacs.custom.el)
;;
;; The package list is derived from the config itself, so there is nothing
;; extra to maintain:
;;   * every symbol in `package-selected-packages' in the custom file
;;   * every `(require 'FEATURE)' in the init file that Emacs doesn't ship with
;; Package archives are read from `(add-to-list 'package-archives ...)' forms
;; in the init file, on top of Emacs's defaults.
;;
;; Exit status: 0 on success, 1 if anything failed.

(require 'package)
(require 'cl-lib)

(defvar es-mode (or (getenv "EMACS_SYNC_MODE") "install"))
(defvar es-init-file (expand-file-name (or (getenv "EMACS_SYNC_INIT") "~/.emacs")))
(defvar es-custom-file (expand-file-name (or (getenv "EMACS_SYNC_CUSTOM") "~/.emacs.custom.el")))
(defvar es-failures nil "List of (PACKAGE . REASON) for everything that went wrong.")

;; Never prompt: in batch mode a prompt reads stdin and would hang or abort.
(setq vterm-always-compile-module t)
(setq package-install-upgrade-built-in nil)
(setq byte-compile-warnings nil)
(setq native-comp-async-report-warnings-errors 'silent)
(setq warning-minimum-log-level :error)

(defun es-log (fmt &rest args)
  (princ (concat "[emacs-sync] " (apply #'format fmt args) "\n")))

(defun es-fail (pkg fmt &rest args)
  (let ((reason (apply #'format fmt args)))
    (push (cons pkg reason) es-failures)
    (es-log "FAIL %s: %s" pkg reason)))

;;;; Reading the config

(defun es-read-forms (file)
  "Return all top-level forms in FILE, or nil if it doesn't exist."
  (when (file-readable-p file)
    (with-temp-buffer
      (insert-file-contents file)
      (goto-char (point-min))
      (let (forms)
        (condition-case nil
            (while t (push (read (current-buffer)) forms))
          (end-of-file nil))
        (nreverse forms)))))

(defun es-walk (form fn)
  "Call FN on FORM and every cons nested inside it."
  (when (consp form)
    (funcall fn form)
    (while (consp form)
      (es-walk (car form) fn)
      (setq form (cdr form)))))

(defun es-unquote (x)
  (if (and (consp x) (memq (car x) '(quote function))) (cadr x) x))

(defun es-valid-package-symbol-p (sym)
  ;; Filters junk such as `##' (the empty symbol) that Custom sometimes writes.
  (and (symbolp sym) sym (not (eq sym t)) (> (length (symbol-name sym)) 0)))

(defun es-selected-packages ()
  "Symbols listed in `package-selected-packages' in the custom file."
  (let (result)
    (dolist (form (es-read-forms es-custom-file))
      (es-walk form
               (lambda (f)
                 (when (and (eq (car f) 'package-selected-packages)
                            (consp (cdr f)))
                   (let ((val (es-unquote (cadr f))))
                     (when (listp val)
                       (dolist (s val)
                         (when (es-valid-package-symbol-p s) (push s result)))))))))
    (delete-dups (nreverse result))))

(defun es-required-features ()
  "Features named in `(require 'X)' forms in the init file."
  (let (result)
    (dolist (form (es-read-forms es-init-file))
      (es-walk form
               (lambda (f)
                 (when (and (eq (car f) 'require) (consp (cdr f)))
                   (let ((feat (es-unquote (cadr f))))
                     (when (es-valid-package-symbol-p feat) (push feat result)))))))
    (delete-dups (nreverse result))))

(defun es-archives ()
  "Archives added via `add-to-list' on `package-archives' in the init file."
  (let (result)
    (dolist (form (es-read-forms es-init-file))
      (es-walk form
               (lambda (f)
                 (when (and (eq (car f) 'add-to-list)
                            (eq (es-unquote (nth 1 f)) 'package-archives))
                   (let ((entry (es-unquote (nth 2 f))))
                     (when (and (consp entry) (stringp (car entry)) (stringp (cdr entry)))
                       (push entry result)))))))
    (nreverse result)))

(defun es-builtin-feature-p (feature)
  "Non-nil if FEATURE ships with Emacs (found outside the package dir)."
  (let ((lib (locate-library (symbol-name feature))))
    (and lib
         (not (string-prefix-p (file-truename package-user-dir)
                               (file-truename lib))))))

(defun es-wanted-packages ()
  (let ((pkgs (es-selected-packages)))
    (dolist (feat (es-required-features))
      (unless (or (memq feat pkgs) (es-builtin-feature-p feat))
        (setq pkgs (append pkgs (list feat)))))
    pkgs))

;;;; Install

(defun es-setup-archives ()
  (dolist (entry (es-archives))
    (add-to-list 'package-archives entry t))
  (package-initialize))

(defun es-check-vterm-toolchain ()
  "vterm builds a native module; make sure the tools are there first."
  (let ((missing (cl-remove-if #'executable-find '("cmake" "make"))))
    (unless (or (executable-find "cc") (executable-find "gcc") (executable-find "clang"))
      (push "a C compiler" missing))
    (when missing
      (es-log "vterm needs %s to build its module." (string-join missing ", "))
      (es-log "  macOS:  xcode-select --install && brew install cmake libtool")
      (es-log "  Debian/Ubuntu: sudo apt install build-essential cmake libtool-bin")
      (es-log "  Fedora: sudo dnf install gcc make cmake libtool")
      (es-log "  Arch:   sudo pacman -S base-devel cmake"))
    (null missing)))

(defun es-compile-vterm-module ()
  (when (and (package-installed-p 'vterm)
             (not (locate-library "vterm-module")))
    (es-log "Building vterm-module (native code)...")
    (when (es-check-vterm-toolchain)
      (condition-case err
          (progn
            (require 'vterm-module nil t)
            (unless (featurep 'vterm-module)
              (require 'vterm)
              (unless (featurep 'vterm-module) (vterm-module-compile))))
        (error (es-fail 'vterm "module build failed: %s" (error-message-string err)))))))

(defun es-install ()
  (es-setup-archives)
  (let* ((wanted (es-wanted-packages))
         (missing (cl-remove-if #'package-installed-p wanted)))
    (es-log "Packages in config: %s" (mapconcat #'symbol-name wanted " "))
    (if (null missing)
        (es-log "All packages already installed.")
      (es-log "Installing: %s" (mapconcat #'symbol-name missing " "))
      (es-log "Refreshing package archives...")
      (package-refresh-contents)
      (dolist (pkg missing)
        (es-log "Installing %s..." pkg)
        (condition-case err
            (package-install pkg)
          (error (es-fail pkg "%s" (error-message-string err))))))
    (es-compile-vterm-module)))

;;;; Verify

(defun es-verify ()
  ;; 1. Load the real init file exactly as a normal startup would.
  (es-log "Loading %s..." es-init-file)
  (setq user-init-file es-init-file)
  (condition-case err
      (load es-init-file nil t)
    (error (es-fail 'init-file "error while loading %s: %s"
                    es-init-file (error-message-string err))))
  ;; 2. Each package must be installed and its main feature loadable.
  (dolist (pkg (es-wanted-packages))
    (cond
     ((not (package-installed-p pkg))
      (es-fail pkg "not installed"))
     (t
      (condition-case err
          (progn
            (require pkg)
            (es-log "ok   %s" pkg))
        (error (es-fail pkg "require failed: %s" (error-message-string err)))))))
  ;; 3. Package-specific checks for things `require' alone doesn't prove.
  (when (memq 'vterm (es-wanted-packages))
    (if (featurep 'vterm-module)
        (es-log "ok   vterm-module (native module loaded)")
      (es-fail 'vterm "vterm-module is not loaded"))))

;;;; Main

(condition-case err
    (pcase es-mode
      ("install" (es-install))
      ("verify" (es-verify))
      (_ (es-fail 'bootstrap "unknown mode %s" es-mode)))
  (error (es-fail 'bootstrap "%s" (error-message-string err))))

(if es-failures
    (progn
      (es-log "%d problem(s):" (length es-failures))
      (dolist (f (reverse es-failures))
        (es-log "  %s: %s" (car f) (cdr f)))
      (kill-emacs 1))
  (es-log "%s: success" es-mode)
  (kill-emacs 0))

;;; bootstrap.el ends here
