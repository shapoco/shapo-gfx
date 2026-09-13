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

html:
	$(SPHINXBUILD) -b html "$(SOURCEDIR)" "$(BUILDDIR)/html" $(SPHINXOPTS) $(O)
	rm -rf ../docs/ref
	mkdir -p ../docs/ref
	cp -r $(BUILDDIR)/html/. ../docs/ref/

clean:
	rm -rf $(BUILDDIR)

serve:
	python3 -m http.server --directory ../docs 52880
