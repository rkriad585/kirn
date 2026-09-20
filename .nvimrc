" .nvimrc - Neovim editor preferences for the coco repo.
" Line endings and encoding are LF/UTF-8 (see .editorconfig / .gitattributes);
" do not override those here.
set fileencoding=utf-8
set fileformats=unix,dos
set encoding=utf-8
set tabstop=4
set shiftwidth=4
set expandtab
set autoindent
set smartindent
set softtabstop=4
set textwidth=100
set colorcolumn=100
" .co sources are C-like for highlighting/session purposes.
autocmd BufRead,BufNewFile *.co setlocal filetype=c
