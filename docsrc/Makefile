# Sphinx documentation build.
#   make html     build into _build/html and copy the result to ../docs/ref/
#   make serve    serve ../docs/ on http://localhost:52880/ (same as ../launch_web_server.sh)

SPHINXOPTS    ?=
SPHINXBUILD   ?= sphinx-build
SOURCEDIR     = .
BUILDDIR      = _build

.PHONY: help html clean serve

help:
	@$(SPHINXBUILD) -M help "$(SOURCEDIR)" "$(BUILDDIR)" $(SPHINXOPTS) $(O)

# -d keeps Sphinx's internal cache (.doctrees) out of the published output:
# it embeds absolute build paths and would differ between machines and CI.
html:
	$(SPHINXBUILD) -b html -d "$(BUILDDIR)/doctrees" "$(SOURCEDIR)" "$(BUILDDIR)/html" $(SPHINXOPTS) $(O)
	rm -rf ../docs/ref
	mkdir -p ../docs/ref
	cp -r $(BUILDDIR)/html/. ../docs/ref/
	touch ../docs/.nojekyll

clean:
	rm -rf $(BUILDDIR)

serve:
	python3 -m http.server --directory ../docs 52880
