# ASPLOS 2027 submission build. Do not add geometry/vspace hacks here.
#
# Usage (from this directory):
#   make          # build main.pdf (latexmk + bibtex, reruns as needed)
#   make pages    # build and print page count
#   make clean    # remove auxiliary files and main.pdf
#
LATEX := latexmk
FLAGS := -pdf -interaction=nonstopmode -halt-on-error
MAIN  := main.tex

.PHONY: all clean pages help rebuild

.DEFAULT_GOAL := all

all:
	$(LATEX) $(FLAGS) $(MAIN)

rebuild:
	$(LATEX) $(FLAGS) -g $(MAIN)

pages: all
	@pdfinfo main.pdf 2>/dev/null | awk '/Pages:/ {print}'
	@echo "Body must be <= 11 pages excluding refs/appendix (visual check)."

clean:
	$(LATEX) -C $(MAIN)

help:
	@echo "Targets:"
	@echo "  make         Build paper/main.pdf (skip if up-to-date)"
	@echo "  make rebuild Force full recompile"
	@echo "  make pages   Build and show page count"
	@echo "  make clean   Remove build artifacts"
	@echo "  make help    Show this message"
