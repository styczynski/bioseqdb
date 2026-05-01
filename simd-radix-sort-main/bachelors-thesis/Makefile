THESIS_NAME=bachelors-thesis
OUTPUT_DIR=output

.PHONY: pdf
pdf: $(OUTPUT_DIR)/$(THESIS_NAME).pdf

$(OUTPUT_DIR)/$(THESIS_NAME).pdf: $(THESIS_NAME).tex $(THESIS_NAME).bib data/*.dat
	@mkdir -p $(OUTPUT_DIR)
	@echo "* pdflatex run 1/3:"
	-pdflatex -shell-escape -output-directory=$(OUTPUT_DIR) -interaction=batchmode $(THESIS_NAME).tex
	@echo "* index run 1/3:"
	bibtex $(OUTPUT_DIR)/$(THESIS_NAME)
	@echo "* index run 2/3:"
	makeindex $(OUTPUT_DIR)/$(THESIS_NAME).glo -s nomencl.ist -o $(OUTPUT_DIR)/$(THESIS_NAME).gls
	@echo "* index run 3/3:"
	makeindex $(OUTPUT_DIR)/$(THESIS_NAME).idx
	@echo "* pdflatex run 2/3:"
	-pdflatex -shell-escape -output-directory=$(OUTPUT_DIR) -interaction=nonstopmode $(THESIS_NAME).tex
	@echo "* pdflatex run 3/3:"
	pdflatex -shell-escape -output-directory=$(OUTPUT_DIR) $(THESIS_NAME).tex

.PHONY: clean
clean:
	-rm -rf $(OUTPUT_DIR)
