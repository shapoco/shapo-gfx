# Sphinx documentation build.
#
# The published site is built and deployed by .github/workflows/publish-docs.yml;
# nothing generated here is committed. These targets are for local preview only.
#
#   make html      build the manual into _build/html
#   make preview   assemble the whole site (docs/ + the manual as ref/) and serve it
#                  on http://localhost:52880/ so links to the demos work too

SPHINXOPTS    ?=
SPHINXBUILD   ?= sphinx-build
SOURCEDIR     = .
BUILDDIR      = _build
SITEDIR       = $(BUILDDIR)/site

.PHONY: help html clean preview serve

help:
	@$(SPHINXBUILD) -M help "$(SOURCEDIR)" "$(BUILDDIR)" $(SPHINXOPTS) $(O)

# -d keeps Sphinx's internal cache (.doctrees) out of the output: it embeds
# absolute build paths and has no place in the published site.
html:
	$(SPHINXBUILD) -b html -d "$(BUILDDIR)/doctrees" "$(SOURCEDIR)" "$(BUILDDIR)/html" $(SPHINXOPTS) $(O)

# Same layout as the deployed site, so the links from the manual to the demos resolve
preview: html
	rm -rf $(SITEDIR)
	mkdir -p $(SITEDIR)
	cp -r ../docs/. $(SITEDIR)/
	rm -rf $(SITEDIR)/ref
	cp -r $(BUILDDIR)/html $(SITEDIR)/ref
	python3 -m http.server --directory $(SITEDIR) 52880

serve: preview

clean:
	rm -rf $(BUILDDIR)
