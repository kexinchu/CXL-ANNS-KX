# ASPLOS 2027 submission build. Do not add geometry/vspace hacks here.
LATEX := latexmk
FLAGS := -pdf -interaction=nonstopmode -halt-on-error

.PHONY: all clean pages

all:
	$(LATEX) $(FLAGS) main.tex

pages: all
	@pdfinfo main.pdf 2>/dev/null | awk '/Pages:/ {print}'
	@echo "Body must be <= 11 pages excluding refs/appendix (visual check)."

clean:
	$(LATEX) -C main.tex
